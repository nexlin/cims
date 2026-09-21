"""계측기 저장소 — 시나리오/프로파일(패키지 동봉 YAML + 운영자 추가분), 토폴로지(Tester.DataDir),
run 색인(관리 store) + run 본체(Tester.DataDir).

- 시나리오·프로파일 = 파일. 패키지 `scenarios/` 가 기본이고, `Tester.DataDir/scenarios/` 에
  운영자가 추가한 것이 같은 id 로 있으면 그것이 이긴다(패키지 업그레이드에 살아남는다).
- 토폴로지 = `Tester.DataDir/topologies/` 의 file_store 레코드(`<id>.json` + `.seq`, 레코드 1개 = 토폴로지 1개).
  운영자 정의(시나리오·creds)와 같은 자리 — DataDir 하나만 옮기면 계측기 정의 전부가 따라간다.
  옛 자리(관리 store 도메인 `modules/oam-cims-tester/runtime/topologies`)에 레코드가 있고 새 자리가 비어
  있으면 기동 때 한 번 복사해 잇는다.
- run 색인 = `modules/oam-cims-tester/runtime/runs` (RunRecord 요약). 1초 버킷 지표·이벤트는
  `Tester.DataDir/runs/<id>/` — jsonl 레코드 스토어의 대상이 아니다(test_instrument.md §1).
"""
from __future__ import annotations

import glob
import os
import shutil
import time
from datetime import datetime
from typing import Dict, List, Optional, Tuple

import yaml

from services import file_store
from services.tester_models import LoadProfile, Scenario, Topology, RunRecord, validate, normalize_topology_doc

LEGACY_DOMAIN_TOPOLOGIES = 'modules/oam-cims-tester/runtime/topologies'   # 이어받기 원본(관리 store)
DOMAIN_RUNS = 'modules/oam-cims-tester/runtime/runs'

_component_root = ''
_config: dict = {}
_data_dir_cache = None


def init(component_root: str, config: dict) -> None:
    global _component_root, _config, _data_dir_cache
    _component_root = component_root
    _config = config or {}
    _data_dir_cache = None
    os.makedirs(runs_dir(), exist_ok=True)
    os.makedirs(user_scenarios_dir(), exist_ok=True)
    _migrate_topologies()


def _mtime(p: str) -> float:
    try:
        return os.path.getmtime(p)
    except OSError:
        return 0.0


def _has_content(d: str) -> bool:
    """data 디렉터리에 실제 파일이 있는가 — 설치가 만든 빈 runs/·scenarios/ 만 있는 것은 내용이 아니다."""
    for _root, _dirs, files in os.walk(d):
        if files:
            return True
    return False


def _default_data_dir() -> str:
    """기본 DataDir — 배포 레이아웃(`<모듈>/<버전>/<모듈>/` + `<모듈>/runtime/`)이면 버전과 무관한 `runtime/data`,
    아니면(소스 트리·단독 실행) 컴포넌트 아래 `data`. agent 가 만드는 `runtime/` 은 업그레이드에 살아남는다
    (인증서와 같은 자리 — agent.md). 처음 옮겨 갈 때는 기존 `data`(이 버전 디렉터리, 비었으면 가장 최근 형제 버전 디렉터리)를 복사해 잇는다."""
    legacy = os.path.join(_component_root, 'data')
    runtime = os.path.normpath(os.path.join(_component_root, '..', '..', 'runtime'))
    if not os.path.isdir(runtime):
        return legacy
    d = os.path.join(runtime, 'data')
    if not os.path.isdir(d):
        # 이어받을 원본 — 이 버전 디렉터리의 data, 없으면(업그레이드 직후라 비어 있다) 형제 버전 디렉터리 중 내용이 있는 가장 최근 것
        mod = os.path.basename(_component_root)
        cands = [legacy] + sorted(glob.glob(os.path.join(runtime, '..', '*', mod, 'data')), key=_mtime, reverse=True)
        src = next((c for c in cands if _has_content(c)), None)
        try:
            if src:
                shutil.copytree(src, d)
            else:
                os.makedirs(d, exist_ok=True)
        except OSError:
            return legacy
    return d


def data_dir() -> str:
    global _data_dir_cache
    d = ((_config.get('Tester') or {}).get('DataDir') or '').strip()
    if d:
        return d
    if _data_dir_cache is None:
        _data_dir_cache = _default_data_dir()
    return _data_dir_cache


def runs_dir() -> str:
    return os.path.join(data_dir(), 'runs')


def run_dir(run_id: str) -> str:
    return os.path.join(runs_dir(), file_store._safe_key(run_id))


def bundled_scenarios_dir() -> str:
    return os.path.join(_component_root, 'scenarios')


def user_scenarios_dir() -> str:
    return os.path.join(data_dir(), 'scenarios')


def topologies_dir() -> str:
    return os.path.join(data_dir(), 'topologies')


# ── YAML 파일 계열 (시나리오·프로파일) ─────────────────────────────────────

def _yaml_files(root: str, sub: Optional[str] = None, exclude_sub: Tuple[str, ...] = ()) -> List[str]:
    base = os.path.join(root, sub) if sub else root
    out = []
    if not os.path.isdir(base):
        return out
    for dp, dns, fns in os.walk(base):
        rel = os.path.relpath(dp, root)
        if any(rel == ex or rel.startswith(ex + os.sep) for ex in exclude_sub):
            continue
        for fn in fns:
            if fn.endswith(('.yaml', '.yml')) and not fn.startswith('topology'):
                out.append(os.path.join(dp, fn))
    return sorted(out)


def _load_yaml(path: str) -> dict:
    with open(path, 'r', encoding='utf-8') as f:
        doc = yaml.safe_load(f)
    return doc if isinstance(doc, dict) else {}


def list_scenarios() -> List[dict]:
    """[{id,title,tags,source,path,errors}] — 검증 실패한 파일도 errors 와 함께 목록에 남긴다
    (조용히 사라지면 오타 난 시나리오를 찾을 수 없다)."""
    seen: Dict[str, dict] = {}
    for source, root in (('bundled', bundled_scenarios_dir()), ('user', user_scenarios_dir())):
        for p in _yaml_files(root, exclude_sub=('profiles',)):
            doc = _load_yaml(p)
            model, errs = validate('scenario', doc)
            sid = (model.id if model else str(doc.get('id') or os.path.basename(p)))
            seen[sid] = {'id': sid, 'title': (model.title if model else doc.get('title')),
                         'tags': (model.tags if model else doc.get('tags') or []),
                         'steps': (len(model.flow) if model else 0),
                         'source': source, 'path': p, 'errors': errs}
    return sorted(seen.values(), key=lambda r: r['id'])


def get_scenario(sid: str) -> Tuple[Optional[Scenario], Optional[dict], List[str]]:
    for row in list_scenarios():
        if row['id'] == sid:
            doc = _load_yaml(row['path'])
            model, errs = validate('scenario', doc)
            return model, doc, errs
    return None, None, [f'시나리오 없음: {sid}']


def list_profiles() -> List[dict]:
    seen: Dict[str, dict] = {}
    for source, root in (('bundled', bundled_scenarios_dir()), ('user', user_scenarios_dir())):
        for p in _yaml_files(root, sub='profiles'):
            doc = _load_yaml(p)
            model, errs = validate('profile', doc)
            name = (model.name if model and model.name else None) or os.path.splitext(os.path.basename(p))[0]
            seen[name] = {'name': name, 'model': (model.model if model else doc.get('model')),
                          'source': source, 'path': p, 'errors': errs}
    return sorted(seen.values(), key=lambda r: r['name'])


def get_profile(name: str) -> Tuple[Optional[LoadProfile], Optional[dict], List[str]]:
    for row in list_profiles():
        if row['name'] == name:
            doc = _load_yaml(row['path'])
            model, errs = validate('profile', doc)
            return model, doc, errs
    return None, None, [f'프로파일 없음: {name}']


# ── 운영자 YAML 쓰기 (시나리오·프로파일) ─────────────────────────────────────
#  콘솔 편집기가 저장하는 곳 = Tester.DataDir/scenarios/ (프로파일은 그 아래 profiles/). 패키지 동봉본은
#  읽기 전용 — 같은 id 로 저장하면 운영자본이 덮어쓰기(override)가 되고, 삭제는 운영자본만 가능하다
#  (동봉본을 지우면 패키지 업그레이드 때 되살아나 혼란만 남는다).

def read_yaml_text(path: str) -> str:
    with open(path, 'r', encoding='utf-8') as f:
        return f.read()


def _safe_name(key: str) -> str:
    return file_store._safe_key(key)


def _user_scenario_path(sid: str) -> str:
    return os.path.join(user_scenarios_dir(), _safe_name(sid) + '.yaml')


def _user_profile_path(name: str) -> str:
    return os.path.join(user_scenarios_dir(), 'profiles', _safe_name(name) + '.yaml')


def _parse_yaml_text(text: str) -> Tuple[Optional[dict], List[str]]:
    try:
        doc = yaml.safe_load(text)
    except Exception as e:
        return None, [f'YAML 파싱 실패: {e}']
    if not isinstance(doc, dict):
        return None, ['YAML 최상위는 매핑이어야 한다']
    return doc, []


def save_scenario_yaml(sid: str, text: str) -> Tuple[Optional[dict], List[str]]:
    """검증 통과분만 저장. 문서의 id 는 경로의 id 와 같아야 한다(id 바꾸기 = 새 파일). 반환 (목록 행, errors)."""
    doc, errs = _parse_yaml_text(text)
    if errs:
        return None, errs
    model, errs = validate('scenario', doc)
    if errs:
        return None, errs
    if model.id != sid:
        return None, [f'id 불일치: 경로 {sid} ≠ 문서 {model.id} — id 를 바꾸려면 새 시나리오로 저장한다']
    path = _user_scenario_path(sid)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text if text.endswith('\n') else text + '\n')
    for row in list_scenarios():
        if row['id'] == sid:
            return row, []
    return None, ['저장 후 목록에서 찾지 못함']


def delete_scenario(sid: str) -> Tuple[bool, Optional[str]]:
    """운영자본만 지운다. 반환 (지움, 거절 사유)."""
    for row in list_scenarios():
        if row['id'] == sid:
            if row['source'] != 'user':
                return False, 'bundled'
            try:
                os.remove(row['path'])
            except FileNotFoundError:
                pass
            return True, None
    return False, 'not_found'


def save_profile_yaml(name: str, text: str) -> Tuple[Optional[dict], List[str]]:
    doc, errs = _parse_yaml_text(text)
    if errs:
        return None, errs
    model, errs = validate('profile', doc)
    if errs:
        return None, errs
    if model.name and model.name != name:
        return None, [f'name 불일치: 경로 {name} ≠ 문서 {model.name}']
    path = _user_profile_path(name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text if text.endswith('\n') else text + '\n')
    for row in list_profiles():
        if row['name'] == name:
            return row, []
    return None, ['저장 후 목록에서 찾지 못함']


def delete_profile(name: str) -> Tuple[bool, Optional[str]]:
    for row in list_profiles():
        if row['name'] == name:
            if row['source'] != 'user':
                return False, 'bundled'
            try:
                os.remove(row['path'])
            except FileNotFoundError:
                pass
            return True, None
    return False, 'not_found'


# ── 토폴로지 (Tester.DataDir/topologies — file_store 레코드 꼴) ──────────────

def _topo_dir() -> str:
    d = topologies_dir()
    os.makedirs(d, exist_ok=True)
    return d


def _legacy_topo_dir() -> str:
    """옛 자리(관리 store 도메인) — 만들지 않고 경로만(domain_dir 는 makedirs 한다)."""
    return os.path.join(file_store.runtime_root(_config), file_store._domain_rel(LEGACY_DOMAIN_TOPOLOGIES))


def _migrate_topologies() -> int:
    """관리 store 의 토폴로지 레코드를 `Tester.DataDir/topologies/` 로 한 번 이어받는다.
    새 자리에 레코드가 하나라도 있으면 건드리지 않는다(운영자가 그 뒤 옛 자리를 고쳤더라도 새 자리가 정본).
    `.seq` 도 함께 옮겨 id 가 이어진다. 원본은 지우지 않는다(되돌리기·구버전 병행). 반환 = 옮긴 레코드 수."""
    dst = _topo_dir()
    if glob.glob(os.path.join(dst, '*.json')):
        return 0
    src = _legacy_topo_dir()
    if not os.path.isdir(src) or os.path.realpath(src) == os.path.realpath(dst):
        return 0
    n = 0
    try:
        for fn in os.listdir(src):
            is_rec = fn.endswith('.json') and not fn.startswith('.')
            if is_rec or fn == '.seq':
                shutil.copy2(os.path.join(src, fn), os.path.join(dst, fn))
                n += 1 if is_rec else 0
    except OSError:
        return n
    return n


def is_topology_v1(doc: dict) -> bool:
    """이전 꼴(`target.csp/csc/oam` · `workers[].url` · `pools.*.bind.ip`) 인가 — hosts 가 없고 target.csp 가 있으면 v1."""
    return isinstance(doc, dict) and 'hosts' not in doc and isinstance(doc.get('target'), dict) and 'csp' in doc['target']


def _host_of_url(url: str) -> Tuple[str, int]:
    u = str(url).split('://', 1)[-1].split('/', 1)[0]
    host, _, port = u.rpartition(':')
    if not host:
        return u, 7100
    try:
        return host, int(port)
    except ValueError:
        return u, 7100


def topology_v1_to_v2(doc: dict) -> dict:
    """v1 레코드 → 호스트›워커·대상 노드›풀 3단(§4) 기계적 변환 — 기존 레코드 승계.
    csp/csc/oam → 호스트 하나(+주소가 다른 것은 호스트 추가) + 노드 셋, workers[].url → 호스트+포트, pools.*.bind.ip → 그 주소의 워커.
    UE 풀은 첫 워커에 놓는다(v1 은 컨트롤러가 워커 사이를 나눴지만 v2 는 풀 정의가 나눈다)."""
    t = doc.get('target') or {}
    csp = t.get('csp') or {}
    hosts: Dict[str, dict] = {}
    by_ip: Dict[str, str] = {}

    def host_for(ip: str, name: Optional[str] = None) -> str:
        ip = str(ip or '')
        if ip in by_ip:
            return by_ip[ip]
        hid = 'h' + ''.join(ch for ch in ip.replace('.', '_') if ch.isalnum() or ch == '_')
        if not hid or hid == 'h':
            hid = f'h{len(hosts) + 1}'
        hid = hid.lower()
        base, n = hid, 1
        while hid in hosts:
            n += 1
            hid = f'{base}_{n}'
        hosts[hid] = {'ip': ip, **({'name': name} if name else {})}
        by_ip[ip] = hid
        return hid

    target_host = host_for(csp.get('ip'), t.get('name'))
    domains = [d for d in (csp.get('domain_volte'), csp.get('domain_ptt')) if d]
    nodes: Dict[str, dict] = {'csp': {
        'role': 'sip', 'fn': 'CSP', 'host': target_host, 'procs': ['csp'],
        'sip': {'access': {'udp': csp.get('udp', 5060), 'tcp': csp.get('tcp', 25061), 'tls': csp.get('tls', 5061), 'domains': domains}},
    }}
    if csp.get('peering'):
        pr = csp['peering']
        nodes['csp']['sip']['peering'] = {'port': pr.get('port'), 'protocol': pr.get('protocol', 'udp'),
                                          'local_node': pr.get('local_node', 'cims-tester-peering')}
    if t.get('csc'):
        c = t['csc']
        nodes['csc'] = {'role': 'subscriber', 'fn': 'CSC', 'host': host_for(c.get('host')), 'procs': ['csc'],
                        'api': {'port': c.get('port', 4430), 'tls': bool(c.get('tls', True))}}
    if t.get('oam'):
        o = t['oam']
        oh, op = _host_of_url(o.get('url', ''))
        tls = str(o.get('url', '')).startswith('https')
        blk = {'port': op, 'tls': tls, 'observe': list(t.get('observe') or [])}
        if o.get('token_env'):
            blk['token_env'] = o['token_env']
        if o.get('csp_deployment_id'):
            blk['csp_deployment_id'] = o['csp_deployment_id']
        nodes['oam'] = {'role': 'oam', 'fn': 'OAM', 'host': host_for(oh), 'procs': ['oam'], 'oam': blk}
    workers = []
    worker_by_ip: Dict[str, str] = {}
    for i, w in enumerate(doc.get('workers') or []):
        if not isinstance(w, dict) or not w.get('url'):
            continue
        ip, port = _host_of_url(w['url'])
        name = str(w.get('name') or f'w{i + 1}')
        name = ''.join(ch if ch.isalnum() or ch == '_' else '_' for ch in name).lower()
        if not name or not name[0].isalpha():
            name = f'w{i + 1}'
        row = {'name': name, 'host': host_for(ip), 'port': port}
        if w.get('cpus'):
            row['cpus'] = w['cpus']
        workers.append(row)
        worker_by_ip.setdefault(ip, name)
    if not workers:
        workers.append({'name': 'w1', 'host': host_for('127.0.0.1', 'localhost'), 'port': 7100})
    pools = {}
    for pname, p in (doc.get('pools') or {}).items():
        q = dict(p)
        if q.get('kind') == 'peer':
            bind = dict(q.get('bind') or {})
            bip = str(bind.pop('ip', '') or '')
            q['bind'] = bind
            q['worker'] = worker_by_ip.get(bip) or workers[0]['name']
            q['peering'] = 'csp'
        else:
            q['worker'] = workers[0]['name']
            q['access'] = 'csp'
            src = dict(q.get('source') or {})
            if src.get('db') == 'target':
                src['db'] = 'csc' if 'csc' in nodes else 'csp'
                q['source'] = src
        pools[pname] = q
    return {'name': doc.get('name'), 'hosts': hosts, 'workers': workers,
            'target': {'name': t.get('name') or doc.get('name'), 'kind': 'cims', 'nodes': nodes}, 'pools': pools}


def _normalize_rec(rec: Optional[dict]) -> Optional[dict]:
    """읽기 경로 — v1 레코드는 v2 로, 노드 `sip.access/peering` 꼴은 `sip.listeners` 로 바꿔 돌려주고, 쓸 수 있으면 그 자리에서
    승계 저장한다."""
    if rec is None:
        return None
    doc = rec.get('doc') or {}
    changed = False
    if is_topology_v1(doc):
        rec = dict(rec)
        doc = rec['doc'] = topology_v1_to_v2(doc)
        rec['migrated_from'] = 'v1'
        changed = True
    normalized = normalize_topology_doc(doc)
    if normalized is not doc:
        rec = dict(rec)
        rec['doc'] = normalized
        changed = True
    if changed:
        try:
            file_store.save(_topo_dir(), rec['id'], rec)
        except Exception:
            pass
    return rec


def list_topologies() -> List[dict]:
    rows = [_normalize_rec(r) for r in file_store.load_all(_topo_dir())]
    return sorted(rows, key=lambda r: r.get('id', 0))


def get_topology(tid: int) -> Optional[dict]:
    return _normalize_rec(file_store.by_id(_topo_dir(), int(tid)))


def find_topology(ref) -> Optional[dict]:
    """id(정수 또는 숫자 문자열) 또는 name 으로."""
    if ref is None:
        return None
    if isinstance(ref, int) or (isinstance(ref, str) and ref.isdigit()):
        return get_topology(int(ref))
    for r in list_topologies():
        if (r.get('doc') or {}).get('name') == ref or r.get('name') == ref:
            return r
    return None


def save_topology(doc: dict, tid: Optional[int] = None) -> Tuple[Optional[dict], List[str]]:
    """검증 통과분만 저장. 반환 (레코드, errors)."""
    if is_topology_v1(doc):
        doc = topology_v1_to_v2(doc)
    doc = normalize_topology_doc(doc)
    model, errs = validate('topology', doc)
    if errs:
        return None, errs
    d = _topo_dir()
    now = datetime.now().isoformat(timespec='seconds')
    if tid is None:
        rec = {'id': file_store.next_id(d), 'created_at': now}
    else:
        rec = file_store.by_id(d, int(tid))
        if rec is None:
            return None, [f'토폴로지 없음: {tid}']
    rec.update({'name': model.name, 'doc': model.model_dump(by_alias=True, exclude_none=True),
                'updated_at': now})
    file_store.save(d, rec['id'], rec)
    return rec, []


def delete_topology(tid: int) -> bool:
    return file_store.delete(_topo_dir(), int(tid))


def topology_model(rec: dict) -> Optional[Topology]:
    model, _ = validate('topology', rec.get('doc') or {})
    return model


# ── run 색인 ───────────────────────────────────────────────────────────────

def _runs_index_dir() -> str:
    return file_store.domain_dir(_config, DOMAIN_RUNS)


def list_runs(limit: int = 100, filters: Optional[dict] = None) -> List[dict]:
    """run 색인(최신순). filters = {scenario, build, verdict(콤마 구분), since(ISO 또는 일수 '7d'), profile, label(부분 일치), topology, load(bool)}."""
    rows = file_store.load_all(_runs_index_dir())
    f = filters or {}
    if f.get('scenario'):
        rows = [r for r in rows if r.get('scenario_id') == f['scenario']]
    if f.get('build'):
        rows = [r for r in rows if str(r.get('target_build') or '') == str(f['build'])]
    if f.get('verdict'):
        want = {v.strip() for v in str(f['verdict']).split(',') if v.strip()}
        rows = [r for r in rows if r.get('verdict') in want]
    if f.get('profile'):
        rows = [r for r in rows if (r.get('profile') or '') == f['profile']]
    if f.get('topology'):
        rows = [r for r in rows if (r.get('topology') or '') == f['topology']]
    if f.get('load') in (True, '1', 'true'):
        rows = [r for r in rows if r.get('profile')]
    if f.get('label'):
        q = str(f['label']).lower()
        rows = [r for r in rows if q in str(r.get('label') or '').lower() or q in str(r.get('id') or '').lower()
                or q in str(r.get('scenario_id') or '').lower()]
    if f.get('since'):
        since = str(f['since']).strip()
        try:
            if since.endswith('d') and since[:-1].isdigit():
                cut = datetime.fromtimestamp(time.time() - int(since[:-1]) * 86400).isoformat(timespec='seconds')
            else:
                cut = datetime.fromisoformat(since).isoformat(timespec='seconds')
            rows = [r for r in rows if (r.get('started_at') or '') >= cut]
        except ValueError:
            pass
    rows.sort(key=lambda r: r.get('started_at') or '', reverse=True)
    return rows[:max(1, limit)]


def get_run(run_id: str) -> Optional[dict]:
    return file_store.load(_runs_index_dir(), run_id)


def save_run_index(rec: RunRecord) -> dict:
    row = rec.model_dump(exclude_none=True)
    file_store.save(_runs_index_dir(), rec.id, row)
    return row


def delete_run(run_id: str) -> bool:
    """run 색인 + 본체 디렉터리 제거(진행 중은 호출자가 막는다). 반환=색인이 있었는가."""
    ok = file_store.delete(_runs_index_dir(), run_id)
    shutil.rmtree(run_dir(run_id), ignore_errors=True)
    return ok


def purge_runs(retain_days: int) -> int:
    """보존기간 지난 run 색인 + 디렉터리 제거. 0=무제한. 반환=지운 run 수."""
    if not retain_days or retain_days <= 0:
        return 0
    cutoff = time.time() - retain_days * 86400
    n = 0
    for row in file_store.load_all(_runs_index_dir()):
        try:
            started = datetime.fromisoformat(row.get('started_at')).timestamp()
        except Exception:
            continue
        if started >= cutoff or row.get('verdict') == 'running':
            continue
        rid = str(row.get('id'))
        file_store.delete(_runs_index_dir(), rid)
        shutil.rmtree(run_dir(rid), ignore_errors=True)
        n += 1
    return n
