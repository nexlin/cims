"""
CIMS Recording REST API  (파일시스템 기반 — DB 미사용)

녹취 메타/파일 구조:
  VoIP: {ServiceLogDir}/voip/YYYY/MM/DD/HH/{prefix}/{caller}/{session}.d/
        call.json, segments.jsonl, seg_NNNN.json, seg_NNNN_a.rtp, seg_NNNN_b.rtp
  PTT:  {ServiceLogDir}/ptt/{groupId}/sessions/{key}.d/
        session.json, events.jsonl, recordings/ (segments.jsonl, seg_NNNN.json, seg_NNNN_audio.rtp)

변환 상태 판별 (파일 존재 기반):
  .rtp.recording  → recording (녹취 중)
  .rtp            → raw (변환 전)
  .transcoding    → transcoding (변환 마커)
  .wav / .mp4     → ready (변환 완료)

트랜스코딩:
  - CMP는 raw RTP 파일만 저장
  - 클라이언트가 재생 요청 시 CSC가 on-demand 변환 (ffmpeg)
  - 변환된 파일은 캐싱하여 재요청 시 즉시 응답
"""

import os
import json
import glob
import struct
import shutil
import subprocess
import threading
import time
import logging
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import urlparse, unquote
from pathlib import PurePath

from httpsrv.handler import HandlerArgs, HandlerResult

logger = logging.getLogger(__name__)

# 변환 중인 작업 추적 (중복 변환 방지)
_transcoding_locks = {}
_transcoding_mutex = threading.Lock()

# 변환 워커 풀 (bounded). 재생 요청(_ensure_segment_ready)은 작업만 큐잉하고 즉시 202 반환,
# 실제 ffmpeg 는 이 풀의 워커에서 실행 — 요청 처리와 분리 + 동시 변환 수 제한(CPU 보호).
# 요청마다 무제한 스레드를 띄우던 기존 방식의 CPU 폭주를 막는다. init()에서 생성.
_transcode_executor = None
_transcode_workers = 2

# ServiceLogDir — init()에서 설정
_service_log_dir = ''

# 변환툴(ffmpeg) 경로 — init()에서 결정.
# raw RTP → mp4/wav 변환에 필요. air-gapped(private) 환경에서는 시스템 PATH 에
# 없을 수 있으므로 OAM 패키지에 번들된 vendor 바이너리를 우선 사용한다.
# (OAM 패키지화 시 oam/vendor/bin/ffmpeg 또는 oam/vendor/ffmpeg 로 동봉할 것.)
_FFMPEG = 'ffmpeg'


def _resolve_ffmpeg(ffmpeg_bin: str = '') -> str:
    # 1) 명시 인자 (oam_app 이 번들 경로 주입)
    if ffmpeg_bin and os.path.exists(ffmpeg_bin):
        return ffmpeg_bin
    # 2) 환경변수 (agent/패키지가 번들 바이너리 경로 주입)
    env = os.environ.get('CIMS_FFMPEG')
    if env and os.path.exists(env):
        return env
    # 3) 시스템 PATH
    found = shutil.which('ffmpeg')
    if found:
        return found
    # 4) fallback — 호출 시 명확한 에러(없음)로 드러남
    return 'ffmpeg'


def init(service_log_dir: str = '', ffmpeg_bin: str = '', transcode_workers: int = 2):
    global _service_log_dir, _FFMPEG, _transcode_executor, _transcode_workers
    _service_log_dir = service_log_dir
    _FFMPEG = _resolve_ffmpeg(ffmpeg_bin)
    if _FFMPEG == 'ffmpeg' and not shutil.which('ffmpeg'):
        logger.warning("ffmpeg 변환툴을 찾지 못함 — 녹취 재생(raw RTP→mp4 변환) 불가. "
                       "OAM 패키지에 vendor ffmpeg 동봉 또는 시스템 설치 필요.")
    try:
        _transcode_workers = max(1, int(transcode_workers or 2))
    except (TypeError, ValueError):
        _transcode_workers = 2
    if _transcode_executor is None:
        _transcode_executor = ThreadPoolExecutor(max_workers=_transcode_workers,
                                                 thread_name_prefix='rec-transcode')
        logger.info("녹취 변환 워커 풀 시작 (max_workers=%d)", _transcode_workers)


# ══════════════════════════════════════════════════════════════
#  RTP → codec 추출 유틸
# ══════════════════════════════════════════════════════════════

# AMR-WB FT별 frame 크기 (octet-aligned, bytes) — FT 0~8=speech, 9=SID
_AMRWB_FS = [17, 23, 32, 36, 40, 46, 50, 58, 60, 5, 0, 0, 0, 0, 0, 0]


def _detect_audio_pt(raw_rtp_path: str):
    """raw RTP 파일에서 오디오 PT 자동 감지 — payload 6바이트 이상 패킷의 최빈 PT.
    협상 PT는 leg마다 다르다(UE 동적 96 등, cspsim 99) — 고정값을 가정하지 않는다.
    telephone-event(payload 4바이트)·헤더-only 패킷은 표본에서 배제."""
    counts = {}
    try:
        with open(raw_rtp_path, 'rb') as fin:
            while True:
                hdr = fin.read(12)  # 4(len) + 8(usec)
                if len(hdr) < 12:
                    break
                pkt_len = struct.unpack('<I', hdr[:4])[0]
                pkt = fin.read(pkt_len)
                if len(pkt) < pkt_len or pkt_len < 12:
                    continue
                cc = pkt[0] & 0x0F
                has_ext = (pkt[0] >> 4) & 0x01
                hdr_len = 12 + cc * 4
                if has_ext and hdr_len + 4 <= pkt_len:
                    ext_len = struct.unpack_from('>H', pkt, hdr_len + 2)[0]
                    hdr_len += 4 + ext_len * 4
                if pkt_len - hdr_len < 6:
                    continue
                pt = pkt[1] & 0x7F
                counts[pt] = counts.get(pt, 0) + 1
    except Exception as e:
        logger.error("_detect_audio_pt error: %s", e)
        return None
    if not counts:
        return None
    return max(counts, key=counts.get)


def _video_has_media(path: str, size_threshold: int = 4096) -> bool:
    """영상 raw 파일에 실제 미디어(payload 있는 RTP)가 있는가.
    음성 통화에서도 UE 가 영상 포트로 헤더-only keepalive(레코드당 24B)를 보내
    24B 짜리 va/vb 파일이 생긴다 — 이를 영상 있음으로 오판하면 콘솔이 음성 호를
    검은 영상 플레이어로 표시한다. 크면(임계 이상) 실영상, 작으면 payload 스캔."""
    try:
        sz = os.path.getsize(path)
    except OSError:
        return False
    if sz >= size_threshold:
        return True
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError:
        return False
    off = 0
    while off + 12 <= len(data):
        ln = struct.unpack('<I', data[off:off + 4])[0]
        off += 12
        pkt = data[off:off + ln]
        off += ln
        if len(pkt) > 12 and len(pkt) > 12 + (pkt[0] & 0x0F) * 4:
            return True
    return False


def _strip_rtp_to_amrwb(raw_rtp_path: str, out_amr_path: str, audio_pt=None) -> bool:
    """raw RTP 파일에서 AMR-WB payload 추출 (octet-aligned).
    raw 형식: [uint32 len][int64 recv_usec][RTP pkt] 반복
    오디오 PT: 세그먼트 메타(audio_pt — CMP 가 CSP 협상값 기록)를 우선 사용, 메타 없는
    구 녹취는 파일에서 자동 감지(_detect_audio_pt) fallback.
    패킷당 다중 프레임(ToC F-bit 체인, ptime 40ms=2프레임 등) 지원.
    """
    if not audio_pt or audio_pt <= 0:
        audio_pt = _detect_audio_pt(raw_rtp_path)
    if audio_pt is None:
        return False
    try:
        with open(raw_rtp_path, 'rb') as fin, open(out_amr_path, 'wb') as fout:
            fout.write(b'#!AMR-WB\n')
            while True:
                hdr = fin.read(12)  # 4(len) + 8(usec)
                if len(hdr) < 12:
                    break
                pkt_len = struct.unpack('<I', hdr[:4])[0]
                pkt = fin.read(pkt_len)
                if len(pkt) < pkt_len or pkt_len < 12:
                    continue
                pt = pkt[1] & 0x7F
                if pt != audio_pt:
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
                if len(payload) < 2:
                    continue
                # octet-aligned: CMR(1) + ToC 체인(F-bit 연쇄) + 프레임 데이터들
                i = 1
                tocs = []
                while i < len(payload):
                    t = payload[i]
                    i += 1
                    tocs.append(t)
                    if not (t & 0x80):
                        break
                for t in tocs:
                    ft = (t >> 3) & 0x0F
                    n = _AMRWB_FS[ft]
                    if n == 0 or i + n > len(payload):
                        break
                    # storage ToC = F 비트 제거 (FT·Q 유지)
                    fout.write(bytes([t & 0x7F]))
                    fout.write(payload[i:i + n])
                    i += n
        return os.path.exists(out_amr_path) and os.path.getsize(out_amr_path) > 9
    except Exception as e:
        logger.error("_strip_rtp_to_amrwb error: %s", e)
        return False


def _strip_rtp_to_h264(raw_rtp_path: str, out_h264_path: str) -> bool:
    """raw RTP 파일에서 H.264 Annex-B NAL 스트림 추출.
    세그먼트 경계에서 SPS/PPS가 파일 초반에 없을 수 있으므로,
    전체 RTP에서 SPS/PPS를 찾아 출력 시작에 보장한다.
    """
    START_CODE = b'\x00\x00\x00\x01'

    def _parse_rtp_nals(fin):
        """RTP 패킷에서 NAL unit 추출 (generator)"""
        fu_buffer = bytearray()
        while True:
            hdr = fin.read(12)
            if len(hdr) < 12:
                break
            pkt_len = struct.unpack('<I', hdr[:4])[0]
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
            if 1 <= nal_type <= 23:
                yield bytes(payload)
            elif nal_type == 28:
                if len(payload) < 2:
                    continue
                fu_indicator = payload[0]
                fu_header = payload[1]
                start_bit = (fu_header >> 7) & 1
                end_bit = (fu_header >> 6) & 1
                nal_t = fu_header & 0x1F
                if start_bit:
                    fu_nal_hdr = (fu_indicator & 0xE0) | nal_t
                    fu_buffer = bytearray([fu_nal_hdr])
                    fu_buffer.extend(payload[2:])
                else:
                    fu_buffer.extend(payload[2:])
                if end_bit and fu_buffer:
                    yield bytes(fu_buffer)
                    fu_buffer = bytearray()
            elif nal_type == 24:
                off = 1
                while off + 2 <= len(payload):
                    nal_sz = struct.unpack_from('>H', payload, off)[0]
                    off += 2
                    if off + nal_sz <= len(payload):
                        yield bytes(payload[off:off + nal_sz])
                    off += nal_sz

    try:
        # 1패스: SPS/PPS 수집 + NAL 목록
        sps = None
        pps = None
        nals = []
        with open(raw_rtp_path, 'rb') as fin:
            for nal in _parse_rtp_nals(fin):
                nt = nal[0] & 0x1F
                if nt == 7:
                    sps = nal
                elif nt == 8:
                    pps = nal
                nals.append(nal)

        # 출력: SPS+PPS를 첫 프레임 전에 보장
        with open(out_h264_path, 'wb') as fout:
            sps_written = False
            for nal in nals:
                nt = nal[0] & 0x1F
                if not sps_written and nt not in (7, 8):
                    # 첫 프레임 데이터 전에 SPS+PPS 삽입
                    if sps:
                        fout.write(START_CODE + sps)
                    if pps:
                        fout.write(START_CODE + pps)
                    sps_written = True
                fout.write(START_CODE + nal)

        return os.path.exists(out_h264_path) and os.path.getsize(out_h264_path) > 4
    except Exception as e:
        logger.error("_strip_rtp_to_h264 error: %s", e)
        return False


# ══════════════════════════════════════════════════════════════
#  파일시스템 탐색 — 녹취 세션 디렉터리 검색
# ══════════════════════════════════════════════════════════════

def _read_json(path: str):
    """JSON 파일 읽기. 실패 시 None."""
    try:
        with open(path, 'r') as f:
            return json.loads(f.read())
    except Exception:
        return None


def _read_jsonl(path: str) -> list:
    """JSONL 파일 읽기. 한 줄씩 JSON 파싱."""
    result = []
    try:
        with open(path, 'r') as f:
            for line in f:
                line = line.strip()
                if line:
                    result.append(json.loads(line))
    except Exception:
        pass
    return result


def _scan_voip_sessions(base: str, caller: str = '', from_dt: str = '', to_dt: str = '',
                        limit: int = 200, offset: int = 0):
    """VoIP 세션 디렉터리를 스캔하여 녹취 목록 반환.
    디렉터리: {base}/voip/YYYY/MM/DD/HH/{prefix}/{caller}/{session}.d/
    """
    voip_root = os.path.join(base, 'volte')
    if not os.path.isdir(voip_root):
        return [], 0

    # .d 디렉터리를 역순(최신 먼저) 수집
    dirs = sorted(glob.glob(os.path.join(voip_root, '**', '*.d'), recursive=True), reverse=True)

    results = []
    for d in dirs:
        call_json = os.path.join(d, 'call.json')
        if not os.path.exists(call_json):
            continue
        meta = _read_json(call_json)
        if not meta:
            continue

        # 필터
        if caller:
            if caller not in (meta.get('initiator', ''), meta.get('callee', '')):
                continue
        if from_dt:
            invite = meta.get('invite_time', '')
            if invite and invite[:10] < from_dt:
                continue
        if to_dt:
            invite = meta.get('invite_time', '')
            if invite and invite[:10] > to_dt:
                continue

        # segments.jsonl 읽어서 세그먼트 수 계산
        segs = _read_jsonl(os.path.join(d, 'segments.jsonl'))
        seg_count = len(segs)
        total_ms = sum(s.get('duration_ms', 0) for s in segs)

        # has_video: va/vb 세그먼트 파일에 실제 미디어가 있는지 (keepalive-only 24B 파일 배제)
        has_video = any(
            _video_has_media(os.path.join(d, f))
            for s in segs
            for f in [s.get('video_file_a', ''), s.get('video_file_b', '')]
            if f
        )

        # 상태 판정
        status = _session_status(d, segs)

        results.append({
            'dir': d,
            'call_type': meta.get('call_type', 'volte'),
            'caller': meta.get('initiator', ''),
            'callee': meta.get('callee', ''),
            'group_id': None,
            'start_time': meta.get('invite_time'),
            'end_time': meta.get('end_time'),
            'duration': meta.get('duration', 0),
            'has_video': has_video,
            'status': status,
            'segment_count': seg_count,
            'total_speech_ms': total_ms,
        })

    total = len(results)
    return results[offset:offset + limit], total


def _scan_ptt_sessions(base: str, group_id: str = '', caller: str = '',
                       from_dt: str = '', to_dt: str = '',
                       limit: int = 200, offset: int = 0):
    """PTT 시간창 디렉터리를 스캔하여 녹취 목록 반환.
    디렉터리: {base}/ptt/{groupKey}/{YYYY}/{MM}/{DD}/{HH}/  (groupKey=ptt_groups.id)
    """
    ptt_root = os.path.join(base, 'ptt')
    if not os.path.isdir(ptt_root):
        return [], 0

    # 시간창 = ptt/{groupKey}/{YYYY}/{MM}/{DD}/{HH}  (segments.jsonl 보유)
    d4, d2 = '[0-9]' * 4, '[0-9]' * 2
    dirs = sorted(glob.glob(os.path.join(ptt_root, '*', d4, d2, d2, d2)), reverse=True)

    results = []
    for d in dirs:
        if not os.path.isdir(d):
            continue
        segs = _read_jsonl(os.path.join(d, 'segments.jsonl'))
        if not segs:
            continue
        rec_dir = d

        # 그룹 키: 경로 ptt/{groupKey}/{YYYY}/... 에서 추출
        rel = os.path.relpath(d, ptt_root).split(os.sep)
        gid = rel[0] if rel else ''
        yyyy, mm, dd, hh = (rel[1], rel[2], rel[3], rel[4]) if len(rel) >= 5 else ('', '', '', '')

        if group_id and gid != group_id:
            continue
        start = f"{yyyy}-{mm}-{dd}T{hh}:00:00" if yyyy else (segs[0].get('start_time', '') if segs else '')
        if from_dt and start and start[:10] < from_dt:
            continue
        if to_dt and start and start[:10] > to_dt:
            continue
        if caller and caller != (segs[0].get('speaker_id', '') if segs else ''):
            continue
        seg_count = len(segs)
        total_ms = sum(s.get('duration_ms', 0) for s in segs)

        # 발언 턴·화자·최대 동시 발언 — 슬롯 트랙(동시 발언·전이중)까지 반영한다.
        #   세그먼트 수만 세면 동시 발언이 과소 집계된다(3명이 겹쳐 말해도 세그먼트 1개).
        turn_count = 0
        speakers = set()
        max_con = 0
        for s in segs:
            tr = _seg_tracks(s)
            max_con = max(max_con, _max_concurrent(tr))
            for t in tr:
                if t['kind'] != 'audio':
                    continue
                spans = t['speakers']
                turn_count += len(spans) or 1
                speakers.update(sp['id'] for sp in spans if sp.get('id'))
        if not speakers:
            speakers = {s.get('speaker_id', '') for s in segs if s.get('speaker_id')}

        status = _session_status(rec_dir, segs)

        results.append({
            'dir': os.path.relpath(d, base),
            'call_type': 'ptt',
            'caller': segs[0].get('speaker_id', '') if segs else '',
            'callee': None,
            'group_id': gid,
            'window': f"{yyyy}{mm}{dd}{hh}",
            'start_time': start,
            'end_time': f"{yyyy}-{mm}-{dd}T{hh}:59:59" if yyyy else None,
            'duration': 0,
            'has_video': any(s.get('has_video') for s in segs),
            'status': status,
            'segment_count': seg_count,
            'turn_count': turn_count,
            'speaker_count': len(speakers),
            'max_concurrent': max_con,
            'total_speech_ms': total_ms,
        })

    total = len(results)
    return results[offset:offset + limit], total


def _session_status(rec_dir: str, segs: list) -> str:
    """세션 녹취 상태: recording / raw / ready.
    segs는 segments.jsonl의 원본 리스트 (병합 전).
    """
    if not segs:
        return 'raw'

    # seq 목록 추출 (중복 제거)
    seq_set = set()
    has_recording = False
    for s in segs:
        seq_set.add(s.get('seq', 0))
        af = s.get('audio_file', '')
        if af:
            full = os.path.join(rec_dir, af)
            if os.path.exists(full + '.recording'):
                has_recording = True

    if has_recording:
        return 'recording'

    all_ready = True
    for seq in seq_set:
        if seq <= 0:
            continue
        mp4 = _converted_path_mp4(rec_dir, seq)
        if not os.path.exists(mp4):
            all_ready = False
            break

    return 'ready' if all_ready else 'raw'


# ══════════════════════════════════════════════════════════════
#  세그먼트 트랙 정규화
# ══════════════════════════════════════════════════════════════

def _seg_tracks(s: dict) -> list:
    """세그먼트 원본 메타(segments.jsonl 한 행) → 트랙 목록 정규화.

    CMP 가 기록하는 `tracks[]` 가 정본이다. 그 이전 녹취는 flat 키
    (audio_file/audio1_file/speaker_id_audio1/audio_pt …)만 있으므로 여기서 합성해
    호출부가 두 포맷을 구분하지 않게 한다.
    반환 원소: {prefix, kind, slot, side, file, pt, codec, speakers[{id,offset_ms,dur_ms}]}
    """
    raw = s.get('tracks')
    if isinstance(raw, list) and raw:
        out = []
        for t in raw:
            if not isinstance(t, dict) or not t.get('file'):
                continue
            out.append({
                'prefix': t.get('prefix', ''),
                'kind': t.get('kind', 'audio'),
                'slot': t.get('slot', -1),
                'side': t.get('side', ''),
                'file': t.get('file', ''),
                'pt': t.get('pt', 0),
                'codec': t.get('codec', ''),
                'speakers': [sp for sp in (t.get('speakers') or []) if isinstance(sp, dict)],
            })
        return out

    # ── 구 녹취 합성 (tracks[] 이전) ──
    out = []
    dur = s.get('duration_ms', 0)
    seg_type = s.get('type', s.get('call_type', 'ptt'))

    def _spans(sid):
        return [{'id': sid, 'offset_ms': 0, 'dur_ms': dur}] if sid else []

    if seg_type == 'ptt':
        rep = s.get('speaker_id', '')
        for key, val in s.items():
            if not key.endswith('_file') or not val or not isinstance(val, str):
                continue
            prefix = key[:-len('_file')]          # audio, audio1, video, video2 …
            if prefix.startswith('audio'):
                kind, tail = 'audio', prefix[len('audio'):]
            elif prefix.startswith('video'):
                kind, tail = 'video', prefix[len('video'):]
            else:
                continue
            slot = int(tail) if tail.isdigit() else 0
            sid = s.get(f'speaker_id_{prefix}', '') or (rep if slot == 0 else '')
            out.append({
                'prefix': prefix, 'kind': kind, 'slot': slot, 'side': '', 'file': val,
                'pt': s.get('audio_pt', 0) if (kind == 'audio' and slot == 0) else 0,
                'codec': s.get('audio_codec', '') if (kind == 'audio' and slot == 0) else '',
                'speakers': _spans(sid),
            })
        out.sort(key=lambda t: (t['kind'], t['slot']))
        return out

    # VoIP — leg(a/b) 기준
    for prefix, key, kind, side in (('a', 'audio_file_a', 'audio', 'a'),
                                    ('b', 'audio_file_b', 'audio', 'b'),
                                    ('va', 'video_file_a', 'video', 'a'),
                                    ('vb', 'video_file_b', 'video', 'b')):
        val = s.get(key, '')
        if not val:
            continue
        out.append({
            'prefix': prefix, 'kind': kind, 'slot': -1, 'side': side, 'file': val,
            'pt': s.get(f'audio_pt_{side}', 0) if kind == 'audio' else 0,
            'codec': s.get(f'audio_codec_{side}', '') if kind == 'audio' else '',
            'speakers': _spans(s.get('caller', '') if side == 'a' else s.get('callee', '')),
        })
    return out


def _track_speaker_ids(tracks: list) -> list:
    """트랙 화자 구간을 시간순으로 훑어 등장 순서대로 중복 없는 화자 목록"""
    spans = []
    for t in tracks:
        if t['kind'] != 'audio':
            continue
        for sp in t['speakers']:
            if sp.get('id'):
                spans.append((sp.get('offset_ms', 0), sp['id']))
    spans.sort(key=lambda x: x[0])
    seen = []
    for _, sid in spans:
        if sid not in seen:
            seen.append(sid)
    return seen


def _max_concurrent(tracks: list) -> int:
    """세그먼트 안에서 동시에 열려 있던 화자 구간의 최대 수 (동시 발언 인원)"""
    events = []
    for t in tracks:
        if t['kind'] != 'audio':
            continue
        for sp in t['speakers']:
            off = sp.get('offset_ms', 0)
            d = sp.get('dur_ms', 0)
            if d <= 0:
                continue
            events.append((off, 1))
            events.append((off + d, -1))
    if not events:
        return 1 if any(t['kind'] == 'audio' for t in tracks) else 0
    events.sort(key=lambda e: (e[0], e[1]))   # 같은 시각이면 종료(-1) 먼저 — 인접 구간을 겹침으로 세지 않는다
    cur = peak = 0
    for _, delta in events:
        cur += delta
        peak = max(peak, cur)
    return peak


# ══════════════════════════════════════════════════════════════
#  세그먼트 상태 판정 + on-demand 변환
# ══════════════════════════════════════════════════════════════

def _failed_marker_path(rec_dir: str, seq: int, slot=None) -> str:
    return _converted_path_mp4(rec_dir, seq, slot) + '.failed'


def _write_failed_marker(rec_dir: str, seq: int, reason: str, slot=None):
    """변환 불가/실패 마커 — 상태를 'failed' 로 고정해 무한 '변환중' 표시를 막는다.
    retry(?retry=1) 시 삭제되어 재시도 가능."""
    try:
        with open(_failed_marker_path(rec_dir, seq, slot), 'w') as f:
            json.dump({'reason': reason, 'ts': time.time()}, f, ensure_ascii=False)
    except Exception:
        pass


def _read_failed_reason(rec_dir: str, seq: int, slot=None) -> str:
    try:
        with open(_failed_marker_path(rec_dir, seq, slot), 'r') as f:
            return (json.load(f) or {}).get('reason', '') or '변환 실패'
    except Exception:
        return '변환 실패'


def _segment_status(rec_dir: str, seg: dict, slot=None) -> str:
    """개별 세그먼트(또는 슬롯 단독본) 상태: recording / raw / transcoding / ready / failed"""
    seq = seg.get('seq', 0)

    # 대표 오디오 파일 — slot 지정 시 그 슬롯 트랙, 아니면 세그먼트 대표
    if slot is None:
        audio_file = (seg.get('audio_file', '') or seg.get('audio_file_a', '')
                      or seg.get('_audio_a', '') or seg.get('_audio', ''))
    else:
        audio_file = ''
        for t in seg.get('_tracks', []) or []:
            if t['kind'] == 'audio' and t['slot'] == slot:
                audio_file = t['file']
                break

    # seq 기반 MP4 변환 파일 확인 (최우선)
    if seq > 0:
        mp4 = _converted_path_mp4(rec_dir, seq, slot)
        if os.path.exists(mp4):
            return 'ready'
        if os.path.exists(mp4 + '.transcoding'):
            return 'transcoding'
        if os.path.exists(mp4 + '.failed'):
            return 'failed'

    # 오디오 파일 참조 자체가 없음(무데이터 세그먼트) — 변환 대상이 없어 영구 재생불가
    if not audio_file:
        return 'failed'

    full = os.path.join(rec_dir, audio_file)

    # .recording 존재 → 녹취 중
    if os.path.exists(full + '.recording'):
        return 'recording'

    # raw만 존재
    if os.path.exists(full):
        return 'raw'

    # 참조된 원본이 디스크에 없음(삭제/유실) — 재생불가
    return 'failed'


def _ffprobe_bin() -> str:
    p = (_FFMPEG[:-len('ffmpeg')] + 'ffprobe') if _FFMPEG.endswith('ffmpeg') else 'ffprobe'
    if os.path.isabs(p) and os.path.exists(p):
        return p
    return shutil.which('ffprobe') or 'ffprobe'


def _audio_duration(path: str, default: str = '60') -> str:
    """오디오 파일 길이(초, 문자열). 영상 mux 시 동기화 기준."""
    try:
        ret = subprocess.run([_ffprobe_bin(), '-v', 'error', '-show_entries', 'format=duration',
                              '-of', 'csv=p=0', path], capture_output=True, timeout=10)
        d = ret.stdout.decode().strip()
        return d or default
    except Exception:
        return default


# 파형 피크 버킷 수 — 콘솔 파형 레인 폭(≈600px)에 맞춘 고정 해상도.
_PEAKS_BUCKETS = 600


def _write_peaks(pcm_path: str, out_path: str) -> bool:
    """s16le mono PCM → 진폭 피크 배열(0..255) JSON. 콘솔 파형 레인용.

    PCM 은 변환 ffmpeg 의 두 번째 출력으로 곁다리 생성되므로 추가 프로세스가 없다.
    """
    try:
        size = os.path.getsize(pcm_path)
    except OSError:
        return False
    if size < 2:
        return False
    bucket = max(1, (size // 2) // _PEAKS_BUCKETS)
    peaks = []
    try:
        with open(pcm_path, 'rb') as f:
            while True:
                chunk = f.read(bucket * 2)
                n = len(chunk) // 2
                if n == 0:
                    break
                vals = struct.unpack(f'<{n}h', chunk[:n * 2])
                peaks.append(min(255, max(abs(v) for v in vals) * 255 // 32768))
        with open(out_path, 'w') as f:
            json.dump({'buckets': len(peaks), 'peaks': peaks}, f)
        return True
    except Exception as e:
        logger.error("_write_peaks error: %s", e)
        return False


def _video_grid_filter(n: int, audio_dur: str):
    """영상 트랙 n개를 격자로 합치는 filter_complex + 캔버스 크기.
    1개는 호출부가 copy mux 로 처리하므로 여기서는 2개 이상만 다룬다
    (2 = 좌우, 3~4 = 2x2 …). 입력 라벨은 [v0]..[v{n-1}] 로 온다고 가정."""
    cols = 2
    rows = (n + cols - 1) // cols
    cell = 640
    w, h = cols * cell, rows * cell
    parts = [f'color=c=black:s={w}x{h}:r=15:d={audio_dur}[bg]']
    for i in range(n):
        parts.append(f'[v{i}]scale={cell}:{cell}:force_original_aspect_ratio=decrease,'
                     f'pad={cell}:{cell}:(ow-iw)/2:(oh-ih)/2:black[c{i}]')
    prev = 'bg'
    for i in range(n):
        x, y = (i % cols) * cell, (i // cols) * cell
        label = 'vout' if i == n - 1 else f'ov{i}'
        parts.append(f'[{prev}][c{i}]overlay={x}:{y}:eof_action=pass[{label}]')
        prev = label
    return ';'.join(parts), w, h


def _transcode_ptt_multi(rec_dir: str, seg: dict, slot, out_path: str, tmp_out: str) -> bool:
    """PTT 세그먼트 변환 — 슬롯 트랙 N개를 다룬다.

    slot=None → 화자 전원 amix (실제로 무전에서 들린 소리 = 기본 재생본)
    slot=K    → 슬롯 K 화자 단독본

    동시 발언(dual/multi-talker)과 floor 없는 private call(전이중, 멤버별 슬롯)이
    모두 이 경로를 쓴다. 슬롯이 1개뿐이면 종전 단일 트랙 변환과 동일한 결과다.
    """
    seq = seg.get('seq', 0)
    tracks = seg.get('_tracks', []) or []
    atracks = [t for t in tracks if t['kind'] == 'audio']
    vtracks = [t for t in tracks if t['kind'] == 'video']
    if slot is not None:
        atracks = [t for t in atracks if t['slot'] == slot]
        vtracks = [t for t in vtracks if t['slot'] == slot]
    atracks.sort(key=lambda t: t['slot'])
    vtracks.sort(key=lambda t: t['slot'])

    tmp_files = []
    try:
        # 1) 슬롯별 AMR-WB 추출 — PT 는 슬롯마다 다를 수 있다(이종 단말 혼재)
        amrs = []
        for t in atracks:
            raw = os.path.join(rec_dir, t['file'])
            if not os.path.exists(raw):
                continue
            amr = f"{raw}.amr_s{t['slot']}"
            ok = _strip_rtp_to_amrwb(raw, amr, audio_pt=t.get('pt', 0))
            if os.path.exists(amr):
                tmp_files.append(amr)
            if ok:
                amrs.append(amr)
            codec = t.get('codec', '')
            if codec and not codec.upper().startswith('AMR-WB'):
                logger.warning("transcode seg %d slot %s: unsupported codec meta '%s' — AMR-WB 로 시도",
                               seq, t['slot'], codec)

        if not amrs:
            _write_failed_marker(rec_dir, seq, '녹취 음성 데이터 없음(프레임 0)', slot)
            logger.warning("transcode seg %d slot=%s: no audio frames — failed 확정", seq, slot)
            return False

        # 2) 슬롯별 H.264 추출 (keepalive-only 는 시도 생략)
        h264s = []
        for t in vtracks:
            raw = os.path.join(rec_dir, t['file'])
            if not os.path.exists(raw) or not _video_has_media(raw):
                continue
            h = f"{raw}.h264_s{t['slot']}"
            ok = _strip_rtp_to_h264(raw, h)
            if os.path.exists(h):
                tmp_files.append(h)
            if ok:
                h264s.append(h)

        audio_dur = _audio_duration(amrs[0])
        pcm_out = tmp_out + '.pcm'

        cmd = [_FFMPEG, '-y', '-hide_banner', '-loglevel', 'error']
        for a in amrs:
            cmd += ['-i', a]
        for h in h264s:
            cmd += ['-f', 'h264', '-r', '15', '-i', h]

        n_a, n_v = len(amrs), len(h264s)
        # 필터 출력 라벨은 한 번만 소비할 수 있으므로 asplit 으로 mp4/파형용을 나눈다.
        if n_a > 1:
            mix = ''.join(f'[{i}:a]' for i in range(n_a))
            afilter = (f'{mix}amix=inputs={n_a}:duration=longest:normalize=0,'
                       f'dynaudnorm,asplit=2[aout][apcm]')
        else:
            afilter = '[0:a]asplit=2[aout][apcm]'

        if n_v == 0:
            cmd += ['-filter_complex', afilter, '-map', '[aout]']
        elif n_v == 1:
            # 영상 1개 — 재인코딩 없이 원본 mux (종전 PTT 영상 경로와 동일)
            cmd += ['-filter_complex', afilter, '-map', f'{n_a}:v', '-map', '[aout]',
                    '-t', audio_dur, '-c:v', 'copy']
        else:
            vf, _w, _h = _video_grid_filter(n_v, audio_dur)
            # 입력 인덱스 → [v{i}] 라벨 매핑을 filter 앞에 붙인다
            relabel = ''.join(f'[{n_a + i}:v]null[v{i}];' for i in range(n_v))
            cmd += ['-filter_complex', f'{afilter};{relabel}{vf}',
                    '-map', '[vout]', '-map', '[aout]',
                    '-t', audio_dur, '-c:v', 'libx264', '-preset', 'fast', '-crf', '23']

        cmd += ['-c:a', 'aac', '-ar', '16000', '-ac', '1', '-movflags', '+faststart', tmp_out]
        # 두 번째 출력 = 파형 피크용 PCM (같은 디코딩·믹싱 결과를 재사용 — 추가 프로세스 없음)
        cmd += ['-map', '[apcm]', '-f', 's16le', '-ar', '8000', '-ac', '1', pcm_out]

        ret = subprocess.run(cmd, capture_output=True, timeout=300)
        if ret.returncode != 0:
            logger.warning("ffmpeg ptt seg=%d slot=%s failed: %s", seq, slot,
                           ret.stderr.decode(errors='replace')[:500])

        if os.path.exists(pcm_out):
            tmp_files.append(pcm_out)
            _write_peaks(pcm_out, _peaks_path(rec_dir, seq, slot))

        if os.path.exists(tmp_out) and os.path.getsize(tmp_out) > 256:
            os.replace(tmp_out, out_path)
            try: os.remove(_failed_marker_path(rec_dir, seq, slot))
            except OSError: pass
            return True

        _write_failed_marker(rec_dir, seq, '변환 실패(ffmpeg 출력 없음)', slot)
        logger.warning("transcode seg %d slot=%s produced no/empty output — failed 확정", seq, slot)
        return False
    finally:
        for tmp in tmp_files:
            try: os.remove(tmp)
            except OSError: pass


def _transcode_segment_file(rec_dir: str, seg: dict, slot=None):
    """세그먼트의 raw RTP를 변환 (백그라운드 스레드). 출력: seg_NNNN.mp4 / seg_NNNN_s{K}.mp4

    VoIP 음성:  A + B 음성 mixing → MP4 (AAC)
    VoIP 영상:  A + B 음성 mixing + A + B 영상 좌우 배치 → MP4
    PTT:        슬롯 트랙 N개 amix(믹스) 또는 슬롯 단독 → MP4 (AAC) — _transcode_ptt_multi
    """
    seg_type = seg.get('type', seg.get('_type', 'ptt'))
    seq = seg.get('seq', 0)
    lock_key = f"{rec_dir}:{seq}:{'mix' if slot is None else slot}"
    out_path = _converted_path_mp4(rec_dir, seq, slot)
    # ffmpeg 는 출력 파일을 변환 시작 시점에 생성하므로(부분 파일), 최종 경로(seg_NNNN.mp4)에
    # 바로 쓰면 변환 도중 _segment_status 가 부분 mp4 를 'ready' 로 오판 → 첫 재생이 깨진다.
    # → 임시 파일(.partial.mp4)에 쓰고 완료 시에만 원자적 rename 하여 "mp4 존재 ⟺ 완성" 을 보장.
    tmp_out = (out_path[:-4] if out_path.endswith('.mp4') else out_path) + '.partial.mp4'

    # ── PTT: 슬롯 트랙 기반 변환 (믹스/단독) ──
    # 슬롯 1개짜리 단일 화자 세그먼트도 같은 경로를 지나며 결과는 종전과 같다.
    if seg_type == 'ptt':
        try:
            _transcode_ptt_multi(rec_dir, seg, slot, out_path, tmp_out)
        except Exception as e:
            _write_failed_marker(rec_dir, seq, f'변환 오류: {e}', slot)
            logger.error("transcode seg %d slot=%s error: %s", seq, slot, e)
        finally:
            try:
                if os.path.exists(tmp_out): os.remove(tmp_out)
            except OSError:
                pass
            with _transcoding_mutex:
                _transcoding_locks.pop(lock_key, None)
        return

    # ── VoIP: leg(a/b) 기반 변환 ──
    # raw 파일 경로 + leg 별 오디오 PT/코덱 메타 (없으면 0/'' → strip 이 자동감지)
    raw_a = seg.get('_audio_a', '')
    if raw_a: raw_a = os.path.join(rec_dir, raw_a)
    raw_b = seg.get('_audio_b', '')
    if raw_b: raw_b = os.path.join(rec_dir, raw_b)
    raw_va = seg.get('_video_a', '')
    if raw_va: raw_va = os.path.join(rec_dir, raw_va)
    raw_vb = seg.get('_video_b', '')
    if raw_vb: raw_vb = os.path.join(rec_dir, raw_vb)
    pt_a = seg.get('audio_pt_a', 0)
    pt_b = seg.get('audio_pt_b', 0)
    codecs = [seg.get('audio_codec_a', ''), seg.get('audio_codec_b', '')]

    # 지원 디코더는 AMR-WB 뿐 — 다른 코덱 협상 흔적이 있으면 드러낸다 (변환은 시도).
    for c in codecs:
        if c and not c.upper().startswith('AMR-WB'):
            logger.warning("transcode seg %d: unsupported codec meta '%s' — AMR-WB 로 시도", seq, c)

    # 최소 하나의 오디오 파일 필요 — 없으면 영구 재생불가로 확정 (무한 '변환중' 방지)
    primary = raw_a or raw_b
    if not primary or not os.path.exists(primary):
        _write_failed_marker(rec_dir, seq, '녹취 원본 파일 없음')
        with _transcoding_mutex:
            _transcoding_locks.pop(lock_key, None)
        return

    # .transcoding 마커
    marker = out_path + '.transcoding'
    try: open(marker, 'w').close()
    except: pass

    tmp_files = []

    try:
        # 1) 오디오 추출: AMR-WB (메타 PT 우선, 없으면 자동감지)
        amr_a = primary + '.amr_a'
        amr_b = (raw_b + '.amr_b') if raw_b and os.path.exists(raw_b) else ''
        has_a = _strip_rtp_to_amrwb(raw_a, amr_a, audio_pt=pt_a) if raw_a and os.path.exists(raw_a) else False
        has_b = _strip_rtp_to_amrwb(raw_b, amr_b, audio_pt=pt_b) if amr_b else False
        # 실패(9바이트 헤더만) 산출물도 정리 대상에 포함 — 잔재 방지
        if amr_a and os.path.exists(amr_a): tmp_files.append(amr_a)
        if amr_b and os.path.exists(amr_b): tmp_files.append(amr_b)

        # 오디오 프레임이 한쪽도 없으면(빈/극소 녹취 — keepalive 만 기록 등) 영구 재생불가 확정.
        if not has_a and not has_b:
            _write_failed_marker(rec_dir, seq, '녹취 음성 데이터 없음(프레임 0)')
            logger.warning("transcode seg %d: no audio frames (file too small/empty) — failed 확정", seq)
            return

        # 2) 영상 추출: H.264 (keepalive-only 영상 파일은 시도 자체를 생략)
        h264_a = (raw_va + '.h264_a') if raw_va and os.path.exists(raw_va) and _video_has_media(raw_va) else ''
        h264_b = (raw_vb + '.h264_b') if raw_vb and os.path.exists(raw_vb) and _video_has_media(raw_vb) else ''
        has_va = _strip_rtp_to_h264(raw_va, h264_a) if h264_a else False
        has_vb = _strip_rtp_to_h264(raw_vb, h264_b) if h264_b else False
        # 실패 산출물도 정리 대상에 포함 — 0바이트 .h264_* 잔재 방지
        if h264_a and os.path.exists(h264_a): tmp_files.append(h264_a)
        if h264_b and os.path.exists(h264_b): tmp_files.append(h264_b)

        has_video = has_va or has_vb

        # 음성 길이 결정 (영상 동기화 기준)
        audio_dur = '60'  # 기본값
        aud_ref = amr_a if has_a else (amr_b if has_b else '')
        if aud_ref:
            try:
                _ffprobe = (_FFMPEG[:-len('ffmpeg')] + 'ffprobe') if _FFMPEG.endswith('ffmpeg') else 'ffprobe'
                if not (os.path.isabs(_ffprobe) and os.path.exists(_ffprobe)):
                    _ffprobe = shutil.which('ffprobe') or 'ffprobe'
                dur_ret = subprocess.run(
                    [_ffprobe, '-v', 'error', '-show_entries', 'format=duration',
                     '-of', 'csv=p=0', aud_ref],
                    capture_output=True, timeout=10)
                d = dur_ret.stdout.decode().strip()
                if d:
                    audio_dur = d
            except Exception:
                pass

        if has_video and has_a and has_b and has_va and has_vb:
            # ── VoLTE 영상: 발신(A)=왼쪽, 착신(B)=오른쪽 ──
            # color 소스(검정 1280x640)를 배경으로 깔고 영상을 overlay
            # 영상 없는 구간은 검정, -t로 음성 길이에 맞춤
            cmd = [
                _FFMPEG, '-y', '-hide_banner', '-loglevel', 'error',
                '-i', amr_a, '-i', amr_b,
                '-f', 'h264', '-i', h264_a,
                '-f', 'h264', '-i', h264_b,
                '-f', 'lavfi', '-i', f'color=c=black:s=1280x640:r=25:d={audio_dur}',
                '-filter_complex',
                '[0:a][1:a]amix=inputs=2:duration=longest:normalize=0,dynaudnorm[aout];'
                '[2:v]scale=640:640:force_original_aspect_ratio=decrease,pad=640:640:(ow-iw)/2:(oh-ih)/2:black[va];'
                '[3:v]scale=640:640:force_original_aspect_ratio=decrease,pad=640:640:(ow-iw)/2:(oh-ih)/2:black[vb];'
                '[4:v][va]overlay=0:0:eof_action=pass[left];'
                '[left][vb]overlay=640:0:eof_action=pass[vout]',
                '-map', '[vout]', '-map', '[aout]',
                '-t', audio_dur,
                '-c:v', 'libx264', '-preset', 'fast', '-crf', '23',
                '-c:a', 'aac', '-ar', '16000', '-ac', '1',
                '-movflags', '+faststart',
                tmp_out,
            ]
            ret = subprocess.run(cmd, capture_output=True, timeout=300)
            if ret.returncode != 0:
                logger.warning("ffmpeg video hstack failed seq=%d: %s", seq, ret.stderr.decode(errors='replace')[:500])

        elif seg_type == 'ptt' and has_video and has_a and has_va:
            # ── PTT 영상: AMR-WB→AAC + H.264 원본 mux (re-encode 없음) ──
            # H.264 raw에 타임스탬프가 없으므로 -shortest 대신 -t로 오디오 길이에 맞춤
            cmd = [
                _FFMPEG, '-y', '-hide_banner', '-loglevel', 'error',
                '-f', 'h264', '-r', '15', '-i', h264_a,
                '-i', amr_a,
                '-t', audio_dur,
                '-c:v', 'copy',
                '-c:a', 'aac', '-ar', '16000', '-ac', '1',
                '-movflags', '+faststart',
                tmp_out,
            ]
            ret = subprocess.run(cmd, capture_output=True, timeout=300)
            if ret.returncode != 0:
                logger.warning("ffmpeg ptt video failed seq=%d: %s", seq, ret.stderr.decode(errors='replace')[:500])

        elif has_video and (has_a or has_b) and (has_va or has_vb):
            # ── VoLTE 영상 (한쪽만): 발신=왼쪽, 착신=오른쪽 ──
            vid = h264_a if has_va else h264_b
            aud = amr_a if has_a else amr_b
            x_base = '0' if has_va else '640'
            cmd = [
                _FFMPEG, '-y', '-hide_banner', '-loglevel', 'error',
                '-i', aud,
                '-f', 'h264', '-i', vid,
                '-f', 'lavfi', '-i', f'color=c=black:s=1280x640:r=25:d={audio_dur}',
                '-filter_complex',
                '[1:v]scale=640:640:force_original_aspect_ratio=decrease,pad=640:640:(ow-iw)/2:(oh-ih)/2:black[v1];'
                f'[2:v][v1]overlay=x={x_base}:y=0:eof_action=pass[vout]',
                '-map', '[vout]', '-map', '0:a',
                '-t', audio_dur,
                '-c:v', 'libx264', '-preset', 'fast', '-crf', '23',
                '-c:a', 'aac', '-ar', '16000', '-ac', '1',
                '-movflags', '+faststart',
                tmp_out,
            ]
            subprocess.run(cmd, capture_output=True, timeout=300)

        elif has_a and has_b:
            # ── VoLTE 음성: A+B mixing → MP4 (AAC audio only) ──
            cmd = [
                _FFMPEG, '-y', '-hide_banner', '-loglevel', 'error',
                '-i', amr_a, '-i', amr_b,
                '-filter_complex', '[0:a][1:a]amix=inputs=2:duration=longest:normalize=0,dynaudnorm[aout]',
                '-map', '[aout]',
                '-c:a', 'aac', '-ar', '16000', '-ac', '1',
                '-movflags', '+faststart',
                tmp_out,
            ]
            subprocess.run(cmd, capture_output=True, timeout=120)

        elif has_a or has_b:
            # ── PTT 또는 VoLTE 단측 → MP4 (AAC) ──
            aud = amr_a if has_a else amr_b
            cmd = [
                _FFMPEG, '-y', '-hide_banner', '-loglevel', 'error',
                '-i', aud,
                '-c:a', 'aac', '-ar', '16000', '-ac', '1',
                '-movflags', '+faststart',
                tmp_out,
            ]
            subprocess.run(cmd, capture_output=True, timeout=120)

        # 변환 완료분만 원자적으로 노출 — 정상 출력(.partial.mp4)이 생성됐을 때만 최종 경로로 rename.
        # 이로써 _segment_status 가 mp4 존재만으로 'ready' 판정해도 항상 "완성된" 파일을 가리킨다.
        if os.path.exists(tmp_out) and os.path.getsize(tmp_out) > 256:
            os.replace(tmp_out, out_path)
            # 재시도 성공 시 이전 실패 마커 해제
            try: os.remove(_failed_marker_path(rec_dir, seq))
            except OSError: pass
        else:
            _write_failed_marker(rec_dir, seq, '변환 실패(ffmpeg 출력 없음)')
            logger.warning("transcode seg %d produced no/empty output — failed 확정", seq)

    except Exception as e:
        _write_failed_marker(rec_dir, seq, f'변환 오류: {e}')
        logger.error("transcode seg %d error: %s", seq, e)
    finally:
        for tmp in tmp_files:
            try: os.remove(tmp)
            except: pass
        # 실패/중단으로 남은 부분 파일 정리 (성공 시엔 rename 되어 이미 없음)
        try:
            if os.path.exists(tmp_out): os.remove(tmp_out)
        except: pass
        try: os.remove(marker)
        except: pass
        # 어느 경로로 빠져나가든(조기 return 포함) 중복방지 lock 을 반드시 해제한다.
        with _transcoding_mutex:
            _transcoding_locks.pop(lock_key, None)


def _ensure_segment_ready(rec_dir: str, seg: dict, slot=None) -> str:
    """세그먼트(또는 슬롯 단독본) 변환 보장. 상태 문자열 반환.
    seg 는 _build_segments()의 병합된 dict."""
    if slot is None:
        status = seg.get('status', _segment_status(rec_dir, seg))
    else:
        status = _segment_status(rec_dir, seg, slot)

    if status in ('recording', 'ready', 'transcoding', 'failed'):
        return status

    if status == 'raw':
        seq = seg.get('seq', 0)
        lock_key = f"{rec_dir}:{seq}:{'mix' if slot is None else slot}"
        with _transcoding_mutex:
            if lock_key in _transcoding_locks:
                return 'transcoding'
            _transcoding_locks[lock_key] = True
        # 워커 풀에 큐잉 — max_workers 만큼만 동시 ffmpeg 실행(초과분은 큐 대기).
        # 요청 스레드는 즉시 반환(202), 변환은 풀에서 비동기 처리.
        if _transcode_executor is not None:
            _transcode_executor.submit(_transcode_segment_file, rec_dir, seg, slot)
        else:
            # init 전(이론상 없음) fallback — 단발 스레드
            threading.Thread(target=_transcode_segment_file, args=(rec_dir, seg, slot),
                             daemon=True).start()
        return 'transcoding'

    return status


# ══════════════════════════════════════════════════════════════
#  세그먼트 목록 빌드
# ══════════════════════════════════════════════════════════════

def _build_segments(rec_dir: str) -> list:
    """segments.jsonl에서 세그먼트 목록 구성.
    VoIP: 한 행에 audio_file_a/b, video_file_a/b 모두 포함 (PRtpTrans 통합 기록).
    PTT: 한 행에 audio_file 포함.
    """
    segs_raw = _read_jsonl(os.path.join(rec_dir, 'segments.jsonl'))
    result = []

    for s in segs_raw:
        seq = s.get('seq', 0)
        seg_type = s.get('type', s.get('call_type', 'ptt'))

        # 트랙 정규화 — 슬롯(동시 발언·전이중 private call)·leg(VoIP) 공통 표현
        tracks = _seg_tracks(s)
        for t in tracks:
            if t['kind'] == 'video' and not _video_has_media(os.path.join(rec_dir, t['file'])):
                t['_ghost'] = True     # keepalive-only 영상 트랙 — 없는 것으로 취급
        tracks = [t for t in tracks if not t.get('_ghost')]

        # 유령 영상 방어 — keepalive-only(헤더만) 영상 트랙만 있으면 음성 세그먼트로 정정.
        #   (콘텐츠 타입·플레이어 선택이 이 값을 따른다)
        has_video = bool(s.get('has_video', False)) and any(t['kind'] == 'video' for t in tracks)

        # 대표 오디오 파일 (상태 판정용)
        primary_audio = (s.get('audio_file_a', '') or s.get('audio_file', '')
                         or s.get('audio_file_b', ''))
        if not primary_audio:
            for t in tracks:
                if t['kind'] == 'audio':
                    primary_audio = t['file']
                    break

        status = _segment_status(rec_dir, {'audio_file': primary_audio, 'seq': seq})
        status_reason = ''
        if status == 'failed':
            if not primary_audio:
                status_reason = '녹취 음성 데이터 없음'
            elif os.path.exists(_failed_marker_path(rec_dir, seq)):
                status_reason = _read_failed_reason(rec_dir, seq)
            else:
                status_reason = '녹취 원본 파일 없음'

        file_size = 0
        conv = _converted_path_mp4(rec_dir, seq)
        if os.path.exists(conv):
            file_size = os.path.getsize(conv)

        audio_tracks = [t for t in tracks if t['kind'] == 'audio']
        video_slots = {t['slot'] for t in tracks if t['kind'] == 'video'}
        speaker_ids = _track_speaker_ids(tracks)

        result.append({
            'seq': seq,
            'type': seg_type,
            'speaker_id': s.get('speaker_id', ''),
            'caller': s.get('caller', ''),
            'callee': s.get('callee', ''),
            'start_time': s.get('start_time'),
            'end_time': s.get('end_time'),
            'duration_ms': s.get('duration_ms', 0),
            'has_video': has_video,
            'file_size': file_size,
            'status': status,
            'status_reason': status_reason,
            # ── 슬롯 트랙 (동시 발언 dual/multi-talker, floor 없는 전이중 private call) ──
            # 슬롯이 1개뿐인 단일 화자 세그먼트는 tracks 원소도 1개 — 콘솔이 종전과 같이 보인다.
            'tracks': [{
                'slot': t['slot'],
                'kind': t['kind'],
                'side': t['side'],
                'pt': t['pt'],
                'codec': t['codec'],
                'speakers': t['speakers'],
                'has_video': (t['slot'] in video_slots) if t['kind'] == 'audio' else True,
                'status': _segment_status(rec_dir, {'seq': seq, '_tracks': tracks}, t['slot'])
                          if (t['kind'] == 'audio' and t['slot'] >= 0) else status,
            } for t in tracks],
            'speaker_ids': speaker_ids,
            'talker_count': len(audio_tracks),
            'max_concurrent': _max_concurrent(tracks),
            # 오디오 PT/코덱 메타 (CMP 기록 — 변환기 PT 판별 근거. 구 녹취는 없음 → 자동감지)
            'audio_pt': s.get('audio_pt', 0),
            'audio_codec': s.get('audio_codec', ''),
            'audio_pt_a': s.get('audio_pt_a', 0),
            'audio_codec_a': s.get('audio_codec_a', ''),
            'audio_pt_b': s.get('audio_pt_b', 0),
            'audio_codec_b': s.get('audio_codec_b', ''),
            # 파일 참조 (변환에서 사용 — 응답에서는 제거)
            '_tracks': tracks,
            '_audio_a': s.get('audio_file_a', ''),
            '_audio_b': s.get('audio_file_b', ''),
            '_audio': s.get('audio_file', ''),
            '_video_a': s.get('video_file_a', ''),
            '_video_b': s.get('video_file_b', ''),
            '_video': s.get('video_file', ''),
        })
    return result


def _converted_path_mp4(rec_dir: str, seq: int, slot=None) -> str:
    """변환된 MP4 파일 경로.
    slot=None → 세그먼트 믹스(동시 발언 화자 전원 합성 — 실제로 들린 소리, 기본 재생본).
    slot=K    → 슬롯 K 화자 단독본 (화자 식별·증거용).
    믹스 경로는 종전과 같아 기존 변환 캐시가 그대로 유효하다."""
    if slot is None:
        return os.path.join(rec_dir, f"seg_{seq:04d}.mp4")
    return os.path.join(rec_dir, f"seg_{seq:04d}_s{int(slot)}.mp4")


def _peaks_path(rec_dir: str, seq: int, slot=None) -> str:
    """파형 피크 JSON 경로 (변환 산출물과 함께 캐시)"""
    mp4 = _converted_path_mp4(rec_dir, seq, slot)
    return mp4[:-4] + '.peaks.json' if mp4.endswith('.mp4') else mp4 + '.peaks.json'


# ══════════════════════════════════════════════════════════════
#  HTTP Handler
# ══════════════════════════════════════════════════════════════

_REC_BASE = '/api/v1/recordings'


def _path_parts(full_path: str, base: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _parse_rec_route(parts: tuple):
    """parts에서 session_dir과 action을 분리.
    예약어: segments, audio, video, delete
    parts 끝에서부터 역방향으로 예약어를 탐지하여 분리.

    예시:
      ('volte','2026','04','14','10','+82..','S2026...d')
        → session_dir='voip/2026/04/14/10/+82../S2026...d', action=None, extra=()
      ('volte','2026','04','14','10','+82..','S2026...d','segments')
        → session_dir=..., action='segments', extra=()
      ('volte','2026','04','14','10','+82..','S2026...d','segments','1','audio')
        → session_dir=..., action='segments', extra=('1','audio')
      ('volte','2026','04','14','10','+82..','S2026...d','audio')
        → session_dir=..., action='audio', extra=()
    """
    if not parts:
        return '', None, ()

    # 끝에서부터 action 패턴 탐지
    # 패턴 1: .../segments/{seq}/audio|video|peaks
    if len(parts) >= 3 and parts[-1] in ('audio', 'video', 'peaks'):
        # .../segments/{seq}/audio 패턴 확인
        try:
            seq_idx = len(parts) - 2
            int(parts[seq_idx])  # seq가 숫자인지
            if seq_idx >= 1 and parts[seq_idx - 1] == 'segments':
                session_dir = '/'.join(parts[:seq_idx - 1])
                return session_dir, 'segments', (parts[seq_idx], parts[-1])
        except (ValueError, IndexError):
            pass

    # 패턴 2: .../segments
    if parts[-1] == 'segments':
        session_dir = '/'.join(parts[:-1])
        return session_dir, 'segments', ()

    # 패턴 3: .../audio 또는 .../video
    if parts[-1] in ('audio', 'video'):
        session_dir = '/'.join(parts[:-1])
        return session_dir, parts[-1], ()

    # 패턴 4: 그 외 — 전체가 session_dir
    session_dir = '/'.join(parts)
    return session_dir, None, ()


async def handle_recordings(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    base = config.get('ServiceLogDir', _service_log_dir)
    # query string 은 full_path 가 아니라 query_params dict 로 전달된다 (이미 URL-decode).
    qs = handler_args.query_params or {}
    parts = _path_parts(handler_args.full_path, _REC_BASE)
    method = handler_args.method.upper()

    try:
        # GET /recordings — 목록
        if len(parts) == 0 and method == 'GET':
            return await _list_recordings(base, qs)

        if len(parts) < 1:
            return HandlerResult(status=404, body={'error': 'Not Found'})

        session_dir, action, extra = _parse_rec_route(parts)

        if not session_dir:
            return HandlerResult(status=404, body={'error': 'Not Found'})

        if action is None:
            if method == 'GET':
                return await _get_recording(base, session_dir)
            elif method == 'DELETE':
                return await _delete_recording(base, session_dir)

        elif action == 'segments':
            if not extra and method == 'GET':
                return await _list_segments(base, session_dir)
            elif len(extra) == 2 and method == 'GET':
                seq = int(extra[0])
                retry = str(qs.get('retry', '')).strip() in ('1', 'true')
                # slot 미지정 = 믹스(동시 발언 화자 전원 합성). slot=K = 슬롯 K 화자 단독본.
                slot_q = str(qs.get('slot', '')).strip()
                slot = int(slot_q) if slot_q.isdigit() else None
                if extra[1] == 'peaks':
                    return await _stream_peaks(base, session_dir, seq, slot=slot)
                return await _stream_segment(base, session_dir, seq,
                                             video=(extra[1] == 'video'), retry=retry, slot=slot)

        elif action == 'audio' and method == 'GET':
            return await _stream_whole_audio(base, session_dir)

        return HandlerResult(status=404, body={'error': 'Not Found'})
    except Exception as e:
        logger.error("handle_recordings error: %s", e)
        return HandlerResult(status=500, body={'error': str(e)})


async def _list_recordings(base: str, qs: dict) -> HandlerResult:
    def qp(name, default=None):
        v = qs.get(name)
        return v if v not in (None, '') else default

    call_type = qp('call_type', '')
    caller = qp('caller', '')
    group_id = qp('group_id', '')
    from_dt = qp('from_dt', '')
    to_dt = qp('to_dt', '')
    limit = min(int(qp('limit', '200')), 1000)
    offset = int(qp('offset', '0'))

    all_recs = []

    if call_type != 'ptt':
        voip, _ = _scan_voip_sessions(base, caller=caller, from_dt=from_dt, to_dt=to_dt,
                                       limit=9999, offset=0)
        all_recs.extend(voip)

    if call_type != 'volte':
        ptt, _ = _scan_ptt_sessions(base, group_id=group_id, caller=caller,
                                     from_dt=from_dt, to_dt=to_dt, limit=9999, offset=0)
        all_recs.extend(ptt)

    # 시간 역순 정렬
    all_recs.sort(key=lambda r: r.get('start_time') or '', reverse=True)
    total = len(all_recs)
    page = all_recs[offset:offset + limit]

    # dir를 id로 사용 (base 경로 제거하여 상대경로로)
    for r in page:
        d = r.pop('dir', '')
        r['id'] = os.path.relpath(d, base) if base else d

    return HandlerResult(status=200, body={
        'total': total, 'limit': limit, 'offset': offset, 'recordings': page
    })


async def _get_recording(base: str, rel_dir: str) -> HandlerResult:
    d = os.path.join(base, rel_dir)
    if not os.path.isdir(d):
        return HandlerResult(status=404, body={'error': 'Not found'})

    # VoIP or PTT?
    call_json = os.path.join(d, 'call.json')
    sess_json = os.path.join(d, 'session.json')

    if os.path.exists(call_json):
        meta = _read_json(call_json) or {}
        rec_dir = d
        rec = {
            'id': rel_dir,
            'call_type': meta.get('call_type', 'volte'),
            'caller': meta.get('initiator', ''),
            'callee': meta.get('callee', ''),
            'group_id': None,
            'start_time': meta.get('invite_time'),
            'end_time': meta.get('end_time'),
            'duration': meta.get('duration', 0),
            'end_reason': meta.get('end_reason'),
        }
    else:
        # PTT: session.json 있으면 사용, 없으면 디렉터리/segments.jsonl 기반
        meta = _read_json(sess_json) if os.path.exists(sess_json) else {}
        if meta is None:
            meta = {}
        # rec_dir: recordings/ 하위 또는 세션 디렉터리 직접
        rec_dir = os.path.join(d, 'recordings')
        if not os.path.exists(os.path.join(rec_dir, 'segments.jsonl')):
            rec_dir = d

        # 그룹 ID: meta에서 또는 디렉터리 경로에서
        gid = meta.get('group_id', '')
        if not gid:
            parts = d.split(os.sep)
            try:
                sess_idx = parts.index('sessions')
                gid = parts[sess_idx - 1] if sess_idx > 0 else ''
            except ValueError:
                gid = ''

        start = meta.get('start_time', '')
        if not start:
            # 첫 세그먼트 시간
            first_segs = _read_jsonl(os.path.join(rec_dir, 'segments.jsonl'))
            if first_segs:
                start = first_segs[0].get('start_time', '')

        if not meta and not os.path.exists(os.path.join(rec_dir, 'segments.jsonl')):
            return HandlerResult(status=404, body={'error': 'No metadata'})

        rec = {
            'id': rel_dir,
            'call_type': 'ptt',
            'caller': meta.get('initiator', ''),
            'callee': None,
            'group_id': gid,
            'start_time': start,
            'end_time': meta.get('end_time'),
            'duration': 0,
            'state': meta.get('state', ''),
        }

    segs = _build_segments(rec_dir)
    rec['segments'] = [_public_seg(s) for s in segs]
    rec['segment_count'] = len(segs)
    rec['total_speech_ms'] = sum(s['duration_ms'] for s in segs)
    # 발언 턴 = 화자 구간 수 (동시 발언 세그먼트는 턴이 여럿). 세그먼트 수와 구분한다.
    rec['turn_count'] = sum(len(t.get('speakers') or []) or 1
                            for s in segs for t in s.get('tracks', []) if t.get('kind') == 'audio')
    rec['max_concurrent'] = max((s.get('max_concurrent', 1) for s in segs), default=0)
    rec['has_video'] = any(s['has_video'] for s in segs)
    rec['status'] = _session_status(rec_dir, _read_jsonl(os.path.join(rec_dir, 'segments.jsonl')))

    return HandlerResult(status=200, body=rec)


async def _delete_recording(base: str, rel_dir: str) -> HandlerResult:
    d = os.path.join(base, rel_dir)
    if not os.path.isdir(d):
        return HandlerResult(status=404, body={'error': 'Not found'})

    import shutil
    shutil.rmtree(d, ignore_errors=True)
    return HandlerResult(status=200, body={'id': rel_dir, 'deleted': True})


def _public_seg(seg: dict) -> dict:
    """응답용 세그먼트 — 변환기 전용 내부 키(_tracks/_audio_* 등 raw 파일 참조)는 제외."""
    return {k: v for k, v in seg.items() if not k.startswith('_')}


def _find_rec_dir(d: str) -> str:
    """세그먼트가 있는 녹취 디렉터리 찾기: recordings/ 하위 또는 세션 디렉터리 직접"""
    rec_dir = os.path.join(d, 'recordings')
    if os.path.exists(os.path.join(rec_dir, 'segments.jsonl')):
        return rec_dir
    return d


async def _list_segments(base: str, rel_dir: str) -> HandlerResult:
    d = os.path.join(base, rel_dir)
    rec_dir = _find_rec_dir(d)
    segs = _build_segments(rec_dir)
    return HandlerResult(status=200, body={'id': rel_dir, 'segments': [_public_seg(s) for s in segs]})


def _find_seg(rec_dir: str, seq: int):
    for s in _build_segments(rec_dir):
        if s.get('seq') == seq:
            return s
    return None


def _slot_has_video(seg: dict, slot) -> bool:
    """해당 재생 단위에 영상이 있는가 — Content-Type·플레이어 선택 기준"""
    if slot is None:
        return bool(seg.get('has_video'))
    for t in seg.get('tracks', []) or []:
        if t.get('kind') == 'audio' and t.get('slot') == slot:
            return bool(t.get('has_video'))
    return False


async def _stream_segment(base: str, rel_dir: str, seq: int, video: bool = False,
                          retry: bool = False, slot=None) -> HandlerResult:
    d = os.path.join(base, rel_dir)
    rec_dir = _find_rec_dir(d)

    # 명시 재시도(?retry=1) — 실패 마커를 지워 raw 로 되돌린 뒤 재변환 큐잉
    if retry:
        try: os.remove(_failed_marker_path(rec_dir, seq, slot))
        except OSError: pass

    # _build_segments로 병합된 세그먼트 데이터 사용
    seg = _find_seg(rec_dir, seq)
    if not seg:
        return HandlerResult(status=404, body={'error': 'Segment not found'})

    status = _ensure_segment_ready(rec_dir, seg, slot)

    if status == 'recording':
        return HandlerResult(status=202, body={'status': 'recording', 'message': '녹취 진행 중'})
    if status == 'transcoding':
        return HandlerResult(status=202, body={'status': 'transcoding', 'message': '변환 중'})
    if status == 'failed':
        reason = (seg.get('status_reason') if slot is None else '') or _read_failed_reason(rec_dir, seq, slot)
        return HandlerResult(status=500, body={'status': 'failed', 'reason': reason,
                                               'message': f'재생 불가 — {reason}'})

    # MP4 변환 파일 확인
    conv = _converted_path_mp4(rec_dir, seq, slot)
    if not os.path.exists(conv):
        return HandlerResult(status=404, body={'error': 'File not found'})

    ct = 'video/mp4' if _slot_has_video(seg, slot) else 'audio/mp4'

    return HandlerResult(status=200, body=conv, headers={
        'Content-Type': ct, 'X-File-Path': conv
    })


async def _stream_peaks(base: str, rel_dir: str, seq: int, slot=None) -> HandlerResult:
    """파형 피크 배열 — 변환 산출물과 함께 만들어지므로 변환을 먼저 보장한다.
    미변환이면 202(변환중) — 콘솔은 오디오와 같은 폴링 규약으로 기다린다."""
    d = os.path.join(base, rel_dir)
    rec_dir = _find_rec_dir(d)

    peaks = _peaks_path(rec_dir, seq, slot)
    if os.path.exists(peaks):
        data = _read_json(peaks) or {}
        return HandlerResult(status=200, body={'seq': seq, 'slot': slot, **data})

    seg = _find_seg(rec_dir, seq)
    if not seg:
        return HandlerResult(status=404, body={'error': 'Segment not found'})

    status = _ensure_segment_ready(rec_dir, seg, slot)
    if status in ('recording', 'transcoding', 'raw'):
        return HandlerResult(status=202, body={'status': status, 'message': '변환 중'})
    if status == 'failed':
        reason = _read_failed_reason(rec_dir, seq, slot)
        return HandlerResult(status=500, body={'status': 'failed', 'reason': reason})

    # 변환은 끝났는데 피크가 없다 — 이 변환본은 피크 생성 이전(구 캐시)이다.
    return HandlerResult(status=404, body={'error': 'No waveform for this recording',
                                           'message': '이 녹취는 파형 없이 변환되었습니다'})


async def _stream_whole_audio(base: str, rel_dir: str) -> HandlerResult:
    """첫 번째 세그먼트의 오디오를 스트리밍 (레거시 호환)"""
    d = os.path.join(base, rel_dir)
    rec_dir = _find_rec_dir(d)

    segs = _read_jsonl(os.path.join(rec_dir, 'segments.jsonl'))
    if not segs:
        return HandlerResult(status=404, body={'error': 'No segments'})

    return await _stream_segment(base, rel_dir, segs[0].get('seq', 1))


CIMS_RECORDING_HANDLER_LIST = [
    (_REC_BASE, handle_recordings, {}),
]
