"""csc/src/services/dispatch_history.py — 관제 데스크 통합 이력 조회 백엔드.

dispatch_center.md §5.6/§8.4 · dispatch_desktop_ui.md §11(② PTT 내역 · ④ 일반통화 내역) ·
android_ue_provisioning.md §3-2 · mcdata_messaging.md §4.1.

`GET /provisioning/history?kind=call|ptt|message&since=&limit=`(mcptt.handle_provisioning_history)의
데이터 계층. 진행 중(live) 상태는 표준 구독(RFC 4235 dialog · RFC 4575 conference)이 담당하고 —
이 API 는 그 구독을 대체하지 않는다 — 여기서는 **관제 범위 안의 지난 이력**만 커서(`since`)로 준다.

백엔드 = CSP/CSC 가 공유 NAS(`ServiceLogging.Dir`)에 남기는 파일 SoT (flow_logger.py 가 콘솔용으로
읽는 것과 같은 파일. 이쪽은 관제사(가입자) PKCE 토큰으로 **관제 그룹 범위**만 걸러 주는 얇은 구독자
뷰다 — 콘솔 이력 API 를 재구현하지 않는다):
  - kind=call    VoLTE 통화     `{sl}/{Y}/{M}/{D}/{H}/**/call.json`      (+ live `{sl}/state/volte/*.json`)
  - kind=ptt     PTT 그룹 세션  `{sl}/ptt/*/{Y}/{M}/{D}/{H}/**/session.json` (+ live `{sl}/state/ptt/*.json`)
  - kind=message SDS            그룹 `{sl}/message/*/{Y}/{M}/{D}/{H}/messages.jsonl`
                                1:1  `{sl}/message_direct/{Y}/{M}/{D}/{H}/messages.jsonl`

범위(scope)는 호출자(mcptt)가 dispatch 그룹에서 유도한다 — CSP `CanWatch`/`CanListenPtt` 와 같은 규칙:
  members     감시 대상 VoLTE 가입자 user-part 집합 (monitor_scope 해석) — call·1:1 message
  ptt_groups  청취 대상 PTT 그룹 mcptt_group_id 집합 (ptt_listen 해석) — ptt·group message
"""
from __future__ import annotations

import glob as _glob
import json as _json
import os
from datetime import datetime, timedelta
from typing import List, Optional, Tuple

from services import logger as _logger

logger = _logger

# 커서가 아주 과거여도 스캔을 유계로 — 시간 버킷 상한(관제 내역 패널은 최근만 본다).
_MAX_BUCKETS = 48          # 최대 48 시간 버킷(≈2일) 뒤로만 스캔
_DEFAULT_LIMIT = 200
_MAX_LIMIT = 1000
_MSG_KINDS = ("sds", "fd", "text")


# ── 시간 유틸 ──────────────────────────────────────────────────────────────

def parse_ts(s) -> Optional[datetime]:
    """ISO8601(naive local) 또는 epoch(초) 문자열 → datetime. 파싱 실패 시 None."""
    if s is None:
        return None
    s = str(s).strip()
    if not s:
        return None
    if s.replace('.', '', 1).isdigit():                 # epoch 초
        try:
            return datetime.fromtimestamp(float(s))
        except (ValueError, OSError):
            return None
    s = s.replace('Z', '').replace('T', ' ')
    for fmt in ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
        try:
            return datetime.strptime(s, fmt)
        except ValueError:
            continue
    return None


def _iso(dt: Optional[datetime]) -> str:
    return dt.strftime("%Y-%m-%dT%H:%M:%S") if dt else ""


def _hour_buckets(since_dt: datetime, until_dt: datetime) -> List[Tuple[str, str, str, str]]:
    """[since, until] 을 덮는 (YYYY,MM,DD,HH) 시간 버킷 목록(최신→과거). _MAX_BUCKETS 로 절삭."""
    cur = until_dt.replace(minute=0, second=0, microsecond=0)
    floor = since_dt.replace(minute=0, second=0, microsecond=0)
    out = []
    while cur >= floor and len(out) < _MAX_BUCKETS:
        out.append((f"{cur.year:04d}", f"{cur.month:02d}", f"{cur.day:02d}", f"{cur.hour:02d}"))
        cur -= timedelta(hours=1)
    return out


# ── 신원 정규화(범위 대조용) ────────────────────────────────────────────────

def userpart(uri) -> str:
    """tel:/sip: scheme 과 @도메인 제거 + 소문자 → user-part. 단일 PTT/VoLTE 도메인 전제
    (mcptt.py `_norm_mcptt_uri` 와 같은 절충 — 사용자부가 시스템에서 유일)."""
    s = str(uri or '').strip().lower()
    for p in ('tel:', 'sip:', 'sips:'):
        if s.startswith(p):
            s = s[len(p):]
            break
    return s.split('@', 1)[0].split(';', 1)[0]


# ── 파일 읽기 ──────────────────────────────────────────────────────────────

def _read_json(path: str) -> Optional[dict]:
    try:
        with open(path, encoding='utf-8') as f:
            o = _json.load(f)
            return o if isinstance(o, dict) else None
    except (OSError, ValueError):
        return None


def _read_jsonl(path: str):
    try:
        with open(path, encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    o = _json.loads(line)
                    if isinstance(o, dict):
                        yield o
                except ValueError:
                    continue
    except OSError:
        return


# ── kind=call ──────────────────────────────────────────────────────────────

def _call_row(cj: dict) -> Optional[dict]:
    if not isinstance(cj, dict) or not cj.get('call_id'):
        return None
    ts = cj.get('end_time') or cj.get('invite_time') or cj.get('start_time')
    return {
        "kind": "call", "ts": ts, "id": cj.get('call_id'),
        "initiator": cj.get('initiator', ''), "callee": cj.get('callee', ''),
        "state": cj.get('state', ''), "inviteTime": cj.get('invite_time'),
        "answerTime": cj.get('answer_time'), "endTime": cj.get('end_time'),
        "duration": cj.get('duration'), "sipStatus": cj.get('sip_status'),
        "endReason": cj.get('end_reason'),
    }


def _call_in_scope(cj: dict, members: set) -> bool:
    return userpart(cj.get('initiator')) in members or userpart(cj.get('callee')) in members


def scan_calls(sl_dir: str, members: set, since_dt: datetime, until_dt: datetime) -> List[dict]:
    if not members:
        return []
    rows = []
    seen = set()
    # live (진행 중) — state/volte 스냅샷. 참여자 필드는 call.json 과 동형(CSP 기록).
    for fp in _glob.glob(os.path.join(sl_dir, "state", "volte", "*.json")):
        cj = _read_json(fp)
        if cj and _call_in_scope(cj, members):
            r = _call_row(cj)
            if r and r["id"] not in seen:
                seen.add(r["id"]); rows.append(r)
    # 종료분 — 시간 버킷의 call.json. 실서버 레이아웃 = {sl}/volte/{Y}/{M}/{D}/{H}/{prefix}/{caller}/{cid}.d/call.json
    #   (flow_logger _find_all_d_dirs 와 동형). 구/올인원 레이아웃({sl}/{Y}/...)도 함께 훑는다(버킷 한정이라 저비용).
    for (y, m, d, h) in _hour_buckets(since_dt, until_dt):
        pats = (os.path.join(sl_dir, "volte", y, m, d, h, "**", "call.json"),
                os.path.join(sl_dir, y, m, d, h, "**", "call.json"))
        for pat in pats:
            for fp in _glob.glob(pat, recursive=True):
                cj = _read_json(fp)
                if cj and (cj.get('call_type') in (None, '', 'volte')) and _call_in_scope(cj, members):
                    r = _call_row(cj)
                    if r and r["id"] not in seen:
                        seen.add(r["id"]); rows.append(r)
    return rows


# ── kind=ptt ────────────────────────────────────────────────────────────────

def _ptt_row(sj: dict) -> Optional[dict]:
    gid = sj.get('mcptt_group_id') or sj.get('group_id')
    if not isinstance(sj, dict) or not gid:
        return None
    # session.json 은 start_time/sesid, live state/ptt 스냅샷은 started_at/session_id — 둘 다 수용.
    start = sj.get('start_time') or sj.get('started_at')
    ses = sj.get('sesid') or sj.get('session_id')
    ts = sj.get('end_time') or start or sj.get('updated_at')
    return {
        "kind": "ptt", "ts": ts, "id": ses or sj.get('call_id') or gid,
        "groupId": gid, "groupName": sj.get('name', ''),
        "initiator": sj.get('initiator', '') or sj.get('subscriber_id', ''),
        "callId": sj.get('call_id'), "state": sj.get('state', ''),
        "startTime": start, "endTime": sj.get('end_time'),
        "memberCount": sj.get('member_count'),
    }


def scan_ptt(sl_dir: str, group_ids: set, since_dt: datetime, until_dt: datetime) -> List[dict]:
    if not group_ids:
        return []
    rows = []
    seen = set()
    for fp in _glob.glob(os.path.join(sl_dir, "state", "ptt", "*.json")):
        sj = _read_json(fp)
        if sj and (sj.get('mcptt_group_id') or sj.get('group_id')) in group_ids:
            r = _ptt_row(sj)
            if r and r["id"] not in seen:
                seen.add(r["id"]); rows.append(r)
    for (y, m, d, h) in _hour_buckets(since_dt, until_dt):
        for fp in _glob.glob(os.path.join(sl_dir, "ptt", "*", y, m, d, h, "**", "session.json"), recursive=True):
            sj = _read_json(fp)
            if sj and (sj.get('mcptt_group_id') or sj.get('group_id')) in group_ids:
                r = _ptt_row(sj)
                if r and r["id"] not in seen:
                    seen.add(r["id"]); rows.append(r)
    return rows


# ── kind=message ─────────────────────────────────────────────────────────────

def _msg_row(rec: dict, scope: str) -> dict:
    return {
        "kind": "message", "ts": rec.get('ts'),
        "id": rec.get('msg_id') or rec.get('conv_id') or '',
        "scope": scope,                                  # "group" | "direct"
        "groupId": rec.get('group'), "from": rec.get('from'), "to": rec.get('to'),
        "msgType": rec.get('msg_type', 'sds'), "convId": rec.get('conv_id'),
        "text": rec.get('text', ''), "size": rec.get('size'),
        "dispositionReq": rec.get('disposition_req'), "fanout": rec.get('fanout'),
        "fileName": rec.get('file_name'), "fileUrl": rec.get('file_url'),
    }


def scan_messages(sl_dir: str, group_ids: set, members: set,
                  since_dt: datetime, until_dt: datetime) -> List[dict]:
    rows = []
    buckets = _hour_buckets(since_dt, until_dt)
    # 그룹 SDS — 범위 = ptt_listen(group_ids). 그룹 디렉터리 전체를 훑고 레코드 group 으로 대조.
    if group_ids:
        for (y, m, d, h) in buckets:
            for fp in _glob.glob(os.path.join(sl_dir, "message", "*", y, m, d, h, "messages.jsonl")):
                for rec in _read_jsonl(fp):
                    if rec.get('group') in group_ids:
                        rows.append(_msg_row(rec, "group"))
    # 1:1 SDS — 범위 = monitor_scope(members). 감시 멤버가 발신 또는 수신인 것 (mcdata_messaging.md §4.3).
    #   CSP 가 Setup.McData.StoreOneToOneSds 로 보관을 켰을 때만 파일이 존재한다.
    if members:
        for (y, m, d, h) in buckets:
            fp = os.path.join(sl_dir, "message_direct", y, m, d, h, "messages.jsonl")
            for rec in _read_jsonl(fp):
                if userpart(rec.get('from')) in members or userpart(rec.get('to')) in members:
                    rows.append(_msg_row(rec, "direct"))
    return rows


# ── 통합 조회 ────────────────────────────────────────────────────────────────

def query(sl_dir: str, kind: str, scope: dict, since_dt: Optional[datetime],
          limit: int) -> Tuple[List[dict], str]:
    """(items, next_since) — items 는 ts 오름차순 최근 limit 개(> since). next_since = 마지막 항목 ts
    (다음 폴링에 그대로 넣으면 그 이후만 받는다). scope = {'members': set, 'ptt_groups': set}."""
    limit = max(1, min(int(limit or _DEFAULT_LIMIT), _MAX_LIMIT))
    until_dt = datetime.now()
    if since_dt is None:
        since_dt = until_dt - timedelta(hours=1)         # 커서 없으면 최근 1시간
    if not sl_dir or not os.path.isdir(sl_dir):
        return [], _iso(since_dt)

    members = scope.get('members') or set()
    ptt_groups = scope.get('ptt_groups') or set()
    if kind == "call":
        rows = scan_calls(sl_dir, members, since_dt, until_dt)
    elif kind == "ptt":
        rows = scan_ptt(sl_dir, ptt_groups, since_dt, until_dt)
    elif kind == "message":
        rows = scan_messages(sl_dir, ptt_groups, members, since_dt, until_dt)
    else:
        return [], _iso(since_dt)

    # ts 파싱 + since 필터(엄격히 이후) + 오름차순 정렬.
    dated = []
    for r in rows:
        dt = parse_ts(r.get("ts"))
        if dt is not None and dt > since_dt:
            dated.append((dt, r))
    dated.sort(key=lambda x: x[0])
    if len(dated) > limit:
        dated = dated[-limit:]                            # 가장 최근 limit 개
    items = [r for _dt, r in dated]
    next_since = _iso(dated[-1][0]) if dated else _iso(since_dt)
    return items, next_since
