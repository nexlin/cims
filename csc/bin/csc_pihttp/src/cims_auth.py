"""
CIMS Web Auth API
Routes:
  POST /api/v1/auth/login      - 로그인 (email + password → JWT)
  POST /api/v1/auth/register   - 회원가입 (role=user 고정)
  GET  /api/v1/auth/me         - 내 정보 (JWT 필요)
  PUT  /api/v1/auth/password   - 비밀번호 변경 (JWT 필요)
"""

import hashlib
import datetime

import jwt
import pymysql
import pymysql.cursors
from urllib.parse import urlparse
from pathlib import PurePath

from util.pi_http.http_handler import HandlerArgs, HandlerResult

# ── 상수 ──────────────────────────────────────────────────────
_SECRET    = 'cims_jwt_secret_2024'
_TTL_SEC   = 86400 * 7   # 7일


# ── 공통 유틸 ──────────────────────────────────────────────────

def _hash(pw: str) -> str:
    return hashlib.sha256(pw.encode()).hexdigest()


def _make_token(user: dict) -> str:
    payload = {
        'sub':   str(user['id']),
        'email': user['email'],
        'role':  user['role'],
        'exp':   datetime.datetime.utcnow() + datetime.timedelta(seconds=_TTL_SEC),
    }
    return jwt.encode(payload, _SECRET, algorithm='HS256')


def verify_token(token: str) -> dict | None:
    try:
        return jwt.decode(token, _SECRET, algorithms=['HS256'])
    except Exception:
        return None


def extract_token(handler_args: HandlerArgs) -> dict | None:
    auth = handler_args.headers.get('authorization', '')
    if auth.startswith('Bearer '):
        return verify_token(auth[7:])
    return None


def require_auth(handler_args: HandlerArgs):
    """토큰 검증 → (payload | None, HandlerResult | None)"""
    payload = extract_token(handler_args)
    if payload is None:
        return None, HandlerResult(status=401, body={'error': '로그인이 필요합니다'})
    return payload, None


def require_admin(handler_args: HandlerArgs):
    payload, err = require_auth(handler_args)
    if err:
        return None, err
    if payload.get('role') != 'admin':
        return None, HandlerResult(status=403, body={'error': '관리자 권한이 필요합니다'})
    return payload, None


# ── DB 헬퍼 ──────────────────────────────────────────────────

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


def _dt(val):
    return val.isoformat() if val else None


def _user_with_subs(cur, user_id: int) -> dict:
    # 소프트폰 자동 등록에 passwd 필요 → 본인 조회이므로 포함
    cur.execute(
        "SELECT id, auth_id, passwd, dnd, forward_id, register_time, logout_time "
        "FROM cims_call_users WHERE user_id=%s ORDER BY id",
        (user_id,)
    )
    call_subs = cur.fetchall()
    for s in call_subs:
        s['dnd'] = bool(s['dnd'])
        s['register_time'] = _dt(s['register_time'])
        s['logout_time']   = _dt(s['logout_time'])

    cur.execute(
        "SELECT id, auth_id, passwd, dnd, forward_id, register_time, logout_time "
        "FROM cims_ptt_users WHERE user_id=%s ORDER BY id",
        (user_id,)
    )
    ptt_subs = cur.fetchall()
    for s in ptt_subs:
        s['dnd'] = bool(s['dnd'])
        s['register_time'] = _dt(s['register_time'])
        s['logout_time']   = _dt(s['logout_time'])

    return {'call_subscriptions': call_subs, 'ptt_subscriptions': ptt_subs}


# ── 핸들러 ──────────────────────────────────────────────────────

_AUTH_BASE = '/api/v1/auth'


def _parts(full_path: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(_AUTH_BASE))
        return tuple(p for p in rel.parts if p)
    except ValueError:
        return ()


async def handle_auth(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    parts  = _parts(handler_args.full_path)
    action = parts[0] if parts else None
    method = handler_args.method.upper()

    try:
        if action == 'login'    and method == 'POST':
            return await _login(handler_args.body, config)
        if action == 'register' and method == 'POST':
            return await _register(handler_args.body, config)
        if action == 'me'       and method == 'GET':
            return await _me(handler_args, config)
        if action == 'password' and method == 'PUT':
            return await _change_password(handler_args, config)
        return HandlerResult(status=404, body={'error': 'Not Found'})
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})


async def _login(body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON 형식이 아닙니다'})
    email    = (body.get('email')    or '').strip()
    password = (body.get('password') or '').strip()
    if not email or not password:
        return HandlerResult(status=400, body={'error': '이메일과 비밀번호를 입력하세요'})

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, name, email, role FROM cims_users "
                "WHERE email=%s AND password=%s",
                (email, _hash(password))
            )
            user = cur.fetchone()
            if user is None:
                return HandlerResult(status=401, body={'error': '이메일 또는 비밀번호가 잘못되었습니다'})
            subs = _user_with_subs(cur, user['id'])

    user.update(subs)
    token = _make_token(user)
    return HandlerResult(status=200, body={'token': token, 'user': user})


async def _register(body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON 형식이 아닙니다'})
    name     = (body.get('name')     or '').strip()
    email    = (body.get('email')    or '').strip()
    password = (body.get('password') or '').strip()

    if not name:
        return HandlerResult(status=400, body={'error': '이름을 입력하세요'})
    if not email:
        return HandlerResult(status=400, body={'error': '이메일을 입력하세요'})
    if not password or len(password) < 4:
        return HandlerResult(status=400, body={'error': '비밀번호는 4자 이상이어야 합니다'})

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT id FROM cims_users WHERE email=%s", (email,))
            if cur.fetchone():
                return HandlerResult(status=409, body={'error': '이미 사용 중인 이메일입니다'})
            cur.execute(
                "INSERT INTO cims_users (name, email, password, role, org_id, create_time, update_time) "
                "VALUES (%s, %s, %s, 'user', '', NOW(), NOW())",
                (name, email, _hash(password))
            )
            uid = cur.lastrowid
            cur.execute(
                "SELECT id, name, email, role FROM cims_users WHERE id=%s", (uid,)
            )
            user = cur.fetchone()

    token = _make_token(user)
    user.update({'call_subscriptions': [], 'ptt_subscriptions': []})
    return HandlerResult(status=201, body={'token': token, 'user': user})


async def _me(handler_args, config):
    payload, err = require_auth(handler_args)
    if err:
        return err

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, name, email, role FROM cims_users WHERE id=%s",
                (int(payload['sub']),)
            )
            user = cur.fetchone()
            if user is None:
                return HandlerResult(status=404, body={'error': '사용자를 찾을 수 없습니다'})
            subs = _user_with_subs(cur, user['id'])

    user.update(subs)
    return HandlerResult(status=200, body=user)


async def _change_password(handler_args, config):
    payload, err = require_auth(handler_args)
    if err:
        return err

    body = handler_args.body
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON 형식이 아닙니다'})
    old_pw = (body.get('old_password') or '').strip()
    new_pw = (body.get('new_password') or '').strip()
    if not old_pw or not new_pw:
        return HandlerResult(status=400, body={'error': '현재/새 비밀번호를 입력하세요'})
    if len(new_pw) < 4:
        return HandlerResult(status=400, body={'error': '새 비밀번호는 4자 이상이어야 합니다'})

    uid = int(payload['sub'])
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id FROM cims_users WHERE id=%s AND password=%s",
                (uid, _hash(old_pw))
            )
            if cur.fetchone() is None:
                return HandlerResult(status=401, body={'error': '현재 비밀번호가 올바르지 않습니다'})
            cur.execute(
                "UPDATE cims_users SET password=%s, update_time=NOW() WHERE id=%s",
                (_hash(new_pw), uid)
            )

    return HandlerResult(status=200, body={'ok': True})


# ── 핸들러 목록 ────────────────────────────────────────────────
CIMS_AUTH_HANDLER_LIST = [
    (_AUTH_BASE, handle_auth, {}),
]
