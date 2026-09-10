"""csc/src/services/dispatch_history.py — 관제 데스크 통합 이력 조회 백엔드.

dispatch_center.md §5.6/§8.4 · dispatch_desktop_ui.md §11(② PTT 내역 · ④ 일반통화 내역) ·
android_ue_provisioning.md §3-2 · mcdata_messaging.md §4.1.

`GET /provisioning/history?kind=call|ptt|message&since=&until=&limit=`(mcptt.handle_provisioning_history)의
데이터 계층. `until` 이 있으면 [since, until] 창 조회(관제 앱 이력 화면 — 하루 단위 페이지), 없으면 폴링 커서.
종료분 항목에는 녹취 식별자(`recordingId` = 세션 디렉터리의 ServiceLogDir 상대 경로, OAM `/api/v1/recordings/{id}` 와
같은 키)와 `hasRecording`(segments.jsonl 존재)을 싣는다 — 재생은 `/provisioning/recordings/{id}`(dispatch_recordings.py). 진행 중(live) 상태는 표준 구독(RFC 4235 dialog · RFC 4575 conference)이 담당하고 —
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

PTT **창 조회**(until 있음 — 관제 앱 [이력] 화면)는 발언 지표(턴·화자·발화·동시 발언)가 필요한데 그 값은 CMP segments.jsonl 을
집계한 OAM 세션 인덱스(ptt_index — 콘솔 `/api/v1/ptt/sessions`)에만 있다. 그래서 창 조회는 호출자가 OAM 인덱스를 프록시해
받은 행을 `ptt_row_from_oam` 으로 같은 내부 row 로 바꾸고(범위 = 저장키 → 청취 그룹), `finish_rows` 로 같은 마무리를 한다.
OAM 에 닿지 않으면 파일 스캔으로 폴백한다(지표 없음). 폴링(until 없음)은 파일 스캔이다.
응답에는 시간대 분포 `hours`(콘솔 히트맵과 같은 축) 가 함께 실린다.
"""
from __future__ import annotations

import glob as _glob
import json as _json
import os
import re
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

def _group_uri(gid: str) -> str:
    """mcptt_group_id → GMS 그룹 URI(tel: 형). mcptt.py `_group_uri` 와 같은 규칙(순환 import 회피)."""
    if not gid:
        return "tel:"
    if gid.startswith(('tel:', 'sip:')):
        return gid
    if gid.startswith('+'):
        return f"tel:{gid}"
    return f"tel:+{gid}" if gid.isdigit() else f"tel:{gid}"


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

def _rec_info(sl_dir: str, path: Optional[str]) -> Tuple[str, bool]:
    """(recordingId, hasRecording) — path = call.json/session.json 경로. 세션 디렉터리의 sl_dir 상대 경로('/' 구분)가
    녹취 식별자(OAM handlers/recording.py `id` 와 같은 키). segments.jsonl(또는 recordings/segments.jsonl) 이 있으면 녹취 있음."""
    if not path or not sl_dir:
        return "", False
    d = os.path.dirname(path)
    try:
        rel = os.path.relpath(d, sl_dir).replace(os.sep, '/')
    except ValueError:
        return "", False
    if rel.startswith('..'):
        return "", False
    has = os.path.isfile(os.path.join(d, 'segments.jsonl')) or os.path.isfile(os.path.join(d, 'recordings', 'segments.jsonl'))
    return rel, has


def _call_row(cj: dict, sl_dir: str = "", path: Optional[str] = None) -> Optional[dict]:
    if not isinstance(cj, dict) or not cj.get('call_id'):
        return None
    ts = cj.get('end_time') or cj.get('invite_time') or cj.get('start_time')
    rec_id, has_rec = _rec_info(sl_dir, path)
    return {
        "kind": "call", "ts": ts, "id": cj.get('call_id'), "recordingId": rec_id, "hasRecording": has_rec,
        "callType": cj.get('call_type') or 'volte',
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
                    r = _call_row(cj, sl_dir, fp)
                    if r and r["id"] not in seen:
                        seen.add(r["id"]); rows.append(r)
    return rows


# ── kind=ptt ────────────────────────────────────────────────────────────────

def _ptt_row(sj: dict, sl_dir: str = "", path: Optional[str] = None) -> Optional[dict]:
    gid = sj.get('mcptt_group_id') or sj.get('group_id')
    if not isinstance(sj, dict) or not gid:
        return None
    # session.json 은 start_time/sesid, live state/ptt 스냅샷은 started_at/session_id — 둘 다 수용.
    start = sj.get('start_time') or sj.get('started_at')
    ses = sj.get('sesid') or sj.get('session_id')
    ts = sj.get('end_time') or start or sj.get('updated_at')
    rec_id, has_rec = _rec_info(sl_dir, path)
    return {
        "kind": "ptt", "ts": ts, "id": ses or sj.get('call_id') or gid, "recordingId": rec_id, "hasRecording": has_rec,
        "groupId": gid, "groupName": sj.get('name', ''), "sessionKind": "group",
        "initiator": sj.get('initiator', '') or sj.get('subscriber_id', ''),
        "callId": sj.get('call_id'), "state": sj.get('state', ''),
        "startTime": start, "endTime": sj.get('end_time'),
        "memberCount": sj.get('member_count'),
        # 세션 당시 floor 축(session.json 스냅샷). 발언 지표(턴·화자·발화)는 CMP segments.jsonl 에서 나오는 값이라
        # 파일 스캔 경로에는 없다 — 창 조회는 OAM 세션 인덱스(ptt_row_from_oam)가 채운다.
        "floorControl": sj.get('floor_control', ''), "floorPolicy": sj.get('floor_policy', ''),
        "maxTalkers": sj.get('max_talkers', 0),
    }


# 세션 디렉터리 키 'S{yyyymmddHHMMSSuuuuuu}_{n}' (콘솔 pttSession.tsx SES_KEY_RE 와 같은 규칙). 그 외는 구 녹취(시간창 = 디렉터리).
_SES_KEY_RE = re.compile(r'^S\d{14,20}_\d+$')


def ptt_recording_id(group_key: str, ses_dir: str, windows=None) -> str:
    """OAM 세션 인덱스 행(group_key·dir·windows) → 녹취 식별자 `ptt/{저장키}/{Y}/{M}/{D}/{H}[/{세션키}]`
    (콘솔 recIdOf 와 같은 규칙 — 세션이 시간을 넘긴 버킷은 OAM recording.py 가 찾아 붙인다)."""
    src = ses_dir if _SES_KEY_RE.match(ses_dir or '') else ((windows or [ses_dir or ''])[0] or ses_dir or '')
    w = ''.join(c for c in str(src) if c.isdigit())
    if len(w) < 10 or not group_key:
        return ""
    base = f"ptt/{group_key}/{w[0:4]}/{w[4:6]}/{w[6:8]}/{w[8:10]}"
    return f"{base}/{ses_dir}" if _SES_KEY_RE.match(ses_dir or '') else base


def ptt_row_from_oam(it: dict, sl_dir: str = "") -> Optional[dict]:
    """OAM `/api/v1/ptt/sessions` 항목(콘솔 PTT 이력의 읽기 모델 — ptt_index) → 내부 row.
    파일 스캔(_ptt_row)과 같은 키에 발언 지표·종류·참여자를 더한다. id 는 스캔 경로와 같은 우선순위(sesid → call_id → dir)라
    폴링 커서(스캔)와 창 조회(OAM)가 같은 세션을 같은 키로 낸다."""
    if not isinstance(it, dict):
        return None
    gid = it.get('mcptt_group_id') or ''
    key = it.get('dir') or ''
    gk = str(it.get('group_key') or '')
    if not gid or not key:
        return None
    rec_id = ptt_recording_id(gk, key, it.get('windows'))
    has_rec = False
    if rec_id and sl_dir:
        d = os.path.join(sl_dir, rec_id)
        has_rec = os.path.isfile(os.path.join(d, 'segments.jsonl')) or os.path.isfile(os.path.join(d, 'recordings', 'segments.jsonl'))
    if not has_rec:
        has_rec = int(it.get('segment_count') or 0) > 0
    start, end = it.get('start_time'), it.get('end_time')
    return {
        "kind": "ptt", "ts": end or start, "id": it.get('sesid') or it.get('call_id') or key,
        "recordingId": rec_id, "hasRecording": has_rec,
        "groupId": gid, "groupName": it.get('group_name', ''), "sessionKind": it.get('kind') or 'group',
        "initiator": it.get('initiator', ''), "callId": it.get('call_id'), "state": it.get('state', ''),
        "startTime": start, "endTime": end, "memberCount": it.get('member_count'),
        "people": list(it.get('people') or []),
        "turnCount": it.get('turn_count'), "speakerCount": it.get('speaker_count'),
        "totalSpeechMs": it.get('total_speech_ms'), "talkMs": it.get('talk_ms'), "maxConcurrent": it.get('max_concurrent'),
        "floorControl": it.get('floor_control', ''), "floorPolicy": it.get('floor_policy', ''),
        "maxTalkers": it.get('max_talkers', 0),
    }


def _session_end_from_dir(ses_dir: str) -> Optional[str]:
    """세션 디렉터리의 종료 시각 — events.jsonl 의 session_end(없으면 마지막 이벤트) 또는 segments.jsonl 의 마지막 end_time.
    session.json 은 세션 **시작** 때 한 번 기록되는 스냅샷이라 end_time/state 가 비어 있는 것이 보통이다(OAM ptt_index 와 같은 유도)."""
    last_ev = None
    for ev in _read_jsonl(os.path.join(ses_dir, 'events.jsonl')):
        ts = ev.get('ts')
        if not ts:
            continue
        if ev.get('type') == 'session_end':
            return ts
        if last_ev is None or ts > last_ev:
            last_ev = ts
    last_seg = None
    for sg in _read_jsonl(os.path.join(ses_dir, 'segments.jsonl')):
        ts = sg.get('end_time')
        if ts and (last_seg is None or ts > last_seg):
            last_seg = ts
    return last_seg or last_ev


def scan_ptt(sl_dir: str, group_ids: set, since_dt: datetime, until_dt: datetime) -> List[dict]:
    if not group_ids:
        return []
    rows = []
    seen = set()
    # 진행 중 = state/ptt/*.json (CSP 가 참가자마다 쓰고 떠나면 지운다) 에 세션이 있는 것. 이것이 라이브 판정의 정본이다 —
    #   시간 버킷의 session.json 은 시작 스냅샷이라 state/end_time 이 없어도 '진행 중' 이 아니다(콘솔 ptt_index 와 같은 기준).
    live_ids = set()
    for fp in _glob.glob(os.path.join(sl_dir, "state", "ptt", "*.json")):
        sj = _read_json(fp)
        if not sj:
            continue
        for k in ('session_id', 'sesid', 'call_id'):
            if sj.get(k):
                live_ids.add(sj[k])
        if (sj.get('mcptt_group_id') or sj.get('group_id')) in group_ids:
            r = _ptt_row(sj)
            if r and r["id"] not in seen:
                seen.add(r["id"]); rows.append(r)
    for (y, m, d, h) in _hour_buckets(since_dt, until_dt):
        for fp in _glob.glob(os.path.join(sl_dir, "ptt", "*", y, m, d, h, "**", "session.json"), recursive=True):
            sj = _read_json(fp)
            if sj and (sj.get('mcptt_group_id') or sj.get('group_id')) in group_ids:
                r = _ptt_row(sj, sl_dir, fp)
                if not r or r["id"] in seen:
                    continue
                live = r["id"] in live_ids or (r.get("callId") and r["callId"] in live_ids)
                if live:
                    r["state"] = "active"; r["endTime"] = None
                elif r.get("state") != "ended" or not r.get("endTime"):
                    r["state"] = "ended"
                    r["endTime"] = r.get("endTime") or _session_end_from_dir(os.path.dirname(fp)) or r.get("startTime")
                    r["ts"] = r["endTime"] or r["ts"]
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


# ── 앱(HistoryEntry) 와이어 매핑 ──────────────────────────────────────────────
#   Windows 관제 앱 `Services/HistoryClient.Parse` 계약(dispatch_desktop_ui.md §13):
#   item = {id, time(ISO8601+offset), kind, event, from, to, group, duration(sec), emergency, text} + 최상위 next.
#   event 이름표는 앱 switch 와 1:1 — call.answered/missed·ptt.session.start/end·message.sds/sms.

def _local_iso(ts) -> str:
    """파일의 naive-local 시각 → 로컬 offset ISO8601(앱 DateTime.TryParse RoundtripKind 용). 실패 시 ""."""
    dt = parse_ts(ts)
    return dt.astimezone().isoformat(timespec="seconds") if dt else ""


def _dur_sec(a, b) -> int:
    da, db = parse_ts(a), parse_ts(b)
    if da and db:
        return max(0, int((db - da).total_seconds()))
    return 0


def _event_of(row: dict) -> str:
    k = row.get("kind")
    if k == "call":
        return "call.answered" if row.get("answerTime") else "call.missed"
    if k == "ptt":
        return "ptt.session.end" if (row.get("state") == "ended" or row.get("endTime")) else "ptt.session.start"
    if k == "message":
        # 그룹 SDS → ②(sds), 1:1 → ④(sms 취급, 사람 대 사람). 앱이 event prefix 로 패널을 가른다.
        return "message.sds" if row.get("scope") == "group" else "message.sms"
    return ""


def format_item(row: dict) -> dict:
    """내부 row → 앱 와이어 item. 없는 값은 빈 문자열/0/false (앱은 모르는 필드 무시·필수 id·time 없으면 스킵)."""
    k = row.get("kind")
    gid = row.get("groupId")
    group_uri = _group_uri(gid) if gid else ""
    if k == "call":
        duration = int(row.get("duration") or 0)
        frm, to = row.get("initiator", ""), row.get("callee", "")
        group_uri = ""
    elif k == "ptt":
        duration = _dur_sec(row.get("startTime"), row.get("endTime"))
        frm, to = row.get("initiator", ""), ""
    else:  # message
        duration = 0
        frm, to = row.get("from", "") or "", row.get("to", "") or ""
        if row.get("scope") != "group":
            group_uri = ""
    item = {
        "id": row.get("id", ""), "time": _local_iso(row.get("ts")), "kind": k, "event": _event_of(row),
        "from": frm or "", "to": to or "", "group": group_uri,
        "duration": duration, "emergency": bool(row.get("emergency", False)), "text": row.get("text", "") or "",
        "recordingId": row.get("recordingId", "") or "", "hasRecording": bool(row.get("hasRecording", False)),
    }
    # 이력 화면(§4.6 — 콘솔 VoLTE/PTT 이력과 같은 열)용 확장 필드. 폴링 병합(②④)은 공통 필드만 읽고 나머지는 무시한다.
    if k == "call":
        item.update({
            "callType": row.get("callType") or "volte", "state": row.get("state") or "",
            "inviteTime": _local_iso(row.get("inviteTime")), "answerTime": _local_iso(row.get("answerTime")),
            "endTime": _local_iso(row.get("endTime")), "endReason": row.get("endReason") or "",
            "sipStatus": _int0(row.get("sipStatus")),
        })
    elif k == "ptt":
        item.update({
            "sessionKind": row.get("sessionKind") or "group", "state": row.get("state") or "",
            "startTime": _local_iso(row.get("startTime")), "endTime": _local_iso(row.get("endTime")),
            "groupName": row.get("groupName") or "", "memberCount": _int0(row.get("memberCount")),
            "people": [str(p) for p in (row.get("people") or []) if p],
            "turnCount": _int0(row.get("turnCount")), "speakerCount": _int0(row.get("speakerCount")),
            "totalSpeechMs": _int0(row.get("totalSpeechMs")), "talkMs": _int0(row.get("talkMs")),
            "maxConcurrent": _int0(row.get("maxConcurrent")),
            "floorControl": row.get("floorControl") or "", "floorPolicy": row.get("floorPolicy") or "",
            "maxTalkers": _int0(row.get("maxTalkers")),
        })
    return item


def _int0(v) -> int:
    try:
        return int(v or 0)
    except (TypeError, ValueError):
        return 0


def hour_histogram(rows: List[dict]) -> dict:
    """시간대(HH) → 건수. 통화는 INVITE 시각, PTT 는 세션 시작 시각 기준(콘솔 /call/logs·/ptt/sessions 의 hours 와 같은 축),
    없으면 항목 시각. 창 조회 화면의 시간대 밴드(필터이자 그날의 분포) — limit 절삭 **전** 전체 행으로 센다."""
    hours: dict = {}
    for r in rows:
        dt = parse_ts(r.get("inviteTime") or r.get("startTime") or r.get("ts"))
        if dt is None:
            continue
        h = f"{dt.hour:02d}"
        hours[h] = hours.get(h, 0) + 1
    return hours


# ── 통합 조회 ────────────────────────────────────────────────────────────────

def window(since_dt: Optional[datetime], until_dt: Optional[datetime]) -> Tuple[datetime, datetime]:
    """요청의 (since, until) 을 실제 조회 창으로 — until 은 지금을 넘지 않고, since 가 없으면 최근 1시간(폴링 첫 요청)."""
    now = datetime.now()
    if until_dt is None or until_dt > now:
        until_dt = now
    if since_dt is None:
        since_dt = until_dt - timedelta(hours=1)
    return since_dt, until_dt


def finish_rows(rows: List[dict], since_dt: datetime, until_dt: datetime, limit: int) -> Tuple[List[dict], str, dict]:
    """내부 row 들 → (items, next_since, hours). ts 파싱 + since 필터(엄격히 이후) + until 이하 + 오름차순, 최근 limit 개.
    hours 는 절삭 전 창 안 전체 행의 시간대 분포. 스캔 경로(query)와 OAM 인덱스 경로(창 조회)가 같은 마무리를 쓴다."""
    limit = max(1, min(int(limit or _DEFAULT_LIMIT), _MAX_LIMIT))
    dated = []
    for r in rows:
        dt = parse_ts(r.get("ts"))
        if dt is not None and dt > since_dt and dt <= until_dt:
            dated.append((dt, r))
    dated.sort(key=lambda x: x[0])
    hours = hour_histogram([r for _dt, r in dated])
    if len(dated) > limit:
        dated = dated[-limit:]                            # 가장 최근 limit 개
    items = [r for _dt, r in dated]
    next_since = _iso(dated[-1][0]) if dated else _iso(since_dt)
    return items, next_since, hours


def scan(sl_dir: str, kind: str, scope: dict, since_dt: datetime, until_dt: datetime) -> List[dict]:
    """kind 별 파일 스캔(범위 대조 포함). 미지 kind 는 빈 목록."""
    members = scope.get('members') or set()
    ptt_groups = scope.get('ptt_groups') or set()
    if kind == "call":
        return scan_calls(sl_dir, members, since_dt, until_dt)
    if kind == "ptt":
        return scan_ptt(sl_dir, ptt_groups, since_dt, until_dt)
    if kind == "message":
        return scan_messages(sl_dir, ptt_groups, members, since_dt, until_dt)
    return []


def query_ex(sl_dir: str, kind: str, scope: dict, since_dt: Optional[datetime],
             limit: int, until_dt: Optional[datetime] = None) -> Tuple[List[dict], str, dict]:
    """(items, next_since, hours) — 파일 스캔 경로. scope = {'members': set, 'ptt_groups': set}.
    until_dt 가 있으면 창 조회(이력 화면) — 스캔 버킷 상한(_MAX_BUCKETS)은 그대로라 앱은 하루 단위로 나눠 묻는다."""
    since_dt, until_dt = window(since_dt, until_dt)
    if not sl_dir or not os.path.isdir(sl_dir):
        return [], _iso(since_dt), {}
    return finish_rows(scan(sl_dir, kind, scope, since_dt, until_dt), since_dt, until_dt, limit)


def query(sl_dir: str, kind: str, scope: dict, since_dt: Optional[datetime],
          limit: int, until_dt: Optional[datetime] = None) -> Tuple[List[dict], str]:
    """(items, next_since) — items 는 ts 오름차순 최근 limit 개(> since, ≤ until). next_since = 마지막 항목 ts
    (다음 폴링에 그대로 넣으면 그 이후만 받는다)."""
    items, next_since, _hours = query_ex(sl_dir, kind, scope, since_dt, limit, until_dt)
    return items, next_since
