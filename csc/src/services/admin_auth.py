"""Admin JWT verification — oam/csc 공유 라이브러리.

설계: docs/design/oam_csc_split.md §결정 3 "Admin 인증 모델 1".

- 발급(issue) — oam 의 handlers/auth.py 가 책임 (관리자 로그인).
- 검증(verify) — oam, csc 모두 import 가능해야 함. csc 에 admin JWT 검증 endpoint
  가 추가되면 (Phase 3 이후) 본 모듈을 import.

Phase 1: 아직 csc 측 핸들러는 admin JWT 검증을 하지 않으므로 본 모듈은 scaffold 의
미. 실제 검증 로직은 handlers/auth.py 의 verify_token 과 동일 (같은 비밀키 _SECRET).
Phase 3 에서 oam.json + csc.json 양쪽에 동일한 CimsAuth.JwtSecret 설정.
"""

from typing import Optional

import jwt

from httpsrv.handler import HandlerResult


_SECRET = 'cims_jwt_secret_change_me'  # config 로 갱신

# ─────────────────────────────────────────────────────────────
#  RBAC 역할 모델 — docs/design/features/mcptt_authorization.md §2·§3
#    역할 = roles 행(능력 + 범위). 내장 4행 admin > manager > operator > monitor 는 계층으로도 읽힌다
#    (require_role 의 최소 등급 게이트). 토큰 role 클레임 = roles.id — 콘솔 계정(OAM console_accounts·내장
#    admin)의 속성으로 토큰에만 온다. 커스텀 역할 id(role-…)가 배정된 콘솔 계정은 계층 게이트에서 monitor
#    등급(읽기 전용)으로 보고(OAM 로컬 게이트와 같은 규칙 — oam/src/services/admin_auth.py), 실제 능력·범위는
#    services/authz.can() 이 roles 행으로 판정한다(§2.2). 이 파일은 roles 행을 읽지 않는다(인증 + 계층 다리만).
#    user 는 "콘솔 계정 없음"(가입자 = telephony 전용, 로그인 불가) 의 자리값. DB users 에는 role 이 없다.
# ─────────────────────────────────────────────────────────────
_ROLE_RANK = {'user': 0, 'monitor': 1, 'operator': 2, 'manager': 3, 'admin': 4}
ROLES = tuple(_ROLE_RANK.keys())
CUSTOM_ROLE_PREFIX = 'role-'


def is_custom_role(role: Optional[str]) -> bool:
    return bool(role) and role.startswith(CUSTOM_ROLE_PREFIX) and len(role) > len(CUSTOM_ROLE_PREFIX)


def role_rank(role: Optional[str]) -> int:
    if is_custom_role(role):
        return _ROLE_RANK['monitor']
    return _ROLE_RANK.get(role or '', 0)


def can_login(role: Optional[str]) -> bool:
    """OAM 콘솔 로그인 가능 여부. user(=telephony 전용)는 로그인 불가."""
    return role_rank(role) >= _ROLE_RANK['monitor']


def require_role(handler_args, min_role: str):
    """JWT 검증 + 최소 역할 등급 게이팅 → (payload | None, err | None).

    payload 에는 sub/login_id/role 클레임이 담긴다 (handlers/auth.py _make_token).
    """
    headers = getattr(handler_args, 'headers', None)
    payload = extract_admin_jwt(headers)
    if payload is None:
        return None, HandlerResult(status=401, body={'error': '로그인이 필요합니다'})
    if role_rank(payload.get('role')) < role_rank(min_role):
        return None, HandlerResult(status=403, body={'error': '권한이 부족합니다'})
    return payload, None


def init(config: dict) -> None:
    """init(config) — startup 시 1회 호출. CimsAuth.JwtSecret 로드."""
    global _SECRET
    secret = (config.get('CimsAuth') or {}).get('JwtSecret')
    if secret:
        _SECRET = secret


def verify_admin_jwt(token: str) -> Optional[dict]:
    """Admin JWT 검증 → claims dict | None."""
    try:
        return jwt.decode(token, _SECRET, algorithms=['HS256'])
    except Exception:
        return None


def service_token(role: str = 'monitor', ttl_sec: int = 120, sub: str = 'csc') -> str:
    """서버 간 호출용 단기 서비스 토큰 — OAM 이력·녹취 API(role ≥ monitor 게이트)를 CSC 관제 프록시
    (handlers/dispatch_recordings)가 부를 때 붙인다. 콘솔 admin JWT 와 같은 시크릿(CimsAuth.JwtSecret)·
    알고리즘·클레임 형태(sub/login_id/role)라 OAM 은 별도 경로 없이 require_role 로 검증한다.
    가입자 게이트(범위·감사)는 CSC 가 이미 수행했으므로 OAM 쪽 신원은 서비스 주체 하나다."""
    import time
    now = int(time.time())
    payload = {'sub': sub, 'login_id': f'{sub}-service', 'role': role, 'svc': sub,
               'iat': now, 'exp': now + max(30, int(ttl_sec))}
    tok = jwt.encode(payload, _SECRET, algorithm='HS256')
    return tok.decode('utf-8') if isinstance(tok, bytes) else tok


def extract_admin_jwt(headers: dict) -> Optional[dict]:
    """Authorization Bearer 헤더에서 JWT 추출 후 검증."""
    auth = headers.get('authorization', '') if headers else ''
    if auth.startswith('Bearer '):
        return verify_admin_jwt(auth[7:])
    return None
