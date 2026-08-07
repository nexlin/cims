"""API 게이트웨이 — base OAM 단일 오리진(4419) 뒤 독립 서비스 모듈로의 경로 세그먼트 프록시.

oam_base_service_split P1 (§5). base OAM 이 `/api/v1/<service>/*` 를 라우트 테이블(file_store
control/gateway_routes)에 따라 업스트림으로 프록시한다. upstream host 는 기본 loopback
(127.0.0.1 — base 와 모듈 동거 배치)이고, 모듈이 base 와 다른 호스트에 배치되면(분리
토폴로지) 운영자가 배포 설정 `Server.GatewayHost` 에 그룹 VIP(권장)/노드 IP 를 명시해
그 주소로 등록된다. 서비스 모듈은 설치 시 자기 라우트를 self-register(POST
/api/v1/gateway/routes)하며, base 코어 수정 없이 새 서비스가 테이블 한 줄로 추가된다.
미등록/disabled/업스트림 부재 → 503 (I3 장애격리).

불변식:
  - I1 단일 공개 오리진: 브라우저는 4419 만 본다. 업스트림은 게이트웨이만 접근하는
    비공개 주소 (loopback 또는 운영자 명시 관리망 주소).
  - I3 단방향: base → service 프록시만. 업스트림 부재 시 base 정상, 해당 라우트만 503.
  - 인증 공유(§5): 게이트웨이는 Authorization 헤더를 전달만 하고, 각 모듈이 동일 JwtSecret 로
    토큰을 독립 검증한다(base 에 되묻지 않음).

라우트 마운트(register_gateway)는 --role base 에서만 — --role all 은 서비스 핸들러가 in-process
라 프록시가 필요 없다(세그먼트 충돌 방지).

Routes (control plane, base 귀속, 모든 role 에서 등록):
  GET    /api/v1/gateway/routes              라우트 테이블 목록 {routes:[...]}
  POST   /api/v1/gateway/routes              self-register(upsert by segment) (admin)
  GET    /api/v1/gateway/routes/{id}         1건
  DELETE /api/v1/gateway/routes/{id}         삭제 (admin)
  GET    /api/v1/gateway/health              게이트웨이 상태 + 업스트림 liveness
"""
from urllib.parse import urlparse, unquote
from pathlib import PurePath
from datetime import datetime
import json
import asyncio

from httpsrv.handler import HandlerArgs, HandlerResult
from services import file_store
from util.log_util import Logger
from handlers import auth

try:
    import aiohttp   # OAM vendor 기본 async HTTP 클라이언트
except Exception:    # vendor 부재 환경 — 프록시 비활성, 제어 API 는 동작
    aiohttp = None


_BASE = '/api/v1/gateway'
_DOMAIN = 'gateway_routes'   # file_store control 카테고리 (file_store._OAM_CATEGORY)

# 프록시가 업스트림으로 전달할 요청 헤더 화이트리스트 (hop-by-hop/Host 제외).
_REQ_HEADER_ALLOW = {
    'authorization', 'content-type', 'accept', 'accept-language',
    'if-none-match', 'if-modified-since', 'range', 'x-request-id',
}
# 업스트림 응답에서 클라이언트로 보존할 헤더 (ETag/304·다운로드·캐시·Range).
_RESP_HEADER_ALLOW = {
    'etag', 'content-disposition', 'cache-control', 'last-modified',
    'content-range', 'accept-ranges', 'vary',
}

_DEFAULT_TIMEOUT = 5.0          # 일반 프록시 타임아웃(초)
_STREAM_TIMEOUT = 120.0         # 대용량(녹취 등) 다운로드 타임아웃

_logger = Logger()
_session = None                 # lazy aiohttp.ClientSession (bind 되는 event loop = 서버 루프)
_ADMIN_SERVER = None            # register_gateway 에서 set — 런타임 hot-mount/unmount 용 (role base)
_GW_CONFIG = None               # 〃 — proxy 핸들러 kwargs 의 config


def _get_session():
    global _session
    if aiohttp is None:
        return None
    if _session is None or _session.closed:
        _session = aiohttp.ClientSession()
    return _session


# ──────────────────────────────────────────────────────────────────────────
#  Route table (file_store control/gateway_routes)
# ──────────────────────────────────────────────────────────────────────────

def _dir(config):
    return file_store.domain_dir(config, _DOMAIN)


def _parts(full_path: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(_BASE))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _body_dict(handler_args: HandlerArgs):
    b = getattr(handler_args, 'body', None)
    if isinstance(b, dict):
        return b
    if isinstance(b, (bytes, bytearray)):
        try: return json.loads(b.decode('utf-8'))
        except Exception: return None
    if isinstance(b, str):
        try: return json.loads(b)
        except Exception: return None
    return None


def _normalize_segment(seg: str) -> str:
    """라우트 세그먼트(=마운트 base_path) 정규화. 반드시 /api/v1/ 하위, trailing slash 제거."""
    seg = (seg or '').strip()
    if not seg:
        return ''
    if not seg.startswith('/'):
        seg = '/' + seg
    seg = seg.rstrip('/')
    return seg


def _normalize_upstream(up: str) -> str:
    """업스트림 base URL 정규화 — http(s) + 호스트 필수.

    호스트는 loopback(동거 배치 기본) 외에 운영자가 배포 설정 `Server.GatewayHost`
    로 명시한 원격 호스트(분리 배치의 그룹 VIP/노드 IP)도 허용한다. I1(단일 공개
    오리진)은 유지 — 브라우저는 여전히 4419 만 보고, 업스트림은 게이트웨이만
    접근하는 비공개 주소다. 0.0.0.0/빈 호스트 등 무효 주소만 거부."""
    up = (up or '').strip().rstrip('/')
    if not up:
        return ''
    p = urlparse(up)
    if p.scheme not in ('http', 'https'):
        return ''
    host = (p.hostname or '').lower()
    if not host or host in ('0.0.0.0', '::'):
        return ''   # bind-any 주소는 도달 불능 — upstream 으로 무효
    return up


def load_routes(config: dict) -> list:
    """라우트 테이블 전체. enabled 무관 모든 레코드."""
    return file_store.load_all(_dir(config))


def enabled_routes(config: dict) -> list:
    return [r for r in load_routes(config)
            if r.get('enabled', True) and r.get('segment') and r.get('upstream')]


def upsert_route(config: dict, route: dict) -> dict:
    """segment 기준 upsert(self-register 멱등). 검증 실패 시 ValueError."""
    seg = _normalize_segment(route.get('segment'))
    up = _normalize_upstream(route.get('upstream'))
    if not seg.startswith('/api/v1/'):
        raise ValueError(f'segment must start with /api/v1/ : {route.get("segment")!r}')
    if not up:
        raise ValueError(f'upstream must be a reachable http(s) URL : {route.get("upstream")!r}')
    d = _dir(config)
    existing = file_store.find_by(d, lambda r: _normalize_segment(r.get('segment')) == seg)
    now = datetime.now().isoformat(timespec='seconds')
    rec = dict(existing or {})
    rec.update({
        'segment': seg,
        'upstream': up,
        'module': str(route.get('module', '') or rec.get('module', '')),
        'enabled': bool(route.get('enabled', rec.get('enabled', True))),
        'deprecated': bool(route.get('deprecated', rec.get('deprecated', False))),
        'sunset': route.get('sunset', rec.get('sunset')),
        'requires_base_oam': route.get('requires_base_oam', rec.get('requires_base_oam')),
        'updated_at': now,
    })
    if not existing:
        rec['id'] = file_store.next_id(d)
        rec['registered_at'] = now
    file_store.save(d, rec['id'], rec)
    return rec


def seed_routes(config: dict) -> int:
    """라우트 테이블이 비었을 때 base.json Gateway.Routes(또는 기본 csc) 로 시드.
    멱등 — 이미 있으면 0. 반환=시드한 라우트 수."""
    if load_routes(config):
        return 0
    seeds = ((config.get('Gateway') or {}).get('Routes')) or _DEFAULT_SEED_ROUTES
    n = 0
    for s in seeds:
        try:
            upsert_route(config, s)
            n += 1
        except ValueError as e:
            _logger.log_warning(f'[gateway] seed route skip ({s.get("segment")}): {e}')
    return n


# self-register 로 전환 — base 는 서비스 모듈을 미리 알지 않는다(하드코딩 시드 제거).
#   라우트는 각 서비스 모듈 배포 시 OAM 이 그 모듈의 선언 세그먼트(pkg meta.gateway.routes)+
#   실제 Server.Port 로 등록(register_module_routes)한다. base.json Gateway.Routes 로 수동
#   시드도 가능하나 기본은 비움.
_DEFAULT_SEED_ROUTES = []


# ──────────────────────────────────────────────────────────────────────────
#  Proxy handler — registered per-segment (route in kwargs['_route'])
# ──────────────────────────────────────────────────────────────────────────

def _filter_req_headers(headers: dict) -> dict:
    out = {}
    for k, v in (headers or {}).items():
        if k.lower() in _REQ_HEADER_ALLOW:
            out[k] = v
    return out


def _filter_resp_headers(headers) -> dict:
    out = {}
    try:
        items = headers.items()
    except AttributeError:
        items = (headers or {}).items()
    for k, v in items:
        if k.lower() in _RESP_HEADER_ALLOW:
            out[k] = v
    return out


def _ssl_param(upstream: str):
    """https 업스트림은 TLS 검증 비활성 — 모듈들이 self-signed cert 라 검증 무의미
    (loopback 은 물론, 운영자가 명시한 관리망 내 원격 upstream 도 동일 전제).
    http 업스트림은 None(무시). aiohttp 의 request(ssl=...) 인자로 전달."""
    p = urlparse(upstream or '')
    if p.scheme == 'https':
        return False
    return None


def _request_body(handler_args: HandlerArgs):
    """controller 가 파싱한 body 를 업스트림 전송 형태로 환원.
    반환: (content: bytes|None, json_obj: dict|list|None) — 둘 중 하나만 사용."""
    b = getattr(handler_args, 'body', None)
    if b is None:
        return None, None
    if isinstance(b, (bytes, bytearray)):
        return bytes(b), None
    if isinstance(b, str):
        return b.encode('utf-8'), None
    if isinstance(b, (dict, list)):
        return None, b
    return None, None


async def proxy(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    route = kwargs.get('_route') or {}
    upstream = route.get('upstream')
    if aiohttp is None or not upstream:
        return HandlerResult(status=503, body={'error': 'gateway upstream unavailable',
                                               'segment': route.get('segment')})
    session = _get_session()
    if session is None:
        return HandlerResult(status=503, body={'error': 'gateway client unavailable'})

    # 전체 경로 보존 — 업스트림이 /api/v1/<service>/... 를 그대로 받는다(D2: 모듈이 세그먼트 소유).
    path = urlparse(handler_args.full_path).path
    url = upstream + path
    method = (handler_args.method or 'GET').upper()
    req_headers = _filter_req_headers(handler_args.headers)
    content, json_obj = _request_body(handler_args)
    is_download = 'recordings' in path or path.rstrip('/').endswith('download')
    timeout = aiohttp.ClientTimeout(total=_STREAM_TIMEOUT if is_download else _DEFAULT_TIMEOUT)

    try:
        kw = dict(params=handler_args.query_params or None,
                  headers=req_headers or None, timeout=timeout, allow_redirects=False,
                  ssl=_ssl_param(upstream))
        if json_obj is not None:
            kw['json'] = json_obj
        elif content is not None:
            kw['data'] = content
        async with session.request(method, url, **kw) as resp:
            status = resp.status
            ct = resp.headers.get('Content-Type', '')
            resp_headers = _filter_resp_headers(resp.headers)
            body = await resp.read()
    except asyncio.TimeoutError:
        _logger.log_error(f'[gateway] proxy {method} {url} timeout')
        return HandlerResult(status=504, body={'error': 'gateway timeout', 'upstream': upstream})
    except Exception as exc:
        _logger.log_error(f'[gateway] proxy {method} {url} failed: {exc}')
        return HandlerResult(status=502, body={'error': 'bad gateway',
                                               'detail': str(exc), 'upstream': upstream})

    # D6: deprecation alias 면 RFC 8594 Sunset/Deprecation 헤더 부착 + 호출 로깅.
    if route.get('deprecated'):
        resp_headers['Deprecation'] = 'true'
        if route.get('sunset'):
            resp_headers['Sunset'] = str(route['sunset'])
        _logger.log_info(f'[gateway] deprecated route hit: {method} {path} (sunset={route.get("sunset")})')

    # JSON 은 dict/list 로 환원해 _http_response 가 JSONResponse 로 직렬화하도록(스큐 없음).
    if 'application/json' in ct.lower() and body:
        try:
            return HandlerResult(status=status, body=json.loads(body), headers=resp_headers)
        except Exception:
            pass
    return HandlerResult(status=status, body=bytes(body),
                         headers=resp_headers, media_type=ct or None)


# ──────────────────────────────────────────────────────────────────────────
#  register_gateway — --role base 시작 시 세그먼트별 프록시 마운트
# ──────────────────────────────────────────────────────────────────────────

def _config_rollback_mark() -> "str | None":
    """배포 overlay 옆의 `config.json.rolled-back` 마커 — 되돌린 시각(ISO) 또는 None.

    agent 의 `start_oam` 이 기동 실패 시 직전 정상 설정으로 되돌리면 남긴다
    (lifecycle.sh `_oam_rollback_last_good`). 다음 성공 기동에서 지워진다."""
    import os as _os
    try:
        base = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))  # .../oam/src → .../oam
        for cand in (_os.path.join(base, 'config.json.rolled-back'),
                     _os.path.join(_os.path.dirname(base), 'config.json.rolled-back')):
            if _os.path.isfile(cand):
                with open(cand) as f:
                    return (f.read().strip() or 'unknown')[:64]
    except Exception:
        pass
    return None


def register_gateway(admin_server, config: dict) -> int:
    """라우트 테이블의 enabled 라우트마다 프록시 동적 라우트를 등록.
    반환=마운트한 라우트 수. base 고유 경로(/api/v1/users/me 등)는 controller 최장 일치로
    base 가 우선 — 게이트웨이는 더 구체적이지 않은 세그먼트만 잡는다.
    (/api/v1/stats 는 전체가 oam-svc 귀속 세그먼트 — base 직접 서빙 없음.)"""
    global _ADMIN_SERVER, _GW_CONFIG
    _ADMIN_SERVER = admin_server          # 런타임 self-register hot-mount 용
    _GW_CONFIG = config
    seeded = seed_routes(config)
    if seeded:
        _logger.log_info(f'[gateway] seeded {seeded} route(s) (table was empty)')
    n = 0
    for r in enabled_routes(config):
        seg = _normalize_segment(r.get('segment'))
        if not seg:
            continue
        admin_server.add_dynamic_rules([(seg, proxy, {'config': config, '_route': r})])
        _logger.log_info(f"[gateway] mount {seg} → {r.get('upstream')} (module={r.get('module')})")
        n += 1
    # self-heal — 배포는 있는데 라우트가 빠진 모듈 복구 (register_module_routes 가
    # hot-mount 까지 수행하므로 여기서 바로 서비스 가능 상태가 된다).
    n += reconcile_routes_from_deployments(config)
    return n


# ── 런타임 hot-mount / unmount (self-register) ──────────────────────────────
def mount_route(route: dict) -> bool:
    """라우트 1개를 라이브 프록시로 즉시 mount. role base(register_gateway 후)에서만 유효.
    role all 등 _ADMIN_SERVER 미설정 시 no-op(persist 만)."""
    if _ADMIN_SERVER is None or _GW_CONFIG is None:
        return False
    seg = _normalize_segment(route.get('segment'))
    if not seg or not route.get('enabled', True):
        return False
    _ADMIN_SERVER.add_dynamic_rule(seg, proxy, {'config': _GW_CONFIG, '_route': route})
    _logger.log_info(f"[gateway] hot-mount {seg} → {route.get('upstream')} (module={route.get('module')})")
    return True


def unmount_route(segment: str) -> bool:
    if _ADMIN_SERVER is None:
        return False
    seg = _normalize_segment(segment)
    if not seg:
        return False
    try:
        _ADMIN_SERVER.del_dynamic_rule(seg)
        _logger.log_info(f"[gateway] hot-unmount {seg}")
        return True
    except Exception as e:
        _logger.log_warning(f"[gateway] unmount {seg} failed: {e}")
        return False


def register_module_routes(config: dict, module: str, ip: str, port, segments) -> int:
    """서비스 모듈 배포 시 self-register: 모듈이 선언한 세그먼트들을 그 모듈의 실제
    (ip=배포 config 의 Server.GatewayHost, 미지정 시 loopback / port=Server.Port=SoT)
    https upstream 으로 등록+hot-mount. 멱등(segment upsert). base 가 서비스 모듈을
    미리 알 필요 없음(시드 하드코딩 대체)."""
    ip = ip or '127.0.0.1'
    base = f"https://{ip}:{port}"
    n = 0
    for seg in (segments or []):
        try:
            rec = upsert_route(config, {'segment': seg, 'upstream': base, 'module': module})
            mount_route(rec)
            n += 1
        except ValueError as e:
            _logger.log_warning(f"[gateway] self-register {module} {seg} skip: {e}")
    if n:
        _logger.log_info(f"[gateway] self-register {module}: {n} route(s) → {base}")
    return n


def reconcile_routes_from_deployments(config: dict) -> int:
    """라우트 테이블 self-heal — gateway.routes 를 선언한 패키지의 배포가 있는데
    그 세그먼트가 테이블에 없으면 실효 upstream(Server.GatewayHost/Port)으로 재등록.
    과거 삭제·드리프트로 빠진 라우트를 기동 시 복구한다. 존재하는 레코드는 건드리지
    않음 — upstream 값 정정은 설정 저장 전파가 담당. 반환=복구한 라우트 수."""
    try:
        from handlers.agents import (_deploy_load_all, _pkg_load,
                                     effective_server_port, effective_gateway_host)
    except Exception:
        return 0
    existing = {_normalize_segment(r.get('segment')) for r in load_routes(config)}
    n = 0
    for d in _deploy_load_all(config):
        try:
            pkg = _pkg_load(config, d.get('package_id')) or {}
            meta = pkg.get('meta') if isinstance(pkg, dict) else None
            segs = ((meta or {}).get('gateway') or {}).get('routes') or []
            missing = [s for s in segs if _normalize_segment(s) not in existing]
            if not missing:
                continue
            port = effective_server_port(config, pkg, d.get('config'))
            if not port:
                continue
            host = effective_gateway_host(config, pkg, d.get('config')) or '127.0.0.1'
            proc = d.get('process_name') or (pkg.get('name') if isinstance(pkg, dict) else None)
            if not proc:
                continue
            n += register_module_routes(config, proc, host, int(port), missing)
            existing.update(_normalize_segment(s) for s in missing)
        except Exception as e:
            _logger.log_warning(f"[gateway] route reconcile skip (dep={d.get('id')}): {e}")
    if n:
        _logger.log_info(f"[gateway] route reconcile: {n} route(s) 복구")
    return n


def deregister_module_routes(config: dict, module: str) -> int:
    """모듈 제거 시: 그 모듈의 라우트 전부 unmount + 레지스트리 삭제."""
    if not module:
        return 0
    d = _dir(config)
    n = 0
    for r in load_routes(config):
        if str(r.get('module', '')) == str(module):
            unmount_route(r.get('segment'))
            file_store.delete(d, r.get('id'))
            n += 1
    if n:
        _logger.log_info(f"[gateway] deregister {module}: {n} route(s)")
    return n


# ──────────────────────────────────────────────────────────────────────────
#  Control-plane API (/api/v1/gateway/*) — 모든 role 에서 등록
# ──────────────────────────────────────────────────────────────────────────

async def _upstream_alive(upstream: str) -> bool:
    if aiohttp is None or not upstream:
        return False
    session = _get_session()
    if session is None:
        return False
    try:
        async with session.get(upstream + '/health',
                               timeout=aiohttp.ClientTimeout(total=2.0),
                               ssl=_ssl_param(upstream)) as resp:
            return resp.status < 500
    except Exception:
        return False


async def handle_gateway(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    parts = _parts(handler_args.full_path)
    method = (handler_args.method or 'GET').upper()

    # GET /api/v1/gateway/health — 게이트웨이 + 업스트림 liveness
    if len(parts) == 1 and parts[0] == 'health' and method == 'GET':
        routes = load_routes(config)
        checks = await asyncio.gather(
            *[_upstream_alive(r.get('upstream')) for r in routes],
            return_exceptions=True,
        ) if routes else []
        items = []
        for r, ok in zip(routes, checks):
            items.append({'segment': r.get('segment'), 'upstream': r.get('upstream'),
                          'module': r.get('module'), 'enabled': r.get('enabled', True),
                          'alive': bool(ok) if not isinstance(ok, Exception) else False})
        # 관리 store 소유권(리스) 상태 동봉 — 콘솔이 read-only 배너를 띄우는 근거.
        # (base 평면 상태를 한 응답으로 보게 해 새 라우트를 늘리지 않는다.)
        from services import lease as _lease
        _ls = _lease.state()
        return HandlerResult(status=200, body={'proxy_enabled': aiohttp is not None,
                                               'routes': items,
                                               'lease': {
                                                   'active': bool(_ls.get('active')),
                                                   'reason': _ls.get('reason'),
                                                   'node_id': _ls.get('node_id'),
                                                   'epoch': _ls.get('epoch'),
                                                   'lost_at': _ls.get('lost_at'),
                                               },
                                               'read_only': not bool(_ls.get('active')),
                                               # 설정 자가 복구 흔적 — agent 가 기동 실패 후
                                               # 직전 정상 설정으로 되돌렸으면 마커가 남는다.
                                               # 조용히 되돌리면 운영자는 자기가 저장한 설정이
                                               # 적용된 줄 알기 때문에 반드시 노출한다.
                                               'config_rolled_back': _config_rollback_mark()})

    # /api/v1/gateway/routes ...
    if parts and parts[0] == 'routes':
        # GET list
        if len(parts) == 1 and method == 'GET':
            payload, err = auth.require_auth(handler_args)
            if err:
                return err
            return HandlerResult(status=200, body={'routes': load_routes(config)})

        # POST self-register (upsert)
        if len(parts) == 1 and method == 'POST':
            payload, err = auth.require_admin(handler_args)
            if err:
                return err
            body = _body_dict(handler_args)
            if not isinstance(body, dict):
                return HandlerResult(status=400, body={'error': 'json body required'})
            try:
                rec = upsert_route(config, body)
            except ValueError as e:
                return HandlerResult(status=400, body={'error': str(e)})
            mount_route(rec)   # hot-mount — 재기동 없이 즉시 프록시 활성
            return HandlerResult(status=200, body={'route': rec})

        # GET / DELETE by id
        if len(parts) == 2:
            try:
                rid = int(parts[1])
            except ValueError:
                return HandlerResult(status=400, body={'error': 'invalid route id'})
            d = _dir(config)
            if method == 'GET':
                payload, err = auth.require_auth(handler_args)
                if err:
                    return err
                rec = file_store.by_id(d, rid)
                if not rec:
                    return HandlerResult(status=404, body={'error': 'route not found'})
                return HandlerResult(status=200, body={'route': rec})
            if method == 'DELETE':
                payload, err = auth.require_admin(handler_args)
                if err:
                    return err
                _rec = file_store.by_id(d, rid)   # segment 확보(unmount 용)
                ok = file_store.delete(d, rid)
                if ok and _rec:
                    unmount_route(_rec.get('segment'))   # hot-unmount
                return HandlerResult(status=200 if ok else 404,
                                     body={'deleted': ok, 'id': rid})

    return HandlerResult(status=404, body={'error': 'Not Found'})


CIMS_GATEWAY_HANDLER_LIST = [
    (_BASE, handle_gateway, {}),
]
