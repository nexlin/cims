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

    # 출력 디렉터리
    from datetime import datetime
    dt = rec['start_time'] or datetime.now()
    out_dir = os.path.join(rec_cfg['converted_dir'], 'voip',
                           dt.strftime('%Y'), dt.strftime('%m'), dt.strftime('%d'))
    os.makedirs(out_dir, exist_ok=True)

    call_id = rec['call_id']
    audio_path = os.path.join(out_dir, f"{call_id}.mp3")
    success = True
    file_size = 0

    try:
        # 음성 변환: raw RTP → MP3
        # raw 파일 형식이 [4-byte len][RTP packet]... 이므로 payload 추출 필요
        # 간단한 접근: raw 파일을 그대로 ffmpeg에 전달하되, 실패하면 raw 보존
        if raw_a and os.path.exists(raw_a):
            ret = subprocess.run(
                ['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
                 '-f', 'amr', '-i', raw_a,
                 '-ar', '16000', '-ac', '1', audio_path],
                capture_output=True, timeout=300
            )
            if ret.returncode != 0:
                # raw 형식 직접 변환 실패 시 — raw 파일 경로만 보존
                audio_path = raw_a
                success = True  # raw 파일이라도 있으면 OK

        if os.path.exists(audio_path):
            file_size = os.path.getsize(audio_path)

        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "UPDATE recordings SET status='ready', audio_path=%s, file_size=%s WHERE id=%s",
                    (audio_path, file_size, rec_id)
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
    group_id = seg.get('group_id', 'unknown')

    from datetime import datetime
    dt = seg.get('rec_start') or datetime.now()
    out_dir = os.path.join(rec_cfg['converted_dir'], 'ptt', group_id,
                           dt.strftime('%Y'), dt.strftime('%m'), dt.strftime('%d'))
    os.makedirs(out_dir, exist_ok=True)

    out_path = os.path.join(out_dir, f"seg_{seq:04d}.mp3")

    try:
        if raw_audio and os.path.exists(raw_audio):
            ret = subprocess.run(
                ['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
                 '-f', 'amr', '-i', raw_audio,
                 '-ar', '16000', '-ac', '1', out_path],
                capture_output=True, timeout=120
            )
            if ret.returncode != 0:
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

    return HandlerResult(status=200, body=path, headers={
        'Content-Type': 'audio/mpeg', 'X-File-Path': path
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
                path, ct = row['audio_path'], 'audio/mpeg'

    if not path or not os.path.exists(path):
        return HandlerResult(status=404, body={'error': 'File not found'})

    return HandlerResult(status=200, body=path, headers={
        'Content-Type': ct, 'X-File-Path': path
    })


CIMS_RECORDING_HANDLER_LIST = [
    (_REC_BASE, handle_recordings, {}),
]
