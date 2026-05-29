"""
CIMS Web Auth API
Routes:
  POST /api/v1/auth/login      - 로그인 (login_id + password → JWT + 최소 user)
  POST /api/v1/auth/register   - 회원가입 (role=user 고정)
  PUT  /api/v1/auth/password   - 비밀번호 변경 (JWT 필요)

v3 (2026-04-22): 로그인과 프로파일/가입자 정보 분리.
  /users/me              — 프로파일 (handlers/users.py)
  /users/me/subscriptions — 본인 가입자 배열 (Phone UE 용)
"""

import hashlib
import datetime

import jwt
import pymysql
import pymysql.cursors
from urllib.parse import urlparse
from pathlib import PurePath

from httpsrv.handler import HandlerArgs, HandlerResult
from services import admin_auth as _shared_auth

# ── 상수 ──────────────────────────────────────────────────────
_SECRET    = 'cims_jwt_secret_change_me'
_TTL_SEC   = 86400 * 7   # 7일


def init(config: dict) -> None:
    """Read JWT secret from config. Call once at startup.

    services.admin_auth 와 동일 비밀키를 공유 (Phase 3 에서 oam/csc 양쪽이 같은
    K 로 검증 가능하도록 layout 정리)."""
    global _SECRET
    secret = config.get('CimsAuth', {}).get('JwtSecret')
    if secret:
        _SECRET = secret
    _shared_auth.init(config)


# ── 공통 유틸 ──────────────────────────────────────────────────

def _hash(pw: str) -> str:
    return hashlib.sha256(pw.encode()).hexdigest()


def _make_token(user: dict) -> str:
    payload = {
        'sub':      str(user['id']),
        'login_id': user['login_id'],
        'role':     user['role'],
        'exp':      datetime.datetime.utcnow() + datetime.timedelta(seconds=_TTL_SEC),
    }
    return jwt.encode(payload, _SECRET, algorithm='HS256')


def verify_token(token: str) -> dict | None:
    return _shared_auth.verify_admin_jwt(token)


def extract_token(handler_args: HandlerArgs) -> dict | None:
    return _shared_auth.extract_admin_jwt(handler_args.headers)


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
        "SELECT id, service_ref, imsi, passwd, dnd, forward_id, register_time, logout_time "
        "FROM volte_subscriptions WHERE user_id=%s ORDER BY id",
        (user_id,)
    )
    call_subs = cur.fetchall()
    for s in call_subs:
        s['dnd'] = bool(s['dnd'])
        s['register_time'] = _dt(s['register_time'])
        s['logout_time']   = _dt(s['logout_time'])

    cur.execute(
        "SELECT id, service_ref, imsi, passwd, dnd, forward_id, register_time, logout_time "
        "FROM ptt_subscriptions WHERE user_id=%s ORDER BY id",
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
        if action == 'password' and method == 'PUT':
            return await _change_password(handler_args, config)
        # v3 (2026-04-22): /auth/me 제거 — /users/me 로 이관
        return HandlerResult(status=404, body={'error': 'Not Found'})
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})


async def _login(body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON 형식이 아닙니다'})
    login_id = (body.get('login_id') or '').strip()
    password = (body.get('password') or '').strip()
    if not login_id or not password:
        return HandlerResult(status=400, body={'error': '아이디와 비밀번호를 입력하세요'})

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, name, login_id, role FROM users "
                "WHERE login_id=%s AND password=%s",
                (login_id, _hash(password))
            )
            user = cur.fetchone()
            if user is None:
                return HandlerResult(status=401, body={'error': '아이디 또는 비밀번호가 잘못되었습니다'})

    # v3: 로그인 응답은 토큰 + 최소 user 정보만.
    #   가입자 정보는 /users/me/subscriptions 로 분리 (Phone UE 가 별도 호출).
    token = _make_token(user)
    return HandlerResult(status=200, body={'token': token, 'user': user})


async def _register(body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON 형식이 아닙니다'})
    name     = (body.get('name')     or '').strip()
    login_id = (body.get('login_id') or '').strip()
    password = (body.get('password') or '').strip()

    if not name:
        return HandlerResult(status=400, body={'error': '이름을 입력하세요'})
    if not login_id:
        return HandlerResult(status=400, body={'error': '아이디를 입력하세요'})
    if not password or len(password) < 4:
        return HandlerResult(status=400, body={'error': '비밀번호는 4자 이상이어야 합니다'})

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT id FROM users WHERE login_id=%s", (login_id,))
            if cur.fetchone():
                return HandlerResult(status=409, body={'error': '이미 사용 중인 아이디입니다'})
            cur.execute(
                "INSERT INTO users (name, login_id, password, role, org_id, create_time, update_time) "
                "VALUES (%s, %s, %s, 'user', '', NOW(), NOW())",
                (name, login_id, _hash(password))
            )
            uid = cur.lastrowid
            cur.execute(
                "SELECT id, name, login_id, role FROM users WHERE id=%s", (uid,)
            )
            user = cur.fetchone()

    token = _make_token(user)
    user.update({'call_subscriptions': [], 'ptt_subscriptions': []})
    return HandlerResult(status=201, body={'token': token, 'user': user})



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
                "SELECT id FROM users WHERE id=%s AND password=%s",
                (uid, _hash(old_pw))
            )
            if cur.fetchone() is None:
                return HandlerResult(status=401, body={'error': '현재 비밀번호가 올바르지 않습니다'})
            cur.execute(
                "UPDATE users SET password=%s, update_time=NOW() WHERE id=%s",
                (_hash(new_pw), uid)
            )

    return HandlerResult(status=200, body={'ok': True})


# ── 핸들러 목록 ────────────────────────────────────────────────
CIMS_AUTH_HANDLER_LIST = [
    (_AUTH_BASE, handle_auth, {}),
]
