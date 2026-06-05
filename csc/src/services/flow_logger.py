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
from datetime import datetime, timedelta
from urllib.parse import urlparse, parse_qs, unquote

from httpsrv.handler import HandlerArgs, HandlerResult

logger = logging.getLogger(__name__)

_calls_dir: str = ""
_sip_log_dir: str = ""
_msg_log_dir: str = ""
_system_id: str = "csp_01"


def init(service_log_dir: str, sip_log_dir: str = "",
         msg_log_dir: str = "", system_id: str = "csp_01") -> None:
    """ServiceLogging Dir 설정 (통합 디렉토리)"""
    global _calls_dir, _sip_log_dir, _msg_log_dir, _system_id
    _calls_dir = service_log_dir if service_log_dir else ""
    _sip_log_dir = sip_log_dir if sip_log_dir else _calls_dir
    _msg_log_dir = msg_log_dir if msg_log_dir else _calls_dir
    _system_id = system_id if system_id else "csp_01"


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
    types = [call_type] if call_type else ['volte', 'ptt']
    result = []
    for ct in types:
        if hour:
            base = os.path.join(_calls_dir, ct, yyyy, mm, dd, hour.zfill(2))
        else:
            base = os.path.join(_calls_dir, ct, yyyy, mm, dd)
        if ct == 'volte':
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


def _read_json(path: str) -> dict:
    """단일 JSON 파일을 dict 로 읽기 (없거나 파싱 실패 시 {})."""
    try:
        with open(path, 'r') as f:
            obj = json.load(f)
        return obj if isinstance(obj, dict) else {}
    except:
        return {}


def _has_recording(d_dir: str) -> bool:
    # 구 형식 (단일 파일)
    for fn in ('recording_mixed.wav', 'recording_mixed.mp4', 'raw_a.rtp', 'raw_audio.rtp'):
        p = os.path.join(d_dir, fn)
        if os.path.exists(p) and os.path.getsize(p) > 0:
            return True
    # 신 형식 (세그먼트): segments.jsonl 또는 seg shard 하위 seg_*_*.rtp 존재
    if os.path.exists(os.path.join(d_dir, 'segments.jsonl')):
        return True
    import glob as _glob
    if _glob.glob(os.path.join(d_dir, 'seg', '*', 'seg_*_*.rtp')) or \
       _glob.glob(os.path.join(d_dir, 'seg_*_*.rtp')):
        return True
    return False


def _has_active_recording(d_dir: str) -> bool:
    """녹취가 진행 중인지 — CMP 가 열린 세그먼트를 *.recording 으로 기록 후 close 시 rename."""
    import glob as _glob
    return bool(
        _glob.glob(os.path.join(d_dir, 'seg', '*', '*.recording')) or
        _glob.glob(os.path.join(d_dir, '*.recording'))
    )


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
        # PTT: raw_audio.rtp (단일 파일)
        ptt_raw = os.path.join(d_dir, 'raw_audio.rtp')
        if os.path.exists(ptt_raw) and os.path.getsize(ptt_raw) > 0:
            amr_ptt = os.path.join(d_dir, 'raw_audio.rtp.amr')
            tmps.append(amr_ptt)
            ok_ptt = _strip_rtp_to_amrwb(ptt_raw, amr_ptt)
            if ok_ptt:
                subprocess.run(['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
                                '-i', amr_ptt, '-ar', '16000', '-ac', '1', '-c:a', 'pcm_s16le', out],
                               capture_output=True, timeout=60)
                for t in tmps:
                    try: os.remove(t)
                    except: pass
                return out if os.path.exists(out) and os.path.getsize(out) > 44 else ''

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


def _resolve_flow_paths(date_str: str, hour: str, service: str) -> list:
    """통합 flow.jsonl 경로 목록 반환 (모든 노드)

    New: {ServiceLogDir}/YYYY/MM/DD/HH/{node_id}.flow.jsonl (csp_01.flow.jsonl, cmp_01.flow.jsonl)
    Legacy: {ServiceLogDir}/YYYY/MM/DD/HH/{system_id}_{service}.flow.jsonl
    """
    if not _sip_log_dir and not _calls_dir:
        return []
    yyyy, mm, dd = _date_parts(date_str)
    hours = [hour.zfill(2)] if hour else [f"{h:02d}" for h in range(24)]
    paths = []
    for hh in hours:
        base_dir = os.path.join(_calls_dir, yyyy, mm, dd, hh) if _calls_dir else ""

        if base_dir:
            # 1) New 통합: {node_id}.flow.jsonl (와일드카드로 모든 노드)
            import glob
            pattern_new = os.path.join(base_dir, "*.flow.jsonl")
            found_new = sorted(glob.glob(pattern_new))
            if found_new:
                paths.extend(found_new)
                continue

            # 2) Legacy: {system_id}_{service}.flow.jsonl
            legacy_path = os.path.join(base_dir, f"{_system_id}_{service}.flow.jsonl")
            if os.path.exists(legacy_path):
                paths.append(legacy_path)
                # CMP legacy도 추가
                cmp_path = os.path.join(base_dir, f"cmp_01_{service}.flow.jsonl")
                if os.path.exists(cmp_path):
                    paths.append(cmp_path)
                continue

        # 3) Legacy fallback: {sip_log_dir}
        if _sip_log_dir:
            base = os.path.join(_sip_log_dir, yyyy, mm, dd, hh)
            for pattern in [f"{service}_flow.jsonl", "flow.jsonl", "sip.jsonl"]:
                p = os.path.join(base, pattern)
                if os.path.exists(p):
                    paths.append(p)
                    break
    return paths


def _resolve_detail_path(date_str: str, hh: str, service: str, proto: str) -> str:
    """서비스별 detail 파일 경로 반환 (fallback 포함) — legacy용"""
    if not _sip_log_dir and not _msg_log_dir:
        return ""
    yyyy, mm, dd = _date_parts(date_str)

    # proto → interface name
    if proto == "SIP":
        iface = "sip"
        legacy_name = "sip.jsonl"
    elif proto == "CSC":
        iface = "csc"
        legacy_name = "csc.jsonl"
    else:
        iface = "cmp"
        legacy_name = "cmp.jsonl"

    # 1) New structure: {MsgLogDir}/YYYY/MM/DD/HH/{system_id}/{system_id}_{iface}.jsonl
    if _msg_log_dir:
        new_base = os.path.join(_msg_log_dir, yyyy, mm, dd, hh, _system_id)
        new_path = os.path.join(new_base, f"{_system_id}_{iface}.jsonl")
        if os.path.exists(new_path):
            return new_path

    if _sip_log_dir:
        base = os.path.join(_sip_log_dir, yyyy, mm, dd, hh)

        # 2) New structure under sip_log_dir: {system_id}/{system_id}_{iface}.jsonl
        new_base2 = os.path.join(base, _system_id)
        new_path2 = os.path.join(new_base2, f"{_system_id}_{iface}.jsonl")
        if os.path.exists(new_path2):
            return new_path2

        # 3) Legacy: {service}_{proto}.jsonl
        svc_name = f"{service}_{iface}.jsonl"
        svc_path = os.path.join(base, svc_name)
        if os.path.exists(svc_path):
            return svc_path

        # 4) Legacy: {proto}.jsonl
        legacy = os.path.join(base, legacy_name)
        if os.path.exists(legacy):
            return legacy

    return ""


def _lookup_body_by_seq(date_str: str, hour: str, seq: int, iface: str = "sip",
                         node: str = "") -> str:
    """인터페이스별 jsonl의 seq번째 줄에서 msg 필드 반환

    node가 지정되면 {node}_*_{iface}.msg.jsonl 로 명확히 선택.
    예: node="cmp", iface="csp" → cmp_01_csp.msg.jsonl
    node 미지정 시 *_{iface}.msg.jsonl 글롭 (레거시 호환).

    New: {MsgLogDir}/YYYY/MM/DD/HH/{node}_{iface}.msg.jsonl
    Legacy: {sip_log_dir}/YYYY/MM/DD/HH/raw.jsonl
    """
    if seq <= 0:
        return ""
    if not _sip_log_dir and not _msg_log_dir:
        return ""
    yyyy, mm, dd = _date_parts(date_str)
    hh = hour.zfill(2)

    # Determine interface file path
    # 1) 통합: {Dir}/YYYY/MM/DD/HH/{node}_{iface}.msg.jsonl
    path = ""
    if _msg_log_dir:
        import glob
        base = os.path.join(_msg_log_dir, yyyy, mm, dd, hh)
        patterns = []
        if node:
            # node 기준 정확한 매칭 (csp/cmp/csc 중 하나)
            patterns.append(f"{node}_*_{iface}.msg.jsonl")
            patterns.append(f"{node}_{iface}.msg.jsonl")
        patterns.extend([f"*_{iface}.msg.jsonl",
                          f"{_system_id}_{iface}.msg.jsonl",
                          f"{_system_id}_{iface}.jsonl"])
        for pattern in patterns:
            matches = sorted(glob.glob(os.path.join(base, pattern)))
            if matches:
                path = matches[0]
                break
        if not path:
            # 레거시: {Dir}/YYYY/MM/DD/HH/{system_id}/{system_id}_{iface}.jsonl
            legacy = os.path.join(base, _system_id, f"{_system_id}_{iface}.jsonl")
            if os.path.exists(legacy):
                path = legacy

    # 2) New structure under sip_log_dir
    if not path and _sip_log_dir:
        new_path2 = os.path.join(_sip_log_dir, yyyy, mm, dd, hh, _system_id,
                                 f"{_system_id}_{iface}.jsonl")
        if os.path.exists(new_path2):
            path = new_path2

    # 3) Legacy: raw.jsonl
    if not path and _sip_log_dir:
        legacy = os.path.join(_sip_log_dir, yyyy, mm, dd, hh, "raw.jsonl")
        if os.path.exists(legacy):
            path = legacy

    if not path:
        return ""
    try:
        with open(path, 'r') as f:
            for i, line in enumerate(f, 1):
                if i == seq:
                    try:
                        return json.loads(line.strip()).get("msg", "")
                    except Exception:
                        return ""
    except Exception as e:
        logger.error("_lookup_body_by_seq: %s", e)
    return ""


def _extract_sesids_from_msg_jsonl(call_ids: list, date_str: str, hour: str = None) -> set:
    """sip msg.jsonl 의 raw SIP 메시지에서 Call-ID 가 매칭되는 라인의 sesid 추출.

    flow.jsonl 의 SIP 라인에는 call_id/subid 가 없으므로 (caller/callee/sesid/method 만),
    raw SIP body 가 들어있는 msg.jsonl 에서 Call-ID 매칭 후 sesid 모음. 이후 sesid 기반
    으로 flow.jsonl 필터링 → VoLTE 호의 다른 PTT 메시지 섞임 방지.
    """
    if not _calls_dir or not call_ids:
        return set()
    sesids: set = set()
    yyyy, mm, dd = _date_parts(date_str)
    hours = [hour.zfill(2)] if hour else [f"{h:02d}" for h in range(24)]
    for hh in hours:
        base = os.path.join(_calls_dir, yyyy, mm, dd, hh)
        if not os.path.isdir(base):
            continue
        # csp_*_sip.msg.jsonl 우선, fallback msg.jsonl
        for pat in ("*_sip.msg.jsonl", "*_sip.jsonl"):
            for path in _glob.glob(os.path.join(base, pat)):
                try:
                    with open(path, 'r') as f:
                        for line in f:
                            if not any(cid in line for cid in call_ids):
                                continue
                            try:
                                obj = json.loads(line)
                            except Exception:
                                continue
                            s = obj.get("sesid", "")
                            if s:
                                sesids.add(s)
                except Exception:
                    pass
    return sesids


def _search_sip_messages(call_ids: list, date_str: str, hour: str = None,
                         service: str = "volte",
                         sesid_set: set = None) -> list:
    """서비스별 flow.jsonl 에서 SIP 메시지 검색 (compact, body 없음).

    flow.jsonl 의 SIP 라인에는 Call-ID 필드가 없고 sesid/caller/callee/method 만 있음.
    `sesid_set` 가 주어지면 그것으로 매칭 (정확). 그렇지 않으면 substring fallback.
    """
    if not _sip_log_dir:
        return []

    results = []
    call_id_set = set(call_ids or [])
    flow_paths = _resolve_flow_paths(date_str, hour, service)

    for jsonl_path in flow_paths:
        try:
            with open(jsonl_path, 'r') as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        obj = json.loads(line)
                    except Exception:
                        continue
                    if obj.get("proto", "SIP") != "SIP":
                        continue
                    # 1차: sesid 정확 매칭 (flow.jsonl 의 sesid 필드)
                    if sesid_set:
                        if obj.get("sesid", "") in sesid_set:
                            results.append(obj)
                            continue
                        # sesid 매칭 안 되면 skip (혼합 방지)
                        continue
                    # 2차 fallback: legacy substring 매칭 (call_id 가 라인에 포함)
                    if not any(cid in line for cid in call_id_set):
                        continue
                    msg_cid = obj.get("subid") or obj.get("call_id", "")
                    if msg_cid in call_id_set:
                        results.append(obj)
        except Exception as e:
            logger.error("_search_sip_messages: %s", e)

    return results


def _search_cmp_messages(call_ids: list, date_str: str, hour: str = None,
                         time_start: str = "", time_end: str = "",
                         call_type: str = "volte",
                         sesid_set: set = None) -> list:
    """서비스별 flow.jsonl에서 CMP(proto=JSON)/CSC(proto=CSC) 메시지 검색.

    필터 정책 — CSC 는 디버깅 데이터 액세스 계층이므로 **세션 식별 (sesid)
    기준으로만 필터**한다. method 블랙리스트나 HEARTBEAT 숨김 같은 표시
    레벨 처리는 console 에서 처리. 호출자는 raw 데이터를 받고 필요 시
    필터링.

    - sesid_set 이 주어지면 그 set 과 매칭되는 메시지만 (가장 정확).
    - 없으면 시간 범위 [time_start, time_end] 로 fallback.
    """
    if not _sip_log_dir:
        return []

    # Map call_type to service for flow file selection
    service = "volte" if call_type.startswith("volte") else "ptt"
    results = []
    flow_paths = _resolve_flow_paths(date_str, hour, service)

    for jsonl_path in flow_paths:
        try:
            with open(jsonl_path, 'r') as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    if '"proto"' not in line:
                        continue
                    if '"JSON"' not in line and '"CSC"' not in line:
                        continue
                    try:
                        obj = json.loads(line)
                    except Exception:
                        continue
                    if obj.get("proto") not in ("JSON", "CSC"):
                        continue

                    # ── 1차: sesid 매칭 (정확) ──
                    if sesid_set:
                        if obj.get("sesid", "") not in sesid_set:
                            continue
                        results.append(obj)
                        continue

                    # ── fallback: 시간 범위 (sesid 미사용 시) ──
                    ts = obj.get("ts", "")
                    if time_start and ts < time_start:
                        continue
                    if time_end and ts > time_end:
                        continue
                    results.append(obj)
        except Exception as e:
            logger.error("_search_cmp_messages: %s", e)

    return results


def _flow_msg_from_log(obj: dict, call_ids: list = None) -> dict:
    """flow.jsonl → FlowMessage 변환. from/to는 로그에 기록된 값 그대로 사용.
    VoIP B2BUA인 경우 call_id로 ue→ue_o/ue_t 구분."""
    proto = obj.get("proto", "SIP")
    method = obj.get("method", "")
    from_actor = obj.get("from", "")
    to_actor = obj.get("to", "")
    # SIP Call-ID는 flow.jsonl에 `subid` 필드로 저장됨 (legacy fallback: call_id)
    msg_cid = obj.get("subid") or obj.get("call_id", "")

    # VoIP B2BUA: SIP ue를 call_id로 ue_o/ue_t 구분
    if proto == "SIP" and call_ids and len(call_ids) >= 2:
        if from_actor == "ue":
            from_actor = "ue_o" if msg_cid == call_ids[0] else "ue_t"
        if to_actor == "ue":
            to_actor = "ue_o" if msg_cid == call_ids[0] else "ue_t"

    result = {
        "ts": obj.get("ts", ""),
        "from": from_actor,
        "to": to_actor,
        "proto": proto,
        "label": method,
    }
    if obj.get("node"):
        result["node"] = obj["node"]
    if obj.get("service"):
        result["service"] = obj["service"]
    if obj.get("mid"):
        result["mid"] = obj["mid"]
    elif obj.get("tx_id"):  # 레거시 호환
        result["mid"] = obj["tx_id"]
    if obj.get("seq"):
        result["seq"] = obj["seq"]
    if obj.get("iface"):
        result["iface"] = obj["iface"]
    if obj.get("detail"):
        result["detail"] = obj["detail"]
    elif obj.get("body"):
        result["detail"] = obj["body"]
    if obj.get("sesid"):
        result["sesid"] = obj["sesid"]
    if obj.get("subid"):
        result["subid"] = obj["subid"]
    return result


def _flow_node_of(m: dict) -> str:
    """FlowMessage 의 표시 노드 결정.
    CSP↔CMP 제어 메시지(proto=JSON, iface=cmp)는 CSP 가 자기 자신을 node 로
    로깅하지만, 의미상 CMP 노드로 묶어 Flow 뷰에서 별도 레인/토글로 노출한다."""
    iface = m.get('iface', '')
    proto = m.get('proto', '')
    if iface == 'cmp' or proto in ('INT', 'RTP', 'MCPTT'):
        return 'cmp'
    node = m.get('node', '')
    if node and node != 'unknown':
        return node
    return 'csp'


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

    # 호의 sesid set 추출 — flow.jsonl 의 SIP 라인에는 Call-ID 없으므로 raw SIP
    # 메시지가 들어있는 msg.jsonl 에서 Call-ID 매칭 라인의 sesid 모음.
    # B2BUA 호는 양 leg 의 sesid 가 달라 둘 다 포함됨 (call_ids 가 2개라).
    sesid_set = _extract_sesids_from_msg_jsonl(call_ids, date_str, hour)

    # SIP 메시지 검색 (sesid 매칭 우선, fallback substring)
    sip_msgs = _search_sip_messages(call_ids, date_str, hour,
                                     service="volte", sesid_set=sesid_set)

    # sesid_set 보강 — flow.jsonl SIP 라인의 sesid 도 추가 (msg.jsonl 누락 대비)
    for m in sip_msgs:
        s = m.get("sesid", "")
        if s: sesid_set.add(s)

    # 시간 범위 (sesid 미사용 시 fallback, 2초 여유)
    time_start = ""
    time_end = ""
    if sip_msgs:
        times = [m.get("ts", "") for m in sip_msgs if m.get("ts")]
        if times:
            time_start = min(times)
            te = max(times)
            try:
                parts = te.split(":")
                secs = float(parts[2]) + 2.0
                if secs >= 60:
                    secs -= 60; parts[1] = f"{int(parts[1])+1:02d}"
                parts[2] = f"{secs:09.6f}"
                time_end = ":".join(parts)
            except:
                time_end = te

    # CMP 메시지 검색: sesid 우선, 없으면 시간 범위 fallback
    ct = call_json.get("call_type", "volte") if call_json else "volte"
    cmp_msgs = _search_cmp_messages(call_ids, date_str, hour, time_start, time_end, ct,
                                     sesid_set=sesid_set if sesid_set else None)

    # FlowMessage 형식으로 변환
    messages = []
    for obj in sip_msgs:
        messages.append(_flow_msg_from_log(obj, call_ids))
    for obj in cmp_msgs:
        messages.append(_flow_msg_from_log(obj, call_ids))

    # CMP flow는 이제 통합 디렉터리의 cmp_01_{service}.flow.jsonl에서 읽힘
    # (_search_cmp_messages → _resolve_flow_paths에서 자동 포함)

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
    # call_type=volte|ptt — VoLTE 와 PTT 의 .d 디렉토리가 같은 prefix 부분 매칭
    # 으로 충돌하는 경우 방지. VolteHistoryPage/PttHistoryPage 는 명시 전달.
    call_type = qs.get("call_type", [None])[0]
    if call_type and call_type not in ("volte", "ptt"):
        call_type = None

    if after == "" or after == "list":
        dirs = _find_all_d_dirs(date_str, hour, call_type)
        call_ids = [os.path.basename(d).replace(".d", "") for d in dirs]
        return HandlerResult(status=200, body=json.dumps({
            "date": date_str, "hour": hour, "call_type": call_type,
            "call_ids": call_ids, "count": len(call_ids),
        }), media_type="application/json")

    # call_id lookup
    call_id = unquote(after)
    d_dir = _find_d_dir_by_callid(date_str, hour, call_id, call_type)
    if not d_dir:
        return HandlerResult(status=404, body=f"'{call_id}' not found for date {date_str}")

    # SIP log 기반 메시지 로딩
    if _sip_log_dir:
        messages = _build_flow_from_sip_log(d_dir, date_str, hour)
    else:
        # fallback: 기존 csp.jsonl 기반
        messages = _load_messages(d_dir)

    # 노드별 배열로 분류
    nodes: dict[str, list] = {}
    for m in messages:
        node = _flow_node_of(m)
        if node not in nodes:
            nodes[node] = []
        nodes[node].append(m)
    for node_msgs in nodes.values():
        node_msgs.sort(key=lambda m: m.get('ts', ''))

    return HandlerResult(status=200, body=json.dumps({
        "call_id": call_id, "date": date_str, "nodes": nodes,
    }), media_type="application/json")


# ── Call Logs API (DB 대체) ──

async def _handle_call_logs(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    if handler_args.method != "GET":
        return HandlerResult(status=405, body="Method Not Allowed")

    # 쿼리 파라미터: handler_args.query_params(dict) 우선, 없으면 full_path 파싱.
    # (이 OAM 라우팅은 query 를 query_params 로 전달 — full_path 만 보면 date 가
    #  항상 now()로 디폴트되어 날짜필터가 깨졌었음.)
    qp = getattr(handler_args, 'query_params', {}) or {}
    qs = parse_qs(urlparse(handler_args.full_path or "").query)
    def _q(name, default=None):
        v = qp.get(name)
        if v:
            return v[0] if isinstance(v, list) else v
        vl = qs.get(name)
        return vl[0] if vl else default

    date_str = _q("date", datetime.now().strftime("%Y-%m-%d"))
    hour = _q("hour")
    call_type = _q("call_type")
    msisdn = _q("msisdn")
    group_id = _q("group_id")
    limit = min(int(_q("limit", "200")), 1000)
    offset = int(_q("offset", "0"))

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
            cj['dir_name'] = os.path.relpath(d, _calls_dir) if _calls_dir else os.path.basename(d).replace('.d', '')
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
                    cj['dir_name'] = os.path.relpath(d_dir, _calls_dir) if _calls_dir else os.path.basename(d_dir).replace('.d', '')
                    logs.append(cj)

    # 필터
    if call_type:
        if call_type == 'volte':
            logs = [l for l in logs if l.get('call_type', '').startswith('volte')]
        else:
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
                cj['dir_name'] = os.path.relpath(d, _calls_dir) if _calls_dir else os.path.basename(d).replace('.d', '')
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

def _derive_session_meta_from_events(d_dir: str) -> dict:
    """session.json이 없을 때 events.jsonl에서 메타데이터 추출"""
    events_path = os.path.join(d_dir, "events.jsonl")
    if not os.path.exists(events_path):
        return {}
    events = _read_jsonl(events_path)
    if not events:
        return {}
    events.sort(key=lambda e: e.get("ts", ""))

    members = set()
    initiator = ""
    start_time = ""
    end_time = ""
    state = "active"

    for ev in events:
        t = ev.get("type", "")
        ts = ev.get("ts", "")
        if not start_time and ts:
            start_time = ts
        if ts:
            end_time = ts
        if t == "session_start":
            if ev.get("initiator"):
                initiator = ev["initiator"]
        elif t == "session_end":
            state = "ended"
        elif t == "member_join":
            m = ev.get("member")
            if m:
                members.add(m)
                if not initiator:
                    initiator = m
        elif t == "member_leave":
            pass

    # 녹취 segment 존재 시 initiator를 speaker로 대체 (session_start 없는 경우)
    if not initiator:
        seg_path = os.path.join(d_dir, "segments.jsonl")
        if os.path.exists(seg_path):
            segs = _read_jsonl(seg_path)
            if segs:
                initiator = segs[0].get("speaker_id", "") or initiator

    # 마지막 이벤트가 member_leave이고 모든 멤버가 이탈했으면 ended 추정
    joined = set()
    for ev in events:
        t = ev.get("type", "")
        m = ev.get("member")
        if t == "member_join" and m:
            joined.add(m)
        elif t == "member_leave" and m:
            joined.discard(m)
    if not joined:
        state = "ended"

    return {
        "start_time": start_time,
        "end_time": end_time if state == "ended" else None,
        "state": state,
        "initiator": initiator,
        "member_count": len(members),
    }


def _ptt_group_base(group_id: str) -> str:
    """PTT 그룹 base 디렉터리 ptt/{gid} (없으면 '+' prefix 보정 시도)"""
    if not _calls_dir:
        return ""
    safe_gid = _sanitize(group_id)
    base = os.path.join(_calls_dir, "ptt", safe_gid)
    if os.path.isdir(base):
        return base
    if group_id and group_id[0:1] == ' ':
        return _ptt_group_base('+' + group_id[1:])
    if group_id and not group_id.startswith('+'):
        alt = os.path.join(_calls_dir, "ptt", "+" + safe_gid)
        if os.path.isdir(alt):
            return alt
    return ""


def _find_ptt_sessions(group_id: str, date: str = None) -> list:
    """PTT 그룹의 시간창(YYYY/MM/DD/HH) 목록 반환. window dir 이름 = 'YYYYMMDDHH'.

    date('YYYY-MM-DD') 지정 시 해당 일자의 시간창만 글롭(장시간 세션의 전체
    버킷 폭주 방지 — 365일 세션이면 일자 미지정 시 최대 8760개 반환됨)."""
    base = _ptt_group_base(group_id)
    if not base:
        return []
    import glob as _glob
    digit4, digit2 = "[0-9][0-9][0-9][0-9]", "[0-9][0-9]"
    # date 필터: 'YYYY-MM-DD' → base/YYYY/MM/DD/* 만 스캔
    day_digits = "".join(c for c in (date or "") if c.isdigit())
    if len(day_digits) >= 8:
        pattern = os.path.join(base, day_digits[0:4], day_digits[4:6], day_digits[6:8], digit2)
    else:
        pattern = os.path.join(base, digit4, digit2, digit2, digit2)
    result = []
    now_window = datetime.now().strftime("%Y%m%d%H")
    for hh_dir in _glob.glob(pattern):
        if not os.path.isdir(hh_dir):
            continue
        rel = os.path.relpath(hh_dir, base).split(os.sep)
        if len(rel) != 4:
            continue
        yyyy, mm, dd, hh = rel
        window = f"{yyyy}{mm}{dd}{hh}"
        # 시간창 segments.jsonl 1회 읽어 세그먼트수·화자수·발화시간·실제 시간범위 집계
        seg_count = 0
        speakers: set = set()
        total_ms = 0
        st_min = ""
        en_max = ""
        segs = _read_jsonl(os.path.join(hh_dir, "segments.jsonl"))
        for s in segs:
            seg_count += 1
            sp = s.get("speaker_id", "")
            if sp:
                speakers.add(sp)
            total_ms += int(s.get("duration_ms", 0) or 0)
            stt = s.get("start_time", "")
            ent = s.get("end_time", "")
            if stt and (not st_min or stt < st_min):
                st_min = stt
            if ent and (not en_max or ent > en_max):
                en_max = ent
        # 진행중 판정: 현재 시각 시간창이고 녹취가 아직 .recording 인 경우
        is_active = (window == now_window) and _has_active_recording(hh_dir)
        result.append({
            "dir": window,
            "session_id": f"{yyyy}-{mm}-{dd} {hh}:00",
            "start_time": st_min or f"{yyyy}-{mm}-{dd}T{hh}:00:00",
            "end_time": (None if is_active else (en_max or f"{yyyy}-{mm}-{dd}T{hh}:59:59")),
            "state": "active" if is_active else "ended",
            "segment_count": seg_count,
            "speaker_count": len(speakers),
            "total_speech_ms": total_ms,
        })
    result.sort(key=lambda x: x["dir"], reverse=True)
    return result


def _ptt_heatmap(group_id: str, days: int = 7) -> list:
    """그룹의 최근 days 일 × 24시간 활동 히트맵 — 시간버킷별 발언수/화자수/발화시간.
       셀 = {date, hour, window(YYYYMMDDHH), segment_count, speaker_count, total_speech_ms}."""
    base = _ptt_group_base(group_id)
    if not base:
        return []
    cells = []
    today = datetime.now().date()
    for di in range(days):
        d = today - timedelta(days=di)
        ymd = d.strftime("%Y%m%d")
        for hh in range(24):
            hh_dir = os.path.join(base, ymd[0:4], ymd[4:6], ymd[6:8], f"{hh:02d}")
            if not os.path.isdir(hh_dir):
                continue
            segs = _read_jsonl(os.path.join(hh_dir, "segments.jsonl"))
            speakers = {s.get("speaker_id") for s in segs if s.get("speaker_id")}
            total_ms = sum(int(s.get("duration_ms", 0) or 0) for s in segs)
            cells.append({
                "date": d.strftime("%Y-%m-%d"), "hour": hh,
                "window": f"{ymd}{hh:02d}",
                "segment_count": len(segs), "speaker_count": len(speakers),
                "total_speech_ms": total_ms,
            })
    return cells


def _ptt_group_summaries() -> dict:
    """모든 PTT 그룹의 경량 요약(세션수·최근 시간창)을 그룹키별로 반환.
    디렉터리 글롭만 수행(파일 미독) → 그룹 다수에도 저렴. 키 = ptt/{groupKey}."""
    if not _calls_dir:
        return {}
    import glob as _glob
    ptt_root = os.path.join(_calls_dir, "ptt")
    if not os.path.isdir(ptt_root):
        return {}
    d4, d2 = "[0-9][0-9][0-9][0-9]", "[0-9][0-9]"
    summaries: dict = {}
    for hh_dir in _glob.glob(os.path.join(ptt_root, "*", d4, d2, d2, d2)):
        rel = os.path.relpath(hh_dir, ptt_root).split(os.sep)
        if len(rel) != 5:
            continue
        gid, yyyy, mm, dd, hh = rel
        window = f"{yyyy}{mm}{dd}{hh}"
        s = summaries.get(gid)
        if s is None:
            summaries[gid] = {"session_count": 1, "last_window": window}
        else:
            s["session_count"] += 1
            if window > s["last_window"]:
                s["last_window"] = window
    return summaries


def _find_ptt_session_dir(group_id: str, session_dir: str) -> str:
    """시간창 식별자 'YYYYMMDDHH' → ptt/{gid}/{YYYY}/{MM}/{DD}/{HH} 경로"""
    base = _ptt_group_base(group_id)
    if not base:
        return ""
    w = "".join(c for c in (session_dir or "") if c.isdigit())
    if len(w) >= 10:
        d = os.path.join(base, w[:4], w[4:6], w[6:8], w[8:10])
        if os.path.isdir(d):
            return d
    return ""


def _load_ptt_events(d_dir: str, date: str = None) -> list:
    """PTT 시간창 이벤트 로드 (events.jsonl — 멤버 join/leave 등). floor 는 별도 endpoint."""
    events = []
    events_path = os.path.join(d_dir, "events.jsonl")
    if os.path.exists(events_path):
        events.extend(_read_jsonl(events_path))

    events.sort(key=lambda e: e.get("ts", ""))
    return events


def _sip_raw_to_flow(obj: dict) -> dict:
    """raw sip.jsonl 항목({ts, dir, peer, proto, msg})을 flow 형식으로 변환"""
    import re
    msg = obj.get("msg", "")
    direction = obj.get("dir", "")  # RX or TX
    ts = obj.get("ts", "")
    peer = obj.get("peer", "")

    # SIP 메서드/상태 추출
    first_line = msg.split("\r\n")[0] if msg else ""
    method = ""
    if first_line.startswith("SIP/"):
        # Response: SIP/2.0 200 OK
        parts = first_line.split(" ", 2)
        method = parts[1] + " " + (parts[2] if len(parts) > 2 else "") if len(parts) >= 2 else first_line
    else:
        # Request: INVITE sip:... SIP/2.0
        method = first_line.split(" ")[0] if first_line else ""

    # Call-ID 추출
    call_id = ""
    m = re.search(r'Call-ID:\s*(.+)', msg, re.IGNORECASE)
    if m:
        call_id = m.group(1).strip()

    # From/To URI 추출
    from_uri = ""
    to_uri = ""
    m = re.search(r'From:\s*<?([^>;]+)', msg, re.IGNORECASE)
    if m:
        from_uri = m.group(1).strip()
    m = re.search(r'To:\s*<?([^>;]+)', msg, re.IGNORECASE)
    if m:
        to_uri = m.group(1).strip()

    # from/to actor
    if direction == "RX":
        from_actor, to_actor = "ue", "csp"
    else:
        from_actor, to_actor = "csp", "ue"

    return {
        "ts": ts,
        "from": from_actor,
        "to": to_actor,
        "proto": "SIP",
        "method": method,
        "call_id": call_id,
        "from_uri": from_uri,
        "to_uri": to_uri,
        "peer": peer,
    }


def _search_sip_for_group(group_id: str, date_str: str) -> list:
    """ptt_flow.jsonl + sip.jsonl에서 group_id 관련 메시지 검색.
    SIP: from_uri/to_uri 또는 msg 본문에 group_id 포함. CMP/CSC: SIP 시간 범위 내 전부 포함."""
    if not group_id:
        return []

    # 1차: ptt_flow.jsonl (SIP + CMP 혼합)
    sip_results = []
    all_non_sip = []
    flow_paths = _resolve_flow_paths(date_str, None, "ptt")

    for jsonl_path in flow_paths:
        try:
            with open(jsonl_path, 'r') as f:
                for line in f:
                    line = line.strip()
                    if not line: continue
                    try:
                        obj = json.loads(line)
                    except: continue

                    # 통합 파일: service 필드로 필터 (없으면 레거시 — 전부 포함)
                    svc = obj.get("service", "")
                    if svc and svc != "ptt":
                        continue

                    proto = obj.get("proto", "SIP")

                    if proto == "SIP":
                        from_uri = obj.get("from_uri", "")
                        to_uri = obj.get("to_uri", "")
                        if group_id in from_uri or group_id in to_uri:
                            sip_results.append(obj)
                    else:
                        all_non_sip.append(obj)
        except Exception as e:
            logger.error("_search_sip_for_group flow: %s", e)

    # 2차: msg_log의 sip.jsonl에서도 group_id 검색 (ptt_flow.jsonl에 SIP가 누락된 경우)
    if not sip_results:
        yyyy, mm, dd = _date_parts(date_str)
        sip_detail_paths = []
        for hh in [f"{h:02d}" for h in range(24)]:
            # New: {Dir}/YYYY/MM/DD/HH/{system_id}_sip.msg.jsonl (통합)
            if _msg_log_dir:
                p = os.path.join(_msg_log_dir, yyyy, mm, dd, hh,
                                 f"{_system_id}_sip.msg.jsonl")
                if os.path.exists(p):
                    sip_detail_paths.append(p)
                else:
                    # 레거시: {Dir}/YYYY/MM/DD/HH/{system_id}/{system_id}_sip.jsonl
                    p2 = os.path.join(_msg_log_dir, yyyy, mm, dd, hh, _system_id,
                                      f"{_system_id}_sip.jsonl")
                    if os.path.exists(p2):
                        sip_detail_paths.append(p2)
        for jsonl_path in sip_detail_paths:
            try:
                with open(jsonl_path, 'r') as f:
                    for line in f:
                        if group_id not in line:
                            continue
                        line = line.strip()
                        if not line: continue
                        try:
                            obj = json.loads(line)
                        except: continue
                        msg_body = obj.get("msg", "")
                        if group_id in msg_body:
                            # raw SIP 메시지에서 flow 형식으로 변환
                            sip_results.append(_sip_raw_to_flow(obj))
            except Exception as e:
                logger.error("_search_sip_for_group sip detail: %s", e)

    # 2차: CMP/CSC 메시지를 SIP 시간 범위 내로 필터
    results = list(sip_results)
    if sip_results and all_non_sip:
        sip_times = [m.get("ts", "") for m in sip_results if m.get("ts")]
        if sip_times:
            t_start = min(sip_times)
            t_end = max(sip_times)
            # 2초 여유
            try:
                parts = t_end.split(":")
                secs = float(parts[2]) + 2.0
                if secs >= 60: secs -= 60; parts[1] = f"{int(parts[1])+1:02d}"
                parts[2] = f"{secs:09.6f}"
                t_end = ":".join(parts)
            except: pass
            for obj in all_non_sip:
                ts = obj.get("ts", "")
                if t_start <= ts <= t_end:
                    results.append(obj)

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
    qp = getattr(handler_args, 'query_params', {}) or {}
    qs = parse_qs(urlparse(full_path).query)
    def _qp(name, default=None):
        v = qp.get(name)
        if v: return v
        vl = qs.get(name)
        return vl[0] if vl else default

    after = full_path.split("?")[0]
    after = after[len("/api/v1/ptt/history"):].lstrip("/")

    if not after:
        # ── 그룹별 요약(좌측 패널용): 세션수·최근 시간창 ──
        if _qp("summary"):
            return HandlerResult(status=200, body=json.dumps({
                "summaries": _ptt_group_summaries(),
            }), media_type="application/json")

        # ── 세션 목록 ──
        group_id = _qp("group_id")
        if not group_id:
            return HandlerResult(status=400, body=json.dumps({"error": "group_id required"}),
                                 media_type="application/json")

        # 시간창(YYYY/MM/DD/HH) 목록 스캔 (date 지정 시 해당 일자만)
        sessions = _find_ptt_sessions(group_id, _qp("date"))

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

    if session_dir == "heatmap":
        # ── 그룹 시간대 히트맵 (최근 days 일 × 24h) ──
        try:
            days = max(1, min(int(_qp("days", "7") or 7), 31))
        except (TypeError, ValueError):
            days = 7
        return HandlerResult(status=200, body=json.dumps({
            "group_id": group_id, "days": days, "cells": _ptt_heatmap(group_id, days),
        }), media_type="application/json")

    if len(parts) >= 3 and parts[2] == "audio":
        # ── PTT 세션 녹취 오디오 ──
        d_dir = _find_ptt_session_dir(group_id, session_dir)
        if not d_dir:
            return HandlerResult(status=404, body=json.dumps({"error": "Session not found"}))

        mixed_wav = os.path.join(d_dir, 'recording_mixed.wav')
        if not (os.path.exists(mixed_wav) and os.path.getsize(mixed_wav) > 44):
            mixed_wav = _transcode_audio(d_dir)
        if mixed_wav and os.path.exists(mixed_wav) and os.path.getsize(mixed_wav) > 44:
            return HandlerResult(status=200, body=mixed_wav, headers={
                'Content-Type': 'audio/wav', 'X-File-Path': mixed_wav
            })
        return HandlerResult(status=404, body=json.dumps({"error": "No recording available"}))

    if len(parts) >= 3 and parts[2] == "flow":
        # ── Flow: 모든 노드의 flow.jsonl 읽기 → 필터 → 노드별 배열 ──
        date_str = _qp("date", datetime.now().strftime("%Y-%m-%d"))
        flow_paths = _resolve_flow_paths(date_str, None, "ptt")

        # 세션의 시간 범위: events.jsonl에서 추출 (ISO 형식 → HH:MM:SS.ffffff)
        def _iso_to_hms(iso: str) -> str:
            if not iso: return ""
            if "T" in iso:
                return iso.split("T", 1)[1][:15]
            return iso[:15]

        d_dir = _find_ptt_session_dir(group_id, session_dir)
        session_meta = _load_session_json(d_dir) if d_dir else {}
        events = _load_ptt_events(d_dir) if d_dir else []
        ev_times = [e.get("ts", "") for e in events if e.get("ts")]
        ses_t_start = _iso_to_hms(min(ev_times)) if ev_times else ""
        ses_t_end = _iso_to_hms(max(ev_times)) if ev_times else ""
        # 여유 버퍼: 시작 1초 전 ~ 종료 2초 후
        if ses_t_start:
            try:
                parts_s = ses_t_start.split(":")
                secs = float(parts_s[2]) - 1.0
                if secs < 0: secs = 0
                parts_s[2] = f"{secs:09.6f}"
                ses_t_start = ":".join(parts_s)
            except: pass
        if ses_t_end:
            try:
                parts_e = ses_t_end.split(":")
                secs = float(parts_e[2]) + 2.0
                if secs >= 60:
                    secs -= 60
                    parts_e[1] = f"{int(parts_e[1])+1:02d}"
                parts_e[2] = f"{secs:09.6f}"
                ses_t_end = ":".join(parts_e)
            except: pass

        # 1) flow.jsonl 전체를 1회 로드 (mcptt/ptt 서비스). 시간 필터·method
        #    필터는 적용하지 않는다 — CSC 는 raw 데이터를 반환하고 필터링은
        #    호출자(console)가 결정.
        all_ptt_msgs = []
        for jsonl_path in flow_paths:
            try:
                with open(jsonl_path, 'r') as f:
                    for line in f:
                        line = line.strip()
                        if not line: continue
                        try: obj = json.loads(line)
                        except: continue
                        svc = obj.get("service", "")
                        if svc not in ("mcptt", "ptt", ""): continue
                        all_ptt_msgs.append(obj)
            except Exception as e:
                logger.error("flow read error: %s", e)

        # 2) 세션 식별 (sesid_set) 추출:
        #    세션 시간 범위 [ses_t_start, ses_t_end] 내에서 group_id 와 일치하는
        #    메시지의 sesid 를 모은다. PTT 의 sesid 는 그룹 세션 단위로 발급 →
        #    JOIN/LEAVE/ADD/REMOVE 모두 같은 sesid 를 공유.
        sesid_set: set = set()
        subid_set: set = set()
        if group_id:
            for obj in all_ptt_msgs:
                ts = obj.get("ts", "")
                if ses_t_start and ts and ts < ses_t_start: continue
                if ses_t_end and ts and ts > ses_t_end: continue
                detail = obj.get("detail", "") or ""
                sesid_val = obj.get("sesid", "") or ""
                subid_val = obj.get("subid", "") or ""
                if group_id in detail or group_id in sesid_val or group_id in subid_val:
                    if sesid_val: sesid_set.add(sesid_val)
                    if subid_val and obj.get("proto") == "SIP":
                        subid_set.add(subid_val)

        # 3) sesid 매칭으로 전체 메시지 필터 — 시간 범위 무관.
        #    같은 그룹 세션의 startup-time ADD_PTT_GROUP, 종료 후 REMOVE 등도
        #    자연스럽게 포함됨. SIP 응답은 subid (Call-ID) 기준으로 묶음.
        all_matched = []
        for obj in all_ptt_msgs:
            sesid_val = obj.get("sesid", "") or ""
            subid_val = obj.get("subid", "") or ""
            if sesid_set and sesid_val in sesid_set:
                all_matched.append(obj)
                continue
            if subid_set and subid_val in subid_set and obj.get("proto") == "SIP":
                all_matched.append(obj)
                continue

        # 4) sesid 매칭으로 아무것도 안 잡히면 fallback: 기존 substring 매칭
        #    (legacy 로그 — sesid 누락 케이스 대비).
        if not all_matched and group_id:
            for obj in all_ptt_msgs:
                ts = obj.get("ts", "")
                if ses_t_start and ts and ts < ses_t_start: continue
                if ses_t_end and ts and ts > ses_t_end: continue
                detail = obj.get("detail", "") or ""
                sesid_val = obj.get("sesid", "") or ""
                subid_val = obj.get("subid", "") or ""
                if group_id in detail or group_id in sesid_val or group_id in subid_val:
                    all_matched.append(obj)

        filtered = all_matched

        # B2BUA call_ids (ue_o/ue_t 구분용)
        call_ids = session_meta.get("call_ids", []) if session_meta else []

        # FlowMessage 변환 → 노드별 분류
        nodes: dict[str, list] = {}
        for obj in filtered:
            msg = _flow_msg_from_log(obj, call_ids)
            node = _flow_node_of(msg)
            if node not in nodes:
                nodes[node] = []
            nodes[node].append(msg)

        for node_msgs in nodes.values():
            node_msgs.sort(key=lambda m: m.get("ts", ""))

        return HandlerResult(status=200, body=json.dumps({
            "call_id": group_id, "date": date_str, "nodes": nodes,
        }), media_type="application/json")

    elif len(parts) >= 3 and parts[2] == "floor":
        # ── 세션 로컬 floor 타임라인 (CMP 가 .d/floor.jsonl 에 기록) ──
        d_dir = _find_ptt_session_dir(group_id, session_dir)
        if not d_dir:
            return HandlerResult(status=404, body=json.dumps({"error": "Session not found"}),
                                 media_type="application/json")
        floor_path = os.path.join(d_dir, "floor.jsonl")
        floor = _read_jsonl(floor_path) if os.path.exists(floor_path) else []
        return HandlerResult(status=200, body=json.dumps({"floor": floor}),
                             media_type="application/json")

    else:
        # ── 시간창 이벤트 ──
        date = _qp("date")

        d_dir = _find_ptt_session_dir(group_id, session_dir)
        if not d_dir:
            return HandlerResult(status=404, body=json.dumps({"error": "Session not found"}),
                                 media_type="application/json")

        # 그룹 스냅샷은 base/group.json 에서
        session_meta = {"session_id": session_dir}
        base = _ptt_group_base(group_id)
        if base:
            gj = _read_json(os.path.join(base, "group.json"))
            if gj:
                session_meta = gj
                session_meta["session_id"] = session_dir
        group_snapshot = session_meta.get("group_snapshot", {}) if session_meta else {}
        events = _load_ptt_events(d_dir, date)

        # participants 정보도 포함 (없으면 events에서 유도)
        participants = _load_participants(d_dir)
        if not participants and events:
            member_times = {}  # msisdn → {join, leave}
            for ev in events:
                m = ev.get("member")
                if not m:
                    continue
                if m not in member_times:
                    member_times[m] = {"msisdn": m, "role": "member",
                                        "join_time": None, "leave_time": None}
                ts = ev.get("ts")
                t = ev.get("type", "")
                if t == "member_join":
                    if not member_times[m]["join_time"]:
                        member_times[m]["join_time"] = ts
                elif t == "member_leave":
                    member_times[m]["leave_time"] = ts
            participants = list(member_times.values())

        has_rec = _has_recording(d_dir) if d_dir else False
        return HandlerResult(status=200, body=json.dumps({
            "session": session_meta or {},
            "group_snapshot": group_snapshot,
            "events": events,
            "participants": participants,
            "has_recording": has_rec,
        }), media_type="application/json")


# ── Message Body Lookup ──

def _lookup_body_from_detail(date_str: str, hour: str, ts: str, direction: str, proto: str,
                             service: str = "") -> str:
    """Legacy: per-protocol detail 파일에서 ts+dir로 body 조회 (구 로그 호환)"""
    if not _sip_log_dir or not ts:
        return ""

    yyyy, mm, dd = _date_parts(date_str)

    if hour:
        hours = [hour.zfill(2)]
    else:
        hh_from_ts = ts[:2] if len(ts) >= 2 else ""
        hours = [hh_from_ts] if hh_from_ts else [f"{h:02d}" for h in range(24)]

    services = [service] if service else ["volte", "ptt", "system"]

    for hh in hours:
        for svc in services:
            detail_path = _resolve_detail_path(date_str, hh, svc, proto)
            if not detail_path:
                continue
            try:
                with open(detail_path, 'r') as f:
                    for line in f:
                        line = line.strip()
                        if not line:
                            continue
                        if ts not in line:
                            continue
                        try:
                            obj = json.loads(line)
                        except Exception:
                            continue
                        if obj.get("ts") == ts and obj.get("dir") == direction:
                            return obj.get("msg", "")
            except Exception as e:
                logger.error("_lookup_body_from_detail: %s", e)

    return ""


async def _handle_flow_body(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """메시지 body 조회 API

    GET /api/v1/flow/body?date=YYYY-MM-DD&hour=HH&seq=42
    Fallback (legacy): ?date=...&hour=HH&ts=...&dir=TX&proto=SIP
    Returns: {"body": "full message text"}
    """
    if handler_args.method != "GET":
        return HandlerResult(status=405, body="Method Not Allowed")

    qp = getattr(handler_args, 'query_params', {}) or {}
    qs = parse_qs(urlparse(handler_args.full_path).query)
    def _qval(name, default=None):
        v = qp.get(name)
        if v: return v
        vl = qs.get(name)
        return vl[0] if vl else default

    date_str = _qval("date", datetime.now().strftime("%Y-%m-%d"))
    hour = _qval("hour")
    seq_str = _qval("seq", "")
    iface = _qval("iface", "sip")
    node = _qval("node", "")  # 여러 노드 msg 파일 존재 시 정확한 선택

    # New: seq-based lookup ({node}_*_{iface}.msg.jsonl)
    if seq_str and hour:
        try:
            seq = int(seq_str)
        except ValueError:
            seq = 0
        if seq > 0:
            body = _lookup_body_by_seq(date_str, hour, seq, iface=iface, node=node)
            return HandlerResult(status=200, body=json.dumps({"body": body}),
                                 media_type="application/json")

    # Legacy fallback: ts+dir based lookup
    ts = _qval("ts", "")
    direction = _qval("dir", "")
    proto = _qval("proto", "SIP")
    service = _qval("service", "")

    if not ts or not direction:
        return HandlerResult(status=400, body=json.dumps({"error": "seq+hour or ts+dir required"}),
                             media_type="application/json")

    body = _lookup_body_from_detail(date_str, hour, ts, direction, proto, service=service)

    return HandlerResult(status=200, body=json.dumps({"body": body}),
                         media_type="application/json")


FLOW_HANDLER_LIST = [
    ("/api/v1/flow/body", _handle_flow_body, {}),
    ("/api/v1/flow", _handle_flow, {}),
    ("/api/v1/call/logs", _handle_call_logs, {}),
    ("/api/v1/recordings", _handle_recordings, {}),
    ("/api/v1/ptt/history", _handle_ptt_history, {}),
]
