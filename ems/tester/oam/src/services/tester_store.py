"""계측기 저장소 — 시나리오/프로파일(패키지 동봉 YAML + 운영자 추가분), 토폴로지(관리 store),
run 색인(관리 store) + run 본체(Tester.DataDir).

- 시나리오·프로파일 = 파일. 패키지 `scenarios/` 가 기본이고, `Tester.DataDir/scenarios/` 에
  운영자가 추가한 것이 같은 id 로 있으면 그것이 이긴다(패키지 업그레이드에 살아남는다).
- 토폴로지 = file_store 도메인 `modules/oam-cims-tester/runtime/topologies` (레코드 1개 = 토폴로지 1개).
- run 색인 = `modules/oam-cims-tester/runtime/runs` (RunRecord 요약). 1초 버킷 지표·이벤트는
  `Tester.DataDir/runs/<id>/` — jsonl 레코드 스토어의 대상이 아니다(test_instrument.md §1).
"""
from __future__ import annotations

import os
import shutil
import time
from datetime import datetime
from typing import Dict, List, Optional, Tuple

import yaml

from services import file_store
from services.tester_models import LoadProfile, Scenario, Topology, RunRecord, validate

DOMAIN_TOPOLOGIES = 'modules/oam-cims-tester/runtime/topologies'
DOMAIN_RUNS = 'modules/oam-cims-tester/runtime/runs'

_component_root = ''
_config: dict = {}


def init(component_root: str, config: dict) -> None:
    global _component_root, _config
    _component_root = component_root
    _config = config or {}
    os.makedirs(runs_dir(), exist_ok=True)
    os.makedirs(user_scenarios_dir(), exist_ok=True)


def data_dir() -> str:
    d = ((_config.get('Tester') or {}).get('DataDir') or '').strip()
    if not d:
        d = os.path.join(_component_root, 'data')
    return d


def runs_dir() -> str:
    return os.path.join(data_dir(), 'runs')


def run_dir(run_id: str) -> str:
    return os.path.join(runs_dir(), file_store._safe_key(run_id))


def bundled_scenarios_dir() -> str:
    return os.path.join(_component_root, 'scenarios')


def user_scenarios_dir() -> str:
    return os.path.join(data_dir(), 'scenarios')


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


# ── 토폴로지 (관리 store 레코드) ────────────────────────────────────────────

def _topo_dir() -> str:
    return file_store.domain_dir(_config, DOMAIN_TOPOLOGIES)


def list_topologies() -> List[dict]:
    rows = file_store.load_all(_topo_dir())
    return sorted(rows, key=lambda r: r.get('id', 0))


def get_topology(tid: int) -> Optional[dict]:
    return file_store.by_id(_topo_dir(), int(tid))


def save_topology(doc: dict, tid: Optional[int] = None) -> Tuple[Optional[dict], List[str]]:
    """검증 통과분만 저장. 반환 (레코드, errors)."""
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


def list_runs(limit: int = 100) -> List[dict]:
    rows = file_store.load_all(_runs_index_dir())
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
