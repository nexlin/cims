"""
CIMS Stats & Health REST API
실시간 모니터링 + 통계 집계

Routes:
  GET /api/v1/stats/health                          헬스체크 + 실시간 상태
  GET /api/v1/stats/messages                        메시지 통계
  GET /api/v1/stats/service/voip                    VoIP 서비스 통계
  GET /api/v1/stats/service/ptt                     PTT 서비스 통계
  GET /api/v1/stats/service/summary                 일간 KPI 요약
"""

import os
import glob
import json
import socket
import time
import asyncio
from datetime import datetime, timedelta
from urllib.parse import urlparse, parse_qs, unquote
from pathlib import PurePath

import pymysql
import pymysql.cursors

from httpsrv.handler import HandlerArgs, HandlerResult


def _get_db(config: dict):
    db = config.get('CimsDatabase', {})
    return pymysql.connect(
        host=db.get('Host', '127.0.0.1'),
        port=int(db.get('Port', 3306)),
        user=db.get('User', 'cims'),
        password=db.get('Password', ''),
        database=db.get('Db', 'cims'),
        charset='utf8mb4',
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=True,
    )


def _dt(val):
    return val.isoformat() if val else None


def _path_parts(full_path: str, base: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


# ──────────────────────────────────────────────────────────────
#  UDP 통신 헬퍼 (CSP/CMP stats 수집)
# ──────────────────────────────────────────────────────────────

def _udp_request(ip: str, port: int, data: dict, timeout: float = 1.0) -> dict:
    """UDP로 JSON 요청 보내고 응답 수신. timeout 단축(down 서버 fail-fast)."""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(timeout)
        msg = json.dumps(data).encode('utf-8')
        sock.sendto(msg, (ip, port))
        resp_data, _ = sock.recvfrom(4096)
        sock.close()
        return json.loads(resp_data.decode('utf-8'))
    except Exception:
        return {}


# csp/cmp 상태 단기 캐시 — down 서버 probe 가 timeout 까지 블로킹하므로, 다중 위젯/스위퍼의
# 반복 요청이 매번 probe 하지 않도록 TTL 캐시. (정상 서버는 즉시 응답하므로 영향 미미.)
_STATS_CACHE: dict = {}
_STATS_TTL = 3.0


def _cached(key: str, producer):
    now = time.time()
    e = _STATS_CACHE.get(key)
    if e and now - e[0] < _STATS_TTL:
        return e[1]
    v = producer()
    _STATS_CACHE[key] = (now, v)
    return v


def _get_csp_stats(config: dict) -> dict:
    """CSP에 stats 요청 (3s 캐시)."""
    def probe():
        notify = config.get('CspNotify', {})
        ip = notify.get('Ip', '127.0.0.1')
        port = int(notify.get('Port', 4421))
        resp = _udp_request(ip, port, {"event": "STATS_REQUEST", "uri": "", "action": ""})
        return resp if resp.get('status') == 'OK' else {}
    return _cached('csp', probe)


def _service_log_dir(config: dict) -> str:
    """ServiceLogDir 를 config 에서 조회. csc_app.py 와 동일 로직."""
    sl = config.get('ServiceLogging', {})
    d = sl.get('Dir', '')
    if not d:
        d = config.get('ServiceLogDir', config.get('MsgLogDir', ''))
    return d


def _load_active_states(config: dict, kind: str) -> list:
    """{ServiceLogDir}/state/{kind}/*.json 을 읽어 가입자별 활성 상태 리스트 반환.
       CSP 가 원자 쓰기(.tmp+rename)로 관리하므로 부분 쓰기 읽음은 없음.
       .tmp 잔여 파일은 무시.
    """
    base = _service_log_dir(config)
    if not base:
        return []
    pattern = os.path.join(base, 'state', kind, '*.json')
    items = []
    for fpath in glob.glob(pattern):
        if fpath.endswith('.tmp'):
            continue
        try:
            with open(fpath, 'r') as f:
                items.append(json.loads(f.read()))
        except Exception:
            # 경합/파일 깨짐 등 — 조용히 skip
            pass
    return items


def _get_cmp_stats(config: dict) -> dict:
    """CMP에 stats 요청 (3s 캐시)."""
    def probe():
        cmp_ip = config.get('CmpIp')
        if cmp_ip:
            cmp_port = int(config.get('CmpPort', 9000))
        else:
            # CmpIp 미설정(콘솔 관리 oam-svc 설정) — MediaServer.Endpoints 첫 노드를 대표 probe.
            cmp_ip, cmp_port = _media_endpoints(config)[0]
        resp = _udp_request(cmp_ip, cmp_port, {
            "trans_id": int(time.time()) % 100000,
            "payload": {"cmd": "STATS_REQUEST"}
        })
        if isinstance(resp.get('response'), dict):
            return resp['response']
        return {}
    return _cached('cmp', probe)


def _media_endpoints(config: dict):
    """전 미디어 노드 (ip, port). MediaServer.Endpoints 우선, 없으면 CmpIp 단일.
    Endpoints 원소는 {ip, port} dict(oam.json) 또는 "ip:port" 문자열
    (oam-svc config_template string_list) 둘 다 허용."""
    ms = config.get('MediaServer', {}) or {}
    eps = ms.get('Endpoints') or []
    # 최상위 값이 콤마 문자열이면 리스트로 분해 — string_list 가 배열로 정규화되지 않고
    # 들어온 경우에도 문자 단위 순회로 깨지지 않게 방어. (예: "a:9000, b:9000")
    if isinstance(eps, str):
        eps = [s.strip() for s in eps.split(',') if s.strip()]
    out = []
    for e in eps:
        if isinstance(e, str):
            ip, _, port = e.partition(':')
            if ip.strip():
                try:
                    out.append((ip.strip(), int(port.strip() or 9000)))
                except ValueError:
                    pass
        elif isinstance(e, dict) and e.get('ip'):
            out.append((e['ip'], int(e.get('port', 9000))))
    if not out:
        out = [(config.get('CmpIp', '127.0.0.1'), int(config.get('CmpPort', 9000)))]
    return out


_CMP_LAST_GOOD: dict = {}
_CMP_LAST_GOOD_TTL = 30.0   # 마지막 정상값 보존 한도 — 이 이상 연속 실패면 진짜 down 으로 인정


def _probe_cmp(ip: str, port: int) -> dict:
    """단일 CMP 노드 STATS probe (노드별 3s 캐시).
       부하 중 CMP STATS 응답이 1s 를 넘겨 일시 타임아웃되면 노드가 used=0/down 으로
       튀어 대시보드 RTP 합계가 요동친다 → (1) timeout 을 2.5s 로 늘리고
       (2) miss 시 30s 이내 최근 정상값을 유지해 한 번의 타임아웃으로 0 이 되지 않게 한다."""
    key = f'{ip}:{port}'

    def probe():
        resp = _udp_request(ip, port, {"trans_id": int(time.time()) % 100000,
                                       "payload": {"cmd": "STATS_REQUEST"}}, timeout=2.5)
        r = resp.get('response') if isinstance(resp.get('response'), dict) else None
        now = time.time()
        if r:
            _CMP_LAST_GOOD[key] = (now, r)
            return r
        lg = _CMP_LAST_GOOD.get(key)   # probe miss → 최근 정상값 유지(연속 실패 30s 까지)
        if lg and now - lg[0] < _CMP_LAST_GOOD_TTL:
            return lg[1]
        return {}
    return _cached(f'cmp:{ip}:{port}', probe)


def _all_media_stats(config: dict):
    """전 미디어 노드 STATS — [{host, port, stats}]."""
    return [{'host': ip, 'port': port, 'stats': _probe_cmp(ip, port)}
            for ip, port in _media_endpoints(config)]


def _check_db_health(config: dict) -> bool:
    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute("SELECT 1")
        return True
    except Exception:
        return False


# ──────────────────────────────────────────────────────────────
#  Handlers
# ──────────────────────────────────────────────────────────────

_STATS_BASE = '/api/v1/stats'


async def handle_stats(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    # full_path 는 경로만 담고 query string 은 별도(query_params dict)로 전달된다
    # (controller 가 Starlette request.query_params 를 그대로 넣음, 이미 URL-decode 됨).
    qs = handler_args.query_params or {}
    parts = _path_parts(handler_args.full_path, _STATS_BASE)
    method = handler_args.method.upper()

    if method != 'GET':
        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

    def qp(name, default=None):
        v = qs.get(name)
        return v if v not in (None, '') else default

    try:
        if len(parts) == 0:
            return HandlerResult(status=200, body={'endpoints': [
                '/api/v1/stats/health', '/api/v1/stats/messages',
                '/api/v1/stats/leak-reclaims',
                '/api/v1/stats/service/voip', '/api/v1/stats/service/ptt',
                '/api/v1/stats/service/summary'
            ]})

        if parts[0] == 'health':
            return await _health(config)

        if parts[0] == 'subscribers':
            return await _subscribers_status(
                config,
                status=qp('status', 'active'),
                q=qp('q', '') or '',
                page=qp('page', '1'),
                limit=qp('limit', '50'),
                org=qp('org', '') or '',
            )

        if parts[0] == 'messages':
            iface = parts[1] if len(parts) > 1 else None  # sip, cmp, csc, https
            date = qp('date')
            return await _messages_stats_v2(config, iface, date)

        if parts[0] == 'leak-reclaims':
            return await _leak_reclaims(config, qp('date'))

        # NOTE: service/* (KPI 관측) 는 oam-svc 모듈 귀속 (oam_base_service_split §4).
        # 별도 핸들러 handle_stats_service(_STATS_SERVICE_BASE) 가 처리한다.
        # --role all 에서는 두 핸들러가 모두 등록되며 controller 최장 일치로
        # /api/v1/stats/service/* 는 handle_stats_service 로 라우팅된다(여기 도달 안 함).
        # --role base 에서는 service 핸들러 미등록 → service/* 는 여기서 404 (격리).

        return HandlerResult(status=404, body={'error': 'Not Found'})
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


_STATS_SERVICE_BASE = '/api/v1/stats/service'


async def handle_stats_service(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """서비스 KPI 관측 (CSP/CMP VoIP·PTT) — oam-svc 모듈 귀속.
    base 의 노드 health(_health) 와 함수 단위로 분리(oam_base_service_split §4).
    --role all 에서는 base handle_stats 와 함께 등록되어 controller 최장 일치로
    /api/v1/stats/service/* 만 이 핸들러로 들어온다 (동작 무변경)."""
    config = kwargs.get('config', {})
    qs = handler_args.query_params or {}
    parts = _path_parts(handler_args.full_path, _STATS_SERVICE_BASE)
    method = handler_args.method.upper()

    if method != 'GET':
        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

    def qp(name, default=None):
        v = qs.get(name)
        return v if v not in (None, '') else default

    try:
        svc = parts[0] if len(parts) > 0 else 'summary'
        if svc == 'live':
            return await _service_live(config)
        if svc == 'trend':
            return await _service_trend(config, qp('window', '8h'))
        if svc == 'events':
            return await _service_events(config, qp('limit', '60'))
        if svc == 'org':
            return await _service_org(config)
        if svc == 'ptt-members':
            return await _ptt_members(config, qp('group', ''), qp('page', '1'), qp('limit', '50'))
        gran = qp('granularity', '1d')
        from_dt = qp('from')
        to_dt = qp('to')
        date = qp('date')
        return await _service_stats(config, svc, gran, from_dt, to_dt, date)
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


# ──────────────────────────────────────────────────────────────
#  Health check (Part 1 대시보드 데이터)
# ──────────────────────────────────────────────────────────────

def _get_dashboard_counts(config: dict) -> dict:
    """대시보드 KPI 용 DB 카운트 (3s 캐시). 가입자/번호/등록/그룹.
    등록 = register_time NOT NULL AND (logout_time NULL OR register_time > logout_time)
    — csp 가 REGISTER 시 register_time, 로그아웃/만료 시 logout_time 을 갱신함."""
    def probe():
        try:
            with _get_db(config) as conn:
                with conn.cursor() as cur:
                    cur.execute("""
                        SELECT
                          (SELECT COUNT(*) FROM users)                                   AS subscribers_total,
                          (SELECT COUNT(*) FROM volte_subscriptions)                      AS volte_numbers,
                          (SELECT COUNT(*) FROM volte_subscriptions
                             WHERE register_time IS NOT NULL
                               AND (logout_time IS NULL OR register_time > logout_time))  AS volte_registered,
                          (SELECT COUNT(*) FROM ptt_subscriptions)                        AS ptt_numbers,
                          (SELECT COUNT(*) FROM ptt_subscriptions
                             WHERE register_time IS NOT NULL
                               AND (logout_time IS NULL OR register_time > logout_time))  AS ptt_registered,
                          (SELECT COUNT(*) FROM ptt_groups)                               AS ptt_groups_total
                    """)
                    row = cur.fetchone()
                    if not row:
                        return {}
                    # DictCursor / tuple 양쪽 대응
                    keys = ['subscribers_total', 'volte_numbers', 'volte_registered',
                            'ptt_numbers', 'ptt_registered', 'ptt_groups_total']
                    if isinstance(row, dict):
                        return {k: int(row.get(k) or 0) for k in keys}
                    return {k: int(row[i] or 0) for i, k in enumerate(keys)}
        except Exception:
            return {}
    return _cached('counts', probe)


async def _health(config: dict) -> HandlerResult:
    # csp/cmp UDP probe + DB 체크를 thread 로 병렬 — 이벤트 루프 비블로킹(down 서버 timeout 이
    # 다른 요청을 막지 않도록). 캐시(_cached)와 함께 /stats/health 지연 대폭 감소.
    # CMP 는 전 미디어 노드 집계(AA 다중 노드) — up = any 노드 응답, 카운터는 전 노드 합산.
    csp, media, db_ok, counts = await asyncio.gather(
        asyncio.to_thread(_get_csp_stats, config),
        asyncio.to_thread(_all_media_stats, config),
        asyncio.to_thread(_check_db_health, config),
        asyncio.to_thread(_get_dashboard_counts, config),
    )
    nodes = [nd.get('stats') or {} for nd in media]
    cmp = {}
    if any(nodes):
        _sum_keys = ('sessions', 'groups',
                     'rtp_ports_total', 'rtp_ports_used', 'rtp_ports_free',
                     'ptt_rtp_ports_total', 'ptt_rtp_ports_used', 'ptt_rtp_ports_free',
                     'session_timeout', 'leak_reclaim_total',
                     'leak_reclaim_orphan', 'leak_reclaim_hold')
        cmp = {k: sum((s.get(k, 0) or 0) for s in nodes) for k in _sum_keys}
        cmp['orphan_reclaim_sec'] = max((s.get('orphan_reclaim_sec', 0) or 0) for s in nodes)

    result = {
        'health': {
            'csp': 'up' if csp else 'down',
            'cmp': 'up' if any(nodes) else 'down',
            'db': 'up' if db_ok else 'down',
        },
        'csp': {
            'registered_users': csp.get('registered_users', 0),
            'active_calls': csp.get('active_calls', 0),
            'db_connected': csp.get('db_connected', False),
            'roles': csp.get('roles', {}),
            # 대시보드 KPI — DB 카운트 (가입자/번호/등록/그룹). probe 실패 시 0.
            'subscribers_total': counts.get('subscribers_total', 0),
            'volte_numbers':     counts.get('volte_numbers', 0),
            'volte_registered':  counts.get('volte_registered', 0),
            'ptt_numbers':       counts.get('ptt_numbers', 0),
            'ptt_registered':    counts.get('ptt_registered', 0),
            'ptt_groups_total':  counts.get('ptt_groups_total', 0),
        },
        'cmp': {
            'sessions': cmp.get('sessions', 0),
            'groups': cmp.get('groups', 0),
            # VoIP 풀 (하위호환: rtp_ports = VoIP)
            'rtp_ports': {
                'total': cmp.get('rtp_ports_total', 0),
                'used': cmp.get('rtp_ports_used', 0),
                'free': cmp.get('rtp_ports_free', 0),
            },
            # PTT(그룹통화) 전용 풀 — cmp STATS ptt_rtp_ports_* (구버전 cmp 면 0).
            'rtp_ports_ptt': {
                'total': cmp.get('ptt_rtp_ports_total', 0),
                'used': cmp.get('ptt_rtp_ports_used', 0),
                'free': cmp.get('ptt_rtp_ports_free', 0),
            },
            # 누수 회수(sweeper) 관측 — CSP crash/teardown 누락 등으로 고아가 된 relay 를 CMP sweeper 가
            #   회수한 누적 카운터. RtpMap fix 후 정상 환경에서는 0 이 기대값 — 증가 시 새 누수 신호.
            'sweeper': {
                'session_timeout': cmp.get('session_timeout', 0),
                'orphan_reclaim_sec': cmp.get('orphan_reclaim_sec', 0),
                'leak_reclaim_total': cmp.get('leak_reclaim_total', 0),
                'leak_reclaim_orphan': cmp.get('leak_reclaim_orphan', 0),
                'leak_reclaim_hold': cmp.get('leak_reclaim_hold', 0),
            },
        },
        'record_enable': csp.get('record_enable', False),
    }

    # v3 (2026-04-22): 가입자별 state 파일 기반 실시간 활성 통화 조회.
    #   CSP 가 {ServiceLogDir}/state/{volte,ptt}/{subscriber}.json 에 원자 쓰기로 관리.
    #   VoLTE: 한 통화당 caller+callee 2개 파일 → call_id 로 dedup 해서 통화 목록.
    #   PTT: 가입자별 참여 상태 → group_id 별 집계.
    volte_states = _load_active_states(config, 'volte')
    ptt_states = _load_active_states(config, 'ptt')

    # VoLTE: call_id 기준 dedup (caller 쪽을 우선)
    voip_calls = {}
    for st in volte_states:
        cid = st.get('call_id', '')
        if not cid:
            continue
        entry = voip_calls.setdefault(cid, {
            'call_id': cid,
            'session_id': st.get('session_id', ''),
            'state': st.get('state'),
            'video': st.get('video', False),
            'invite_time': st.get('started_at'),
            'answered_at': st.get('answered_at'),
        })
        role = st.get('role', '')
        sub = st.get('subscriber_id', '')
        if role == 'caller':
            entry['initiator'] = sub
            entry['callee'] = st.get('peer_id', entry.get('callee', ''))
        elif role == 'callee':
            entry['callee'] = sub
            entry.setdefault('initiator', st.get('peer_id', ''))
    result['active_voip'] = sorted(voip_calls.values(), key=lambda x: x.get('invite_time') or '')

    # PTT: group_id 별 참여자 집계
    ptt_groups = {}
    for st in ptt_states:
        gid = st.get('group_id', '')
        if not gid:
            continue
        grp = ptt_groups.setdefault(gid, {
            'call_id': st.get('call_id', ''),
            'group_id': gid,
            'session_id': st.get('session_id', ''),
            'invite_time': st.get('started_at'),
            'state': st.get('state', 'active'),
            'members': [],
            'initiator': None,
        })
        sub = st.get('subscriber_id', '')
        role = st.get('role', 'member')
        if role == 'initiator':
            grp['initiator'] = sub
        grp['members'].append({'subscriber_id': sub, 'role': role})
    result['active_ptt'] = sorted(ptt_groups.values(), key=lambda x: x.get('invite_time') or '')

    # active_calls 보정: csp STATS 의 GetActiveVoipCallCount 는 현재 no-op(항상 0,
    # file-based CallDir 로 전환됨). state 파일 기반 실시간 집계가 SoT 이므로 그 수로 덮음
    # (VoLTE 통화 수 + PTT 활성 그룹 수). csp 값이 더 크면(미래 복구 대비) 큰 쪽 유지.
    file_active = len(result['active_voip']) + len(result['active_ptt'])
    result['csp']['active_calls'] = max(result['csp'].get('active_calls', 0), file_active)

    return HandlerResult(status=200, body=result)


# ──────────────────────────────────────────────────────────────
#  Message stats (Part 3.1)
# ──────────────────────────────────────────────────────────────

_SIP_REQUEST_METHODS = {
    'INVITE', 'ACK', 'BYE', 'CANCEL', 'OPTIONS', 'REGISTER', 'PRACK',
    'SUBSCRIBE', 'NOTIFY', 'PUBLISH', 'INFO', 'REFER', 'MESSAGE', 'UPDATE',
}


def _parse_msg_method(msg: str) -> str:
    """SIP/CMP 원문(msg)에서 통계용 키 추출.
    - 요청: 첫 줄 첫 토큰이 메서드 (INVITE/REGISTER/BYE/...).
    - 응답: 'SIP/2.0 200 OK' → 상태코드('200'/'401'/'180'/...).
    - CMP JSON: payload.cmd (HEARTBEAT/ADD/REMOVE/...).
    """
    if not msg:
        return 'unknown'
    first = msg.replace('\r', '\n').split('\n', 1)[0].strip()
    if not first:
        return 'unknown'
    # CMP/CSC JSON-over-UDP — cmd/type/event 순으로 분류, 응답({result:..})은 RESPONSE.
    if first.startswith('{'):
        try:
            j = json.loads(msg)
            payload = j.get('payload') if isinstance(j.get('payload'), dict) else {}
            for src in (payload, j):
                for k in ('cmd', 'type', 'event'):
                    v = src.get(k)
                    if v:
                        return str(v)
            if 'result' in j or 'status' in j or 'response' in j:
                return 'RESPONSE'
            return 'json'
        except Exception:
            return 'json'
    tok = first.split()
    if first.startswith('SIP/2.0'):
        return tok[1] if len(tok) > 1 else 'response'   # status code
    method = tok[0].upper()
    return method if method in _SIP_REQUEST_METHODS else method


async def _leak_reclaims(config, date=None) -> HandlerResult:
    """CMP sweeper 가 회수한 누수 세션 상세.
       {ServiceLogDir}/leak_reclaim/YYYY/MM/DD/reclaim.jsonl 을 읽어 목록 + reason/node 별 집계 반환.
       RtpMap fix 후 정상 환경에서는 빈 목록이 기대값 — 항목이 있으면 CSP crash/teardown 누락 등 누수 신호."""
    if not date:
        date = datetime.now().strftime('%Y-%m-%d')
    d = date.replace('-', '')
    yyyy, mm, dd = d[:4], d[4:6], d[6:8]
    base = _service_log_dir(config)
    items = []
    counts = {'total': 0, 'orphan_no_rtp': 0, 'hold_timeout': 0}
    by_node: dict = {}
    if base:
        path = os.path.join(base, 'leak_reclaim', yyyy, mm, dd, 'reclaim.jsonl')
        if os.path.exists(path):
            try:
                with open(path, 'r') as f:
                    for line in f:
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            o = json.loads(line)
                        except Exception:
                            continue
                        items.append(o)
                        counts['total'] += 1
                        r = o.get('reason', '')
                        if r in counts:
                            counts[r] += 1
                        n = o.get('node', '') or '?'
                        by_node[n] = by_node.get(n, 0) + 1
            except Exception:
                pass
    items.sort(key=lambda x: x.get('ts', ''), reverse=True)
    return HandlerResult(status=200, body={'date': date, 'counts': counts, 'by_node': by_node,
                                           'items': items[:500]})


async def _messages_stats_v2(config, iface, date) -> HandlerResult:
    """service_log JSONL 기반 인터페이스별 메시지 통계.

    실제 레이아웃: {ServiceLogDir}/YYYY/MM/DD/HH/csp_01_{sip|cmp|csc}.msg.jsonl
    각 라인 = {ts,dir,peer,caller,callee,sesid,proto,msg}. method 는 msg 본문에서 파싱.
    (옛 MsgLogDir/{comp}/.../{iface}.jsonl 레이아웃 + entry['method'] 가정은 폐기됨.)
    """
    import glob as _glob

    if not date:
        date = datetime.now().strftime('%Y-%m-%d')
    d = date.replace('-', '')
    yyyy, mm, dd = d[:4], d[4:6], d[6:8]

    base = _service_log_dir(config)
    if not base:
        return HandlerResult(status=200, body={'date': date, 'interface': iface,
                                               'total': 0, 'buckets': [], 'method_counts': {}})

    hourly = {}         # hour → count
    method_counts = {}  # method/status → count

    if iface == 'https':
        # HTTPS(콘솔/XCAP) — *_ue.msg 에는 본문이 없어 method 불가 → flow 로그의
        # proto=HTTPS 엔트리(method='GET /path', detail='status=NNN')로 집계.
        # (구버전은 unknown iface 가 sip+cmp+csc 전체 합산으로 fallback — 잘못된 수치)
        patterns = [os.path.join(base, yyyy, mm, dd, '*', '*.flow.jsonl'),
                    os.path.join(base, yyyy, mm, dd, '*', '*.flow.[0-9][0-9].jsonl')]
        for pattern in patterns:
            for fpath in _glob.glob(pattern):
                try:
                    with open(fpath, 'r') as f:
                        for line in f:
                            line = line.strip()
                            if not line:
                                continue
                            try:
                                entry = json.loads(line)
                                if entry.get('proto') != 'HTTPS':
                                    continue
                                ts = entry.get('ts', '')
                                hour = int(ts.split(':')[0]) if ':' in ts else 0
                                hourly[hour] = hourly.get(hour, 0) + 1
                                verb = str(entry.get('method', '')).split(' ', 1)[0].upper() or 'unknown'
                                method_counts[verb] = method_counts.get(verb, 0) + 1
                                m = str(entry.get('detail', ''))
                                if m.startswith('status='):
                                    code = m[7:].split()[0]
                                    method_counts[code] = method_counts.get(code, 0) + 1
                            except Exception:
                                pass
                except Exception:
                    pass
    elif iface in ('sip', 'cmp', 'csc'):
        # 시간당 단일 파일(*.msg.jsonl) + 5분 버킷 파일(*.msg.{mm5}.jsonl) 모두 포함.
        # systemId 는 와일드카드 — csp_01 하드코딩 시 csp_02(standby)/멀티노드 누락.
        patterns = [os.path.join(base, yyyy, mm, dd, '*', f'*_{iface}.msg.jsonl'),
                    os.path.join(base, yyyy, mm, dd, '*', f'*_{iface}.msg.[0-9][0-9].jsonl')]
        for pattern in patterns:
            for fpath in _glob.glob(pattern):
                try:
                    with open(fpath, 'r') as f:
                        for line in f:
                            line = line.strip()
                            if not line:
                                continue
                            try:
                                entry = json.loads(line)
                                ts = entry.get('ts', '')
                                hour = int(ts.split(':')[0]) if ':' in ts else 0
                                method = _parse_msg_method(entry.get('msg', ''))
                                hourly[hour] = hourly.get(hour, 0) + 1
                                method_counts[method] = method_counts.get(method, 0) + 1
                            except Exception:
                                pass
                except Exception:
                    pass
    # unknown iface → 빈 결과 (구: 전체 합산 fallback)

    buckets = [{'hour': h, 'count': hourly.get(h, 0)} for h in range(24)]
    sorted_methods = dict(sorted(method_counts.items(), key=lambda x: -x[1]))

    return HandlerResult(status=200, body={
        'date': date,
        'interface': iface,
        'total': sum(hourly.values()),
        'buckets': buckets,
        'method_counts': sorted_methods,
    })


# ──────────────────────────────────────────────────────────────
#  Service stats (Part 3.2) — 파일 기반 (call.json / call.jsonl 스캔)
#
#  v3 (2026-04-22) 이후 call_logs DB 테이블 DROP. service_log/{volte|ptt}/
#  YYYY/MM/DD/HH/.../*.d/call.json 이 SoT. 옛 _messages_stats / DB 기반
#  _calc_*_stats 는 모두 제거됨 (msg_log JSONL 기반 _messages_stats_v2 가
#  /api/v1/stats/messages 를 처리).
# ──────────────────────────────────────────────────────────────

def _iter_call_jsons(config: dict, call_type: str, from_dt: str, to_dt: str):
    """[from_dt, to_dt] 범위 내 .d/call.json (volte) 또는 .d/call.jsonl (ptt) 파싱 결과 yield.

    날짜 단위로만 디렉토리 스캔 → from/to 의 분/초는 결과 객체 ts 비교로 필터.
    """
    base = _service_log_dir(config)
    if not base:
        return
    try:
        f_day = datetime.strptime(from_dt[:10], '%Y-%m-%d').date()
        t_day = datetime.strptime(to_dt[:10], '%Y-%m-%d').date()
    except Exception:
        return
    day = f_day
    while day <= t_day:
        yyyy = f"{day.year:04d}"
        mm = f"{day.month:02d}"
        dd = f"{day.day:02d}"
        date_base = os.path.join(base, call_type, yyyy, mm, dd)
        if os.path.isdir(date_base):
            for cj_path in glob.glob(os.path.join(date_base, '**', '*.d', 'call.json'), recursive=True):
                try:
                    with open(cj_path, 'r', encoding='utf-8') as f:
                        yield json.load(f)
                except Exception:
                    continue
            # PTT 는 call.jsonl (세션별 누적) — 마지막 세션만 사용
            for cjl_path in glob.glob(os.path.join(date_base, '**', '*.d', 'call.jsonl'), recursive=True):
                try:
                    last = None
                    with open(cjl_path, 'r', encoding='utf-8') as f:
                        for line in f:
                            line = line.strip()
                            if not line:
                                continue
                            try:
                                last = json.loads(line)
                            except Exception:
                                continue
                    if last:
                        yield last
                except Exception:
                    continue
        day += timedelta(days=1)


def _ts_of(record: dict, call_type: str) -> str:
    """call_type 별 시작 시각 필드 추출 (volte=invite_time, ptt=start_time).

    call.json 은 ISO 'T' 구분자("2026-06-01T15:52:37"), from/to 파라미터는 공백
    구분자("2026-06-01 00:00:00"). 문자열 비교 시 'T'(0x54) > ' '(0x20) 라 전 레코드가
    to_dt 초과로 배제되는 버그가 있었음 → 여기서 ' ' 로 정규화해 비교/버킷 모두 일치시킴.
    """
    if call_type == 'ptt':
        ts = record.get('start_time', '') or record.get('invite_time', '')
    else:
        ts = record.get('invite_time', '')
    return ts.replace('T', ' ', 1) if ts else ts


def _bucket_key(ts: str, gran: str) -> str:
    """gran 별 버킷 키 — '시간(0-23)' 또는 'YYYY-MM-DD'."""
    if not ts:
        return ''
    if gran in ('5m', '10m', '1h'):
        # HH 추출 (ISO ts 의 11..13 또는 'T' 다음 두 자리)
        try:
            return str(int(ts[11:13]))
        except Exception:
            return ''
    return ts[:10]


async def _service_stats(config, svc, gran, from_dt, to_dt, date) -> HandlerResult:
    if not from_dt:
        if date:
            from_dt = date + ' 00:00:00'
            to_dt = date + ' 23:59:59'
        else:
            today = datetime.now().strftime('%Y-%m-%d')
            from_dt = today + ' 00:00:00'
            to_dt = today + ' 23:59:59'
    elif not to_dt:
        to_dt = from_dt

    try:
        voip = _calc_voip_stats(config, from_dt, to_dt, gran) if svc in ('volte', 'voip', 'summary') else None
        ptt = _calc_ptt_stats(config, from_dt, to_dt, gran) if svc in ('ptt', 'summary') else None

        result = {'granularity': gran, 'from': from_dt, 'to': to_dt}
        if voip:
            result['volte'] = voip
        if ptt:
            result['ptt'] = ptt

        return HandlerResult(status=200, body=result)
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


def _calc_voip_stats(config, from_dt, to_dt, gran):
    total = 0
    success = 0
    durations = []
    end_reasons: dict = {}
    by_bucket_total: dict = {}  # bucket_key -> count
    by_bucket_ok: dict = {}     # bucket_key -> count

    for rec in _iter_call_jsons(config, 'volte', from_dt, to_dt):
        ts = _ts_of(rec, 'volte')
        if not ts or ts < from_dt or ts > to_dt:
            continue
        total += 1
        state = rec.get('state', '')
        reason = rec.get('end_reason') or 'unknown'
        dur = int(rec.get('duration', 0) or 0)
        is_success = (state == 'ended' and reason == 'normal')
        if is_success:
            success += 1
        if dur > 0:
            durations.append(dur)
        if state == 'ended':
            end_reasons[reason] = end_reasons.get(reason, 0) + 1
        bk = _bucket_key(ts, gran)
        if bk:
            by_bucket_total[bk] = by_bucket_total.get(bk, 0) + 1
            if is_success:
                by_bucket_ok[bk] = by_bucket_ok.get(bk, 0) + 1

    if gran in ('5m', '10m', '1h'):
        keys = sorted(by_bucket_total.keys(), key=lambda k: int(k))
        buckets = []
        for k in keys:
            cnt = by_bucket_total[k]
            ok = by_bucket_ok.get(k, 0)
            buckets.append({'hour': int(k), 'attempts': cnt, 'success': ok,
                            'success_rate': round(ok / cnt * 100, 1) if cnt > 0 else 0})
    else:
        keys = sorted(by_bucket_total.keys())
        buckets = []
        for k in keys:
            cnt = by_bucket_total[k]
            ok = by_bucket_ok.get(k, 0)
            buckets.append({'date': k, 'attempts': cnt, 'success': ok,
                            'success_rate': round(ok / cnt * 100, 1) if cnt > 0 else 0})

    avg_dur = round(sum(durations) / len(durations), 1) if durations else 0
    return {
        'total_attempts': total,
        'total_success': success,
        'success_rate': round(success / total * 100, 1) if total > 0 else 0,
        'avg_duration_sec': avg_dur,
        'end_reasons': end_reasons,
        'buckets': buckets,
    }


def _calc_ptt_stats(config, from_dt, to_dt, gran):
    total = 0
    durations = []
    by_group: dict = {}
    by_bucket: dict = {}

    for rec in _iter_call_jsons(config, 'ptt', from_dt, to_dt):
        ts = _ts_of(rec, 'ptt')
        if not ts or ts < from_dt or ts > to_dt:
            continue
        total += 1
        gid = rec.get('group_id', '') or 'unknown'
        by_group[gid] = by_group.get(gid, 0) + 1
        dur = int(rec.get('duration', 0) or 0)
        if dur > 0:
            durations.append(dur)
        bk = _bucket_key(ts, gran)
        if bk:
            by_bucket[bk] = by_bucket.get(bk, 0) + 1

    if gran in ('5m', '10m', '1h'):
        buckets = [{'hour': int(k), 'calls': by_bucket[k]}
                   for k in sorted(by_bucket.keys(), key=lambda k: int(k))]
    else:
        buckets = [{'date': k, 'calls': by_bucket[k]} for k in sorted(by_bucket.keys())]

    by_group_sorted = dict(sorted(by_group.items(), key=lambda x: -x[1]))
    avg_dur = round(sum(durations) / len(durations), 1) if durations else 0
    return {
        'total_calls': total,
        'avg_duration_sec': avg_dur,
        'by_group': by_group_sorted,
        'buckets': buckets,
    }


# ──────────────────────────────────────────────────────────────
#  Subscribers real-time status (가입자별 실시간 접속/통화 상태)
# ──────────────────────────────────────────────────────────────

async def _subscribers_status(config: dict, status: str = 'active',
                              q: str = '', page='1', limit='50', org: str = '') -> HandlerResult:
    """가입자 서비스 이용 상태 조회 — 서버사이드 필터/페이지네이션.

       대규모(수천~) 가입자에서도 일정한 비용을 유지하기 위해 전건 반환을 폐기하고,
       status/q/page/limit 로 DB 가 직접 거른 한 페이지만 반환한다. 상단 요약은
       counts(all/online/active) 로 별도 집계해 행 목록과 분리한다.

       - status='active' (기본): 현재 통화/그룹 참여 중인 가입자만. 동시호 용량에
         bound 되므로 가입자 수와 무관하게 작고 빠르다 (활성 세션 현황 = A 뷰).
       - status='online': 등록(접속) 중인 가입자.
       - status='all': 전체 가입자 (이름/번호 검색·페이지로 조회 = B 뷰).

       v3: call_logs 테이블 DROP 후 state 파일이 SOT — CSP 가
       {ServiceLogDir}/state/{volte|ptt}/{subscriber}.json 에 원자 쓰기로 관리한다.
       state 의 subscriber_id 는 canonical id(=MSISDN, volte/ptt_subscriptions.id)."""

    status = (status or 'active').lower()
    if status not in ('active', 'online', 'all'):
        status = 'active'
    q = (q or '').strip()
    try:
        page = max(1, int(page))
    except (TypeError, ValueError):
        page = 1
    try:
        limit = min(200, max(1, int(limit)))
    except (TypeError, ValueError):
        limit = 50
    offset = (page - 1) * limit

    # 1) state 파일에서 active 통화/그룹 인덱스 구축
    volte_states = _load_active_states(config, 'volte')
    ptt_states = _load_active_states(config, 'ptt')

    volte_active_by_sub = {}   # subscriber_id(MSISDN) → [{call_id, peer, role, state, invite_time}]
    for st in volte_states:
        sub = st.get('subscriber_id', '')
        if not sub:
            continue
        volte_active_by_sub.setdefault(sub, []).append({
            'call_id': st.get('call_id', ''),
            'peer': st.get('peer_id', ''),
            'role': st.get('role', ''),
            'state': st.get('state', ''),
            'invite_time': st.get('started_at'),
            'answered_at': st.get('answered_at'),
            'video': st.get('video', False),
        })

    # PTT: 그룹별 참여자 집계 (active_members = state 파일 기반)
    group_active_members = {}  # group_id → count
    ptt_states_by_sub = {}     # subscriber_id(MSISDN) → [state...]
    for st in ptt_states:
        gid = st.get('group_id', '')
        if gid:
            group_active_members[gid] = group_active_members.get(gid, 0) + 1
        sub = st.get('subscriber_id', '')
        if sub:
            ptt_states_by_sub.setdefault(sub, []).append(st)

    # 활성 가입자 식별자 집합 (MSISDN). active 필터/카운트의 IN 절에 사용.
    active_ids = set(volte_active_by_sub) | set(ptt_states_by_sub)

    # CMP 에서 floor holder 조회 시도 (실패해도 무관)
    group_floor_holder = {}
    try:
        cmp_stats = _get_cmp_stats(config)
        for gd in (cmp_stats or {}).get('group_details', []) or []:
            gid = gd.get('group_id', '')
            if gid and gd.get('floor_holder'):
                group_floor_holder[gid] = gd['floor_holder']
    except Exception:
        pass

    # online 판정 SQL 조각 (register_time 유효 && 미로그아웃)
    _VOLTE_ON = ("(vs.id IS NOT NULL AND vs.register_time IS NOT NULL AND "
                 "(vs.logout_time IS NULL OR vs.register_time > vs.logout_time))")
    _PTT_ON = ("(ps.id IS NOT NULL AND ps.register_time IS NOT NULL AND "
               "(ps.logout_time IS NULL OR ps.register_time > ps.logout_time))")
    _BASE = ("FROM users u "
             "LEFT JOIN volte_subscriptions vs ON vs.user_id = u.id "
             "LEFT JOIN ptt_subscriptions ps ON ps.user_id = u.id ")

    subscribers = []
    counts = {'all': 0, 'online': 0, 'active': 0}
    total = 0
    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                # ── 상단 요약 카운트 (필터와 무관, 토글 뱃지용) ──
                cur.execute("SELECT COUNT(*) AS c FROM users")
                counts['all'] = cur.fetchone()['c']
                cur.execute(f"SELECT COUNT(*) AS c {_BASE} WHERE ({_VOLTE_ON} OR {_PTT_ON})")
                counts['online'] = cur.fetchone()['c']
                if active_ids:
                    ids = list(active_ids)
                    ph = ','.join(['%s'] * len(ids))
                    cur.execute(
                        f"SELECT COUNT(DISTINCT u.id) AS c {_BASE} "
                        f"WHERE vs.id IN ({ph}) OR ps.id IN ({ph}) "
                        f"OR vs.imsi IN ({ph}) OR ps.imsi IN ({ph})",
                        ids * 4,
                    )
                    counts['active'] = cur.fetchone()['c']

                # ── 현재 필터의 WHERE 절 구성 ──
                where, params = [], []
                if status == 'online':
                    where.append(f"({_VOLTE_ON} OR {_PTT_ON})")
                elif status == 'active':
                    if not active_ids:
                        where.append("1=0")  # 활성 없음 → 빈 페이지
                    else:
                        ids = list(active_ids)
                        ph = ','.join(['%s'] * len(ids))
                        where.append(f"(vs.id IN ({ph}) OR ps.id IN ({ph}) "
                                     f"OR vs.imsi IN ({ph}) OR ps.imsi IN ({ph}))")
                        params += ids * 4
                if q:
                    where.append("(u.name LIKE %s OR vs.id LIKE %s OR ps.id LIKE %s)")
                    like = f"%{q}%"
                    params += [like, like, like]
                if org:
                    codes = _org_descendants(config, org)   # 부서(회사/본부/팀) → 하위 전체
                    ph = ','.join(['%s'] * len(codes))
                    where.append(f"u.org_id IN ({ph})")
                    params += codes
                where_sql = ("WHERE " + " AND ".join(where)) if where else ""

                # ── 현재 필터의 total + 페이지 행 ──
                cur.execute(f"SELECT COUNT(*) AS c {_BASE} {where_sql}", params)
                total = cur.fetchone()['c']

                cur.execute(
                    "SELECT u.id AS person_id, u.name, u.org_id AS org_id, "
                    "vs.id AS volte_id, vs.imsi AS volte_imsi, "
                    "vs.register_time AS volte_reg_time, vs.logout_time AS volte_logout_time, "
                    "ps.id AS ptt_id, ps.imsi AS ptt_imsi, "
                    "ps.register_time AS ptt_reg_time, ps.logout_time AS ptt_logout_time "
                    f"{_BASE} {where_sql} ORDER BY u.name LIMIT %s OFFSET %s",
                    params + [limit, offset],
                )
                rows = cur.fetchall()
                org_paths = _org_paths(config)

                # ── 페이지에 등장한 활성 그룹의 총 멤버 수만 조회 ──
                page_gids = set()
                for row in rows:
                    for key in (row.get('ptt_id'), row.get('ptt_imsi')):
                        for st in ptt_states_by_sub.get(key, []):
                            if st.get('group_id'):
                                page_gids.add(st['group_id'])
                group_total = {}
                if page_gids:
                    # state 의 group_id = mcptt_group_id 식별자. members.group_id 는
                    # surrogate 이므로 ptt_groups JOIN 으로 mcptt_group_id 기준 집계.
                    ph = ','.join(['%s'] * len(page_gids))
                    cur.execute(
                        f"SELECT g.mcptt_group_id AS gid, COUNT(*) AS cnt "
                        f"FROM ptt_group_members m JOIN ptt_groups g ON g.id = m.group_id "
                        f"WHERE g.mcptt_group_id IN ({ph}) GROUP BY g.mcptt_group_id",
                        tuple(page_gids),
                    )
                    for r in cur.fetchall():
                        group_total[r['gid']] = r['cnt']

                def _ptt_groups_for(key):
                    out = []
                    for st in ptt_states_by_sub.get(key, []):
                        gid = st.get('group_id', '')
                        out.append({
                            'call_id': st.get('call_id', ''),
                            'group_id': gid,
                            'state': st.get('state', 'active'),
                            'role': st.get('role', 'member'),
                            'invite_time': st.get('started_at'),
                            'total_members': group_total.get(gid, 0),
                            'active_members': group_active_members.get(gid, 0),
                            'floor_holder': group_floor_holder.get(gid),
                        })
                    return out

                for row in rows:
                    volte_id = row.get('volte_id')
                    ptt_id = row.get('ptt_id')

                    volte_online = bool(volte_id and row.get('volte_reg_time') and (
                        not row.get('volte_logout_time') or row['volte_reg_time'] > row['volte_logout_time']))
                    ptt_online = bool(ptt_id and row.get('ptt_reg_time') and (
                        not row.get('ptt_logout_time') or row['ptt_reg_time'] > row['ptt_logout_time']))

                    org_code = row.get('org_id') or ''
                    sub = {
                        'person_id': row['person_id'],
                        'name': row['name'],
                        'org': org_code,
                        'org_path': org_paths.get(org_code, org_code),
                        'volte': None,
                        'ptt': None,
                    }

                    if volte_id:
                        calls = (volte_active_by_sub.get(volte_id)
                                 or volte_active_by_sub.get(row.get('volte_imsi')) or [])
                        sub['volte'] = {
                            'msisdn': volte_id,
                            'online': volte_online,
                            'register_time': _dt(row.get('volte_reg_time')),
                            'calls': calls,
                        }

                    if ptt_id:
                        groups = (_ptt_groups_for(ptt_id)
                                  or _ptt_groups_for(row.get('ptt_imsi')))
                        sub['ptt'] = {
                            'msisdn': ptt_id,
                            'online': ptt_online,
                            'register_time': _dt(row.get('ptt_reg_time')),
                            'groups': groups,
                        }

                    subscribers.append(sub)

    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})

    return HandlerResult(status=200, body={
        'total': total,
        'page': page,
        'limit': limit,
        'status': status,
        'counts': counts,
        'subscribers': subscribers,
    })


# ──────────────────────────────────────────────────────────────
#  서비스 라이브 모니터링 (VoLTE 호 / PTT 그룹 중심 + KPI/용량/이상)
# ──────────────────────────────────────────────────────────────

def _parse_iso(s):
    if not s:
        return None
    try:
        return datetime.fromisoformat(str(s).replace('Z', '').split('+')[0])
    except Exception:
        return None


# 동시 사용량 추세 — live 폴링 시점마다 현재값을 분 단위 버킷에 기록(롤링).
#   {minute_epoch: {'volte':n,'ringing':n,'ptt':n,'talking':n}}  (최근 ~12시간 유지)
_TREND_SAMPLES: dict = {}
_TREND_MAX_MIN = 720


def _trend_record(now: datetime, volte: int, ringing: int, ptt: int, talking: int):
    minute = int(now.timestamp()) // 60
    cur = _TREND_SAMPLES.get(minute)
    if cur is None:
        _TREND_SAMPLES[minute] = {'volte': volte, 'ringing': ringing, 'ptt': ptt, 'talking': talking}
    else:
        # 같은 분 내 여러 폴링 → 피크 유지
        cur['volte'] = max(cur['volte'], volte)
        cur['ringing'] = max(cur['ringing'], ringing)
        cur['ptt'] = max(cur['ptt'], ptt)
        cur['talking'] = max(cur['talking'], talking)
    if len(_TREND_SAMPLES) > _TREND_MAX_MIN + 60:
        cutoff = minute - _TREND_MAX_MIN
        for k in [k for k in _TREND_SAMPLES if k < cutoff]:
            _TREND_SAMPLES.pop(k, None)


def _floor_held_secs(config: dict, surrogate_id, holder: str, now: datetime):
    """그룹의 현재 시간버킷 floor.jsonl 에서 holder 에게 마지막 GRANT 된 시각 → 점유 경과(초).
       경로: {ServiceLogDir}/ptt/{surrogate_id}/{YYYY}/{MM}/{DD}/{HH}/floor.jsonl"""
    base = _service_log_dir(config)
    if not base or surrogate_id is None or not holder:
        return None
    fpath = os.path.join(base, 'ptt', str(surrogate_id),
                         f'{now.year:04d}', f'{now.month:02d}', f'{now.day:02d}',
                         f'{now.hour:02d}', 'floor.jsonl')
    last_grant = None
    try:
        with open(fpath, 'r') as f:
            for line in f:
                try:
                    ev = json.loads(line)
                except Exception:
                    continue
                if ev.get('op') in ('GRANT', 'TAKEN') and ev.get('user') == holder:
                    last_grant = ev.get('ts')
    except Exception:
        return None
    g = _parse_iso(last_grant)
    if not g:
        return None
    return max(0, int((now - g).total_seconds()))


_RINGING_ANOMALY_SEC = 30
_FLOOR_MONOPOLY_SEC = 60


def _ptt_floor_activity(config: dict, window_min: int = 5) -> dict:
    """최근 window_min 분간 그룹별 floor GRANT(발언) 활동 집계.
       {mcptt_group_id: {'count': N, 'last_ts': iso}}. floor.jsonl 스캔(현재+직전 시간버킷).
       상시활성·대규모(그룹 다수) 환경에서 '발언 활동' 기준 랭킹/필터용."""
    base = _service_log_dir(config)
    if not base:
        return {}
    now = datetime.now()
    cutoff = now - timedelta(minutes=window_min)
    buckets = _hour_buckets(now - timedelta(hours=1), now)
    sur2g = {}
    for gj in glob.glob(os.path.join(base, 'ptt', '*', 'group.json')):
        try:
            with open(gj) as f:
                sur2g[os.path.basename(os.path.dirname(gj))] = json.load(f).get('mcptt_group_id')
        except Exception:
            pass
    act = {}
    for (Y, M, D, H) in buckets:
        for fp in glob.glob(os.path.join(base, 'ptt', '*', f'{Y:04d}', f'{M:02d}', f'{D:02d}', f'{H:02d}', 'floor.jsonl')):
            gid = sur2g.get(fp.split(os.sep)[-6])
            if not gid:
                continue
            try:
                with open(fp) as f:
                    for line in f:
                        try:
                            ev = json.loads(line)
                        except Exception:
                            continue
                        if ev.get('op') != 'GRANT':
                            continue
                        ts = _parse_iso(ev.get('ts'))
                        if not ts or ts < cutoff:
                            continue
                        a = act.setdefault(gid, {'count': 0, 'last_ts': None})
                        a['count'] += 1
                        if not a['last_ts'] or (ev.get('ts') or '') > a['last_ts']:
                            a['last_ts'] = ev.get('ts')
            except Exception:
                pass
    return act


async def _service_live(config: dict) -> HandlerResult:
    """VoLTE 호 / PTT 그룹 중심 실시간 모니터링 스냅샷 + KPI/용량/이상징후."""
    now = datetime.now()
    volte_states = _load_active_states(config, 'volte')
    ptt_states = _load_active_states(config, 'ptt')
    media = _all_media_stats(config)
    counts = _get_dashboard_counts(config)

    # ── VoLTE: call_id 기준 dedup (caller+callee 2파일 → 1호) ──
    calls = {}
    for st in volte_states:
        cid = st.get('call_id', '')
        if not cid:
            continue
        e = calls.setdefault(cid, {
            'call_id': cid, 'session_id': st.get('session_id', ''),
            'state': st.get('state'), 'video': bool(st.get('video', False)),
            'invite_time': st.get('started_at'), 'answered_at': st.get('answered_at'),
            'media_node': st.get('media_node', ''), 'org': '',
            'caller': '', 'callee': '',
        })
        if st.get('media_node') and not e.get('media_node'):
            e['media_node'] = st['media_node']
        role = st.get('role', '')
        sub = st.get('subscriber_id', '')
        if role == 'caller':
            e['caller'] = sub
            if not e['callee']:
                e['callee'] = st.get('peer_id', '')
        elif role == 'callee':
            e['callee'] = sub
            if not e['caller']:
                e['caller'] = st.get('peer_id', '')
        if st.get('answered_at') and not e.get('answered_at'):
            e['answered_at'] = st.get('answered_at')

    volte_calls = []
    ringing = 0
    durations = []
    for e in calls.values():
        inv = _parse_iso(e['invite_time'])
        e['duration_sec'] = max(0, int((now - inv).total_seconds())) if inv else 0
        anomalies = []
        if e['state'] == 'ringing':
            ringing += 1
            if e['duration_sec'] > _RINGING_ANOMALY_SEC:
                anomalies.append({'type': 'long_ringing',
                                  'detail': f"호출 {e['duration_sec']}초 무응답"})
        else:
            durations.append(e['duration_sec'])
        e['anomalies'] = anomalies
        volte_calls.append(e)
    volte_calls.sort(key=lambda x: x.get('invite_time') or '')
    active_volte = sum(1 for c in volte_calls if c['state'] != 'ringing')
    avg_dur = int(sum(durations) / len(durations)) if durations else 0

    # ── PTT: group_id 별 집계 ──
    groups = {}
    for st in ptt_states:
        gid = st.get('group_id', '')
        if not gid:
            continue
        g = groups.setdefault(gid, {
            'group_id': gid, 'session_id': st.get('session_id', ''),
            'invite_time': st.get('started_at'), 'members': [], 'initiator': None,
        })
        sub = st.get('subscriber_id', '')
        role = st.get('role', 'member')
        if role == 'initiator':
            g['initiator'] = sub
        g['members'].append({'subscriber_id': sub, 'role': role})

    # CMP floor holder (전 노드 group_details 병합)
    floor_holder = {}
    for nd in media:
        for gd in (nd['stats'].get('group_details') or []):
            gg = gd.get('group_id', '')
            if gg and gd.get('floor_holder'):
                floor_holder[gg] = gd['floor_holder']

    # DB: 그룹 메타(이름/타입/총멤버/surrogate id/org) + 활성 가입자 org 매핑(조직별 필터용)
    gmeta = {}
    m2o = {}   # msisdn → org_id (활성 가입자만)
    active_msisdns = ({st.get('subscriber_id') for st in volte_states if st.get('subscriber_id')}
                      | {st.get('subscriber_id') for st in ptt_states if st.get('subscriber_id')})
    if groups or active_msisdns:
        try:
            with _get_db(config) as conn:
                with conn.cursor() as cur:
                    if groups:
                        ph = ','.join(['%s'] * len(groups))
                        cur.execute(
                            f"SELECT g.id, g.mcptt_group_id AS gid, g.name, g.group_type, g.org_code, "
                            f"(SELECT COUNT(*) FROM ptt_group_members m WHERE m.group_id = g.id) AS total "
                            f"FROM ptt_groups g WHERE g.mcptt_group_id IN ({ph})",
                            tuple(groups.keys()),
                        )
                        for r in cur.fetchall():
                            gmeta[r['gid']] = r
                    if active_msisdns:
                        ids = list(active_msisdns)
                        ph = ','.join(['%s'] * len(ids))
                        cur.execute(
                            f"SELECT vs.id AS m, COALESCE(NULLIF(u.org_id,''),'') AS o FROM volte_subscriptions vs JOIN users u ON u.id=vs.user_id WHERE vs.id IN ({ph}) "
                            f"UNION SELECT ps.id, COALESCE(NULLIF(u.org_id,''),'') FROM ptt_subscriptions ps JOIN users u ON u.id=ps.user_id WHERE ps.id IN ({ph})",
                            tuple(ids + ids),
                        )
                        for r in cur.fetchall():
                            if r['o']:
                                m2o[r['m']] = r['o']
        except Exception:
            pass

    # 활성 호 org 태깅(발신자 기준)
    for c in volte_calls:
        c['org'] = m2o.get(c.get('caller'), '')

    floor_act = _ptt_floor_activity(config, 5)   # 최근 5분 발언 활동
    ptt_groups_out = []
    talking = 0
    participants = 0
    talkers = []   # 현재 발언 중(floor 점유) 가입자 — 조직 가입자 활동 집계용
    for gid, g in groups.items():
        inv = _parse_iso(g['invite_time'])
        g['duration_sec'] = max(0, int((now - inv).total_seconds())) if inv else 0
        meta = gmeta.get(gid, {})
        g['name'] = meta.get('name') or gid
        g['type'] = meta.get('group_type') or ''
        g['org'] = (meta.get('org_code') or m2o.get(g.get('initiator'), '') or '')
        g['total_members'] = int(meta.get('total') or len(g['members']))
        g['active_members'] = len(g['members'])
        fa = floor_act.get(gid, {})
        g['floor_count'] = fa.get('count', 0)   # 최근 5분 발언 횟수
        g['last_floor'] = fa.get('last_ts')
        g['floor_holder'] = floor_holder.get(gid)
        g.pop('members', None)   # 멤버 비인라인(그룹당 100~200명) → drill 엔드포인트로 페이지네이션
        participants += g['active_members']
        anomalies = []
        if g['floor_holder']:
            talking += 1
            talkers.append({'msisdn': g['floor_holder'], 'org': m2o.get(g['floor_holder'], ''),
                            'group_id': gid, 'group_name': g['name']})
            held = _floor_held_secs(config, meta.get('id'), g['floor_holder'], now)
            if held is not None and held > _FLOOR_MONOPOLY_SEC:
                anomalies.append({'type': 'floor_monopoly',
                                  'detail': f"Floor {held}초 점유"})
                g['floor_held_sec'] = held
        g['anomalies'] = anomalies
        ptt_groups_out.append(g)

    # 활동 순 정렬: 발언 중 우선 → 최근 발언수 → 최근 발언시각
    def _act_key(x):
        lf = _parse_iso(x.get('last_floor'))
        return (0 if x.get('floor_holder') else 1, -int(x.get('floor_count') or 0),
                -(lf.timestamp() if lf else 0))
    ptt_groups_out.sort(key=_act_key)

    # ── 용량: 전 미디어 노드 RTP 풀 집계 + 노드별 분산 ──
    #   (PTT 그룹 리소스 = rtp+floor+video 묶음 1:1 → ptt_rtp 풀 == 그룹/floor 동시 capacity) ──
    def _pool(s, pfx):
        return {'total': int(s.get(f'{pfx}_total', 0) or 0),
                'used': int(s.get(f'{pfx}_used', 0) or 0),
                'free': int(s.get(f'{pfx}_free', 0) or 0)}
    vt = {'total': 0, 'used': 0, 'free': 0}
    pt = {'total': 0, 'used': 0, 'free': 0}
    nodes = []
    for nd in media:
        s = nd['stats'] or {}
        vr, pr = _pool(s, 'rtp_ports'), _pool(s, 'ptt_rtp_ports')
        for k in ('total', 'used', 'free'):
            vt[k] += vr[k]
            pt[k] += pr[k]
        nodes.append({'host': nd['host'], 'up': bool(s),
                      'volte_rtp': vr, 'ptt_rtp': pr,
                      'groups': len(s.get('group_details') or [])})
    capacity = {'volte_rtp': vt, 'ptt_rtp': pt, 'nodes': nodes}

    # ── 이상 징후 집계 ──
    anomalies_all = []
    for c in volte_calls:
        for a in c['anomalies']:
            anomalies_all.append({'kind': 'volte', 'type': a['type'], 'detail': a['detail'],
                                  'label': f"{c['caller']} → {c['callee']}", 'ref': c['call_id']})
    for g in ptt_groups_out:
        for a in g['anomalies']:
            anomalies_all.append({'kind': 'ptt', 'type': a['type'], 'detail': a['detail'],
                                  'label': f"{g['name']} / {g['floor_holder']}", 'ref': g['group_id']})

    # 추세 표본 기록
    _trend_record(now, active_volte, ringing, len(ptt_groups_out), talking)

    return HandlerResult(status=200, body={
        'ts': now.isoformat(timespec='seconds'),
        'volte': {
            'kpi': {'active': active_volte, 'ringing': ringing, 'avg_duration_sec': avg_dur,
                    'registered': counts.get('volte_registered', 0),
                    'numbers': counts.get('volte_numbers', 0)},
            'calls': volte_calls,
        },
        'ptt': {
            'kpi': {'talking': talking, 'recent_active': len(floor_act),
                    'active_groups': len(ptt_groups_out), 'participants': participants,
                    'total_groups': counts.get('ptt_groups_total', 0),
                    'registered': counts.get('ptt_registered', 0),
                    'numbers': counts.get('ptt_numbers', 0)},
            'groups': ptt_groups_out,
            'talkers': talkers,
        },
        'capacity': capacity,
        'anomalies': anomalies_all,
    })


_BACKFILL_CACHE: dict = {'min': None, 'window': None, 'data': {}}
_BACKFILL_LOOKBACK_H = 3  # 장시간 세션 포착용 lookback (시간버킷)


def _hour_buckets(start: datetime, end: datetime):
    out = []
    cur = start.replace(minute=0, second=0, microsecond=0)
    while cur <= end:
        out.append((cur.year, cur.month, cur.day, cur.hour))
        cur += timedelta(hours=1)
    return out


def _add_interval(per_min: dict, key: str, s_ts: float, e_ts: float, from_min: int, to_min: int):
    m0 = max(from_min, int(s_ts // 60))
    m1 = min(to_min, int(e_ts // 60))
    for m in range(m0, m1 + 1):
        per_min.setdefault(m, {'volte': 0, 'ptt': 0})[key] += 1


def _trend_backfill(config: dict, from_min: int, to_min: int) -> dict:
    """라이브 표본이 없는 분을 서비스 로그에서 동시성 재구성 (분 단위 캐시).
       VoLTE: call.json [invite_time, end_time] (call_id dedup).
       PTT: events.jsonl member_join/leave → 활성(멤버>0) 구간."""
    if _BACKFILL_CACHE['min'] == to_min and _BACKFILL_CACHE['window'] == (to_min - from_min):
        return _BACKFILL_CACHE['data']
    base = _service_log_dir(config)
    per_min: dict = {}
    if not base:
        _BACKFILL_CACHE.update({'min': to_min, 'window': to_min - from_min, 'data': per_min})
        return per_min
    now = datetime.now()
    now_ts = now.timestamp()
    buckets = _hour_buckets(now - timedelta(hours=_BACKFILL_LOOKBACK_H), now)
    # 비정상 종료(크래시·강제kill)로 end_time 미기록된 레코드가 "현재까지 활성"으로
    # 오인되지 않도록, 현재 active state 에 있는 호/그룹만 now 까지 연장한다.
    active_cids = {st.get('call_id') for st in _load_active_states(config, 'volte') if st.get('call_id')}
    active_gids = {st.get('group_id') for st in _load_active_states(config, 'ptt') if st.get('group_id')}

    # ── VoLTE: call.json 구간 (call_id dedup) ──
    calls = {}
    for (Y, M, D, H) in buckets:
        pat = os.path.join(base, 'volte', f'{Y:04d}', f'{M:02d}', f'{D:02d}', f'{H:02d}',
                           '*', '*', '*.d', 'call.json')
        for fp in glob.glob(pat):
            try:
                with open(fp) as f:
                    d = json.load(f)
            except Exception:
                continue
            cid = d.get('call_id')
            inv = _parse_iso(d.get('invite_time'))
            if not cid or not inv:
                continue
            end = _parse_iso(d.get('end_time'))
            if end:
                e_ts = end.timestamp()
            elif cid in active_cids:
                e_ts = now_ts                       # 진짜 진행 중
            else:
                try:
                    e_ts = os.path.getmtime(fp)     # 비정상 종료 → 마지막 기록 시각
                except OSError:
                    e_ts = inv.timestamp()
            s_ts = inv.timestamp()
            if e_ts < s_ts:
                e_ts = s_ts
            prev = calls.get(cid)
            calls[cid] = (min(prev[0], s_ts), max(prev[1], e_ts)) if prev else (s_ts, e_ts)
    for (s_ts, e_ts) in calls.values():
        _add_interval(per_min, 'volte', s_ts, e_ts, from_min, to_min)

    # ── PTT: 그룹별 활성(멤버>0) 구간 ──
    for gd in glob.glob(os.path.join(base, 'ptt', '*') + os.sep):
        # 그룹 현재 활성 여부 (mcptt_group_id ↔ active state)
        g_active = False
        try:
            with open(os.path.join(gd, 'group.json')) as f:
                g_active = json.load(f).get('mcptt_group_id') in active_gids
        except Exception:
            pass
        last_event_ts = None
        events = []
        for (Y, M, D, H) in buckets:
            ep = os.path.join(gd, f'{Y:04d}', f'{M:02d}', f'{D:02d}', f'{H:02d}', 'events.jsonl')
            try:
                with open(ep) as f:
                    for line in f:
                        try:
                            ev = json.loads(line)
                        except Exception:
                            continue
                        ts = _parse_iso(ev.get('ts'))
                        t = ev.get('type')
                        if ts and t in ('member_join', 'member_leave'):
                            events.append((ts.timestamp(), 1 if t == 'member_join' else -1))
            except Exception:
                pass
        if not events:
            continue
        events.sort()
        last_event_ts = events[-1][0]
        cnt = 0
        seg_start = None
        for ts, delta in events:
            prev = cnt
            cnt = max(0, cnt + delta)
            if prev == 0 and cnt > 0:
                seg_start = ts
            elif prev > 0 and cnt == 0 and seg_start is not None:
                _add_interval(per_min, 'ptt', seg_start, ts, from_min, to_min)
                seg_start = None
        if seg_start is not None:
            # 미닫힌 구간: 현재 활성이면 now, 아니면 마지막 이벤트까지
            _add_interval(per_min, 'ptt', seg_start, now_ts if g_active else last_event_ts, from_min, to_min)

    _BACKFILL_CACHE.update({'min': to_min, 'window': to_min - from_min, 'data': per_min})
    return per_min


_TREND_WINDOWS = {'2h': 120, '4h': 240, '8h': 480, '16h': 960, '24h': 1440}
# 윈도우 → (버킷 길이[초], 버킷 수). 모든 윈도우를 24등분(NB=24).
_TREND_BUCKETS = {
    '2h':  (300, 24),    # 5분 × 24
    '4h':  (600, 24),    # 10분 × 24
    '8h':  (1200, 24),   # 20분 × 24
    '16h': (2400, 24),   # 40분 × 24
    '24h': (3600, 24),   # 1시간 × 24
}
_TREND_METRICS = ('volte_active', 'volte_calls', 'ptt_grants', 'ptt_speakers', 'ptt_groups')
_TREND2_CACHE: dict = {'key': None, 'data': None}


async def _service_trend(config: dict, window='8h') -> HandlerResult:
    """사용량 추세 — 윈도우를 24등분한 버킷으로 지표를 서비스 로그에서 재구성.
       간격: 2h=5분, 4h=10분, 8h=20분, 16h=40분, 24h=1시간 (모두 24버킷). 버킷 경계는 정시(clock) 정렬,
       마지막 칸은 현재 시각이 속한 버킷.
       지표: VoLTE 동시통화(구간 겹침)·발생호(구간 시작), PTT 발언수(floor GRANT)·발언자(distinct)·활성그룹."""
    spec = _TREND_BUCKETS.get(str(window))
    if spec is None:
        wmin = _TREND_WINDOWS.get(str(window))
        if wmin is None:
            try:
                wmin = max(60, min(int(window), 1440))
            except (TypeError, ValueError):
                wmin = 360
            window = next((k for k, v in _TREND_WINDOWS.items() if v == wmin), f'{wmin}m')
        # 미정의 윈도우 → 24등분 fallback
        bucket_sec = max(60, wmin * 60 // 24)
        NB = 24
    else:
        bucket_sec, NB = spec
    wmin = bucket_sec * NB // 60
    now = datetime.now()
    now_min = int(now.timestamp()) // 60
    ck = (window, now_min)
    if _TREND2_CACHE['key'] == ck:
        return HandlerResult(status=200, body=_TREND2_CACHE['data'])

    # 현재 시각이 속한 버킷을 마지막 칸으로 두고, 버킷 경계를 정시(clock)에 정렬
    cur_bucket = int(now.timestamp()) // bucket_sec * bucket_sec
    start_ts = float(cur_bucket - (NB - 1) * bucket_sec)

    def bidx(ts):
        i = int((ts - start_ts) // bucket_sec)
        return i if 0 <= i < NB else -1

    buckets = [{'volte_active': 0, 'volte_calls': 0, 'ptt_grants': 0,
                'ptt_speakers': set(), 'ptt_groups': set()} for _ in range(NB)]
    base = _service_log_dir(config)
    if base:
        now_ts = now.timestamp()
        hbs = _hour_buckets(now - timedelta(minutes=wmin) - timedelta(hours=2), now)
        # VoLTE: call.json (call_id dedup)
        seen = set()
        for (Y, M, D, H) in hbs:
            for fp in glob.glob(os.path.join(base, 'volte', f'{Y:04d}', f'{M:02d}', f'{D:02d}', f'{H:02d}',
                                             '*', '*', '*.d', 'call.json')):
                try:
                    with open(fp) as f:
                        d = json.load(f)
                except Exception:
                    continue
                cid = d.get('call_id')
                inv = _parse_iso(d.get('invite_time'))
                if not cid or cid in seen or not inv:
                    continue
                seen.add(cid)
                end = _parse_iso(d.get('end_time'))
                inv_ts = inv.timestamp()
                if end:
                    end_ts = end.timestamp()
                else:
                    try:
                        end_ts = os.path.getmtime(fp)   # end_time 미기록(크래시) → 마지막 기록 시각
                    except OSError:
                        end_ts = inv_ts
                if end_ts < inv_ts:
                    end_ts = inv_ts
                bi = bidx(inv_ts)
                if bi >= 0:
                    buckets[bi]['volte_calls'] += 1
                for i in range(NB):
                    bs = start_ts + i * bucket_sec
                    if inv_ts < bs + bucket_sec and end_ts > bs:
                        buckets[i]['volte_active'] += 1
        # PTT: floor.jsonl GRANT
        sur2g = {}
        for gj in glob.glob(os.path.join(base, 'ptt', '*', 'group.json')):
            try:
                with open(gj) as f:
                    sur2g[os.path.basename(os.path.dirname(gj))] = json.load(f).get('mcptt_group_id')
            except Exception:
                pass
        for (Y, M, D, H) in hbs:
            for fp in glob.glob(os.path.join(base, 'ptt', '*', f'{Y:04d}', f'{M:02d}', f'{D:02d}', f'{H:02d}', 'floor.jsonl')):
                gid = sur2g.get(fp.split(os.sep)[-6]) or fp.split(os.sep)[-6]
                try:
                    with open(fp) as f:
                        for line in f:
                            try:
                                ev = json.loads(line)
                            except Exception:
                                continue
                            if ev.get('op') != 'GRANT':
                                continue
                            ts = _parse_iso(ev.get('ts'))
                            if not ts:
                                continue
                            bi = bidx(ts.timestamp())
                            if bi < 0:
                                continue
                            buckets[bi]['ptt_grants'] += 1
                            if ev.get('user'):
                                buckets[bi]['ptt_speakers'].add(ev['user'])
                            buckets[bi]['ptt_groups'].add(gid)
                except Exception:
                    pass

    points = []
    for i, b in enumerate(buckets):
        points.append({'t': int(start_ts + i * bucket_sec),
                       'volte_active': b['volte_active'], 'volte_calls': b['volte_calls'],
                       'ptt_grants': b['ptt_grants'], 'ptt_speakers': len(b['ptt_speakers']),
                       'ptt_groups': len(b['ptt_groups'])})
    body = {'window': window, 'window_min': wmin, 'bucket_sec': bucket_sec, 'points': points,
            'peaks': {k: max((p[k] for p in points), default=0) for k in _TREND_METRICS}}
    _TREND2_CACHE['key'], _TREND2_CACHE['data'] = ck, body
    return HandlerResult(status=200, body=body)


# ──────────────────────────────────────────────────────────────
#  ④ 라이브 이벤트 스트림 / ⑥ 조직별 집계
# ──────────────────────────────────────────────────────────────

def _build_service_events(config: dict) -> list:
    base = _service_log_dir(config)
    if not base:
        return []
    now = datetime.now()
    buckets = _hour_buckets(now - timedelta(hours=1), now)  # 현재+직전 시간버킷
    events = []

    # VoLTE: call.json → call_start / call_end (call_id dedup)
    seen = set()
    for (Y, M, D, H) in buckets:
        for fp in glob.glob(os.path.join(base, 'volte', f'{Y:04d}', f'{M:02d}', f'{D:02d}', f'{H:02d}',
                                         '*', '*', '*.d', 'call.json')):
            try:
                with open(fp) as f:
                    d = json.load(f)
            except Exception:
                continue
            cid = d.get('call_id')
            if not cid or cid in seen:
                continue
            seen.add(cid)
            ini, cal = d.get('initiator', ''), d.get('callee', '')
            vid = d.get('call_type') == 'volte_video'
            if d.get('invite_time'):
                events.append({'ts': d['invite_time'], 'kind': 'volte', 'type': 'call_start',
                               'detail': f"{ini} → {cal} ({'영상' if vid else '음성'})", 'ref': cid})
            if d.get('end_time'):
                events.append({'ts': d['end_time'], 'kind': 'volte', 'type': 'call_end',
                               'detail': f"{ini} → {cal} ({d.get('duration', 0)}s, {d.get('end_reason', '')})", 'ref': cid})

    # PTT surrogate → mcptt_group_id 매핑
    sur2g = {}
    for gj in glob.glob(os.path.join(base, 'ptt', '*', 'group.json')):
        try:
            with open(gj) as f:
                sur2g[os.path.basename(os.path.dirname(gj))] = json.load(f).get('mcptt_group_id')
        except Exception:
            pass

    for (Y, M, D, H) in buckets:
        # floor.jsonl: GRANT/RELEASE/REJECT
        for fp in glob.glob(os.path.join(base, 'ptt', '*', f'{Y:04d}', f'{M:02d}', f'{D:02d}', f'{H:02d}', 'floor.jsonl')):
            parts = fp.split(os.sep)
            g = sur2g.get(parts[-6]) or parts[-6]
            try:
                with open(fp) as f:
                    for line in f:
                        try:
                            ev = json.loads(line)
                        except Exception:
                            continue
                        op, user, ts = ev.get('op'), ev.get('user', ''), ev.get('ts')
                        if not ts:
                            continue
                        if op == 'GRANT':
                            events.append({'ts': ts, 'kind': 'ptt', 'type': 'floor_grant', 'detail': f"{g}: {user} 발언 시작", 'ref': g})
                        elif op == 'RELEASE':
                            events.append({'ts': ts, 'kind': 'ptt', 'type': 'floor_release', 'detail': f"{g}: 발언 종료", 'ref': g})
                        elif op == 'REJECT':
                            events.append({'ts': ts, 'kind': 'ptt', 'type': 'floor_reject', 'detail': f"{g}: {user} 요청 거부 ({ev.get('reason', '')})", 'ref': g})
            except Exception:
                pass
        # events.jsonl: member_join/leave
        for fp in glob.glob(os.path.join(base, 'ptt', '*', f'{Y:04d}', f'{M:02d}', f'{D:02d}', f'{H:02d}', 'events.jsonl')):
            parts = fp.split(os.sep)
            g = sur2g.get(parts[-6]) or parts[-6]
            try:
                with open(fp) as f:
                    for line in f:
                        try:
                            ev = json.loads(line)
                        except Exception:
                            continue
                        t, ts = ev.get('type'), ev.get('ts')
                        if not ts:
                            continue
                        if t == 'member_join':
                            events.append({'ts': ts, 'kind': 'ptt', 'type': 'member_join', 'detail': f"{g}: {ev.get('member', '')} 입장", 'ref': g})
                        elif t == 'member_leave':
                            events.append({'ts': ts, 'kind': 'ptt', 'type': 'member_leave', 'detail': f"{g}: {ev.get('member', '')} 퇴장", 'ref': g})
            except Exception:
                pass

    events.sort(key=lambda e: e['ts'], reverse=True)
    return events[:200]


async def _service_events(config: dict, limit='60') -> HandlerResult:
    """최근 서비스 이벤트 피드 — VoLTE 호 시작/종료, PTT floor GRANT/RELEASE/REJECT,
       멤버 입장/퇴장을 로그에서 모아 시각 역순. 3초 캐시."""
    try:
        limit = max(1, min(int(limit), 200))
    except (TypeError, ValueError):
        limit = 60
    events = _cached('svc_events', lambda: _build_service_events(config))
    return HandlerResult(status=200, body={'events': events[:limit]})


_ORG_TREE_CACHE: dict = {'ts': 0, 'data': None}


def _org_tree(config: dict) -> dict:
    """조직 트리 (10s 캐시) → {'nodes': {code:{code,name,parent,sort}}, 'children': {parent_code:[codes]}}."""
    now = time.time()
    c = _ORG_TREE_CACHE
    if c.get('data') and now - c.get('ts', 0) < 10:
        return c['data']
    rows = []
    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute("SELECT id, code, name, parent_id, sort_order FROM organizations")
                rows = cur.fetchall()
    except Exception:
        rows = []
    id2code = {r['id']: r['code'] for r in rows}
    nodes, children = {}, {}
    for r in rows:
        pc = id2code.get(r['parent_id'])
        nodes[r['code']] = {'code': r['code'], 'name': r['name'], 'parent': pc, 'sort': r['sort_order'] or 0}
        children.setdefault(pc, []).append(r['code'])
    for k in children:
        children[k].sort(key=lambda cc: (nodes.get(cc, {}).get('sort', 0), cc))
    data = {'nodes': nodes, 'children': children}
    c['ts'], c['data'] = now, data
    return data


def _org_paths(config: dict) -> dict:
    """{code: 'CIMS > 제1본부 > 팀02'} — 가입자 부서 경로 표시용."""
    nodes = _org_tree(config)['nodes']
    out = {}
    for code in nodes:
        names, cur, seen = [], code, set()
        while cur and cur in nodes and cur not in seen:
            seen.add(cur)
            names.append(nodes[cur]['name'])
            cur = nodes[cur]['parent']
        out[code] = ' > '.join(reversed(names))
    return out


def _org_descendants(config: dict, code: str):
    """code 와 그 모든 하위 조직 코드 (subscribers org 필터용 — users.org_id 는 leaf 팀코드)."""
    ch = _org_tree(config)['children']
    out, stack = [], [code]
    while stack:
        x = stack.pop()
        out.append(x)
        stack.extend(ch.get(x, []))
    return out


async def _service_org(config: dict) -> HandlerResult:
    """조직 트리별 이용 — 회사>본부>팀 트리 + 구성원/등록/활성 롤업(상위=하위 합)."""
    volte_states = _load_active_states(config, 'volte')
    ptt_states = _load_active_states(config, 'ptt')
    av = {st.get('subscriber_id') for st in volte_states if st.get('subscriber_id')}
    ap = {st.get('subscriber_id') for st in ptt_states if st.get('subscriber_id')}
    talkers = set()
    for nd in _all_media_stats(config):
        for gd in (nd['stats'].get('group_details') or []):
            if gd.get('floor_holder'):
                talkers.add(gd['floor_holder'])

    UNSET = '(미지정)'
    KEYS = ('members', 'volte_reg', 'ptt_reg', 'active_volte', 'active_ptt', 'ptt_talking')
    leaf = {}   # org_id(leaf 팀코드) → stats

    def L(code):
        return leaf.setdefault(code, {k: 0 for k in KEYS})

    m2o = {}
    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT COALESCE(NULLIF(u.org_id,''),%s) AS code, COUNT(DISTINCT u.id) AS members, "
                    "COUNT(DISTINCT CASE WHEN vs.register_time IS NOT NULL AND (vs.logout_time IS NULL OR vs.register_time>vs.logout_time) THEN vs.id END) AS volte_reg, "
                    "COUNT(DISTINCT CASE WHEN ps.register_time IS NOT NULL AND (ps.logout_time IS NULL OR ps.register_time>ps.logout_time) THEN ps.id END) AS ptt_reg "
                    "FROM users u LEFT JOIN volte_subscriptions vs ON vs.user_id=u.id "
                    "LEFT JOIN ptt_subscriptions ps ON ps.user_id=u.id GROUP BY code", (UNSET,))
                for r in cur.fetchall():
                    d = L(r['code'])
                    d['members'] = int(r['members'] or 0)
                    d['volte_reg'] = int(r['volte_reg'] or 0)
                    d['ptt_reg'] = int(r['ptt_reg'] or 0)
                allact = list(av | ap | talkers)
                if allact:
                    ph = ','.join(['%s'] * len(allact))
                    cur.execute(
                        f"SELECT vs.id AS m, COALESCE(NULLIF(u.org_id,''),%s) AS o FROM volte_subscriptions vs JOIN users u ON u.id=vs.user_id WHERE vs.id IN ({ph}) "
                        f"UNION SELECT ps.id, COALESCE(NULLIF(u.org_id,''),%s) FROM ptt_subscriptions ps JOIN users u ON u.id=ps.user_id WHERE ps.id IN ({ph})",
                        tuple([UNSET] + allact + [UNSET] + allact))
                    for r in cur.fetchall():
                        m2o[r['m']] = r['o']
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})

    for ms in av:
        L(m2o.get(ms, UNSET))['active_volte'] += 1
    for ms in ap:
        L(m2o.get(ms, UNSET))['active_ptt'] += 1
    for ms in talkers:
        L(m2o.get(ms, UNSET))['ptt_talking'] += 1

    tree = _org_tree(config)
    nodes, children = tree['nodes'], tree['children']
    rollup = {}

    def _roll(code):
        agg = {k: leaf.get(code, {}).get(k, 0) for k in KEYS}
        for ch in children.get(code, []):
            sub = _roll(ch)
            for k in KEYS:
                agg[k] += sub[k]
        rollup[code] = agg
        return agg

    roots = children.get(None, [])
    for rc in roots:
        _roll(rc)

    out = []

    def _dfs(code, depth):
        n = nodes[code]
        out.append({'code': code, 'name': n['name'], 'parent': n['parent'], 'depth': depth,
                    **rollup.get(code, {k: 0 for k in KEYS})})
        for ch in children.get(code, []):
            _dfs(ch, depth + 1)

    for rc in roots:
        _dfs(rc, 0)
    # 트리에 없는 leaf(미지정 등)
    for code in leaf:
        if code not in nodes:
            out.append({'code': code, 'name': code, 'parent': None, 'depth': 0,
                        **{k: leaf[code][k] for k in KEYS}})
    return HandlerResult(status=200, body={'orgs': out})


async def _ptt_members(config: dict, group: str, page='1', limit='50') -> HandlerResult:
    """그룹 멤버 on-demand 페이지네이션 (그룹당 100~200명 → 비인라인 drill)."""
    group = (group or '').strip()
    try:
        page = max(1, int(page))
    except (TypeError, ValueError):
        page = 1
    try:
        limit = min(200, max(1, int(limit)))
    except (TypeError, ValueError):
        limit = 50
    if not group:
        return HandlerResult(status=400, body={'error': 'group required'})
    offset = (page - 1) * limit
    # 현재 발언/참여 표시용
    ptt_states = _load_active_states(config, 'ptt')
    active_in_group = {st.get('subscriber_id') for st in ptt_states
                       if st.get('group_id') == group and st.get('subscriber_id')}
    floor_holder = None
    for nd in _all_media_stats(config):
        for gd in (nd['stats'].get('group_details') or []):
            if gd.get('group_id') == group and gd.get('floor_holder'):
                floor_holder = gd['floor_holder']
    members = []
    total = 0
    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT COUNT(*) AS c FROM ptt_group_members m JOIN ptt_groups g ON g.id=m.group_id "
                    "WHERE g.mcptt_group_id=%s", (group,))
                total = cur.fetchone()['c']
                cur.execute(
                    "SELECT m.user_id AS msisdn, m.role, m.priority, u.name "
                    "FROM ptt_group_members m JOIN ptt_groups g ON g.id=m.group_id "
                    "LEFT JOIN ptt_subscriptions ps ON ps.id=m.user_id "
                    "LEFT JOIN users u ON u.id=ps.user_id "
                    "WHERE g.mcptt_group_id=%s ORDER BY m.priority, m.user_id LIMIT %s OFFSET %s",
                    (group, limit, offset))
                for r in cur.fetchall():
                    members.append({'msisdn': r['msisdn'], 'name': r.get('name') or '',
                                    'role': r.get('role') or 'member', 'priority': r.get('priority'),
                                    'active': r['msisdn'] in active_in_group,
                                    'talking': r['msisdn'] == floor_holder})
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})
    return HandlerResult(status=200, body={
        'group': group, 'total': total, 'page': page, 'limit': limit,
        'active_count': len(active_in_group), 'floor_holder': floor_holder,
        'members': members,
    })


# base(노드 health/messages/leak/subscribers) — base OAM 귀속.
CIMS_STATS_HANDLER_LIST = [
    (_STATS_BASE, handle_stats, {}),
]

# service KPI(/api/v1/stats/service/*) — oam-svc 모듈 귀속.
# controller 최장 일치 덕에 _STATS_BASE 와 충돌 없이 공존(longest match → service).
CIMS_STATS_SERVICE_HANDLER_LIST = [
    (_STATS_SERVICE_BASE, handle_stats_service, {}),
]
