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
import logging
from urllib.parse import urlparse, parse_qs, unquote
from pathlib import PurePath

from httpsrv.handler import HandlerArgs, HandlerResult

logger = logging.getLogger(__name__)

# 변환 중인 작업 추적 (중복 변환 방지)
_transcoding_locks = {}
_transcoding_mutex = threading.Lock()

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


def init(service_log_dir: str = '', ffmpeg_bin: str = ''):
    global _service_log_dir, _FFMPEG
    _service_log_dir = service_log_dir
    _FFMPEG = _resolve_ffmpeg(ffmpeg_bin)
    if _FFMPEG == 'ffmpeg' and not shutil.which('ffmpeg'):
        logger.warning("ffmpeg 변환툴을 찾지 못함 — 녹취 재생(raw RTP→mp4 변환) 불가. "
                       "OAM 패키지에 vendor ffmpeg 동봉 또는 시스템 설치 필요.")


# ══════════════════════════════════════════════════════════════
#  RTP → codec 추출 유틸
# ══════════════════════════════════════════════════════════════

def _strip_rtp_to_amrwb(raw_rtp_path: str, out_amr_path: str) -> bool:
    """raw RTP 파일에서 AMR-WB payload 추출.
    raw 형식: [uint32 len][int64 recv_usec][RTP pkt] 반복
    PT=99(AMR-WB)만 추출, DTMF(101)/PCMU(0) 등은 스킵.
    """
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
                # PT 필터: AMR-WB(99)만 처리, DTMF(101)/PCMU(0) 등 스킵
                pt = pkt[1] & 0x7F
                if pt != 99:
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
                # AMR-WB RTP: CMR(1) + ToC(1) + frame_data
                if len(payload) > 2:
                    toc = payload[1]
                    ft = (toc >> 3) & 0x0F
                    # storage ToC: F(0)|FT(4)|Q(1)|pad(2)
                    q = (toc >> 2) & 0x01
                    amr_toc = (ft << 3) | (q << 2)
                    fout.write(bytes([amr_toc]))
                    fout.write(payload[2:])  # frame data (CMR, ToC 제외)
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

        # has_video: va/vb 세그먼트 파일 존재 여부
        has_video = any(
            os.path.exists(os.path.join(d, f))
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
    """PTT 세션 디렉터리를 스캔하여 녹취 목록 반환.
    디렉터리: {base}/ptt/{groupId}/sessions/{key}.d/
    """
    ptt_root = os.path.join(base, 'ptt')
    if not os.path.isdir(ptt_root):
        return [], 0

    dirs = sorted(glob.glob(os.path.join(ptt_root, '**', 'sessions', '*.d'), recursive=True), reverse=True)

    results = []
    for d in dirs:
        sess_json = os.path.join(d, 'session.json')
        meta = _read_json(sess_json) if os.path.exists(sess_json) else {}
        if meta is None:
            meta = {}

        # segments.jsonl 위치: recordings/ 하위 또는 세션 디렉터리 직접
        rec_dir = os.path.join(d, 'recordings')
        if not os.path.exists(os.path.join(rec_dir, 'segments.jsonl')):
            rec_dir = d  # 세그먼트가 세션 디렉터리에 직접 있는 경우

        segs = _read_jsonl(os.path.join(rec_dir, 'segments.jsonl'))
        # session.json도 segments.jsonl도 없으면 스킵
        if not meta and not segs:
            continue

        # 그룹 ID: session.json에서 또는 디렉터리 경로에서 추출
        gid = meta.get('group_id', '')
        if not gid:
            # 경로에서 추출: .../ptt/{groupId}/sessions/...
            parts = d.split(os.sep)
            try:
                sess_idx = parts.index('sessions')
                gid = parts[sess_idx - 1] if sess_idx > 0 else ''
            except ValueError:
                gid = ''

        if group_id and gid != group_id:
            continue
        if caller and caller != meta.get('initiator', ''):
            continue
        start = meta.get('start_time', '')
        # session.json 없을 때 첫 세그먼트 시작 시간 사용
        if not start and segs:
            start = segs[0].get('start_time', '')
        if from_dt and start and start[:10] < from_dt:
            continue
        if to_dt and start and start[:10] > to_dt:
            continue
        seg_count = len(segs)
        total_ms = sum(s.get('duration_ms', 0) for s in segs)

        status = _session_status(rec_dir, segs)

        results.append({
            'dir': d,
            'call_type': 'ptt',
            'caller': meta.get('initiator', ''),
            'callee': None,
            'group_id': gid,
            'start_time': start,
            'end_time': meta.get('end_time'),
            'duration': 0,
            'has_video': False,
            'status': status,
            'segment_count': seg_count,
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
#  세그먼트 상태 판정 + on-demand 변환
# ══════════════════════════════════════════════════════════════

def _segment_status(rec_dir: str, seg: dict) -> str:
    """개별 세그먼트 상태: recording / raw / transcoding / ready"""
    # 대표 오디오 파일 (병합 전/후 모두 대응)
    audio_file = (seg.get('audio_file', '') or seg.get('audio_file_a', '')
                  or seg.get('_audio_a', '') or seg.get('_audio', ''))
    seq = seg.get('seq', 0)

    # seq 기반 MP4 변환 파일 확인 (최우선)
    if seq > 0:
        mp4 = _converted_path_mp4(rec_dir, seq)
        if os.path.exists(mp4):
            return 'ready'
        if os.path.exists(mp4 + '.transcoding'):
            return 'transcoding'

    if not audio_file:
        return 'raw'

    full = os.path.join(rec_dir, audio_file)

    # .recording 존재 → 녹취 중
    if os.path.exists(full + '.recording'):
        return 'recording'

    # raw만 존재
    if os.path.exists(full):
        return 'raw'

    return 'raw'


def _transcode_segment_file(rec_dir: str, seg: dict):
    """세그먼트의 raw RTP를 변환 (백그라운드 스레드). 출력: seg_NNNN.mp4 (통일)

    VoIP 음성:  A + B 음성 mixing → MP4 (AAC)
    VoIP 영상:  A + B 음성 mixing + A + B 영상 좌우 배치 → MP4
    PTT:        단일 음성 → MP4 (AAC)
    """
    seg_type = seg.get('type', seg.get('_type', 'ptt'))
    seq = seg.get('seq', 0)
    out_path = _converted_path_mp4(rec_dir, seq)

    # raw 파일 경로 결정
    if seg_type == 'ptt':
        raw_a = seg.get('_audio', seg.get('audio_file', ''))
        if raw_a: raw_a = os.path.join(rec_dir, raw_a)
        raw_b = ''
        raw_va = seg.get('_video', seg.get('video_file', ''))
        if raw_va: raw_va = os.path.join(rec_dir, raw_va)
        raw_vb = ''
    else:
        raw_a = seg.get('_audio_a', '')
        if raw_a: raw_a = os.path.join(rec_dir, raw_a)
        raw_b = seg.get('_audio_b', '')
        if raw_b: raw_b = os.path.join(rec_dir, raw_b)
        raw_va = seg.get('_video_a', '')
        if raw_va: raw_va = os.path.join(rec_dir, raw_va)
        raw_vb = seg.get('_video_b', '')
        if raw_vb: raw_vb = os.path.join(rec_dir, raw_vb)

    # 최소 하나의 오디오 파일 필요
    primary = raw_a or raw_b
    if not primary or not os.path.exists(primary):
        return

    # .transcoding 마커
    marker = out_path + '.transcoding'
    try: open(marker, 'w').close()
    except: pass

    tmp_files = []

    try:
        # 1) 오디오 추출: AMR-WB
        amr_a = primary + '.amr_a'
        amr_b = (raw_b + '.amr_b') if raw_b and os.path.exists(raw_b) else ''
        has_a = _strip_rtp_to_amrwb(raw_a, amr_a) if raw_a and os.path.exists(raw_a) else False
        has_b = _strip_rtp_to_amrwb(raw_b, amr_b) if amr_b else False
        if has_a: tmp_files.append(amr_a)
        if has_b: tmp_files.append(amr_b)

        # 2) 영상 추출: H.264
        h264_a = (raw_va + '.h264_a') if raw_va and os.path.exists(raw_va) and os.path.getsize(raw_va) > 0 else ''
        h264_b = (raw_vb + '.h264_b') if raw_vb and os.path.exists(raw_vb) and os.path.getsize(raw_vb) > 0 else ''
        has_va = _strip_rtp_to_h264(raw_va, h264_a) if h264_a else False
        has_vb = _strip_rtp_to_h264(raw_vb, h264_b) if h264_b else False
        if has_va: tmp_files.append(h264_a)
        if has_vb: tmp_files.append(h264_b)

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
                out_path,
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
                out_path,
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
                out_path,
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
                out_path,
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
                out_path,
            ]
            subprocess.run(cmd, capture_output=True, timeout=120)

    except Exception as e:
        logger.error("transcode seg %d error: %s", seq, e)
    finally:
        for tmp in tmp_files:
            try: os.remove(tmp)
            except: pass
        try: os.remove(marker)
        except: pass

    lock_key = f"{rec_dir}:{seq}"
    with _transcoding_mutex:
        _transcoding_locks.pop(lock_key, None)


def _ensure_segment_ready(rec_dir: str, seg: dict) -> str:
    """세그먼트 변환 보장. 상태 문자열 반환. seg는 _build_segments()의 병합된 dict."""
    status = seg.get('status', _segment_status(rec_dir, seg))

    if status in ('recording', 'ready', 'transcoding'):
        return status

    if status == 'raw':
        seq = seg.get('seq', 0)
        lock_key = f"{rec_dir}:{seq}"
        with _transcoding_mutex:
            if lock_key in _transcoding_locks:
                return 'transcoding'
            _transcoding_locks[lock_key] = True
        t = threading.Thread(target=_transcode_segment_file, args=(rec_dir, seg), daemon=True)
        t.start()
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
        has_video = s.get('has_video', False)

        # 대표 오디오 파일 (상태 판정용)
        primary_audio = (s.get('audio_file_a', '') or s.get('audio_file', '')
                         or s.get('audio_file_b', ''))

        status = _segment_status(rec_dir, {'audio_file': primary_audio, 'seq': seq})

        file_size = 0
        conv = _converted_path_mp4(rec_dir, seq)
        if os.path.exists(conv):
            file_size = os.path.getsize(conv)

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
            # 파일 참조 (변환에서 사용)
            '_audio_a': s.get('audio_file_a', ''),
            '_audio_b': s.get('audio_file_b', ''),
            '_audio': s.get('audio_file', ''),
            '_video_a': s.get('video_file_a', ''),
            '_video_b': s.get('video_file_b', ''),
            '_video': s.get('video_file', ''),
        })
    return result


def _converted_path_mp4(rec_dir: str, seq: int) -> str:
    """변환된 MP4 파일 경로"""
    return os.path.join(rec_dir, f"seg_{seq:04d}.mp4")


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
    # 패턴 1: .../segments/{seq}/audio|video
    if len(parts) >= 3 and parts[-1] in ('audio', 'video'):
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
    parsed = urlparse(handler_args.full_path)
    qs = parse_qs(parsed.query)
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
                video = (extra[1] == 'video')
                return await _stream_segment(base, session_dir, seq, video=video)

        elif action == 'audio' and method == 'GET':
            return await _stream_whole_audio(base, session_dir)

        return HandlerResult(status=404, body={'error': 'Not Found'})
    except Exception as e:
        logger.error("handle_recordings error: %s", e)
        return HandlerResult(status=500, body={'error': str(e)})


async def _list_recordings(base: str, qs: dict) -> HandlerResult:
    def qp(name, default=None):
        vals = qs.get(name)
        return unquote(vals[0]) if vals else default

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
    rec['segments'] = segs
    rec['segment_count'] = len(segs)
    rec['total_speech_ms'] = sum(s['duration_ms'] for s in segs)
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
    return HandlerResult(status=200, body={'id': rel_dir, 'segments': segs})


async def _stream_segment(base: str, rel_dir: str, seq: int, video: bool = False) -> HandlerResult:
    d = os.path.join(base, rel_dir)
    rec_dir = _find_rec_dir(d)

    # _build_segments로 병합된 세그먼트 데이터 사용
    built_segs = _build_segments(rec_dir)
    seg = None
    for s in built_segs:
        if s.get('seq') == seq:
            seg = s
            break
    if not seg:
        return HandlerResult(status=404, body={'error': 'Segment not found'})

    status = _ensure_segment_ready(rec_dir, seg)

    if status == 'recording':
        return HandlerResult(status=202, body={'status': 'recording', 'message': '녹취 진행 중'})
    if status == 'transcoding':
        return HandlerResult(status=202, body={'status': 'transcoding', 'message': '변환 중'})

    # MP4 변환 파일 확인
    conv = _converted_path_mp4(rec_dir, seq)
    if not os.path.exists(conv):
        return HandlerResult(status=404, body={'error': 'File not found'})

    ct = 'video/mp4' if seg.get('has_video') else 'audio/mp4'

    return HandlerResult(status=200, body=conv, headers={
        'Content-Type': ct, 'X-File-Path': conv
    })


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
