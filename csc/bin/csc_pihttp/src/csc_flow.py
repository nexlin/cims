"""
csc_flow.py — 메시지 플로우 API

디렉터리 구조:
  msg_logs/YYYY/MM/DD/{prefix}/{caller}/{HHmmss}_{caller}_{callee}_{id}.d/*.jsonl

GET /api/v1/flow/list?date=YYYY-MM-DD (또는 YYYYMMDD)
    → 해당 날짜의 통화 디렉터리 목록 반환

GET /api/v1/flow/{dir_name}?date=YYYY-MM-DD
    → 해당 디렉터리의 JSONL 파일을 병합·시간순 정렬하여 반환
"""

import json
import os
import glob as _glob
from datetime import datetime
from urllib.parse import unquote

from util.pi_http.http_handler import HandlerArgs, HandlerResult

_msg_log_dir: str = ""


def init(msg_log_dir: str) -> None:
    global _msg_log_dir
    _msg_log_dir = msg_log_dir


def _parse_date(date_str: str) -> tuple:
    """YYYYMMDD 또는 YYYY-MM-DD → (YYYY, MM, DD)"""
    d = date_str.replace("-", "")
    if len(d) == 8:
        return d[:4], d[4:6], d[6:8]
    now = datetime.now()
    return now.strftime("%Y"), now.strftime("%m"), now.strftime("%d")


def _today_date() -> str:
    return datetime.now().strftime("%Y-%m-%d")


def _date_dir(date_str: str) -> str:
    """msg_logs/YYYY/MM/DD 경로 반환"""
    yyyy, mm, dd = _parse_date(date_str)
    return os.path.join(_msg_log_dir, "msg_logs", yyyy, mm, dd)


def _find_call_dirs(date_str: str) -> list:
    """해당 날짜의 .d 디렉터리 목록을 재귀 탐색하여 반환"""
    if not _msg_log_dir:
        return []
    base = _date_dir(date_str)
    if not os.path.isdir(base):
        return []

    result = []
    # msg_logs/YYYY/MM/DD/{prefix}/{caller}/{xxx}.d
    pattern = os.path.join(base, "*", "*", "*.d")
    for d in sorted(_glob.glob(pattern)):
        if os.path.isdir(d):
            # 상대 이름만 (표시용)
            rel = os.path.relpath(d, base)
            dir_name = os.path.basename(d)
            result.append({
                "dir_name": dir_name,
                "rel_path": rel,
                "full_path": d,
            })
    return result


def _find_call_dir_by_name(date_str: str, dir_name: str) -> str:
    """dir_name(.d 이름)으로 전체 경로를 찾는다"""
    base = _date_dir(date_str)
    pattern = os.path.join(base, "*", "*", dir_name)
    matches = _glob.glob(pattern)
    if matches:
        return matches[0]

    # fallback: 기존 flat 구조 호환 (msg_logs/YYYYMMDD/{call_id}/)
    legacy = os.path.join(_msg_log_dir, "msg_logs", date_str.replace("-", ""), dir_name)
    if os.path.isdir(legacy):
        return legacy
    return ""


def _load_messages(call_dir: str) -> list:
    """디렉터리 내 JSONL 파일들을 병합하고 타임스탬프로 정렬"""
    if not call_dir or not os.path.isdir(call_dir):
        return []

    messages = []
    for jsonl_file in _glob.glob(os.path.join(call_dir, "*.jsonl")):
        try:
            with open(jsonl_file, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line:
                        try:
                            messages.append(json.loads(line))
                        except json.JSONDecodeError:
                            pass
        except OSError:
            pass

    messages.sort(key=lambda m: m.get("ts", ""))
    return messages


# ── HTTP 핸들러 ──────────────────────────────────────────────────────────────

async def _handle_flow(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    if handler_args.method != "GET":
        return HandlerResult(status=405, body="Method Not Allowed")

    full_path = handler_args.full_path or ""
    after = full_path[len("/api/v1/flow"):].lstrip("/")

    date_str = (handler_args.query_params or {}).get("date", _today_date())

    if after == "" or after == "list":
        dirs = _find_call_dirs(date_str)
        call_ids = [d["dir_name"] for d in dirs]
        body = json.dumps({"date": date_str, "call_ids": call_ids, "count": len(call_ids)})
        return HandlerResult(status=200, body=body, media_type="application/json")

    # call_id (dir_name) lookup
    dir_name = unquote(after)
    call_dir = _find_call_dir_by_name(date_str, dir_name)
    if not call_dir:
        return HandlerResult(status=404, body=f"'{dir_name}' not found for date {date_str}")

    messages = _load_messages(call_dir)
    body = json.dumps({
        "call_id": dir_name,
        "date": date_str,
        "messages": messages,
    })
    return HandlerResult(status=200, body=body, media_type="application/json")


FLOW_HANDLER_LIST = [
    ("/api/v1/flow", _handle_flow, {}),
]
