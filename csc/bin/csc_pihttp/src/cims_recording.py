"""
CIMS Recording REST API
녹취 조회, on-demand 트랜스코딩, 스트리밍, 삭제

트랜스코딩 전략:
  - CMP는 raw RTP 파일만 저장
  - 클라이언트가 재생 요청 시 CSC가 변환 (ffmpeg)
  - 변환된 파일은 캐싱하여 재요청 시 즉시 응답
  - status: raw → transcoding → ready / failed
"""

import os
import subprocess
import threading
from urllib.parse import urlparse, parse_qs, unquote
from pathlib import PurePath

import pymysql
import pymysql.cursors

from util.pi_http.http_handler import HandlerArgs, HandlerResult

# 변환 중인 작업 추적 (중복 변환 방지)
_transcoding_locks = {}
_transcoding_mutex = threading.Lock()

import struct
import logging
logger = logging.getLogger(__name__)


def _strip_rtp_to_amrwb(raw_rtp_path: str, out_amr_path: str) -> bool:
    """raw RTP 파일에서 RTP 헤더를 제거하고 AMR-WB payload만 추출한다.

    raw 파일 형식: [uint32_t len][RTP packet][uint32_t len][RTP packet]...
    AMR-WB RTP payload (RFC 4867 octet-aligned):
      [CMR 1byte][ToC 1byte][frame data]
    출력: AMR-WB 파일 헤더("#!AMR-WB\\n") + frame data 연속
    """
    try:
        with open(raw_rtp_path, 'rb') as fin, open(out_amr_path, 'wb') as fout:
            fout.write(b'#!AMR-WB\n')
            while True:
                lenbuf = fin.read(4)
                if len(lenbuf) < 4:
                    break
                pkt_len = struct.unpack('<I', lenbuf)[0]
                pkt = fin.read(pkt_len)
                if len(pkt) < pkt_len:
                    break
                if pkt_len < 12:
                    continue  # too short for RTP
                # RTP 헤더 파싱: V(2)+P(1)+X(1)+CC(4) = 1byte, PT 1byte, seq 2, ts 4, ssrc 4
                cc = pkt[0] & 0x0F
                has_ext = (pkt[0] >> 4) & 0x01
                hdr_len = 12 + cc * 4
                if has_ext and hdr_len + 4 <= pkt_len:
                    ext_len = struct.unpack_from('>H', pkt, hdr_len + 2)[0]
                    hdr_len += 4 + ext_len * 4
                if hdr_len >= pkt_len:
                    continue
                payload = pkt[hdr_len:]
                # AMR-WB RTP payload: CMR(1) + ToC(1) + frame_data
                if len(payload) > 2:
                    toc = payload[1]
                    ft = (toc >> 3) & 0x0F
                    # AMR-WB frame type → ToC byte for .amr file: F(1)+FT(4)+Q(1)+pad(2)
                    amr_toc = 0x04 | (ft << 3) | ((toc >> 2) & 0x01) << 2
                    fout.write(bytes([amr_toc]))
                    fout.write(payload[2:])  # frame data (without CMR and RTP ToC)
        return os.path.exists(out_amr_path) and os.path.getsize(out_amr_path) > 9
    except Exception as e:
        logger.error("_strip_rtp_to_amrwb error: %s", e)
        return False


def _strip_rtp_to_h264(raw_rtp_path: str, out_h264_path: str) -> bool:
    """raw RTP 파일에서 RTP 헤더를 제거하고 H.264 Annex-B NAL 스트림을 추출한다.

    H.264 RTP (RFC 6184): Single NAL / FU-A fragmentation 처리
    출력: [00 00 00 01][NAL unit]... (Annex-B 형식)
    """
    START_CODE = b'\x00\x00\x00\x01'
    fu_buffer = bytearray()
    fu_nal_hdr = 0

    try:
        with open(raw_rtp_path, 'rb') as fin, open(out_h264_path, 'wb') as fout:
            while True:
                lenbuf = fin.read(4)
                if len(lenbuf) < 4:
                    break
                pkt_len = struct.unpack('<I', lenbuf)[0]
                pkt = fin.read(pkt_len)
                if len(pkt) < pkt_len or pkt_len < 12:
                    continue
                cc = pkt[0] & 0x0F
                has_ext = (pkt[0] >> 4) & 0x01
                hdr_len = 12 + cc * 4
                if has_ext and hdr_len + 4 <= pkt_len:
                    ext_len = struct.unpack_from('>H', pkt, hdr_len + 2)[0]
                    hdr_len += 4 + ext_len * 4
                if hdr_len >= pkt_len:
                    continue
                payload = pkt[hdr_len:]
                if len(payload) < 1:
                    continue

                nal_type = payload[0] & 0x1F
                if nal_type >= 1 and nal_type <= 23:
                    # Single NAL unit
                    fout.write(START_CODE + payload)
                elif nal_type == 28:
                    # FU-A: fragmented NAL
                    if len(payload) < 2:
                        continue
                    fu_indicator = payload[0]
                    fu_header = payload[1]
                    start = (fu_header >> 7) & 1
                    end = (fu_header >> 6) & 1
                    nal_t = fu_header & 0x1F

                    if start:
                        fu_nal_hdr = (fu_indicator & 0xE0) | nal_t
                        fu_buffer = bytearray([fu_nal_hdr])
                        fu_buffer.extend(payload[2:])
                    else:
                        fu_buffer.extend(payload[2:])

                    if end and fu_buffer:
                        fout.write(START_CODE + bytes(fu_buffer))
                        fu_buffer = bytearray()
                elif nal_type == 24:
                    # STAP-A: aggregated NALs
                    off = 1
                    while off + 2 <= len(payload):
                        nal_sz = struct.unpack_from('>H', payload, off)[0]
                        off += 2
                        if off + nal_sz <= len(payload):
                            fout.write(START_CODE + payload[off:off+nal_sz])
                        off += nal_sz
        return os.path.exists(out_h264_path) and os.path.getsize(out_h264_path) > 4
    except Exception as e:
        logger.error("_strip_rtp_to_h264 error: %s", e)
        return False


def _get_db(config: dict):
    db = config.get('CimsDatabase', {})
    return pymysql.connect(
        host=db.get('Host', '127.0.0.1'),
        port=int(db.get('Port', 3306)),
        user=db.get('User', 'root'),
        password=db.get('Password', ''),
        database=db.get('Db', 'cims'),
        charset='utf8mb4',
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=True,
    )


def _dt(val):
    return val.isoformat() if val else None


def _path_parts(full_path: str, base: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _get_recording_config(config):
    """녹취 파일 경로 설정"""
    rec = config.get('Recording', {})
    return {
        'raw_dir': rec.get('RawDir', 'recordings/raw'),
        'converted_dir': rec.get('ConvertedDir', 'recordings/converted'),
    }


# ──────────────────────────────────────────────────────────────
#  On-demand 트랜스코딩
# ──────────────────────────────────────────────────────────────

def _transcode_recording(rec_id: int, config: dict):
    """VoIP 녹취 파일을 on-demand로 변환 (백그라운드 스레드)"""
    rec_cfg = _get_recording_config(config)

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM recordings WHERE id=%s", (rec_id,))
            rec = cur.fetchone()
            if not rec or rec['status'] not in ('raw',):
                return

            cur.execute(
                "UPDATE recordings SET status='transcoding' WHERE id=%s",
                (rec_id,)
            )

    raw_a = rec.get('raw_path_a', '')
    raw_b = rec.get('raw_path_b', '')
    has_video = rec.get('has_video', 0)

    # 출력 디렉터리: raw 파일과 같은 폴더에 저장
    out_dir = os.path.dirname(raw_a) if raw_a else rec_cfg.get('converted_dir', '/tmp')
    os.makedirs(out_dir, exist_ok=True)

    audio_path = None
    success = True
    file_size = 0

    try:
        video_path_a = None
        video_path_b = None

        for side, raw_audio, raw_video in [
            ('a', raw_a, rec.get('raw_path_va', '')),
            ('b', raw_b, rec.get('raw_path_vb', '')),
        ]:
            if not raw_audio or not os.path.exists(raw_audio):
                continue

            amr_tmp = raw_audio + '.amr'
            has_video_side = (raw_video and os.path.exists(raw_video)
                              and os.path.getsize(raw_video) > 0)

            if has_video_side:
                # audio + video → MP4 muxing
                h264_tmp = raw_video + '.h264'
                mp4_out = os.path.join(out_dir, f"recording_{side}.mp4")

                amr_ok = _strip_rtp_to_amrwb(raw_audio, amr_tmp)
                h264_ok = _strip_rtp_to_h264(raw_video, h264_tmp)

                if amr_ok and h264_ok:
                    ret = subprocess.run(
                        ['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
                         '-i', amr_tmp, '-f', 'h264', '-i', h264_tmp,
                         '-c:a', 'aac', '-ar', '16000', '-ac', '1',
                         '-c:v', 'copy', '-movflags', '+faststart', mp4_out],
                        capture_output=True, timeout=300
                    )
                    if ret.returncode == 0:
                        if side == 'a':
                            video_path_a = mp4_out
                            audio_path = mp4_out
                        else:
                            video_path_b = mp4_out
                    else:
                        logger.warning("ffmpeg mux(%s) failed: %s", side, ret.stderr.decode(errors='replace'))
                for tmp in [amr_tmp, h264_tmp]:
                    try: os.remove(tmp)
                    except: pass
            else:
                # audio only → WAV (LPCM 16kHz mono 16bit)
                wav_out = os.path.join(out_dir, f"recording_{side}.wav")
                if _strip_rtp_to_amrwb(raw_audio, amr_tmp):
                    ret = subprocess.run(
                        ['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
                         '-i', amr_tmp,
                         '-ar', '16000', '-ac', '1', '-c:a', 'pcm_s16le', wav_out],
                        capture_output=True, timeout=300
                    )
                    if ret.returncode == 0:
                        if side == 'a' and audio_path is None:
                            audio_path = wav_out
                    else:
                        logger.warning("ffmpeg wav(%s) failed: %s", side, ret.stderr.decode(errors='replace'))
                try: os.remove(amr_tmp)
                except: pass

        if not audio_path:
            audio_path = raw_a  # fallback

        if os.path.exists(audio_path):
            file_size = os.path.getsize(audio_path)

        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "UPDATE recordings SET status='ready', audio_path=%s, "
                    "video_path_a=%s, video_path_b=%s, file_size=%s WHERE id=%s",
                    (audio_path, video_path_a, video_path_b, file_size, rec_id)
                )
    except Exception as e:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute("UPDATE recordings SET status='failed' WHERE id=%s", (rec_id,))

    # 변환 완료 → 잠금 해제
    with _transcoding_mutex:
        _transcoding_locks.pop(f"rec_{rec_id}", None)


def _transcode_segment(rec_id: int, seq: int, config: dict):
    """PTT 세그먼트를 on-demand로 변환 (백그라운드 스레드)"""
    rec_cfg = _get_recording_config(config)

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT s.*, r.group_id, r.start_time AS rec_start "
                "FROM recording_segments s JOIN recordings r ON s.recording_id=r.id "
                "WHERE s.recording_id=%s AND s.seq=%s",
                (rec_id, seq)
            )
            seg = cur.fetchone()
            if not seg or seg['status'] not in ('raw',):
                return

            cur.execute(
                "UPDATE recording_segments SET status='transcoding' WHERE recording_id=%s AND seq=%s",
                (rec_id, seq)
            )

    raw_audio = seg.get('raw_audio_path', '')
    raw_video = seg.get('raw_video_path', '')
    has_video_seg = seg.get('has_video', 0) and raw_video and os.path.exists(raw_video) and os.path.getsize(raw_video) > 0

    # 출력: raw 파일과 같은 폴더에 저장
    out_dir = os.path.dirname(raw_audio) if raw_audio else '/tmp'
    os.makedirs(out_dir, exist_ok=True)

    if has_video_seg:
        out_path = os.path.join(out_dir, f"seg_{seq:04d}.mp4")
    else:
        out_path = os.path.join(out_dir, f"seg_{seq:04d}.wav")

    try:
        if raw_audio and os.path.exists(raw_audio):
            amr_tmp = raw_audio + '.amr'

            if has_video_seg:
                # audio + video → MP4 muxing
                h264_tmp = raw_video + '.h264'
                amr_ok = _strip_rtp_to_amrwb(raw_audio, amr_tmp)
                h264_ok = _strip_rtp_to_h264(raw_video, h264_tmp)
                if amr_ok and h264_ok:
                    ret = subprocess.run(
                        ['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
                         '-i', amr_tmp, '-f', 'h264', '-i', h264_tmp,
                         '-c:a', 'aac', '-ar', '16000', '-ac', '1',
                         '-c:v', 'copy', '-movflags', '+faststart', out_path],
                        capture_output=True, timeout=120
                    )
                    if ret.returncode != 0:
                        out_path = raw_audio
                for tmp in [amr_tmp, h264_tmp]:
                    try: os.remove(tmp)
                    except: pass
            else:
                # audio only → WAV (LPCM 16kHz mono 16bit)
                if _strip_rtp_to_amrwb(raw_audio, amr_tmp):
                    ret = subprocess.run(
                        ['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
                         '-i', amr_tmp,
                         '-ar', '16000', '-ac', '1', '-c:a', 'pcm_s16le', out_path],
                        capture_output=True, timeout=120
                    )
                    if ret.returncode != 0:
                        out_path = raw_audio
                    try: os.remove(amr_tmp)
                    except: pass
                else:
                    out_path = raw_audio

        file_size = os.path.getsize(out_path) if os.path.exists(out_path) else 0

        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "UPDATE recording_segments SET status='ready', audio_path=%s, file_size=%s "
                    "WHERE recording_id=%s AND seq=%s",
                    (out_path, file_size, rec_id, seq)
                )
    except Exception:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "UPDATE recording_segments SET status='failed' WHERE recording_id=%s AND seq=%s",
                    (rec_id, seq)
                )

    with _transcoding_mutex:
        _transcoding_locks.pop(f"seg_{rec_id}_{seq}", None)


def _ensure_transcoded(rec_id: int, config: dict) -> str:
    """녹취 변환 상태 확인, 필요 시 변환 시작. status 반환."""
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT status FROM recordings WHERE id=%s", (rec_id,))
            row = cur.fetchone()
            if not row:
                return 'not_found'
            status = row['status']

    if status == 'ready':
        return 'ready'
    if status == 'transcoding':
        return 'transcoding'
    if status == 'raw':
        lock_key = f"rec_{rec_id}"
        with _transcoding_mutex:
            if lock_key in _transcoding_locks:
                return 'transcoding'
            _transcoding_locks[lock_key] = True
        t = threading.Thread(target=_transcode_recording, args=(rec_id, config), daemon=True)
        t.start()
        return 'transcoding'
    return status


def _ensure_segment_transcoded(rec_id: int, seq: int, config: dict) -> str:
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT status FROM recording_segments WHERE recording_id=%s AND seq=%s",
                (rec_id, seq)
            )
            row = cur.fetchone()
            if not row:
                return 'not_found'
            status = row['status']

    if status == 'ready':
        return 'ready'
    if status == 'transcoding':
        return 'transcoding'
    if status == 'raw':
        lock_key = f"seg_{rec_id}_{seq}"
        with _transcoding_mutex:
            if lock_key in _transcoding_locks:
                return 'transcoding'
            _transcoding_locks[lock_key] = True
        t = threading.Thread(target=_transcode_segment, args=(rec_id, seq, config), daemon=True)
        t.start()
        return 'transcoding'
    return status


# ──────────────────────────────────────────────────────────────
#  Handler
# ──────────────────────────────────────────────────────────────

_REC_BASE = '/api/v1/recordings'


async def handle_recordings(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    parsed = urlparse(handler_args.full_path)
    qs = parse_qs(parsed.query)
    parts = _path_parts(handler_args.full_path, _REC_BASE)
    method = handler_args.method.upper()

    def qp(name, default=None):
        vals = qs.get(name)
        return unquote(vals[0]) if vals else default

    try:
        if len(parts) == 0:
            if method == 'GET':
                return await _list_recordings(config, qs)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        rec_id = parts[0]

        if len(parts) == 1:
            if method == 'GET':
                return await _get_recording(rec_id, config)
            elif method == 'DELETE':
                return await _delete_recording(rec_id, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        sub = parts[1]

        if sub == 'audio' and method == 'GET':
            return await _stream_audio(rec_id, config)

        if sub == 'video' and method == 'GET':
            side = qp('side', 'a')
            return await _stream_video(rec_id, side, config)

        if sub == 'segments':
            if len(parts) == 2 and method == 'GET':
                return await _list_segments(rec_id, config)
            if len(parts) >= 4 and parts[3] == 'audio' and method == 'GET':
                seq = int(parts[2])
                return await _stream_segment(rec_id, seq, config)

        return HandlerResult(status=404, body={'error': 'Not Found'})
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})


async def _list_recordings(config, qs):
    def qp(name, default=None):
        vals = qs.get(name)
        return unquote(vals[0]) if vals else default

    call_type = qp('call_type')
    caller = qp('caller')
    group_id = qp('group_id')
    from_dt = qp('from_dt')
    to_dt = qp('to_dt')
    limit = min(int(qp('limit', '200')), 1000)
    offset = int(qp('offset', '0'))

    where, params = [], []
    if call_type:
        where.append("call_type=%s"); params.append(call_type)
    if caller:
        where.append("(caller=%s OR callee=%s)"); params += [caller, caller]
    if group_id:
        where.append("group_id=%s"); params.append(group_id)
    if from_dt:
        where.append("start_time >= %s"); params.append(from_dt + ' 00:00:00')
    if to_dt:
        where.append("start_time <= %s"); params.append(to_dt + ' 23:59:59')

    wc = ("WHERE " + " AND ".join(where)) if where else ""

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(f"SELECT COUNT(*) AS cnt FROM recordings {wc}", params)
            total = cur.fetchone()['cnt']
            cur.execute(
                f"SELECT id, call_id, call_type, group_id, caller, callee, "
                f"start_time, end_time, duration, has_video, file_size, status, "
                f"segment_count, total_speech_ms "
                f"FROM recordings {wc} ORDER BY start_time DESC LIMIT %s OFFSET %s",
                params + [limit, offset]
            )
            rows = cur.fetchall()
            for r in rows:
                r['start_time'] = _dt(r['start_time'])
                r['end_time'] = _dt(r['end_time'])

    return HandlerResult(status=200, body={
        'total': total, 'limit': limit, 'offset': offset, 'recordings': rows
    })


async def _get_recording(rec_id, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM recordings WHERE id=%s", (rec_id,))
            row = cur.fetchone()
            if not row:
                return HandlerResult(status=404, body={'error': 'Recording not found'})
            row['start_time'] = _dt(row['start_time'])
            row['end_time'] = _dt(row['end_time'])

            if row['call_type'] == 'ptt':
                cur.execute(
                    "SELECT seq, speaker_id, start_time, end_time, duration_ms, "
                    "has_video, file_size, status "
                    "FROM recording_segments WHERE recording_id=%s ORDER BY seq",
                    (rec_id,)
                )
                segs = cur.fetchall()
                for s in segs:
                    s['start_time'] = _dt(s['start_time'])
                    s['end_time'] = _dt(s['end_time'])
                row['segments'] = segs

    return HandlerResult(status=200, body=row)


async def _delete_recording(rec_id, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT audio_path, video_path_a, video_path_b, "
                "raw_path_a, raw_path_b, raw_path_va, raw_path_vb "
                "FROM recordings WHERE id=%s", (rec_id,)
            )
            row = cur.fetchone()
            if not row:
                return HandlerResult(status=404, body={'error': 'Recording not found'})

            for col in row.values():
                if col and isinstance(col, str) and os.path.exists(col):
                    os.remove(col)

            cur.execute(
                "SELECT audio_path, video_path, raw_audio_path, raw_video_path "
                "FROM recording_segments WHERE recording_id=%s", (rec_id,)
            )
            for seg in cur.fetchall():
                for col in seg.values():
                    if col and isinstance(col, str) and os.path.exists(col):
                        os.remove(col)

            cur.execute("DELETE FROM recording_segments WHERE recording_id=%s", (rec_id,))
            cur.execute("DELETE FROM recordings WHERE id=%s", (rec_id,))

    return HandlerResult(status=200, body={'id': rec_id})


async def _stream_audio(rec_id, config):
    status = _ensure_transcoded(int(rec_id), config)

    if status == 'not_found':
        return HandlerResult(status=404, body={'error': 'Recording not found'})
    if status in ('transcoding', 'raw'):
        return HandlerResult(status=202, body={'status': 'transcoding', 'message': 'Converting, please retry'})
    if status == 'failed':
        return HandlerResult(status=500, body={'error': 'Transcoding failed'})

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT audio_path FROM recordings WHERE id=%s", (rec_id,))
            row = cur.fetchone()
            path = row['audio_path'] if row else None

    if not path or not os.path.exists(path):
        return HandlerResult(status=404, body={'error': 'Audio file not found'})

    ct = 'audio/wav' if path.endswith('.wav') else 'audio/mpeg'
    return HandlerResult(status=200, body=path, headers={
        'Content-Type': ct, 'X-File-Path': path
    })


async def _stream_video(rec_id, side, config):
    status = _ensure_transcoded(int(rec_id), config)

    if status in ('transcoding', 'raw'):
        return HandlerResult(status=202, body={'status': 'transcoding'})
    if status == 'failed':
        return HandlerResult(status=500, body={'error': 'Transcoding failed'})

    col = 'video_path_a' if side == 'a' else 'video_path_b'
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(f"SELECT {col} FROM recordings WHERE id=%s", (rec_id,))
            row = cur.fetchone()
            path = row[col] if row else None

    if not path or not os.path.exists(path):
        return HandlerResult(status=404, body={'error': 'Video file not found'})

    return HandlerResult(status=200, body=path, headers={
        'Content-Type': 'video/mp4', 'X-File-Path': path
    })


async def _list_segments(rec_id, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT seq, speaker_id, start_time, end_time, duration_ms, "
                "has_video, file_size, status "
                "FROM recording_segments WHERE recording_id=%s ORDER BY seq",
                (rec_id,)
            )
            segs = cur.fetchall()
            for s in segs:
                s['start_time'] = _dt(s['start_time'])
                s['end_time'] = _dt(s['end_time'])

    return HandlerResult(status=200, body={'recording_id': rec_id, 'segments': segs})


async def _stream_segment(rec_id, seq, config):
    status = _ensure_segment_transcoded(int(rec_id), seq, config)

    if status == 'not_found':
        return HandlerResult(status=404, body={'error': 'Segment not found'})
    if status in ('transcoding', 'raw'):
        return HandlerResult(status=202, body={'status': 'transcoding'})
    if status == 'failed':
        return HandlerResult(status=500, body={'error': 'Transcoding failed'})

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT audio_path, has_video, video_path "
                "FROM recording_segments WHERE recording_id=%s AND seq=%s",
                (rec_id, seq)
            )
            row = cur.fetchone()
            if not row:
                return HandlerResult(status=404, body={'error': 'Segment not found'})

            if row['has_video'] and row['video_path'] and os.path.exists(row['video_path']):
                path, ct = row['video_path'], 'video/mp4'
            else:
                ap = row['audio_path'] or ''
                ct = 'audio/wav' if ap.endswith('.wav') else 'audio/mpeg'
                path = ap

    if not path or not os.path.exists(path):
        return HandlerResult(status=404, body={'error': 'File not found'})

    return HandlerResult(status=200, body=path, headers={
        'Content-Type': ct, 'X-File-Path': path
    })


CIMS_RECORDING_HANDLER_LIST = [
    (_REC_BASE, handle_recordings, {}),
]
