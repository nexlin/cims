"""
csc_flow.py — 메시지 플로우 + 통화이력 API (파일시스템 기반)

디렉터리 구조:
  {ext_mnt}/calls/YYYY/MM/DD/HH/{prefix}/{caller}/{sanitized_call_id}.d/
    ├── call.json       통화 이력
    ├── csp.jsonl       SIP 메시지 Flow
    ├── participants.jsonl  참여자
    └── raw_a.rtp       녹취 raw
  {ext_mnt}/calls/YYYY/MM/DD/HH/index.json  시간 단위 요약 (JSONL)

API:
  GET /api/v1/flow/list?date=2026-04-08&hour=19  → .d 디렉터리 목록
  GET /api/v1/flow/{call_id}?date=2026-04-08&hour=19  → JSONL 병합
  GET /api/v1/call/logs?date=2026-04-08&hour=19&call_type=voip&limit=50  → 통화 이력 목록
"""

import json
import os
import subprocess
import struct
import glob as _glob
import logging
from datetime import datetime
from urllib.parse import urlparse, parse_qs, unquote

from util.pi_http.http_handler import HandlerArgs, HandlerResult

logger = logging.getLogger(__name__)

_calls_dir: str = ""
_sip_log_dir: str = ""


def init(service_log_dir: str, sip_log_dir: str = "") -> None:
    """ServiceLogDir, SipLogDir 경로 설정"""
    global _calls_dir, _sip_log_dir
    _calls_dir = service_log_dir if service_log_dir else ""
    _sip_log_dir = sip_log_dir if sip_log_dir else ""


def _parse_date(s: str) -> str:
    """YYYY-MM-DD 또는 YYYYMMDD → YYYY-MM-DD"""
    s = s.replace("-", "")
    if len(s) == 8:
        return f"{s[:4]}-{s[4:6]}-{s[6:8]}"
    return datetime.now().strftime("%Y-%m-%d")


def _date_parts(date_str: str):
    d = _parse_date(date_str)
    return d[:4], d[5:7], d[8:10]


def _find_all_d_dirs(date_str: str, hour: str = None, call_type: str = None) -> list:
    """해당 날짜(+시간)의 .d 디렉터리 목록"""
    if not _calls_dir:
        return []
    yyyy, mm, dd = _date_parts(date_str)
    types = [call_type] if call_type else ['voip', 'ptt']
    result = []
    for ct in types:
        if hour:
            base = os.path.join(_calls_dir, ct, yyyy, mm, dd, hour.zfill(2))
        else:
            base = os.path.join(_calls_dir, ct, yyyy, mm, dd)
        if ct == 'voip':
            # voip: HH/{prefix}/{caller}/{call_id}.d
            pat = os.path.join(base, "**", "*.d") if hour else os.path.join(base, "*", "**", "*.d")
        else:
            # ptt: HH/{prefix}/{group_id}.d
            pat = os.path.join(base, "**", "*.d") if hour else os.path.join(base, "*", "**", "*.d")
        result.extend(_glob.glob(pat, recursive=True))
    return sorted(set(d for d in result if os.path.isdir(d)))


def _find_d_dir_by_callid(date_str: str, hour: str, call_id: str, call_type: str = None) -> str:
    """call_id 또는 session_id(sanitized)로 .d 디렉터리 찾기"""
    safe = _sanitize(call_id)

    dirs = _find_all_d_dirs(date_str, hour, call_type)
    # 정확한 이름 매칭
    for d in dirs:
        if os.path.basename(d) == safe + ".d":
            return d

    # 부분 매칭
    prefix = safe[:16] if len(safe) > 16 else safe
    for d in dirs:
        if prefix in os.path.basename(d):
            return d

    # call.json 내 call_id 또는 session.json 내 call_ids로 검색
    for d in dirs:
        for fn in ('call.json', 'session.json'):
            fp = os.path.join(d, fn)
            if os.path.exists(fp):
                try:
                    with open(fp, 'r') as f:
                        content = f.read()
                        if call_id in content:
                            return d
                except:
                    pass

    # hour 없이 재검색
    if hour:
        return _find_d_dir_by_callid(date_str, None, call_id, call_type)
    return ""


def _sanitize(s: str) -> str:
    r = []
    for c in s:
        if c in '/ \\ : * ? " < > |':
            r.append('_')
        else:
            r.append(c)
    return ''.join(r)[:80]


def _load_call_json(d_dir: str) -> dict:
    # VoIP: call.json (단일 JSON)
    path = os.path.join(d_dir, "call.json")
    if os.path.exists(path):
        try:
            with open(path, 'r') as f:
                return json.load(f)
        except:
            pass

    # PTT: call.jsonl (누적 JSONL) — 마지막 세션 정보 반환
    jlpath = os.path.join(d_dir, "call.jsonl")
    if os.path.exists(jlpath):
        entries = _read_jsonl(jlpath)
        if entries:
            last = entries[-1]
            last['call_type'] = 'ptt'
            last['session_count'] = len(entries)
            return last

    # call.json/call.jsonl 모두 없으면 유효한 이력 아님 (sync/integrity 디렉터리)
    return None


def _load_participants(d_dir: str) -> list:
    path = os.path.join(d_dir, "participants.jsonl")
    if not os.path.exists(path):
        return []
    result = []
    try:
        with open(path, 'r') as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        result.append(json.loads(line))
                    except:
                        pass
    except:
        pass
    return result


def _load_messages(d_dir: str) -> list:
    """디렉터리 내 *.jsonl 병합, 시간순 정렬"""
    messages = []
    for jf in _glob.glob(os.path.join(d_dir, "*.jsonl")):
        if os.path.basename(jf) == "participants.jsonl":
            continue
        try:
            with open(jf, 'r') as f:
                for line in f:
                    line = line.strip()
                    if line:
                        try:
                            messages.append(json.loads(line))
                        except:
                            pass
        except:
            pass
    messages.sort(key=lambda m: m.get("ts", ""))
    return messages


def _load_index(date_str: str, hour: str = None) -> list:
    """index.json(JSONL) 읽기"""
    yyyy, mm, dd = _date_parts(date_str)
    if hour:
        path = os.path.join(_calls_dir, yyyy, mm, dd, hour.zfill(2), "index.json")
        if os.path.exists(path):
            return _read_jsonl(path)
        return []
    # hour 미지정 → 모든 시간대 합산
    result = []
    for hh in range(24):
        path = os.path.join(_calls_dir, yyyy, mm, dd, f"{hh:02d}", "index.json")
        if os.path.exists(path):
            result.extend(_read_jsonl(path))
    return result


def _read_jsonl(path: str) -> list:
    result = []
    try:
        with open(path, 'r') as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        result.append(json.loads(line))
                    except:
                        pass
    except:
        pass
    return result


def _has_recording(d_dir: str) -> bool:
    for fn in ('recording_mixed.wav', 'recording_mixed.mp4', 'raw_a.rtp'):
        if os.path.exists(os.path.join(d_dir, fn)):
            return True
    return False


def _has_video_recording(d_dir: str) -> bool:
    for fn in ('raw_va.rtp', 'raw_vb.rtp'):
        p = os.path.join(d_dir, fn)
        if os.path.exists(p) and os.path.getsize(p) > 0:
            return True
    return False


# ── RTP → AMR-WB 추출 ──

def _strip_rtp_to_amrwb(raw_rtp_path: str, out_amr_path: str) -> bool:
    """RTP → AMR-WB 추출. DTX 묵음 구간에 NO_DATA 프레임 삽입으로 타이밍 보정."""
    # AMR-WB NO_DATA frame: ToC = FT=15(no data), F=0, Q=0 → 0x78 + 0 bytes payload
    AMR_NO_DATA_TOC = bytes([0x78])
    TS_INCREMENT = 320  # 20ms @ 16kHz

    try:
        # 1단계: RTP에서 (timestamp, payload) 추출
        frames = []  # [(ts, toc_byte, frame_data), ...]
        with open(raw_rtp_path, 'rb') as fin:
            while True:
                lenbuf = fin.read(4)
                if len(lenbuf) < 4: break
                pkt_len = struct.unpack('<I', lenbuf)[0]
                # recv_usec 존재 여부 판별: 다음 8바이트가 타임스탬프인지 확인
                # 새 형식: [uint32 len][int64 recv_usec][rtp_pkt]
                peek = fin.read(8)
                if len(peek) < 8: break
                maybe_usec = struct.unpack('<q', peek)[0]
                # recv_usec는 2020~2030년 범위 (1.58e15 ~ 1.9e15)
                if 1_500_000_000_000_000 < maybe_usec < 2_000_000_000_000_000:
                    pkt = fin.read(pkt_len)
                else:
                    # 구 형식: [uint32 len][rtp_pkt] — peek가 pkt 시작
                    remaining = pkt_len - 8
                    pkt = peek + (fin.read(remaining) if remaining > 0 else b'')
                if len(pkt) < pkt_len: break
                if pkt_len < 12: continue
                ts = struct.unpack('>I', pkt[4:8])[0]
                cc = pkt[0] & 0x0F
                has_ext = (pkt[0] >> 4) & 0x01
                hdr_len = 12 + cc * 4
                if has_ext and hdr_len + 4 <= pkt_len:
                    ext_len = struct.unpack_from('>H', pkt, hdr_len + 2)[0]
                    hdr_len += 4 + ext_len * 4
                if hdr_len >= pkt_len: continue
                payload = pkt[hdr_len:]
                # AMR-WB RTP payload: CMR(1) + ToC(1) + frame_data
                if len(payload) >= 2:
                    toc = payload[1]
                    frame_data = payload[2:] if len(payload) > 2 else b''
                    frames.append((ts, toc, frame_data))

        if not frames:
            return False

        # 2단계: 타임스탬프 기반으로 AMR-WB 파일 생성 (DTX 묵음 구간 보정)
        with open(out_amr_path, 'wb') as fout:
            fout.write(b'#!AMR-WB\n')

            # 프레임을 타임스탬프 순으로 정렬
            frames.sort(key=lambda x: x[0])

            # 타임스탬프 → 프레임 맵 (중복 ts는 첫 번째만)
            ts_map = {}
            for ts, toc, data in frames:
                if ts not in ts_map:
                    ts_map[ts] = (toc, data)

            # 첫 ts부터 마지막 ts까지 20ms 간격으로 채움
            first_ts = frames[0][0]
            last_ts = frames[-1][0]
            cur_ts = first_ts
            written = 0

            while cur_ts <= last_ts:
                if cur_ts in ts_map:
                    toc, data = ts_map[cur_ts]
                    fout.write(bytes([toc]))
                    fout.write(data)
                else:
                    # DTX 묵음 구간: NO_DATA 프레임 삽입 (FT=15, 0바이트 payload)
                    fout.write(AMR_NO_DATA_TOC)
                cur_ts += TS_INCREMENT
                written += 1

        return os.path.exists(out_amr_path) and os.path.getsize(out_amr_path) > 9
    except Exception as e:
        logger.error("_strip_rtp_to_amrwb: %s", e)
        return False


def _read_rtp_packets(raw_rtp_path: str):
    """RTP 파일에서 (recv_usec, rtp_pkt) 리스트 반환. 구/신 형식 자동 감지."""
    packets = []  # [(recv_usec, pkt), ...]
    try:
        with open(raw_rtp_path, 'rb') as f:
            while True:
                lb = f.read(4)
                if len(lb) < 4: break
                pkt_len = struct.unpack('<I', lb)[0]
                peek = f.read(8)
                if len(peek) < 8: break
                maybe_usec = struct.unpack('<q', peek)[0]
                if 1_500_000_000_000_000 < maybe_usec < 2_000_000_000_000_000:
                    recv_usec = maybe_usec
                    pkt = f.read(pkt_len)
                else:
                    recv_usec = 0
                    remaining = pkt_len - 8
                    pkt = peek + (f.read(remaining) if remaining > 0 else b'')
                if len(pkt) < pkt_len or pkt_len < 12: continue
                packets.append((recv_usec, pkt))
    except:
        pass
    return packets


def _calc_video_fps(raw_rtp_path: str) -> float:
    """RTP 타임스탬프에서 실제 영상 프레임레이트 계산"""
    try:
        packets = _read_rtp_packets(raw_rtp_path)
        timestamps = []
        for _, pkt in packets:
            ts = struct.unpack('>I', pkt[4:8])[0]
            if not timestamps or ts != timestamps[-1]:
                timestamps.append(ts)
        if len(timestamps) > 2:
            total_ts = timestamps[-1] - timestamps[0]
            if total_ts > 0:
                return (len(timestamps) - 1) / (total_ts / 90000.0)
    except:
        pass
    return 15.0  # default


def _calc_stream_offset(audio_rtp_path: str, video_rtp_path: str) -> float:
    """음성/영상 첫 패킷 수신 시간 차이 계산 (초). 양수면 영상이 늦게 시작."""
    try:
        a_pkts = _read_rtp_packets(audio_rtp_path)
        v_pkts = _read_rtp_packets(video_rtp_path)
        if a_pkts and v_pkts and a_pkts[0][0] > 0 and v_pkts[0][0] > 0:
            diff_usec = v_pkts[0][0] - a_pkts[0][0]
            return diff_usec / 1_000_000.0
    except:
        pass
    return 0.0


def _strip_rtp_to_h264(raw_rtp_path: str, out_h264_path: str) -> bool:
    SC = b'\x00\x00\x00\x01'
    fu_buf = bytearray()
    try:
        packets = _read_rtp_packets(raw_rtp_path)
        with open(out_h264_path, 'wb') as fout:
            for _, pkt in packets:
                cc = pkt[0] & 0x0F
                has_ext = (pkt[0] >> 4) & 0x01
                hdr_len = 12 + cc * 4
                if has_ext and hdr_len + 4 <= len(pkt):
                    ext_len = struct.unpack_from('>H', pkt, hdr_len + 2)[0]
                    hdr_len += 4 + ext_len * 4
                if hdr_len >= len(pkt): continue
                pl = pkt[hdr_len:]
                if len(pl) < 1: continue
                nt = pl[0] & 0x1F
                if 1 <= nt <= 23:
                    fout.write(SC + pl)
                elif nt == 28 and len(pl) >= 2:
                    fh = pl[1]; s = (fh >> 7) & 1; e = (fh >> 6) & 1
                    if s:
                        fu_buf = bytearray([(pl[0] & 0xE0) | (fh & 0x1F)])
                        fu_buf.extend(pl[2:])
                    else:
                        fu_buf.extend(pl[2:])
                    if e and fu_buf:
                        fout.write(SC + bytes(fu_buf)); fu_buf = bytearray()
                elif nt == 24:
                    off = 1
                    while off + 2 <= len(pl):
                        nsz = struct.unpack_from('>H', pl, off)[0]; off += 2
                        if off + nsz <= len(pl): fout.write(SC + pl[off:off+nsz])
                        off += nsz
        return os.path.exists(out_h264_path) and os.path.getsize(out_h264_path) > 4
    except Exception as e:
        logger.error("_strip_rtp_to_h264: %s", e)
        return False


def _transcode_audio(d_dir: str) -> str:
    """음성: raw_a.rtp + raw_b.rtp → PCM 16kHz 디코딩 → 모노 믹싱 → recording_mixed.wav"""
    out = os.path.join(d_dir, 'recording_mixed.wav')
    if os.path.exists(out) and os.path.getsize(out) > 44:
        return out

    amr_a = os.path.join(d_dir, 'raw_a.rtp.amr')
    amr_b = os.path.join(d_dir, 'raw_b.rtp.amr')
    wav_a = os.path.join(d_dir, '_tmp_a.wav')
    wav_b = os.path.join(d_dir, '_tmp_b.wav')
    tmps = [amr_a, amr_b, wav_a, wav_b]

    try:
        ok_a = _strip_rtp_to_amrwb(os.path.join(d_dir, 'raw_a.rtp'), amr_a)
        ok_b = _strip_rtp_to_amrwb(os.path.join(d_dir, 'raw_b.rtp'), amr_b)

        if ok_a and ok_b:
            # 양쪽 모두 → PCM 변환 후 믹싱
            for amr, wav in [(amr_a, wav_a), (amr_b, wav_b)]:
                subprocess.run(['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
                                '-i', amr, '-ar', '16000', '-ac', '1', '-c:a', 'pcm_s16le', wav],
                               capture_output=True, timeout=60)
            # amix 필터로 모노 믹싱 (볼륨 정규화)
            subprocess.run(['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
                            '-i', wav_a, '-i', wav_b,
                            '-filter_complex', '[0:a][1:a]amix=inputs=2:duration=longest:normalize=0[out]',
                            '-map', '[out]', '-ar', '16000', '-ac', '1', '-c:a', 'pcm_s16le', out],
                           capture_output=True, timeout=60)
        elif ok_a:
            subprocess.run(['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
                            '-i', amr_a, '-ar', '16000', '-ac', '1', '-c:a', 'pcm_s16le', out],
                           capture_output=True, timeout=60)
        elif ok_b:
            subprocess.run(['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
                            '-i', amr_b, '-ar', '16000', '-ac', '1', '-c:a', 'pcm_s16le', out],
                           capture_output=True, timeout=60)
    except Exception as e:
        logger.error("_transcode_audio: %s", e)
    finally:
        for t in tmps:
            try: os.remove(t)
            except: pass

    return out if os.path.exists(out) and os.path.getsize(out) > 44 else ''


def _transcode_video(d_dir: str) -> str:
    """영상: raw_va/vb → H.264 추출 → 좌(발신)/우(착신) 배치 + 음성 믹싱 → recording_mixed.mp4"""
    out = os.path.join(d_dir, 'recording_mixed.mp4')
    if os.path.exists(out) and os.path.getsize(out) > 100:
        return out

    h264_a = os.path.join(d_dir, '_tmp_va.h264')
    h264_b = os.path.join(d_dir, '_tmp_vb.h264')
    amr_a = os.path.join(d_dir, '_tmp_a.amr')
    amr_b = os.path.join(d_dir, '_tmp_b.amr')
    tmps = [h264_a, h264_b, amr_a, amr_b]

    try:
        ok_va = _strip_rtp_to_h264(os.path.join(d_dir, 'raw_va.rtp'), h264_a)
        ok_vb = _strip_rtp_to_h264(os.path.join(d_dir, 'raw_vb.rtp'), h264_b)
        ok_aa = _strip_rtp_to_amrwb(os.path.join(d_dir, 'raw_a.rtp'), amr_a)
        ok_ab = _strip_rtp_to_amrwb(os.path.join(d_dir, 'raw_b.rtp'), amr_b)

        # RTP 타임스탬프에서 실제 framerate 계산
        fps_a = _calc_video_fps(os.path.join(d_dir, 'raw_va.rtp')) if ok_va else 15.0
        fps_b = _calc_video_fps(os.path.join(d_dir, 'raw_vb.rtp')) if ok_vb else fps_a
        fps_str_a = f'{fps_a:.2f}'
        fps_str_b = f'{fps_b:.2f}'

        inputs = []
        filter_parts = []

        if ok_va and ok_vb:
            # 좌(발신) + 우(착신) side-by-side
            inputs += ['-f', 'h264', '-framerate', fps_str_a, '-i', h264_a,
                       '-f', 'h264', '-framerate', fps_str_b, '-i', h264_b]
            filter_parts.append('[0:v][1:v]hstack=inputs=2[vout]')
            vmap = '[vout]'
            vid_idx = 2  # audio inputs start after 2 video inputs
        elif ok_va:
            inputs += ['-f', 'h264', '-framerate', fps_str_a, '-i', h264_a]
            vmap = '0:v'
            vid_idx = 1
        elif ok_vb:
            inputs += ['-f', 'h264', '-framerate', fps_str_b, '-i', h264_b]
            vmap = '0:v'
            vid_idx = 1
        else:
            return ''

        # 음성 믹싱 추가
        audio_inputs = []
        amix_srcs = []
        if ok_aa:
            audio_inputs += ['-i', amr_a]
            amix_srcs.append(f'[{vid_idx}:a]')
            vid_idx += 1
        if ok_ab:
            audio_inputs += ['-i', amr_b]
            amix_srcs.append(f'[{vid_idx}:a]')
            vid_idx += 1

        if len(amix_srcs) == 2:
            filter_parts.append(f'{amix_srcs[0]}{amix_srcs[1]}amix=inputs=2:duration=longest:normalize=0[aout]')
            amap = '[aout]'
        elif len(amix_srcs) == 1:
            amap = amix_srcs[0]
        else:
            amap = None

        # 음성/영상 시작 시점 오프셋 계산 (recv_usec 기반)
        audio_offset = 0.0
        video_offset = 0.0
        if ok_va and ok_aa:
            av_offset = _calc_stream_offset(
                os.path.join(d_dir, 'raw_a.rtp'),
                os.path.join(d_dir, 'raw_va.rtp'))
            if av_offset > 0.01:  # 영상이 음성보다 10ms 이상 늦게 시작
                audio_offset = av_offset  # 음성을 영상만큼 지연
            elif av_offset < -0.01:  # 음성이 영상보다 늦게 시작
                video_offset = -av_offset  # 영상을 음성만큼 지연

        cmd = ['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error']
        # 영상 입력에 오프셋 적용
        if video_offset > 0:
            cmd += ['-itsoffset', f'{video_offset:.3f}']
        cmd += inputs
        # 음성 입력에 오프셋 적용
        if audio_offset > 0:
            cmd += ['-itsoffset', f'{audio_offset:.3f}']
        cmd += audio_inputs

        if filter_parts:
            cmd += ['-filter_complex', ';'.join(filter_parts)]

        cmd += ['-map', vmap]
        if amap:
            cmd += ['-map', amap, '-c:a', 'aac', '-ar', '16000', '-ac', '1']
        cmd += ['-c:v', 'libx264', '-preset', 'fast', '-movflags', '+faststart',
                '-shortest', out]

        r = subprocess.run(cmd, capture_output=True, timeout=300)
        if r.returncode != 0:
            logger.error("_transcode_video ffmpeg error: %s", r.stderr.decode(errors='replace')[:500])
    except Exception as e:
        logger.error("_transcode_video: %s", e)
    finally:
        for t in tmps:
            try: os.remove(t)
            except: pass

    return out if os.path.exists(out) and os.path.getsize(out) > 100 else ''


# ── SIP Log 검색 ──

def _load_session_json(d_dir: str) -> dict:
    """session.json에서 session_id, call_ids 읽기"""
    path = os.path.join(d_dir, "session.json")
    if os.path.exists(path):
        try:
            with open(path, 'r') as f:
                return json.load(f)
        except Exception:
            pass
    return {}


def _search_sip_messages(call_ids: list, date_str: str, hour: str = None) -> list:
    """sip.jsonl에서 call_ids에 해당하는 메시지 검색"""
    if not _sip_log_dir or not call_ids:
        return []

    yyyy, mm, dd = _date_parts(date_str)
    results = []

    # 검색할 시간 범위 결정
    if hour:
        hours = [hour.zfill(2)]
    else:
        hours = [f"{h:02d}" for h in range(24)]

    call_id_set = set(call_ids)

    for hh in hours:
        jsonl_path = os.path.join(_sip_log_dir, yyyy, mm, dd, hh, "sip.jsonl")
        if not os.path.exists(jsonl_path):
            continue
        try:
            with open(jsonl_path, 'r') as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    # 빠른 사전 필터: call_id 문자열이 라인에 포함되는지 확인
                    matched = any(cid in line for cid in call_id_set)
                    if not matched:
                        continue
                    try:
                        obj = json.loads(line)
                    except Exception:
                        continue
                    msg_cid = obj.get("call_id", "")
                    if msg_cid in call_id_set:
                        results.append(obj)
        except Exception as e:
            logger.error("_search_sip_messages: %s", e)

    return results


def _search_cmp_messages(call_ids: list, date_str: str, hour: str = None,
                         time_start: str = "", time_end: str = "",
                         call_type: str = "voip") -> list:
    """sip.jsonl에서 CMP(proto=JSON) 메시지 중 시간 범위에 해당하는 것 검색"""
    if not _sip_log_dir:
        return []

    yyyy, mm, dd = _date_parts(date_str)
    results = []

    if hour:
        hours = [hour.zfill(2)]
    else:
        hours = [f"{h:02d}" for h in range(24)]

    for hh in hours:
        jsonl_path = os.path.join(_sip_log_dir, yyyy, mm, dd, hh, "sip.jsonl")
        if not os.path.exists(jsonl_path):
            continue
        try:
            with open(jsonl_path, 'r') as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    if '"proto"' not in line or '"JSON"' not in line:
                        continue
                    try:
                        obj = json.loads(line)
                    except Exception:
                        continue
                    if obj.get("proto") != "JSON":
                        continue
                    # CMP heartbeat 필터링
                    method = obj.get("method", "")
                    if method in ("alive", "response"):
                        continue
                    # VoIP: 세션 명령만 (add/remove/modify)
                    # PTT: 그룹 명령만 (addgroup/joingroup/leavegroup/removegroup)
                    if call_type == "voip" and method in ("addgroup", "removegroup", "joingroup",
                                                           "leavegroup", "modifygroup"):
                        continue
                    if call_type == "ptt" and method in ("add", "remove", "modify"):
                        continue
                    # 시간 범위 필터
                    ts = obj.get("ts", "")
                    if time_start and ts < time_start:
                        continue
                    if time_end and ts > time_end:
                        continue
                    results.append(obj)
        except Exception as e:
            logger.error("_search_cmp_messages: %s", e)

    return results


def _derive_flow_message(obj: dict, call_ids: list, initiator: str, callee: str) -> dict:
    """sip.jsonl 메시지를 FlowMessage 형식으로 변환

    B2BUA 구조: call_ids[0] = originating leg, call_ids[1] = terminating leg
    - Originating leg (call_ids[0]): ue_o <-> csp
    - Terminating leg (call_ids[1]): csp <-> ue_t
    - CMP JSON: csp <-> cmp
    """
    proto = obj.get("proto", "SIP")
    direction = obj.get("dir", "")
    msg_cid = obj.get("call_id", "")
    method = obj.get("method", "")
    body = obj.get("msg", "")

    # CMP JSON 메시지
    if proto == "JSON":
        if direction == "TX":
            from_actor, to_actor = "csp", "cmp"
        else:
            from_actor, to_actor = "cmp", "csp"
        return {
            "ts": obj.get("ts", ""),
            "from": from_actor,
            "to": to_actor,
            "proto": proto,
            "label": method,
            "body": body,
        }

    # SIP 메시지: leg 판별
    is_orig_leg = True  # 기본: originating leg
    if len(call_ids) >= 2 and msg_cid == call_ids[1]:
        is_orig_leg = False
    elif len(call_ids) >= 2 and msg_cid == call_ids[0]:
        is_orig_leg = True
    elif len(call_ids) == 1:
        # 단일 Call-ID (Proxy 모드): from_uri/to_uri로 판별
        from_uri = obj.get("from_uri", "")
        to_uri = obj.get("to_uri", "")
        if direction == "RX":
            # 수신: 발신자로부터 오면 ue_o->csp, 착신자로부터 오면 ue_t->csp
            if callee and (callee in from_uri):
                is_orig_leg = False
            else:
                is_orig_leg = True
        else:
            # 송신: 착신자에게 보내면 csp->ue_t, 발신자에게 보내면 csp->ue_o
            if callee and (callee in to_uri):
                is_orig_leg = False
            else:
                is_orig_leg = True

    if is_orig_leg:
        if direction == "RX":
            from_actor, to_actor = "ue_o", "csp"
        else:
            from_actor, to_actor = "csp", "ue_o"
    else:
        if direction == "TX":
            from_actor, to_actor = "csp", "ue_t"
        else:
            from_actor, to_actor = "ue_t", "csp"

    return {
        "ts": obj.get("ts", ""),
        "from": from_actor,
        "to": to_actor,
        "proto": "SIP",
        "label": method,
        "body": body,
    }


def _build_flow_from_sip_log(d_dir: str, date_str: str, hour: str = None) -> list:
    """sip.jsonl 기반으로 FlowMessage 목록 생성"""
    # call.json에서 initiator/callee 정보 로드
    call_json = _load_call_json(d_dir)
    initiator = call_json.get("initiator", "") if call_json else ""
    callee = call_json.get("callee", "") if call_json else ""

    # session.json에서 Call-ID 목록 로드 (B2BUA: 2개, Proxy: 1개)
    session = _load_session_json(d_dir)
    call_ids = session.get("call_ids", [])

    # session.json이 없으면 call.json의 call_id 사용
    if not call_ids:
        cid = call_json.get("call_id", "") if call_json else ""
        if cid:
            call_ids = [cid]

    if not call_ids:
        # SIP log 없이 기존 방식 fallback
        return _load_messages(d_dir)

    # SIP 메시지 검색
    sip_msgs = _search_sip_messages(call_ids, date_str, hour)

    # 시간 범위 추출 (CMP 메시지 필터링용, 1초 여유)
    time_start = ""
    time_end = ""
    if sip_msgs:
        times = [m.get("ts", "") for m in sip_msgs if m.get("ts")]
        if times:
            time_start = min(times)
            # time_end에 1초 여유 추가 (BYE 직후 CMP remove 포함)
            te = max(times)
            try:
                parts = te.split(":")
                secs = float(parts[2]) + 1.0
                if secs >= 60:
                    secs -= 60; parts[1] = f"{int(parts[1])+1:02d}"
                parts[2] = f"{secs:09.6f}"
                time_end = ":".join(parts)
            except:
                time_end = te

    # CMP 메시지 검색 (시간 범위 내, call_type에 따라 필터)
    ct = call_json.get("call_type", "voip") if call_json else "voip"
    cmp_msgs = _search_cmp_messages(call_ids, date_str, hour, time_start, time_end, ct)

    # FlowMessage 형식으로 변환
    messages = []
    for obj in sip_msgs:
        messages.append(_derive_flow_message(obj, call_ids, initiator, callee))
    for obj in cmp_msgs:
        messages.append(_derive_flow_message(obj, call_ids, initiator, callee))

    # 시간순 정렬
    messages.sort(key=lambda m: m.get("ts", ""))
    return messages


# ── Flow API ──

async def _handle_flow(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    if handler_args.method != "GET":
        return HandlerResult(status=405, body="Method Not Allowed")

    full_path = handler_args.full_path or ""
    after = full_path[len("/api/v1/flow"):].lstrip("/")
    qs = parse_qs(urlparse(full_path).query)
    date_str = qs.get("date", [datetime.now().strftime("%Y-%m-%d")])[0]
    hour = qs.get("hour", [None])[0]

    if after == "" or after == "list":
        dirs = _find_all_d_dirs(date_str, hour)
        call_ids = [os.path.basename(d).replace(".d", "") for d in dirs]
        return HandlerResult(status=200, body=json.dumps({
            "date": date_str, "hour": hour, "call_ids": call_ids, "count": len(call_ids)
        }), media_type="application/json")

    # call_id lookup
    call_id = unquote(after)
    d_dir = _find_d_dir_by_callid(date_str, hour, call_id)
    if not d_dir:
        return HandlerResult(status=404, body=f"'{call_id}' not found for date {date_str}")

    # SIP log 기반 메시지 로딩
    if _sip_log_dir:
        messages = _build_flow_from_sip_log(d_dir, date_str, hour)
    else:
        # fallback: 기존 csp.jsonl 기반
        messages = _load_messages(d_dir)

    return HandlerResult(status=200, body=json.dumps({
        "call_id": call_id, "date": date_str, "messages": messages,
    }), media_type="application/json")


# ── Call Logs API (DB 대체) ──

async def _handle_call_logs(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    if handler_args.method != "GET":
        return HandlerResult(status=405, body="Method Not Allowed")

    qs = parse_qs(urlparse(handler_args.full_path).query)
    date_str = qs.get("date", [datetime.now().strftime("%Y-%m-%d")])[0]
    hour = qs.get("hour", [None])[0]
    call_type = qs.get("call_type", [None])[0]
    msisdn = qs.get("msisdn", [None])[0]
    group_id = qs.get("group_id", [None])[0]
    limit = min(int(qs.get("limit", ["200"])[0]), 1000)
    offset = int(qs.get("offset", ["0"])[0])

    # index.json 기반 빠른 조회 시도
    index_entries = _load_index(date_str, hour)

    # index가 비어있으면 .d 디렉터리 직접 스캔
    if not index_entries:
        dirs = _find_all_d_dirs(date_str, hour, call_type)
        logs = []
        for d in dirs:
            cj = _load_call_json(d)
            if not cj:
                continue
            cj['participants'] = _load_participants(d)
            cj['has_recording'] = _has_recording(d)
            cj['dir_name'] = os.path.basename(d).replace('.d', '')
            logs.append(cj)
    else:
        # index에서 dir 경로로 call.json 로드
        logs = []
        for entry in index_entries:
            dir_name = entry.get('dir')
            if not dir_name:
                continue
            d_dir = _find_d_dir_by_callid(date_str, hour, dir_name.replace('.d', ''))
            if d_dir:
                cj = _load_call_json(d_dir)
                if cj:
                    cj['participants'] = _load_participants(d_dir)
                    cj['has_recording'] = _has_recording(d_dir)
                    cj['dir_name'] = os.path.basename(d_dir).replace('.d', '')
                    logs.append(cj)

    # 필터
    if call_type:
        logs = [l for l in logs if l.get('call_type') == call_type]
    if msisdn:
        logs = [l for l in logs if msisdn in l.get('initiator', '') or msisdn in l.get('callee', '') or
                any(msisdn in p.get('msisdn', '') for p in l.get('participants', []))]
    if group_id:
        logs = [l for l in logs if l.get('group_id') == group_id]

    # 정렬 (최신 순)
    logs.sort(key=lambda l: l.get('invite_time', ''), reverse=True)

    total = len(logs)
    paged = logs[offset:offset + limit]

    # end_reason_ko 추가
    reason_map = {'normal': '정상종료', 'no_answer': '무응답', 'busy': '통화중',
                  'rejected': '거절', 'error': '오류', 'timeout': '시간초과'}
    for l in paged:
        l['end_reason_ko'] = reason_map.get(l.get('end_reason', ''), l.get('end_reason', ''))

    return HandlerResult(status=200, body=json.dumps({
        "total": total, "limit": limit, "offset": offset, "logs": paged,
    }), media_type="application/json")


# ── Recordings API (DB 대체) ──

async def _handle_recordings(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    if handler_args.method != "GET":
        return HandlerResult(status=405, body="Method Not Allowed")

    full_path = handler_args.full_path or ""
    qs = parse_qs(urlparse(full_path).query)
    after = full_path[len("/api/v1/recordings"):].lstrip("/")

    if not after:
        # 목록 조회
        date_str = qs.get("date", [datetime.now().strftime("%Y-%m-%d")])[0]
        hour = qs.get("hour", [None])[0]
        call_type = qs.get("call_type", [None])[0]
        limit = min(int(qs.get("limit", ["200"])[0]), 1000)
        offset = int(qs.get("offset", ["0"])[0])

        dirs = _find_all_d_dirs(date_str, hour)
        recordings = []
        for d in dirs:
            if _has_recording(d):
                cj = _load_call_json(d)
                if call_type and cj.get('call_type') != call_type:
                    continue
                cj['dir_name'] = os.path.basename(d).replace('.d', '')
                cj['has_recording'] = True
                recordings.append(cj)

        recordings.sort(key=lambda r: r.get('invite_time', ''), reverse=True)
        total = len(recordings)
        paged = recordings[offset:offset + limit]

        return HandlerResult(status=200, body=json.dumps({
            "total": total, "recordings": paged,
        }), media_type="application/json")

    # {call_id}/audio 또는 {call_id}/video
    parts = after.split("/")
    call_id = unquote(parts[0])
    sub = parts[1] if len(parts) > 1 else None
    date_str = qs.get("date", [datetime.now().strftime("%Y-%m-%d")])[0]
    hour = qs.get("hour", [None])[0]

    d_dir = _find_d_dir_by_callid(date_str, hour, call_id)
    if not d_dir:
        return HandlerResult(status=404, body=json.dumps({"error": "Not found"}))

    if sub == "audio":
        # on-demand 변환: raw_a.rtp + raw_b.rtp → recording_mixed.wav
        mixed_wav = os.path.join(d_dir, 'recording_mixed.wav')
        if not (os.path.exists(mixed_wav) and os.path.getsize(mixed_wav) > 44):
            mixed_wav = _transcode_audio(d_dir)
        if mixed_wav and os.path.exists(mixed_wav) and os.path.getsize(mixed_wav) > 44:
            return HandlerResult(status=200, body=mixed_wav, headers={
                'Content-Type': 'audio/wav', 'X-File-Path': mixed_wav
            })
        return HandlerResult(status=404, body=json.dumps({"error": "Audio not found"}))

    if sub == "video":
        # on-demand 변환: raw_va/vb.rtp → recording_mixed.mp4 (좌발신/우착신)
        mixed_mp4 = os.path.join(d_dir, 'recording_mixed.mp4')
        if not (os.path.exists(mixed_mp4) and os.path.getsize(mixed_mp4) > 100):
            if _has_video_recording(d_dir):
                mixed_mp4 = _transcode_video(d_dir)
        if mixed_mp4 and os.path.exists(mixed_mp4) and os.path.getsize(mixed_mp4) > 100:
            return HandlerResult(status=200, body=mixed_mp4, headers={
                'Content-Type': 'video/mp4', 'X-File-Path': mixed_mp4
            })
        return HandlerResult(status=404, body=json.dumps({"error": "Video not found"}))

    # 상세
    cj = _load_call_json(d_dir)
    cj['participants'] = _load_participants(d_dir)
    cj['has_recording'] = _has_recording(d_dir)
    return HandlerResult(status=200, body=json.dumps(cj), media_type="application/json")


# ── PTT History API ──

def _find_ptt_sessions(group_id: str) -> list:
    """PTT 그룹의 세션 디렉터리 목록 반환"""
    if not _calls_dir:
        return []
    safe_gid = _sanitize(group_id)
    sessions_base = os.path.join(_calls_dir, "ptt", safe_gid, "sessions")
    if not os.path.isdir(sessions_base):
        return []
    result = []
    for entry in sorted(os.listdir(sessions_base), reverse=True):
        d_path = os.path.join(sessions_base, entry)
        if os.path.isdir(d_path) and entry.endswith(".d"):
            sj = _load_session_json(d_path)
            if not sj:
                # session.json 없으면 디렉터리 이름에서 추론
                sj = {"session_id": entry.replace(".d", "")}
            sj["dir"] = entry.replace(".d", "")
            result.append(sj)
    return result


def _find_ptt_session_dir(group_id: str, session_dir: str) -> str:
    """PTT 세션 디렉터리 경로 반환"""
    if not _calls_dir:
        return ""
    safe_gid = _sanitize(group_id)
    safe_sid = _sanitize(session_dir)
    d_path = os.path.join(_calls_dir, "ptt", safe_gid, "sessions", safe_sid + ".d")
    if os.path.isdir(d_path):
        return d_path
    # .d 없이 시도
    d_path2 = os.path.join(_calls_dir, "ptt", safe_gid, "sessions", safe_sid)
    if os.path.isdir(d_path2):
        return d_path2
    return ""


def _load_ptt_events(d_dir: str, date: str = None) -> list:
    """PTT 세션 이벤트 로드. date 지정 시 daily/YYYY-MM-DD.jsonl, 아니면 events.jsonl"""
    events = []
    if date:
        daily_path = os.path.join(d_dir, "daily", f"{date}.jsonl")
        if os.path.exists(daily_path):
            events.extend(_read_jsonl(daily_path))
    else:
        events_path = os.path.join(d_dir, "events.jsonl")
        if os.path.exists(events_path):
            events.extend(_read_jsonl(events_path))

    # cmp.jsonl 병합 (floor 이벤트)
    cmp_path = os.path.join(d_dir, "cmp.jsonl")
    if os.path.exists(cmp_path):
        cmp_events = _read_jsonl(cmp_path)
        if date:
            cmp_events = [e for e in cmp_events if e.get("ts", "").startswith(date)]
        events.extend(cmp_events)

    events.sort(key=lambda e: e.get("ts", ""))
    return events


def _search_sip_for_group(group_id: str, date_str: str) -> list:
    """SIP 로그에서 group_id가 포함된 메시지 검색 (To/From URI)"""
    if not _sip_log_dir or not group_id:
        return []

    yyyy, mm, dd = _date_parts(date_str)
    results = []

    for hh in range(24):
        jsonl_path = os.path.join(_sip_log_dir, yyyy, mm, dd, f"{hh:02d}", "sip.jsonl")
        if not os.path.exists(jsonl_path):
            continue
        try:
            with open(jsonl_path, 'r') as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    # 빠른 필터: group_id 문자열 포함 여부
                    if group_id not in line:
                        continue
                    try:
                        obj = json.loads(line)
                    except Exception:
                        continue
                    # To/From URI 또는 call_id에 group_id 포함 확인
                    from_uri = obj.get("from_uri", "")
                    to_uri = obj.get("to_uri", "")
                    msg_body = obj.get("msg", "")
                    if (group_id in from_uri or group_id in to_uri or
                            group_id in msg_body):
                        results.append(obj)
        except Exception as e:
            logger.error("_search_sip_for_group: %s", e)

    return results


async def _handle_ptt_history(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """PTT History API

    GET /api/v1/ptt/history?group_id=X           — 그룹 세션 목록
    GET /api/v1/ptt/history/{group_id}/{session}  — 세션 이벤트
    GET /api/v1/ptt/history/{group_id}/{session}/flow — SIP Flow
    """
    if handler_args.method != "GET":
        return HandlerResult(status=405, body="Method Not Allowed")

    full_path = handler_args.full_path or ""
    qs = parse_qs(urlparse(full_path).query)
    after = full_path.split("?")[0]
    after = after[len("/api/v1/ptt/history"):].lstrip("/")

    if not after:
        # ── 세션 목록 ──
        group_id = qs.get("group_id", [None])[0]
        if not group_id:
            return HandlerResult(status=400, body=json.dumps({"error": "group_id required"}),
                                 media_type="application/json")

        # 1) sessions 디렉터리 스캔
        sessions = _find_ptt_sessions(group_id)

        # 2) index.jsonl 빠른 조회 시도
        if not sessions:
            safe_gid = _sanitize(group_id)
            idx_path = os.path.join(_calls_dir, "ptt", safe_gid, "index.jsonl")
            if os.path.exists(idx_path):
                sessions = _read_jsonl(idx_path)

        # 3) 날짜 기반 .d 디렉터리 스캔 (fallback)
        if not sessions:
            date_str = qs.get("date", [datetime.now().strftime("%Y-%m-%d")])[0]
            dirs = _find_all_d_dirs(date_str, call_type="ptt")
            for d in dirs:
                cj = _load_call_json(d)
                if cj and cj.get("group_id") == group_id:
                    sj = _load_session_json(d)
                    entry = {
                        "dir": os.path.basename(d).replace(".d", ""),
                        "start_time": cj.get("invite_time", ""),
                        "end_time": cj.get("end_time"),
                        "state": cj.get("state", ""),
                        "initiator": cj.get("initiator", ""),
                        "member_count": len(cj.get("participants", [])) if "participants" in cj else
                                        len(_load_participants(d)),
                    }
                    if sj:
                        entry.update({k: v for k, v in sj.items() if k not in entry or not entry[k]})
                    sessions.append(entry)

        return HandlerResult(status=200, body=json.dumps({
            "group_id": group_id,
            "sessions": sessions,
        }), media_type="application/json")

    parts = after.split("/")
    group_id = unquote(parts[0])

    if len(parts) < 2:
        # group_id만 있으면 세션 목록으로 리다이렉트
        sessions = _find_ptt_sessions(group_id)
        return HandlerResult(status=200, body=json.dumps({
            "group_id": group_id, "sessions": sessions,
        }), media_type="application/json")

    session_dir = unquote(parts[1])

    if len(parts) >= 3 and parts[2] == "flow":
        # ── SIP Flow ──
        date_str = qs.get("date", [datetime.now().strftime("%Y-%m-%d")])[0]

        # 세션 디렉터리에서 call_ids 확인
        d_dir = _find_ptt_session_dir(group_id, session_dir)
        if d_dir:
            session = _load_session_json(d_dir)
            call_ids = session.get("call_ids", [])
            if call_ids:
                sip_msgs = _search_sip_messages(call_ids, date_str)
                call_json = _load_call_json(d_dir)
                initiator = call_json.get("initiator", "") if call_json else ""
                callee = call_json.get("callee", "") if call_json else ""
                messages = []
                for obj in sip_msgs:
                    messages.append(_derive_flow_message(obj, call_ids, initiator, callee))

                # CMP 메시지도 추가
                time_start = ""
                time_end = ""
                if sip_msgs:
                    times = [m.get("ts", "") for m in sip_msgs if m.get("ts")]
                    if times:
                        time_start = min(times)
                        time_end = max(times)
                cmp_msgs = _search_cmp_messages(call_ids, date_str,
                                                 time_start=time_start, time_end=time_end,
                                                 call_type="ptt")
                for obj in cmp_msgs:
                    messages.append(_derive_flow_message(obj, call_ids, initiator, callee))

                messages.sort(key=lambda m: m.get("ts", ""))
                return HandlerResult(status=200, body=json.dumps({
                    "call_id": group_id, "date": date_str, "messages": messages,
                }), media_type="application/json")

        # Fallback: group_id로 SIP 로그 직접 검색
        sip_raw = _search_sip_for_group(group_id, date_str)
        messages = []
        for obj in sip_raw:
            fm = {
                "ts": obj.get("ts", ""),
                "from": "ue_o" if obj.get("dir") == "RX" else "csp",
                "to": "csp" if obj.get("dir") == "RX" else "ue_t",
                "proto": obj.get("proto", "SIP"),
                "label": obj.get("method", ""),
                "body": obj.get("msg", ""),
            }
            messages.append(fm)
        messages.sort(key=lambda m: m.get("ts", ""))

        return HandlerResult(status=200, body=json.dumps({
            "call_id": group_id, "date": date_str, "messages": messages,
        }), media_type="application/json")

    else:
        # ── 세션 이벤트 ──
        date = qs.get("date", [None])[0]

        d_dir = _find_ptt_session_dir(group_id, session_dir)
        if not d_dir:
            # fallback: 날짜 기반 검색
            fallback_date = date or session_dir[:4] + "-" + session_dir[4:6] + "-" + session_dir[6:8] \
                if len(session_dir) >= 8 and session_dir[:8].isdigit() else \
                datetime.now().strftime("%Y-%m-%d")
            d_dir = _find_d_dir_by_callid(fallback_date, None, session_dir, "ptt")

        if not d_dir:
            return HandlerResult(status=404, body=json.dumps({"error": "Session not found"}),
                                 media_type="application/json")

        session_meta = _load_session_json(d_dir)
        call_json = _load_call_json(d_dir)
        if call_json and not session_meta:
            session_meta = call_json

        group_snapshot = session_meta.get("group_snapshot", {}) if session_meta else {}
        events = _load_ptt_events(d_dir, date)

        # participants 정보도 포함
        participants = _load_participants(d_dir)

        return HandlerResult(status=200, body=json.dumps({
            "session": session_meta or {},
            "group_snapshot": group_snapshot,
            "events": events,
            "participants": participants,
        }), media_type="application/json")


FLOW_HANDLER_LIST = [
    ("/api/v1/flow", _handle_flow, {}),
    ("/api/v1/call/logs", _handle_call_logs, {}),
    ("/api/v1/recordings", _handle_recordings, {}),
    ("/api/v1/ptt/history", _handle_ptt_history, {}),
]
