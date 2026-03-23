"""
CIMS Admin REST API
Subscriber and PTT group CRUD operations backed by MariaDB.

Routes (prefix-matched):
  /api/v1/users                               GET list / POST create
  /api/v1/users/{pid}                         GET / PUT / DELETE
  /api/v1/users/{pid}/call                    GET list / POST add call subscription
  /api/v1/users/{pid}/call/{msisdn}           PUT update / DELETE remove call subscription
  /api/v1/users/{pid}/ptt                     GET list / POST add PTT subscription
  /api/v1/users/{pid}/ptt/{msisdn}            PUT update / DELETE remove PTT subscription
  /api/v1/ptt/groups                          GET list / POST create
  /api/v1/ptt/groups/{id}                     GET / PUT / DELETE
  /api/v1/ptt/groups/{id}/members             GET list / POST add
  /api/v1/ptt/groups/{id}/members/{uid}       DELETE
"""

from urllib.parse import urlparse, unquote
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
    """Return the path segments that come after *base*, URL-decoded.

    e.g. base='/api/v1/users', full_path='/api/v1/users/%2B821001' → ('+821001',)
    """
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _dt(val):
    """Convert datetime to ISO string or None."""
    if val is None:
        return None
    return val.isoformat()


# ──────────────────────────────────────────────────────────────
#  Users handler
# ──────────────────────────────────────────────────────────────

_USERS_BASE = '/api/v1/users'


async def handle_users(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    parts = _path_parts(handler_args.full_path, _USERS_BASE)
    # parts: () | (pid,) | (pid, 'call'|'ptt') | (pid, 'call'|'ptt', msisdn)
    person_id = parts[0] if len(parts) > 0 else None
    sub       = parts[1] if len(parts) > 1 else None   # 'call' | 'ptt'
    sub_id    = parts[2] if len(parts) > 2 else None   # MSISDN of the subscription
    method    = handler_args.method.upper()

    try:
        if person_id is None:
            if method == 'GET':
                return await _list_users(config)
            elif method == 'POST':
                return await _create_user(handler_args.body, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        if sub is None:
            if method == 'GET':
                return await _get_user(person_id, config)
            elif method == 'PUT':
                return await _update_user(person_id, handler_args.body, config)
            elif method == 'DELETE':
                return await _delete_user(person_id, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        if sub in ('call', 'ptt'):
            if sub_id is None:
                if method == 'GET':
                    return await _list_subscriptions(person_id, sub, config)
                elif method == 'POST':
                    return await _add_subscription(person_id, sub, handler_args.body, config)
                return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
            else:
                if method == 'PUT':
                    return await _update_subscription(person_id, sub, sub_id, handler_args.body, config)
                elif method == 'DELETE':
                    return await _delete_subscription(person_id, sub, sub_id, config)
                return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        return HandlerResult(status=404, body={'error': 'Not Found'})
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})


def _has_email_column(cur) -> bool:
    """Check whether cims_users.email column exists (migration may not have run yet)."""
    cur.execute(
        "SELECT COUNT(*) AS cnt FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cims_users' AND COLUMN_NAME='email'"
    )
    return cur.fetchone()['cnt'] > 0


async def _list_users(config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            has_email = _has_email_column(cur)
            email_col = ", u.email" if has_email else ""
            cur.execute(
                f"SELECT u.id, u.name{email_col}, u.org_id, u.details, "
                "u.create_time, u.update_time "
                "FROM cims_users u "
                "ORDER BY u.id"
            )
            rows = cur.fetchall()
            for row in rows:
                if not has_email:
                    row['email'] = ''
                row['create_time'] = _dt(row['create_time'])
                row['update_time'] = _dt(row['update_time'])
                # attach reject list
                cur.execute(
                    "SELECT reject_id FROM cims_user_rejects WHERE user_id=%s",
                    (row['id'],)
                )
                row['reject_id'] = [r['reject_id'] for r in cur.fetchall()]
                # attach subscriptions
                cur.execute(
                    "SELECT id, auth_id, dnd, forward_id, register_time, logout_time "
                    "FROM cims_call_users WHERE user_id=%s ORDER BY id",
                    (row['id'],)
                )
                call_subs = cur.fetchall()
                for s in call_subs:
                    s['dnd'] = bool(s['dnd'])
                    s['register_time'] = _dt(s['register_time'])
                    s['logout_time']   = _dt(s['logout_time'])
                row['call_subscriptions'] = call_subs

                cur.execute(
                    "SELECT id, auth_id, dnd, forward_id, register_time, logout_time "
                    "FROM cims_ptt_users WHERE user_id=%s ORDER BY id",
                    (row['id'],)
                )
                ptt_subs = cur.fetchall()
                for s in ptt_subs:
                    s['dnd'] = bool(s['dnd'])
                    s['register_time'] = _dt(s['register_time'])
                    s['logout_time']   = _dt(s['logout_time'])
                row['ptt_subscriptions'] = ptt_subs
    return HandlerResult(status=200, body={'users': rows})


async def _get_user(person_id: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            has_email = _has_email_column(cur)
            email_col = ", email" if has_email else ""
            cur.execute(
                f"SELECT id, name{email_col}, org_id, details, create_time, update_time "
                "FROM cims_users WHERE id=%s",
                (person_id,)
            )
            row = cur.fetchone()
            if row is None:
                return HandlerResult(status=404, body={'error': 'User not found'})
            if not has_email:
                row['email'] = ''
            row['create_time'] = _dt(row['create_time'])
            row['update_time'] = _dt(row['update_time'])

            # reject list
            cur.execute(
                "SELECT reject_id FROM cims_user_rejects WHERE user_id=%s",
                (person_id,)
            )
            row['reject_id'] = [r['reject_id'] for r in cur.fetchall()]

            # call subscriptions
            cur.execute(
                "SELECT id, auth_id, dnd, forward_id, register_time, logout_time "
                "FROM cims_call_users WHERE user_id=%s ORDER BY id",
                (person_id,)
            )
            call_subs = cur.fetchall()
            for s in call_subs:
                s['dnd'] = bool(s['dnd'])
                s['register_time'] = _dt(s['register_time'])
                s['logout_time']   = _dt(s['logout_time'])
            row['call_subscriptions'] = call_subs

            # ptt subscriptions
            cur.execute(
                "SELECT id, auth_id, dnd, forward_id, register_time, logout_time "
                "FROM cims_ptt_users WHERE user_id=%s ORDER BY id",
                (person_id,)
            )
            ptt_subs = cur.fetchall()
            for s in ptt_subs:
                s['dnd'] = bool(s['dnd'])
                s['register_time'] = _dt(s['register_time'])
                s['logout_time']   = _dt(s['logout_time'])
            row['ptt_subscriptions'] = ptt_subs

    return HandlerResult(status=200, body=row)


async def _create_user(body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    name = body.get('name', '').strip()
    if not name:
        return HandlerResult(status=400, body={'error': 'name is required'})

    email      = body.get('email', '')
    org_id     = body.get('org_id', '')
    details    = body.get('details') or None
    reject_ids = body.get('reject_id', [])

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            has_email = _has_email_column(cur)
            if has_email:
                cur.execute(
                    "INSERT INTO cims_users "
                    "(name, email, org_id, details, create_time, update_time) "
                    "VALUES (%s, %s, %s, %s, NOW(), NOW())",
                    (name, email, org_id, details)
                )
            else:
                cur.execute(
                    "INSERT INTO cims_users "
                    "(name, org_id, details, create_time, update_time) "
                    "VALUES (%s, %s, %s, NOW(), NOW())",
                    (name, org_id, details)
                )
            person_id = cur.lastrowid

            if reject_ids:
                for rid in reject_ids:
                    cur.execute(
                        "INSERT IGNORE INTO cims_user_rejects (user_id, reject_id) VALUES (%s, %s)",
                        (person_id, rid)
                    )
    return HandlerResult(status=201, body={'id': person_id})


async def _update_user(person_id: str, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})

    fields = []
    values = []
    for col in ('name', 'email', 'org_id', 'details'):
        if col in body:
            fields.append(f'{col}=%s')
            values.append(body[col])

    if fields:
        fields.append('update_time=NOW()')
        values.append(person_id)
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    f"UPDATE cims_users SET {', '.join(fields)} WHERE id=%s",
                    values
                )
                if cur.rowcount == 0:
                    return HandlerResult(status=404, body={'error': 'User not found'})
                if 'reject_id' in body:
                    cur.execute("DELETE FROM cims_user_rejects WHERE user_id=%s", (person_id,))
                    for rid in body['reject_id']:
                        cur.execute(
                            "INSERT IGNORE INTO cims_user_rejects (user_id, reject_id) VALUES (%s, %s)",
                            (person_id, rid)
                        )
    elif 'reject_id' in body:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute("SELECT id FROM cims_users WHERE id=%s", (person_id,))
                if cur.fetchone() is None:
                    return HandlerResult(status=404, body={'error': 'User not found'})
                cur.execute("DELETE FROM cims_user_rejects WHERE user_id=%s", (person_id,))
                for rid in body['reject_id']:
                    cur.execute(
                        "INSERT IGNORE INTO cims_user_rejects (user_id, reject_id) VALUES (%s, %s)",
                        (person_id, rid)
                    )
    else:
        return HandlerResult(status=400, body={'error': 'No updatable fields provided'})

    return HandlerResult(status=200, body={'id': person_id})


async def _delete_user(person_id: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("DELETE FROM cims_user_rejects WHERE user_id=%s", (person_id,))
            cur.execute("DELETE FROM cims_users WHERE id=%s", (person_id,))
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'User not found'})
    return HandlerResult(status=200, body={'id': person_id})


# ──────────────────────────────────────────────────────────────
#  Subscription handlers (call / ptt)
# ──────────────────────────────────────────────────────────────

def _sub_table(svc: str) -> str:
    return 'cims_call_users' if svc == 'call' else 'cims_ptt_users'


async def _list_subscriptions(person_id: str, svc: str, config):
    table = _sub_table(svc)
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT id FROM cims_users WHERE id=%s", (person_id,))
            if cur.fetchone() is None:
                return HandlerResult(status=404, body={'error': 'User not found'})
            cur.execute(
                f"SELECT id, auth_id, dnd, forward_id, register_time, logout_time "
                f"FROM {table} WHERE user_id=%s ORDER BY id",
                (person_id,)
            )
            subs = cur.fetchall()
            for s in subs:
                s['dnd'] = bool(s['dnd'])
                s['register_time'] = _dt(s['register_time'])
                s['logout_time']   = _dt(s['logout_time'])
    return HandlerResult(status=200, body={'subscriptions': subs})


async def _add_subscription(person_id: str, svc: str, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    msisdn = body.get('id', '').strip()
    if not msisdn:
        return HandlerResult(status=400, body={'error': 'id (MSISDN) is required'})

    auth_id    = body.get('auth_id', msisdn)
    passwd     = body.get('passwd', '')
    dnd        = 1 if body.get('dnd', False) else 0
    forward_id = body.get('forward_id', '')
    table      = _sub_table(svc)

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT id FROM cims_users WHERE id=%s", (person_id,))
            if cur.fetchone() is None:
                return HandlerResult(status=404, body={'error': 'User not found'})
            cur.execute(
                f"INSERT INTO {table} (id, user_id, auth_id, passwd, dnd, forward_id) "
                f"VALUES (%s, %s, %s, %s, %s, %s)",
                (msisdn, person_id, auth_id, passwd, dnd, forward_id)
            )
    return HandlerResult(status=201, body={'id': msisdn})


async def _update_subscription(person_id: str, svc: str, msisdn: str, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})

    auth_id    = body.get('auth_id', msisdn)
    passwd     = body.get('passwd', '')
    dnd        = 1 if body.get('dnd', False) else 0
    forward_id = body.get('forward_id', '')
    table      = _sub_table(svc)

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                f"UPDATE {table} SET auth_id=%s, passwd=%s, dnd=%s, forward_id=%s "
                f"WHERE id=%s AND user_id=%s",
                (auth_id, passwd, dnd, forward_id, msisdn, person_id)
            )
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'Subscription not found'})
    return HandlerResult(status=200, body={'id': msisdn})


async def _delete_subscription(person_id: str, svc: str, msisdn: str, config):
    table = _sub_table(svc)
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                f"DELETE FROM {table} WHERE id=%s AND user_id=%s",
                (msisdn, person_id)
            )
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'Subscription not found'})
    return HandlerResult(status=200, body={'id': msisdn})


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


# ──────────────────────────────────────────────────────────────
#  Call logs handler
# ──────────────────────────────────────────────────────────────

_CALL_LOGS_BASE = '/api/v1/call/logs'

_END_REASON_KO = {
    'normal': '정상종료', 'busy': '통화중', 'cancel': '취소',
    'timeout': '시간초과', 'error': '오류',
}


def _call_log_row(row: dict) -> dict:
    row['invite_time'] = _dt(row['invite_time'])
    row['answer_time'] = _dt(row['answer_time'])
    row['end_time']    = _dt(row['end_time'])
    row['end_reason_ko'] = _END_REASON_KO.get(row.get('end_reason') or '', '')
    return row


def _tables_exist(cur) -> bool:
    cur.execute(
        "SELECT COUNT(*) AS cnt FROM information_schema.TABLES "
        "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cims_call_logs'"
    )
    return cur.fetchone()['cnt'] > 0


async def handle_call_logs(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """
    GET /api/v1/call/logs
        ?state=ringing|active|ended   (default: all)
        ?caller=MSISDN
        ?callee=MSISDN
        ?msisdn=MSISDN                (caller OR callee OR participant)
        ?group_id=ID
        ?call_type=voip|ptt
        ?from_dt=YYYY-MM-DD
        ?to_dt=YYYY-MM-DD
        ?limit=N                      (default 200, max 1000)
        ?offset=N                     (default 0)

    GET /api/v1/call/logs/active      (shorthand: state=ringing,active)
    """
    from urllib.parse import urlparse, parse_qs, unquote

    config = kwargs.get('config', {})
    parsed = urlparse(handler_args.full_path)
    qs     = parse_qs(parsed.query)

    def qp(name, default=None):
        vals = qs.get(name)
        return unquote(vals[0]) if vals else default

    # distinguish /call/logs vs /call/logs/active
    path_tail = parsed.path.rstrip('/')
    active_only = path_tail.endswith('/active')

    if handler_args.method.upper() != 'GET':
        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

    state     = qp('state')
    caller    = qp('caller')
    callee    = qp('callee')
    msisdn    = qp('msisdn')
    group_id  = qp('group_id')
    call_type = qp('call_type')
    from_dt   = qp('from_dt')
    to_dt     = qp('to_dt')
    limit     = min(int(qp('limit', 200)), 1000)
    offset    = int(qp('offset', 0))

    if active_only:
        state = 'active_ringing'  # special token

    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                if not _tables_exist(cur):
                    return HandlerResult(status=503, body={
                        'error': 'Call log tables not created yet. Run: sudo mysql cims < sql/migrate_call_logs.sql'
                    })

                # ── build WHERE ──
                where = []
                params = []

                if state == 'active_ringing':
                    where.append("l.state IN ('ringing','active')")
                elif state:
                    where.append("l.state = %s")
                    params.append(state)

                if caller:
                    where.append("l.initiator = %s")
                    params.append(caller)
                if callee:
                    where.append("l.callee = %s")
                    params.append(callee)
                if group_id:
                    where.append("l.group_id = %s")
                    params.append(group_id)
                if call_type:
                    where.append("l.call_type = %s")
                    params.append(call_type)
                if from_dt:
                    where.append("l.invite_time >= %s")
                    params.append(from_dt + ' 00:00:00')
                if to_dt:
                    where.append("l.invite_time <= %s")
                    params.append(to_dt + ' 23:59:59')

                # msisdn: caller OR callee OR participant
                if msisdn:
                    where.append(
                        "(l.initiator = %s OR l.callee = %s OR EXISTS("
                        " SELECT 1 FROM cims_call_participants cp"
                        " WHERE cp.log_id = l.id AND cp.msisdn = %s))"
                    )
                    params += [msisdn, msisdn, msisdn]

                where_sql = ("WHERE " + " AND ".join(where)) if where else ""

                # ── total count ──
                cur.execute(
                    f"SELECT COUNT(*) AS cnt FROM cims_call_logs l {where_sql}",
                    params
                )
                total = cur.fetchone()['cnt']

                # ── main query ──
                cur.execute(
                    f"SELECT l.id, l.call_id, l.call_type, l.group_id, "
                    f"l.initiator, l.callee, l.state, "
                    f"l.invite_time, l.answer_time, l.end_time, "
                    f"l.duration, l.sip_status, l.end_reason "
                    f"FROM cims_call_logs l {where_sql} "
                    f"ORDER BY l.invite_time DESC "
                    f"LIMIT %s OFFSET %s",
                    params + [limit, offset]
                )
                logs = cur.fetchall()

                for row in logs:
                    _call_log_row(row)

                    # attach participants
                    cur.execute(
                        "SELECT msisdn, role, join_time, leave_time "
                        "FROM cims_call_participants WHERE log_id = %s "
                        "ORDER BY role, join_time",
                        (row['id'],)
                    )
                    parts = cur.fetchall()
                    for p in parts:
                        p['join_time']  = _dt(p['join_time'])
                        p['leave_time'] = _dt(p['leave_time'])
                    row['participants'] = parts

        return HandlerResult(status=200, body={
            'total': total, 'limit': limit, 'offset': offset, 'logs': logs
        })
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})


CIMS_ADMIN_HANDLER_LIST = [
    (_USERS_BASE,    handle_users,      {}),
    (_GROUPS_BASE,   handle_ptt_groups, {}),
    (_CALL_LOGS_BASE, handle_call_logs, {}),
]
