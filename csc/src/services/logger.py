"""
csc_logger.py — CSC 서비스 로그 + 메시지 로그 유틸리티 (통합 포맷)

신 포맷 (5분 버킷·open-per-write — CSP/CMP 와 동일; mm5=00/05/.../55):
  Flow: {ServiceLogDir}/YYYY/MM/DD/HH/csc_01.flow.{mm5}.jsonl
  Msg : {ServiceLogDir}/YYYY/MM/DD/HH/csc_01_{iface}.msg.{mm5}.jsonl

공통 키: ts, node, service, from, to, proto, method, detail, mid, sesid, subid, seq, iface

sesid 포맷: {caller}::{module}::{yyyymmddHHMMSSuuuuuu}::{counter}
"""

import os
import json
import threading
from datetime import datetime

_service_log_dir: str = ""
_system_id: str = "csc_01"
_lock = threading.Lock()

# iface → 누적 seq 카운터 (프로세스 재시작 시 파일 라인 수로 복원)
_seq_map: dict = {}

# sesid 카운터: 동일 us_ts가 연속될 때 1씩 증가
_sesid_lock = threading.Lock()
_sesid_last_ts: str = ""
_sesid_counter: int = 0

# key (call_id | group_id | user_id 등) → sesid 캐시
_sesid_cache: dict = {}


def init(service_log_dir: str = "", system_id: str = "csc_01"):
    global _service_log_dir, _system_id, _seq_map
    _service_log_dir = service_log_dir or ""
    _system_id = system_id or "csc_01"
    _seq_map = {}


def issue_sesid(caller: str = "", module: str = "csc") -> str:
    """신규 sesid 발행: {caller}::{module}::{us_ts}::{counter}"""
    global _sesid_last_ts, _sesid_counter
    now = datetime.now()
    ts = now.strftime("%Y%m%d%H%M%S") + f"{now.microsecond:06d}"
    with _sesid_lock:
        if ts == _sesid_last_ts:
            _sesid_counter += 1
        else:
            _sesid_last_ts = ts
            _sesid_counter = 1
        counter = _sesid_counter
    return f"{caller}::{module}::{ts}::{counter}"


def get_or_issue_sesid(key: str, caller: str = "", module: str = "csc") -> str:
    """key 기반으로 캐시된 sesid 조회. 없으면 신규 발행 후 저장."""
    if not key:
        return issue_sesid(caller, module)
    with _sesid_lock:
        sid = _sesid_cache.get(key)
        if sid:
            return sid
    sid = issue_sesid(caller, module)
    with _sesid_lock:
        _sesid_cache[key] = sid
        # 캐시 크기 제한
        if len(_sesid_cache) > 10000:
            # 가장 오래된 entry 10% 제거
            for k in list(_sesid_cache.keys())[:1000]:
                del _sesid_cache[k]
    return sid


def clear_sesid(key: str):
    """세션/콜 종료 후 캐시 정리"""
    if not key:
        return
    with _sesid_lock:
        _sesid_cache.pop(key, None)


def _ensure_dir(path: str):
    os.makedirs(path, exist_ok=True)


def _ts_hms() -> str:
    now = datetime.now()
    return now.strftime("%H:%M:%S.") + f"{now.microsecond:06d}"


def _ymdh():
    now = datetime.now()
    return now.strftime("%Y"), now.strftime("%m"), now.strftime("%d"), now.strftime("%H")


def _hour_dir() -> str:
    if not _service_log_dir:
        return ""
    yyyy, mm, dd, hh = _ymdh()
    d = os.path.join(_service_log_dir, yyyy, mm, dd, hh)
    _ensure_dir(d)
    return d


def _bucket() -> str:
    """5분 버킷 suffix "00".."55" (CSP/CMP 와 동일). open-per-write 이므로 핸들 미유지 —
    버킷 전환 시 _next_seq 가 새 파일 줄 수로 자동 리셋(파일경로가 seq_map 키)."""
    return "%02d" % ((datetime.now().minute // 5) * 5)


def _next_seq(iface: str, msg_path: str) -> int:
    key = f"{iface}:{msg_path}"
    cur = _seq_map.get(key)
    if cur is None:
        # 파일 라인 수 복원
        cur = 0
        try:
            if os.path.exists(msg_path):
                with open(msg_path, "rb") as f:
                    cur = sum(1 for _ in f)
        except Exception:
            cur = 0
    cur += 1
    _seq_map[key] = cur
    return cur


# ── Flow + Msg 통합 로깅 ──────────────────────────────────

def log_flow(service: str,
              from_actor: str, to_actor: str,
              proto: str, method: str,
              detail: str = "", sesid: str = "", subid: str = "",
              iface: str = "ue",
              body: str = "", peer: str = "",
              mid: str = "",
              caller: str = "",
              callee: str = ""):
    """CSC의 flow + msg 이중 기록.

    - Flow: csc_01.flow.jsonl (경량, body 없음)
    - Msg : csc_01_{iface}.msg.jsonl (원문 포함) — seq로 flow와 상관
    - 모든 값이 빈 문자열이면 해당 key 생략
    - sesid 없으면 자동 발행 (caller 파라미터로 발신 MSISDN 전달 권장)
    """
    if not _service_log_dir:
        return

    hour_dir = _hour_dir()
    if not hour_dir:
        return

    ts = _ts_hms()
    mm5 = _bucket()
    flow_path = os.path.join(hour_dir, f"{_system_id}.flow.{mm5}.jsonl")
    msg_path  = os.path.join(hour_dir, f"{_system_id}_{iface}.msg.{mm5}.jsonl")

    # sesid 미지정 시 자동 발행
    if not sesid:
        sesid = issue_sesid(caller, "csc")

    with _lock:
        seq = _next_seq(iface, msg_path)

        # msg: ts, dir, peer, caller, callee, proto, msg (빈값 key 생략)
        msg_entry: dict = {"ts": ts}
        direction = "RX" if to_actor == "csc" else "TX"
        msg_entry["dir"] = direction
        if peer:   msg_entry["peer"] = peer
        if caller: msg_entry["caller"] = caller
        if callee: msg_entry["callee"] = callee
        if proto:  msg_entry["proto"] = proto
        if body:   msg_entry["msg"] = body[:8000]

        # flow: ts, service, caller, callee, sesid, subid, node, from, to,
        #        proto, method, detail, mid, seq, iface
        flow_entry: dict = {"ts": ts}
        if service:    flow_entry["service"] = service
        if caller:     flow_entry["caller"] = caller
        if callee:     flow_entry["callee"] = callee
        if sesid:      flow_entry["sesid"] = sesid
        if subid:      flow_entry["subid"] = subid
        if _system_id: flow_entry["node"] = _system_id
        if from_actor: flow_entry["from"] = from_actor
        if to_actor:   flow_entry["to"] = to_actor
        if proto:      flow_entry["proto"] = proto
        if method:     flow_entry["method"] = method
        if detail:     flow_entry["detail"] = detail[:200]
        if mid:        flow_entry["mid"] = mid
        if seq > 0:    flow_entry["seq"] = seq
        if iface:      flow_entry["iface"] = iface

        try:
            with open(msg_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(msg_entry, ensure_ascii=False) + "\n")
            with open(flow_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(flow_entry, ensure_ascii=False) + "\n")
        except Exception:
            pass


# ── 레거시 호환 API ────────────────────────────────────────

def log_msg(interface: str, direction: str, proto: str, method: str, peer: str = "",
             caller: str = "", sesid: str = "", service: str = "mcptt"):
    """메시지 통계 로그. 신 포맷으로 redirect.
    service 기본값 "mcptt" (MCPTT 엔드포인트). admin/console 호출에선 "console" 명시.
    interface 는 method prefix 로 보존 (예: "IdMS/authreq").
    """
    from_actor = "ue" if direction == "in" else "csc"
    to_actor   = "csc" if direction == "in" else "ue"
    # interface 가 sub-function(idms/gms/cms)인 경우 method 앞에 prefix 로 붙임
    qualified_method = method
    if interface and interface not in ("mcptt", "console", "system"):
        qualified_method = f"{interface}/{method}"
    log_flow(service=service, from_actor=from_actor, to_actor=to_actor,
             proto=proto, method=qualified_method, detail=peer, iface="ue", peer=peer,
             sesid=sesid, caller=caller or peer)


def log_ptt_service(group_id: str, direction: str, proto: str, method: str, body: str = "",
                     caller: str = ""):
    """PTT 서비스 로그. group_id 기반 sesid 캐시 활용. service=mcptt 고정."""
    from_actor = "ue" if direction == "in" else "csc"
    to_actor   = "csc" if direction == "in" else "ue"
    sesid = get_or_issue_sesid(group_id, caller, "csc")
    log_flow(service="mcptt", from_actor=from_actor, to_actor=to_actor,
             proto=proto, method=method,
             sesid=sesid, iface="ue", body=body, caller=caller,
             detail=group_id)


def log_console(method: str, caller: str = "", peer: str = "",
                 proto: str = "HTTPS", body: str = "", detail: str = ""):
    """CSC admin/console API 호출 로그. service="console" 고정."""
    log_flow(service="console",
             from_actor="ue", to_actor="csc",
             proto=proto, method=method,
             detail=detail, peer=peer, iface="ue",
             caller=caller, body=body)


def log_ptt_participant(group_id: str, user_id: str, action: str):
    """레거시: PTT participants.jsonl 기록. 유지 (필요 시 호출부에서 제거)."""
    if not _service_log_dir or not group_id:
        return
    yyyy, mm, dd, hh = _ymdh()

    def _sanitize(s: str, max_len: int = 20) -> str:
        r = ''.join('_' if c in '/\\:*?"<>| ' else c for c in s)
        return r[:max_len]

    sg = _sanitize(group_id)
    dir_path = os.path.join(_service_log_dir, "ptt", yyyy, mm, dd, hh,
                             sg[:-2] if len(sg) > 2 else sg, sg + ".d")
    _ensure_dir(dir_path)
    entry = {
        "msisdn": user_id,
        "action": action,
        "time": datetime.now().strftime("%Y-%m-%dT%H:%M:%S"),
    }
    with open(os.path.join(dir_path, "participants.jsonl"), "a") as f:
        f.write(json.dumps(entry, ensure_ascii=False) + "\n")
