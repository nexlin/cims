"""접속 서비스(`access_services`) 읽기 — 관리평면 소비자의 단일 진입점.

SoT 는 **대상 노드의 파일** `<install_path>/config/access_services.jsonl` 이다. CSP 가
그 파일을 직접 읽고, 콘솔 편집은 OAM → agent PUT 으로 그 파일에만 쓴다
(handlers/agents.py `_put_deployment_collection`).

그런데 OAM·CSC 는 CSP 와 같은 노드에 있으리라는 보장이 없다. 그래서 읽기를 두 층으로 둔다.

  1) **agent proxy** — csp deployment 에 `GET /collection`. 원본이라 항상 최신.
  2) **로컬 읽기 복제(미러)** — `modules/csp/runtime/collections/access_services/`.
     agent 가 도달 불가(모듈 정지·망단절)일 때의 폴백이자, **CSC 의 유일한 경로**다
     (CSC 에는 agent 클라이언트가 없다).

미러는 이름 그대로 **읽기 전용 복제**다. 쓰기 주체는 콘솔 → OAM → agent 하나뿐이며 미러는
그 PUT 이 성공한 뒤 따라 갱신된다(`refresh_mirror`). 미러 파일을 고쳐도 CSP 에는 반영되지
않는다 — 두 번째 쓰기 경로가 아니다.

왜 이 모듈이 필요한가: 소비자(통계 서비스 판정·가입자 도메인 해석·CSC realm 해석)가 각자
경로를 추측하다 서로 다른 곳을 보게 됐다. 컬렉션이 `sip_service` 에서 `access_services` 로
이관될 때 쓰는 쪽만 옮겨가고 읽는 쪽이 남은 결과다. 읽기 경로는 여기 하나로 모은다.
"""
import threading
import time

from services import file_store, ha_lookup
from util.log_util import Logger

logger = Logger()

COLLECTION = 'access_services'

# 캐시 — 통계 조회 1건마다 agent 왕복을 하지 않도록. 접속 서비스는 거의 바뀌지 않고,
# 바뀌면 PUT 경로가 미러와 캐시를 즉시 갱신하므로 TTL 은 폴백 안전망 역할만 한다.
_TTL_SEC = 30
_lock = threading.Lock()
_cache: dict = {'at': 0.0, 'records': None}


# ──────────────────────────────────────────────────────────────
#  원본 — agent proxy
# ──────────────────────────────────────────────────────────────

def _csp_deployment(config: dict):
    """csp 패키지의 deployment 1건. HA 그룹이면 첫 멤버(멤버 간 드리프트는 drift_sweeper 담당)."""
    from handlers.agents import _agent_load_all_deployments, _pkg_load

    for d in _agent_load_all_deployments(config) or []:
        if d.get('package_name') == 'csp':
            return d
        pkg = _pkg_load(config, pid=d.get('package_id')) or {}
        if pkg.get('name') == 'csp':
            d = dict(d)
            d['package_name'] = 'csp'
            return d
    return None


def fetch_from_agent(config: dict):
    """대상 노드에서 직접 읽는다. 실패(미배포·agent 불통)면 None — 빈 목록과 구분해야 한다."""
    from handlers.agents import _agent_load, _agent_proxy_call

    dep = _csp_deployment(config)
    if not dep or not dep.get('install_path'):
        return None
    agent = _agent_load(config, aid=dep.get('agent_id')) or {}
    status, body = _agent_proxy_call(
        'GET', agent, '/collection',
        {'install_path': dep['install_path'], 'name': COLLECTION},
        None, 10, config)
    if status != 200 or not isinstance(body, dict):
        return None
    records = body.get('records')
    return records if isinstance(records, list) else None


# ──────────────────────────────────────────────────────────────
#  읽기 복제(미러)
# ──────────────────────────────────────────────────────────────

def read_mirror(config: dict) -> list:
    return file_store.load_all(ha_lookup.collection_dir(config, COLLECTION))


def refresh_mirror(config: dict, records: list) -> bool:
    """미러를 records 로 맞춘다(없는 레코드는 삭제). 갱신했으면 True.

    소유권 리스가 없으면 write 가 거부된다(관리 store 는 단일 writer) — 그 경우는 조용히
    False. 미러는 복제일 뿐이라 실패해도 원본 읽기는 계속 동작한다.
    """
    if not isinstance(records, list):
        return False
    try:
        cdir = ha_lookup.collection_dir(config, COLLECTION, create=True)
        keep = set()
        for r in records:
            if not isinstance(r, dict):
                continue
            key = r.get('id') or r.get('name')
            if not key:
                continue
            keep.add(str(key))
            file_store.save(cdir, key, r)
        for old in file_store.load_all(cdir):
            key = str(old.get('id') or old.get('name') or '')
            if key and key not in keep:
                file_store.delete(cdir, key)
    except Exception as e:
        logger.log_warning(f"access_services 미러 갱신 실패 (원본 읽기는 계속 동작): {e}")
        return False
    with _lock:
        _cache['records'] = list(records)
        _cache['at'] = time.time()
    return True


# ──────────────────────────────────────────────────────────────
#  소비자 API
# ──────────────────────────────────────────────────────────────

def load(config: dict, force: bool = False) -> list:
    """접속 서비스 레코드 목록. 원본(agent) 우선, 불통이면 미러.

    원본을 읽었으면 미러도 따라 갱신한다 — 다음 불통 구간과 CSC 를 위해서다.
    """
    now = time.time()
    if not force:
        with _lock:
            if _cache['records'] is not None and now - _cache['at'] < _TTL_SEC:
                return _cache['records']

    records = None
    try:
        records = fetch_from_agent(config)
    except Exception as e:
        logger.log_warning(f"access_services agent 조회 실패 — 미러로 폴백: {e}")

    if records is not None:
        refresh_mirror(config, records)
        return records

    records = read_mirror(config)
    with _lock:
        _cache['records'] = records
        _cache['at'] = now
    return records


def domain_kind_map(config: dict) -> dict:
    """{도메인(소문자) → kind(소문자)}. enabled 인 것만, priority 오름차순 첫 항목 우선."""
    rows = [r for r in load(config)
            if isinstance(r, dict) and r.get('enabled') is not False
            and r.get('domain') and r.get('kind')]
    out = {}
    for r in sorted(rows, key=lambda x: (x.get('priority')
                                         if isinstance(x.get('priority'), int) else 1 << 30)):
        out.setdefault(str(r['domain']).lower(), str(r['kind']).lower())
    return out


def name_domain_map(config: dict) -> dict:
    """{service_ref(name) → domain}. 가입자 레코드의 service_ref 해석용."""
    out = {}
    for r in load(config):
        if isinstance(r, dict) and r.get('name') and r.get('domain'):
            out[r['name']] = r['domain']
    return out
