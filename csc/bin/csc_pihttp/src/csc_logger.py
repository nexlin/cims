"""
csc_logger.py — CSC 서비스 로그 + 메시지 로그 유틸리티

서비스 로그: {ServiceLogDir}/ptt/{YYYY}/{MM}/{DD}/{HH}/{prefix}/{group_id}.d/csc.jsonl
메시지 로그: {MsgLogDir}/csc/{YYYY}/{MM}/{DD}/{HH}/{interface}.jsonl
"""

import os
import json
import time
from datetime import datetime

_service_log_dir: str = ""
_msg_log_dir: str = ""


def init(service_log_dir: str = "", msg_log_dir: str = ""):
    global _service_log_dir, _msg_log_dir
    _service_log_dir = service_log_dir
    _msg_log_dir = msg_log_dir


def _ensure_dir(path: str):
    os.makedirs(path, exist_ok=True)


def _now_ts() -> str:
    return datetime.now().strftime("%H:%M:%S")


def _now_iso() -> str:
    return datetime.now().strftime("%Y-%m-%dT%H:%M:%S")


def _date_hour_parts():
    now = datetime.now()
    return now.strftime("%Y"), now.strftime("%m"), now.strftime("%d"), now.strftime("%H")


def _sanitize(s: str, max_len: int = 20) -> str:
    r = ''.join('_' if c in '/\\:*?"<>| ' else c for c in s)
    return r[:max_len]


def _prefix(s: str) -> str:
    s = _sanitize(s)
    return s[:-2] if len(s) > 2 else s


# ── 서비스 로그 (Flow용) ──────────────────────────────────

def log_ptt_service(group_id: str, direction: str, proto: str, method: str, body: str = ""):
    """PTT 서비스 로그: {ServiceLogDir}/ptt/YYYY/MM/DD/HH/{prefix}/{group_id}.d/csc.jsonl"""
    if not _service_log_dir or not group_id:
        return

    yyyy, mm, dd, hh = _date_hour_parts()
    sg = _sanitize(group_id)
    dir_path = os.path.join(_service_log_dir, "ptt", yyyy, mm, dd, hh,
                             _prefix(sg), sg + ".d")
    _ensure_dir(dir_path)

    entry = {
        "ts": _now_ts(),
        "from": "ue" if direction == "in" else "csc",
        "to": "csc" if direction == "in" else "ue",
        "proto": proto,
        "label": method,
        "body": body[:2000] if body else "",
    }

    with open(os.path.join(dir_path, "csc.jsonl"), "a") as f:
        f.write(json.dumps(entry, ensure_ascii=False) + "\n")


def log_ptt_participant(group_id: str, user_id: str, action: str):
    """PTT 참여자 로그: participants.jsonl에 기록"""
    if not _service_log_dir or not group_id:
        return

    yyyy, mm, dd, hh = _date_hour_parts()
    sg = _sanitize(group_id)
    dir_path = os.path.join(_service_log_dir, "ptt", yyyy, mm, dd, hh,
                             _prefix(sg), sg + ".d")
    _ensure_dir(dir_path)

    entry = {
        "msisdn": user_id,
        "action": action,
        "time": _now_iso(),
    }

    with open(os.path.join(dir_path, "participants.jsonl"), "a") as f:
        f.write(json.dumps(entry, ensure_ascii=False) + "\n")


# ── 메시지 로그 (통계용) ──────────────────────────────────

def log_msg(interface: str, direction: str, proto: str, method: str, peer: str = ""):
    """메시지 통계 로그: {MsgLogDir}/csc/YYYY/MM/DD/HH/{interface}.jsonl"""
    if not _msg_log_dir:
        return

    yyyy, mm, dd, hh = _date_hour_parts()
    dir_path = os.path.join(_msg_log_dir, "csc", yyyy, mm, dd, hh)
    _ensure_dir(dir_path)

    entry = {
        "ts": _now_ts(),
        "dir": direction,
        "proto": proto,
        "method": method,
        "peer": peer,
    }

    with open(os.path.join(dir_path, interface + ".jsonl"), "a") as f:
        f.write(json.dumps(entry, ensure_ascii=False) + "\n")
