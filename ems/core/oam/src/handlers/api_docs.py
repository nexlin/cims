"""
API 문서 수집 REST API — 각 모듈이 자기 엔드포인트를 스스로 기술한 것을 모아서 준다.

문서의 소스는 **그 API 를 구현한 모듈의 코드 옆**이다 (핸들러 파일의 `*_API_DOCS`). 중앙 카탈로그가
아니라서:
  - 모듈이 설치·가용해야 그 모듈의 API 문서도 존재한다. csc 미설치면 가입자/조직/PTT그룹 API 는
    아예 나오지 않는다 (`csc/src/handlers/admin.py`·`org.py` 자체가 없으므로 import 실패).
  - 경로·파라미터를 고칠 때 같은 파일의 문서를 같이 고치게 된다 (문서-코드 drift 최소화).

Routes (mounted at /api/v1/api-docs):
  GET /api/v1/api-docs   가용 모듈의 API 문서 전체 → {modules[], count, apis[]}

**소비처는 여기서 모른다.** 어떤 위젯이 어떤 API 를 쓰는지는 콘솔의 `WidgetDef.apis`(id 목록)가
선언하고, 콘솔이 id 로 골라 쓴다. 이 선언은 "이 API 가 무엇인가" 만 담는다.

가용 판정은 `console_layouts.installed_services()` 를 그대로 쓴다 (role=base 는 게이트웨이 라우트
테이블이 권위, role=all 은 in-process 전체). 즉 "모듈 파일이 import 되나" AND "그 서비스가 가용하나".

엔트리 스키마 (모듈이 선언):
  id       고유 id (예: 'stats.service.volte') — 콘솔 WidgetDef.apis 가 참조하는 키
  module   제공 모듈 — 'csc' | 'oam-svc' | None(base 상주). 가용 판정 키.
  method   HTTP 메서드
  path     전체 경로 (/api/v1 포함)
  summary  한 줄 설명
  params   [{name, in(query|path|body), type, required, enum, desc}]
  response 응답 요약
  auth     필요 권한 (예: 'Bearer JWT (monitor)')
"""
from urllib.parse import urlparse
from pathlib import PurePath
import time

from httpsrv.handler import HandlerArgs, HandlerResult


_BASE = '/api/v1/api-docs'
_REMOTE_TTL = 60          # 원격 모듈 문서 캐시(초) — 문서는 배포 단위로만 바뀐다
_remote_cache: dict = {}  # module → (fetched_at, apis)


def _parts(full_path: str):
    path = urlparse(full_path).path
    try:
        return tuple(PurePath(path).relative_to(PurePath(_BASE)).parts)
    except ValueError:
        return ()


def _bearer(handler_args: HandlerArgs) -> str:
    """원격 모듈 문서 조회에 그대로 전달할 호출자 토큰."""
    for k, v in (getattr(handler_args, 'headers', None) or {}).items():
        if k.lower() == 'authorization' and isinstance(v, str):
            return v.split(None, 1)[1] if v.lower().startswith('bearer ') else v
    return ''


# ── 모듈별 문서 로더 ────────────────────────────────────────────────────────
#  각 로더는 독립 try 로 호출된다 — 모듈 미설치/import 실패는 그 그룹만 빠지고 나머지는 살아있다.
#  (oam_app 의 `try: from handlers.admin import ...` 선택 로드와 같은 규약.)

def _agent_docs():
    from handlers.agents import CIMS_AGENT_API_DOCS
    return CIMS_AGENT_API_DOCS


def _stats_docs():
    from handlers.stats import CIMS_STATS_API_DOCS
    return CIMS_STATS_API_DOCS


def _recording_docs():
    from handlers.recording import CIMS_RECORDING_API_DOCS
    return CIMS_RECORDING_API_DOCS


def _flow_docs():
    from services.flow_logger import FLOW_API_DOCS
    return FLOW_API_DOCS


def _csc_admin_docs():
    # csc 모듈이 배포될 때 OAM handlers/ 에 설치된다 — 미설치면 ImportError.
    from handlers.admin import CIMS_ADMIN_API_DOCS
    return CIMS_ADMIN_API_DOCS


def _csc_org_docs():
    from handlers.org import CIMS_ORG_API_DOCS
    return CIMS_ORG_API_DOCS


_LOADERS = (_agent_docs, _stats_docs, _recording_docs, _flow_docs, _csc_admin_docs, _csc_org_docs)


def _declared() -> list:
    out = []
    for load in _LOADERS:
        try:
            out.extend(load() or [])
        except Exception:
            continue   # 모듈 미설치 — 그 API 는 존재하지 않으므로 문서도 없다
    return out


def _available(config: dict) -> set:
    """가용 서비스 모듈명(소문자). 라우트 테이블은 대문자로 저장하므로 양쪽 다 소문자로 비교한다."""
    try:
        from handlers.console_layouts import installed_services
        return {str(s).lower() for s in (installed_services(config) or set())}
    except Exception:
        return set()


def _upstreams(config: dict) -> dict:
    """가용 모듈 → 업스트림 URL 목록(최근 등록 우선). 같은 세그먼트에 라우트가 여럿(HA/잔재)일 수 있어
    리스트로 들고 순서대로 시도한다."""
    try:
        from handlers import gateway
        routes = sorted(gateway.enabled_routes(config),
                        key=lambda r: r.get('id') or 0, reverse=True)
    except Exception:
        return {}
    out: dict = {}
    for r in routes:
        mod = str(r.get('module') or '').lower()
        up = str(r.get('upstream') or '').rstrip('/')
        if not mod or not up:
            continue
        lst = out.setdefault(mod, [])
        if up not in lst:
            lst.append(up)
    return out


def _fetch_remote(module: str, upstreams: list, token: str) -> list:
    """원격 모듈이 직접 서비스하는 자기 문서를 가져온다 (분리 배포에서 import 불가한 모듈).
    호출자의 토큰을 그대로 전달한다 (subscriber_import 와 같은 규약)."""
    hit = _remote_cache.get(module)
    if hit and (time.time() - hit[0]) < _REMOTE_TTL:
        return hit[1]
    apis: list = []
    try:
        import requests
        try:
            import urllib3
            urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
        except Exception:
            pass
        for up in upstreams:
            try:
                r = requests.get(up + _BASE, headers={'Authorization': f'Bearer {token}'},
                                 verify=False, timeout=5)
            except Exception:
                continue
            if r.status_code == 200:
                body = r.json()
                apis = [a for a in (body.get('apis') or []) if isinstance(a, dict)]
                break
    except Exception:
        apis = []
    _remote_cache[module] = (time.time(), apis)
    return apis


def collect(config: dict, token: str = '') -> list:
    """가용 모듈의 API 문서 전체.

    어떤 위젯이 어떤 API 를 쓰는지는 **콘솔의 WidgetDef.apis(id 목록)** 가 선언한다 — 여기서는
    필터하지 않고 전부 준다(콘솔이 id 로 골라 쓴다). 백엔드 선언은 "이 API 가 무엇인가" 만 담는다.

    소스 두 갈래:
      1) 로컬 import — 그 모듈 코드가 이 OAM 에 있는 경우 (base 상주 + 동일 호스트 서비스 모듈).
      2) 업스트림 조회 — 분리 배포로 코드가 없는 가용 모듈 (예: 다른 서버의 csc). 모듈이 자기 문서를
         직접 서비스하므로 가져와 병합한다.
    """
    avail = _available(config)
    declared = _declared()

    apis = [a for a in declared
            if not a.get('module') or str(a['module']).lower() in avail]

    local_mods = {str(a['module']).lower() for a in declared if a.get('module')}
    ups = _upstreams(config)
    for mod in sorted(avail - local_mods):
        if ups.get(mod):
            apis += _fetch_remote(mod, ups[mod], token)

    return apis


async def handle_api_docs(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    method = handler_args.method.upper()

    if method != 'GET':
        return HandlerResult(status=405, body={'error': 'method_not_allowed'})
    if _parts(handler_args.full_path):
        return HandlerResult(status=404, body={'error': 'not_found'})

    try:
        apis = collect(config, _bearer(handler_args))
        return HandlerResult(status=200, body={
            'modules': sorted({a['module'] for a in apis if a.get('module')}),
            'count': len(apis),
            'apis': apis,
        })
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


CIMS_API_DOCS_HANDLER_LIST = [
    (_BASE, handle_api_docs, {}),
]
