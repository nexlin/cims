"""
CIMS 콘솔 계정 (OAM 로그인 계정) — file_store 영속 REST API.

설계 (2026-06-15): OAM 콘솔 로그인 계정을 DB `users` 에서 분리해 file_store 도메인
`console_accounts` 로 관리한다. (DB `users` 는 가입자(person) 전용으로 환원.)

  - SoT = file_store 도메인 `console_accounts/<login_id>.json` (console.py 의
    console_layouts/console_menu 와 동일 패턴). OAM 이 직접 읽어 로그인 인증.
  - 콘솔에서 CRUD 가능 (정적 oam.json 내장 계정과 별개). 내장 계정(admin)은
    부트스트랩(DB/파일 미구축)용으로 유지하며 login 시 항상 우선.
  - 역할은 console 로그인 가능 등급만 (admin/manager/operator/monitor). 'user'
    (telephony 전용)는 가입자이므로 콘솔 계정이 될 수 없다.
  - HA: file_store 도메인이므로 OAM 런타임 공유(AS/AA 공유 스토리지) 시 함께
    동기화 (console_layouts/console_menu 와 동일 취급).

Routes (mounted at /api/v1/console-accounts):
  GET    /api/v1/console-accounts              계정 목록 (password_sha256 제외)
  POST   /api/v1/console-accounts              계정 생성 {login_id,name,role,password[,email]}
  GET    /api/v1/console-accounts/{login_id}   계정 1건
  PUT    /api/v1/console-accounts/{login_id}   계정 수정 {name?,role?,email?}
  DELETE /api/v1/console-accounts/{login_id}   계정 삭제
  PUT    /api/v1/console-accounts/{login_id}/password  비밀번호 재설정 {new_password}
"""
import hashlib
from urllib.parse import urlparse, unquote
from pathlib import PurePath

from httpsrv.handler import HandlerArgs, HandlerResult
from services import file_store
from services import admin_auth as _shared_auth

_BASE = '/api/v1/console-accounts'
_DOMAIN = 'console_accounts'


def _hash(pw: str) -> str:
    return hashlib.sha256(pw.encode()).hexdigest()


def _dir(config: dict) -> str:
    return file_store.domain_dir(config, _DOMAIN)


# ── 서비스 API (auth.py 가 import) ─────────────────────────────────

def get_account(config: dict, login_id: str):
    """login_id 계정 1건 (password_sha256 포함) | None."""
    lid = (login_id or '').strip()
    if not lid:
        return None
    return file_store.load(_dir(config), lid)


def list_accounts(config: dict) -> list:
    """전체 계정 (password_sha256 제외, login_id 정렬)."""
    rows = file_store.load_all(_dir(config))
    rows.sort(key=lambda r: (r.get('login_id') or ''))
    return [_public(r) for r in rows]


def verify(config: dict, login_id: str, password: str):
    """login_id/password 검증 → 계정 dict(password 제외) | None."""
    acct = get_account(config, login_id)
    if acct is None:
        return None
    if _hash(password) != (acct.get('password_sha256') or ''):
        return None
    return _public(acct)


def change_password(config: dict, login_id: str, old_password: str, new_password: str) -> str:
    """본인 비밀번호 변경 (old 검증). 반환: 'ok' | 'notfound' | 'badold'."""
    d = _dir(config)
    acct = file_store.load(d, login_id)
    if acct is None:
        return 'notfound'
    if _hash(old_password) != (acct.get('password_sha256') or ''):
        return 'badold'
    acct['password_sha256'] = _hash(new_password)
    file_store.save(d, login_id, acct)
    return 'ok'


def _public(r: dict) -> dict:
    """password_sha256 제거한 외부 노출 형태."""
    return {
        'login_id': r.get('login_id'),
        'name':     r.get('name') or r.get('login_id'),
        'role':     r.get('role'),
        'email':    r.get('email') or '',
        'create_time': r.get('create_time'),
        'update_time': r.get('update_time'),
    }


# ── REST ──────────────────────────────────────────────────────────

def _parts(full_path: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(_BASE))
        return tuple(unquote(p) for p in rel.parts if p)
    except ValueError:
        return ()


def _body(handler_args: HandlerArgs):
    b = getattr(handler_args, 'body', None)
    return b if isinstance(b, dict) else None


def _valid_role(role: str) -> bool:
    # 콘솔 계정은 로그인 가능 등급(monitor 이상)만. 'user'(가입자)는 불가.
    return _shared_auth.can_login(role)


async def handle_console_accounts(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    # 콘솔 계정 관리는 admin 권한.
    _payload, err = _require_admin(handler_args)
    if err:
        return err

    parts  = _parts(handler_args.full_path)
    method = handler_args.method.upper()
    d = _dir(config)

    # 목록 / 생성
    if not parts:
        if method == 'GET':
            return HandlerResult(status=200, body={'items': list_accounts(config)})
        if method == 'POST':
            return _create(config, _body(handler_args))
        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

    login_id = parts[0]
    # 비밀번호 재설정
    if len(parts) == 2 and parts[1] == 'password' and method == 'PUT':
        return _set_password(config, login_id, _body(handler_args))
    if len(parts) != 1:
        return HandlerResult(status=404, body={'error': 'Not Found'})

    if method == 'GET':
        acct = file_store.load(d, login_id)
        if acct is None:
            return HandlerResult(status=404, body={'error': '계정이 없습니다'})
        return HandlerResult(status=200, body=_public(acct))
    if method == 'PUT':
        return _update(config, login_id, _body(handler_args))
    if method == 'DELETE':
        if file_store.delete(d, login_id):
            return HandlerResult(status=200, body={'ok': True})
        return HandlerResult(status=404, body={'error': '계정이 없습니다'})
    return HandlerResult(status=405, body={'error': 'Method Not Allowed'})


def _require_admin(handler_args: HandlerArgs):
    payload = _shared_auth.extract_admin_jwt(getattr(handler_args, 'headers', None))
    if payload is None:
        return None, HandlerResult(status=401, body={'error': '로그인이 필요합니다'})
    if _shared_auth.role_rank(payload.get('role')) < _shared_auth.role_rank('admin'):
        return None, HandlerResult(status=403, body={'error': '관리자 권한이 필요합니다'})
    return payload, None


def _create(config: dict, body):
    if not body:
        return HandlerResult(status=400, body={'error': 'JSON 형식이 아닙니다'})
    login_id = (body.get('login_id') or '').strip()
    name     = (body.get('name') or '').strip()
    role     = (body.get('role') or '').strip()
    password = (body.get('password') or '').strip()
    if not login_id:
        return HandlerResult(status=400, body={'error': '아이디를 입력하세요'})
    if not _valid_role(role):
        return HandlerResult(status=400, body={'error': "역할은 admin/manager/operator/monitor 중 하나여야 합니다"})
    if not password or len(password) < 4:
        return HandlerResult(status=400, body={'error': '비밀번호는 4자 이상이어야 합니다'})
    d = _dir(config)
    if file_store.exists(d, login_id):
        return HandlerResult(status=409, body={'error': '이미 사용 중인 아이디입니다'})
    rec = {
        'login_id': login_id,
        'name':     name or login_id,
        'role':     role,
        'email':    (body.get('email') or '').strip(),
        'password_sha256': _hash(password),
    }
    file_store.save(d, login_id, rec)
    return HandlerResult(status=201, body=_public(file_store.load(d, login_id)))


def _update(config: dict, login_id: str, body):
    if not body:
        return HandlerResult(status=400, body={'error': 'JSON 형식이 아닙니다'})
    d = _dir(config)
    acct = file_store.load(d, login_id)
    if acct is None:
        return HandlerResult(status=404, body={'error': '계정이 없습니다'})
    if 'name' in body:
        acct['name'] = (body.get('name') or '').strip() or login_id
    if 'email' in body:
        acct['email'] = (body.get('email') or '').strip()
    if 'role' in body:
        role = (body.get('role') or '').strip()
        if not _valid_role(role):
            return HandlerResult(status=400, body={'error': "역할은 admin/manager/operator/monitor 중 하나여야 합니다"})
        acct['role'] = role
    file_store.save(d, login_id, acct)
    return HandlerResult(status=200, body=_public(file_store.load(d, login_id)))


def _set_password(config: dict, login_id: str, body):
    if not body:
        return HandlerResult(status=400, body={'error': 'JSON 형식이 아닙니다'})
    new_pw = (body.get('new_password') or '').strip()
    if not new_pw or len(new_pw) < 4:
        return HandlerResult(status=400, body={'error': '새 비밀번호는 4자 이상이어야 합니다'})
    d = _dir(config)
    acct = file_store.load(d, login_id)
    if acct is None:
        return HandlerResult(status=404, body={'error': '계정이 없습니다'})
    acct['password_sha256'] = _hash(new_pw)
    file_store.save(d, login_id, acct)
    return HandlerResult(status=200, body={'ok': True})


CIMS_CONSOLE_ACCOUNTS_HANDLER_LIST = [
    (_BASE, handle_console_accounts, {}),
]
