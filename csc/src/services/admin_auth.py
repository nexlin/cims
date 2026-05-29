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


_SECRET = 'cims_jwt_secret_change_me'  # config 로 갱신


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
