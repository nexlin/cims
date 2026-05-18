"""
ha_group ↔ deployment ↔ agent 매핑 헬퍼.

흐름 A (모듈 통째 config) / 흐름 B (런타임 컬렉션) 양쪽이 같은 룰로 fan-out 하려면
"이 패키지(csp 등) 를 호스팅하는 ha_group 의 모든 멤버 agent_id" 가 필요하다.

여기서는 file_store 만 의존 — DB / HTTP 없음. 호출자는 thread offload 권장.
"""
from __future__ import annotations

from typing import Optional

from services import file_store


_HA_DOMAIN = 'ha_groups'
_DEPLOY_DOMAIN = 'deployments'
_PKG_DOMAIN = 'packages'


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
    """런타임에서 컬렉션→패키지 매핑 추가 (cmp 등 미래 확장)."""
    _COLLECTION_OWNER[collection] = package_name
