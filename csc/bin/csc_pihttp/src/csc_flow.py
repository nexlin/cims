"""
csc_flow.py — 메시지 플로우 API

GET /api/v1/flow/list?date=YYYYMMDD
    → 해당 날짜(없으면 오늘)의 call_id 목록 반환

GET /api/v1/flow/{call_id}?date=YYYYMMDD
    → call_id 별 JSONL 파일을 병합·시간순 정렬하여 반환

응답 형식 (application/json):
    {
      "call_id": "...",
      "date": "YYYYMMDD",
      "messages": [
        {"ts":"16:05:30.123456","from":"cwrtc","to":"csp","proto":"SIP","label":"REGISTER","body":"..."},
        ...
      ]
    }
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


def _today_str() -> str:
    return datetime.now().strftime("%Y%m%d")


def _list_call_ids(date_str: str) -> list:
    if not _msg_log_dir:
        return []
    date_dir = os.path.join(_msg_log_dir, "msg_logs", date_str)
    if not os.path.isdir(date_dir):
        return []
    return sorted(
        e for e in os.listdir(date_dir)
        if os.path.isdir(os.path.join(date_dir, e))
    )


def _load_messages(date_str: str, call_id: str) -> list:
    """JSONL 파일들을 병합하고 타임스탬프로 정렬"""
    if not _msg_log_dir:
        return []
    call_dir = os.path.join(_msg_log_dir, "msg_logs", date_str, call_id)
    if not os.path.isdir(call_dir):
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
# 라우터는 prefix 매칭이므로 /api/v1/flow 한 개의 핸들러에서 분기

async def _handle_flow(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    if handler_args.method != "GET":
        return HandlerResult(status=405, body="Method Not Allowed")

    # full_path 예: /api/v1/flow/list  or  /api/v1/flow/<call_id>
    full_path = handler_args.full_path or ""
    # /api/v1/flow 이후 세그먼트
    after = full_path[len("/api/v1/flow"):]  # e.g. "" or "/list" or "/some-call-id"
    after = after.lstrip("/")

    date_str = (handler_args.query_params or {}).get("date", _today_str())

    if after == "" or after == "list":
        call_ids = _list_call_ids(date_str)
        body = json.dumps({"date": date_str, "call_ids": call_ids})
        return HandlerResult(status=200, body=body, media_type="application/json")

    # call_id lookup
    call_id = unquote(after)
    messages = _load_messages(date_str, call_id)
    if not messages and not os.path.isdir(
        os.path.join(_msg_log_dir, "msg_logs", date_str, call_id)
    ):
        return HandlerResult(status=404, body=f"call_id '{call_id}' not found for date {date_str}")

    body = json.dumps({"call_id": call_id, "date": date_str, "messages": messages})
    return HandlerResult(status=200, body=body, media_type="application/json")


FLOW_HANDLER_LIST = [
    ("/api/v1/flow", _handle_flow, {}),
]
