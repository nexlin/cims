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

import pymysql
import pymysql.cursors

from httpsrv.handler import HandlerArgs, HandlerResult


_HA_GROUPS_BASE = '/api/v1/ha-groups'

_VRID_MIN = 51
_VRID_MAX = 255


def _get_db(config: dict):
    db = config.get('CimsDatabase', {})
    return pymysql.connect(
        host=db.get('Host', '127.0.0.1'),
        port=int(db.get('Port', 3306)),
        user=db.get('User', 'root'),
        password=db.get('Password', ''),
        database=db.get('Db', 'cims'),
        charset='utf8mb4',
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=True,
    )


def _path_parts(full_path: str, base: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _alloc_vrid(cur) -> int:
    """51-255 range 에서 next available VRID 반환. 없으면 RuntimeError."""
    cur.execute("SELECT vrid FROM ha_groups ORDER BY vrid")
    used = {r['vrid'] for r in cur.fetchall()}
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

    return {
        "node_name":     agent_row.get('name') or f"agent-{agent_id}",
        "interface":     default_iface,
        "local_ip":      agent_row.get('ip_address') or "127.0.0.1",
        "peer_ip":       (peer_row.get('ip_address') if peer_row else "") or "",
        "initial_state": "MASTER" if is_master else "BACKUP",
        "vip_mask":      group['vip_mask'],
        "auth_pass":     group['auth_pass'],
        "ha_log_dir":    "/var/log/cims-ha",
        "cims_home":     "/opt/cims",
        "cims_user":     "cims",
        "services":      services,
    }


def _enqueue_update_ha_for_members(cur, group_id: int) -> int:
    """그룹 멤버들에게 update_ha job 큐잉. 큐잉된 job 수 반환."""
    cur.execute(
        "SELECT g.*, GROUP_CONCAT(m.agent_id) AS member_ids "
        "FROM ha_groups g LEFT JOIN ha_group_members m ON m.group_id=g.id "
        "WHERE g.id=%s GROUP BY g.id", (group_id,)
    )
    group = cur.fetchone()
    if not group or not group.get('member_ids'):
        return 0
    vip_bindings = _decode_vip_bindings(group.pop('vip_bindings_json', None)) \
                   if 'vip_bindings_json' in group else []

    cur.execute(
        "SELECT group_id, agent_id, priority, role FROM ha_group_members "
        "WHERE group_id=%s", (group_id,)
    )
    members = list(cur.fetchall())

    cur.execute("SELECT id, name, ip_address FROM cims_agent WHERE id IN ({})".format(
        ",".join(["%s"] * len(members))
    ), tuple(m['agent_id'] for m in members))
    agents = {r['id']: r for r in cur.fetchall()}

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
        cur.execute(
            "INSERT INTO agent_job (agent_id, job_type, params, status) "
            "VALUES (%s, 'update_ha', %s, 'queued')",
            (m['agent_id'], json.dumps(params, ensure_ascii=False))
        )
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


def _decode_vip_bindings(raw):
    if not raw: return []
    try:
        v = json.loads(raw)
        return v if isinstance(v, list) else []
    except (TypeError, ValueError):
        return []


async def _list_groups(config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, name, mode, vip, vrid, vip_mask, auth_pass, note, "
                "       vip_bindings_json, create_time, update_time FROM ha_groups ORDER BY id"
            )
            groups = cur.fetchall()
            for g in groups:
                if g.get('create_time'): g['create_time'] = g['create_time'].isoformat()
                if g.get('update_time'): g['update_time'] = g['update_time'].isoformat()
                g['vip_bindings'] = _decode_vip_bindings(g.pop('vip_bindings_json', None))
                cur.execute(
                    "SELECT m.agent_id, m.priority, m.role, a.name AS agent_name "
                    "FROM ha_group_members m JOIN cims_agent a ON a.id=m.agent_id "
                    "WHERE m.group_id=%s ORDER BY m.priority DESC",
                    (g['id'],)
                )
                g['members'] = cur.fetchall()
    return HandlerResult(status=200, body={'groups': groups})


async def _get_group(gid: int, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, name, mode, vip, vrid, vip_mask, auth_pass, note, "
                "       vip_bindings_json, create_time, update_time FROM ha_groups WHERE id=%s", (gid,)
            )
            g = cur.fetchone()
            if not g:
                return HandlerResult(status=404, body={'error': 'Group not found'})
            if g.get('create_time'): g['create_time'] = g['create_time'].isoformat()
            if g.get('update_time'): g['update_time'] = g['update_time'].isoformat()
            g['vip_bindings'] = _decode_vip_bindings(g.pop('vip_bindings_json', None))
            cur.execute(
                "SELECT m.agent_id, m.priority, m.role, a.name AS agent_name "
                "FROM ha_group_members m JOIN cims_agent a ON a.id=m.agent_id "
                "WHERE m.group_id=%s ORDER BY m.priority DESC", (gid,)
            )
            g['members'] = cur.fetchall()
    return HandlerResult(status=200, body=g)


async def _create_group(body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    name = (body.get('name') or '').strip()
    mode = (body.get('mode') or '').strip()
    vip  = (body.get('vip')  or '').strip()
    auth_pass = (body.get('auth_pass') or '').strip()
    vip_mask = int(body.get('vip_mask', 24))
    note = body.get('note', '')
    members = body.get('members', [])

    if not name:
        return HandlerResult(status=400, body={'error': 'name required'})
    if mode not in ('active_standby', 'all_active'):
        return HandlerResult(status=400, body={'error': 'mode must be active_standby or all_active'})
    if not vip:
        return HandlerResult(status=400, body={'error': 'vip required'})
    if not auth_pass or len(auth_pass) > 8:
        return HandlerResult(status=400, body={'error': 'auth_pass required (max 8 chars)'})
    if mode == 'active_standby' and len(members) not in (0, 2):
        return HandlerResult(status=400, body={'error': 'active_standby requires exactly 2 members (or 0 for late add)'})

    vip_bindings = body.get('vip_bindings')
    vip_bindings_json = json.dumps(vip_bindings, ensure_ascii=False) if isinstance(vip_bindings, list) else None
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            vrid = _alloc_vrid(cur)
            cur.execute(
                "INSERT INTO ha_groups (name, mode, vip, vrid, vip_mask, auth_pass, note, "
                "                       vip_bindings_json) "
                "VALUES (%s, %s, %s, %s, %s, %s, %s, %s)",
                (name, mode, vip, vrid, vip_mask, auth_pass, note, vip_bindings_json)
            )
            gid = cur.lastrowid
            # 멤버 추가
            for idx, m in enumerate(members):
                aid = int(m.get('agent_id'))
                role = m.get('role') or ('master' if idx == 0 else 'backup')
                priority = int(m.get('priority', 100 if role == 'master' else 90))
                cur.execute(
                    "INSERT INTO ha_group_members (group_id, agent_id, priority, role) "
                    "VALUES (%s, %s, %s, %s)",
                    (gid, aid, priority, role)
                )
            # update_ha job 큐잉
            _enqueue_update_ha_for_members(cur, gid)
    return HandlerResult(status=201, body={'id': gid, 'vrid': vrid})


async def _update_group(gid: int, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            fields, vals = [], []
            for k in ('name', 'vip', 'auth_pass', 'note'):
                if k in body:
                    fields.append(f"{k}=%s"); vals.append(body[k])
            if 'vip_mask' in body:
                fields.append("vip_mask=%s"); vals.append(int(body['vip_mask']))
            if 'vip_bindings' in body:
                v = body.get('vip_bindings')
                fields.append("vip_bindings_json=%s")
                vals.append(json.dumps(v, ensure_ascii=False) if v is not None else None)
            if 'mode' in body:
                return HandlerResult(status=400, body={'error': 'mode 변경 불가 (그룹 재생성 필요)'})
            if fields:
                vals.append(gid)
                cur.execute(
                    "UPDATE ha_groups SET " + ", ".join(fields) + " WHERE id=%s", vals
                )
                if cur.rowcount == 0:
                    return HandlerResult(status=404, body={'error': 'Group not found'})
            if 'members' in body:
                cur.execute("DELETE FROM ha_group_members WHERE group_id=%s", (gid,))
                for idx, m in enumerate(body['members']):
                    aid = int(m.get('agent_id'))
                    role = m.get('role') or ('master' if idx == 0 else 'backup')
                    priority = int(m.get('priority', 100 if role == 'master' else 90))
                    cur.execute(
                        "INSERT INTO ha_group_members (group_id, agent_id, priority, role) "
                        "VALUES (%s, %s, %s, %s)",
                        (gid, aid, priority, role)
                    )
            _enqueue_update_ha_for_members(cur, gid)
    return HandlerResult(status=200, body={'id': gid})


async def _delete_group(gid: int, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("DELETE FROM ha_groups WHERE id=%s", (gid,))
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'Group not found'})
    return HandlerResult(status=200, body={'id': gid, 'deleted': True})


async def _list_members(gid: int, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT m.agent_id, m.priority, m.role, a.name AS agent_name "
                "FROM ha_group_members m JOIN cims_agent a ON a.id=m.agent_id "
                "WHERE m.group_id=%s ORDER BY m.priority DESC", (gid,)
            )
            members = cur.fetchall()
    return HandlerResult(status=200, body={'members': members})


async def _add_member(gid: int, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    aid = int(body.get('agent_id', 0))
    role = body.get('role', 'backup')
    priority = int(body.get('priority', 100 if role == 'master' else 90))
    if not aid:
        return HandlerResult(status=400, body={'error': 'agent_id required'})
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO ha_group_members (group_id, agent_id, priority, role) "
                "VALUES (%s, %s, %s, %s)",
                (gid, aid, priority, role)
            )
            _enqueue_update_ha_for_members(cur, gid)
    return HandlerResult(status=201, body={'group_id': gid, 'agent_id': aid})


async def _apply_group(gid: int, config):
    """그룹의 모든 멤버에 update_ha job 큐잉 — VipPanel [적용] 진입점.
    데이터 변경 없이 강제 재 render + keepalived reload."""
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT id FROM ha_groups WHERE id=%s", (gid,))
            if not cur.fetchone():
                return HandlerResult(status=404, body={'error': 'Group not found'})
            count = _enqueue_update_ha_for_members(cur, gid)
    return HandlerResult(status=202, body={'group_id': gid, 'jobs_queued': count})


async def _remove_member(gid: int, aid: int, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "DELETE FROM ha_group_members WHERE group_id=%s AND agent_id=%s",
                (gid, aid)
            )
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'Member not found'})
            _enqueue_update_ha_for_members(cur, gid)
    return HandlerResult(status=200, body={'group_id': gid, 'agent_id': aid, 'removed': True})


CIMS_HA_GROUPS_HANDLER_LIST = (
    (_HA_GROUPS_BASE, handle_ha_groups, {}),
)
