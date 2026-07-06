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

from httpsrv.handler import HandlerArgs, HandlerResult
from services.mcptt import notify_csp, refresh_group_members
from services import admin_auth

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


def _coerce_dnd(val) -> int:
    """dnd 값을 0/1 로 정규화. bool/int/문자열 모두 처리.

    ⚠ `1 if val else 0` 는 문자열 "false"/"0" 도 truthy 라 1 이 되는 버그가 있어
    (배치 import 경로와 동일하게) 명시적 참값 집합으로 판정한다.
    """
    return 1 if str(val).strip().upper() in ('Y', 'YES', '1', 'TRUE', 'ON') else 0


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

    # v3 (2026-04-22): /api/v1/users/me[/...] 경로는 users.py 가 담당 (본인 리소스).
    #   admin.handle_users 는 /api/v1/users/:pid (관리자 CRUD) 만 담당.
    if person_id == 'me':
        from . import users as _users
        return await _users.handle_users(handler_args, kwargs)

    # RBAC 게이팅 — 조회는 monitor+, 변경은 manager+ (계획서 §3 권한 매트릭스).
    payload, err = admin_auth.require_role(handler_args, 'monitor' if method == 'GET' else 'manager')
    if err:
        return err

    try:
        if person_id is None:
            if method == 'GET':
                return await _list_users(config)
            elif method == 'POST':
                return await _create_user(handler_args.body, config, payload)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        if person_id == 'batch' and method == 'DELETE':
            return await _batch_delete_users(handler_args.body, config)

        if sub is None:
            if method == 'GET':
                return await _get_user(person_id, config)
            elif method == 'PUT':
                return await _update_user(person_id, handler_args.body, config, payload)
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
    """Check whether users.email column exists (migration may not have run yet)."""
    cur.execute(
        "SELECT COUNT(*) AS cnt FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='users' AND COLUMN_NAME='email'"
    )
    return cur.fetchone()['cnt'] > 0


async def _list_users(config):
    """Phase 4d2 N+1 fix — 옛 패턴: 5020 users × 3 sub query = 15,061 SQL calls.
    cross-host DB (ctrl02 → ctrl01) 에서 ~16s 응답. 4 bulk query 로 단축.
    """
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            has_email = _has_email_column(cur)
            email_col = ", u.email" if has_email else ""
            cur.execute(
                f"SELECT u.id, u.name, u.login_id{email_col}, u.org_id, u.details, "
                "u.create_time, u.update_time "
                "FROM users u "
                "ORDER BY u.id"
            )
            rows = cur.fetchall()
            if not rows:
                return HandlerResult(status=200, body={'users': []})

            # 1 query for all rejects (user_id grouping)
            cur.execute("SELECT user_id, reject_id FROM user_rejects")
            rejects_by_user: dict = {}
            for r in cur.fetchall():
                rejects_by_user.setdefault(r['user_id'], []).append(r['reject_id'])

            # 1 query for all volte_subscriptions
            cur.execute(
                "SELECT id, user_id, service_ref, imsi, dnd, forward_id, register_time, logout_time "
                "FROM volte_subscriptions ORDER BY user_id, id"
            )
            call_subs_by_user: dict = {}
            for s in cur.fetchall():
                s['dnd'] = bool(s['dnd'])
                s['register_time'] = _dt(s['register_time'])
                s['logout_time']   = _dt(s['logout_time'])
                uid = s.pop('user_id')
                call_subs_by_user.setdefault(uid, []).append(s)

            # 1 query for all ptt_subscriptions
            cur.execute(
                "SELECT id, user_id, service_ref, imsi, dnd, forward_id, register_time, logout_time "
                "FROM ptt_subscriptions ORDER BY user_id, id"
            )
            ptt_subs_by_user: dict = {}
            for s in cur.fetchall():
                s['dnd'] = bool(s['dnd'])
                s['register_time'] = _dt(s['register_time'])
                s['logout_time']   = _dt(s['logout_time'])
                uid = s.pop('user_id')
                ptt_subs_by_user.setdefault(uid, []).append(s)

            # group-by 적용
            for row in rows:
                if not has_email:
                    row['email'] = ''
                row['create_time'] = _dt(row['create_time'])
                row['update_time'] = _dt(row['update_time'])
                row['reject_id']          = rejects_by_user.get(row['id'], [])
                row['call_subscriptions'] = call_subs_by_user.get(row['id'], [])
                row['ptt_subscriptions']  = ptt_subs_by_user.get(row['id'], [])
    return HandlerResult(status=200, body={'users': rows})


async def _get_user(person_id: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            has_email = _has_email_column(cur)
            email_col = ", email" if has_email else ""
            cur.execute(
                f"SELECT id, name, login_id{email_col}, org_id, details, create_time, update_time "
                "FROM users WHERE id=%s",
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
                "SELECT reject_id FROM user_rejects WHERE user_id=%s",
                (person_id,)
            )
            row['reject_id'] = [r['reject_id'] for r in cur.fetchall()]

            # call subscriptions
            cur.execute(
                "SELECT id, service_ref, imsi, dnd, forward_id, register_time, logout_time "
                "FROM volte_subscriptions WHERE user_id=%s ORDER BY id",
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
                "SELECT id, service_ref, imsi, dnd, forward_id, register_time, logout_time "
                "FROM ptt_subscriptions WHERE user_id=%s ORDER BY id",
                (person_id,)
            )
            ptt_subs = cur.fetchall()
            for s in ptt_subs:
                s['dnd'] = bool(s['dnd'])
                s['register_time'] = _dt(s['register_time'])
                s['logout_time']   = _dt(s['logout_time'])
            row['ptt_subscriptions'] = ptt_subs

    return HandlerResult(status=200, body=row)


async def _create_user(body, config, payload=None):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    name = body.get('name', '').strip()
    if not name:
        return HandlerResult(status=400, body={'error': 'name is required'})

    # users = 가입자(person). login_id/passwd = 단말(IdMS) 로그인 자격 — MCPTT ID 와 별개.
    #   (콘솔 admin 계정은 OAM console_accounts(file_store) — 여기와 무관.)
    email      = body.get('email', '')
    org_id     = body.get('org_id', '')
    details    = body.get('details') or None
    login_id   = (body.get('login_id') or '').strip() or None
    passwd     = body.get('passwd') or None
    reject_ids = body.get('reject_id', [])

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            has_email = _has_email_column(cur)
            cols = ['name', 'login_id', 'passwd', 'org_id', 'details']
            vals = [name, login_id, passwd, org_id, details]
            if has_email:
                cols.insert(1, 'email'); vals.insert(1, email)
            placeholders = ', '.join(['%s'] * len(vals))
            cur.execute(
                f"INSERT INTO users ({', '.join(cols)}, create_time, update_time) "
                f"VALUES ({placeholders}, NOW(), NOW())",
                vals
            )
            person_id = cur.lastrowid

            if reject_ids:
                for rid in reject_ids:
                    cur.execute(
                        "INSERT IGNORE INTO user_rejects (user_id, reject_id) VALUES (%s, %s)",
                        (person_id, rid)
                    )
    return HandlerResult(status=201, body={'id': person_id})


async def _update_user(person_id: str, body, config, payload=None):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})

    fields = []
    values = []
    # login_id/passwd = 단말(IdMS) 로그인 자격(가입자). 콘솔 admin 계정(OAM)과는 별개.
    for col in ('name', 'login_id', 'passwd', 'email', 'org_id', 'details'):
        if col in body:
            fields.append(f'{col}=%s')
            values.append(body[col])

    if fields:
        fields.append('update_time=NOW()')
        values.append(person_id)
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    f"UPDATE users SET {', '.join(fields)} WHERE id=%s",
                    values
                )
                if cur.rowcount == 0:
                    return HandlerResult(status=404, body={'error': 'User not found'})
                if 'reject_id' in body:
                    cur.execute("DELETE FROM user_rejects WHERE user_id=%s", (person_id,))
                    for rid in body['reject_id']:
                        cur.execute(
                            "INSERT IGNORE INTO user_rejects (user_id, reject_id) VALUES (%s, %s)",
                            (person_id, rid)
                        )
    elif 'reject_id' in body:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute("SELECT id FROM users WHERE id=%s", (person_id,))
                if cur.fetchone() is None:
                    return HandlerResult(status=404, body={'error': 'User not found'})
                cur.execute("DELETE FROM user_rejects WHERE user_id=%s", (person_id,))
                for rid in body['reject_id']:
                    cur.execute(
                        "INSERT IGNORE INTO user_rejects (user_id, reject_id) VALUES (%s, %s)",
                        (person_id, rid)
                    )
    else:
        return HandlerResult(status=400, body={'error': 'No updatable fields provided'})

    return HandlerResult(status=200, body={'id': person_id})


async def _delete_user(person_id: str, config):
    sub_ids = []
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            # 삭제 전 연관 subscription ID 수집
            for table in ('volte_subscriptions', 'ptt_subscriptions'):
                cur.execute(f"SELECT id FROM {table} WHERE user_id=%s", (person_id,))
                sub_ids.extend(r['id'] for r in cur.fetchall())
            cur.execute("DELETE FROM volte_subscriptions WHERE user_id=%s", (person_id,))
            cur.execute("DELETE FROM ptt_subscriptions WHERE user_id=%s", (person_id,))
            cur.execute("DELETE FROM user_rejects WHERE user_id=%s", (person_id,))
            cur.execute("DELETE FROM users WHERE id=%s", (person_id,))
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'User not found'})
    for sid in sub_ids:
        notify_csp("USER_CHANGED", f"tel:{sid}", "DELETE")
    return HandlerResult(status=200, body={'id': person_id})


async def _batch_delete_users(body, config):
    """다수의 가입자를 한번에 삭제"""
    ids = body.get('ids', []) if body else []
    if not ids:
        return HandlerResult(status=400, body={'error': 'ids 필드가 필요합니다'})

    deleted = 0
    errors = []
    for pid in ids:
        try:
            result = await _delete_user(str(pid), config)
            if result.status == 200:
                deleted += 1
            else:
                errors.append({'id': pid, 'error': 'not found'})
        except Exception as e:
            errors.append({'id': pid, 'error': str(e)})

    return HandlerResult(status=200, body={
        'deleted': deleted, 'errors': errors
    })


# ──────────────────────────────────────────────────────────────
#  Subscription handlers (call / ptt)
# ──────────────────────────────────────────────────────────────

def _sub_table(svc: str) -> str:
    return 'volte_subscriptions' if svc == 'call' else 'ptt_subscriptions'




async def _list_subscriptions(person_id: str, svc: str, config):
    table = _sub_table(svc)
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT id FROM users WHERE id=%s", (person_id,))
            if cur.fetchone() is None:
                return HandlerResult(status=404, body={'error': 'User not found'})
            cur.execute(
                f"SELECT id, service_ref, imsi, dnd, forward_id, "
                f"       register_time, logout_time "
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

    # v3 (2026-04-22): service_ref 는 access_services.name 을 참조하는 VARCHAR
    service_ref = body.get('service_ref')
    if service_ref in (None, '', 0, '0'):
        service_ref = None
    else:
        service_ref = str(service_ref).strip() or None
    imsi       = (body.get('imsi') or '').strip() or None
    # P8: auth_id 제거 — imsi 필수
    if not imsi:
        return HandlerResult(status=400, body={'error': 'imsi required'})
    passwd     = body.get('passwd', '')
    dnd        = _coerce_dnd(body.get('dnd', False))
    forward_id = body.get('forward_id', '')
    table      = _sub_table(svc)

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT id FROM users WHERE id=%s", (person_id,))
            if cur.fetchone() is None:
                return HandlerResult(status=404, body={'error': 'User not found'})
            cur.execute(
                f"INSERT INTO {table} (id, user_id, service_ref, imsi, passwd, dnd, forward_id) "
                f"VALUES (%s,%s,%s,%s,%s,%s,%s)",
                (msisdn, person_id, service_ref, imsi, passwd, dnd, forward_id)
            )
    notify_csp("USER_CHANGED", f"tel:{msisdn}", "POST")
    return HandlerResult(status=201, body={'id': msisdn})


async def _update_subscription(person_id: str, svc: str, msisdn: str, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})

    passwd     = body.get('passwd', '')
    dnd        = _coerce_dnd(body.get('dnd', False))
    forward_id = body.get('forward_id', '')
    table      = _sub_table(svc)

    # service_ref/imsi 는 부분 업데이트 — 키가 있을 때만 반영
    fields = ["passwd=%s", "dnd=%s", "forward_id=%s"]
    values = [passwd, dnd, forward_id]
    if 'service_ref' in body:
        sid = body.get('service_ref')
        if sid in (None, '', 0, '0'):
            fields.append("service_ref=NULL")
        else:
            fields.append("service_ref=%s"); values.append(str(sid).strip())
    if 'imsi' in body:
        imsi = (body.get('imsi') or '').strip() or None
        fields.append("imsi=%s"); values.append(imsi)
    values.extend([msisdn, person_id])

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                f"UPDATE {table} SET {', '.join(fields)} "
                f"WHERE id=%s AND user_id=%s",
                values
            )
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'Subscription not found'})
    notify_csp("USER_CHANGED", f"tel:{msisdn}", "PUT")
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
    notify_csp("USER_CHANGED", f"tel:{msisdn}", "DELETE")
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

    # RBAC (계획서 §3 권한 매트릭스) — 조회 monitor+, 그룹 생성 operator+,
    # 기존 그룹 변경은 operator+(본인 소유 그룹) / manager+(모든 그룹).
    if method == 'GET':
        payload, err = admin_auth.require_role(handler_args, 'monitor')
    else:
        payload, err = admin_auth.require_role(handler_args, 'operator')
    if err:
        return err
    if method != 'GET' and group_id is not None \
            and admin_auth.role_rank(payload.get('role')) < admin_auth.role_rank('manager'):
        owner_err = _ensure_group_owner(group_id, payload, config)
        if owner_err:
            return owner_err

    try:
        if group_id is None:
            if method == 'GET':
                return await _list_groups(config)
            elif method == 'POST':
                return await _create_group(handler_args.body, config, payload)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        if sub is None:
            if method == 'GET':
                return await _get_group(group_id, config)
            elif method == 'PUT':
                return await _update_group(group_id, handler_args.body, config, payload)
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


# 그룹 조회 컬럼 (id=surrogate, mcptt_group_id=식별자). 응답에서 id 는 mcptt_group_id 로 노출.
_GROUP_COLS = (
    "id, mcptt_group_id, name, video_enabled, priority, encryption, emergency_call, "
    "imminent_peril_call, emergency_alert, adhoc_enabled, "
    "org_code, session_start, session_end, group_type, on_network, max_members, "
    "require_affiliation, alias, authorized_user_id, created_at"
)


def _shape_group(g: dict, members: list, owner: dict = None):
    """DB row → API 형태. id(응답)=mcptt_group_id, db_id=surrogate.

    owner: {authorized_user(MCPTT ID), authorized_user_name} — authorized_user_id 파생값.
    """
    g['db_id'] = g['id']
    g['id'] = g.get('mcptt_group_id') or str(g['db_id'])
    g['video_enabled'] = bool(g.get('video_enabled', 0))
    g['encryption'] = bool(g.get('encryption', 0))
    g['emergency_call'] = bool(g.get('emergency_call', 0))
    g['imminent_peril_call'] = bool(g.get('imminent_peril_call', 1))
    g['emergency_alert'] = bool(g.get('emergency_alert', 1))
    g['adhoc_enabled'] = bool(g.get('adhoc_enabled', 0))
    g['on_network'] = bool(g.get('on_network', 1))
    g['require_affiliation'] = bool(g.get('require_affiliation', 1))
    if g.get('session_start'): g['session_start'] = g['session_start'].isoformat()
    if g.get('session_end'): g['session_end'] = g['session_end'].isoformat()
    if g.get('created_at'): g['created_at'] = g['created_at'].isoformat()
    # 그룹 소유 (3GPP authorized user) 파생값
    owner = owner or {}
    g['authorized_user'] = owner.get('authorized_user')           # 파생 MCPTT ID (tel:URI)
    g['authorized_user_name'] = owner.get('authorized_user_name')  # 표시명
    g['members'] = members
    return g


def _owner_map(cur, auth_ids):
    """authorized_user_id 집합 → {uid: {authorized_user(tel:MSISDN), authorized_user_name}}.

    파생 MCPTT ID = 그 user 의 PTT 가입 MSISDN (ptt_subscriptions). 표시명 = users.name/login_id.
    """
    ids = [i for i in {a for a in auth_ids} if i]
    if not ids:
        return {}
    fmt = ','.join(['%s'] * len(ids))
    cur.execute(
        f"SELECT u.id, u.name, "
        f"  (SELECT id FROM ptt_subscriptions WHERE user_id=u.id ORDER BY id LIMIT 1) AS ptt_id "
        f"FROM users u WHERE u.id IN ({fmt})",
        tuple(ids)
    )
    out = {}
    for r in cur.fetchall():
        ptt = r.get('ptt_id')
        out[r['id']] = {
            'authorized_user': (f"tel:{ptt}" if ptt else None),
            'authorized_user_name': r.get('name'),
        }
    return out


def _ensure_group_owner(group_id: str, payload: dict, config):
    """operator 의 본인 소유 그룹 여부 확인. owner 아니면 HandlerResult(err), OK 면 None.

    그룹 부재 시 404. authorized_user_id 미지정(레거시) 그룹은 operator 변경 불가(403).
    """
    try:
        uid = int(payload.get('sub'))
    except (TypeError, ValueError):
        return HandlerResult(status=403, body={'error': '권한이 부족합니다'})
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT authorized_user_id FROM ptt_groups WHERE mcptt_group_id=%s",
                (group_id,)
            )
            row = cur.fetchone()
    if row is None:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if row.get('authorized_user_id') != uid:
        return HandlerResult(status=403, body={'error': '본인이 소유한 그룹만 변경할 수 있습니다'})
    return None


async def _list_groups(config):
    """Phase 4d2 N+1 fix — group 당 sub query 제거. 2 bulk query 로 단축."""
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(f"SELECT {_GROUP_COLS} FROM ptt_groups ORDER BY mcptt_group_id")
            groups = cur.fetchall()
            if not groups:
                return HandlerResult(status=200, body={'groups': []})
            # 1 query for all members (group_id=surrogate grouping)
            cur.execute(
                "SELECT group_id, user_id, priority, role, mcptt_id FROM ptt_group_members "
                "ORDER BY group_id, priority"
            )
            members_by_group: dict = {}
            for m in cur.fetchall():
                gid = m.pop('group_id')
                members_by_group.setdefault(gid, []).append(m)
            owners = _owner_map(cur, [g.get('authorized_user_id') for g in groups])
            groups = [
                _shape_group(g, members_by_group.get(g['id'], []),
                             owners.get(g.get('authorized_user_id')))
                for g in groups
            ]
    return HandlerResult(status=200, body={'groups': groups})


async def _get_group(group_id: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                f"SELECT {_GROUP_COLS} FROM ptt_groups WHERE mcptt_group_id=%s",
                (group_id,)
            )
            group = cur.fetchone()
            if group is None:
                return HandlerResult(status=404, body={'error': 'Group not found'})
            cur.execute(
                "SELECT user_id, priority, role, mcptt_id FROM ptt_group_members "
                "WHERE group_id=%s ORDER BY priority",
                (group['id'],)
            )
            members = cur.fetchall()
            owners = _owner_map(cur, [group.get('authorized_user_id')])
            group = _shape_group(group, members, owners.get(group.get('authorized_user_id')))
    return HandlerResult(status=200, body=group)


def _resolve_group_pk(cur, group_id: str):
    """mcptt_group_id 식별자 → surrogate id. 없으면 None."""
    cur.execute("SELECT id FROM ptt_groups WHERE mcptt_group_id=%s", (group_id,))
    row = cur.fetchone()
    return row['id'] if row else None


def _insert_member(cur, gpk, m):
    uid  = m.get('user_id', m.get('id', ''))
    if not uid:
        return
    prio = int(m.get('priority', 0))
    role = m.get('role', 'participant')
    if role not in ('chair', 'participant'):
        role = 'participant'
    mcptt_id = m.get('mcptt_id') or None
    cur.execute(
        "INSERT IGNORE INTO ptt_group_members "
        "(group_id, user_id, priority, role, mcptt_id) VALUES (%s, %s, %s, %s, %s)",
        (gpk, uid, prio, role, mcptt_id)
    )


def _is_ptt_subscriber(cur, user_id) -> bool:
    cur.execute("SELECT 1 FROM ptt_subscriptions WHERE user_id=%s LIMIT 1", (user_id,))
    return cur.fetchone() is not None


async def _create_group(body, config, payload=None):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    # id = mcptt_group_id 식별자 (surrogate id 는 자동발행)
    group_id = (body.get('mcptt_group_id') or body.get('id', '')).strip()
    if not group_id:
        return HandlerResult(status=400, body={'error': 'id (mcptt_group_id) is required'})
    name           = body.get('name', group_id)
    video_enabled  = 1 if body.get('video_enabled', False) else 0
    priority       = int(body.get('priority', 5))
    encryption     = 1 if body.get('encryption', False) else 0
    emergency_call = 1 if body.get('emergency_call', False) else 0
    imminent_peril_call = 1 if body.get('imminent_peril_call', True) else 0
    emergency_alert     = 1 if body.get('emergency_alert', True) else 0
    adhoc_enabled       = 1 if body.get('adhoc_enabled', False) else 0
    org_code       = body.get('org_code', '') or None
    session_start  = body.get('session_start') or None
    session_end    = body.get('session_end') or None
    group_type     = body.get('group_type', 'prearranged')
    if group_type not in ('prearranged', 'chat', 'broadcast'):
        group_type = 'prearranged'
    on_network     = 1 if body.get('on_network', True) else 0
    max_members    = int(body.get('max_members', 0))
    require_affiliation = 1 if body.get('require_affiliation', True) else 0
    alias          = body.get('alias', '') or None
    members        = body.get('members', [])

    # 그룹 소유 (authorized user) — 명시 없으면 생성자(payload sub) 기본.
    authorized_user_id = body.get('authorized_user_id')
    explicit_owner = authorized_user_id is not None
    if not explicit_owner and payload is not None:
        try:
            authorized_user_id = int(payload.get('sub'))
        except (TypeError, ValueError):
            authorized_user_id = None
    if authorized_user_id is not None:
        try:
            authorized_user_id = int(authorized_user_id)
        except (TypeError, ValueError):
            return HandlerResult(status=400, body={'error': 'invalid authorized_user_id'})

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            # 규격: authorized user 는 PTT 가입자여야 함 (명시 지정 시 강제 검증).
            if explicit_owner and authorized_user_id is not None \
                    and not _is_ptt_subscriber(cur, authorized_user_id):
                return HandlerResult(status=400,
                                     body={'error': 'authorized user 는 PTT 가입자여야 합니다'})
            cur.execute(
                "INSERT INTO ptt_groups (mcptt_group_id, name, video_enabled, priority, encryption, "
                "emergency_call, imminent_peril_call, emergency_alert, adhoc_enabled, "
                "org_code, session_start, session_end, group_type, on_network, "
                "max_members, require_affiliation, alias, authorized_user_id) "
                "VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)",
                (group_id, name, video_enabled, priority, encryption,
                 emergency_call, imminent_peril_call, emergency_alert, adhoc_enabled,
                 org_code, session_start, session_end, group_type,
                 on_network, max_members, require_affiliation, alias, authorized_user_id)
            )
            gpk = cur.lastrowid
            for m in members:
                _insert_member(cur, gpk, m)
    notify_csp("GROUP_CHANGED", f"tel:{group_id}", "POST")
    return HandlerResult(status=201, body={'id': group_id})


async def _update_group(group_id: str, body, config, payload=None):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})

    # 소유자(authorized_user_id) 재지정은 manager+ 만 (operator 는 소유 이전 불가).
    can_reassign_owner = payload is None or \
        admin_auth.role_rank(payload.get('role')) >= admin_auth.role_rank('manager')

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            gpk = _resolve_group_pk(cur, group_id)
            if gpk is None:
                return HandlerResult(status=404, body={'error': 'Group not found'})
            if 'authorized_user_id' in body and can_reassign_owner:
                new_owner = body.get('authorized_user_id')
                if new_owner is not None:
                    try:
                        new_owner = int(new_owner)
                    except (TypeError, ValueError):
                        return HandlerResult(status=400, body={'error': 'invalid authorized_user_id'})
                    if not _is_ptt_subscriber(cur, new_owner):
                        return HandlerResult(status=400,
                                             body={'error': 'authorized user 는 PTT 가입자여야 합니다'})
                cur.execute("UPDATE ptt_groups SET authorized_user_id=%s WHERE id=%s", (new_owner, gpk))
            update_fields = []
            update_vals   = []
            if 'name' in body:
                update_fields.append('name=%s')
                update_vals.append(body['name'])
            if 'video_enabled' in body:
                update_fields.append('video_enabled=%s')
                update_vals.append(1 if body['video_enabled'] else 0)
            for fld in ('priority', 'max_members'):
                if fld in body:
                    update_fields.append(f'{fld}=%s')
                    update_vals.append(int(body[fld]))
            for fld in ('encryption', 'emergency_call', 'imminent_peril_call', 'emergency_alert',
                        'adhoc_enabled', 'on_network', 'require_affiliation'):
                if fld in body:
                    update_fields.append(f'{fld}=%s')
                    update_vals.append(1 if body[fld] else 0)
            if 'group_type' in body and body['group_type'] in ('prearranged', 'chat', 'broadcast'):
                update_fields.append('group_type=%s')
                update_vals.append(body['group_type'])
            for fld in ('org_code', 'alias'):
                if fld in body:
                    update_fields.append(f'{fld}=%s')
                    update_vals.append(body[fld] or None)
            for fld in ('session_start', 'session_end'):
                if fld in body:
                    update_fields.append(f'{fld}=%s')
                    update_vals.append(body[fld] or None)
            if update_fields:
                update_vals.append(gpk)
                cur.execute(
                    "UPDATE ptt_groups SET " + ", ".join(update_fields) + " WHERE id=%s",
                    update_vals
                )
            if 'members' in body:
                cur.execute("DELETE FROM ptt_group_members WHERE group_id=%s", (gpk,))
                for m in body['members']:
                    _insert_member(cur, gpk, m)
    refresh_group_members(group_id)
    notify_csp("GROUP_CHANGED", f"tel:{group_id}", "PUT")
    return HandlerResult(status=200, body={'id': group_id})


async def _delete_group(group_id: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            # FK ON DELETE CASCADE 가 members/affiliations 정리
            cur.execute("DELETE FROM ptt_groups WHERE mcptt_group_id=%s", (group_id,))
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'Group not found'})
    notify_csp("GROUP_CHANGED", f"tel:{group_id}", "DELETE")
    return HandlerResult(status=200, body={'id': group_id})


async def _list_members(group_id: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            gpk = _resolve_group_pk(cur, group_id)
            if gpk is None:
                return HandlerResult(status=404, body={'error': 'Group not found'})
            cur.execute(
                "SELECT user_id, priority, role, mcptt_id FROM ptt_group_members "
                "WHERE group_id=%s ORDER BY priority",
                (gpk,)
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
    role = body.get('role', 'participant')
    if role not in ('chair', 'participant'):
        role = 'participant'
    mcptt_id = body.get('mcptt_id') or None

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            gpk = _resolve_group_pk(cur, group_id)
            if gpk is None:
                return HandlerResult(status=404, body={'error': 'Group not found'})
            cur.execute(
                "INSERT INTO ptt_group_members (group_id, user_id, priority, role, mcptt_id) "
                "VALUES (%s, %s, %s, %s, %s) "
                "ON DUPLICATE KEY UPDATE priority=VALUES(priority), role=VALUES(role), mcptt_id=VALUES(mcptt_id)",
                (gpk, user_id, priority, role, mcptt_id)
            )
    refresh_group_members(group_id)
    notify_csp("GROUP_CHANGED", f"tel:{group_id}", "PUT")
    return HandlerResult(status=201, body={'group_id': group_id, 'user_id': user_id})


async def _remove_member(group_id: str, user_id: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            gpk = _resolve_group_pk(cur, group_id)
            if gpk is None:
                return HandlerResult(status=404, body={'error': 'Group not found'})
            cur.execute(
                "DELETE FROM ptt_group_members WHERE group_id=%s AND user_id=%s",
                (gpk, user_id)
            )
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'Member not found'})
    refresh_group_members(group_id)
    notify_csp("GROUP_CHANGED", f"tel:{group_id}", "PUT")
    return HandlerResult(status=200, body={'group_id': group_id, 'user_id': user_id})


# ──────────────────────────────────────────────────────────────
#  Handler list (registered in app.py)
# ──────────────────────────────────────────────────────────────




CIMS_ADMIN_HANDLER_LIST = [
    (_USERS_BASE,    handle_users,      {}),
    (_GROUPS_BASE,   handle_ptt_groups, {}),
    # call_logs는 csc_flow.py의 파일시스템 기반 API로 이동
]
