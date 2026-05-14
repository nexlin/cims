"""
CIMS HA Groups REST API.

Routes (prefix-matched, mounted at /api/v1/ha-groups):
  /api/v1/ha-groups                              GET list / POST create
  /api/v1/ha-groups/{id}                         GET / PUT / DELETE
  /api/v1/ha-groups/{id}/members                 GET list / POST add
  /api/v1/ha-groups/{id}/members/{agent_id}      DELETE

ha_groups 정책 (sql/migrate_ha_groups.sql):
  - 1 agent = 1 group (uk_agent UNIQUE)
  - mode = 'active_standby' | 'all_active'
  - VRID 는 51-255 range 자동 할당, VIP 는 운영자 수동 입력

그룹 생성/수정/멤버 변경 시 update_ha job 자동 큐잉 (services.ha_render).
"""
from __future__ import annotations

from urllib.parse import urlparse, unquote
from pathlib import PurePath
import json

from httpsrv.handler import HandlerArgs, HandlerResult
from services import file_store


_HA_GROUPS_BASE = '/api/v1/ha-groups'
_HA_DOMAIN = 'ha_groups'

_VRID_MIN = 51
_VRID_MAX = 255


def _ha_dir(config):
    return file_store.domain_dir(config, _HA_DOMAIN)


def _ha_load(config, gid: int):
    return file_store.by_id(_ha_dir(config), gid)


def _ha_load_all(config) -> list:
    return file_store.load_all(_ha_dir(config))


def _path_parts(full_path: str, base: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _alloc_vrid(config) -> int:
    """51-255 range 에서 next available VRID 반환. 없으면 RuntimeError."""
    used = {g.get('vrid') for g in _ha_load_all(config) if g.get('vrid') is not None}
    for v in range(_VRID_MIN, _VRID_MAX + 1):
        if v not in used:
            return v
    raise RuntimeError(f"VRID pool exhausted ({_VRID_MIN}-{_VRID_MAX})")


def _pick_default_iface(vip_bindings: list, agent_id: int) -> str:
    """vip_bindings.memberIfaces 에서 이 agent 의 가장 흔한 iface 추출. 없으면 빈 문자열."""
    for b in (vip_bindings or []):
        iface = (b.get('memberIfaces') or {}).get(str(agent_id)) \
                or (b.get('memberIfaces') or {}).get(agent_id)
        if iface:
            return iface
    return ''


def _iface_ip(agent_row: dict, iface_name: str) -> str:
    """agent_row.interfaces[] 에서 iface_name 에 해당하는 IP 반환. 없으면 빈 문자열."""
    if not iface_name:
        return ''
    for it in (agent_row.get('interfaces') or []):
        if it.get('name') == iface_name and it.get('ip'):
            return it['ip']
    return ''


def _render_ha_for_agent(group: dict, members: list, agent_id: int,
                         agent_row: dict, peer_row: dict | None,
                         vip_bindings: list | None = None) -> dict:
    """그룹 + 멤버 → 특정 agent 의 ha.json 내용.

    vip_bindings 가 있으면 multi-VIP 한 vrrp_instance (services.<group_name>.vips[]).
    없으면 legacy 단일 vip path (group.vip).
    """
    is_master = next((m.get('role') == 'master' for m in members if m['agent_id'] == agent_id),
                     False)
    vip_bindings = vip_bindings or []
    default_iface = _pick_default_iface(vip_bindings, agent_id) or "eth0"

    services: dict = {}
    if vip_bindings:
        vips = []
        svc_iface = default_iface
        for b in vip_bindings:
            slot = (b.get('slot') or '').strip()
            ip   = (b.get('ip')   or '').strip()
            if not slot or not ip:
                continue
            mask = int(b.get('mask') or group.get('vip_mask') or 24)
            iface = (b.get('memberIfaces') or {}).get(str(agent_id)) \
                    or (b.get('memberIfaces') or {}).get(agent_id)
            if iface:
                svc_iface = iface
            vips.append({'slot': slot, 'ip': ip, 'mask': mask})
        if vips:
            services[group['name']] = {
                'enabled':  True,
                'vrid':     group['vrid'],
                'interface': svc_iface,
                'vips':     vips,
                'priority': 100 if is_master else 90,
            }
    elif group.get('vip') and group['vip'] not in ('', '0.0.0.0'):
        # legacy 단일 vip
        services[group['name']] = {
            'enabled':  True,
            'vrid':     group['vrid'],
            'interface': default_iface,
            'vip':      group['vip'],
            'priority': 100 if is_master else 90,
        }

    # local_ip / peer_ip 은 VRRP advertise 가 송신되는 interface 의 IP 여야 함.
    # interface=svc 인데 agent.ip_address=mgmt 망이면 split brain 발생.
    local_ip = _iface_ip(agent_row, default_iface) or agent_row.get('ip_address') or "127.0.0.1"
    peer_ip = ''
    if peer_row:
        peer_ip = _iface_ip(peer_row, default_iface) or peer_row.get('ip_address') or ''

    return {
        "node_name":     agent_row.get('name') or f"agent-{agent_id}",
        "interface":     default_iface,
        "local_ip":      local_ip,
        "peer_ip":       peer_ip,
        "initial_state": "MASTER" if is_master else "BACKUP",
        "vip_mask":      group['vip_mask'],
        "auth_pass":     group['auth_pass'],
        "ha_log_dir":    "/var/log/cims-ha",
        "cims_home":     "/opt/cims",
        "cims_user":     "cims",
        "services":      services,
    }


def _enqueue_update_ha_for_members(group_id: int, config: dict) -> int:
    """그룹 멤버들에게 update_ha job 큐잉. 큐잉된 job 수 반환."""
    group = _ha_load(config, group_id)
    if not group:
        return 0
    members = list(group.get('members') or [])
    if not members:
        return 0
    vip_bindings = group.get('vip_bindings') or []

    from handlers.agents import _agent_load, _job_create
    agents = {}
    for m in members:
        a = _agent_load(config, aid=m.get('agent_id'))
        if a:
            agents[m['agent_id']] = {'id': a.get('id'), 'name': a.get('name'),
                                     'ip_address': a.get('ip_address'),
                                     'interfaces': a.get('interfaces') or []}

    enqueued = 0
    for m in members:
        agent = agents.get(m['agent_id'])
        if not agent:
            continue
        peer = None
        for other in members:
            if other['agent_id'] != m['agent_id']:
                peer = agents.get(other['agent_id'])
                break
        ha_json = _render_ha_for_agent(group, members, m['agent_id'], agent, peer, vip_bindings)
        params = {
            "install_path": f"/opt/cims/{agent.get('name','agent')}",
            "ha_json": ha_json,
        }
        _job_create(config, m['agent_id'], 'update_ha', params)
        enqueued += 1
    return enqueued


async def handle_ha_groups(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """Dispatch /api/v1/ha-groups/* routes."""
    config = kwargs.get('config', {})
    parts = _path_parts(handler_args.full_path, _HA_GROUPS_BASE)
    group_id = parts[0] if len(parts) > 0 else None
    sub      = parts[1] if len(parts) > 1 else None
    member   = parts[2] if len(parts) > 2 else None
    method = handler_args.method.upper()

    try:
        if group_id is None:
            if method == 'GET':
                return await _list_groups(config)
            elif method == 'POST':
                return await _create_group(handler_args.body, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        try:
            gid = int(group_id)
        except (TypeError, ValueError):
            return HandlerResult(status=400, body={'error': 'invalid group id'})

        if sub is None:
            if method == 'GET':
                return await _get_group(gid, config)
            elif method == 'PUT':
                return await _update_group(gid, handler_args.body, config)
            elif method == 'DELETE':
                return await _delete_group(gid, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        if sub == 'members':
            if member is None:
                if method == 'GET':
                    return await _list_members(gid, config)
                elif method == 'POST':
                    return await _add_member(gid, handler_args.body, config)
                return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
            try:
                aid = int(member)
            except (TypeError, ValueError):
                return HandlerResult(status=400, body={'error': 'invalid agent id'})
            if method == 'DELETE':
                return await _remove_member(gid, aid, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        if sub == 'apply' and method == 'POST':
            return await _apply_group(gid, config)

        return HandlerResult(status=404, body={'error': 'Not Found'})
    except pymysql.IntegrityError as e:
        # uk_agent (1 agent = 1 group) / uk_vrid 위반 등
        return HandlerResult(status=409, body={'error': 'conflict', 'detail': str(e)})
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})


def _attach_member_names(members: list, config: dict) -> list:
    """members rows 에 agent_name 을 file_store 에서 채워준다."""
    from handlers.agents import _agent_load
    cache: dict = {}
    for m in members:
        aid = m.get('agent_id')
        if aid is None:
            continue
        if aid not in cache:
            cache[aid] = _agent_load(config, aid=aid) or {}
        m['agent_name'] = cache[aid].get('name')
    return members


def _serialize_group(g: dict, config: dict) -> dict:
    """file_store group dict → 응답용 (멤버 정렬 + agent_name enrich)."""
    out = dict(g)
    members = list(out.get('members') or [])
    members.sort(key=lambda m: -int(m.get('priority') or 0))
    out['members'] = _attach_member_names(members, config)
    out.setdefault('vip_bindings', [])
    return out


async def _list_groups(config):
    groups = _ha_load_all(config)
    groups.sort(key=lambda g: g.get('id', 0))
    return HandlerResult(status=200,
                         body={'groups': [_serialize_group(g, config) for g in groups]})


async def _get_group(gid: int, config):
    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    return HandlerResult(status=200, body=_serialize_group(g, config))


def _normalize_member(m: dict, idx: int) -> dict:
    aid = int(m.get('agent_id'))
    role = m.get('role') or ('master' if idx == 0 else 'backup')
    priority = int(m.get('priority', 100 if role == 'master' else 90))
    return {'agent_id': aid, 'role': role, 'priority': priority}


async def _create_group(body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    name = (body.get('name') or '').strip()
    mode = (body.get('mode') or '').strip()
    vip  = (body.get('vip')  or '').strip() or None
    auth_pass = (body.get('auth_pass') or '').strip()
    vip_mask = int(body.get('vip_mask', 24))
    note = body.get('note', '')
    members_in = body.get('members', [])

    if not name:
        return HandlerResult(status=400, body={'error': 'name required'})
    if mode not in ('active_standby', 'all_active'):
        return HandlerResult(status=400, body={'error': 'mode must be active_standby or all_active'})
    if not auth_pass or len(auth_pass) > 8:
        return HandlerResult(status=400, body={'error': 'auth_pass required (max 8 chars)'})
    if mode == 'active_standby' and len(members_in) not in (0, 2):
        return HandlerResult(status=400,
                             body={'error': 'active_standby requires exactly 2 members (or 0 for late add)'})

    vip_bindings = body.get('vip_bindings')
    if vip_bindings is not None and not isinstance(vip_bindings, list):
        vip_bindings = None

    vrid = _alloc_vrid(config)
    gid = file_store.next_id(_ha_dir(config))
    members = [_normalize_member(m, i) for i, m in enumerate(members_in)]
    group = {
        'id': gid,
        'name': name,
        'mode': mode,
        'vip': vip,
        'vrid': vrid,
        'vip_mask': vip_mask,
        'auth_pass': auth_pass,
        'note': note,
        'vip_bindings': vip_bindings or [],
        'members': members,
    }
    file_store.save(_ha_dir(config), gid, group)
    _enqueue_update_ha_for_members(gid, config)
    return HandlerResult(status=201, body={'id': gid, 'vrid': vrid})


async def _update_group(gid: int, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    if 'mode' in body:
        return HandlerResult(status=400, body={'error': 'mode 변경 불가 (그룹 재생성 필요)'})

    existing = _ha_load(config, gid)
    if not existing:
        return HandlerResult(status=404, body={'error': 'Group not found'})

    for k in ('name', 'vip', 'auth_pass', 'note'):
        if k in body:
            existing[k] = body[k]
    if 'vip_mask' in body:
        existing['vip_mask'] = int(body['vip_mask'])
    if 'vip_bindings' in body:
        v = body.get('vip_bindings')
        existing['vip_bindings'] = v if isinstance(v, list) else []
    if 'members' in body:
        existing['members'] = [_normalize_member(m, i) for i, m in enumerate(body['members'])]

    file_store.save(_ha_dir(config), gid, existing)
    _enqueue_update_ha_for_members(gid, config)
    return HandlerResult(status=200, body={'id': gid})


async def _delete_group(gid: int, config):
    if not file_store.delete(_ha_dir(config), gid):
        return HandlerResult(status=404, body={'error': 'Group not found'})
    return HandlerResult(status=200, body={'id': gid, 'deleted': True})


async def _list_members(gid: int, config):
    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    members = sorted(g.get('members') or [], key=lambda m: -int(m.get('priority') or 0))
    return HandlerResult(status=200, body={'members': _attach_member_names(members, config)})


async def _add_member(gid: int, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    aid = int(body.get('agent_id', 0))
    role = body.get('role', 'backup')
    priority = int(body.get('priority', 100 if role == 'master' else 90))
    if not aid:
        return HandlerResult(status=400, body={'error': 'agent_id required'})

    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    members = list(g.get('members') or [])
    # 동일 agent_id 가 이미 있으면 priority/role 갱신
    found = False
    for m in members:
        if m.get('agent_id') == aid:
            m['role'] = role; m['priority'] = priority
            found = True
            break
    if not found:
        members.append({'agent_id': aid, 'role': role, 'priority': priority})
    g['members'] = members
    file_store.save(_ha_dir(config), gid, g)
    _enqueue_update_ha_for_members(gid, config)
    return HandlerResult(status=201, body={'group_id': gid, 'agent_id': aid})


async def _apply_group(gid: int, config):
    """그룹의 모든 멤버에 update_ha job 큐잉 — VipPanel [적용] 진입점."""
    if not _ha_load(config, gid):
        return HandlerResult(status=404, body={'error': 'Group not found'})
    count = _enqueue_update_ha_for_members(gid, config)
    return HandlerResult(status=202, body={'group_id': gid, 'jobs_queued': count})


async def _remove_member(gid: int, aid: int, config):
    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    members = list(g.get('members') or [])
    new_members = [m for m in members if m.get('agent_id') != aid]
    if len(new_members) == len(members):
        return HandlerResult(status=404, body={'error': 'Member not found'})
    g['members'] = new_members
    file_store.save(_ha_dir(config), gid, g)
    _enqueue_update_ha_for_members(gid, config)
    return HandlerResult(status=200, body={'group_id': gid, 'agent_id': aid, 'removed': True})


CIMS_HA_GROUPS_HANDLER_LIST = (
    (_HA_GROUPS_BASE, handle_ha_groups, {}),
)
