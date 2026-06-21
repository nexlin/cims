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
#  RBAC 역할 모델 (계층적 5종) — docs/design/features/mcptt_authorization.md §3
#    admin > manager > operator > monitor > user
#    user 는 telephony 전용으로 OAM 콘솔 로그인 불가.
# ─────────────────────────────────────────────────────────────
_ROLE_RANK = {'user': 0, 'monitor': 1, 'operator': 2, 'manager': 3, 'admin': 4}
ROLES = tuple(_ROLE_RANK.keys())


def role_rank(role: Optional[str]) -> int:
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


def extract_admin_jwt(headers: dict) -> Optional[dict]:
    """Authorization Bearer 헤더에서 JWT 추출 후 검증."""
    auth = headers.get('authorization', '') if headers else ''
    if auth.startswith('Bearer '):
        return verify_admin_jwt(auth[7:])
    return None
