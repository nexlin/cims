"""
관제 앱(가입자=관제사, PKCE provisioning 토큰) 의 녹취 조회·재생 — docs/design/features/dispatch_center.md §5.6a ·
android_ue_provisioning.md §3-2.

  GET /provisioning/recordings/{id}                                  세션·세그먼트 메타(OAM `/api/v1/recordings/{id}` 응답 그대로)
  GET /provisioning/recordings/{id}/segments/{seq}/audio?slot=&retry=  MP4(AAC) 본체 · 202 {status:transcoding|recording} · 500 failed
  GET /provisioning/recordings/{id}/segments/{seq}/peaks?slot=         파형 {seq,slot,buckets,peaks[]}
  GET /provisioning/history/ptt/{id}                                   PTT 세션 상세 {session, participants[], events[], floor[], hasRecording}
                                                                       (OAM `/api/v1/ptt/history/{group_key}/{session}` + `/floor` 합본)
  fetch_ptt_sessions(...)                                              `/provisioning/history?kind=ptt&until=` 창 조회가 쓰는 OAM 세션 인덱스
                                                                       (`/api/v1/ptt/sessions`) 프록시 — 호출자 mcptt.handle_provisioning_history

`id` = 세션 디렉터리의 ServiceLogDir 상대 경로(`/provisioning/history` 항목의 `recordingId`, OAM 녹취 API 와 같은 키).
CSC 는 **범위 게이트 + 프록시**만 한다 — 원시 RTP → MP4 변환·캐시·다중 버킷 결합은 oam-svc `handlers/recording.py` 하나가
소유하고(변환 상태·워커 풀·failed 마커) CSC 가 재구현하지 않는다(통합 이력의 "얇은 구독자 뷰" 원칙). 범위 판정은
`/provisioning/history` 와 같은 집합(monitor_scope → members, ptt_listen → ptt_groups):
  ptt/{groupKey}/…  → groupKey(ptt_groups.id 또는 mcptt id) 가 청취 대상 그룹     (session.json 의 mcptt_group_id 로 대조)
  volte/…/{cid}.d   → call.json 의 initiator/callee 중 하나가 감시 대상 가입자
범위 밖·관제 미소속 = 403, 경로 이탈(`..`) = 400. 열람은 감사 E-AUD-016(tap_mode=recording).
설정: `Recording.OamUrl`(비면 `https://{Fm.OamIp}:4419` — OAM 게이트웨이) · `Recording.VerifyTls`(기본 false, 자가서명).
"""

import json
import os
from urllib.parse import urlparse, unquote, quote

import pymysql
import requests

from httpsrv.handler import HandlerArgs, HandlerResult
from services import mcptt as _m
from services import dispatch_history as _dh
from services.mcptt import logger

_BASE = '/provisioning/recordings'
_ACTIONS = ('audio', 'peaks')
_TIMEOUT = (5.0, 130.0)          # (connect, read) — 첫 재생은 OAM 변환 대기(120초 상한) 뒤에 온다
_client = None


def _json(status: int, body, headers=None) -> HandlerResult:
    return HandlerResult(status=status, body=body, media_type='application/json', headers=headers or {})


def _oam_base(config: dict) -> str:
    rec = (config or {}).get('Recording') or {}
    url = str(rec.get('OamUrl') or '').strip().rstrip('/')
    if url:
        return url
    fm = (config or {}).get('Fm') or {}
    return f"https://{fm.get('OamIp', '127.0.0.1')}:4419"


def _http(config: dict) -> requests.Session:
    global _client
    if _client is None:
        rec = (config or {}).get('Recording') or {}
        v = rec.get('VerifyTls', False)                     # 설정 bool 은 문자열("true"/"false")로도 온다 — fm_reporter 와 같은 해석
        _client = requests.Session()
        _client.verify = v is True or str(v).lower() == 'true'
        if not _client.verify:
            try:
                import urllib3
                urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
            except Exception:
                pass
    return _client


def _parts(full_path: str):
    path = urlparse(full_path).path
    if not path.startswith(_BASE + '/'):
        return None, None, None
    rest = [unquote(p) for p in path[len(_BASE) + 1:].split('/') if p]
    # …/{id...}/segments/{seq}/{audio|peaks} — 뒤에서부터 예약어를 떼고 앞이 id.
    if len(rest) >= 4 and rest[-3] == 'segments' and rest[-1] in _ACTIONS:
        return '/'.join(rest[:-3]), rest[-2], rest[-1]
    return '/'.join(rest), None, None


def _safe_id(rec_id: str) -> bool:
    if not rec_id or rec_id.startswith('/') or '\\' in rec_id:
        return False
    return all(seg not in ('', '.', '..') for seg in rec_id.split('/'))


def _scope_sets(config: dict, token: dict):
    """(msisdn, scope) — /provisioning/history 와 같은 해석. DB 오류는 예외."""
    db = (config or {}).get('CimsDatabase') or {}
    msisdn = _m._msisdn_from_id(token.get('mcptt_id') or token.get('sub') or '')
    conn = pymysql.connect(host=db.get('Host', '127.0.0.1'), port=int(db.get('Port', 3306)),
                           user=db.get('User', 'root'), password=db.get('Password', ''),
                           database=db.get('Db', 'cims'), connect_timeout=5)
    try:
        cur = conn.cursor()
        user_id = None
        for t in ('volte_subscriptions', 'ptt_subscriptions'):
            cur.execute(f"SELECT user_id FROM {t} WHERE id=%s", (msisdn,))
            r = cur.fetchone()
            if r:
                user_id = r[0]
                break
        scope = _m._dispatch_scope_sets(cur, user_id) if user_id is not None else None
        group_key_of = {}
        if scope and scope.get('ptt_groups'):
            cur.execute("SELECT id, mcptt_group_id FROM ptt_groups")
            group_key_of = {str(r[0]): r[1] for r in cur.fetchall()}
        return msisdn, scope, group_key_of
    finally:
        conn.close()


def in_scope(sl_dir: str, rec_id: str, scope: dict, group_key_of: dict) -> bool:
    """녹취 id 가 관제 범위 안인가. 파일 SoT(session.json/call.json)로 당사자를 대조한다."""
    top = rec_id.split('/', 1)[0]
    if top == 'ptt':
        segs = rec_id.split('/')
        if len(segs) < 2:
            return False
        key = segs[1]
        gid = group_key_of.get(key, key)                       # surrogate id → mcptt id, priv-/mcptt id 는 그대로
        if gid in scope.get('ptt_groups', set()):
            return True
        sj = _dh._read_json(os.path.join(sl_dir, rec_id, 'session.json'))
        return bool(sj) and (sj.get('mcptt_group_id') or sj.get('group_id')) in scope.get('ptt_groups', set())
    if top == 'volte':
        cj = _dh._read_json(os.path.join(sl_dir, rec_id, 'call.json'))
        return bool(cj) and _dh._call_in_scope(cj, scope.get('members', set()))
    return False


async def handle_recordings(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {}) or {}
    if handler_args.method.upper() != 'GET':
        return _json(405, {'error': 'Method Not Allowed'})
    token = _m.extract_token(handler_args.headers.get('authorization') or handler_args.headers.get('Authorization'))
    if not token:
        return _json(401, {"error": "invalid_token"})
    sc = token.get('scope') or []
    if isinstance(sc, str):
        sc = sc.split()
    if sc and _m.SCOPE_PROVISIONING not in sc:
        return _json(403, {"error": "insufficient_scope", "required": _m.SCOPE_PROVISIONING})

    rec_id, seq, action = _parts(handler_args.full_path)
    if rec_id is None or not _safe_id(rec_id):
        return _json(400, {'error': 'invalid_recording_id'})
    sl_dir = _m._SERVICE_LOG_DIR
    if not sl_dir or not os.path.isdir(sl_dir):
        return _json(503, {'error': 'service_log_unavailable'})
    if not os.path.isdir(os.path.join(sl_dir, rec_id)):
        return _json(404, {'error': 'not_found'})
    try:
        msisdn, scope, group_key_of = _scope_sets(config, token)
    except Exception as e:
        logger.log_error(f"[provisioning/recordings] DB error: {e}")
        return _json(503, {"error": "db_error", "detail": str(e)})
    if not scope:
        return _json(403, {"error": "no_monitor_scope"})
    if not in_scope(sl_dir, rec_id, scope, group_key_of):
        return _json(403, {"error": "out_of_scope"})

    # 프록시 — OAM 녹취 API(handlers/recording.py). id 는 세그먼트별 percent-encoding(슬래시 유지).
    enc_id = '/'.join(quote(p, safe='') for p in rec_id.split('/'))
    path = f"/api/v1/recordings/{enc_id}"
    if seq is not None:
        path += f"/segments/{quote(seq, safe='')}/{action}"
    qp = getattr(handler_args, 'query_params', None) or {}
    params = {k: (v[0] if isinstance(v, list) else v) for k, v in qp.items() if k in ('slot', 'retry')}
    url = _oam_base(config) + path
    try:
        r = _http(config).get(url, params=params, headers={'Accept': '*/*'}, timeout=_TIMEOUT)
    except requests.RequestException as e:
        logger.log_error(f"[provisioning/recordings] OAM unreachable {url}: {e}")
        return _json(502, {'error': 'oam_unreachable', 'detail': str(e)})

    # 감사 — 당사자 모르게 녹취를 여는 동작(E-AUD-016 tap_mode=recording). 메타 조회는 세션당 한 번이 아니라 요청마다 남긴다(재생 단위).
    if action == 'audio' and r.status_code == 200:
        try:
            from services import fm_reporter as _fm
            fr = _fm.get()
            if fr is not None:
                fr.send_event('call_monitored', kind='audit', mo=f"{fr.node}/csc",
                              params={"monitor": msisdn, "group": scope["groupId"], "tap_mode": "recording",
                                      "recording": rec_id, "segment": seq, "slot": params.get('slot', '')},
                              message=f"{msisdn} played recording {rec_id} seg {seq}")
        except Exception as e:
            logger.log_warning(f"[provisioning/recordings] audit emit failed: {e}")

    ct = r.headers.get('content-type', '') or 'application/octet-stream'
    logger.log_info(f"[provisioning/recordings] {msisdn} {rec_id} seq={seq} {action or 'meta'} → {r.status_code} {ct} {len(r.content)}B")
    if ct.startswith('application/json'):
        try:
            return _json(r.status_code, json.loads(r.content.decode('utf-8') or '{}'))
        except ValueError:
            pass
    return HandlerResult(status=r.status_code, body=bytes(r.content), media_type=ct.split(';', 1)[0].strip(),
                         headers={'Content-Type': ct})


# ── PTT 세션 이력 — OAM 세션 인덱스 창 조회 + 세션 상세 프록시 ─────────────────────────────────────────────
#   콘솔 PTT 이력(`/service/history/ptt`)의 읽기 모델(ptt_index — 발언 턴·화자·발화·동시 발언·참여자·floor 타임라인)을
#   관제 앱 [이력] 화면이 같은 값으로 보게 한다. CSC 는 여기서도 **범위 게이트 + 프록시**만 한다.
_HIST_PTT_BASE = '/provisioning/history/ptt'
_TIMEOUT_INDEX = (5.0, 30.0)
_SES_RE = _dh._SES_KEY_RE


def fetch_ptt_sessions(config: dict, since_dt, until_dt, group_keys: set):
    """OAM `/api/v1/ptt/sessions` (세션 축 평면 목록 — 콘솔 PTT 이력) 창 조회. group_keys = 청취 범위 그룹의 녹취 저장 키
    (`ptt_groups.id`) — OAM 은 그룹 세션만 그 키로 좁힌다(1:1·임시는 그룹 엔티티가 아니라 관제 범위 밖).
    반환 = 항목 목록, 실패(OAM 미도달·비정상 응답) 는 None — 호출자가 파일 스캔으로 폴백한다."""
    if not group_keys:
        return []
    params = {"group_key": ",".join(sorted(str(k) for k in group_keys)), "limit": "1000"}
    f, t = since_dt.strftime("%Y-%m-%d"), until_dt.strftime("%Y-%m-%d")
    if f == t:
        params["date"] = f
    else:
        params["from"], params["to"] = f, t
    url = _oam_base(config) + "/api/v1/ptt/sessions"
    try:
        r = _http(config).get(url, params=params, headers={'Accept': 'application/json'}, timeout=_TIMEOUT_INDEX)
    except requests.RequestException as e:
        logger.log_warning(f"[provisioning/history] OAM ptt index unreachable {url}: {e} — falling back to file scan")
        return None
    if r.status_code != 200:
        logger.log_warning(f"[provisioning/history] OAM ptt index {url} → {r.status_code} — falling back to file scan")
        return None
    try:
        body = json.loads(r.content.decode('utf-8') or '{}')
    except ValueError:
        return None
    items = body.get('items') if isinstance(body, dict) else None
    return items if isinstance(items, list) else None


def _ptt_session_ref(rec_id: str):
    """녹취 id `ptt/{저장키}/{Y}/{M}/{D}/{H}[/{세션키}]` → (group_key, OAM session dir). 구 녹취(세션키 없음)는 시간창 YYYYMMDDHH."""
    parts = rec_id.split('/')
    if len(parts) < 6 or parts[0] != 'ptt':
        return None, None
    key = parts[1]
    last = parts[-1]
    if _SES_RE.match(last):
        return key, last
    if len(parts) == 6:
        return key, ''.join(parts[2:6])
    return None, None


async def handle_ptt_session_detail(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """GET /provisioning/history/ptt/{recordingId} — 세션 상세(참여자·입퇴장 이벤트·floor 타임라인). 범위 게이트는 녹취와 같고,
    본문은 OAM `/api/v1/ptt/history/{group_key}/{session}` + `…/floor` 를 합친 것. 열람은 감사 E-AUD-016(tap_mode=history)."""
    config = kwargs.get('config', {}) or {}
    if handler_args.method.upper() != 'GET':
        return _json(405, {'error': 'Method Not Allowed'})
    token = _m.extract_token(handler_args.headers.get('authorization') or handler_args.headers.get('Authorization'))
    if not token:
        return _json(401, {"error": "invalid_token"})
    sc = token.get('scope') or []
    if isinstance(sc, str):
        sc = sc.split()
    if sc and _m.SCOPE_PROVISIONING not in sc:
        return _json(403, {"error": "insufficient_scope", "required": _m.SCOPE_PROVISIONING})

    path = urlparse(handler_args.full_path).path
    if not path.startswith(_HIST_PTT_BASE + '/'):
        return _json(400, {'error': 'invalid_recording_id'})
    rec_id = '/'.join(unquote(p) for p in path[len(_HIST_PTT_BASE) + 1:].split('/') if p)
    if not _safe_id(rec_id):
        return _json(400, {'error': 'invalid_recording_id'})
    group_key, ses_dir = _ptt_session_ref(rec_id)
    if not group_key:
        return _json(400, {'error': 'invalid_recording_id'})
    sl_dir = _m._SERVICE_LOG_DIR
    if not sl_dir or not os.path.isdir(sl_dir):
        return _json(503, {'error': 'service_log_unavailable'})
    if not os.path.isdir(os.path.join(sl_dir, rec_id)):
        return _json(404, {'error': 'not_found'})
    try:
        msisdn, scope, group_key_of = _scope_sets(config, token)
    except Exception as e:
        logger.log_error(f"[provisioning/history/ptt] DB error: {e}")
        return _json(503, {"error": "db_error", "detail": str(e)})
    if not scope:
        return _json(403, {"error": "no_monitor_scope"})
    if not in_scope(sl_dir, rec_id, scope, group_key_of):
        return _json(403, {"error": "out_of_scope"})

    base = _oam_base(config) + f"/api/v1/ptt/history/{quote(group_key, safe='')}/{quote(ses_dir, safe='')}"
    try:
        r1 = _http(config).get(base, headers={'Accept': 'application/json'}, timeout=_TIMEOUT_INDEX)
        r2 = _http(config).get(base + "/floor", headers={'Accept': 'application/json'}, timeout=_TIMEOUT_INDEX)
    except requests.RequestException as e:
        logger.log_error(f"[provisioning/history/ptt] OAM unreachable {base}: {e}")
        return _json(502, {'error': 'oam_unreachable', 'detail': str(e)})
    if r1.status_code != 200:
        try:
            body = json.loads(r1.content.decode('utf-8') or '{}')
        except ValueError:
            body = {'error': 'oam_error'}
        return _json(r1.status_code, body if isinstance(body, dict) else {'error': 'oam_error'})
    try:
        j1 = json.loads(r1.content.decode('utf-8') or '{}')
    except ValueError:
        return _json(502, {'error': 'oam_bad_response'})
    floor = []
    if r2.status_code == 200:
        try:
            floor = (json.loads(r2.content.decode('utf-8') or '{}') or {}).get('floor') or []
        except ValueError:
            floor = []
    body = {
        "recordingId": rec_id,
        "session": j1.get('session') or {},
        "participants": j1.get('participants') or [],
        "events": j1.get('events') or [],
        "floor": floor,
        "hasRecording": bool(j1.get('has_recording')),
    }
    try:
        from services import fm_reporter as _fm
        fr = _fm.get()
        if fr is not None:
            fr.send_event('call_monitored', kind='audit', mo=f"{fr.node}/csc",
                          params={"monitor": msisdn, "group": scope["groupId"], "tap_mode": "history",
                                  "hist_kind": "ptt_session", "recording": rec_id, "count": 1},
                          message=f"{msisdn} read ptt session {rec_id}")
    except Exception as e:
        logger.log_warning(f"[provisioning/history/ptt] audit emit failed: {e}")
    logger.log_info(f"[provisioning/history/ptt] {msisdn} {rec_id} → participants={len(body['participants'])} "
                    f"events={len(body['events'])} floor={len(floor)}")
    return _json(200, body)


CSC_RECORDINGS_HANDLER_LIST = [
    (_BASE, handle_recordings, {}),
    (_HIST_PTT_BASE, handle_ptt_session_detail, {}),      # 가장 긴 접두 우선 — /provisioning/history(목록) 보다 먼저 잡힌다
]
