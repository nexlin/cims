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
from services.mcptt import notify_csp, refresh_group_members, DEFAULT_USER_PROFILE, update_user_profile_cache
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

        # 사용자 MCPTT 프로파일 (SOS 대상 결정·개시 인가) — /users/:pid/ptt/:msisdn/profile
        if sub == 'ptt' and sub_id is not None and len(parts) > 3 and parts[3] == 'profile':
            if method == 'GET':
                return await _get_ptt_profile(person_id, sub_id, config)
            elif method == 'PUT':
                return await _put_ptt_profile(person_id, sub_id, handler_args.body, config)
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


def _has_user_column(cur, column: str) -> bool:
    """Check whether users.<column> exists (migration may not have run yet)."""
    cur.execute(
        "SELECT COUNT(*) AS cnt FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='users' AND COLUMN_NAME=%s",
        (column,)
    )
    return cur.fetchone()['cnt'] > 0


def _has_email_column(cur) -> bool:
    return _has_user_column(cur, 'email')


async def _list_users(config):
    """Phase 4d2 N+1 fix — 옛 패턴: 5020 users × 3 sub query = 15,061 SQL calls.
    cross-host DB (ctrl02 → ctrl01) 에서 ~16s 응답. 4 bulk query 로 단축.
    """
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            has_email = _has_email_column(cur)
            email_col = ", u.email" if has_email else ""
            has_title = _has_user_column(cur, 'title')
            title_col = ", u.title" if has_title else ""
            cur.execute(
                f"SELECT u.id, u.name, u.login_id{email_col}, u.org_id{title_col}, u.details, "
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
                if not has_title:
                    row['title'] = ''
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
            has_title = _has_user_column(cur, 'title')
            title_col = ", title" if has_title else ""
            cur.execute(
                f"SELECT id, name, login_id{email_col}, org_id{title_col}, details, create_time, update_time "
                "FROM users WHERE id=%s",
                (person_id,)
            )
            row = cur.fetchone()
            if row is None:
                return HandlerResult(status=404, body={'error': 'User not found'})
            if not has_email:
                row['email'] = ''
            if not has_title:
                row['title'] = ''
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

            # 사용자 MCPTT 프로파일 동봉 (부재/마이그레이션 전 = None → 콘솔이 기본값 표시)
            profiles = {}
            if ptt_subs:
                try:
                    ph = ','.join(['%s'] * len(ptt_subs))
                    cur.execute(
                        "SELECT ptt_id, allow_emergency_call, allow_emergency_alert, allow_adhoc_call, "
                        f"emergency_group_mode, emergency_group_id FROM ptt_user_profile WHERE ptt_id IN ({ph})",
                        [s['id'] for s in ptt_subs])
                    for p in cur.fetchall():
                        profiles[p['ptt_id']] = {
                            'allow_emergency_call': bool(p['allow_emergency_call']),
                            'allow_emergency_alert': bool(p['allow_emergency_alert']),
                            'allow_adhoc_call': bool(p['allow_adhoc_call']),
                            'emergency_group_mode': p['emergency_group_mode'],
                            'emergency_group_id': p['emergency_group_id'],
                        }
                except pymysql.Error:
                    pass
            for s in ptt_subs:
                s['mcptt_profile'] = profiles.get(s['id'])
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
    title      = body.get('title', '')
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
            if _has_user_column(cur, 'title'):
                cols.append('title'); vals.append(title)
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
    for col in ('name', 'login_id', 'passwd', 'email', 'org_id', 'title', 'details'):
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
    if svc == 'ptt':
        update_user_profile_cache(msisdn, None)  # 프로파일 행은 FK CASCADE 로 함께 삭제됨
    notify_csp("USER_CHANGED", f"tel:{msisdn}", "DELETE")
    return HandlerResult(status=200, body={'id': msisdn})


# ──────────────────────────────────────────────────────────────
#  사용자 MCPTT 프로파일 (ptt_user_profile — TS 24.484 / TS 24.379 §6.3.3.1.13.2)
# ──────────────────────────────────────────────────────────────

_PROFILE_BOOL_FIELDS = ('allow_emergency_call', 'allow_emergency_alert', 'allow_adhoc_call')


async def _get_ptt_profile(person_id: str, msisdn: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT 1 FROM ptt_subscriptions WHERE id=%s AND user_id=%s", (msisdn, person_id))
            if cur.fetchone() is None:
                return HandlerResult(status=404, body={'error': 'Subscription not found'})
            cur.execute(
                "SELECT allow_emergency_call, allow_emergency_alert, allow_adhoc_call, "
                "emergency_group_mode, emergency_group_id "
                "FROM ptt_user_profile WHERE ptt_id=%s", (msisdn,))
            row = cur.fetchone()
    if row:
        prof = {k: bool(row[k]) for k in _PROFILE_BOOL_FIELDS}
        prof['emergency_group_mode'] = row['emergency_group_mode']
        prof['emergency_group_id'] = row['emergency_group_id']
        prof['exists'] = True
    else:
        prof = dict(DEFAULT_USER_PROFILE)
        prof['exists'] = False
    prof['id'] = msisdn
    return HandlerResult(status=200, body=prof)


async def _put_ptt_profile(person_id: str, msisdn: str, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    mode = (body.get('emergency_group_mode') or 'DedicatedGroup').strip()
    if mode not in ('DedicatedGroup', 'UseCurrentlySelectedGroup'):
        return HandlerResult(status=400, body={'error': 'invalid emergency_group_mode'})
    egid = (body.get('emergency_group_id') or '').strip() or None
    allow_call  = 1 if body.get('allow_emergency_call', True) else 0
    allow_alert = 1 if body.get('allow_emergency_alert', True) else 0
    allow_adhoc = 1 if body.get('allow_adhoc_call', True) else 0

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT 1 FROM ptt_subscriptions WHERE id=%s AND user_id=%s", (msisdn, person_id))
            if cur.fetchone() is None:
                return HandlerResult(status=404, body={'error': 'Subscription not found'})
            if egid:
                cur.execute("SELECT 1 FROM ptt_groups WHERE mcptt_group_id=%s", (egid,))
                if cur.fetchone() is None:
                    return HandlerResult(status=400, body={'error': f'unknown emergency_group_id: {egid}'})
            cur.execute(
                "INSERT INTO ptt_user_profile (ptt_id, allow_emergency_call, allow_emergency_alert, "
                "allow_adhoc_call, emergency_group_mode, emergency_group_id, update_time) "
                "VALUES (%s,%s,%s,%s,%s,%s,NOW()) "
                "ON DUPLICATE KEY UPDATE allow_emergency_call=VALUES(allow_emergency_call), "
                "allow_emergency_alert=VALUES(allow_emergency_alert), "
                "allow_adhoc_call=VALUES(allow_adhoc_call), "
                "emergency_group_mode=VALUES(emergency_group_mode), "
                "emergency_group_id=VALUES(emergency_group_id), update_time=NOW()",
                (msisdn, allow_call, allow_alert, allow_adhoc, mode, egid))

    prof = {
        "allow_emergency_call": bool(allow_call),
        "allow_emergency_alert": bool(allow_alert),
        "allow_adhoc_call": bool(allow_adhoc),
        "emergency_group_mode": mode,
        "emergency_group_id": egid,
    }
    update_user_profile_cache(msisdn, prof)  # user-profile 문서 ETag 는 내용 파생 — 자동 갱신
    notify_csp("USER_CHANGED", f"tel:{msisdn}", "PUT")
    return HandlerResult(status=200, body=dict(prof, id=msisdn))


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
    "emergency_alert, "
    "allow_sds, allow_fd, max_sds_size, max_auto_recv, "
    "org_code, session_start, session_end, group_type, on_network, max_members, "
    "require_affiliation, alias, authorized_user_id, floor_policy, max_talkers, created_at"
)

# floor 동시 발언 정책 (mcptt_csp_cmp_roadmap_contract.md §B.1) — CSP 가 CMP 로 발행한다.
_FLOOR_POLICIES = ('single', 'dual', 'multi')
_MAX_TALKERS_MIN = 2      # multi 의 하한 (dual 은 정원 2 고정, 값은 무시)
_MAX_TALKERS_MAX = 8      # CMP 슬롯 상한 (MCPTT_MAX_TALKER_SLOTS)


def _norm_floor(policy, talkers, cur_policy='single', cur_talkers=2):
    """floor_policy/max_talkers 정규화. multi 는 정원이 범위 밖이면 거절한다 —
    CMP 가 BAD_REQUEST 로 그룹 생성을 거부해 통화 불가가 되므로 저장 단계에서 막는다."""
    p = (policy or cur_policy or 'single').strip().lower()
    if p not in _FLOOR_POLICIES:
        return None, None, f"floor_policy must be one of {'|'.join(_FLOOR_POLICIES)}"
    try:
        n = int(talkers if talkers is not None else cur_talkers)
    except (TypeError, ValueError):
        return None, None, 'max_talkers must be an integer'
    if p == 'multi' and not (_MAX_TALKERS_MIN <= n <= _MAX_TALKERS_MAX):
        return None, None, f'max_talkers must be {_MAX_TALKERS_MIN}..{_MAX_TALKERS_MAX} for floor_policy=multi'
    if p != 'multi':
        n = 2      # single/dual 은 정원을 해석하지 않는다 — 기본값으로 정규화
    return p, n, None


def _shape_group(g: dict, members: list, owner: dict = None):
    """DB row → API 형태. id(응답)=mcptt_group_id, db_id=surrogate.

    owner: {authorized_user(MCPTT ID), authorized_user_name} — authorized_user_id 파생값.
    """
    g['db_id'] = g['id']
    g['id'] = g.get('mcptt_group_id') or str(g['db_id'])
    g['video_enabled'] = bool(g.get('video_enabled', 0))
    g['encryption'] = bool(g.get('encryption', 0))
    g['emergency_call'] = bool(g.get('emergency_call', 0))
    g['emergency_alert'] = bool(g.get('emergency_alert', 1))
    g['allow_sds'] = bool(g.get('allow_sds', 1))
    g['allow_fd'] = bool(g.get('allow_fd', 0))
    g['max_sds_size'] = int(g.get('max_sds_size', 10000) or 0)
    g['max_auto_recv'] = int(g.get('max_auto_recv', 1048576) or 0)
    g['on_network'] = bool(g.get('on_network', 1))
    g['require_affiliation'] = bool(g.get('require_affiliation', 1))
    g['floor_policy'] = g.get('floor_policy') or 'single'
    g['max_talkers'] = int(g.get('max_talkers', 2) or 2)
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
    # 접두사 예약 — CSP 가 즉석 세션 ID 로 사용(adhoc-=애드혹 임시그룹, priv-=1:1 합성그룹).
    #   편성 그룹이 이 접두사를 쓰면 즉석 세션 라우팅과 충돌한다.
    if group_id.startswith(('adhoc-', 'priv-')):
        return HandlerResult(status=400,
                             body={'error': "group id 접두사 'adhoc-'/'priv-' 는 즉석 세션용 예약어입니다"})
    name           = body.get('name', group_id)
    video_enabled  = 1 if body.get('video_enabled', False) else 0
    priority       = int(body.get('priority', 5))
    encryption     = 1 if body.get('encryption', False) else 0
    emergency_call = 1 if body.get('emergency_call', False) else 0
    emergency_alert     = 1 if body.get('emergency_alert', True) else 0
    allow_sds           = 1 if body.get('allow_sds', True) else 0
    allow_fd            = 1 if body.get('allow_fd', False) else 0
    max_sds_size        = int(body.get('max_sds_size', 10000))
    max_auto_recv       = int(body.get('max_auto_recv', 1048576))
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
    floor_policy, max_talkers, floor_err = _norm_floor(body.get('floor_policy'), body.get('max_talkers'))
    if floor_err:
        return HandlerResult(status=400, body={'error': floor_err})

    # 그룹 소유 (authorized user) — 명시 없으면 생성자(payload sub) 기본.
    # 단 OAM builtin 관리자(CimsAuth, sub<0)는 users 행이 아니다 — 소유자로
    # 넣으면 FK(fk_grp_authorized_user) 위반. 소유자 미지정 그룹으로 생성한다.
    authorized_user_id = body.get('authorized_user_id')
    explicit_owner = authorized_user_id is not None
    if not explicit_owner and payload is not None:
        try:
            authorized_user_id = int(payload.get('sub'))
        except (TypeError, ValueError):
            authorized_user_id = None
        if payload.get('builtin') or (authorized_user_id is not None
                                      and authorized_user_id < 0):
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
                "emergency_call, emergency_alert, "
                "allow_sds, allow_fd, max_sds_size, max_auto_recv, "
                "org_code, session_start, session_end, group_type, on_network, "
                "max_members, require_affiliation, alias, authorized_user_id, "
                "floor_policy, max_talkers) "
                "VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)",
                (group_id, name, video_enabled, priority, encryption,
                 emergency_call, emergency_alert,
                 allow_sds, allow_fd, max_sds_size, max_auto_recv,
                 org_code, session_start, session_end, group_type,
                 on_network, max_members, require_affiliation, alias, authorized_user_id,
                 floor_policy, max_talkers)
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
            for fld in ('priority', 'max_members', 'max_sds_size', 'max_auto_recv'):
                if fld in body:
                    update_fields.append(f'{fld}=%s')
                    update_vals.append(int(body[fld]))
            for fld in ('encryption', 'emergency_call', 'emergency_alert',
                        'on_network', 'require_affiliation',
                        'allow_sds', 'allow_fd'):
                if fld in body:
                    update_fields.append(f'{fld}=%s')
                    update_vals.append(1 if body[fld] else 0)
            if 'group_type' in body and body['group_type'] in ('prearranged', 'chat', 'broadcast'):
                update_fields.append('group_type=%s')
                update_vals.append(body['group_type'])
            # floor 동시 발언 정책 — 한 축만 보내도 나머지는 현재 값을 기준으로 검증한다.
            if 'floor_policy' in body or 'max_talkers' in body:
                cur.execute("SELECT floor_policy, max_talkers FROM ptt_groups WHERE id=%s", (gpk,))
                cur_row = cur.fetchone() or {}
                fp, mt, err = _norm_floor(
                    body.get('floor_policy'), body.get('max_talkers'),
                    cur_row.get('floor_policy') or 'single', cur_row.get('max_talkers') or 2)
                if err:
                    return HandlerResult(status=400, body={'error': err})
                update_fields.append('floor_policy=%s'); update_vals.append(fp)
                update_fields.append('max_talkers=%s'); update_vals.append(mt)
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


# ── API 문서 (개발자 모드) ──────────────────────────────────────────────────
#  이 모듈이 제공하는 엔드포인트의 자기기술. OAM 의 handlers/api_docs.py 가 수집한다.
#  이 파일은 csc 모듈 배포 시 OAM handlers/ 에 설치되므로, csc 미설치 환경에서는 수집에서 자연히
#  빠진다(그 API 가 실제로 없으므로). 경로/파라미터를 바꾸면 **여기도 같은 커밋에서** 갱신한다.
_AUTH_MONITOR = {'scheme': 'bearer', 'role': 'monitor', 'token_from': 'POST /api/v1/auth/login'}
_AUTH_MANAGER = {'scheme': 'bearer', 'role': 'manager', 'token_from': 'POST /api/v1/auth/login'}

_ERR_COMMON = [
    {'status': 401, 'when': 'Authorization 헤더 없음 / 토큰 만료', 'body': {'error': 'unauthorized'}},
    {'status': 403, 'when': '권한 등급 미달', 'body': {'error': 'forbidden'}},
]

_USER_FIELDS = [
    {'name': 'id', 'type': 'integer', 'desc': '가입자(person) id — 이후 경로의 {person_id}'},
    {'name': 'name', 'type': 'string', 'desc': '이름'},
    {'name': 'title', 'type': 'string', 'desc': '직함 (그룹문서 확장으로 단말에 전달)'},
    {'name': 'login_id', 'type': 'string', 'desc': '단말/IdMS 로그인 ID (콘솔 계정과 별개)'},
    {'name': 'org_id', 'type': 'string', 'desc': '소속 조직 코드'},
    {'name': 'email', 'type': 'string', 'desc': '이메일 (스키마에 컬럼이 없으면 생략됨)'},
    {'name': 'details', 'type': 'string', 'desc': '비고'},
    {'name': 'reject_id[]', 'type': 'string', 'desc': '착신 거부 번호 목록'},
    {'name': 'call_subscriptions[]', 'type': 'object', 'desc': 'VoLTE 번호 목록 (아래 가입 필드)'},
    {'name': 'ptt_subscriptions[]', 'type': 'object', 'desc': 'PTT 번호 목록 (아래 가입 필드)'},
    {'name': '*_subscriptions[].id', 'type': 'string', 'desc': '번호(MSISDN)'},
    {'name': '*_subscriptions[].imsi', 'type': 'string', 'desc': 'SIM IMSI — 인증 username 의 user 파트'},
    {'name': '*_subscriptions[].service_ref', 'type': 'string', 'desc': '소속 서비스명 (도메인 결정)'},
    {'name': '*_subscriptions[].dnd', 'type': 'boolean', 'desc': '방해금지'},
    {'name': '*_subscriptions[].forward_id', 'type': 'string', 'desc': '착신전환 대상'},
    {'name': '*_subscriptions[].register_time', 'type': 'string', 'desc': 'ISO8601 최근 등록 시각'},
    {'name': '*_subscriptions[].logout_time', 'type': 'string', 'desc': 'ISO8601 최근 로그아웃 시각'},
    {'name': 'create_time', 'type': 'string', 'desc': 'ISO8601 생성'},
    {'name': 'update_time', 'type': 'string', 'desc': 'ISO8601 수정'},
]

_USER_EXAMPLE = {
    'id': 11, 'name': '홍길동', 'title': '팀장', 'login_id': 'test001', 'org_id': 'D110',
    'email': 'gildong@example.com', 'details': '', 'reject_id': [],
    'call_subscriptions': [{'id': '01000000001', 'imsi': '450050000000001',
                            'service_ref': 'volte', 'dnd': False, 'forward_id': '',
                            'register_time': '2026-07-30T08:40:11', 'logout_time': None}],
    'ptt_subscriptions': [{'id': '01000000001', 'imsi': '450050000000001',
                           'service_ref': 'mcptt', 'dnd': False, 'forward_id': '',
                           'register_time': '2026-07-30T08:40:12', 'logout_time': None}],
    'create_time': '2026-05-02T10:00:00', 'update_time': '2026-07-29T17:20:00',
}

_GROUP_FIELDS = [
    {'name': 'id', 'type': 'string', 'desc': 'MCPTT 그룹 ID (응답에서 id 는 mcptt_group_id 로 노출)'},
    {'name': 'name', 'type': 'string', 'desc': '그룹명'},
    {'name': 'alias', 'type': 'string', 'desc': '별칭'},
    {'name': 'org_code', 'type': 'string', 'desc': '소속 조직 코드'},
    {'name': 'group_type', 'type': 'string', 'desc': '그룹 종류'},
    {'name': 'priority', 'type': 'integer', 'desc': '그룹 우선순위'},
    {'name': 'video_enabled', 'type': 'boolean', 'desc': '영상 허용'},
    {'name': 'encryption', 'type': 'boolean', 'desc': '암호화 사용'},
    {'name': 'emergency_call', 'type': 'boolean', 'desc': '긴급 통화 허용'},
    {'name': 'emergency_alert', 'type': 'boolean', 'desc': '긴급 알림 허용'},
    {'name': 'allow_sds', 'type': 'boolean', 'desc': 'MCData 텍스트(SDS) 허용'},
    {'name': 'allow_fd', 'type': 'boolean', 'desc': '파일 전송(FD) 허용'},
    {'name': 'max_sds_size', 'type': 'integer', 'unit': 'byte', 'desc': 'SDS 최대 크기'},
    {'name': 'max_auto_recv', 'type': 'integer', 'unit': 'byte', 'desc': '자동 수신 최대 크기'},
    {'name': 'on_network', 'type': 'boolean', 'desc': 'on-network 그룹 여부'},
    {'name': 'max_members', 'type': 'integer', 'unit': '명', 'desc': '정원'},
    {'name': 'require_affiliation', 'type': 'boolean', 'desc': 'affiliation 필수'},
    {'name': 'authorized_user_id', 'type': 'string', 'desc': '그룹 권한 사용자 (PTT 가입자여야 함)'},
    {'name': 'floor_policy', 'type': 'string', 'enum': ['single', 'dual', 'multi'],
     'desc': '동시 발언 정책'},
    {'name': 'max_talkers', 'type': 'integer', 'unit': '명',
     'desc': 'multi 정원 (2~8). dual 은 2 고정이라 값 무시'},
    {'name': 'session_start', 'type': 'string', 'desc': '세션 허용 시작 시각'},
    {'name': 'session_end', 'type': 'string', 'desc': '세션 허용 종료 시각'},
    {'name': 'created_at', 'type': 'string', 'desc': 'ISO8601 생성'},
]

_GROUP_EXAMPLE = {
    'id': 'g-ops-1', 'name': '운영1팀', 'alias': 'OPS1', 'org_code': 'D110',
    'group_type': 'normal', 'priority': 5, 'video_enabled': False, 'encryption': True,
    'emergency_call': True, 'emergency_alert': True, 'allow_sds': True, 'allow_fd': True,
    'max_sds_size': 4096, 'max_auto_recv': 1048576, 'on_network': True, 'max_members': 50,
    'require_affiliation': False, 'authorized_user_id': '01000000003',
    'floor_policy': 'single', 'max_talkers': 0,
    'session_start': None, 'session_end': None, 'created_at': '2026-05-10T09:00:00',
}

CIMS_ADMIN_API_DOCS = [
    {'id': 'csc.users.list', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/users',
     'summary': '가입자(person) 목록 — VoLTE/PTT 번호 포함. 비밀번호는 미포함',
     'params': [],
     'response': '{users[]}',
     'response_fields': [{'name': 'users[].' + f['name'], **{k: v for k, v in f.items() if k != 'name'}}
                         for f in _USER_FIELDS],
     'example': {'users': [_USER_EXAMPLE]},
     'errors': list(_ERR_COMMON),
     'notes': ['가입자가 없으면 {"users": []} 를 반환한다.',
               'passwd 는 어떤 응답에도 포함되지 않는다 (입력 전용).',
               'email/title 컬럼이 없는 구 스키마에서는 그 키가 생략된다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'csc.users.get', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/users/{person_id}',
     'summary': '가입자 1건 상세 (목록 항목과 동일 구조)',
     'params': [{'name': 'person_id', 'in': 'path', 'type': 'integer', 'required': True, 'desc': '가입자 id'}],
     'response': '가입자 객체 (users[] 항목과 동일)',
     'response_fields': list(_USER_FIELDS),
     'example': dict(_USER_EXAMPLE),
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 id', 'body': {'error': 'User not found'}}],
     'notes': ['person_id=me 는 별도 핸들러(본인 리소스)로 위임된다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'csc.users.create', 'module': 'csc', 'method': 'POST', 'path': '/api/v1/users',
     'summary': '가입자 생성 (번호는 별도 API 로 추가)',
     'params': [{'name': 'body', 'in': 'body', 'type': 'object', 'required': True,
                 'desc': '{name(필수), org_id, title?, email?, details?, login_id?, passwd?, reject_id?[]}'}],
     'response': '{id}',
     'response_fields': [{'name': 'id', 'type': 'integer', 'desc': '생성된 가입자 id'}],
     'example': {'id': 12},
     'errors': _ERR_COMMON + [
         {'status': 400, 'when': 'JSON 본문 없음', 'body': {'error': 'JSON body required'}},
         {'status': 400, 'when': 'name 누락', 'body': {'error': 'name is required'}},
     ],
     'notes': ['성공 시 **201** 을 반환한다 (200 아님).',
               'passwd 는 단말/IdMS 로그인 자격이다 — 콘솔 계정과 다르다.'],
     'auth': dict(_AUTH_MANAGER)},

    {'id': 'csc.users.update', 'module': 'csc', 'method': 'PUT', 'path': '/api/v1/users/{person_id}',
     'summary': '가입자 수정 (전달한 필드만 변경)',
     'params': [
         {'name': 'person_id', 'in': 'path', 'type': 'integer', 'required': True, 'desc': '가입자 id'},
         {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '변경할 필드만'},
     ],
     'response': '{id}',
     'response_fields': [{'name': 'id', 'type': 'integer', 'desc': '수정된 가입자 id'}],
     'example': {'id': 11},
     'errors': _ERR_COMMON + [
         {'status': 400, 'when': 'JSON 본문 없음', 'body': {'error': 'JSON body required'}},
         {'status': 400, 'when': '변경 가능한 필드가 하나도 없음',
          'body': {'error': 'No updatable fields provided'}},
         {'status': 404, 'when': '없는 id', 'body': {'error': 'User not found'}},
     ],
     'notes': ['passwd 는 변경할 때만 전송한다 (미전송 시 유지).'],
     'auth': dict(_AUTH_MANAGER)},

    {'id': 'csc.users.delete', 'module': 'csc', 'method': 'DELETE', 'path': '/api/v1/users/{person_id}',
     'summary': '가입자 삭제 (번호·거부목록 함께 정리)',
     'params': [{'name': 'person_id', 'in': 'path', 'type': 'integer', 'required': True, 'desc': '가입자 id'}],
     'response': '{id}',
     'response_fields': [{'name': 'id', 'type': 'integer', 'desc': '삭제된 가입자 id'}],
     'example': {'id': 11},
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 id', 'body': {'error': 'User not found'}}],
     'notes': ['등록 중 단말이 있어도 삭제된다 — 이후 재등록이 거부된다.'],
     'auth': dict(_AUTH_MANAGER)},

    {'id': 'csc.users.batch-delete', 'module': 'csc', 'method': 'DELETE', 'path': '/api/v1/users/batch',
     'summary': '가입자 일괄 삭제 (부분 성공 허용)',
     'params': [{'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '{ids: [정수]}'}],
     'response': '{deleted, errors[]}',
     'response_fields': [
         {'name': 'deleted', 'type': 'integer', 'unit': '건', 'desc': '삭제 성공 건수'},
         {'name': 'errors[]', 'type': 'object', 'desc': '실패 항목 — {id, error}'},
     ],
     'example': {'deleted': 2, 'errors': [{'id': 99, 'error': 'User not found'}]},
     'errors': _ERR_COMMON + [
         {'status': 400, 'when': 'ids 누락', 'body': {'error': 'ids 필드가 필요합니다'}},
     ],
     'notes': ['일부 실패해도 200 이다 — **errors 배열을 반드시 확인**해야 한다.',
               '경로가 /users/{person_id} 와 겹치므로 id 자리에 batch 라는 예약어를 쓴다.'],
     'auth': dict(_AUTH_MANAGER)},

    {'id': 'csc.users.subs.list', 'module': 'csc', 'method': 'GET',
     'path': '/api/v1/users/{person_id}/{kind}',
     'summary': '가입자의 번호(가입) 목록',
     'params': [
         {'name': 'person_id', 'in': 'path', 'type': 'integer', 'required': True, 'desc': '가입자 id'},
         {'name': 'kind', 'in': 'path', 'type': 'string', 'required': True,
          'enum': ['call', 'ptt'], 'desc': '가입 종류 — call = VoLTE'},
     ],
     'response': '{subscriptions[]}',
     'response_fields': [
         {'name': 'subscriptions[].id', 'type': 'string', 'desc': '번호(MSISDN)'},
         {'name': 'subscriptions[].imsi', 'type': 'string', 'desc': 'SIM IMSI'},
         {'name': 'subscriptions[].service_ref', 'type': 'string', 'desc': '소속 서비스명'},
         {'name': 'subscriptions[].dnd', 'type': 'boolean', 'desc': '방해금지'},
         {'name': 'subscriptions[].forward_id', 'type': 'string', 'desc': '착신전환 대상'},
         {'name': 'subscriptions[].register_time', 'type': 'string', 'desc': '최근 등록 시각'},
         {'name': 'subscriptions[].logout_time', 'type': 'string', 'desc': '최근 로그아웃 시각'},
     ],
     'example': {'subscriptions': [{'id': '01000000001', 'imsi': '450050000000001',
                                    'service_ref': 'volte', 'dnd': False, 'forward_id': '',
                                    'register_time': '2026-07-30T08:40:11', 'logout_time': None}]},
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 가입자', 'body': {'error': 'User not found'}}],
     'notes': ['**경로가 `/subscriptions` 가 아니라 `/call` · `/ptt` 다.**',
               '등록 여부는 register_time > logout_time 인지로 판단한다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'csc.users.subs.add', 'module': 'csc', 'method': 'POST',
     'path': '/api/v1/users/{person_id}/{kind}',
     'summary': '가입자에 번호 추가 (IMSI 필수)',
     'params': [
         {'name': 'person_id', 'in': 'path', 'type': 'integer', 'required': True, 'desc': '가입자 id'},
         {'name': 'kind', 'in': 'path', 'type': 'string', 'required': True,
          'enum': ['call', 'ptt'], 'desc': '가입 종류'},
         {'name': 'body', 'in': 'body', 'type': 'object', 'required': True,
          'desc': '{id(MSISDN, 필수), imsi(필수), service_ref?, dnd?, forward_id?}'},
     ],
     'response': '{id}',
     'response_fields': [{'name': 'id', 'type': 'string', 'desc': '추가된 번호(MSISDN)'}],
     'example': {'id': '01000000001'},
     'errors': _ERR_COMMON + [
         {'status': 400, 'when': 'JSON 본문 없음', 'body': {'error': 'JSON body required'}},
         {'status': 400, 'when': 'id 누락', 'body': {'error': 'id (MSISDN) is required'}},
         {'status': 400, 'when': 'imsi 누락', 'body': {'error': 'imsi required'}},
         {'status': 404, 'when': '없는 가입자', 'body': {'error': 'User not found'}},
     ],
     'notes': ['성공 시 **201** 이다.', 'imsi 는 SIP 인증 username 의 user 파트로 쓰여 필수다.'],
     'auth': dict(_AUTH_MANAGER)},

    {'id': 'csc.users.subs.update', 'module': 'csc', 'method': 'PUT',
     'path': '/api/v1/users/{person_id}/{kind}/{msisdn}',
     'summary': '번호 설정 변경 (DND/착신전환/서비스 소속 등)',
     'params': [
         {'name': 'person_id', 'in': 'path', 'type': 'integer', 'required': True, 'desc': '가입자 id'},
         {'name': 'kind', 'in': 'path', 'type': 'string', 'required': True,
          'enum': ['call', 'ptt'], 'desc': '가입 종류'},
         {'name': 'msisdn', 'in': 'path', 'type': 'string', 'required': True,
          'desc': '번호 (+ 로 시작하면 URL-encode)'},
         {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '변경할 필드만'},
     ],
     'response': '{id}',
     'response_fields': [{'name': 'id', 'type': 'string', 'desc': '변경된 번호'}],
     'example': {'id': '01000000001'},
     'errors': _ERR_COMMON + [
         {'status': 400, 'when': 'JSON 본문 없음', 'body': {'error': 'JSON body required'}},
         {'status': 404, 'when': '없는 가입자/번호'},
     ],
     'notes': ['dnd 는 "Y"/"1"/"true"/"on" 같은 문자열도 참으로 해석된다 ("false"/"0" 은 거짓).'],
     'auth': dict(_AUTH_MANAGER)},

    {'id': 'csc.users.subs.delete', 'module': 'csc', 'method': 'DELETE',
     'path': '/api/v1/users/{person_id}/{kind}/{msisdn}',
     'summary': '번호 삭제',
     'params': [
         {'name': 'person_id', 'in': 'path', 'type': 'integer', 'required': True, 'desc': '가입자 id'},
         {'name': 'kind', 'in': 'path', 'type': 'string', 'required': True,
          'enum': ['call', 'ptt'], 'desc': '가입 종류'},
         {'name': 'msisdn', 'in': 'path', 'type': 'string', 'required': True, 'desc': '번호'},
     ],
     'response': '{id}',
     'response_fields': [{'name': 'id', 'type': 'string', 'desc': '삭제된 번호'}],
     'example': {'id': '01000000001'},
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 가입자/번호'}],
     'notes': ['그룹 구성원으로 참여 중인 PTT 번호를 지우면 그룹 멤버십도 정리된다.'],
     'auth': dict(_AUTH_MANAGER)},

    # ── PTT 그룹 ────────────────────────────────────────────────────────────
    {'id': 'csc.ptt-groups.list', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/ptt/groups',
     'summary': 'PTT 그룹 목록 (id = mcptt_group_id)',
     'params': [],
     'response': '{groups[]}',
     'response_fields': [{'name': 'groups[].' + f['name'], **{k: v for k, v in f.items() if k != 'name'}}
                         for f in _GROUP_FIELDS],
     'example': {'groups': [_GROUP_EXAMPLE]},
     'errors': list(_ERR_COMMON),
     'notes': ['mcptt_group_id 오름차순이다.',
               'DB 의 surrogate id 는 응답에 노출되지 않는다 — id 는 항상 mcptt_group_id.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'csc.ptt-groups.get', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/ptt/groups/{group_id}',
     'summary': 'PTT 그룹 1건 상세',
     'params': [{'name': 'group_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': 'MCPTT 그룹 ID'}],
     'response': '그룹 객체 (groups[] 항목과 동일)',
     'response_fields': list(_GROUP_FIELDS),
     'example': dict(_GROUP_EXAMPLE),
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 그룹', 'body': {'error': 'Group not found'}}],
     'notes': [],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'csc.ptt-groups.create', 'module': 'csc', 'method': 'POST', 'path': '/api/v1/ptt/groups',
     'summary': 'PTT 그룹 생성',
     'params': [{'name': 'body', 'in': 'body', 'type': 'object', 'required': True,
                 'desc': "{id(mcptt_group_id, 필수), name, org_code, floor_policy?, max_talkers?, "
                         "authorized_user_id?, allow_sds?, allow_fd?, ...}"}],
     'response': '{id}',
     'response_fields': [{'name': 'id', 'type': 'string', 'desc': '생성된 MCPTT 그룹 ID'}],
     'example': {'id': 'g-ops-3'},
     'errors': _ERR_COMMON + [
         {'status': 400, 'when': 'JSON 본문 없음', 'body': {'error': 'JSON body required'}},
         {'status': 400, 'when': 'id 누락', 'body': {'error': 'id (mcptt_group_id) is required'}},
         {'status': 400, 'when': "id 가 'adhoc-'/'priv-' 로 시작 (즉석 세션 예약어)"},
         {'status': 400, 'when': 'floor 정책/정원 조합 무효 (multi 는 2~8)'},
         {'status': 400, 'when': 'authorized_user_id 가 PTT 가입자가 아님'},
     ],
     'errors_note': '',
     'notes': ['성공 시 **201** 이다.',
               "그룹 id 접두사 'adhoc-'·'priv-' 는 즉석/1:1 세션용 예약어라 거부된다.",
               'floor_policy=multi 면 max_talkers 는 2~8 이어야 한다 (dual 은 2 고정).'],
     'auth': {'scheme': 'bearer', 'role': 'operator', 'token_from': 'POST /api/v1/auth/login'}},

    {'id': 'csc.ptt-groups.update', 'module': 'csc', 'method': 'PUT',
     'path': '/api/v1/ptt/groups/{group_id}',
     'summary': 'PTT 그룹 수정 (operator 는 본인 소유 그룹만, manager+ 는 전체)',
     'params': [
         {'name': 'group_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': 'MCPTT 그룹 ID'},
         {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '변경할 필드만'},
     ],
     'response': '{id}',
     'response_fields': [{'name': 'id', 'type': 'string', 'desc': '수정된 그룹 ID'}],
     'example': {'id': 'g-ops-1'},
     'errors': _ERR_COMMON + [
         {'status': 400, 'when': 'JSON 본문 없음 / floor 조합 무효 / authorized_user 부적격'},
         {'status': 403, 'when': 'operator 가 남의 소유 그룹을 수정 시도'},
         {'status': 404, 'when': '없는 그룹', 'body': {'error': 'Group not found'}},
     ],
     'notes': ['floor_policy·max_talkers 변경은 CSP→CMP 로 전파된다.'],
     'auth': {'scheme': 'bearer', 'role': 'operator', 'token_from': 'POST /api/v1/auth/login',
              'note': '기존 그룹 변경은 소유자 검사 — manager+ 는 전체 허용'}},

    {'id': 'csc.ptt-groups.delete', 'module': 'csc', 'method': 'DELETE',
     'path': '/api/v1/ptt/groups/{group_id}',
     'summary': 'PTT 그룹 삭제',
     'params': [{'name': 'group_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': 'MCPTT 그룹 ID'}],
     'response': '{id}',
     'response_fields': [{'name': 'id', 'type': 'string', 'desc': '삭제된 그룹 ID'}],
     'example': {'id': 'g-ops-1'},
     'errors': _ERR_COMMON + [
         {'status': 403, 'when': 'operator 가 남의 소유 그룹 삭제 시도'},
         {'status': 404, 'when': '없는 그룹', 'body': {'error': 'Group not found'}},
     ],
     'notes': ['구성원 매핑도 함께 정리된다.'],
     'auth': {'scheme': 'bearer', 'role': 'operator', 'token_from': 'POST /api/v1/auth/login',
              'note': '소유자 검사 — manager+ 는 전체 허용'}},

    {'id': 'csc.ptt-groups.members.list', 'module': 'csc', 'method': 'GET',
     'path': '/api/v1/ptt/groups/{group_id}/members',
     'summary': '그룹 구성원 목록 (priority 오름차순)',
     'params': [{'name': 'group_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': 'MCPTT 그룹 ID'}],
     'response': '{group_id, members[]}',
     'response_fields': [
         {'name': 'group_id', 'type': 'string', 'desc': '요청한 그룹'},
         {'name': 'members[].user_id', 'type': 'string', 'desc': '구성원 번호(MSISDN)'},
         {'name': 'members[].mcptt_id', 'type': 'string', 'desc': 'MCPTT ID (SIP URI 형태)'},
         {'name': 'members[].role', 'type': 'string', 'desc': '그룹 내 역할'},
         {'name': 'members[].priority', 'type': 'integer', 'desc': 'floor 우선순위 (작을수록 우선)'},
     ],
     'example': {'group_id': 'g-ops-1',
                 'members': [{'user_id': '01000000003', 'mcptt_id': 'sip:01000000003@cims.local',
                              'role': 'member', 'priority': 5}]},
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 그룹', 'body': {'error': 'Group not found'}}],
     'notes': ['실시간 참여/발언 상태는 여기 없다 — stats.service.ptt-members 를 쓴다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'csc.ptt-groups.members.add', 'module': 'csc', 'method': 'POST',
     'path': '/api/v1/ptt/groups/{group_id}/members',
     'summary': '그룹 구성원 추가',
     'params': [
         {'name': 'group_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': 'MCPTT 그룹 ID'},
         {'name': 'body', 'in': 'body', 'type': 'object', 'required': True,
          'desc': '{user_id(MSISDN, 필수), role?, priority?}'},
     ],
     'response': '{group_id, user_id}',
     'response_fields': [
         {'name': 'group_id', 'type': 'string', 'desc': '대상 그룹'},
         {'name': 'user_id', 'type': 'string', 'desc': '추가된 구성원 번호'},
     ],
     'example': {'group_id': 'g-ops-1', 'user_id': '01000000004'},
     'errors': _ERR_COMMON + [
         {'status': 400, 'when': 'user_id 누락 / PTT 가입자가 아님 / 정원 초과'},
         {'status': 403, 'when': 'operator 가 남의 소유 그룹 변경 시도'},
         {'status': 404, 'when': '없는 그룹'},
     ],
     'notes': ['추가 후 CSP 에 그룹 멤버 갱신이 통보되어 단말 그룹문서가 갱신된다.'],
     'auth': {'scheme': 'bearer', 'role': 'operator', 'token_from': 'POST /api/v1/auth/login',
              'note': '소유자 검사 — manager+ 는 전체 허용'}},

    {'id': 'csc.ptt-groups.members.remove', 'module': 'csc', 'method': 'DELETE',
     'path': '/api/v1/ptt/groups/{group_id}/members/{user_id}',
     'summary': '그룹 구성원 제거',
     'params': [
         {'name': 'group_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': 'MCPTT 그룹 ID'},
         {'name': 'user_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '구성원 번호(MSISDN)'},
     ],
     'response': '{group_id, user_id}',
     'response_fields': [
         {'name': 'group_id', 'type': 'string', 'desc': '대상 그룹'},
         {'name': 'user_id', 'type': 'string', 'desc': '제거된 구성원 번호'},
     ],
     'example': {'group_id': 'g-ops-1', 'user_id': '01000000004'},
     'errors': _ERR_COMMON + [
         {'status': 403, 'when': 'operator 가 남의 소유 그룹 변경 시도'},
         {'status': 404, 'when': '없는 그룹/구성원'},
     ],
     'notes': ['진행 중 세션에 참여하고 있으면 그 세션에서도 이탈 처리된다.'],
     'auth': {'scheme': 'bearer', 'role': 'operator', 'token_from': 'POST /api/v1/auth/login',
              'note': '소유자 검사 — manager+ 는 전체 허용'}},
]
