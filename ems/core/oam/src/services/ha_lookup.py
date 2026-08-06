"""
ha_group ↔ deployment ↔ agent 매핑 헬퍼.

흐름 A (모듈 통째 config) / 흐름 B (런타임 컬렉션) 양쪽이 같은 룰로 fan-out 하려면
"이 패키지(csp 등) 를 호스팅하는 ha_group 의 모든 멤버 agent_id" 가 필요하다.

여기서는 file_store 만 의존 — DB / HTTP 없음. 호출자는 thread offload 권장.
"""
from __future__ import annotations

import os as _os
from typing import Optional

from services import file_store


_HA_DOMAIN = 'ha_groups'
_DEPLOY_DOMAIN = 'deployments'
_PKG_DOMAIN = 'packages'
_AGENT_DOMAIN = 'agents'


def _ha_dir(config):
    return file_store.domain_dir(config, _HA_DOMAIN)


def _deploy_dir(config):
    return file_store.domain_dir(config, _DEPLOY_DOMAIN)


def _pkg_dir(config):
    return file_store.domain_dir(config, _PKG_DOMAIN)


def ha_groups_all(config) -> list[dict]:
    return file_store.load_all(_ha_dir(config))


def ha_group_by_id(config, gid: int) -> Optional[dict]:
    return file_store.by_id(_ha_dir(config), gid)


def save_group(config, group: dict) -> dict:
    """그룹 레코드 atomic 저장 — 콘솔 메타 갱신용 (id 필수)."""
    file_store.save(_ha_dir(config), int(group['id']), group)
    return group


def members_of(group: dict) -> list[dict]:
    """그룹 dict 의 members 리스트. 없으면 빈 리스트."""
    ms = group.get('members') or []
    return [m for m in ms if m.get('agent_id') is not None]


def _deployments_for_agent(config, agent_id: int) -> list[dict]:
    return [d for d in file_store.load_all(_deploy_dir(config))
            if d.get('agent_id') == agent_id]


def _pkg_name(config, pid: Optional[int]) -> Optional[str]:
    if pid is None:
        return None
    p = file_store.by_id(_pkg_dir(config), pid)
    return p.get('name') if p else None


def group_vip_set(group: dict) -> set:
    """그룹의 VIP 집합 — vip_bindings[].ip ∪ legacy 단일 vip."""
    vips = set()
    for b in (group.get('vip_bindings') or []):
        ip = (b or {}).get('ip')
        if ip:
            vips.add(str(ip))
    if group.get('vip'):
        vips.add(str(group['vip']))
    return vips


def vip_observation(config, group: dict, stale_sec: int = 90) -> dict:
    """AS 그룹의 실측 ACTIVE 판정 — agent heartbeat(기본 2s 주기, OAM 불통 시
    backoff 최대 60s)가 보고하는 interfaces[](secondary IP 포함 — VIP 추적용)에
    그룹 VIP 가 붙은 멤버 찾기.
    agent 수정·재배포 없이 동작 (데이터는 이미 heartbeat 로 도착, 계산만 추가).

    반환 {'active_agent_id': int|None, 'observed': {agent_id: True|False|None}}:
      observed  True=VIP 보유 / False=미보유 / None=판정 불가(heartbeat stale·VIP 미정의)
      active_agent_id  비-stale 멤버 중 정확히 1명이 보유할 때만 확정 — 0명(이동 중)·
        2명(절체 직후 관측 창)·전원 stale 이면 None. 애매하면 None: 자동 교정이
        잘못된 방향으로 복사하지 않도록 보수적으로 판정한다.
    """
    from datetime import datetime
    vips = group_vip_set(group)
    agents_dir = file_store.domain_dir(config, _AGENT_DOMAIN)
    now = datetime.now()
    observed: dict = {}
    for m in members_of(group):
        aid = m['agent_id']
        a = file_store.by_id(agents_dir, aid) or {}
        stale = True
        hb = a.get('last_heartbeat')
        if hb:
            try:
                stale = (now - datetime.fromisoformat(str(hb))).total_seconds() > stale_sec
            except (ValueError, TypeError):
                stale = True
        if stale or not vips:
            observed[aid] = None
            continue
        observed[aid] = any(str(r.get('ip')) in vips
                            for r in (a.get('interfaces') or []) if isinstance(r, dict))
    holders = [aid for aid, v in observed.items() if v is True]
    return {'active_agent_id': holders[0] if len(holders) == 1 else None,
            'observed': observed}


def auto_sync_enabled(group: dict, pkg_name: str) -> bool:
    """그룹×패키지 자동 동기화 스위치 — AS 그룹만 의미 있음, 부재 시 기본 ON.
    AA/standalone 은 동기화 개념 자체가 없다 (호출측이 mode 로 거른다)."""
    if group.get('mode') != 'active_standby':
        return False
    au = group.get('auto_sync')
    if isinstance(au, dict) and pkg_name in au:
        return bool(au[pkg_name])
    return True


def deployments_in_group_for_package(config, gid: int, package_name: str) -> list[dict]:
    """ha_group 의 모든 멤버 중 package_name 으로 deploy 된 deployment row 모음.

    각 row 에 'agent_id' 와 'package_name' 이 채워진 상태로 반환 (caller 편의).
    file_store 의 deployments 가 package_name 을 직접 안 들고 있는 경우 packages
    도메인을 lookup 해서 enrich.
    """
    g = ha_group_by_id(config, gid)
    if not g:
        return []
    member_ids = {m['agent_id'] for m in members_of(g)}
    if not member_ids:
        return []
    out: list[dict] = []
    pkg_cache: dict = {}
    for d in file_store.load_all(_deploy_dir(config)):
        aid = d.get('agent_id')
        if aid not in member_ids:
            continue
        name = d.get('package_name')
        if not name:
            pid = d.get('package_id')
            if pid not in pkg_cache:
                pkg_cache[pid] = _pkg_name(config, pid)
            name = pkg_cache[pid]
            if name:
                d = dict(d)
                d['package_name'] = name
        if name == package_name:
            out.append(d)
    return out


def packages_in_group(config, group: dict) -> set:
    """그룹 멤버들이 호스팅하는 패키지 이름 집합 — auto-sync 스위퍼의 순회 대상."""
    member_ids = {m['agent_id'] for m in members_of(group)}
    if not member_ids:
        return set()
    out: set = set()
    pkg_cache: dict = {}
    for d in file_store.load_all(_deploy_dir(config)):
        if d.get('agent_id') not in member_ids:
            continue
        name = d.get('package_name')
        if not name:
            pid = d.get('package_id')
            if pid not in pkg_cache:
                pkg_cache[pid] = _pkg_name(config, pid)
            name = pkg_cache[pid]
        if name:
            out.add(name)
    return out


def ha_group_for_package(config, package_name: str) -> Optional[dict]:
    """package_name (예: 'csp') 을 호스팅하는 ha_group 1건.

    멤버 중 1명 이상이 이 패키지를 deploy 했으면 그 그룹. 여러 그룹에 분산되어
    있으면 첫 매치 반환 — 그러나 일반 운영에서는 1:1 가정 (csp 는 control 그룹
    에만, cmp 는 media 그룹에만). 호출자가 멀티-매치 가능성을 의식해야 하는
    환경 (예: 검증 단계) 에선 ha_groups_for_package() 사용.
    """
    groups = ha_groups_for_package(config, package_name)
    return groups[0] if groups else None


def ha_groups_for_package(config, package_name: str) -> list[dict]:
    """package_name 을 호스팅하는 ha_group 들. 멤버 중 1명 이상이 deploy 했으면 매치."""
    all_groups = ha_groups_all(config)
    if not all_groups:
        return []
    # agent_id → group_id 역인덱스
    agent_to_group: dict[int, int] = {}
    for g in all_groups:
        gid = g.get('id')
        for m in members_of(g):
            agent_to_group[m['agent_id']] = gid

    matched_gids: set[int] = set()
    pkg_cache: dict = {}
    for d in file_store.load_all(_deploy_dir(config)):
        aid = d.get('agent_id')
        gid = agent_to_group.get(aid)
        if gid is None:
            continue
        name = d.get('package_name')
        if not name:
            pid = d.get('package_id')
            if pid not in pkg_cache:
                pkg_cache[pid] = _pkg_name(config, pid)
            name = pkg_cache[pid]
        if name == package_name:
            matched_gids.add(gid)

    by_id = {g['id']: g for g in all_groups if g.get('id') is not None}
    return [by_id[g] for g in matched_gids if g in by_id]


def fanout_targets_for_collection(config, collection: str) -> list[dict]:
    """런타임 컬렉션 (csp_listener / sip_trunk / routing_rule / ...) 1건 변경 시
    sync_config job 을 enqueue 할 대상 deployment 리스트.

    현재 컬렉션은 모두 csp 패키지 소유. 향후 cmp 등 다른 패키지의 컬렉션이 추가되면
    여기서 mapping 확장.

    Returns: [{ agent_id, deployment_id, install_path, package_name, ha_group_id, ha_group_name, ha_group_mode }, ...]
    """
    pkg_name = _collection_owner_package(collection)
    if not pkg_name:
        return []
    groups = ha_groups_for_package(config, pkg_name)
    if not groups:
        return []
    out: list[dict] = []
    seen_agents: set[int] = set()
    for g in groups:
        gid = g.get('id')
        for d in deployments_in_group_for_package(config, gid, pkg_name):
            aid = d.get('agent_id')
            if aid in seen_agents:
                continue
            seen_agents.add(aid)
            out.append({
                'agent_id':      aid,
                'deployment_id': d.get('id'),
                'install_path':  d.get('install_path'),
                'package_name':  pkg_name,
                'ha_group_id':   gid,
                'ha_group_name': g.get('name'),
                'ha_group_mode': g.get('mode'),
            })
    return out


# ── 컬렉션 → 소유 패키지 매핑 (csp_runtime.py 의 5 컬렉션 + local_nodes/remote_nodes 등) ─
_COLLECTION_OWNER = {
    'csp_listener':         'csp',
    'sip_trunk':            'csp',
    'routing_rule':         'csp',
    'routing_access_list':  'csp',
    'sip_service':          'csp',
    'local_nodes':          'csp',
    'remote_nodes':         'csp',
    'access_services':      'csp',
    'routes':               'csp',
    'route_sets':           'csp',
    'routing_policies':     'csp',
    'rules':                'csp',
    'rule_sets':            'csp',
    'acl_policies':         'csp',
}


def _collection_owner_package(collection: str) -> Optional[str]:
    return _COLLECTION_OWNER.get(collection)


def register_collection_owner(collection: str, package_name: str) -> None:
    """런타임에서 컬렉션→패키지 매핑 추가 (cmp 등 미래 확장).

    (owner, name) 유일성 강제 — 다른 패키지가 이미 같은 collection 이름을 소유하면
    충돌이므로 거부 (runtime store v2 P3: 평면 네임스페이스 이름충돌 차단).
    URL/API 가 collection 을 이름으로만 식별하므로 이름은 전역 유일해야 한다.
    """
    existing = _COLLECTION_OWNER.get(collection)
    if existing and existing != package_name:
        raise ValueError(
            f"collection name collision: '{collection}' 는 이미 '{existing}' 소유 — "
            f"'{package_name}' 로 재지정 불가. 모듈별 고유 이름 사용 필요.")
    _COLLECTION_OWNER[collection] = package_name


# ── 컬렉션 SoT 디렉터리 — 모듈/소유자 네임스페이스 (runtime store v2 P3) ──
#  평면 {CimsRuntimeDir}/<name> → 소유 모듈 네임스페이스로 분리해 모듈 간 충돌·
#  소유권 역전을 차단.
#   - 표준 배포(<PREFIX>/modules/oam/runtime): <PREFIX>/modules/<owner>/runtime/collections/<name>
#   - dev/비표준(runtime_root 가 modules/oam/runtime 패턴 아님): {runtime_root}/collections/<owner>/<name>
def _collections_base(config: dict, owner: str) -> str:
    rt = file_store.runtime_root(config)
    parent = _os.path.dirname(rt)
    gp = _os.path.dirname(parent)
    if (_os.path.basename(rt) == 'runtime' and _os.path.basename(parent) == 'oam'
            and _os.path.basename(gp) == 'modules'):
        return _os.path.join(gp, owner, 'runtime', 'collections')
    return _os.path.join(rt, 'collections', owner)


def collection_dir(config: dict, name: str, create: bool = False) -> str:
    """컬렉션 SoT 디렉터리 경로. owner 는 _COLLECTION_OWNER 로 결정(미등록 시 'csp').
    create=True 일 때만 mkdir (읽기는 부수효과 없음 — v2 P4 전제)."""
    owner = _COLLECTION_OWNER.get(name) or 'csp'
    path = _os.path.join(_collections_base(config, owner), name)
    if create:
        _os.makedirs(path, exist_ok=True)
    return path


def prune_module_collections(config: dict, owner: str) -> bool:
    """모듈 uninstall(마지막 deployment 제거) 시 그 모듈의 컬렉션 SoT 디렉터리 제거
    (runtime store v2 P4 — 라이프사이클 결합). owner 의 collections base 를 통째로 삭제.
    삭제했으면 True. 데이터 유실 방지: 호출 측이 '마지막 deployment 제거' 를 보장해야 함."""
    import shutil
    base = _collections_base(config, owner)
    # prod: .../modules/<owner>/runtime/collections   dev: {runtime}/collections/<owner>
    if _os.path.isdir(base):
        shutil.rmtree(base, ignore_errors=True)
        # 표준 배포에서 비게 된 modules/<owner>/runtime 도 정리(다른 내용 없으면).
        parent = _os.path.dirname(base)
        if _os.path.basename(base) == 'collections' and _os.path.basename(parent) == 'runtime':
            try:
                _os.rmdir(parent)
            except OSError:
                pass
        return True
    return False


def migrate_flat_collections(config: dict) -> int:
    """1회 이행 — 구 평면 {runtime_root}/<name> 의 컬렉션 데이터를 네임스페이스 경로로 이동.
    빈 잔재(데이터 없는 평면 도메인 디렉터리)는 제거. 이동/제거한 도메인 수 반환."""
    rt = file_store.runtime_root(config)
    moved = 0
    for name in list(_COLLECTION_OWNER.keys()):
        old = _os.path.join(rt, name)
        if not _os.path.isdir(old):
            continue
        new = collection_dir(config, name, create=False)
        if _os.path.abspath(old) == _os.path.abspath(new):
            continue
        records = [f for f in _os.listdir(old) if f.endswith('.json') and not f.startswith('.')]
        if records:
            _os.makedirs(new, exist_ok=True)
            for f in _os.listdir(old):
                src = _os.path.join(old, f); dst = _os.path.join(new, f)
                if not _os.path.exists(dst):
                    _os.replace(src, dst)
            moved += 1
        # 비거나 이동 완료된 구 평면 디렉터리 정리
        try:
            _os.rmdir(old)
        except OSError:
            pass
    return moved


def should_propagate(scope: Optional[str], mode: Optional[str],
                     override: Optional[bool] = None) -> bool:
    """"이 컬렉션은 그룹 멤버 간 동일해야 하는가" 판정 — drift_sweeper 전용.

    설정 저장의 자동 fan-out 은 폐지됨 (저장=단일 서버, 정합은
    POST /deployments/{id}/sync 의 명시적 그룹 동기화로만). 이 함수는 드리프트
    감시가 "동일해야 정상인" 컬렉션을 고를 때만 사용한다.

    Rules:
      override True/False        → 그대로 (호출자 의도 우선)
      scope = "service"/None     → True (그룹 공통 — 불일치=드리프트)
      scope = "system"
          mode = "active_standby"  → True  (VIP 모델 — 양 멤버 동일 기대)
          mode = "all_active"      → False (멤버별 svc IP 정상)
          mode = "standalone"      → False (단일 멤버 — 의미 없음)
          mode = None              → False (그룹 없음)
    """
    if override is not None:
        return bool(override)
    s = (scope or "service").lower()
    if s == "service":
        return True
    if s == "system":
        return mode == "active_standby"
    # 알 수 없는 scope → 보수적으로 fan-out
    return True
