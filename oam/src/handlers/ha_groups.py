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
import asyncio
import json

from httpsrv.handler import HandlerArgs, HandlerResult
from services import file_store


_HA_GROUPS_BASE = '/api/v1/ha-groups'
_HA_DOMAIN = 'ha_groups'

_VRID_MIN = 51
_VRID_MAX = 255


# failover_options 의 default — 현재 hardcoded 동작과 동일.
# 옛 record (failover_options 미존재) 도 _normalize_failover_options 가 이 default 로 채워
# 동작 변경 없음 (호환성 보장).
_FAILOVER_DEFAULTS = {
    'advert_int':      1.0,
    'health': {
        'interval':    2,
        'fall':        2,
        'rise':        2,
        'timeout':     3,
    },
    'track_interface': False,
    'tracked_modules': [],
    'preempt':         'nopreempt',
    'preempt_delay':   0,
}


def _normalize_failover_options(raw) -> dict:
    """입력 dict → 검증된 failover_options. 잘못된 값은 default 로 대체.

    AS 만 의미 있으나, 다른 mode 도 같은 dict 형태로 저장 — UI 가 mode 로 분기.
    range:
      advert_int: 0.5~5 (float, sec)
      health.interval / fall / rise / timeout: 1~60 (int)
      preempt: 'preempt' | 'nopreempt'
      preempt_delay: 0~300 (int, sec)
    """
    if not isinstance(raw, dict):
        raw = {}
    out = {}

    try:
        ai = float(raw.get('advert_int', _FAILOVER_DEFAULTS['advert_int']))
        if 0.5 <= ai <= 5:
            out['advert_int'] = ai
        else:
            out['advert_int'] = _FAILOVER_DEFAULTS['advert_int']
    except (TypeError, ValueError):
        out['advert_int'] = _FAILOVER_DEFAULTS['advert_int']

    health_in = raw.get('health') if isinstance(raw.get('health'), dict) else {}
    health = {}
    for k in ('interval', 'fall', 'rise', 'timeout'):
        try:
            v = int(health_in.get(k, _FAILOVER_DEFAULTS['health'][k]))
            if 1 <= v <= 60:
                health[k] = v
            else:
                health[k] = _FAILOVER_DEFAULTS['health'][k]
        except (TypeError, ValueError):
            health[k] = _FAILOVER_DEFAULTS['health'][k]
    out['health'] = health

    out['track_interface'] = bool(raw.get('track_interface', False))

    tm = raw.get('tracked_modules') or []
    if isinstance(tm, list):
        out['tracked_modules'] = [str(x).strip().lower() for x in tm if str(x).strip()]
    else:
        out['tracked_modules'] = []

    pe = raw.get('preempt') or _FAILOVER_DEFAULTS['preempt']
    out['preempt'] = pe if pe in ('preempt', 'nopreempt') else _FAILOVER_DEFAULTS['preempt']

    try:
        pd = int(raw.get('preempt_delay', 0))
        out['preempt_delay'] = pd if 0 <= pd <= 300 else 0
    except (TypeError, ValueError):
        out['preempt_delay'] = 0

    return out


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


# cims-health 가 ha.json 의 services.<group>.port/proto 를 lookup. 누락 시
# default 가 csc/csp/psp 만 정의되어 있어 그룹명(예: "Control-Server") 으로는
# 찾지 못해 health 가 fail → keepalived 가 BACKUP 강제 → VIP 미할당.
# 해결: ha.json render 시 그룹 멤버 deployment 들의 daemon module 을 보고
# 대표 module 의 default port/proto 를 services.<group> 에 자동 채워준다.
_MODULE_HEALTH_DEFAULTS = {
    'csp':   (5060, 'udp'),
    'isp':   (5060, 'udp'),
    'psp':   (5060, 'udp'),
    'csc':   (4420, 'tcp'),
    'cmp':   (9000, 'udp'),
    'imp':   (9000, 'udp'),
    'pmp':   (9000, 'udp'),
}
# 동일 그룹에 여러 daemon module 이 deployed 되어 있을 때의 우선순위.
# Control: csp 가 핵심 (SIP signaling) — psp/isp/csc 는 부수.
# Media: cmp 가 핵심 (RTP relay).
_HEALTH_MODULE_PRIORITY = ['csp', 'cmp', 'csc', 'psp', 'isp', 'pmp', 'imp']


def _infer_health_port_proto(agent_id: int, config: dict) -> tuple:
    """agent 의 daemon deployment 들 중 가장 적합한 module 로 (port, proto) 추정.

    찾지 못하면 (None, None) 반환 — 이 경우 services entry 에 port/proto 미기재
    (cims-health 가 csp default 5060/udp 로 fallback).
    """
    try:
        from handlers.agents import _deploy_load_all
        deps = [d for d in _deploy_load_all(config)
                if d.get('agent_id') == agent_id]
    except Exception:
        return (None, None)
    # deployment file 에는 package_name 이 없고 process_name 만 있는 케이스가 있음.
    # process_name 우선 (CSP/CMP/CSC 등 대문자 → lowercase). cspsim 등 non-daemon 제외.
    daemon_modules = set()
    for d in deps:
        mod = (d.get('process_name') or '').lower().strip()
        if mod in _MODULE_HEALTH_DEFAULTS:
            daemon_modules.add(mod)
    for mod in _HEALTH_MODULE_PRIORITY:
        if mod in daemon_modules:
            return _MODULE_HEALTH_DEFAULTS[mod]
    return (None, None)


def _compute_master_aid(members: list) -> int | None:
    """VRRP 본래 모델 — priority 가 단일 결정자.
    그룹 내 priority 최대값 멤버가 Master. 동률이면 agent_id 작은 쪽 (안정적 tie-break)."""
    if not members:
        return None
    return min(
        (m for m in members if m.get('agent_id') is not None),
        key=lambda m: (-int(m.get('priority') or 0), int(m.get('agent_id'))),
        default=None,
    ).get('agent_id') if members else None


def _render_ha_for_agent(group: dict, members: list, agent_id: int,
                         agent_row: dict, peer_row: dict | None,
                         vip_bindings: list | None = None,
                         config: dict | None = None) -> dict:
    """그룹 + 멤버 → 특정 agent 의 ha.json 내용.

    vip_bindings 가 있으면 multi-VIP 한 vrrp_instance (services.<group_name>.vips[]).
    없으면 legacy 단일 vip path (group.vip).

    priority 는 멤버 record 값 그대로 ha.json 에 박힘. initial_state 는 priority
    최대 멤버가 MASTER, 나머지 BACKUP (VRRP 본래 모델).
    """
    master_aid = _compute_master_aid(members)
    is_master = (master_aid == agent_id)
    # 이 agent 의 priority — 멤버 record 의 값 그대로. 누락 시 100/90 default (호환성).
    my_priority = next(
        (int(m.get('priority') or 0) for m in members if m.get('agent_id') == agent_id),
        100 if is_master else 90,
    )
    vip_bindings = vip_bindings or []
    default_iface = _pick_default_iface(vip_bindings, agent_id) or "eth0"

    # cims-health 가 lookup 하는 port/proto — agent 의 deployment 로 추정.
    h_port, h_proto = _infer_health_port_proto(agent_id, config) if config else (None, None)

    failover_options = _normalize_failover_options(group.get('failover_options'))

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
            entry = {
                'enabled':  True,
                'vrid':     group['vrid'],
                'interface': svc_iface,
                'vips':     vips,
                'priority': my_priority,
                'failover_options': failover_options,
            }
            if h_port:  entry['port']  = h_port
            if h_proto: entry['proto'] = h_proto
            services[group['name']] = entry
    elif group.get('vip') and group['vip'] not in ('', '0.0.0.0'):
        # legacy 단일 vip
        entry = {
            'enabled':  True,
            'vrid':     group['vrid'],
            'interface': default_iface,
            'vip':      group['vip'],
            'priority': my_priority,
            'failover_options': failover_options,
        }
        if h_port:  entry['port']  = h_port
        if h_proto: entry['proto'] = h_proto
        services[group['name']] = entry

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
        ha_json = _render_ha_for_agent(group, members, m['agent_id'], agent, peer, vip_bindings, config)
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

        if sub == 'collections':
            if not member:
                return HandlerResult(status=400, body={'error': 'collection name required'})
            if method == 'GET':
                return await _get_group_collection(gid, member, handler_args, config)
            if method == 'PUT':
                return await _put_group_collection(gid, member, handler_args, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

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


def _attach_derived_role(members: list) -> list:
    """role 은 derived — priority 최대값 멤버가 'master', 나머지 'backup'.
    동률 시 agent_id 작은 쪽 (안정적 tie-break, _compute_master_aid 와 동일).
    옛 record 의 저장된 role 필드는 무시 (응답에서 redundant).
    """
    master_aid = _compute_master_aid(members)
    for m in members:
        m['role'] = 'master' if m.get('agent_id') == master_aid else 'backup'
    return members


def _serialize_group(g: dict, config: dict) -> dict:
    """file_store group dict → 응답용 (멤버 정렬 + agent_name enrich + role derive)."""
    out = dict(g)
    members = list(out.get('members') or [])
    # priority 우선 정렬, 동률 시 agent_id 오름 (UI 일관 표시)
    members.sort(key=lambda m: (-int(m.get('priority') or 0), int(m.get('agent_id') or 0)))
    members = _attach_derived_role(members)
    out['members'] = _attach_member_names(members, config)
    out.setdefault('vip_bindings', [])
    # 옛 record (failover_options 미존재) 도 UI 가 매번 채울 필요 없도록 default 응답에 포함.
    out['failover_options'] = _normalize_failover_options(out.get('failover_options'))
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
    """role 은 derived (priority 의 결과). 입력은 role 또는 priority 중 하나로 의도 표현.
    저장은 priority 만 — UI 가 'Master' 선택 → role='master' → priority=100, 나머지 90.

    호환성: 옛 client 가 priority 직접 보내면 그대로 사용. 없으면 role 보고 100/90.
    """
    aid = int(m.get('agent_id'))
    if 'priority' in m and m.get('priority') is not None:
        priority = int(m['priority'])
    else:
        # role 또는 idx 로 default — UI 는 보통 명시 priority 보냄.
        role = m.get('role') or ('master' if idx == 0 else 'backup')
        priority = 100 if role == 'master' else 90
    return {'agent_id': aid, 'priority': priority}


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
    # auth_pass — VRRP 인증. active_standby (단일 VIP master/backup) 에서만 필수.
    # all_active 는 VIP 없는 multi-active 시나리오 가정 → 빈 값 허용 (vip_bindings 추가 시점에 갱신).
    if mode == 'active_standby':
        if not auth_pass or len(auth_pass) > 8:
            return HandlerResult(status=400, body={'error': 'auth_pass required for active_standby (max 8 chars)'})
    else:
        if len(auth_pass) > 8:
            return HandlerResult(status=400, body={'error': 'auth_pass max 8 chars'})
    if mode == 'active_standby' and len(members_in) not in (0, 2):
        return HandlerResult(status=400,
                             body={'error': 'active_standby requires exactly 2 members (or 0 for late add)'})

    vip_bindings = body.get('vip_bindings')
    if vip_bindings is not None and not isinstance(vip_bindings, list):
        vip_bindings = None

    vrid = _alloc_vrid(config)
    gid = file_store.next_id(_ha_dir(config))
    members = [_normalize_member(m, i) for i, m in enumerate(members_in)]
    failover_options = _normalize_failover_options(body.get('failover_options'))
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
        'failover_options': failover_options,
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

    # mode 변경 차단 — 시스템 유형은 생성 후 변경 불가. 변경 원하면 삭제 후 재생성.
    if 'mode' in body and body['mode'] != existing.get('mode'):
        return HandlerResult(status=400, body={
            'error': 'mode_change_not_allowed',
            'hint': '시스템 유형 (mode) 은 생성 후 변경 불가. 삭제 후 재생성으로 변경하세요.',
        })
    for k in ('name', 'vip', 'auth_pass', 'note'):
        if k in body:
            existing[k] = body[k]
    if 'vip_mask' in body:
        existing['vip_mask'] = int(body['vip_mask'])
    # auth_pass — active_standby 만 1~8자 required, 그 외 mode 는 (빈값 포함) 8자 이하 OK.
    mode_eff = existing.get('mode')
    auth_eff = existing.get('auth_pass') or ''
    if mode_eff == 'active_standby':
        if not auth_eff or len(auth_eff) > 8:
            return HandlerResult(status=400, body={'error': 'auth_pass required for active_standby (max 8 chars)'})
    else:
        if len(auth_eff) > 8:
            return HandlerResult(status=400, body={'error': 'auth_pass max 8 chars'})
    if 'vip_bindings' in body:
        v = body.get('vip_bindings')
        existing['vip_bindings'] = v if isinstance(v, list) else []
    if 'failover_options' in body:
        existing['failover_options'] = _normalize_failover_options(body.get('failover_options'))
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
    if not aid:
        return HandlerResult(status=400, body={'error': 'agent_id required'})
    # role 또는 priority 둘 중 하나로 의도 표현 — _normalize_member 가 priority 로 통일.
    norm = _normalize_member({'agent_id': aid,
                              'role': body.get('role'),
                              'priority': body.get('priority')}, 0)

    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    members = list(g.get('members') or [])
    # 동일 agent_id 가 이미 있으면 priority 갱신
    found = False
    for m in members:
        if m.get('agent_id') == aid:
            m['priority'] = norm['priority']
            m.pop('role', None)  # 옛 'role' 잔재 제거 (derived 가 SoT)
            found = True
            break
    if not found:
        members.append(norm)
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


def _find_group_member_deployments(gid: int, name: str, package_id, config) -> tuple:
    """그룹 멤버들의 deployment 중 (선택적 package_id 필터 + collection name 정의됨) 매칭.

    반환: (group, [matched_deployment, ...], schema). 매칭 0개면 schema=None.
    schema 는 첫 매칭 deployment 의 template 에서 추출 (멤버간 일관 가정).
    """
    from handlers.agents import _deploy_load_all, _fetch_deployment_for_proxy, _collection_schema
    g = _ha_load(config, gid)
    if not g:
        return None, [], None
    member_ids = {int(m.get('agent_id')) for m in (g.get('members') or [])
                  if m.get('agent_id') is not None}
    all_deps = _deploy_load_all(config)
    matched = []
    schema = None
    for d in all_deps:
        if int(d.get('agent_id', 0)) not in member_ids:
            continue
        if package_id is not None and int(d.get('package_id', 0)) != int(package_id):
            continue
        # template 에 해당 collection 정의 있는지 확인
        dep = _fetch_deployment_for_proxy(int(d['id']), config)
        if not dep:
            continue
        s, _ = _collection_schema(dep.get('config_template_json'), name)
        if s is None:
            continue
        if schema is None:
            schema = s
        matched.append(dep)
    return g, matched, schema


def _parse_package_id(handler_args) -> "int | None":
    qp = handler_args.query_params or {}
    raw = qp.get('package_id')
    if isinstance(raw, list):
        raw = raw[0] if raw else None
    if raw is None or raw == '':
        return None
    try:
        return int(raw)
    except (TypeError, ValueError):
        return None


async def _get_group_collection(gid: int, name: str, handler_args, config):
    """그룹 멤버의 첫 매칭 deployment 에서 collection records fetch."""
    from handlers.agents import _agent_proxy_call
    package_id = _parse_package_id(handler_args)
    g, matched, schema = await asyncio.to_thread(
        _find_group_member_deployments, gid, name, package_id, config)
    if g is None:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if not matched:
        return HandlerResult(status=404,
            body={'error': 'no_matching_deployment',
                  'hint': '그룹 멤버에 collection 정의된 패키지 미배포'})

    dep = matched[0]
    status, resp = await asyncio.to_thread(
        _agent_proxy_call, 'GET', dep,
        '/collection', {'install_path': dep['install_path'], 'name': name},
        None, 15, config)
    if status == 200:
        return HandlerResult(status=200,
            body={'records': resp.get('records') or [], 'schema': schema,
                  'source_deployment_id': dep['id'], 'member_count': len(matched)})
    return HandlerResult(status=status or 502,
        body={'error': 'agent_proxy_failed', 'detail': resp,
              'source_deployment_id': dep['id']})


async def _put_group_collection(gid: int, name: str, handler_args, config):
    """그룹 멤버 deployment 전체에 fan-out PUT. per-member 결과 array 반환."""
    from handlers.agents import _agent_proxy_call, _validate_record, _parse_body
    import uuid as _uuid

    package_id = _parse_package_id(handler_args)
    g, matched, schema = await asyncio.to_thread(
        _find_group_member_deployments, gid, name, package_id, config)
    if g is None:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if not matched:
        return HandlerResult(status=404,
            body={'error': 'no_matching_deployment'})

    body = _parse_body(handler_args)
    records = body.get('records')
    if not isinstance(records, list):
        return HandlerResult(status=400, body={'error': 'records array required'})

    # validation + auto id (deployment PUT 와 동일 로직)
    id_field = schema.get('id_field') or 'id'
    id_type  = schema.get('id_type') or 'uuid'
    all_errors = []
    for i, r in enumerate(records):
        if not isinstance(r, dict):
            all_errors.append({'index': i, 'errors': ['not_object']})
            continue
        if id_type == 'uuid' and not r.get(id_field):
            r[id_field] = _uuid.uuid4().hex[:16]
        errs = _validate_record(schema, r)
        if errs:
            all_errors.append({'index': i, 'errors': errs})
    if all_errors:
        return HandlerResult(status=400,
            body={'error': 'validation_failed', 'details': all_errors})

    do_signal = body.get('signal', True)
    results = []
    for dep in matched:
        status, resp = await asyncio.to_thread(
            _agent_proxy_call, 'PUT', dep,
            '/collection', {'install_path': dep['install_path'], 'name': name},
            {'records': records, 'signal': do_signal}, 15, config)
        if status == 200:
            results.append({'deployment_id': dep['id'],
                            'agent_id': dep.get('agent_id'),
                            'count': resp.get('count'),
                            'signaled': resp.get('signaled') or []})
        else:
            results.append({'deployment_id': dep['id'],
                            'agent_id': dep.get('agent_id'),
                            'error': resp, 'status': status})

    overall_ok = all('error' not in r for r in results)
    return HandlerResult(status=200 if overall_ok else 207,
        body={'ok': overall_ok, 'members': results})


CIMS_HA_GROUPS_HANDLER_LIST = (
    (_HA_GROUPS_BASE, handle_ha_groups, {}),
)
