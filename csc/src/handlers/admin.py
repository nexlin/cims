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

import hashlib

import pymysql
import pymysql.cursors

from httpsrv.handler import HandlerArgs, HandlerResult
from services.mcptt import (notify_csp, refresh_group_members, DEFAULT_USER_PROFILE,
                            update_user_profile_cache, SERVICE_CONFIG_DEFAULTS,
                            get_service_config, update_service_config_cache,
                            get_service_config_xml)
from handlers import dispatch as _dispatch  # 관제 그룹 파생(pickup_group 409 게이트)
from services import admin_auth
from services.auc import auc as _auc
from services.mcptt import logger as _logger

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
            aka_cols = _aka_select_extra(cur)
            pickup_cols = _pickup_select_extra(cur)
            cur.execute(
                "SELECT id, user_id, service_ref, imsi, sip_transport, dnd, forward_id, register_time, logout_time "
                f"{aka_cols}{pickup_cols} FROM volte_subscriptions ORDER BY user_id, id"
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
                "SELECT id, user_id, service_ref, imsi, sip_transport, dnd, forward_id, register_time, logout_time "
                f"{aka_cols}{pickup_cols} FROM ptt_subscriptions ORDER BY user_id, id"
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
            aka_cols = _aka_select_extra(cur)
            pickup_cols = _pickup_select_extra(cur)
            cur.execute(
                "SELECT id, service_ref, imsi, sip_transport, dnd, forward_id, register_time, logout_time "
                f"{aka_cols}{pickup_cols} FROM volte_subscriptions WHERE user_id=%s ORDER BY id",
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
                "SELECT id, service_ref, imsi, sip_transport, dnd, forward_id, register_time, logout_time "
                f"{aka_cols}{pickup_cols} FROM ptt_subscriptions WHERE user_id=%s ORDER BY id",
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
                        "emergency_group_mode, emergency_group_id, allow_emergency_private_call, "
                        f"private_emergency_mode, emergency_private_recipient, {_ambient_select(cur)} "
                        f"FROM ptt_user_profile WHERE ptt_id IN ({ph})",
                        [s['id'] for s in ptt_subs])
                    for p in cur.fetchall():
                        profiles[p['ptt_id']] = {
                            'allow_emergency_call': bool(p['allow_emergency_call']),
                            'allow_emergency_alert': bool(p['allow_emergency_alert']),
                            'allow_adhoc_call': bool(p['allow_adhoc_call']),
                            'emergency_group_mode': p['emergency_group_mode'],
                            'emergency_group_id': p['emergency_group_id'],
                            'allow_emergency_private_call': bool(p['allow_emergency_private_call']),
                            'private_emergency_mode': p['private_emergency_mode'],
                            'emergency_private_recipient': p['emergency_private_recipient'],
                            'allow_ambient_listening': bool(p['allow_ambient_listening']),
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
                f"SELECT id, service_ref, imsi, sip_transport, dnd, forward_id, "
                f"       register_time, logout_time {_aka_select_extra(cur)}{_pickup_select_extra(cur)} "
                f"FROM {table} WHERE user_id=%s ORDER BY id",
                (person_id,)
            )
            subs = cur.fetchall()
            for s in subs:
                s['dnd'] = bool(s['dnd'])
                s['register_time'] = _dt(s['register_time'])
                s['logout_time']   = _dt(s['logout_time'])
    return HandlerResult(status=200, body={'subscriptions': subs})


# ── SIP Digest 인증 자료 (sip_access_security.md §4) ─────────────────────────
#   CSC 가 인증 자료의 유일한 쓰기 주체(HSS 역할). 저장 형식은 H(A1) 이고 평문 passwd 는
#   요청 본문에만 존재한다. H(A1) 은 (imsi, 서비스 domain/realm) 에 결박되므로 그 둘이 바뀌면
#   passwd 재입력 없이는 갱신할 수 없다(400).
#   passwd 컬럼은 없다(§4.7 ⑥ DROP) — ha1 컬럼이 없는(마이그레이션 미적용) DB 에서는 자격 저장을 거부한다(503).

_SIP_TRANSPORTS = ('UDP', 'TCP', 'TLS')
_HAS_HA1_COL = None   # 컬럼 프로브 캐시 (프로세스 수명). None=미확인


def _has_ha1_column(cur) -> bool:
    """subscriptions.ha1 존재 여부 — migrate_subscription_ha1.sql 미적용 DB 에서는 자격을 저장할 곳이 없다
    (평문 passwd 컬럼은 DROP 됐다, §4.7 ⑥). 한 번 확인하면 캐시한다."""
    global _HAS_HA1_COL
    if _HAS_HA1_COL is None:
        cur.execute("SHOW COLUMNS FROM volte_subscriptions LIKE 'ha1'")
        _HAS_HA1_COL = cur.fetchone() is not None
        if not _HAS_HA1_COL:
            _logger.log_warning("subscriptions.ha1 column absent — migrate_subscription_ha1.sql 미적용, SIP 자격 저장 불가")
    return _HAS_HA1_COL


_HA1_SCHEMA_ERROR = {'error': 'schema_not_migrated',
                     'detail': 'subscriptions.ha1 column absent — sql/migrate_subscription_ha1.sql not applied'}


def _service_realm(service_ref):
    """access_services.name → (domain, realm). realm = auth_realm ?? domain (CSP EffectiveRealm 과 동일).
    서비스 정의는 OAM 스토어(config cache 'service') 에 있다. 미해석이면 None."""
    if not service_ref:
        return None
    from services import config_cache as _cfg
    cache = _cfg.CONFIG_CACHE
    if cache is None:
        return None
    for r in cache.get_all('service') or []:
        if r.get('name') == service_ref:
            domain = r.get('domain') or ''
            return domain, (r.get('auth_realm') or domain)
    return None


def _digest_ha1(imsi: str, domain: str, realm: str, passwd: str) -> str:
    return hashlib.md5(f"{imsi}@{domain}:{realm}:{passwd}".encode('utf-8')).hexdigest()


# ── IMS AKA 인증 자료 (sip_access_security.md §8.2 — P3) ─────────────────────────
#   auth_scheme 'digest'(기본) | 'aka'. aka 는 k(hex32) + opc(hex32) 또는 op(hex32) 를 요청 본문으로
#   받아 AuC.Kek 로 암호화 보관한다(services/auc). 평문 K/OPc 는 어떤 응답에도 나가지 않는다 —
#   조회는 auth_scheme 과 aka_provisioned(키 보관 여부)만. 키를 바꾸면 SQN 은 0 으로 되돌린다.

_AUTH_SCHEMES = ('digest', 'aka')


def _aka_select_extra(cur) -> str:
    """목록/단건 SELECT 에 덧붙일 AKA 열 — 마이그레이션 미적용 DB 면 빈 문자열."""
    if not _auc.has_aka_columns(cur):
        return ""
    return ", auth_scheme, (k_enc<>'') AS aka_provisioned"


# ── 당겨받기 그룹 (volte_supplementary_services.md §5.1) ─────────────────────────
#   pickup_group: 같은 값끼리 당겨받기 가능. NULL/빈 값 = 미지정(CSP 는 org_id 폴백).
#   CSP 반영은 다음 REGISTER 갱신부터(등록 바인딩 스냅샷).

_HAS_PICKUP_COL = None  # 컬럼 프로브 캐시 (프로세스 수명). None=미확인

_PICKUP_SCHEMA_ERROR = {'error': 'schema_not_migrated',
                        'detail': 'subscriptions.pickup_group column absent — sql/migrate_subscription_pickup_group.sql not applied'}


def _has_pickup_column(cur) -> bool:
    """subscriptions.pickup_group 존재 여부 — migrate_subscription_pickup_group.sql. 한 번 확인 후 캐시."""
    global _HAS_PICKUP_COL
    if _HAS_PICKUP_COL is None:
        cur.execute("SHOW COLUMNS FROM volte_subscriptions LIKE 'pickup_group'")
        _HAS_PICKUP_COL = cur.fetchone() is not None
    return _HAS_PICKUP_COL


def _pickup_select_extra(cur) -> str:
    """목록/단건 SELECT 에 덧붙일 pickup_group 열 — 마이그레이션 미적용 DB 면 빈 문자열."""
    return ", COALESCE(pickup_group,'') AS pickup_group" if _has_pickup_column(cur) else ""


def _parse_pickup_group(body):
    """body.pickup_group → str|None. 빈 값/공백 = None(그룹 해제 — CSP 는 org 폴백)."""
    v = body.get('pickup_group')
    if v is None:
        return None
    v = str(v).strip()
    return v or None


def _parse_auth_scheme(body):
    """body.auth_scheme → 'digest'|'aka'|None(미지정). 잘못된 값은 ValueError."""
    v = body.get('auth_scheme')
    if v in (None, ''):
        return None
    v = str(v).strip().lower()
    if v not in _AUTH_SCHEMES:
        raise ValueError(v)
    return v


def _aka_fields(cur, body, scheme, stored_has_keys: bool):
    """POST/PUT 공통 — AKA 컬럼 (fields, values) 또는 오류 HandlerResult.

    scheme=='aka' 인데 저장 키도 본문 키도 없으면 400. 키가 본문에 오면 sqn=0 리셋.
    컬럼 부재(마이그레이션 미적용) DB 에서 aka 를 요구하면 400.
    """
    fields, values = [], []
    k_hex, opc_hex, op_hex = body.get('k'), body.get('opc'), body.get('op')
    has_body_keys = bool(k_hex or opc_hex or op_hex)
    if scheme is None and not has_body_keys and 'amf' not in body:
        return fields, values
    if not _auc.has_aka_columns(cur):
        return HandlerResult(status=400, body={'error': 'AKA columns absent — sql/migrate_subscription_aka.sql not applied'})
    if scheme is not None:
        fields.append("auth_scheme=%s"); values.append(scheme)
    if has_body_keys:
        try:
            k_enc, opc_enc = _auc.provision_keys(k_hex or '', opc_hex or '', op_hex or '')
        except _auc.AucError as e:
            return HandlerResult(status=e.status, body={'error': e.code, 'detail': e.detail})
        fields += ["k_enc=%s", "opc_enc=%s", "sqn=%s"]; values += [k_enc, opc_enc, 0]
    if 'amf' in body:
        try:
            fields.append("amf=%s"); values.append(_auc.parse_amf(body.get('amf')))
        except _auc.AucError as e:
            return HandlerResult(status=e.status, body={'error': e.code, 'detail': e.detail})
    if scheme == 'aka' and not has_body_keys and not stored_has_keys:
        return HandlerResult(status=400, body={'error': 'k and opc (or op) required for auth_scheme=aka'})
    return fields, values


def _parse_sip_transport(body):
    """body.sip_transport → 'UDP'|'TCP'|'TLS'|None. 잘못된 값은 ValueError."""
    v = body.get('sip_transport')
    if v in (None, ''):
        return None
    v = str(v).strip().upper()
    if v not in _SIP_TRANSPORTS:
        raise ValueError(v)
    return v


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
    passwd     = body.get('passwd', '') or ''
    try:
        sip_transport = _parse_sip_transport(body)
    except ValueError:
        return HandlerResult(status=400, body={'error': 'sip_transport must be one of UDP/TCP/TLS'})
    dnd        = _coerce_dnd(body.get('dnd', False))
    forward_id = body.get('forward_id', '')
    table      = _sub_table(svc)
    try:
        auth_scheme = _parse_auth_scheme(body)
    except ValueError:
        return HandlerResult(status=400, body={'error': 'auth_scheme must be one of digest/aka'})

    ha1 = ''
    if passwd:
        realm = _service_realm(service_ref)
        if realm is None:
            return HandlerResult(status=400, body={'error': 'service_ref required to derive ha1 (unknown service)'})
        ha1 = _digest_ha1(imsi, realm[0], realm[1], passwd)

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT id FROM users WHERE id=%s", (person_id,))
            if cur.fetchone() is None:
                return HandlerResult(status=404, body={'error': 'User not found'})
            aka = _aka_fields(cur, body, auth_scheme, stored_has_keys=False)
            if isinstance(aka, HandlerResult):
                return aka
            aka_cols = ''.join(', ' + f.split('=')[0] for f in aka[0])
            aka_ph = ',%s' * len(aka[1])
            pickup_col, pickup_vals = '', []
            if 'pickup_group' in body:
                if not _has_pickup_column(cur):
                    return HandlerResult(status=400, body=_PICKUP_SCHEMA_ERROR)
                pickup_col, pickup_vals = ', pickup_group', [_parse_pickup_group(body)]
            if not _has_ha1_column(cur):
                return HandlerResult(status=503, body=_HA1_SCHEMA_ERROR)
            cur.execute(
                f"INSERT INTO {table} (id, user_id, service_ref, imsi, ha1, sip_transport, dnd, forward_id"
                f"{pickup_col}{aka_cols}) "
                f"VALUES (%s,%s,%s,%s,%s,%s,%s,%s{',%s' * len(pickup_vals)}{aka_ph})",
                (msisdn, person_id, service_ref, imsi, ha1, sip_transport, dnd, forward_id, *pickup_vals, *aka[1])
            )
    notify_csp("USER_CHANGED", f"tel:{msisdn}", "POST")
    return HandlerResult(status=201, body={'id': msisdn})


async def _update_subscription(person_id: str, svc: str, msisdn: str, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})

    passwd     = body.get('passwd') or ''
    dnd        = _coerce_dnd(body.get('dnd', False))
    forward_id = body.get('forward_id', '')
    table      = _sub_table(svc)
    try:
        sip_transport = _parse_sip_transport(body)
    except ValueError:
        return HandlerResult(status=400, body={'error': 'sip_transport must be one of UDP/TCP/TLS'})
    try:
        auth_scheme = _parse_auth_scheme(body)
    except ValueError:
        return HandlerResult(status=400, body={'error': 'auth_scheme must be one of digest/aka'})

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            ha1_col = ", ha1" if _has_ha1_column(cur) else ""
            cur.execute(f"SELECT imsi, service_ref{ha1_col} {_aka_select_extra(cur)} FROM {table} WHERE id=%s AND user_id=%s",
                        (msisdn, person_id))
            cur_row = cur.fetchone()
            if cur_row is None:
                return HandlerResult(status=404, body={'error': 'Subscription not found'})
            aka = _aka_fields(cur, body, auth_scheme, stored_has_keys=bool(cur_row.get('aka_provisioned')))
            if isinstance(aka, HandlerResult):
                return aka

            # 부분 업데이트 — service_ref/imsi 는 키가 있을 때만, passwd 는 값이 있을 때만 반영 (P1-d)
            new_imsi = cur_row['imsi']
            new_ref  = cur_row['service_ref']
            fields = ["dnd=%s", "forward_id=%s"]
            values = [dnd, forward_id]
            if 'service_ref' in body:
                sid = body.get('service_ref')
                new_ref = None if sid in (None, '', 0, '0') else str(sid).strip()
                fields.append("service_ref=%s"); values.append(new_ref)
            if 'imsi' in body:
                new_imsi = (body.get('imsi') or '').strip() or None
                fields.append("imsi=%s"); values.append(new_imsi)
            if 'sip_transport' in body:
                fields.append("sip_transport=%s"); values.append(sip_transport)
            if 'pickup_group' in body:
                if not _has_pickup_column(cur):
                    return HandlerResult(status=400, body=_PICKUP_SCHEMA_ERROR)
                # 관제 그룹 소속 가입자의 pickup_group 은 멤버십에서 파생된다 — 직접 편집 409 (dispatch_center.md §3.2)
                dg = _dispatch.dispatch_group_of_user(cur, msisdn)
                if dg is not None and _parse_pickup_group(body) != dg:
                    return HandlerResult(status=409, body={'error': 'derived_from_dispatch_group', 'group_id': dg,
                                                           'detail': 'pickup_group 은 관제 그룹 멤버십(/api/v1/dispatch-groups)에서 파생된다'})
                fields.append("pickup_group=%s"); values.append(_parse_pickup_group(body))

            # H(A1) 결박 — imsi/service_ref 가 바뀌면 기존 ha1 은 무효다. 서버는 원문을 모르므로
            #   passwd 동시 입력을 요구한다 (§4.3).
            binding_changed = (new_imsi != cur_row['imsi']) or (new_ref != cur_row['service_ref'])
            if binding_changed and not passwd:
                return HandlerResult(status=400, body={'error': 'passwd required when imsi or service_ref changes (ha1 rebinding)'})
            # 체계 전환 검증 — aka→digest 는 Digest 자격(H(A1))이 있어야 한다 (§8.2). AKA 가입자는
            #   ha1 이 비어 있을 수 있어(자격 = K/OPc 뿐), passwd 없이 전환하면 등록 불가가 된다.
            if auth_scheme == 'digest' and (cur_row.get('auth_scheme') or 'digest') == 'aka' \
                    and not passwd and not (cur_row.get('ha1') or ''):
                return HandlerResult(status=400, body={'error': 'passwd required when switching auth_scheme to digest (no stored ha1)'})
            if passwd:
                if not new_imsi:
                    return HandlerResult(status=400, body={'error': 'imsi required to derive ha1'})
                realm = _service_realm(new_ref)
                if realm is None:
                    return HandlerResult(status=400, body={'error': 'service_ref required to derive ha1 (unknown service)'})
                if not _has_ha1_column(cur):
                    return HandlerResult(status=503, body=_HA1_SCHEMA_ERROR)
                fields.append("ha1=%s"); values.append(_digest_ha1(new_imsi, realm[0], realm[1], passwd))
            fields += aka[0]; values += aka[1]
            values.extend([msisdn, person_id])

            cur.execute(
                f"UPDATE {table} SET {', '.join(fields)} "
                f"WHERE id=%s AND user_id=%s",
                values
            )
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

_PROFILE_BOOL_FIELDS = ('allow_emergency_call', 'allow_emergency_alert', 'allow_adhoc_call',
                        'allow_emergency_private_call', 'allow_ambient_listening')

_HAS_AMBIENT_COL = None  # ptt_user_profile.allow_ambient_listening 프로브 캐시 (migrate_ptt_ambient_listening.sql)


def _has_ambient_column(cur) -> bool:
    global _HAS_AMBIENT_COL
    if _HAS_AMBIENT_COL is None:
        cur.execute("SHOW COLUMNS FROM ptt_user_profile LIKE 'allow_ambient_listening'")
        _HAS_AMBIENT_COL = cur.fetchone() is not None
    return _HAS_AMBIENT_COL


def _ambient_select(cur) -> str:
    """SELECT 열 — 컬럼 부재 시 상수 0 (자격 없음, dispatch_center.md §5.6)."""
    return "allow_ambient_listening" if _has_ambient_column(cur) else "0 AS allow_ambient_listening"


async def _get_ptt_profile(person_id: str, msisdn: str, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT 1 FROM ptt_subscriptions WHERE id=%s AND user_id=%s", (msisdn, person_id))
            if cur.fetchone() is None:
                return HandlerResult(status=404, body={'error': 'Subscription not found'})
            cur.execute(
                "SELECT allow_emergency_call, allow_emergency_alert, allow_adhoc_call, "
                "emergency_group_mode, emergency_group_id, "
                "allow_emergency_private_call, private_emergency_mode, emergency_private_recipient, "
                f"{_ambient_select(cur)} "
                "FROM ptt_user_profile WHERE ptt_id=%s", (msisdn,))
            row = cur.fetchone()
    if row:
        prof = {k: bool(row[k]) for k in _PROFILE_BOOL_FIELDS}
        prof['emergency_group_mode'] = row['emergency_group_mode']
        prof['emergency_group_id'] = row['emergency_group_id']
        prof['private_emergency_mode'] = row['private_emergency_mode']
        prof['emergency_private_recipient'] = row['emergency_private_recipient']
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
    pmode = (body.get('private_emergency_mode') or 'LocallyDetermined').strip()
    if pmode not in ('LocallyDetermined', 'UsePreConfigured'):
        return HandlerResult(status=400, body={'error': 'invalid private_emergency_mode'})
    precip = (body.get('emergency_private_recipient') or '').strip() or None
    allow_call  = 1 if body.get('allow_emergency_call', True) else 0
    allow_alert = 1 if body.get('allow_emergency_alert', True) else 0
    allow_adhoc = 1 if body.get('allow_adhoc_call', True) else 0
    allow_priv  = 1 if body.get('allow_emergency_private_call', True) else 0
    # 원격 청취 자격 (TS 24.484 allow-ambient-listening, dispatch_center.md §5.6) — 기본 0, 부여는 manager 승인
    allow_amb   = 1 if body.get('allow_ambient_listening', False) else 0

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT 1 FROM ptt_subscriptions WHERE id=%s AND user_id=%s", (msisdn, person_id))
            if cur.fetchone() is None:
                return HandlerResult(status=404, body={'error': 'Subscription not found'})
            has_amb = _has_ambient_column(cur)
            if 'allow_ambient_listening' in body and not has_amb:
                return HandlerResult(status=400, body={'error': 'schema_not_migrated',
                                                       'detail': 'ptt_user_profile.allow_ambient_listening absent — sql/migrate_ptt_ambient_listening.sql not applied'})
            if egid:
                cur.execute("SELECT 1 FROM ptt_groups WHERE mcptt_group_id=%s", (egid,))
                if cur.fetchone() is None:
                    return HandlerResult(status=400, body={'error': f'unknown emergency_group_id: {egid}'})
            if precip:
                cur.execute("SELECT 1 FROM ptt_subscriptions WHERE id=%s", (precip,))
                if cur.fetchone() is None:
                    return HandlerResult(status=400,
                                         body={'error': f'unknown emergency_private_recipient: {precip}'})
            amb_col = ", allow_ambient_listening" if has_amb else ""
            amb_ph = ",%s" if has_amb else ""
            amb_upd = ", allow_ambient_listening=VALUES(allow_ambient_listening)" if has_amb else ""
            amb_vals = (allow_amb,) if has_amb else ()
            cur.execute(
                "INSERT INTO ptt_user_profile (ptt_id, allow_emergency_call, allow_emergency_alert, "
                "allow_adhoc_call, emergency_group_mode, emergency_group_id, "
                "allow_emergency_private_call, private_emergency_mode, emergency_private_recipient"
                f"{amb_col}, update_time) "
                f"VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s{amb_ph},NOW()) "
                "ON DUPLICATE KEY UPDATE allow_emergency_call=VALUES(allow_emergency_call), "
                "allow_emergency_alert=VALUES(allow_emergency_alert), "
                "allow_adhoc_call=VALUES(allow_adhoc_call), "
                "emergency_group_mode=VALUES(emergency_group_mode), "
                "emergency_group_id=VALUES(emergency_group_id), "
                "allow_emergency_private_call=VALUES(allow_emergency_private_call), "
                "private_emergency_mode=VALUES(private_emergency_mode), "
                f"emergency_private_recipient=VALUES(emergency_private_recipient){amb_upd}, update_time=NOW()",
                (msisdn, allow_call, allow_alert, allow_adhoc, mode, egid, allow_priv, pmode, precip, *amb_vals))

    prof = {
        "allow_emergency_call": bool(allow_call),
        "allow_emergency_alert": bool(allow_alert),
        "allow_adhoc_call": bool(allow_adhoc),
        "emergency_group_mode": mode,
        "emergency_group_id": egid,
        "allow_emergency_private_call": bool(allow_priv),
        "private_emergency_mode": pmode,
        "emergency_private_recipient": precip,
        "allow_ambient_listening": bool(allow_amb) if has_amb else False,
    }
    update_user_profile_cache(msisdn, prof)  # user-profile 문서 ETag 는 내용 파생 — 자동 갱신
    notify_csp("USER_CHANGED", f"tel:{msisdn}", "PUT")
    return HandlerResult(status=200, body=dict(prof, id=msisdn))


# ──────────────────────────────────────────────────────────────
#  MCPTT 시스템 서비스 설정 (mcptt_service_config — TS 24.484 service-config)
# ──────────────────────────────────────────────────────────────

_SVC_CFG_BASE = '/api/v1/mcptt/service-config'

_SVC_CFG_BOOLS = ('allow_private_call', 'allow_emergency_call', 'allow_alert',
                  'allow_transmit_request', 'allow_create_delete_group')
#  숫자 항목의 수용 범위 — 규격이 상한을 정하지 않으므로 운영상 무의미한 값만 걸러낸다.
_SVC_CFG_INTS = {'max_affiliations_n2': (1, 1000),
                 'num_levels_group_hierarchy': (1, 10),
                 'num_levels_user_hierarchy': (1, 10)}


async def handle_mcptt_service_config(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """GET/PUT /api/v1/mcptt/service-config — **시스템 전역** MCPTT 정책 1건(단일 행 id=1).

    사용자별 인가는 여기가 아니라 /users/:pid/ptt/:msisdn/profile(user-profile) 이다. 여기서 바꾼
    값은 XCAP service-config 문서로 나가고 단말이 시스템 정책 게이트로 소비한다
    (docs/design/features/android_ue_client.md §7).
    """
    config = kwargs.get('config', {})
    method = handler_args.method.upper()

    payload, err = admin_auth.require_role(handler_args, 'monitor' if method == 'GET' else 'manager')
    if err:
        return err

    try:
        if method == 'GET':
            return await _get_mcptt_service_config(config)
        if method == 'PUT':
            return await _put_mcptt_service_config(handler_args.body, config)
        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
    except Exception as e:
        logger.log_error(f"[ADMIN] mcptt service-config error: {e}")
        return HandlerResult(status=500, body={'error': str(e)})


async def _get_mcptt_service_config(config):
    """현재 값 — DB 행이 없으면(마이그레이션 전) 코드 기본값을 exists=false 로 돌려준다."""
    cols = list(_SVC_CFG_BOOLS) + list(_SVC_CFG_INTS)
    row = None
    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute(f"SELECT {', '.join(cols)}, update_time "
                            "FROM mcptt_service_config WHERE id=1")
                row = cur.fetchone()
    except Exception as e:
        logger.log_info(f"[ADMIN] mcptt_service_config 조회 실패(마이그레이션 전?): {e}")

    if row:
        cfg = {k: bool(row[k]) for k in _SVC_CFG_BOOLS}
        cfg.update({k: int(row[k]) for k in _SVC_CFG_INTS})
        cfg['update_time'] = _dt(row['update_time'])
        cfg['exists'] = True
    else:
        cfg = dict(SERVICE_CONFIG_DEFAULTS)
        cfg['update_time'] = None
        cfg['exists'] = False
    return HandlerResult(status=200, body=cfg)


async def _put_mcptt_service_config(body, config):
    """전 항목 UPSERT — 부분 갱신이 아니라 누락 항목은 현재 값을 유지한다(콘솔이 전체를 보낸다)."""
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})

    cur_cfg = get_service_config()
    new_cfg = {}
    for k in _SVC_CFG_BOOLS:
        new_cfg[k] = bool(body[k]) if k in body else bool(cur_cfg.get(k, True))
    for k, (lo, hi) in _SVC_CFG_INTS.items():
        raw = body.get(k, cur_cfg.get(k, SERVICE_CONFIG_DEFAULTS[k]))
        try:
            val = int(raw)
        except (TypeError, ValueError):
            return HandlerResult(status=400, body={'error': f'{k}: 정수가 아닙니다'})
        if not lo <= val <= hi:
            return HandlerResult(status=400, body={'error': f'{k}: {lo}~{hi} 범위를 벗어났습니다'})
        new_cfg[k] = val

    cols = list(_SVC_CFG_BOOLS) + list(_SVC_CFG_INTS)
    vals = [1 if new_cfg[k] else 0 for k in _SVC_CFG_BOOLS] + [new_cfg[k] for k in _SVC_CFG_INTS]
    upd = ', '.join(f"{c}=VALUES({c})" for c in cols)
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                f"INSERT INTO mcptt_service_config (id, {', '.join(cols)}, update_time) "
                f"VALUES (1, {', '.join(['%s'] * len(cols))}, NOW()) "
                f"ON DUPLICATE KEY UPDATE {upd}, update_time=NOW()", vals)

    # 캐시 갱신 — service-config 문서 ETag 는 내용 파생이라 다음 XCAP GET 이 새 값·새 ETag 를 받는다.
    update_service_config_cache(new_cfg)
    # 전역 문서라 특정 가입자 통지가 아니다 — CSP 가 cms 구독자 **전원**에게 xcap-diff NOTIFY 를
    #   push 한다(SERVICE_CONFIG_CHANGED). uri 는 대상 개념이 없어 빈 값.
    # ETag 는 RFC 7232 형식(따옴표 포함) — CSP 의 UDP JSON 파서는 단순 추출이라 따옴표를 벗겨 보낸다.
    _, etag = get_service_config_xml(None)
    notify_csp("SERVICE_CONFIG_CHANGED", "", "PUT", etag=(etag or "").strip('"'))
    return HandlerResult(status=200, body=dict(new_cfg, exists=True))


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
    (_SVC_CFG_BASE,  handle_mcptt_service_config, {}),
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
    {'name': '*_subscriptions[].sip_transport', 'type': 'string',
     'desc': '채널 정책 — TLS=서버 집행 / UDP·TCP=프로비저닝 힌트 / null=단말 선택'},
    {'name': '*_subscriptions[].auth_scheme', 'type': 'string',
     'desc': '인증 체계 — digest(SIP Digest, ha1) / aka(IMS AKA over TLS — TLS 채널 집행). 마이그레이션 전 DB 는 생략'},
    {'name': '*_subscriptions[].aka_provisioned', 'type': 'boolean',
     'desc': 'AKA K/OPc 보관 여부 (값 자체는 어떤 API 로도 나가지 않는다)'},
    {'name': '*_subscriptions[].service_ref', 'type': 'string', 'desc': '소속 서비스명 (도메인 결정)'},
    {'name': '*_subscriptions[].dnd', 'type': 'boolean', 'desc': '방해금지'},
    {'name': '*_subscriptions[].forward_id', 'type': 'string', 'desc': '착신전환 대상'},
    {'name': '*_subscriptions[].pickup_group', 'type': 'string',
     'desc': '당겨받기 그룹 키 — 같은 값끼리 픽업 가능. 빈 값=미지정(CSP 는 org_id 폴백). 마이그레이션 전 DB 는 생략'},
    {'name': '*_subscriptions[].register_time', 'type': 'string', 'desc': 'ISO8601 최근 등록 시각'},
    {'name': '*_subscriptions[].logout_time', 'type': 'string', 'desc': 'ISO8601 최근 로그아웃 시각'},
    {'name': 'create_time', 'type': 'string', 'desc': 'ISO8601 생성'},
    {'name': 'update_time', 'type': 'string', 'desc': 'ISO8601 수정'},
]

_USER_EXAMPLE = {
    'id': 11, 'name': '홍길동', 'title': '팀장', 'login_id': 'test001', 'org_id': 'D110',
    'email': 'gildong@example.com', 'details': '', 'reject_id': [],
    'call_subscriptions': [{'id': '01000000001', 'imsi': '450050000000001',
                            'service_ref': 'volte', 'sip_transport': None, 'dnd': False, 'forward_id': '',
                            'register_time': '2026-07-30T08:40:11', 'logout_time': None}],
    'ptt_subscriptions': [{'id': '01000000001', 'imsi': '450050000000001',
                           'service_ref': 'mcptt', 'sip_transport': 'TLS', 'dnd': False, 'forward_id': '',
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

_SVC_CFG_FIELDS = [
    {'name': 'allow_private_call', 'type': 'boolean', 'desc': 'allow-private-call — 1:1 통화 발신 허용'},
    {'name': 'allow_emergency_call', 'type': 'boolean', 'desc': 'allow-emergency-call — 긴급통화 허용(사용자 인가와 AND)'},
    {'name': 'allow_alert', 'type': 'boolean', 'desc': 'allow-alert — 긴급경보 허용(사용자 인가와 AND)'},
    {'name': 'allow_transmit_request', 'type': 'boolean', 'desc': 'on-network allow-transmit-request — 발언권 요청 허용'},
    {'name': 'allow_create_delete_group', 'type': 'boolean', 'desc': 'allow-create-delete-group — 사용자 그룹 생성/삭제 허용'},
    {'name': 'max_affiliations_n2', 'type': 'integer', 'desc': 'N2 — 동시 제휴(편성) 채널 상한 (1~1000)'},
    {'name': 'num_levels_group_hierarchy', 'type': 'integer', 'desc': 'num-levels-group-hierarchy (1~10)'},
    {'name': 'num_levels_user_hierarchy', 'type': 'integer', 'desc': 'num-levels-user-hierarchy (1~10)'},
    {'name': 'update_time', 'type': 'string', 'desc': '마지막 변경 시각(ISO) — 행 부재면 null'},
    {'name': 'exists', 'type': 'boolean', 'desc': 'DB 행 존재 여부 (false=코드 기본값 응답)'},
]

_SVC_CFG_EXAMPLE = {
    'allow_private_call': True, 'allow_emergency_call': True, 'allow_alert': True,
    'allow_transmit_request': True, 'allow_create_delete_group': True,
    'max_affiliations_n2': 10, 'num_levels_group_hierarchy': 3, 'num_levels_user_hierarchy': 3,
    'update_time': '2026-08-19T18:00:00', 'exists': True,
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
         {'name': 'subscriptions[].sip_transport', 'type': 'string',
          'desc': '채널 정책 — TLS=서버 집행(비-TLS 채널 요청 403) / UDP·TCP=프로비저닝 힌트 / null=단말 선택'},
         {'name': 'subscriptions[].dnd', 'type': 'boolean', 'desc': '방해금지'},
         {'name': 'subscriptions[].forward_id', 'type': 'string', 'desc': '착신전환 대상'},
         {'name': 'subscriptions[].register_time', 'type': 'string', 'desc': '최근 등록 시각'},
         {'name': 'subscriptions[].logout_time', 'type': 'string', 'desc': '최근 로그아웃 시각'},
     ],
     'example': {'subscriptions': [{'id': '01000000001', 'imsi': '450050000000001',
                                    'service_ref': 'volte', 'sip_transport': None, 'dnd': False, 'forward_id': '',
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
          'desc': '{id(MSISDN, 필수), imsi(필수), service_ref?, passwd?, sip_transport?(UDP|TCP|TLS), '
                  'auth_scheme?(digest|aka), k?(hex32), opc?(hex32)|op?(hex32), amf?(hex4), dnd?, forward_id?, '
                  'pickup_group?}'},
     ],
     'response': '{id}',
     'response_fields': [{'name': 'id', 'type': 'string', 'desc': '추가된 번호(MSISDN)'}],
     'example': {'id': '01000000001'},
     'errors': _ERR_COMMON + [
         {'status': 400, 'when': 'JSON 본문 없음', 'body': {'error': 'JSON body required'}},
         {'status': 400, 'when': 'id 누락', 'body': {'error': 'id (MSISDN) is required'}},
         {'status': 400, 'when': 'imsi 누락', 'body': {'error': 'imsi required'}},
         {'status': 400, 'when': 'passwd 가 있는데 service_ref 가 비었거나 미정의 서비스',
          'body': {'error': 'service_ref required to derive ha1 (unknown service)'}},
         {'status': 400, 'when': 'sip_transport 값 오류', 'body': {'error': 'sip_transport must be one of UDP/TCP/TLS'}},
         {'status': 400, 'when': 'auth_scheme=aka 인데 k/opc(op) 없음', 'body': {'error': 'k and opc (or op) required for auth_scheme=aka'}},
         {'status': 503, 'when': 'subscriptions.ha1 컬럼 없음 (migrate_subscription_ha1.sql 미적용 — 자격 저장처 부재)',
          'body': {'error': 'schema_not_migrated'}},
         {'status': 400, 'when': 'k/opc/op/amf 형식 오류', 'body': {'error': 'bad_key_material'}},
         {'status': 503, 'when': 'AKA 키 입력인데 csc.json AuC.Kek 미설정', 'body': {'error': 'auc_disabled'}},
         {'status': 404, 'when': '없는 가입자', 'body': {'error': 'User not found'}},
     ],
     'notes': ['성공 시 **201** 이다.', 'imsi 는 SIP 인증 username 의 user 파트로 쓰여 필수다.',
               'passwd 는 저장되지 않는다 — H(A1)=MD5(imsi@domain:realm:passwd) 로 변환되어 `ha1` 에 저장된다 '
               '(realm = 서비스 auth_realm ?? domain). 따라서 passwd 를 줄 때는 service_ref 가 해석되어야 한다.',
               'sip_transport=TLS 는 **서버가 집행**한다 — 이 번호의 비-TLS 채널 요청은 REGISTER 포함 전부 403. '
               'UDP/TCP 는 단말 프로비저닝 힌트일 뿐이고 null 이면 단말이 transport 를 고른다.',
               'auth_scheme=aka(IMS AKA over TLS) 는 k + opc(또는 op → OPc 유도) 를 함께 보낸다. K/OPc 는 AuC.Kek 로 '
               '암호화 보관되고 어떤 API 응답에도 나가지 않는다. AKA 가입자는 sip_transport 와 무관하게 TLS 채널만 허용된다.',
               'pickup_group 은 당겨받기 그룹 키 — 같은 값끼리 픽업 가능(volte_supplementary_services.md §5.1). '
               '빈 값/미전송=미지정(CSP 는 org_id 폴백). CSP 반영은 다음 REGISTER 갱신부터.'],
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
         {'status': 400, 'when': 'imsi 또는 service_ref 가 바뀌는데 passwd 미전송',
          'body': {'error': 'passwd required when imsi or service_ref changes (ha1 rebinding)'}},
         {'status': 400, 'when': 'sip_transport 값 오류', 'body': {'error': 'sip_transport must be one of UDP/TCP/TLS'}},
         {'status': 404, 'when': '없는 가입자/번호'},
     ],
     'notes': ['dnd 는 "Y"/"1"/"true"/"on" 같은 문자열도 참으로 해석된다 ("false"/"0" 은 거짓).',
               'passwd 는 변경할 때만 전송한다 — 미전송/빈값이면 기존 ha1 이 유지된다.',
               'H(A1) 은 (imsi, 서비스 domain/realm) 에 결박된다. imsi 나 service_ref 를 바꾸는 요청은 passwd 를 함께 보내야 한다.',
               'sip_transport 는 키가 있을 때만 반영 — null/빈값이면 정책 해제(단말 선택).',
               'auth_scheme/k/opc(op)/amf 는 키가 있을 때만 반영. k 나 opc 를 바꾸면 SQN 이 0 으로 리셋된다. '
               'aka 로 바꾸는데 보관된 키가 없으면 k/opc 를 같이 보내야 한다(400).',
               'pickup_group 은 키가 있을 때만 반영 — 빈 값이면 그룹 해제(org_id 폴백). 반영은 다음 REGISTER 갱신부터.'],
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

    # ── MCPTT 시스템 정책 (TS 24.484 service-config) ──────────────────────────
    {'id': 'csc.mcptt.service-config.get', 'module': 'csc', 'method': 'GET',
     'path': '/api/v1/mcptt/service-config',
     'summary': 'MCPTT 시스템 서비스 설정 조회 (전역 1건)',
     'params': [],
     'response': '설정 객체',
     'response_fields': list(_SVC_CFG_FIELDS),
     'example': dict(_SVC_CFG_EXAMPLE),
     'errors': list(_ERR_COMMON),
     'notes': ['시스템 전역 1건이다 — 사용자별 인가는 GET /api/v1/users/{person_id}/ptt/{msisdn}/profile.',
               'exists=false 는 DB 행 부재(마이그레이션 전)이며 값은 코드 기본값이다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'csc.mcptt.service-config.update', 'module': 'csc', 'method': 'PUT',
     'path': '/api/v1/mcptt/service-config',
     'summary': 'MCPTT 시스템 서비스 설정 변경',
     'params': [{'name': 'body', 'in': 'body', 'type': 'object', 'required': True,
                 'desc': '설정 객체(부분 전송 시 누락 항목은 현재 값 유지)'}],
     'response': '반영된 설정 객체',
     'response_fields': list(_SVC_CFG_FIELDS),
     'example': dict(_SVC_CFG_EXAMPLE),
     'errors': _ERR_COMMON + [{'status': 400, 'when': '정수 아님 / 허용 범위 초과',
                               'body': {'error': 'max_affiliations_n2: 1~1000 범위를 벗어났습니다'}}],
     'notes': ['XCAP service-config 문서로 즉시 반영된다(ETag 는 내용 파생).',
               'CSP 가 cms 구독자 전원에게 xcap-diff NOTIFY 를 push 해(SERVICE_CONFIG_CHANGED) 단말이 곧바로 재조회한다.',
               '단말은 이 값을 user-profile 의 사용자 인가와 AND 로 게이트한다.'],
     'auth': dict(_AUTH_MANAGER)},
]
