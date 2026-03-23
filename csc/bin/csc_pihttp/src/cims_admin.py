"""
CIMS Admin REST API
Subscriber and PTT group CRUD operations backed by MariaDB.

Routes (prefix-matched):
  /api/v1/users                           GET list / POST create
  /api/v1/users/{id}                      GET / PUT / DELETE
  /api/v1/ptt/groups                      GET list / POST create
  /api/v1/ptt/groups/{id}                 GET / PUT / DELETE
  /api/v1/ptt/groups/{id}/members         GET list / POST add
  /api/v1/ptt/groups/{id}/members/{uid}   DELETE
"""

from urllib.parse import urlparse
from pathlib import PurePath

import pymysql
import pymysql.cursors

from util.pi_http.http_handler import HandlerArgs, HandlerResult

# ──────────────────────────────────────────────────────────────
#  DB helper
# ──────────────────────────────────────────────────────────────

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
    """Return the path segments that come after *base*.

    e.g. base='/api/v1/users', full_path='/api/v1/users/1001' → ('1001',)
    """
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return rel.parts
    except ValueError:
        return ()


# ──────────────────────────────────────────────────────────────
#  Users handler
# ──────────────────────────────────────────────────────────────

_USERS_BASE = '/api/v1/users'


async def handle_users(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    parts = _path_parts(handler_args.full_path, _USERS_BASE)
    user_id = parts[0] if parts else None
    method = handler_args.method.upper()

    try:
        if user_id is None:
            if method == 'GET':
                return await _list_users(config)
            elif method == 'POST':
                return await _create_user(handler_args.body, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
        else:
            if method == 'GET':
                return await _get_user(user_id, config)
            elif method == 'PUT':
                return await _update_user(user_id, handler_args.body, config)
            elif method == 'DELETE':
                return await _delete_user(user_id, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})


async def _list_users(config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, auth_id, org_id, dnd, forward_id, "
                "create_time, update_time, register_time, logout_time "
                "FROM cims_users ORDER BY id"
            )
            rows = cur.fetchall()
            # convert datetime objects to ISO strings
            for row in rows:
                for k in ('create_time', 'update_time', 'register_time', 'logout_time'):
                    if row[k] is not None:
                        row[k] = row[k].isoformat()
                row['dnd'] = bool(row['dnd'])
            # attach reject lists
            for row in rows:
                cur.execute(
                    "SELECT reject_id FROM cims_user_rejects WHERE user_id=%s",
                    (row['id'],)
                )
                row['reject_id'] = [r['reject_id'] for r in cur.fetchall()]
    return HandlerResult(status=200, body={'users': rows})


async def _get_user(user_id: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, auth_id, org_id, dnd, forward_id, "
                "create_time, update_time, register_time, logout_time "
                "FROM cims_users WHERE id=%s",
                (user_id,)
            )
            row = cur.fetchone()
            if row is None:
                return HandlerResult(status=404, body={'error': 'User not found'})
            for k in ('create_time', 'update_time', 'register_time', 'logout_time'):
                if row[k] is not None:
                    row[k] = row[k].isoformat()
            row['dnd'] = bool(row['dnd'])
            cur.execute(
                "SELECT reject_id FROM cims_user_rejects WHERE user_id=%s",
                (user_id,)
            )
            row['reject_id'] = [r['reject_id'] for r in cur.fetchall()]
    return HandlerResult(status=200, body=row)


async def _create_user(body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    user_id = body.get('id', '').strip()
    if not user_id:
        return HandlerResult(status=400, body={'error': 'id is required'})

    auth_id    = body.get('auth_id', user_id)
    passwd     = body.get('passwd', '')
    org_id     = body.get('org_id', '')
    dnd        = 1 if body.get('dnd', False) else 0
    forward_id = body.get('forward_id', '')
    reject_ids = body.get('reject_id', [])

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO cims_users "
                "(id, auth_id, passwd, org_id, dnd, forward_id, create_time, update_time) "
                "VALUES (%s, %s, %s, %s, %s, %s, NOW(), NOW())",
                (user_id, auth_id, passwd, org_id, dnd, forward_id)
            )
            if reject_ids:
                cur.execute("DELETE FROM cims_user_rejects WHERE user_id=%s", (user_id,))
                for rid in reject_ids:
                    cur.execute(
                        "INSERT IGNORE INTO cims_user_rejects (user_id, reject_id) VALUES (%s, %s)",
                        (user_id, rid)
                    )
    return HandlerResult(status=201, body={'id': user_id})


async def _update_user(user_id: str, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})

    fields = []
    values = []
    for col in ('auth_id', 'passwd', 'org_id', 'forward_id'):
        if col in body:
            fields.append(f'{col}=%s')
            values.append(body[col])
    if 'dnd' in body:
        fields.append('dnd=%s')
        values.append(1 if body['dnd'] else 0)
    if not fields:
        return HandlerResult(status=400, body={'error': 'No updatable fields provided'})

    fields.append('update_time=NOW()')
    values.append(user_id)

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                f"UPDATE cims_users SET {', '.join(fields)} WHERE id=%s",
                values
            )
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'User not found'})
            if 'reject_id' in body:
                cur.execute("DELETE FROM cims_user_rejects WHERE user_id=%s", (user_id,))
                for rid in body['reject_id']:
                    cur.execute(
                        "INSERT IGNORE INTO cims_user_rejects (user_id, reject_id) VALUES (%s, %s)",
                        (user_id, rid)
                    )
    return HandlerResult(status=200, body={'id': user_id})


async def _delete_user(user_id: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("DELETE FROM cims_user_rejects WHERE user_id=%s", (user_id,))
            cur.execute("DELETE FROM cims_users WHERE id=%s", (user_id,))
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'User not found'})
    return HandlerResult(status=200, body={'id': user_id})


# ──────────────────────────────────────────────────────────────
#  PTT Groups handler
# ──────────────────────────────────────────────────────────────

_GROUPS_BASE = '/api/v1/ptt/groups'


async def handle_ptt_groups(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """Handles all /api/v1/ptt/groups/* routes.

    Path structure:
      /api/v1/ptt/groups                   → group_id=None
      /api/v1/ptt/groups/{id}              → group_id set, sub=None
      /api/v1/ptt/groups/{id}/members      → sub='members', member_id=None
      /api/v1/ptt/groups/{id}/members/{uid}→ sub='members', member_id set
    """
    config = kwargs.get('config', {})
    parts = _path_parts(handler_args.full_path, _GROUPS_BASE)
    group_id  = parts[0] if len(parts) > 0 else None
    sub       = parts[1] if len(parts) > 1 else None   # 'members'
    member_id = parts[2] if len(parts) > 2 else None
    method = handler_args.method.upper()

    try:
        if group_id is None:
            if method == 'GET':
                return await _list_groups(config)
            elif method == 'POST':
                return await _create_group(handler_args.body, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        if sub is None:
            if method == 'GET':
                return await _get_group(group_id, config)
            elif method == 'PUT':
                return await _update_group(group_id, handler_args.body, config)
            elif method == 'DELETE':
                return await _delete_group(group_id, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        if sub == 'members':
            if member_id is None:
                if method == 'GET':
                    return await _list_members(group_id, config)
                elif method == 'POST':
                    return await _add_member(group_id, handler_args.body, config)
                return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
            else:
                if method == 'DELETE':
                    return await _remove_member(group_id, member_id, config)
                return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        return HandlerResult(status=404, body={'error': 'Not Found'})
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})


async def _list_groups(config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT id, name FROM cims_ptt_groups ORDER BY id")
            groups = cur.fetchall()
            for g in groups:
                cur.execute(
                    "SELECT user_id, priority FROM cims_ptt_group_members "
                    "WHERE group_id=%s ORDER BY priority",
                    (g['id'],)
                )
                g['members'] = cur.fetchall()
    return HandlerResult(status=200, body={'groups': groups})


async def _get_group(group_id: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, name FROM cims_ptt_groups WHERE id=%s",
                (group_id,)
            )
            group = cur.fetchone()
            if group is None:
                return HandlerResult(status=404, body={'error': 'Group not found'})
            cur.execute(
                "SELECT user_id, priority FROM cims_ptt_group_members "
                "WHERE group_id=%s ORDER BY priority",
                (group_id,)
            )
            group['members'] = cur.fetchall()
    return HandlerResult(status=200, body=group)


async def _create_group(body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    group_id = body.get('id', '').strip()
    if not group_id:
        return HandlerResult(status=400, body={'error': 'id is required'})
    name    = body.get('name', group_id)
    members = body.get('members', [])

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO cims_ptt_groups (id, name) VALUES (%s, %s)",
                (group_id, name)
            )
            for m in members:
                uid  = m.get('user_id', m.get('id', ''))
                prio = int(m.get('priority', 0))
                if uid:
                    cur.execute(
                        "INSERT IGNORE INTO cims_ptt_group_members "
                        "(group_id, user_id, priority) VALUES (%s, %s, %s)",
                        (group_id, uid, prio)
                    )
    return HandlerResult(status=201, body={'id': group_id})


async def _update_group(group_id: str, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            if 'name' in body:
                cur.execute(
                    "UPDATE cims_ptt_groups SET name=%s WHERE id=%s",
                    (body['name'], group_id)
                )
                if cur.rowcount == 0:
                    return HandlerResult(status=404, body={'error': 'Group not found'})
            if 'members' in body:
                cur.execute(
                    "DELETE FROM cims_ptt_group_members WHERE group_id=%s",
                    (group_id,)
                )
                for m in body['members']:
                    uid  = m.get('user_id', m.get('id', ''))
                    prio = int(m.get('priority', 0))
                    if uid:
                        cur.execute(
                            "INSERT IGNORE INTO cims_ptt_group_members "
                            "(group_id, user_id, priority) VALUES (%s, %s, %s)",
                            (group_id, uid, prio)
                        )
    return HandlerResult(status=200, body={'id': group_id})


async def _delete_group(group_id: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "DELETE FROM cims_ptt_group_members WHERE group_id=%s",
                (group_id,)
            )
            cur.execute(
                "DELETE FROM cims_ptt_groups WHERE id=%s",
                (group_id,)
            )
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'Group not found'})
    return HandlerResult(status=200, body={'id': group_id})


async def _list_members(group_id: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            # verify group exists
            cur.execute("SELECT id FROM cims_ptt_groups WHERE id=%s", (group_id,))
            if cur.fetchone() is None:
                return HandlerResult(status=404, body={'error': 'Group not found'})
            cur.execute(
                "SELECT user_id, priority FROM cims_ptt_group_members "
                "WHERE group_id=%s ORDER BY priority",
                (group_id,)
            )
            members = cur.fetchall()
    return HandlerResult(status=200, body={'group_id': group_id, 'members': members})


async def _add_member(group_id: str, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    user_id  = body.get('user_id', '').strip()
    if not user_id:
        return HandlerResult(status=400, body={'error': 'user_id is required'})
    priority = int(body.get('priority', 0))

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT id FROM cims_ptt_groups WHERE id=%s", (group_id,))
            if cur.fetchone() is None:
                return HandlerResult(status=404, body={'error': 'Group not found'})
            cur.execute(
                "INSERT INTO cims_ptt_group_members (group_id, user_id, priority) "
                "VALUES (%s, %s, %s) "
                "ON DUPLICATE KEY UPDATE priority=VALUES(priority)",
                (group_id, user_id, priority)
            )
    return HandlerResult(status=201, body={'group_id': group_id, 'user_id': user_id})


async def _remove_member(group_id: str, user_id: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "DELETE FROM cims_ptt_group_members WHERE group_id=%s AND user_id=%s",
                (group_id, user_id)
            )
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'Member not found'})
    return HandlerResult(status=200, body={'group_id': group_id, 'user_id': user_id})


# ──────────────────────────────────────────────────────────────
#  Handler list (registered in app.py)
# ──────────────────────────────────────────────────────────────

CIMS_ADMIN_HANDLER_LIST = [
    (_USERS_BASE,  handle_users,      {}),
    (_GROUPS_BASE, handle_ptt_groups, {}),
]
