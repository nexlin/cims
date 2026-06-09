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
import time as _time
from datetime import datetime, timedelta
from urllib.parse import urlparse, parse_qs, unquote

from httpsrv.handler import HandlerArgs, HandlerResult

logger = logging.getLogger(__name__)

_calls_dir: str = ""
_sip_log_dir: str = ""
_msg_log_dir: str = ""
_system_id: str = "csp_01"
_db_config: dict = None   # 호이력 부서/검색 필터(가입자→부서 매핑)용. OAM 컨텍스트에서만 주입.
_trusted_nets: list = []  # 우리 서비스/관리 망 CIDR(ip_network) — 비정상 세션 '외부' 판정 제외.


def init(service_log_dir: str, sip_log_dir: str = "",
         msg_log_dir: str = "", system_id: str = "csp_01", db_config: dict = None,
         trusted_nets: list = None) -> None:
    """ServiceLogging Dir 설정 (통합 디렉토리). db_config 주입 시 호이력 부서/이름 필터 활성화.
    trusted_nets: 우리 서비스/관리 망 CIDR 목록 — 비정상 세션 탐지에서 '외부' 제외(오탐 방지)."""
    global _calls_dir, _sip_log_dir, _msg_log_dir, _system_id, _db_config, _trusted_nets
    _calls_dir = service_log_dir if service_log_dir else ""
    _sip_log_dir = sip_log_dir if sip_log_dir else _calls_dir
    _msg_log_dir = msg_log_dir if msg_log_dir else _calls_dir
    _system_id = system_id if system_id else "csp_01"
    _db_config = db_config
    nets = []
    import ipaddress as _ip
    for c in (trusted_nets or []):
        try:
            nets.append(_ip.ip_network(c, strict=False))
        except Exception:
            pass
    _trusted_nets = nets


def _db_conn():
    """호이력 필터용 DB 연결 (없으면 None → 부서/이름 필터 비활성, 번호 substring 만)."""
    if not _db_config:
        return None
    try:
        import pymysql
        import pymysql.cursors
        return pymysql.connect(
            host=_db_config.get('Host', '127.0.0.1'),
            port=int(_db_config.get('Port', 3306)),
            user=_db_config.get('User', 'root'),
            password=_db_config.get('Password', ''),
            database=_db_config.get('Db', 'cims'),
            charset='utf8mb4',
            cursorclass=pymysql.cursors.DictCursor,
            autocommit=True,
        )
    except Exception:
        return None


def _resolve_volte_msisdns(org: str = None, q: str = None):
    """부서(org, 하위 전체 포함) 또는 검색어(q=이름/번호)에 매칭되는 VoLTE 가입자 msisdn 집합.
       org/q 둘 다 없으면 None(필터 없음). DB 미연결 시 None."""
    org = (org or '').strip()
    q = (q or '').strip()
    if not org and not q:
        return None
    conn = _db_conn()
    if conn is None:
        return None
    try:
        with conn:
            with conn.cursor() as cur:
                where, params = [], []
                if org:
                    # users.org_id 는 leaf 팀코드 → 선택 부서의 모든 하위 코드로 확장
                    cur.execute("SELECT id, code, parent_id FROM organizations")
                    rows = cur.fetchall()
                    id2code = {r['id']: r['code'] for r in rows}
                    children = {}
                    for r in rows:
                        children.setdefault(id2code.get(r['parent_id']), []).append(r['code'])
                    desc, stack = [], [org]
                    while stack:
                        x = stack.pop()
                        desc.append(x)
                        stack.extend(children.get(x, []))
                    ph = ','.join(['%s'] * len(desc))
                    where.append(f"u.org_id IN ({ph})")
                    params.extend(desc)
                if q:
                    where.append("(u.name LIKE %s OR vs.id LIKE %s)")
                    params.extend([f"%{q}%", f"%{q}%"])
                sql = ("SELECT vs.id AS msisdn FROM users u "
                       "JOIN volte_subscriptions vs ON vs.user_id=u.id")
                if where:
                    sql += " WHERE " + " AND ".join(where)
                cur.execute(sql, params)
                return {r['msisdn'] for r in cur.fetchall() if r.get('msisdn')}
    except Exception:
        return None


def _live_call_ids() -> set:
    """현재 라이브 활성 호 call_id 집합. CSP 가 {ServiceLogDir}/state/{volte,ptt}/*.json 에
       원자 쓰기로 관리하고 호 종료 시 제거한다. 여기 없으면 '활성 아님'(stale)으로 판정."""
    out = set()
    if not _calls_dir:
        return out
    for kind in ("volte", "ptt"):
        for fp in _glob.glob(os.path.join(_calls_dir, "state", kind, "*.json")):
            if fp.endswith(".tmp"):
                continue
            try:
                with open(fp) as f:
                    cid = json.load(f).get("call_id")
                    if cid:
                        out.add(cid)
            except Exception:
                pass
    return out


def _parse_date(s: str) -> str:
    """YYYY-MM-DD 또는 YYYYMMDD → YYYY-MM-DD"""
    s = s.replace("-", "")
    if len(s) == 8:
        return f"{s[:4]}-{s[4:6]}-{s[6:8]}"
    return datetime.now().strftime("%Y-%m-%d")


def _date_parts(date_str: str):
    d = _parse_date(date_str)
    return d[:4], d[5:7], d[8:10]


# ── 선택 호 시간창 → 5분 버킷 스코프 ──────────────────────────────
#  선택된 호는 자기 위치를 안다: .d 경로가 YYYY/MM/DD/HH 를, call.json 이
#  invite_time/end_time 을 가짐 → 읽을 5분 버킷(mm5)을 정확히 도출.
#  이를 readers 에 넘겨 하루 24시간(수백 파일) 스캔 대신 해당 1~수개 버킷만 읽는다.
#  (사용자 요청: "호 시각 → 5분 버킷 직접 타겟". hour 파라미터 불필요화.)
def _parse_log_dt(s: str):
    """ISO(2026-06-06T23:34:26[.us]) 또는 공백 구분 → datetime. 실패 시 None."""
    if not s or not isinstance(s, str):
        return None
    s = s.strip().replace("T", " ")
    s = s.split("+")[0].split("Z")[0].strip()  # tz 제거
    for fmt in ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S"):
        try:
            return datetime.strptime(s[:26], fmt)
        except Exception:
            continue
    return None

def _window_scope(call_json: dict):
    """call.json 의 invite_time~end_time(+여유) 가 걸치는 (date,hour,buckets) 스코프.

    반환: [{"date":"YYYY-MM-DD","hour":"HH","buckets":{"30","35",...}}, ...]
    실패/과도(>6h, 비정상 호 방어) 시 None → 호출측이 legacy(hour/24h) 로 폴백.
    """
    if not isinstance(call_json, dict):
        return None
    start = _parse_log_dt(call_json.get("invite_time") or call_json.get("start_time"))
    if not start:
        return None                      # 시작 시각 없으면 스코프 불가 → legacy
    end = _parse_log_dt(call_json.get("end_time"))
    if not end:
        # 종료시각 없음(진행 중·미완 호): duration 있으면 사용, 없으면 1h bounded 폴백.
        #   (start-only 면 장기 호 메시지 누락, 24h 면 느림 → 1h 가 절충: 정상 호 전부 포함 + 12× 축소)
        dur = call_json.get("duration")
        if isinstance(dur, (int, float)) and dur > 0:
            end = start + timedelta(seconds=min(dur, 6 * 3600))
        else:
            end = start + timedelta(hours=1)
    if end < start:
        end = start
    end = end + timedelta(seconds=90)   # BYE/200 OK 지연 + 다음 버킷 경계 여유
    if (end - start).total_seconds() > 6 * 3600:
        return None                      # 비정상적으로 긴 창 → legacy(안전)
    scope: dict = {}
    cur = start.replace(minute=(start.minute // 5) * 5, second=0, microsecond=0)
    step = timedelta(minutes=5)
    guard = 0
    while cur <= end and guard < 200:
        key = (cur.strftime("%Y-%m-%d"), cur.strftime("%H"))
        scope.setdefault(key, set()).add(cur.strftime("%M"))
        cur += step; guard += 1
    return [{"date": d, "hour": h, "buckets": b} for (d, h), b in scope.items()]


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
            # voip 고정 깊이: {HH}/{prefix}/{caller}/{call_id}.d — 재귀 `**` 대신 고정 `*` 글롭으로
            #   전체 트리 walk 회피(수백 호에서 수초→수십ms). hour 지정 시 {prefix}/{caller}/*.d.
            pat = os.path.join(base, "*", "*", "*.d") if hour else os.path.join(base, "*", "*", "*", "*.d")
            result.extend(_glob.glob(pat))
        else:
            # ptt: 레이아웃 가변 → 재귀 glob 유지 (호환).
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


def _resolve_flow_paths(date_str: str, hour: str, service: str, scope: list = None) -> list:
    """통합 flow.jsonl 경로 목록 반환 (모든 노드)

    New: {ServiceLogDir}/YYYY/MM/DD/HH/{node_id}.flow.jsonl (csp_01.flow.jsonl, cmp_01.flow.jsonl)
    Legacy: {ServiceLogDir}/YYYY/MM/DD/HH/{system_id}_{service}.flow.jsonl
    scope 주어지면 [{date,hour,buckets}] 의 해당 5분 버킷 flow 파일만 (선택 호 정밀 조회).
    """
    if not _sip_log_dir and not _calls_dir:
        return []
    # 읽을 (yyyy,mm,dd,hh, buckets) 목록
    slots = []
    if scope:
        for ent in scope:
            y, m, d = _date_parts(ent["date"])
            slots.append((y, m, d, ent["hour"].zfill(2), ent.get("buckets")))
    else:
        yyyy, mm, dd = _date_parts(date_str)
        hours = [hour.zfill(2)] if hour else [f"{h:02d}" for h in range(24)]
        for hh in hours:
            slots.append((yyyy, mm, dd, hh, None))
    paths = []
    for yyyy, mm, dd, hh, buckets in slots:
        base_dir = os.path.join(_calls_dir, yyyy, mm, dd, hh) if _calls_dir else ""

        if base_dir:
            # 1) New 통합: {node_id}.flow.jsonl (와일드카드로 모든 노드)
            #    + 5분 버킷 파일 {node_id}.flow.{mm5}.jsonl (open-per-write 전환) 도 함께 수집.
            import glob
            if buckets:
                bucket_files = []
                for b in sorted(buckets):
                    bucket_files += glob.glob(os.path.join(base_dir, f"*.flow.{b}.jsonl"))
                found_new = sorted(set(bucket_files +
                                       glob.glob(os.path.join(base_dir, "*.flow.jsonl"))))
            else:
                found_new = sorted(glob.glob(os.path.join(base_dir, "*.flow.jsonl")) +
                                   glob.glob(os.path.join(base_dir, "*.flow.[0-9][0-9].jsonl")))
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
                         node: str = "", minute=None) -> str:
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
        # 5분 버킷(open-per-write) 파일: seq 가 버킷별로 리셋되므로 해당 메시지의 minute 으로 정확한 파일 선택.
        if minute is not None:
            try:
                mm5 = "%02d" % ((int(minute) // 5) * 5)
                if node:
                    patterns.append(f"{node}_*_{iface}.msg.{mm5}.jsonl")
                    patterns.append(f"{node}_{iface}.msg.{mm5}.jsonl")
                patterns.append(f"*_{iface}.msg.{mm5}.jsonl")
            except (TypeError, ValueError):
                pass
        if node:
            # node 기준 정확한 매칭 (csp/cmp/csc 중 하나) — legacy(비버킷) 파일
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


def _msg_globs_for(base: str, buckets=None) -> list:
    """base 디렉터리에서 읽을 sip msg 파일 목록.
    buckets(mm5 set) 주어지면 해당 5분 버킷 파일만(+legacy 무버킷), 아니면 시간 전체.
    """
    pats = []
    if buckets:
        for b in sorted(buckets):
            pats.append(f"*_sip.msg.{b}.jsonl")
        pats += ["*_sip.msg.jsonl", "*_sip.jsonl"]   # 구 무버킷(시간당) 파일도 포함
    else:
        pats = ["*_sip.msg.jsonl", "*_sip.msg.[0-9][0-9].jsonl", "*_sip.jsonl"]
    out = []
    for pat in pats:
        out.extend(_glob.glob(os.path.join(base, pat)))
    return sorted(set(out))

def _extract_sesids_from_msg_jsonl(call_ids: list, date_str: str, hour: str = None,
                                   scope: list = None) -> set:
    """sip msg.jsonl 의 raw SIP 메시지에서 Call-ID 가 매칭되는 라인의 sesid 추출.

    flow.jsonl 의 SIP 라인에는 call_id/subid 가 없으므로 (caller/callee/sesid/method 만),
    raw SIP body 가 들어있는 msg.jsonl 에서 Call-ID 매칭 후 sesid 모음. 이후 sesid 기반
    으로 flow.jsonl 필터링 → VoLTE 호의 다른 PTT 메시지 섞임 방지.

    scope 주어지면 [{date,hour,buckets}] 의 해당 5분 버킷 파일만 읽음 (선택 호 정밀 조회).
    """
    if not _calls_dir or not call_ids:
        return set()
    sesids: set = set()
    # 읽을 (base_dir, buckets) 목록 구성
    targets = []
    if scope:
        for ent in scope:
            yyyy, mm, dd = _date_parts(ent["date"])
            base = os.path.join(_calls_dir, yyyy, mm, dd, ent["hour"].zfill(2))
            targets.append((base, ent.get("buckets")))
    else:
        yyyy, mm, dd = _date_parts(date_str)
        hours = [hour.zfill(2)] if hour else [f"{h:02d}" for h in range(24)]
        for hh in hours:
            targets.append((os.path.join(_calls_dir, yyyy, mm, dd, hh), None))
    for base, buckets in targets:
        if not os.path.isdir(base):
            continue
        for path in _msg_globs_for(base, buckets):
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
                         sesid_set: set = None, scope: list = None) -> list:
    """서비스별 flow.jsonl 에서 SIP 메시지 검색 (compact, body 없음).

    flow.jsonl 의 SIP 라인에는 Call-ID 필드가 없고 sesid/caller/callee/method 만 있음.
    `sesid_set` 가 주어지면 그것으로 매칭 (정확). 그렇지 않으면 substring fallback.
    scope: 선택 호 5분 버킷 정밀 조회용.
    """
    if not _sip_log_dir:
        return []

    results = []
    call_id_set = set(call_ids or [])
    flow_paths = _resolve_flow_paths(date_str, hour, service, scope=scope)

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
                         sesid_set: set = None, scope: list = None) -> list:
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
    flow_paths = _resolve_flow_paths(date_str, hour, service, scope=scope)

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

    # 선택 호 정밀 조회: call.json 시간창(invite_time~end_time)으로 읽을 5분 버킷 스코프 도출.
    #   → hour 파라미터/24h 스캔 불필요(호 자체가 위치를 결정). 도출 실패 시 None(legacy 폴백).
    scope = _window_scope(call_json) if call_json else None

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
    sesid_set = _extract_sesids_from_msg_jsonl(call_ids, date_str, hour, scope=scope)

    # SIP 메시지 검색 (sesid 매칭 우선, fallback substring)
    sip_msgs = _search_sip_messages(call_ids, date_str, hour,
                                     service="volte", sesid_set=sesid_set, scope=scope)

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
                                     sesid_set=sesid_set if sesid_set else None, scope=scope)

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

# ── 호이력 경량 목록 캐시 ────────────────────────────────────────────
# call/logs 가 하루 전체를 스캔(필터·정렬·히트맵)하므로, 매 페이지 이동/필터마다 재스캔하면
# 수백 호 × 파일I/O 로 수초씩 걸린다. (1) .d 디렉터리 glob 을 1회만 수행해 basename→path 맵으로
# O(1) 해소(구: index 항목마다 전체 트리 재-glob → O(N²)), (2) participants/has_recording 는
# 목록 단계에서 생략하고 paged 슬라이스에만 부착, (3) 경량 목록(call.json 코어 필드)을 짧은 TTL 로
# 캐시 → 페이지/필터 재요청을 즉시 응답. 프론트 store 캐시와 함께 백엔드 연산 최소화.
_calllog_cache: dict = {}     # (date_str, call_type) → (mono_ts, [lightweight call.json + dir_name])
_CALLLOG_TTL = 4.0            # 초 — 라이브 갱신성과 재스캔 비용의 절충(과거 날짜도 동일; 충분히 신선)


def _calllog_list(date_str: str, call_type: str) -> list:
    """하루치 경량 호이력 목록(call.json 코어 + dir_name). participants/has_recording 미포함.
    .d 디렉터리 glob 1회 + 캐시. 호출자는 반환 리스트를 변형하지 말 것(캐시 공유) — 슬라이스 후 copy."""
    key = (date_str, call_type or "")
    now = _time.monotonic()
    hit = _calllog_cache.get(key)
    if hit and (now - hit[0]) < _CALLLOG_TTL:
        return hit[1]

    index_entries = _load_index(date_str, None)
    # glob 1회 → basename → fullpath 맵 (index 항목 O(1) 해소, 구 per-call 전체-트리 glob 제거)
    all_dirs = _find_all_d_dirs(date_str, None, call_type)
    by_base = {os.path.basename(d): d for d in all_dirs}

    # 로드할 .d 경로 확정
    if index_entries:
        d_dirs = []
        for entry in index_entries:
            dir_name = entry.get('dir')
            if not dir_name:
                continue
            d = by_base.get(dir_name) or by_base.get(dir_name if dir_name.endswith('.d') else dir_name + '.d')
            if d:
                d_dirs.append(d)
    else:
        d_dirs = all_dirs

    # call.json 병렬 로드 — NFS 수백~수천 호의 직렬 read 가 cold 지연의 주원인.
    #   스레드풀로 동시 read(파일I/O 는 GIL 영향 적음) → 직렬 대비 10~수십배.
    def _one(d):
        cj = _load_call_json(d)
        if not cj:
            return None
        cj['dir_name'] = os.path.relpath(d, _calls_dir) if _calls_dir else os.path.basename(d).replace('.d', '')
        return cj

    logs = []
    if d_dirs:
        from concurrent.futures import ThreadPoolExecutor
        workers = min(32, max(4, len(d_dirs)))
        with ThreadPoolExecutor(max_workers=workers) as ex:
            for cj in ex.map(_one, d_dirs):
                if cj:
                    logs.append(cj)

    _calllog_cache[key] = (now, logs)
    return logs


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
    org = _q("org")
    q_search = _q("q")
    limit = min(int(_q("limit", "200")), 1000)
    offset = int(_q("offset", "0"))

    # 시간대 히트맵(hours)은 hour 필터와 무관하게 하루 전체를 집계해야 하므로,
    # 스캔은 항상 하루 전체로 하고 hour 필터는 집계 이후 paged 목록에만 적용한다.
    # 경량 목록(캐시·glob 1회) — participants/has_recording 는 무겁고 표시(paged)에만 필요하므로
    #   목록 단계에서 생략하고 paged 슬라이스에서만 부착(수백 호 × 파일I/O 회피).
    #   (캐시 dict 는 공유되므로 항목별 dict copy 후 변형.)
    logs = [dict(l) for l in _calllog_list(date_str, call_type)]

    # msisdn/org/q 필터는 participants 매칭이 필요 → 해당 필터가 있을 때만 일괄 로드.
    need_participants = bool(msisdn or org or q_search)
    if need_participants:
        for l in logs:
            dn = l.get('dir_name')
            l['participants'] = _load_participants(os.path.join(_calls_dir, dn)) if (dn and _calls_dir) else []

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

    # 부서(org) 필터 — 선택 부서(하위 포함) 가입자가 발/착신/참여한 호만
    if org:
        org_set = _resolve_volte_msisdns(org=org, q=None)
        if org_set is not None:
            logs = [l for l in logs if l.get('initiator') in org_set or l.get('callee') in org_set or
                    any(p.get('msisdn') in org_set for p in l.get('participants', []))]
    # 검색어(q) 필터 — 번호 substring(외부번호 포함) OR 이름/번호 DB 매칭(가입자)
    if q_search:
        q_set = _resolve_volte_msisdns(org=None, q=q_search)

        def _match_q(l):
            if q_search in (l.get('initiator') or '') or q_search in (l.get('callee') or ''):
                return True
            if q_set and (l.get('initiator') in q_set or l.get('callee') in q_set or
                          any(p.get('msisdn') in q_set for p in l.get('participants', []))):
                return True
            return False
        logs = [l for l in logs if _match_q(l)]

    # 미종료(stale) 호 보정 — 종료로그가 남지 않아 state=active/ringing 으로 남은 호를,
    # 현재 라이브 활성 집합(state/*.json)에 없으면 '비정상 종료(기록 없음)'로 표시.
    # (디스크 원본은 변경하지 않는 read-time 보정 → 자기치유적, 안전.)
    # ★ 시간 임계값을 쓰지 않으므로 통화시간이 긴(장시간) 호도 오판하지 않는다:
    #    라이브 상태파일이 존재하는 한(=CSP 가 BYE 전까지 유지) 'active' 그대로 유지된다.
    #    B2BUA 는 caller/callee leg 의 call_id 가 다르므로(동일 session_id),
    #    call.json.call_id 뿐 아니라 session.json 의 양 leg call_id 까지 대조해 누락을 막는다.
    live_ids = _live_call_ids()
    for l in logs:
        if l.get('state') in ('active', 'ringing') and not l.get('end_time'):
            cand = {l.get('call_id')}
            dn = l.get('dir_name')
            if dn and _calls_dir:
                try:
                    with open(os.path.join(_calls_dir, dn, 'session.json')) as f:
                        cand.update(json.load(f).get('call_ids') or [])
                except Exception:
                    pass
            if not (cand & live_ids):
                l['state'] = 'ended'
                if not l.get('end_reason'):
                    l['end_reason'] = 'incomplete'

    # 시간대 히트맵(hour 필터 적용 전, 하루 전체 집계)
    hours_hist = {}
    for l in logs:
        h = (l.get('invite_time') or '')[11:13]
        if h:
            hours_hist[h] = hours_hist.get(h, 0) + 1

    # hour 필터 (집계 이후 — 목록에만 적용)
    if hour:
        hh = str(hour).zfill(2)
        logs = [l for l in logs if (l.get('invite_time') or '')[11:13] == hh]

    # 정렬 (최신 순)
    logs.sort(key=lambda l: l.get('invite_time', ''), reverse=True)

    total = len(logs)
    paged = logs[offset:offset + limit]

    # paged 슬라이스에만 participants/has_recording 부착(목록 전체가 아니라 표시분만 — 파일I/O 최소화)
    # + end_reason_ko 추가
    reason_map = {'normal': '정상종료', 'no_answer': '무응답', 'busy': '통화중',
                  'rejected': '거절', 'error': '오류', 'timeout': '시간초과',
                  'incomplete': '비정상 종료(기록 없음)'}
    def _enrich(l):
        dn = l.get('dir_name')
        d_dir = os.path.join(_calls_dir, dn) if (dn and _calls_dir) else None
        if 'participants' not in l:
            l['participants'] = _load_participants(d_dir) if d_dir else []
        l['has_recording'] = _has_recording(d_dir) if d_dir else False
        l['end_reason_ko'] = reason_map.get(l.get('end_reason', ''), l.get('end_reason', ''))
    if paged:
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=min(32, max(4, len(paged)))) as ex:
            list(ex.map(_enrich, paged))

    return HandlerResult(status=200, body=json.dumps({
        "total": total, "limit": limit, "offset": offset, "logs": paged,
        "hours": hours_hist,
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


def _find_ptt_sessions(group_id: str, date: str = None, days: int = None) -> list:
    """PTT 그룹의 시간창(YYYY/MM/DD/HH) 목록 반환. window dir 이름 = 'YYYYMMDDHH'.

    스캔 범위(우선순위):
      - date('YYYY-MM-DD') 지정 → 해당 일자 시간창만.
      - days(N) 지정 → 최근 N개 캘린더 일자(오늘 포함) 시간창만 (콘솔 일별 히트맵용).
      - 둘 다 미지정 → 전체(주의: 장시간 세션이면 최대 8760개).
    일자별로 디렉터리를 직접 순회해 glob 폭을 N일로 제한한다."""
    base = _ptt_group_base(group_id)
    if not base:
        return []
    import glob as _glob
    digit4, digit2 = "[0-9][0-9][0-9][0-9]", "[0-9][0-9]"
    # date 필터: 'YYYY-MM-DD' → base/YYYY/MM/DD/* 만 스캔
    day_digits = "".join(c for c in (date or "") if c.isdigit())
    patterns = []
    if len(day_digits) >= 8:
        patterns.append(os.path.join(base, day_digits[0:4], day_digits[4:6], day_digits[6:8], digit2))
    elif days and days > 0:
        # 최근 N일(오늘 포함) 각 일자 디렉터리만 glob → 범위 밖 미스캔
        from datetime import timedelta as _td
        today = datetime.now()
        for i in range(days):
            d = today - _td(days=i)
            patterns.append(os.path.join(base, d.strftime("%Y"), d.strftime("%m"), d.strftime("%d"), digit2))
    else:
        patterns.append(os.path.join(base, digit4, digit2, digit2, digit2))
    result = []
    now_window = datetime.now().strftime("%Y%m%d%H")
    hh_dirs = []
    for pat in patterns:
        hh_dirs.extend(_glob.glob(pat))
    for hh_dir in hh_dirs:
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

        # 시간창(YYYY/MM/DD/HH) 목록 스캔 (date 지정 시 해당 일자만, days 지정 시 최근 N일)
        _days = None
        try:
            _dv = _qp("days")
            if _dv:
                _days = max(1, min(90, int(_dv)))
        except (TypeError, ValueError):
            _days = None
        sessions = _find_ptt_sessions(group_id, _qp("date"), _days)

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
        # ── Flow: 해당 세션 시간버킷의 flow.jsonl 읽기 → 필터 → 노드별 배열 ──
        date_str = _qp("date", datetime.now().strftime("%Y-%m-%d"))
        # 세션 식별자 = 'YYYYMMDDHH' (시간버킷). 그 시(HH)만 스캔 — 하루 24시간×5분버킷×전노드
        #   (수백 파일) 전체를 읽던 것을 해당 시간으로 한정해 Flow 조회를 대폭 가속한다.
        _sd = "".join(c for c in session_dir if c.isdigit())
        _hour = _sd[8:10] if len(_sd) >= 10 else None
        flow_paths = _resolve_flow_paths(date_str, _hour, "ptt")

        # 세션의 시간 범위: events.jsonl에서 추출 (ISO 형식 → HH:MM:SS.ffffff)
        def _iso_to_hms(iso: str) -> str:
            if not iso: return ""
            if "T" in iso:
                return iso.split("T", 1)[1][:15]
            return iso[:15]

        d_dir = _find_ptt_session_dir(group_id, session_dir)
        # 매칭 토큰: URL 의 group_id 는 surrogate("1") 라 sesid 부분문자열 매칭 시
        #   스캔/사기 sesid(예: 0000…/9999…, 숫자 '1' 포함)에 오매칭되어 flow 가 오염된다.
        #   group.json 의 mcptt_group_id("g001", 영문 포함→숫자 sesid 와 충돌 없음)로 매칭.
        match_token = group_id
        try:
            _base = _ptt_group_base(group_id)
            if _base:
                _gj = os.path.join(_base, "group.json")
                if os.path.exists(_gj):
                    with open(_gj) as _f:
                        _mg = (json.load(_f) or {}).get("mcptt_group_id")
                        if _mg:
                            match_token = str(_mg)
        except Exception:
            pass
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
                if match_token in detail or match_token in sesid_val or match_token in subid_val:
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
                if match_token in detail or match_token in sesid_val or match_token in subid_val:
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

    # 5분 버킷(open-per-write) 파일 선택용 minute — 프론트가 메시지의 min 또는 ts(HH:MM:SS)를 전달.
    minute = None
    min_str = _qval("min", "")
    if min_str.isdigit():
        minute = int(min_str)
    else:
        ts_q = _qval("ts", "")
        if ts_q and ":" in ts_q:
            parts = ts_q.split(":")
            if len(parts) >= 2 and parts[1].isdigit():
                minute = int(parts[1])

    # New: seq-based lookup ({node}_*_{iface}.msg.jsonl)
    if seq_str and hour:
        try:
            seq = int(seq_str)
        except ValueError:
            seq = 0
        if seq > 0:
            body = _lookup_body_by_seq(date_str, hour, seq, iface=iface, node=node, minute=minute)
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


# ══════════════════════════════════════════════════════════════
#  비정상(이상) 세션 탐지 — 외부 SIP 스캐닝/사기 호 시도
# ══════════════════════════════════════════════════════════════
# 공개 SIP 포트(VIP)로 들어오는 인터넷발 스캔/사기 INVITE/REGISTER 를 탐지한다.
# 신호: 외부(공인) 발신 IP · 알려진 스캐너 UA · 사기성 번호 패턴 · 인증 반복실패.
# 정상 동작(CSP 가 401 로 거부)이지만 로그를 오염시키고 자원을 소모 → 가시화/대응.

import ipaddress as _ipaddr

# 알려진 SIP 스캐너/공격툴 User-Agent (소문자 부분일치)
_SCANNER_UAS = (
    'pplsip', 'friendly-scanner', 'sipvicious', 'sipcli', 'sundayddr',
    'vaxsipuseragent', 'sip-scan', 'sipsak', 'smap', 'iwar', 'sippts',
    'suetv', 'gulp', 'cseq', 'nmap',
)


def _ip_is_public(ip: str) -> bool:
    """공인(글로벌) IP 여부. 사설(10/172.16/192.168)·루프백·링크로컬은 False."""
    try:
        a = _ipaddr.ip_address(ip)
        return not (a.is_private or a.is_loopback or a.is_link_local
                    or a.is_multicast or a.is_unspecified or a.is_reserved)
    except Exception:
        return False


def _ip_external(ip: str) -> bool:
    """외부(공격면) IP = 공인 IP 이면서 우리 신뢰망(_trusted_nets, 예 서비스 VIP /24)에
    속하지 않음. 우리 노드의 공인 IP(VIP·노드)는 신뢰망에 넣어 오탐(자기 트래픽)을 막는다."""
    if not _ip_is_public(ip):
        return False
    try:
        a = _ipaddr.ip_address(ip)
        for net in _trusted_nets:
            if a in net:
                return False
    except Exception:
        return False
    return True


def _first_line(raw: str) -> str:
    return (raw or '').split('\r\n', 1)[0].split('\n', 1)[0].strip()


def _extract_ua(raw: str) -> str:
    for ln in (raw or '').replace('\r\n', '\n').split('\n'):
        l = ln.strip()
        if l[:11].lower() == 'user-agent:':
            return l[11:].strip()
    return ''


def _is_fraud_number(num: str) -> bool:
    """사기성 번호 패턴: E.164 최대(15자리) 초과 숫자, 또는 0/9 의 비정상 긴 반복(9자리+).
    (정상 테스트번호 +82500000001 의 0 7연속 같은 패턴은 오탐하지 않도록 임계 상향.)"""
    if not num:
        return False
    digits = ''.join(c for c in num if c.isdigit())
    if len(digits) > 15:
        return True
    import re as _re
    if _re.search(r'(0{9,}|9{9,})', digits):
        return True
    return False


def _abnormal_sessions(date_str: str, days: int = 1) -> list:
    """CSP 가 수신 시점에 기록한 비정상 세션 로그({systemId}.security.{mm5}.jsonl)를 읽어
    Call-ID 단위로 집계. (탐지/분류는 CSP 가 수행 — OAM 은 권위 판정을 읽기만 한다.)
    security 라인: {ts,peer,method,caller,callee,ua,call_id,reasons,registered_caller}"""
    if not _calls_dir:
        return []
    import glob as _glob
    from datetime import timedelta as _td
    base_day = _parse_date(date_str)
    try:
        d0 = datetime.strptime(base_day, "%Y-%m-%d")
    except Exception:
        d0 = datetime.now()
    day_list = [(d0 - _td(days=i)).strftime("%Y-%m-%d") for i in range(max(1, days))]

    sessions = {}
    for ds in day_list:
        yyyy, mm, dd = _date_parts(ds)
        for hh in (f"{h:02d}" for h in range(24)):
            base_dir = os.path.join(_calls_dir, yyyy, mm, dd, hh)
            if not os.path.isdir(base_dir):
                continue
            files = (_glob.glob(os.path.join(base_dir, "*.security.[0-9][0-9].jsonl")) +
                     _glob.glob(os.path.join(base_dir, "*.security.jsonl")))
            for fp in files:
                for o in _read_jsonl(fp):
                    peer = o.get('peer', '') or ''
                    peer_ip = peer.rsplit(':', 1)[0] if ':' in peer else peer
                    cid = o.get('call_id', '') or ''
                    key = cid or f"{peer_ip}|{o.get('caller','')}|{o.get('callee','')}"
                    s = sessions.get(key)
                    ts = o.get('ts', '')
                    reasons = [r for r in (o.get('reasons', '') or '').split(',') if r]
                    if s is None:
                        s = {
                            'sesid': cid, 'peer_ip': peer_ip, 'date': ds,
                            'caller': o.get('caller', ''), 'callee': o.get('callee', ''),
                            'ua': o.get('ua', ''), 'methods': set(), 'statuses': set(),
                            'attempts': 0, 'first_ts': ts, 'last_ts': ts,
                            'got_2xx': False, 'reasons': set(),
                        }
                        sessions[key] = s
                    s['attempts'] += 1
                    if o.get('method'):
                        s['methods'].add(o.get('method'))
                    for r in reasons:
                        s['reasons'].add(r)
                    if ts:
                        if not s['first_ts'] or ts < s['first_ts']:
                            s['first_ts'] = ts
                        if ts > s['last_ts']:
                            s['last_ts'] = ts
                    if peer_ip and not s['peer_ip']:
                        s['peer_ip'] = peer_ip

    out = []
    for s in sessions.values():
        reasons = sorted(s['reasons'])
        # 심각도: 스캐너 도구/사기번호 = 높음, 그 외(외부 탐침) = 낮음.
        if 'scanner_ua' in reasons or 'fraud_number' in reasons:
            sev = 'major'
        else:
            sev = 'minor'
        out.append({
            'sesid': s['sesid'], 'peer_ip': s['peer_ip'], 'date': s['date'],
            'caller': s['caller'], 'callee': s['callee'], 'ua': s['ua'],
            'methods': sorted(s['methods']), 'statuses': sorted(s['statuses']),
            'attempts': s['attempts'], 'first_ts': s['first_ts'], 'last_ts': s['last_ts'],
            'got_2xx': s['got_2xx'], 'reasons': reasons, 'severity': sev,
        })
    out.sort(key=lambda x: (x['date'], x['last_ts']), reverse=True)
    return out


async def _handle_abnormal_sessions(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """비정상 세션 이력 — 외부 SIP 스캔/사기 호 시도 탐지.
    GET /api/v1/security/abnormal-sessions?date=YYYY-MM-DD[&days=N]
    """
    if handler_args.method != "GET":
        return HandlerResult(status=405, body="Method Not Allowed")
    qp = getattr(handler_args, 'query_params', {}) or {}
    qs = parse_qs(urlparse(handler_args.full_path or "").query)
    def _qp(name, default=None):
        v = qp.get(name)
        if v: return v
        vl = qs.get(name)
        return vl[0] if vl else default
    date_str = _qp("date", datetime.now().strftime("%Y-%m-%d"))
    try:
        days = max(1, min(31, int(_qp("days", "1"))))
    except (TypeError, ValueError):
        days = 1
    sessions = _abnormal_sessions(date_str, days)
    # 요약 집계
    by_ip = {}
    by_reason = {}
    for s in sessions:
        by_ip[s['peer_ip']] = by_ip.get(s['peer_ip'], 0) + 1
        for r in s['reasons']:
            by_reason[r] = by_reason.get(r, 0) + 1
    return HandlerResult(status=200, body=json.dumps({
        "date": date_str, "days": days,
        "total": len(sessions),
        "by_ip": by_ip, "by_reason": by_reason,
        "sessions": sessions,
    }), media_type="application/json")


FLOW_HANDLER_LIST = [
    ("/api/v1/flow/body", _handle_flow_body, {}),
    ("/api/v1/flow", _handle_flow, {}),
    ("/api/v1/call/logs", _handle_call_logs, {}),
    ("/api/v1/recordings", _handle_recordings, {}),
    ("/api/v1/ptt/history", _handle_ptt_history, {}),
    ("/api/v1/security/abnormal-sessions", _handle_abnormal_sessions, {}),
]
