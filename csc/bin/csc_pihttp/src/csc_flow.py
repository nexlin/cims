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
import glob as _glob
from datetime import datetime
from urllib.parse import urlparse, parse_qs, unquote

from util.pi_http.http_handler import HandlerArgs, HandlerResult

_calls_dir: str = ""


def init(service_log_dir: str) -> None:
    """ServiceLogDir을 받아 calls/ 경로 설정"""
    global _calls_dir
    if service_log_dir:
        _calls_dir = service_log_dir


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
    """call_id 또는 group_id(sanitized)로 .d 디렉터리 찾기"""
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

    # 디렉터리명에서 기본 정보 추출
    dirname = os.path.basename(d_dir).replace('.d', '')
    # ptt 경로에 있으면 ptt, 아니면 voip
    call_type = 'ptt' if '/ptt/' in d_dir else 'voip'
    return {'call_id': dirname, 'call_type': call_type, 'state': 'unknown'}


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
    for fn in ('raw_a.rtp', 'recording_a.wav', 'recording_a.mp4'):
        if os.path.exists(os.path.join(d_dir, fn)):
            return True
    return False


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
        for fn in ('recording_a.wav', 'recording_a.mp4', 'raw_a.rtp'):
            path = os.path.join(d_dir, fn)
            if os.path.exists(path) and os.path.getsize(path) > 0:
                ct = 'audio/wav' if fn.endswith('.wav') else ('video/mp4' if fn.endswith('.mp4') else 'application/octet-stream')
                return HandlerResult(status=200, body=path, headers={
                    'Content-Type': ct, 'X-File-Path': path
                })
        return HandlerResult(status=404, body=json.dumps({"error": "Audio not found"}))

    if sub == "video":
        for fn in ('recording_a.mp4', 'recording_b.mp4'):
            path = os.path.join(d_dir, fn)
            if os.path.exists(path):
                return HandlerResult(status=200, body=path, headers={
                    'Content-Type': 'video/mp4', 'X-File-Path': path
                })
        return HandlerResult(status=404, body=json.dumps({"error": "Video not found"}))

    # 상세
    cj = _load_call_json(d_dir)
    cj['participants'] = _load_participants(d_dir)
    cj['has_recording'] = _has_recording(d_dir)
    return HandlerResult(status=200, body=json.dumps(cj), media_type="application/json")


FLOW_HANDLER_LIST = [
    ("/api/v1/flow", _handle_flow, {}),
    ("/api/v1/call/logs", _handle_call_logs, {}),
    ("/api/v1/recordings", _handle_recordings, {}),
]
