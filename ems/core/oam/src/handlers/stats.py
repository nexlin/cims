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
import re
import socket
import time
import asyncio
import logging
import functools
import threading
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta
from urllib.parse import urlparse, parse_qs, unquote
from pathlib import PurePath

import pymysql
import pymysql.cursors

from httpsrv.handler import HandlerArgs, HandlerResult

from services import access_services, stats_rollup
from services.stats_rollup import _pdd_ms, _rate

logger = logging.getLogger(__name__)

# 클라이언트 응답용 공통 에러 바디 — 원인 상세(호스트/계정 힌트가 실리는 DB 예외 문자열 등)는
# 화면에 노출하지 않고 oam 로그에만 남긴다.
_ERR_INTERNAL = {'error': 'stats query failed (oam 로그 참조)'}


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

_PROBE_TIMEOUT = 1.2    # 시도당 UDP 응답 대기(초)
_PROBE_ATTEMPTS = 2     # 총 시도 횟수 — 단발 데이터그램 유실을 재전송으로 복구

# probe 총 예산(timeout × attempts)은 게이트웨이 프록시 타임아웃(gateway._DEFAULT_TIMEOUT, 5s)
# 보다 확실히 작아야 한다. 노드 probe 는 병렬이므로 노드 수와 무관하게 이 값이 상한이다.


def _probe_params(config: dict):
    """(timeout_sec, attempts) — MediaServer.ProbeTimeoutMs / ProbeAttempts 로 조정."""
    ms = config.get('MediaServer', {}) or {}
    try:
        timeout = float(ms.get('ProbeTimeoutMs', _PROBE_TIMEOUT * 1000)) / 1000.0
    except (TypeError, ValueError):
        timeout = _PROBE_TIMEOUT
    try:
        attempts = int(ms.get('ProbeAttempts', _PROBE_ATTEMPTS))
    except (TypeError, ValueError):
        attempts = _PROBE_ATTEMPTS
    return max(timeout, 0.1), max(attempts, 1)


def _udp_request(ip: str, port: int, data: dict,
                 timeout: float = _PROBE_TIMEOUT, attempts: int = 1) -> dict:
    """UDP로 JSON 요청 보내고 응답 수신. timeout 단축(down 서버 fail-fast).
    UDP 는 재전송이 없어 데이터그램 한 개만 유실돼도 timeout 전액을 문다 →
    attempts 회까지 재시도(시도마다 새 소켓). 전부 실패하면 {}. 최악 = timeout × attempts."""
    msg = json.dumps(data).encode('utf-8')
    for _ in range(max(attempts, 1)):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.settimeout(timeout)
                sock.sendto(msg, (ip, port))
                resp_data, _addr = sock.recvfrom(4096)
            return json.loads(resp_data.decode('utf-8'))
        except Exception:
            continue
    return {}


# csp/cmp 상태 단기 캐시 — down 서버 probe 가 timeout 까지 블로킹하므로, 다중 위젯/스위퍼의
# 반복 요청이 매번 probe 하지 않도록 TTL 캐시. (정상 서버는 즉시 응답하므로 영향 미미.)
# 캐시는 uvicorn 루프 스레드(to_thread 워커)·_PROBE_POOL·oam-svc 메인 스레드(alarm_sweeper)
# 에서 동시에 접근한다 → 락 필수.
_STATS_CACHE: dict = {}
_STATS_TTL = 3.0
_CACHE_LOCK = threading.Lock()   # _STATS_CACHE / _CMP_LAST_GOOD / _INFLIGHT 보호
_INFLIGHT: dict = {}             # key → threading.Event (해당 키 갱신 진행 중 표식)


def _cached(key: str, producer):
    """TTL 캐시 + single-flight + stale-while-revalidate.

    producer 는 락 밖에서 실행하고 반환 '후' 시각으로 스탬프한다 — 느린 probe 가 자기
    TTL 을 갉아먹지 않도록(2.4s probe + 3.0s TTL 이면 예전엔 잔여 0.6s 였다).
    같은 키를 동시에 miss 한 스레드는 producer 를 중복 실행하지 않는다(single-flight).
    갱신 중인 키에 stale 값이 있으면 즉시 반환 — down 노드가 매 요청에 timeout 을
    물리지 않는다.
    """
    with _CACHE_LOCK:
        e = _STATS_CACHE.get(key)
        if e and time.time() - e[0] < _STATS_TTL:
            return e[1]
        ev = _INFLIGHT.get(key)
        if ev is not None:
            if e is not None:
                return e[1]          # SWR — 갱신은 leader 에게 맡기고 stale 반환
            leader = False
        else:
            ev = _INFLIGHT[key] = threading.Event()
            leader = True

    if not leader:                   # 캐시가 아직 비어 있는 최초 채움만 대기
        ev.wait(timeout=_STATS_TTL)
        with _CACHE_LOCK:
            e = _STATS_CACHE.get(key)
        return e[1] if e else {}

    v = {}
    try:
        v = producer()               # 락 밖 — 오래 걸려도 다른 키를 막지 않는다
    finally:
        with _CACHE_LOCK:
            _STATS_CACHE[key] = (time.time(), v)   # producer '이후' 시각
            _INFLIGHT.pop(key, None)
        ev.set()
    return v


def _get_csp_stats(config: dict) -> dict:
    """CSP에 stats 요청 (3s 캐시)."""
    def probe():
        notify = config.get('CspNotify', {})
        ip = notify.get('Ip', '127.0.0.1')
        port = int(notify.get('Port', 4421))
        timeout, attempts = _probe_params(config)
        resp = _udp_request(ip, port, {"event": "STATS_REQUEST", "uri": "", "action": ""},
                            timeout=timeout, attempts=attempts)
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


def _cmp_stats_request(ip: str, port: int, timeout: float = 1.0,
                       attempts: int = 1) -> dict:
    """CMP STATS 조회 (envelope v2 — docs/api/cmp_media_api.md).
       응답 payload {resource, detail} 를 대시보드가 쓰는 flat 키로 정규화해 반환."""
    resp = _udp_request(ip, port, {
        "hdr": {"ver": 2, "trans_id": int(time.time()) % 100000,
                "node": "oam", "cmd": "STATS", "type": "request"}
    }, timeout=timeout, attempts=attempts)
    hdr = resp.get('hdr') or {}
    if hdr.get('status') != 'OK':
        return {}
    p = resp.get('payload') or {}
    res = p.get('resource') or {}
    relay = res.get('relay') or {}
    ptt = res.get('ptt') or {}
    det = p.get('detail') or {}
    leak = det.get('leak_reclaim') or {}
    return {
        'sessions': relay.get('sessions', 0),
        'groups': ptt.get('groups', 0),
        'rtp_ports_total': relay.get('total', 0),
        'rtp_ports_used': relay.get('used', 0),
        'rtp_ports_free': relay.get('total', 0) - relay.get('used', 0),
        'ptt_rtp_ports_total': ptt.get('total', 0),
        'ptt_rtp_ports_used': ptt.get('used', 0),
        'ptt_rtp_ports_free': ptt.get('total', 0) - ptt.get('used', 0),
        'session_timeout': det.get('session_timeout', 0),
        'orphan_reclaim_sec': det.get('orphan_reclaim_sec', 0),
        'leak_reclaim_total': leak.get('total', 0),
        'leak_reclaim_orphan': leak.get('orphan', 0),
        'leak_reclaim_hold': leak.get('hold', 0),
        'group_details': det.get('groups', []),
    }


def _get_cmp_stats(config: dict) -> dict:
    """CMP에 stats 요청 (3s 캐시)."""
    def probe():
        cmp_ip = config.get('CmpIp')
        if cmp_ip:
            cmp_port = int(config.get('CmpPort', 9000))
        else:
            # CmpIp 미설정(콘솔 관리 oam-svc 설정) — MediaServer.Endpoints 첫 노드를 대표 probe.
            cmp_ip, cmp_port = _media_endpoints(config)[0]
        timeout, attempts = _probe_params(config)
        return _cmp_stats_request(cmp_ip, cmp_port, timeout=timeout, attempts=attempts)
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

# 노드 probe 전용 풀. 기본 executor(asyncio.to_thread) 와 반드시 분리한다 — _health 는
# 기본 executor 워커에서 _all_media_stats 를 호출하고 그 워커가 여기에 submit 후 대기하므로,
# 같은 풀이면 자기 재submit 기아가 생긴다. 의존은 default → _PROBE_POOL 단방향이고
# _probe_cmp 는 재submit 하지 않는 leaf 라 순환이 없다.
_PROBE_POOL = ThreadPoolExecutor(max_workers=16, thread_name_prefix='cmp-probe')


def _last_good_ttl(config: dict) -> float:
    """probe miss 시 최근 정상값을 몇 초까지 유지할지 — MediaServer.LastGoodTtlSec."""
    ms = config.get('MediaServer', {}) or {}
    try:
        return float(ms.get('LastGoodTtlSec', _CMP_LAST_GOOD_TTL))
    except (TypeError, ValueError):
        return _CMP_LAST_GOOD_TTL


def _probe_cmp(ip: str, port: int, timeout: float = _PROBE_TIMEOUT,
               attempts: int = _PROBE_ATTEMPTS, last_good_ttl: float = _CMP_LAST_GOOD_TTL) -> dict:
    """단일 CMP 노드 STATS probe (노드별 3s 캐시).
       부하 중 CMP STATS 응답이 timeout 을 넘겨 일시 miss 되면 노드가 used=0/down 으로
       튀어 대시보드 RTP 합계가 요동친다 → miss 시 last_good_ttl 이내 최근 정상값을 유지해
       한 번의 타임아웃으로 0 이 되지 않게 한다. 유실 복구는 _udp_request 의 attempts 재시도."""
    key = f'{ip}:{port}'

    def probe():
        r = _cmp_stats_request(ip, port, timeout=timeout, attempts=attempts) or None
        now = time.time()
        if r:
            with _CACHE_LOCK:
                _CMP_LAST_GOOD[key] = (now, r)
            return r
        with _CACHE_LOCK:              # probe miss → 최근 정상값 유지(연속 실패 ttl 까지)
            lg = _CMP_LAST_GOOD.get(key)
        if lg and now - lg[0] < last_good_ttl:
            return lg[1]
        return {}
    return _cached(f'cmp:{ip}:{port}', probe)


def _all_media_stats(config: dict):
    """전 미디어 노드 STATS — [{host, port, stats}].

    노드별 동시 probe — N개 노드 비용이 N×timeout 이 아니라 max(timeout) 이다.
    (직렬이면 down 노드 2개에서 게이트웨이 프록시 타임아웃 5s 를 넘긴다.)
    실행 중인 이벤트 루프를 가정할 수 없어(alarm_sweeper 는 일반 스레드에서 호출)
    asyncio 대신 전용 스레드 풀을 쓴다. 엔드포인트 순서는 보존한다(합산·표시 순서).
    """
    eps = _media_endpoints(config)
    timeout, attempts = _probe_params(config)
    ttl = _last_good_ttl(config)
    if len(eps) == 1:                  # 단일 노드 — 풀 왕복 비용 회피
        ip, port = eps[0]
        return [{'host': ip, 'port': port,
                 'stats': _probe_cmp(ip, port, timeout, attempts, ttl)}]
    futs = [(ip, port, _PROBE_POOL.submit(_probe_cmp, ip, port, timeout, attempts, ttl))
            for ip, port in eps]
    return [{'host': ip, 'port': port, 'stats': f.result()} for ip, port, f in futs]


def _floor_holders(gd: dict) -> list:
    """CMP STATS group_details 항목의 발언자 목록.

    CMP 는 동시 발언(dual/multi-talker)을 담도록 `floor_holders` 배열로 알린다
    (docs/api/cmp_media_api.md §5.3). 구버전 CMP 노드는 단일 `floor_holder` 만
    보내므로 함께 받아준다 — 혼재 배포에서 콘솔이 발언자를 놓치지 않도록.
    """
    hs = gd.get('floor_holders')
    if isinstance(hs, list):
        return [h for h in hs if h]
    h = gd.get('floor_holder')
    return [h] if h else []


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

def _offload(fn):
    """동기 핸들러를 스레드로 오프로드 — 이벤트 루프 스톨 방지.

    이 모듈의 핸들러 본문은 UDP probe·NFS glob·DB 조회 같은 블로킹 I/O 로 이루어져 있다.
    단일 이벤트 루프(uvicorn) 위에서 직접 실행하면 한 핸들러의 지연이 그 순간 처리 중인
    모든 요청을 함께 죽인다. external_systems._probe_result 와 같은 to_thread 패턴.
    """
    @functools.wraps(fn)
    async def _wrapped(*args, **kwargs):
        return await asyncio.to_thread(fn, *args, **kwargs)
    return _wrapped


_STATS_BASE = '/api/v1/stats'


async def handle_stats(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    # full_path 는 경로만 담고 query string 은 별도(query_params dict)로 전달된다
    # (controller 가 Starlette request.query_params 를 그대로 넣음, 이미 URL-decode 됨).
    qs = handler_args.query_params or {}
    parts = _path_parts(handler_args.full_path, _STATS_BASE)
    method = handler_args.method.upper()

    # /stats/calls/rebuild 만 POST — 나머지는 조회 전용.
    if method != 'GET' and not (len(parts) > 1 and parts[0] == 'calls' and parts[1] == 'rebuild'):
        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

    def qp(name, default=None):
        v = qs.get(name)
        return v if v not in (None, '') else default

    try:
        if len(parts) == 0:
            return HandlerResult(status=200, body={'endpoints': [
                '/api/v1/stats/health', '/api/v1/stats/messages',
                '/api/v1/stats/calls', '/api/v1/stats/calls/rebuild',
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
            gran = qp('granularity', '1h')
            svc = (qp('svc', 'all') or 'all').lower()
            # SIP 축만 1분 집계가 있다(다른 인터페이스는 집계 대상이 아니다). 집계가 있으면
            # 그쪽으로, 없으면 옛 원본 스캔으로 — 도입 전 구간·롤업 비활성이 여기로 온다.
            if iface == 'sip' and gran in stats_rollup.GRANULARITIES:
                d = qp('date')
                f = _norm_dt(qp('from') or (d or datetime.now().strftime('%Y-%m-%d')))
                t = _norm_dt(qp('to') or (qp('from') or d or
                                          datetime.now().strftime('%Y-%m-%d')), end=True)
                f, t, tr = _clamp_calls_range(f, t, gran)
                r = await _messages_stats_rollup(config, f, t, gran, svc, d, tr)
                if r.status == 200:
                    return r
                # 204 = 그 구간에 집계 없음 → 폴백
            # 구간 조회 — from/to + granularity. date 는 "그 날 하루" 축약(하위 호환).
            if gran not in _GRAN_MAX_DAYS:
                gran = '1h'      # 옛 스캔 경로는 5m/10m/1h/1d 만 안다
            return await _messages_stats_v2(config, iface, qp('date'),
                                            qp('from'), qp('to'), gran)

        if parts[0] == 'calls' and len(parts) > 1 and parts[1] == 'rebuild':
            # 재집계는 변이 — 조회와 달리 admin 을 요구한다.
            from services.admin_auth import require_role
            _p, deny = require_role(handler_args, 'admin')
            if deny:
                return deny
            if method != 'POST':
                return HandlerResult(status=405, body={'error': 'POST only'})
            d = qp('date')
            f = (qp('from') or d or '')[:10]
            t = (qp('to') or d or f)[:10]
            if len(f) != 10 or len(t) != 10:
                return HandlerResult(status=400,
                                     body={'error': 'from/to (YYYY-MM-DD) 또는 date 필요'})
            return await _calls_rebuild(config, f, t)

        if parts[0] == 'calls':
            gran = qp('granularity', '1h')
            if gran not in stats_rollup.GRANULARITIES:
                return HandlerResult(status=400, body={
                    'error': 'invalid granularity',
                    'allowed': list(stats_rollup.GRANULARITIES)})
            svc = (qp('svc', 'all') or 'all').lower()
            if svc not in ('all', 'volte', 'ptt', 'unknown'):
                return HandlerResult(status=400, body={
                    'error': 'invalid svc', 'allowed': ['all', 'volte', 'ptt', 'unknown']})
            # date=YYYY-MM-DD 는 "그 날 하루" 축약 — messages 축과 같은 규약.
            d = qp('date')
            f = _norm_dt(qp('from') or (d or ''))
            t = _norm_dt(qp('to') or (d or ''), end=True)
            if not f or not t:
                return HandlerResult(status=400, body={'error': 'from/to 또는 date 필요'})
            f, t, truncated = _clamp_calls_range(f, t, gran)
            r = await _calls_stats(config, f, t, gran, svc)
            if truncated and isinstance(r.body, dict):
                r.body['truncated'] = True
            return r

        if parts[0] == 'leak-reclaims':
            return await _leak_reclaims(config, qp('date'))

        # NOTE: service/* (KPI 관측) 는 oam-svc 모듈 귀속 (oam_base_service_split §4).
        # 별도 핸들러 handle_stats_service(_STATS_SERVICE_BASE) 가 처리한다.
        # --role all 에서는 두 핸들러가 모두 등록되며 controller 최장 일치로
        # /api/v1/stats/service/* 는 handle_stats_service 로 라우팅된다(여기 도달 안 함).
        # --role base 에서는 service 핸들러 미등록 → service/* 는 여기서 404 (격리).

        return HandlerResult(status=404, body={'error': 'Not Found'})
    except Exception as e:
        logger.exception('stats handler error: %s', e)
        return HandlerResult(status=500, body=_ERR_INTERNAL)


# ──────────────────────────────────────────────────────────────
#  호 통계 조회 — 1분 기저 집계 위에서 (sip_statistics.md §7)
# ──────────────────────────────────────────────────────────────

# 작은 단위 조회 상한 — 막는 것은 스캔 비용이 아니라 **버킷 수**다. 1분 30일이면 43200 칸이라
# 화면이 읽지 못한다. 콘솔 page-filter(`GRAN_MAX_DAYS`)와 **같은 값**을 쓴다 — 한쪽만 느슨하면
# 화면이 못 고르는 조합을 API 만 받아 두 경로의 동작이 갈린다.
# 1h·1d 는 서버에서 막지 않는다: 계층(1h·1d)이 있어 합산이 싸고, 화면 가독성 상한은
# 콘솔이 갖는다(63일 1d 조회가 여기 걸리면 안 된다).
_CALLS_MAX_DAYS = {'1m': 2, '5m': 3, '10m': 7}


@_offload
def _calls_rebuild(config: dict, from_day: str, to_day: str) -> HandlerResult:
    """1분 집계를 원본에서 다시 만든다 (운영/검증용).

    왜 필요한가: 롤업은 미결·신규 버킷만 다시 계산하므로, **집계 스키마에 축이 추가되면
    이미 적힌 버킷은 그 축이 빈 채로 남는다**(그룹 축 추가 때 실측). 첫 기동 소급이 1일치인
    것도 같은 이유로 과거를 채울 수단이 필요하다.

    watermark 는 건드리지 않는다 — 과거 재생성이 이후의 정상 집계를 되돌리면 안 된다.
    """
    if not stats_rollup.enabled():
        return HandlerResult(status=409, body={
            'error': 'rollup_disabled',
            'hint': 'StatsRollup.Enabled 가 꺼져 있으면 집계 파일을 만들지 않습니다'})
    n = stats_rollup.rebuild_range(from_day, to_day)
    return HandlerResult(status=200, body={
        'ok': True, 'from': from_day, 'to': to_day, 'buckets': n})


def _source_of(cov: dict) -> str:
    """조회가 어디서 왔는지 — rollup(집계) / scan(원본 즉석) / mixed(섞임) / none."""
    r, sc = cov.get('rollup', 0), cov.get('scanned', 0)
    if r and sc:
        return 'mixed'
    if r:
        return 'rollup'
    if sc:
        return 'scan'
    return 'none'


def _clamp_calls_range(from_dt: str, to_dt: str, gran: str):
    """작은 단위의 조회 구간을 자른다. 잘렸으면 (from, to, True).

    제한하는 것은 스캔 비용이 아니라 **화면에 그릴 버킷 수**다 — 큰 단위는 1분 합산이라
    비용이 거의 없어 상한을 두지 않는다. 끝을 기준으로 뒤에서 자른다(최근 구간이 관심사).
    """
    lim = _CALLS_MAX_DAYS.get(gran)
    if not lim:
        return from_dt, to_dt, False
    try:
        f = datetime.strptime(from_dt[:19], '%Y-%m-%d %H:%M:%S')
        t = datetime.strptime(to_dt[:19], '%Y-%m-%d %H:%M:%S')
    except Exception:
        return from_dt, to_dt, False
    if (t - f).days <= lim:
        return from_dt, to_dt, False
    return (t - timedelta(days=lim)).strftime('%Y-%m-%d %H:%M:%S'), to_dt, True


@_offload
def _calls_stats(config: dict, from_dt: str, to_dt: str, gran: str, svc: str) -> HandlerResult:
    """서비스별 호 KPI — 1분 집계를 요청 단위로 접어 낸다.

    집계가 없는 구간(롤업 도입 전, `StatsRollup.Enabled=false`)은 **원본을 그 자리에서
    같은 함수로 접어** 답한다. 폴백에 별도 계산식을 두면 두 경로가 서서히 어긋나므로
    집계와 조회가 같은 `build_minutes`/`aggregate` 를 쓴다. 응답의 `source` 로 어느
    경로였는지 알린다.
    """
    root = _service_log_dir(config)
    if not root:
        return HandlerResult(status=200, body={
            'from': from_dt, 'to': to_dt, 'granularity': gran, 'svc': svc,
            'source': 'none', 'totals': {}, 'buckets': [],
            'hint': 'ServiceLogging.Dir 미설정'})

    rows, cov = stats_rollup.read_range_filled(root, from_dt, to_dt, config, gran=gran)
    buckets, totals = stats_rollup.aggregate(rows, gran, svc)
    body = {
        'from': from_dt, 'to': to_dt, 'granularity': gran, 'svc': svc or 'all',
        'source': _source_of(cov), 'coverage': cov,
        'totals': totals, 'buckets': buckets,
    }
    if cov.get('missing'):
        # 보존기간 밖이라 빠진 구간을 **응답에 적는다** — 조용히 작은 값을 내면 운영자가
        # 그 감소를 실제 트래픽 변화로 읽는다.
        body['warning'] = (f"{cov['missing']}일이 집계 보존기간을 넘어 제외됐습니다 "
                           f"(ServiceLogging.StatsRetainDays.1m). 필요하면 보존기간을 늘리고 "
                           f"POST /api/v1/stats/calls/rebuild 로 다시 만드세요.")
    return HandlerResult(status=200, body=body)


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
        logger.exception('stats handler error: %s', e)
        return HandlerResult(status=500, body=_ERR_INTERNAL)


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
    # envelope v2(cmp_media_api.md)는 명령을 `hdr` 에 둔다 — 여기를 안 보면 CMP 전량이
    # 'json' 한 덩어리가 되어 메서드 분포가 무의미해진다.
    if first.startswith('{'):
        try:
            j = json.loads(msg)
            payload = j.get('payload') if isinstance(j.get('payload'), dict) else {}
            hdr = j.get('hdr') if isinstance(j.get('hdr'), dict) else {}
            for src in (hdr, payload, j):
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
        code = tok[1] if len(tok) > 1 else 'response'
        # 응답은 **어느 트랜잭션의 응답인지**까지 세야 쓸 수 있다. 상태코드만 세면 INVITE 의
        # 200(호 성립)·BYE 의 200(호 해제)·REGISTER 의 200(등록)이 한 칸에 합쳐진다.
        # 근거는 CSeq 헤더(RFC 3261 §8.1.1.5 — 응답의 CSeq 는 요청의 것을 그대로 복사).
        # 메서드를 앞에 두는 이유: 정렬하면 트랜잭션끼리 붙어 표에서 눈이 따라가기 쉽다
        #   (BYE, BYE/200, CANCEL, INVITE, INVITE/180, INVITE/200).
        cseq = _cseq_method(msg)
        return f'{cseq}/{code}' if cseq else code
    method = tok[0].upper()
    return method if method in _SIP_REQUEST_METHODS else method


_CSEQ_RE = re.compile(r'^CSeq\s*:\s*\d+\s+([A-Za-z]+)\s*$', re.IGNORECASE | re.MULTILINE)


def _cseq_method(msg: str) -> str:
    """응답 원문의 `CSeq: <n> <METHOD>` 에서 메서드. 없으면 '' (구 로그·비정상 메시지)."""
    head = msg.split('\r\n\r\n', 1)[0].replace('\r\n', '\n')
    m = _CSEQ_RE.search(head)
    return m.group(1).upper() if m else''


@_offload
def _leak_reclaims(config, date=None) -> HandlerResult:
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


# 서비스 판정(도메인) — CSP `CCspServiceMap::BuildDomainToKindMap` 과 **같은 규칙**이다:
# enabled 인 access service 를 priority 오름차순으로 보고, 도메인마다 첫 항목의 kind 를 남긴다.
#
# 왜 로그의 필드가 아니라 여기서 다시 가르는가: 메시지 로그(`*.msg.jsonl`)는 `service` 를 적지
# 않는다(그 필드는 flow 줄에만 있다). 이미 쌓인 로그를 그대로 쓰려면 소비자가 판정해야 한다.
#
# 왜 도메인인가: 3GPP ICSI(`+g.3gpp.icsi-ref`)가 규격상 정본이지만 **현장 데이터가 신뢰할 수
# 없다** — 단말이 MCPTT 호에 `icsi.mcdata.sds` 를 붙이는 사례가 확인됐다(본문은
# `application/vnd.3gpp.mcptt-info+xml` + `m=application ... UDP MCPTT`). 붙지 않는 호도 많다.
# 도메인은 접속 서비스 정의가 소유하는 값이라 전 구간에서 일관된다.
def _domain_service_map(config: dict) -> dict:
    """{도메인 → kind} — 접속 서비스 정의 기반. 못 읽으면 빈 dict."""
    return access_services.domain_kind_map(config)


_DOMAIN_IN_URI = re.compile(r'@([A-Za-z0-9._-]+)')


def _classify_service(msg: str, dmap: dict) -> str:
    """SIP 원문에서 서비스 판정. Request-URI → To → From 순으로 첫 매치(CSP 와 같은 순서).

    응답(SIP/2.0 …)은 Request-URI 가 없어 To/From 만 본다.
    """
    if not msg or not dmap:
        return ''
    head = msg.split('\r\n\r\n', 1)[0]
    lines = head.split('\r\n')
    cands = []
    first = lines[0] if lines else ''
    if not first.startswith('SIP/2.0'):
        parts = first.split(' ')
        if len(parts) > 1:
            cands.append(parts[1])
    for tag in ('to:', 't:', 'from:', 'f:'):
        for ln in lines[1:]:
            low = ln.lower()
            if low.startswith(tag):
                cands.append(ln)
                break
    for c in cands:
        for dom in _DOMAIN_IN_URI.findall(c):
            kind = dmap.get(dom.lower().split(':')[0])
            if kind:
                return kind
    return ''


_DATE_FROM_PATH = re.compile(r'/(\d{4})/(\d{2})/(\d{2})/\d{2}/[^/]+$')


def _ts_full(fpath: str, ts: str) -> str:
    """로그 줄의 ts + 파일 경로의 날짜 → 'YYYY-MM-DD HH:MM:SS'.

    로그의 `ts` 는 **시각만** 담는다(`'21:00:00.102885'`) — 날짜는 `{루트}/YYYY/MM/DD/HH/` 경로가
    갖는다. 구간 조회는 날짜까지 있어야 비교되므로 여기서 합친다.
    (날짜가 붙은 ts 를 쓰는 줄도 그대로 통과시킨다 — 형식이 바뀌어도 깨지지 않게.)
    """
    ts = (ts or '').strip()
    if len(ts) >= 10 and ts[4] == '-':
        return ts[:19]
    m = _DATE_FROM_PATH.search(fpath.replace('\\', '/'))
    if not m:
        return ts[:19]
    return f'{m.group(1)}-{m.group(2)}-{m.group(3)} {ts[:8]}'


def _svc_bucket(label: str, count: int, svc_counts: dict, io_counts: dict = None) -> dict:
    """한 버킷의 서비스축 분해 — 셋이 **서로 겹치지 않고** 합이 count 다:
        volte + ptt + unknown == count
    화면의 계열 차트가 이 값들을 그대로 쌓으므로, 겹치는 값을 내면 막대가 이중 계산된다.
    `unknown` = 서비스를 못 가린 줄(옛 CSP 가 남긴 service 없는 로그, 도메인 미등록 등).

    `in`/`out` = 그 버킷의 메서드별 건수(수신/송신). 교차표(시간 × 메서드)가 이걸 읽는다 —
    없으면 인터페이스를 바꿀 때 표만 빈칸이 되어 한 화면의 그림과 표가 다른 것을 본다.
    """
    sv = svc_counts.get(label, {})
    volte, ptt = sv.get('volte', 0), sv.get('ptt', 0)
    io = (io_counts or {}).get(label) or {}
    return {'label': label, 'count': count, 'volte': volte, 'ptt': ptt,
            'unknown': max(count - volte - ptt, 0),
            'in': io.get('in') or {}, 'out': io.get('out') or {}}


@_offload
def _messages_stats_rollup(config, from_dt: str, to_dt: str, gran: str,
                           svc: str, date: str, truncated: bool) -> HandlerResult:
    """SIP 메시지 통계 — 1분 집계 위에서 (sip_statistics.md §9 이행).

    옛 응답 필드(`total`·`method_counts`·`service_totals`·`buckets[].{label,count,volte,
    ptt,unknown}`)를 그대로 내고 통일 키(`bucket`·`bucket_start`)를 **덧붙인다** — 기존
    화면을 깨지 않으면서 1m·1w·1M 과 `svc` 를 열기 위해서다.

    집계가 없는 구간은 호출측이 옛 원본 스캔으로 폴백한다(도입 전 구간·롤업 비활성).
    """
    root = _service_log_dir(config)
    if not root:
        return HandlerResult(status=204, body=None)      # 폴백 신호 (호출측에서만 소비)
    rows, cov = stats_rollup.read_range_filled(root, from_dt, to_dt, config, gran=gran)
    if not rows:
        return HandlerResult(status=204, body=None)

    buckets, totals = stats_rollup.aggregate(rows, gran, svc, include_msg=True)

    def _flat(cell):
        m = cell.get('msg') or {}
        out = {}
        for io in ('in', 'out'):
            for k, v in (m.get(io) or {}).items():
                out[k] = out.get(k, 0) + int(v or 0)
        return out

    method_counts = _flat(totals.get('all') or {})
    total = sum(method_counts.values())
    # 옛 method_service: {메서드: {서비스: 건수}} — 서비스축 분해가 이 축의 핵심이다.
    method_service: dict = {}
    for key, cell in totals.items():
        if key == 'all':
            continue
        for meth, n in _flat(cell).items():
            method_service.setdefault(meth, {})[key] = n

    out_buckets = []
    for b in buckets:
        per = {k: sum(_flat(v).values()) for k, v in b.items()
               if k not in ('bucket', 'bucket_start')}
        row = {
            'bucket': b['bucket'], 'bucket_start': b['bucket_start'],
            'label': b['bucket'],                       # 옛 키
            'count': per.get('all', 0),
            'volte': per.get('volte', 0), 'ptt': per.get('ptt', 0),
            'unknown': per.get('unknown', 0),
            'in': (b.get('all') or {}).get('msg', {}).get('in', {}),
            'out': (b.get('all') or {}).get('msg', {}).get('out', {}),
        }
        if gran == '1h':
            try:
                row['hour'] = int(b['bucket'][11:13])
            except (ValueError, IndexError):
                pass
        out_buckets.append(row)

    svc_totals = {k: sum(_flat(totals.get(k) or {}).values()) for k in ('volte', 'ptt')}
    return HandlerResult(status=200, body={
        'from': from_dt, 'to': to_dt, 'granularity': gran, 'truncated': truncated,
        'date': (date or from_dt[:10]), 'interface': 'sip', 'svc': svc or 'all',
        'source': _source_of(cov), 'coverage': cov,
        'total': total, 'buckets': out_buckets,
        'method_counts': dict(sorted(method_counts.items(), key=lambda x: -x[1])),
        'method_service': method_service,
        'service_totals': svc_totals,
        'voip_invite': (method_service.get('INVITE') or {}).get('volte', 0),
        'ptt_invite': (method_service.get('INVITE') or {}).get('ptt', 0),
        **({'warning': f"{cov['missing']}일이 집계 보존기간을 넘어 제외됐습니다"}
           if cov.get('missing') else {}),
    })


@_offload
def _messages_stats_v2(config, iface, date, from_dt=None, to_dt=None, gran='1h') -> HandlerResult:
    """service_log JSONL 기반 인터페이스별 메시지 통계.

    실제 레이아웃: {ServiceLogDir}/YYYY/MM/DD/HH/csp_01_{sip|cmp|csc}.msg.jsonl
    각 라인 = {ts,dir,peer,caller,callee,sesid,proto,msg}. method 는 msg 본문에서 파싱.
    (옛 MsgLogDir/{comp}/.../{iface}.jsonl 레이아웃 + entry['method'] 가정은 폐기됨.)

    조회 구간은 [from,to] — `date` 는 "그 날 하루" 축약형(하위 호환). 버킷은 `gran`
    단위로 끊고, 단위별 최대 범위(_GRAN_MAX_DAYS)를 넘으면 끝에서부터 잘라낸다.
    """
    import glob as _glob

    # 범위 확정 — from 이 없으면 date(없으면 오늘) 하루로.
    if not from_dt:
        date = date or datetime.now().strftime('%Y-%m-%d')
        from_dt, to_dt = date + ' 00:00:00', date + ' 23:59:59'
    elif not to_dt:
        to_dt = from_dt[:10] + ' 23:59:59'
    from_dt, to_dt = _norm_dt(from_dt), _norm_dt(to_dt, end=True)
    from_dt, to_dt, truncated = _clamp_range(from_dt, to_dt, gran)

    base = _service_log_dir(config)
    empty = {'from': from_dt, 'to': to_dt, 'granularity': gran, 'truncated': truncated,
             'date': (date or from_dt[:10]), 'interface': iface,
             'total': 0, 'buckets': [], 'method_counts': {}, 'method_service': {},
             'service_totals': {'volte': 0, 'ptt': 0}, 'voip_invite': 0, 'ptt_invite': 0}
    if not base:
        return HandlerResult(status=200, body=empty)

    # 스캔 대상 일자 목록 (로그가 YYYY/MM/DD/HH 트리라 날짜 단위로 훑는다)
    try:
        d0 = datetime.strptime(from_dt[:10], '%Y-%m-%d')
        d1 = datetime.strptime(to_dt[:10], '%Y-%m-%d')
    except Exception:
        return HandlerResult(status=200, body=empty)
    days = []
    while d0 <= d1:
        days.append((d0.strftime('%Y'), d0.strftime('%m'), d0.strftime('%d')))
        d0 += timedelta(days=1)

    lo, hi = from_dt[:19], to_dt[:19]
    dmap = _domain_service_map(config)   # 도메인 → kind. 스캔 전 1회.
    counts = {}         # 버킷 라벨 → count
    method_counts = {}  # method/status → count
    # 서비스축 분해 — 로그 줄의 `service` 필드(volte|ptt)를 그대로 쓴다.
    svc_counts = {}     # 버킷 라벨 → {svc → count}
    io_counts = {}      # 버킷 라벨 → {'in'|'out' → {method → count}} — 교차표용
    svc_invite = {}     # svc → INVITE 수 (호 시도 규모 비교용)
    method_svc = {}     # method → {svc → count} — '메서드 비중'을 서비스 계열로 쪼갤 때 쓴다

    if iface == 'https':
        # HTTPS(콘솔/XCAP) — *_ue.msg 에는 본문이 없어 method 불가 → flow 로그의
        # proto=HTTPS 엔트리(method='GET /path', detail='status=NNN')로 집계.
        # (구버전은 unknown iface 가 sip+cmp+csc 전체 합산으로 fallback — 잘못된 수치)
        patterns = [os.path.join(base, y, m, d, '*', '*.flow.jsonl')
                    for (y, m, d) in days]
        patterns += [os.path.join(base, y, m, d, '*', '*.flow.[0-9][0-9].jsonl')
                     for (y, m, d) in days]
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
                                ts = _ts_full(fpath, entry.get('ts'))
                                if ts < lo or ts > hi:
                                    continue
                                k = _bucket_start(ts, gran)
                                counts[k] = counts.get(k, 0) + 1
                                verb = str(entry.get('method', '')).split(' ', 1)[0].upper() or 'unknown'
                                method_counts[verb] = method_counts.get(verb, 0) + 1
                                _io = io_counts.setdefault(k, {}).setdefault('in', {})
                                _io[verb] = _io.get(verb, 0) + 1
                                m = str(entry.get('detail', ''))
                                if m.startswith('status='):
                                    code = m[7:].split()[0]
                                    method_counts[code] = method_counts.get(code, 0) + 1
                                    _o = io_counts.setdefault(k, {}).setdefault('out', {})
                                    _o[code] = _o.get(code, 0) + 1
                            except Exception:
                                pass
                except Exception:
                    pass
    elif iface in ('sip', 'cmp', 'csc'):
        # 시간당 단일 파일(*.msg.jsonl) + 5분 버킷 파일(*.msg.{mm5}.jsonl) 모두 포함.
        # systemId 는 와일드카드 — csp_01 하드코딩 시 csp_02(standby)/멀티노드 누락.
        patterns = [os.path.join(base, y, m, d, '*', f'*_{iface}.msg.jsonl')
                    for (y, m, d) in days]
        patterns += [os.path.join(base, y, m, d, '*', f'*_{iface}.msg.[0-9][0-9].jsonl')
                     for (y, m, d) in days]
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
                                ts = _ts_full(fpath, entry.get('ts'))
                                if ts < lo or ts > hi:
                                    continue
                                method = _parse_msg_method(entry.get('msg', ''))
                                k = _bucket_start(ts, gran)
                                counts[k] = counts.get(k, 0) + 1
                                method_counts[method] = method_counts.get(method, 0) + 1
                                # dir 은 CSP·CSC 공통으로 RX(수신)/TX(송신).
                                _d = 'out' if str(entry.get('dir', '')).upper() == 'TX' else 'in'
                                _io = io_counts.setdefault(k, {}).setdefault(_d, {})
                                _io[method] = _io.get(method, 0) + 1
                                svc = _classify_service(entry.get('msg', ''), dmap) or 'unknown'
                                method_svc.setdefault(method, {})
                                method_svc[method][svc] = method_svc[method].get(svc, 0) + 1
                                if svc != 'unknown':
                                    svc_counts.setdefault(k, {})
                                    svc_counts[k][svc] = svc_counts[k].get(svc, 0) + 1
                                    if method == 'INVITE':
                                        svc_invite[svc] = svc_invite.get(svc, 0) + 1
                            except Exception:
                                pass
                except Exception:
                    pass
    # unknown iface → 빈 결과 (구: 전체 합산 fallback)

    # 빈 구간도 0 으로 채워 연속 축을 만든다(구멍 뚫린 막대그래프 방지).
    buckets = []
    step = {'5m': timedelta(minutes=5), '10m': timedelta(minutes=10),
            '1h': timedelta(hours=1), '1d': timedelta(days=1)}.get(gran)
    if step:
        cur = datetime.strptime(_bucket_start(lo, gran).ljust(19, '0')[:19]
                                if gran in ('5m', '10m') else lo[:19], '%Y-%m-%d %H:%M:%S') \
            if gran not in ('5m', '10m') else datetime.strptime(_bucket_start(lo, gran) + ':00', '%Y-%m-%d %H:%M:%S')
        end = datetime.strptime(hi[:19], '%Y-%m-%d %H:%M:%S')
        if gran == '1h':
            cur = cur.replace(minute=0, second=0)
        elif gran == '1d':
            cur = cur.replace(hour=0, minute=0, second=0)
        guard = 0
        while cur <= end and guard < 5000:
            k = _bucket_start(cur.strftime('%Y-%m-%d %H:%M:%S'), gran)
            buckets.append(_svc_bucket(k, counts.get(k, 0), svc_counts, io_counts))
            cur += step
            guard += 1
    else:
        # 1M/1y — 라벨이 가변 길이라 등장한 키만 정렬해 낸다.
        for k in sorted(counts):
            buckets.append(_svc_bucket(k, counts[k], svc_counts, io_counts))

    # 시간(1h) 조회는 옛 소비자(hour 정수 키)를 위해 hour 를 함께 싣는다.
    if gran == '1h':
        for b in buckets:
            try:
                b['hour'] = int(b['label'][11:13])
            except Exception:
                pass

    sorted_methods = dict(sorted(method_counts.items(), key=lambda x: -x[1]))

    return HandlerResult(status=200, body={
        'from': from_dt, 'to': to_dt, 'granularity': gran, 'truncated': truncated,
        'date': (date or from_dt[:10]),
        'interface': iface,
        'total': sum(counts.values()),
        'buckets': buckets,
        'method_counts': sorted_methods,
        # 메서드 × 서비스 — 분포 막대를 서비스 계열 색으로 쪼갠다(각 메서드의 합 = method_counts).
        'method_service': {m: method_svc.get(m, {}) for m in sorted_methods},
        # 서비스축 요약 — 옛 화면이 읽던 voip_invite/ptt_invite 키를 그대로 채운다.
        'service_totals': {k: sum(v.get(k, 0) for v in svc_counts.values()) for k in ('volte', 'ptt')},
        'voip_invite': svc_invite.get('volte', 0),
        'ptt_invite': svc_invite.get('ptt', 0),
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


# 집계 단위별 **최대 조회 범위** — 버킷 수가 폭발하면 차트도 못 읽고 스캔 비용만 는다.
# 기준은 버킷 800개 근처지만, 상한은 거기서 **사람이 말하는 창**으로 반올림했다(3일·일주일·한 달·2년).
# 그래서 실제 버킷 수는 기준을 조금 넘기도 한다: 5m=864 / 10m=1008 / 1h=720 / 1d=730.
# 콘솔 page-filter(`GRAN_MAX_DAYS`)와 **같은 표**를 쓴다 — 한쪽만 바꾸면 화면과 서버가 어긋난다.
_GRAN_MAX_DAYS = {'5m': 3, '10m': 7, '1h': 30, '1d': 730}   # 1M/1y 는 사실상 무제한

def _norm_dt(v: str, end: bool = False) -> str:
    """'YYYY-MM-DD' / 'YYYY-MM-DD HH:MM' → 'YYYY-MM-DD HH:MM:SS'.

    화면(datetime-local)은 초를 보내지 않는다. 구간 계산·버킷 채우기가 모두 초까지 가정하므로
    **입구에서 한 번만** 맞춘다 (파싱하는 자리마다 방어하면 새 자리가 생길 때 또 빠진다).
    """
    if not v:
        return v
    v = str(v).strip().replace('T', ' ')
    if len(v) == 10:
        return v + (' 23:59:59' if end else ' 00:00:00')
    if len(v) == 16:
        return v + (':59' if end else ':00')
    return v[:19]


def _clamp_range(from_dt: str, to_dt: str, gran: str):
    """[from,to] 를 단위 상한으로 자른다. 잘렸으면 (from,to,True) — 응답에 truncated 로 알린다."""
    lim = _GRAN_MAX_DAYS.get(gran)
    if not lim:
        return from_dt, to_dt, False
    try:
        f = datetime.strptime(from_dt[:19], '%Y-%m-%d %H:%M:%S')
        t = datetime.strptime(to_dt[:19], '%Y-%m-%d %H:%M:%S')
    except Exception:
        return from_dt, to_dt, False
    if (t - f).days <= lim:
        return from_dt, to_dt, False
    # 끝을 기준으로 뒤에서 자른다 — 최근 구간이 관심사다.
    # 자른 시작점은 단위 경계로 내려 맞춘다 — 그냥 빼면 '08-25 23:59:59' 같은 지점에서 시작해
    # 첫 버킷이 반토막 나고 표시 구간과 축 첫 칸이 어긋난다.
    f = t - timedelta(days=lim)
    if gran in ('5m', '10m'):
        f = f.replace(minute=f.minute - f.minute % (5 if gran == '5m' else 10), second=0)
    elif gran == '1h':
        f = f.replace(minute=0, second=0)
    elif gran == '1d':
        f = f.replace(hour=0, minute=0, second=0)
    return f.strftime('%Y-%m-%d %H:%M:%S'), to_dt, True


def _bucket_start(ts: str, gran: str) -> str:
    """레코드 ts → 그 레코드가 속한 버킷의 시작 라벨.
       5m/10m = 'YYYY-MM-DD HH:MM', 1h = 'YYYY-MM-DD HH', 1d = 'YYYY-MM-DD',
       1M = 'YYYY-MM', 1y = 'YYYY'."""
    t = (ts or '')[:19]
    if gran == '1y':
        return t[:4]
    if gran == '1M':
        return t[:7]
    if gran == '1d':
        return t[:10]
    if gran == '1h':
        return t[:13]
    if gran in ('5m', '10m'):
        step = 5 if gran == '5m' else 10
        try:
            mi = int(t[14:16])
        except Exception:
            mi = 0
        return f"{t[:14]}{(mi // step) * step:02d}"
    return t[:13]


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


@_offload
def _service_stats(config, svc, gran, from_dt, to_dt, date) -> HandlerResult:
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
        logger.exception('stats handler error: %s', e)
        return HandlerResult(status=500, body=_ERR_INTERNAL)


def _calc_voip_stats(config, from_dt, to_dt, gran):
    """VoLTE 호 KPI — 시도(attempt) 기준 3지표 (sip_statistics.md §2.1).

        성공률 = 세션이 성립한 시도 / 전체 시도      answer_time != null
        소통률 = 실제 통화가 있었던 시도 / 전체 시도  duration > 0
        완료율 = 정상 종료한 시도 / 세션이 성립한 시도  end_reason == "normal"

    셋을 나눠 세는 이유: 붙었는데 미디어가 없는 호(성공·미소통)와 붙어서 통화했지만
    비정상 종료한 호(성공·소통·미완료)는 서로 다른 장애다. 한 칸에 섞으면 어느 쪽인지
    알 수 없다.

    **비율이 아니라 분자·분모를 함께 낸다.** 비율은 합산이 안 되므로(구간 비율의 평균은
    전체 비율이 아니다) 롤업·재집계의 근거는 항상 건수여야 한다(§5.1).

    완료율의 분모는 `attempts` 가 아니라 `sessions` 다 — 붙지도 않은 호를 "완료하지
    못했다" 고 세면 성공률과 같은 것을 두 번 재게 된다.
    """
    attempts = sessions = talked = completed = 0
    durations = []
    pdd_sum_ms = 0
    pdd_n = 0
    end_reasons: dict = {}
    # 버킷별 분자·분모 (bucket_key -> count)
    bk_attempts: dict = {}
    bk_sessions: dict = {}
    bk_talked: dict = {}
    bk_completed: dict = {}

    for rec in _iter_call_jsons(config, 'volte', from_dt, to_dt):
        ts = _ts_of(rec, 'volte')
        if not ts or ts < from_dt or ts > to_dt:
            continue
        attempts += 1
        state = rec.get('state', '')
        reason = rec.get('end_reason') or 'unknown'
        dur = int(rec.get('duration', 0) or 0)

        # 세션 성립 = 200 OK 를 받아 answer_time 이 채워진 것 (CallDir.h VoipCallAnswer).
        # state 로 판정하면 진행중(active)과 종료(ended)를 따로 처리해야 하고, 비정상
        # 종료한 호가 "성립하지 않은 것" 으로 빠진다.
        answered = bool(rec.get('answer_time'))
        if answered:
            sessions += 1
            pdd = _pdd_ms(ts, rec.get('answer_time'))
            if pdd is not None:
                pdd_sum_ms += pdd
                pdd_n += 1
        if dur > 0:
            talked += 1
            durations.append(dur)
        if answered and state == 'ended' and reason == 'normal':
            completed += 1
        if state == 'ended':
            end_reasons[reason] = end_reasons.get(reason, 0) + 1

        bk = _bucket_key(ts, gran)
        if bk:
            bk_attempts[bk] = bk_attempts.get(bk, 0) + 1
            if answered:
                bk_sessions[bk] = bk_sessions.get(bk, 0) + 1
            if dur > 0:
                bk_talked[bk] = bk_talked.get(bk, 0) + 1
            if answered and state == 'ended' and reason == 'normal':
                bk_completed[bk] = bk_completed.get(bk, 0) + 1

    keys = sorted(bk_attempts.keys(), key=lambda k: int(k)) \
        if gran in ('5m', '10m', '1h') else sorted(bk_attempts.keys())
    label = 'hour' if gran in ('5m', '10m', '1h') else 'date'
    buckets = []
    for k in keys:
        a = bk_attempts[k]
        se = bk_sessions.get(k, 0)
        tk = bk_talked.get(k, 0)
        cp = bk_completed.get(k, 0)
        buckets.append({
            label:            int(k) if label == 'hour' else k,
            'attempts':       a,
            'sessions':       se,
            'talked':         tk,
            'completed':      cp,
            'success':        se,                      # 옛 소비자 호환 (= sessions)
            'success_rate':   _rate(se, a),
            'talk_rate':      _rate(tk, a),
            'completion_rate': _rate(cp, se),
        })

    avg_dur = round(sum(durations) / len(durations), 1) if durations else 0
    return {
        # 옛 필드 — 콘솔 KPI 카드가 쓰는 이름. total_success 의 판정 근거가
        # "정상 종료" 에서 "세션 성립" 으로 바뀌었다(§9 이행 표).
        'total_attempts':   attempts,
        'total_success':    sessions,
        'success_rate':     _rate(sessions, attempts),
        'avg_duration_sec': avg_dur,
        'end_reasons':      end_reasons,
        'buckets':          buckets,
        # 3지표 — 분자·분모 동봉
        'total_sessions':   sessions,
        'total_talked':     talked,
        'total_completed':  completed,
        'talk_rate':        _rate(talked, attempts),
        'completion_rate':  _rate(completed, sessions),
        'duration_sum_sec': sum(durations),
        'pdd_sum_ms':       pdd_sum_ms,
        'pdd_n':            pdd_n,
        'avg_pdd_ms':       round(pdd_sum_ms / pdd_n) if pdd_n else 0,
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

@_offload
def _subscribers_status(config: dict, status: str = 'active',
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

    # CMP 에서 floor holder 조회 시도 (실패해도 무관). 동시 발언이면 대표 화자(첫 발언자).
    group_floor_holder = {}
    try:
        cmp_stats = _get_cmp_stats(config)
        for gd in (cmp_stats or {}).get('group_details', []) or []:
            gid = gd.get('group_id', '')
            hs = _floor_holders(gd)
            if gid and hs:
                group_floor_holder[gid] = hs[0]
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
        logger.exception('stats handler error: %s', e)
        return HandlerResult(status=500, body=_ERR_INTERNAL)

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


@_offload
def _service_live(config: dict) -> HandlerResult:
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

    # CMP floor holders (전 노드 group_details 병합) — 그룹당 발언자 목록(동시 발언 포함)
    floor_holders = {}
    for nd in media:
        for gd in (nd['stats'].get('group_details') or []):
            gg = gd.get('group_id', '')
            hs = _floor_holders(gd)
            if gg and hs:
                floor_holders[gg] = hs

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
        hs = floor_holders.get(gid) or []
        g['floor_holders'] = hs
        g['floor_holder'] = hs[0] if hs else None   # 대표 화자 (단일 화자 그룹의 종전 필드)
        g.pop('members', None)   # 멤버 비인라인(그룹당 100~200명) → drill 엔드포인트로 페이지네이션
        participants += g['active_members']
        anomalies = []
        if hs:
            talking += len(hs)   # 동시 발언(dual/multi)이면 발언자 수만큼 계상
            for h in hs:
                talkers.append({'msisdn': h, 'org': m2o.get(h, ''),
                                'group_id': gid, 'group_name': g['name']})
                held = _floor_held_secs(config, meta.get('id'), h, now)
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


@_offload
def _service_trend(config: dict, window='8h') -> HandlerResult:
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


@_offload
def _service_events(config: dict, limit='60') -> HandlerResult:
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


@_offload
def _service_org(config: dict) -> HandlerResult:
    """조직 트리별 이용 — 회사>본부>팀 트리 + 구성원/등록/활성 롤업(상위=하위 합)."""
    volte_states = _load_active_states(config, 'volte')
    ptt_states = _load_active_states(config, 'ptt')
    av = {st.get('subscriber_id') for st in volte_states if st.get('subscriber_id')}
    ap = {st.get('subscriber_id') for st in ptt_states if st.get('subscriber_id')}
    talkers = set()
    for nd in _all_media_stats(config):
        for gd in (nd['stats'].get('group_details') or []):
            talkers.update(_floor_holders(gd))

    UNSET = '(미지정)'
    KEYS = ('members', 'volte_reg', 'ptt_reg', 'active_volte', 'active_ptt', 'ptt_talking')
    leaf = {}   # org_id(leaf 팀코드) → stats
    db_degraded = False  # DB 집계 실패 시 true — 구성원/등록 컬럼만 0 강등

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
        # DB 순단 시 페이지 전체를 죽이지 않는다 — 구성원/등록 수만 0 으로 강등하고
        # 파일(state)·CMP 기반 지표(활성 세션/발언자)는 그대로 서빙. 활성 가입자의
        # 조직 매핑(m2o)도 비므로 활성 카운트는 '(미지정)' 으로 묶인다.
        logger.warning('service/org DB aggregation failed (degraded): %s', e)
        db_degraded = True

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
    body = {'orgs': out}
    if db_degraded:
        body['db_degraded'] = True  # 콘솔이 "DB 조회 실패 — 일부 컬럼 제외" 안내 표시용
    return HandlerResult(status=200, body=body)


@_offload
def _ptt_members(config: dict, group: str, page='1', limit='50') -> HandlerResult:
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
    holders = set()
    for nd in _all_media_stats(config):
        for gd in (nd['stats'].get('group_details') or []):
            if gd.get('group_id') == group:
                holders.update(_floor_holders(gd))
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
                                    'talking': r['msisdn'] in holders})
    except Exception as e:
        logger.exception('stats handler error: %s', e)
        return HandlerResult(status=500, body=_ERR_INTERNAL)
    return HandlerResult(status=200, body={
        'group': group, 'total': total, 'page': page, 'limit': limit,
        'active_count': len(active_in_group),
        'floor_holder': next(iter(sorted(holders)), None),   # 대표 화자
        'floor_holders': sorted(holders),                    # 동시 발언 전원
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


# ── API 문서 (개발자 모드) ──────────────────────────────────────────────────
#  이 모듈이 제공하는 엔드포인트의 자기기술. handlers/api_docs.py 가 수집하고, 콘솔은 각 메뉴에서
#  [API] 버튼으로 읽어 표시한다. 경로/파라미터를 바꾸면 **여기도 같은 커밋에서** 갱신한다.
#  module='oam-svc' — stats 전체가 oam-svc 귀속(role=base 는 게이트웨이 프록시). 스키마는 api_docs.py 주석.
_AUTH_MONITOR = {'scheme': 'bearer', 'role': 'monitor', 'token_from': 'POST /api/v1/auth/login'}

_ERR_COMMON = [
    {'status': 401, 'when': 'Authorization 헤더 없음 / 토큰 만료', 'body': {'error': 'unauthorized'}},
    {'status': 403, 'when': '권한 등급 미달', 'body': {'error': 'forbidden'}},
]

CIMS_STATS_API_DOCS = [
    {'id': 'stats.health', 'module': 'oam-svc', 'method': 'GET', 'path': '/api/v1/stats/health',
     'summary': '서비스 컴포넌트 상태 + 가입자/번호/등록/그룹 카운트 + CMP RTP 풀·sweeper 카운터',
     'params': [],
     'response': '{health{csp,cmp,db}, csp{...}, cmp{rtp_ports{},rtp_ports_ptt{},sweeper{}}, record_enable}',
     'response_fields': [
         {'name': 'health.csp', 'type': 'string', 'enum': ['up', 'down'], 'desc': 'CSP(시그널링) UDP probe 결과'},
         {'name': 'health.cmp', 'type': 'string', 'enum': ['up', 'down'],
          'desc': 'CMP(미디어) — 다중 노드 중 하나라도 응답하면 up'},
         {'name': 'health.db', 'type': 'string', 'enum': ['up', 'down'], 'desc': 'DB 접속 확인'},
         {'name': 'csp.registered_users', 'type': 'integer', 'unit': '명', 'desc': '현재 SIP REGISTER 단말 수'},
         {'name': 'csp.active_calls', 'type': 'integer', 'unit': '건', 'desc': '진행 중 호'},
         {'name': 'csp.db_connected', 'type': 'boolean', 'desc': 'CSP 자체 DB 연결 여부'},
         {'name': 'csp.roles', 'type': 'object', 'desc': 'IMS 역할별 활성 상태 (CSCF/TAS/PTT-AS/IBCF)'},
         {'name': 'csp.subscribers_total', 'type': 'integer', 'unit': '명', 'desc': '가입자(person) 총수'},
         {'name': 'csp.volte_numbers', 'type': 'integer', 'unit': '개', 'desc': 'VoLTE 번호 프로비저닝 총수'},
         {'name': 'csp.volte_registered', 'type': 'integer', 'unit': '개', 'desc': 'VoLTE 등록 중 번호 수'},
         {'name': 'csp.ptt_numbers', 'type': 'integer', 'unit': '개', 'desc': 'PTT 번호 프로비저닝 총수'},
         {'name': 'csp.ptt_registered', 'type': 'integer', 'unit': '개', 'desc': 'PTT 등록 중 번호 수'},
         {'name': 'csp.ptt_groups_total', 'type': 'integer', 'unit': '개', 'desc': 'PTT 그룹 총수'},
         {'name': 'cmp.sessions', 'type': 'integer', 'unit': '개', 'desc': '전 미디어 노드 relay 세션 합'},
         {'name': 'cmp.groups', 'type': 'integer', 'unit': '개', 'desc': '전 노드 PTT 그룹 세션 합'},
         {'name': 'cmp.rtp_ports.total/used/free', 'type': 'integer', 'unit': '개', 'desc': 'VoIP RTP 포트 풀'},
         {'name': 'cmp.rtp_ports_ptt.total/used/free', 'type': 'integer', 'unit': '개', 'desc': 'PTT 전용 RTP 포트 풀'},
         {'name': 'cmp.sweeper.leak_reclaim_total', 'type': 'integer', 'unit': '건',
          'desc': '누수 relay 회수 누적 — 정상 환경은 0, 증가 시 누수 신호'},
         {'name': 'cmp.sweeper.leak_reclaim_orphan', 'type': 'integer', 'unit': '건', 'desc': 'RTP 미수신 고아 회수'},
         {'name': 'cmp.sweeper.leak_reclaim_hold', 'type': 'integer', 'unit': '건', 'desc': 'hold 타임아웃 회수'},
         {'name': 'cmp.sweeper.session_timeout', 'type': 'integer', 'unit': '건', 'desc': '세션 타임아웃 회수'},
         {'name': 'record_enable', 'type': 'boolean', 'desc': '녹취 기능 활성 여부'},
     ],
     'example': {'health': {'csp': 'up', 'cmp': 'up', 'db': 'up'},
                 'csp': {'registered_users': 128, 'active_calls': 3, 'db_connected': True,
                         'roles': {'cscf': True, 'tas': True, 'ptt_as': True},
                         'subscribers_total': 512, 'volte_numbers': 480, 'volte_registered': 120,
                         'ptt_numbers': 300, 'ptt_registered': 96, 'ptt_groups_total': 24},
                 'cmp': {'sessions': 3, 'groups': 1,
                         'rtp_ports': {'total': 2000, 'used': 6, 'free': 1994},
                         'rtp_ports_ptt': {'total': 500, 'used': 2, 'free': 498},
                         'sweeper': {'session_timeout': 0, 'orphan_reclaim_sec': 30,
                                     'leak_reclaim_total': 0, 'leak_reclaim_orphan': 0,
                                     'leak_reclaim_hold': 0}},
                 'record_enable': True},
     'errors': list(_ERR_COMMON),
     'notes': ['결과는 짧게 캐시된다(수 초) — 매 초 폴링해도 실제 probe 는 그보다 드물다.',
               'CSP/CMP 가 down 이면 해당 섹션 카운터는 0 으로 채워진다(오류 아님).',
               'cmp 는 다중 미디어 노드의 **합산**이다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'stats.subscribers', 'module': 'oam-svc', 'method': 'GET', 'path': '/api/v1/stats/subscribers',
     'summary': '가입자별 실시간 접속/통화 상태 (VoLTE·PTT 번호 단위, 페이지네이션)',
     'params': [
         {'name': 'status', 'in': 'query', 'type': 'string', 'required': False,
          'enum': ['active', 'inactive', 'all'], 'desc': '등록 상태 필터 (기본 active)'},
         {'name': 'q', 'in': 'query', 'type': 'string', 'required': False, 'desc': '검색어 (번호/이름 부분일치)'},
         {'name': 'org', 'in': 'query', 'type': 'string', 'required': False, 'desc': '조직 코드 필터'},
         {'name': 'page', 'in': 'query', 'type': 'integer', 'required': False, 'desc': '1-base 페이지 (기본 1)'},
         {'name': 'limit', 'in': 'query', 'type': 'integer', 'required': False, 'desc': '페이지 크기 (기본 50)'},
     ],
     'response': '{total, page, limit, status, counts{}, subscribers[]}',
     'response_fields': [
         {'name': 'total', 'type': 'integer', 'unit': '명', 'desc': '필터 적용 후 전체 건수'},
         {'name': 'page', 'type': 'integer', 'desc': '요청된 페이지'},
         {'name': 'limit', 'type': 'integer', 'desc': '페이지 크기'},
         {'name': 'status', 'type': 'string', 'desc': '적용된 status 필터'},
         {'name': 'counts', 'type': 'object', 'desc': 'active/inactive 집계'},
         {'name': 'subscribers[].person_id', 'type': 'integer', 'desc': '가입자 id'},
         {'name': 'subscribers[].name', 'type': 'string', 'desc': '이름'},
         {'name': 'subscribers[].org', 'type': 'string', 'desc': '조직 코드'},
         {'name': 'subscribers[].org_path', 'type': 'string', 'desc': '조직 경로 (상위 → 하위)'},
         {'name': 'subscribers[].volte', 'type': 'object',
          'desc': 'VoLTE 가입 상태 — {msisdn, online, register_time, calls[]}. 미가입이면 null'},
         {'name': 'subscribers[].ptt', 'type': 'object',
          'desc': 'PTT 가입 상태 — {msisdn, online, register_time, groups[]}. 미가입이면 null'},
     ],
     'example': {'total': 2, 'page': 1, 'limit': 50, 'status': 'active',
                 'counts': {'active': 2, 'inactive': 0},
                 'subscribers': [{'person_id': 11, 'name': '홍길동', 'org': 'D100',
                                  'org_path': '본사 > 운영팀',
                                  'volte': {'msisdn': '01000000001', 'online': True,
                                            'register_time': '2026-07-30T08:40:11', 'calls': []},
                                  'ptt': {'msisdn': '01000000001', 'online': True,
                                          'register_time': '2026-07-30T08:40:12',
                                          'groups': ['g-ops-1']}}]},
     'errors': list(_ERR_COMMON),
     'notes': ['online 판정은 CSP 가 기록하는 가입자별 state 파일 기준(실시간).',
               'calls[]/groups[] 는 현재 참여 중인 것만 담긴다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'stats.calls', 'module': 'oam-svc', 'method': 'GET',
     'path': '/api/v1/stats/calls',
     'summary': '서비스별 호 KPI — 성공률·소통률·완료율·참여율 (1분 기저 집계)',
     'params': [
         {'name': 'from', 'in': 'query', 'type': 'string', 'required': False,
          'desc': 'YYYY-MM-DD[ HH:MM[:SS]] — to 와 짝'},
         {'name': 'to', 'in': 'query', 'type': 'string', 'required': False, 'desc': '구간 끝'},
         {'name': 'date', 'in': 'query', 'type': 'string', 'required': False,
          'desc': 'YYYY-MM-DD — "그 날 하루" 축약 (from/to 대신)'},
         {'name': 'granularity', 'in': 'query', 'type': 'string', 'required': False,
          'enum': list(stats_rollup.GRANULARITIES), 'desc': '버킷 단위 (기본 1h)'},
         {'name': 'svc', 'in': 'query', 'type': 'string', 'required': False,
          'enum': ['all', 'volte', 'ptt', 'unknown'], 'desc': '서비스축 (기본 all)'},
     ],
     'response': '{from, to, granularity, svc, source, totals{}, buckets[]}',
     'response_fields': [
         {'name': 'source', 'type': 'string',
          'desc': 'rollup = 1분 집계, scan = 집계 없는 구간을 원본에서 즉석 계산, none = 로그 경로 미설정'},
         {'name': 'totals.<svc>.attempts', 'type': 'integer', 'unit': '건', 'desc': '호 시도 (성공률·소통률 분모)'},
         {'name': 'totals.<svc>.sessions', 'type': 'integer', 'unit': '건', 'desc': '세션 성립 (성공률 분자·완료율 분모)'},
         {'name': 'totals.<svc>.talked', 'type': 'integer', 'unit': '건', 'desc': '실제 통화 (소통률 분자)'},
         {'name': 'totals.<svc>.completed', 'type': 'integer', 'unit': '건', 'desc': '정상 종료 (완료율 분자)'},
         {'name': 'totals.<svc>.legs_invited', 'type': 'integer', 'unit': '개', 'desc': '초대한 leg (참여율 분모)'},
         {'name': 'totals.<svc>.legs_joined', 'type': 'integer', 'unit': '개', 'desc': 'join 한 leg (참여율 분자)'},
         {'name': 'totals.<svc>.reasons{}', 'type': 'object', 'desc': '종료 사유별 건수'},
         {'name': 'totals.<svc>.open', 'type': 'integer', 'unit': '건', 'desc': '아직 끝나지 않아 값이 확정되지 않은 호'},
         {'name': 'totals.<svc>.late_dropped', 'type': 'integer', 'unit': '건',
          'desc': '보존기간 초과로 되짚지 못한 호 — 계속 오르면 1분 계층 보존기간이 짧다'},
         {'name': 'buckets[].bucket', 'type': 'string', 'desc': '버킷 시작 라벨 (단위 무관 동일 키)'},
         {'name': 'buckets[].bucket_start', 'type': 'string', 'desc': '버킷 시작 ISO8601 (오프셋 포함)'},
     ],
     'example': {'from': '2026-09-03 00:00:00', 'to': '2026-09-03 23:59:59',
                 'granularity': '1h', 'svc': 'all', 'source': 'rollup',
                 'totals': {'volte': {'attempts': 120, 'sessions': 118, 'talked': 110,
                                      'completed': 105, 'success_rate': 98.3,
                                      'talk_rate': 91.7, 'completion_rate': 89.0}},
                 'buckets': [{'bucket': '2026-09-03 15:00',
                              'bucket_start': '2026-09-03T15:00:00+09:00',
                              'all': {'attempts': 12, 'sessions': 11}}]},
     'errors': list(_ERR_COMMON),
     'notes': ['비율은 저장하지 않는다 — 분자·분모를 함께 내므로 화면이 구간을 다시 합칠 수 있다.',
               'PTT 는 attempts 가 0 이다: 실패한 그룹통화 시도가 원천에 없다(sip_statistics.md §8 Y6). '
               '세션 기록이 곧 성립이라 세면 성공률이 항상 100% 가 된다.',
               '`all` 은 svc 필터와 무관하게 전체 서비스 합계다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'stats.calls.rebuild', 'module': 'oam-svc', 'method': 'POST',
     'path': '/api/v1/stats/calls/rebuild',
     'summary': '1분 집계 재생성 (원본에서 다시 계산) — 운영/검증용',
     'params': [
         {'name': 'from', 'in': 'query', 'type': 'string', 'required': False, 'desc': 'YYYY-MM-DD'},
         {'name': 'to', 'in': 'query', 'type': 'string', 'required': False, 'desc': 'YYYY-MM-DD'},
         {'name': 'date', 'in': 'query', 'type': 'string', 'required': False, 'desc': '하루만 재생성'},
     ],
     'response': '{ok, from, to, buckets}',
     'response_fields': [
         {'name': 'buckets', 'type': 'integer', 'unit': '개', 'desc': '다시 적은 버킷 수'},
     ],
     'example': {'ok': True, 'from': '2026-09-03', 'to': '2026-09-03', 'buckets': 12},
     'errors': list(_ERR_COMMON) + [
         {'status': 409, 'when': 'StatsRollup.Enabled 가 꺼져 있음',
          'body': {'error': 'rollup_disabled'}}],
     'notes': ['롤업은 미결·신규 버킷만 다시 계산한다 — 집계 스키마에 축이 추가되면 이미 적힌 '
               '버킷은 그 축이 빈 채 남으므로 이 API 로 채운다.',
               'watermark 를 건드리지 않는다 — 과거 재생성이 이후의 정상 집계를 되돌리지 않는다.',
               '변이라서 admin 권한을 요구한다(조회는 monitor).'],
     'auth': {'scheme': 'bearer', 'role': 'admin', 'token_from': 'POST /api/v1/auth/login'}},

    {'id': 'stats.messages', 'module': 'oam-svc', 'method': 'GET', 'path': '/api/v1/stats/messages',
     'summary': '전 인터페이스 메시지 카운터 (시간대 버킷 + 메서드/상태코드별 집계)',
     'params': [{'name': 'date', 'in': 'query', 'type': 'string', 'required': False,
                 'desc': 'YYYY-MM-DD (기본 오늘)'}],
     'response': '{date, interface, total, buckets[], method_counts{}}',
     'response_fields': [
         {'name': 'date', 'type': 'string', 'desc': '집계 일자 (YYYY-MM-DD)'},
         {'name': 'interface', 'type': 'string', 'desc': '집계 대상 인터페이스 (전체면 null)'},
         {'name': 'total', 'type': 'integer', 'unit': '건', 'desc': '총 메시지 수'},
         {'name': 'buckets[].hour', 'type': 'string', 'desc': '시간대 (00~23)'},
         {'name': 'buckets[].count', 'type': 'integer', 'unit': '건', 'desc': '해당 시간대 메시지 수'},
         {'name': 'method_counts{}', 'type': 'object',
          'desc': '키 = SIP 메서드(INVITE/REGISTER/BYE…) 또는 응답 상태코드(200/401…) 또는 CMP cmd, 값 = 건수'},
     ],
     'example': {'date': '2026-07-30', 'interface': None, 'total': 15840,
                 'buckets': [{'hour': '08', 'count': 640}, {'hour': '09', 'count': 1120}],
                 'method_counts': {'INVITE': 320, 'REGISTER': 2100, 'BYE': 310, '200': 4800, '401': 2050}},
     'errors': list(_ERR_COMMON),
     'notes': ['원본은 서비스 로그 JSONL(시간별 파일)이라 조회 범위가 넓으면 느려진다.',
               '로그 디렉터리가 설정되지 않았으면 0/빈 배열을 반환한다(오류 아님).'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'stats.messages.iface', 'module': 'oam-svc', 'method': 'GET',
     'path': '/api/v1/stats/messages/{iface}',
     'summary': '인터페이스별 메시지 카운터',
     'params': [
         {'name': 'iface', 'in': 'path', 'type': 'string', 'required': True,
          'enum': ['sip', 'cmp', 'csc', 'https'], 'desc': '대상 인터페이스'},
         {'name': 'date', 'in': 'query', 'type': 'string', 'required': False, 'desc': 'YYYY-MM-DD (기본 오늘)'},
         {'name': 'granularity', 'in': 'query', 'type': 'string', 'required': False,
          'desc': 'iface=sip 은 1m/5m/10m/1h/1d/1w/1M/1y (1분 집계). 그 외 인터페이스는 5m/10m/1h/1d'},
         {'name': 'svc', 'in': 'query', 'type': 'string', 'required': False,
          'enum': ['all', 'volte', 'ptt', 'unknown'], 'desc': 'iface=sip 전용 서비스축 (기본 all)'},
     ],
     'response': '{date, interface, total, buckets[], method_counts{}}',
     'response_fields': [
         {'name': 'interface', 'type': 'string', 'desc': '요청한 iface 그대로'},
         {'name': 'buckets[].bucket', 'type': 'string', 'desc': '버킷 시작 라벨 (집계 경로에서만)'},
         {'name': 'buckets[].bucket_start', 'type': 'string', 'desc': '버킷 시작 ISO8601 (집계 경로에서만)'},
         {'name': 'total', 'type': 'integer', 'unit': '건', 'desc': '총 메시지 수'},
         {'name': 'buckets[].hour', 'type': 'string', 'desc': '시간대 (00~23)'},
         {'name': 'buckets[].count', 'type': 'integer', 'unit': '건', 'desc': '건수'},
         {'name': 'method_counts{}', 'type': 'object', 'desc': '메서드/상태코드 → 건수'},
     ],
     'example': {'date': '2026-07-30', 'interface': 'sip', 'total': 9120,
                 'buckets': [{'hour': '09', 'count': 880}],
                 'method_counts': {'INVITE': 320, 'REGISTER': 2100, '200': 4200}},
     'errors': list(_ERR_COMMON),
     'notes': ["iface=https 는 SIP 로그가 아니라 flow 로그의 HTTPS 엔트리에서 집계한다 — "
               "method 가 'GET /path' 형태로 나온다."],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'stats.leak-reclaims', 'module': 'oam-svc', 'method': 'GET',
     'path': '/api/v1/stats/leak-reclaims',
     'summary': 'CMP sweeper 가 회수한 누수 세션 상세 + reason/node 별 집계',
     'params': [{'name': 'date', 'in': 'query', 'type': 'string', 'required': False,
                 'desc': 'YYYY-MM-DD (기본 오늘)'}],
     'response': '{date, counts{}, by_node{}, items[]}',
     'response_fields': [
         {'name': 'date', 'type': 'string', 'desc': '조회 일자'},
         {'name': 'counts.total', 'type': 'integer', 'unit': '건', 'desc': '회수 총건'},
         {'name': 'counts.orphan_no_rtp', 'type': 'integer', 'unit': '건', 'desc': 'RTP 미수신 고아'},
         {'name': 'counts.hold_timeout', 'type': 'integer', 'unit': '건', 'desc': 'hold 타임아웃'},
         {'name': 'by_node{}', 'type': 'object', 'desc': '미디어 노드명 → 회수 건수'},
         {'name': 'items[].ts', 'type': 'string', 'desc': '회수 시각 (최신순)'},
         {'name': 'items[].reason', 'type': 'string', 'enum': ['orphan_no_rtp', 'hold_timeout'],
          'desc': '회수 사유'},
         {'name': 'items[].node', 'type': 'string', 'desc': '회수한 미디어 노드'},
     ],
     'example': {'date': '2026-07-30', 'counts': {'total': 0, 'orphan_no_rtp': 0, 'hold_timeout': 0},
                 'by_node': {}, 'items': []},
     'errors': list(_ERR_COMMON),
     'notes': ['정상 환경의 기대값은 빈 목록이다 — 항목이 있으면 CSP crash/teardown 누락 등 누수 신호.',
               'items 는 최대 500건까지만 반환한다.'],
     'auth': dict(_AUTH_MONITOR)},

    # ── service KPI (/api/v1/stats/service/*) ───────────────────────────────
    {'id': 'stats.service.volte', 'module': 'oam-svc', 'method': 'GET',
     'path': '/api/v1/stats/service/volte',
     'summary': 'VoLTE 서비스 KPI — 호 시도/성공/성공률/평균 통화시간 + 시간대 버킷 + 종료사유 분포',
     'params': [
         {'name': 'granularity', 'in': 'query', 'type': 'string', 'required': False,
          'enum': ['1h', '1d', '1M'], 'desc': '집계 단위 (기본 1d)'},
         {'name': 'date', 'in': 'query', 'type': 'string', 'required': False,
          'desc': 'YYYY-MM-DD — 그 날 00:00:00~23:59:59'},
         {'name': 'from', 'in': 'query', 'type': 'string', 'required': False,
          'desc': '시작 일시 "YYYY-MM-DD HH:MM:SS" (구간 조회)'},
         {'name': 'to', 'in': 'query', 'type': 'string', 'required': False, 'desc': '종료 일시 (미지정 시 from 과 동일)'},
     ],
     'response': '{granularity, from, to, volte{total_attempts, total_success, success_rate, '
                 'avg_duration_sec, end_reasons{}, buckets[]}}',
     'response_fields': [
         {'name': 'granularity', 'type': 'string', 'desc': '적용된 집계 단위'},
         {'name': 'from', 'type': 'string', 'desc': '집계 시작 일시'},
         {'name': 'to', 'type': 'string', 'desc': '집계 종료 일시'},
         {'name': 'volte.total_attempts', 'type': 'integer', 'unit': '건', 'desc': '호 시도 수'},
         {'name': 'volte.total_success', 'type': 'integer', 'unit': '건', 'desc': '연결 성공 수'},
         {'name': 'volte.success_rate', 'type': 'number', 'unit': '%', 'desc': '성공률 (소수 1자리)'},
         {'name': 'volte.avg_duration_sec', 'type': 'number', 'unit': '초', 'desc': '평균 통화시간'},
         {'name': 'volte.end_reasons{}', 'type': 'object', 'desc': '종료 사유 → 건수'},
         {'name': 'volte.buckets[].hour', 'type': 'integer',
          'desc': '시간대 0~23 (granularity=1h 일 때). 1d/1M 이면 date(YYYY-MM-DD) 필드가 대신 온다'},
         {'name': 'volte.buckets[].attempts', 'type': 'integer', 'unit': '건', 'desc': '버킷 내 시도'},
         {'name': 'volte.buckets[].success', 'type': 'integer', 'unit': '건', 'desc': '버킷 내 성공'},
         {'name': 'volte.buckets[].success_rate', 'type': 'number', 'unit': '%', 'desc': '버킷 성공률'},
     ],
     'example': {'granularity': '1h', 'from': '2026-07-30 00:00:00', 'to': '2026-07-30 23:59:59',
                 'volte': {'total_attempts': 1234, 'total_success': 1201, 'success_rate': 97.3,
                           'avg_duration_sec': 82.5,
                           'end_reasons': {'normal': 1180, 'busy': 21, 'timeout': 12},
                           'buckets': [{'hour': 9, 'attempts': 120, 'success': 118, 'success_rate': 98.3}]}},
     'errors': list(_ERR_COMMON),
     'notes': ['버킷 키가 granularity 에 따라 달라진다 — 1h 는 hour(정수), 1d/1M 은 date(문자열).',
               'from/to 미지정 + date 미지정이면 **오늘 하루**가 기본 구간이다.',
               '원본은 호별 call.json 파일 스캔이라 구간이 길면 느려진다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'stats.service.ptt', 'module': 'oam-svc', 'method': 'GET', 'path': '/api/v1/stats/service/ptt',
     'summary': 'PTT 서비스 KPI — 그룹콜 수/평균 세션시간 + 그룹별 분포 + 시간대 버킷',
     'params': [
         {'name': 'granularity', 'in': 'query', 'type': 'string', 'required': False,
          'enum': ['1h', '1d', '1M'], 'desc': '집계 단위 (기본 1d)'},
         {'name': 'date', 'in': 'query', 'type': 'string', 'required': False, 'desc': 'YYYY-MM-DD'},
         {'name': 'from', 'in': 'query', 'type': 'string', 'required': False, 'desc': '시작 일시'},
         {'name': 'to', 'in': 'query', 'type': 'string', 'required': False, 'desc': '종료 일시'},
     ],
     'response': '{granularity, from, to, ptt{total_calls, avg_duration_sec, by_group{}, buckets[]}}',
     'response_fields': [
         {'name': 'ptt.total_calls', 'type': 'integer', 'unit': '건', 'desc': '그룹 세션 수'},
         {'name': 'ptt.avg_duration_sec', 'type': 'number', 'unit': '초', 'desc': '평균 세션시간'},
         {'name': 'ptt.by_group{}', 'type': 'object', 'desc': 'MCPTT 그룹 ID → 세션 수 (내림차순)'},
         {'name': 'ptt.buckets[].hour', 'type': 'integer', 'desc': '시간대 0~23 (1h). 1d/1M 은 date'},
         {'name': 'ptt.buckets[].calls', 'type': 'integer', 'unit': '건', 'desc': '버킷 내 세션 수'},
     ],
     'example': {'granularity': '1d', 'from': '2026-07-30 00:00:00', 'to': '2026-07-30 23:59:59',
                 'ptt': {'total_calls': 87, 'avg_duration_sec': 44.2,
                         'by_group': {'g-ops-1': 51, 'g-ops-2': 36},
                         'buckets': [{'date': '2026-07-30', 'calls': 87}]}},
     'errors': list(_ERR_COMMON),
     'notes': ['PTT 는 세션별 call.jsonl 의 **마지막 레코드**를 세션 1건으로 센다.',
               '버킷 키는 VoLTE 와 동일 규칙(1h→hour, 1d/1M→date).'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'stats.service.summary', 'module': 'oam-svc', 'method': 'GET',
     'path': '/api/v1/stats/service/summary',
     'summary': 'VoLTE + PTT 통합 요약 (두 KPI 를 한 응답에)',
     'params': [
         {'name': 'granularity', 'in': 'query', 'type': 'string', 'required': False,
          'enum': ['1h', '1d', '1M'], 'desc': '집계 단위 (기본 1d)'},
         {'name': 'date', 'in': 'query', 'type': 'string', 'required': False, 'desc': 'YYYY-MM-DD'},
         {'name': 'from', 'in': 'query', 'type': 'string', 'required': False, 'desc': '시작 일시'},
         {'name': 'to', 'in': 'query', 'type': 'string', 'required': False, 'desc': '종료 일시'},
     ],
     'response': '{granularity, from, to, volte{...}, ptt{...}}',
     'response_fields': [
         {'name': 'volte', 'type': 'object', 'desc': 'stats.service.volte 의 volte 객체와 동일 구조'},
         {'name': 'ptt', 'type': 'object', 'desc': 'stats.service.ptt 의 ptt 객체와 동일 구조'},
     ],
     'example': {'granularity': '1d', 'from': '2026-07-30 00:00:00', 'to': '2026-07-30 23:59:59',
                 'volte': {'total_attempts': 1234, 'success_rate': 97.3},
                 'ptt': {'total_calls': 87, 'avg_duration_sec': 44.2}},
     'errors': list(_ERR_COMMON),
     'notes': ['/stats/service 에 하위 경로를 주지 않으면 이 응답이 기본이다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'stats.service.live', 'module': 'oam-svc', 'method': 'GET', 'path': '/api/v1/stats/service/live',
     'summary': '실시간 서비스 현황 — 활성 호/발언 상태, 활성 그룹, RTP 용량, 이상 징후',
     'params': [],
     'response': '{ts, volte{kpi{},calls[]}, ptt{kpi{},groups[],talkers[]}, capacity{}, anomalies[]}',
     'response_fields': [
         {'name': 'ts', 'type': 'string', 'desc': 'ISO8601 — 스냅샷 시각'},
         {'name': 'volte.kpi.active', 'type': 'integer', 'unit': '건', 'desc': '통화 중(ringing 제외)'},
         {'name': 'volte.kpi.ringing', 'type': 'integer', 'unit': '건', 'desc': '호출 중'},
         {'name': 'volte.kpi.avg_duration_sec', 'type': 'integer', 'unit': '초', 'desc': '진행 호 평균 경과'},
         {'name': 'volte.kpi.registered', 'type': 'integer', 'unit': '개', 'desc': '등록 중 번호 수'},
         {'name': 'volte.kpi.numbers', 'type': 'integer', 'unit': '개', 'desc': '프로비저닝 번호 수'},
         {'name': 'volte.calls[]', 'type': 'object',
          'desc': '진행 호 목록 — {call_id, caller, callee, state, start_time}'},
         {'name': 'ptt.kpi.talking', 'type': 'integer', 'unit': '명', 'desc': '현재 발언 중(floor 보유)'},
         {'name': 'ptt.kpi.active_groups', 'type': 'integer', 'unit': '개', 'desc': '활성 그룹 수'},
         {'name': 'ptt.kpi.participants', 'type': 'integer', 'unit': '명', 'desc': '참여자 총수'},
         {'name': 'ptt.groups[]', 'type': 'object',
          'desc': '활성 그룹 — {group_id, name, floor_holder, participants}'},
         {'name': 'ptt.talkers[]', 'type': 'object', 'desc': '발언자 목록 (최근 floor 순)'},
         {'name': 'capacity.volte_rtp', 'type': 'object', 'desc': 'VoIP RTP 풀 {total,used,free}'},
         {'name': 'capacity.ptt_rtp', 'type': 'object', 'desc': 'PTT RTP 풀 {total,used,free}'},
         {'name': 'capacity.nodes', 'type': 'array', 'desc': '미디어 노드별 용량'},
         {'name': 'anomalies[]', 'type': 'object',
          'desc': '이상 징후 — {kind, label, detail, ref}. 장기 통화/floor 장기 점유 등'},
     ],
     'example': {'ts': '2026-07-30T09:12:03',
                 'volte': {'kpi': {'active': 2, 'ringing': 1, 'avg_duration_sec': 41,
                                   'registered': 120, 'numbers': 480},
                           'calls': [{'call_id': 'abc123', 'caller': '01000000001',
                                      'callee': '01000000002', 'state': 'talking',
                                      'start_time': '2026-07-30T09:11:22'}]},
                 'ptt': {'kpi': {'talking': 1, 'recent_active': 4, 'active_groups': 1,
                                 'participants': 6, 'total_groups': 24, 'registered': 96, 'numbers': 300},
                         'groups': [{'group_id': 'g-ops-1', 'name': '운영1팀',
                                     'floor_holder': '01000000003', 'participants': 6}],
                         'talkers': [{'user': '01000000003', 'group_id': 'g-ops-1'}]},
                 'capacity': {'volte_rtp': {'total': 2000, 'used': 6, 'free': 1994},
                              'ptt_rtp': {'total': 500, 'used': 2, 'free': 498}, 'nodes': []},
                 'anomalies': []},
     'errors': list(_ERR_COMMON),
     'notes': ['가입자별 state 파일 기준의 **현재 스냅샷**이다 — 이력 조회용이 아니다.',
               'VoLTE 는 caller/callee 두 state 파일을 call_id 로 dedup 해 1건으로 만든다.',
               '콘솔은 이 엔드포인트를 5초 주기로 폴링한다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'stats.service.trend', 'module': 'oam-svc', 'method': 'GET',
     'path': '/api/v1/stats/service/trend',
     'summary': '동시 사용량 추세 — 구간을 균등 버킷으로 나눈 시계열',
     'params': [{'name': 'window', 'in': 'query', 'type': 'string', 'required': False,
                 'desc': '조회 구간 (예: 1h·8h·24h. 기본 8h)'}],
     'response': '{window, window_min, bucket_sec, points[], peaks{}}',
     'response_fields': [
         {'name': 'window', 'type': 'string', 'desc': '요청한 구간 문자열'},
         {'name': 'window_min', 'type': 'integer', 'unit': '분', 'desc': '구간 길이'},
         {'name': 'bucket_sec', 'type': 'integer', 'unit': '초', 'desc': '버킷 폭'},
         {'name': 'points[].t', 'type': 'integer', 'desc': '버킷 시작 epoch 초'},
         {'name': 'points[].volte_active', 'type': 'integer', 'unit': '건', 'desc': '동시 통화 수'},
         {'name': 'points[].volte_calls', 'type': 'integer', 'unit': '건', 'desc': '버킷 내 발생 호'},
         {'name': 'points[].ptt_grants', 'type': 'integer', 'unit': '건', 'desc': 'floor 승인 수'},
         {'name': 'points[].ptt_speakers', 'type': 'integer', 'unit': '명', 'desc': '고유 발언자 수'},
         {'name': 'points[].ptt_groups', 'type': 'integer', 'unit': '개', 'desc': '활동 그룹 수'},
         {'name': 'peaks{}', 'type': 'object', 'desc': '지표별 최댓값 (points 의 각 키와 동일)'},
     ],
     'example': {'window': '8h', 'window_min': 480, 'bucket_sec': 600,
                 'points': [{'t': 1785000000, 'volte_active': 2, 'volte_calls': 14,
                             'ptt_grants': 31, 'ptt_speakers': 5, 'ptt_groups': 2}],
                 'peaks': {'volte_active': 6, 'volte_calls': 22, 'ptt_grants': 48,
                           'ptt_speakers': 9, 'ptt_groups': 3}},
     'errors': list(_ERR_COMMON),
     'notes': ['동일 window 요청은 캐시된다 — 연속 호출이 저렴하다.',
               'points 는 버킷이 비어도 0 으로 채워 등간격을 유지한다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'stats.service.events', 'module': 'oam-svc', 'method': 'GET',
     'path': '/api/v1/stats/service/events',
     'summary': '최근 서비스 이벤트 피드 (호 시작/종료, floor 승인 등)',
     'params': [{'name': 'limit', 'in': 'query', 'type': 'integer', 'required': False,
                 'desc': '건수 (기본 60)'}],
     'response': '{events[]}',
     'response_fields': [
         {'name': 'events[].ts', 'type': 'string', 'desc': '발생 시각 (최신순)'},
         {'name': 'events[].kind', 'type': 'string', 'desc': '이벤트 종류 (호 시작/종료·floor 등)'},
         {'name': 'events[].detail', 'type': 'string', 'desc': '사람이 읽는 요약 (발신 → 착신, 통화시간 등)'},
         {'name': 'events[].ref', 'type': 'string', 'desc': '연관 식별자 (call_id 또는 group_id)'},
     ],
     'example': {'events': [{'ts': '2026-07-30T09:11:22', 'kind': 'call_start',
                             'detail': '01000000001 → 01000000002 (음성)', 'ref': 'abc123'}]},
     'errors': list(_ERR_COMMON),
     'notes': ['표시용 피드다 — 정확한 이력 집계는 flow.call-logs / flow.ptt-history 를 쓴다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'stats.service.org', 'module': 'oam-svc', 'method': 'GET', 'path': '/api/v1/stats/service/org',
     'summary': '조직(부서) 트리별 서비스 이용 집계',
     'params': [],
     'response': '{orgs[], db_degraded?}',
     'response_fields': [
         {'name': 'orgs[].code', 'type': 'string', 'desc': '조직 코드'},
         {'name': 'orgs[].name', 'type': 'string', 'desc': '조직명'},
         {'name': 'orgs[].parent', 'type': 'string', 'desc': '상위 조직 코드 (루트는 null)'},
         {'name': 'orgs[].depth', 'type': 'integer', 'desc': '트리 깊이 (0=루트). 배열은 트리 순서(DFS)'},
         {'name': 'db_degraded', 'type': 'boolean',
          'desc': 'true 면 DB 조회 일부 실패로 컬럼이 빠졌다는 뜻 (키 자체가 없으면 정상)'},
     ],
     'example': {'orgs': [{'code': 'D100', 'name': '본사', 'parent': None, 'depth': 0},
                          {'code': 'D110', 'name': '운영팀', 'parent': 'D100', 'depth': 1}]},
     'errors': list(_ERR_COMMON),
     'notes': ['각 orgs 항목에는 이용 지표 컬럼이 함께 병합돼 온다 (하위 조직 롤업 포함).',
               '조직 트리에 없는 코드는 depth 0 의 별도 항목으로 붙는다(미지정 등).'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'stats.service.ptt-members', 'module': 'oam-svc', 'method': 'GET',
     'path': '/api/v1/stats/service/ptt-members',
     'summary': 'PTT 그룹 구성원 실시간 상태 (참여/발언 중 표시, 페이지네이션)',
     'params': [
         {'name': 'group', 'in': 'query', 'type': 'string', 'required': True, 'desc': 'MCPTT 그룹 ID'},
         {'name': 'page', 'in': 'query', 'type': 'integer', 'required': False, 'desc': '1-base 페이지 (기본 1)'},
         {'name': 'limit', 'in': 'query', 'type': 'integer', 'required': False, 'desc': '페이지 크기 (기본 50)'},
     ],
     'response': '{group, total, page, limit, active_count, floor_holder, floor_holders[], members[]}',
     'response_fields': [
         {'name': 'group', 'type': 'string', 'desc': '요청한 그룹 ID'},
         {'name': 'total', 'type': 'integer', 'unit': '명', 'desc': '그룹 구성원 총수'},
         {'name': 'active_count', 'type': 'integer', 'unit': '명', 'desc': '현재 참여 중 인원'},
         {'name': 'floor_holder', 'type': 'string', 'desc': '대표 발언자 MSISDN (없으면 null)'},
         {'name': 'floor_holders[]', 'type': 'string', 'desc': '동시 발언자 전원 (dual/multi 정책)'},
         {'name': 'members[].msisdn', 'type': 'string', 'desc': '구성원 번호'},
         {'name': 'members[].name', 'type': 'string', 'desc': '이름 (없으면 빈 문자열)'},
         {'name': 'members[].role', 'type': 'string', 'desc': '그룹 내 역할 (기본 member)'},
         {'name': 'members[].priority', 'type': 'integer', 'desc': 'floor 우선순위'},
         {'name': 'members[].active', 'type': 'boolean', 'desc': '현재 참여 중'},
         {'name': 'members[].talking', 'type': 'boolean', 'desc': '현재 발언 중'},
     ],
     'example': {'group': 'g-ops-1', 'total': 6, 'page': 1, 'limit': 50, 'active_count': 4,
                 'floor_holder': '01000000003', 'floor_holders': ['01000000003'],
                 'members': [{'msisdn': '01000000003', 'name': '김철수', 'role': 'member',
                              'priority': 5, 'active': True, 'talking': True}]},
     'errors': _ERR_COMMON + [
         {'status': 400, 'when': 'group 파라미터 누락', 'body': {'error': 'group required'}},
     ],
     'notes': ['members 는 priority 오름차순이다.',
               'floor_holder 는 floor_holders 의 첫 번째 — 단일 발언 정책이면 둘이 같다.'],
     'auth': dict(_AUTH_MONITOR)},
]
