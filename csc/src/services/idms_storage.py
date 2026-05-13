"""
IDMS Storage — 파일 기반 (file_store).

OAuth 2.0 / OIDC 의 auth-code / refresh-token 저장. 옛 MariaDB 버전을
파일 기반으로 전환 (2026-05-13, Phase 8 마이그레이션).

도메인 (file_store):
  auth_codes/<code>.json
  refresh_tokens/<token>.json

원자성: file_store.save 의 atomic write (tmp + rename) 로 충분.
"""

import time
from typing import Optional

from util.log_util import Logger
from services import file_store

logger = Logger()


class IdmsStorage:
    def __init__(self):
        self._config: dict = {}

    def init_db(self, config: dict):
        """호환을 위한 함수명. 실제로는 file_store runtime root 추출."""
        # 옛 시그니처는 CimsDatabase dict 만 받았으나, file_store 는 전체 csc config 필요.
        # 호출자가 db_cfg 만 주면 runtime_root 가 CWD 기준 fallback 로 잡힘.
        # csc_app.py 호출부에서 전체 config 를 전달하도록 갱신했음.
        self._config = config if isinstance(config, dict) else {}

    # ==================== Auth Code ====================

    def _auth_dir(self):
        return file_store.domain_dir(self._config, 'auth_codes')

    def save_auth_code(self, code: str, data: dict) -> bool:
        try:
            obj = dict(data)
            obj['code'] = code
            obj['used'] = bool(obj.get('used', False))
            file_store.save(self._auth_dir(), code, obj)
            return True
        except Exception as e:
            logger.log_error(f"save_auth_code: {e}")
            return False

    def get_auth_code(self, code: str) -> Optional[dict]:
        try:
            r = file_store.load(self._auth_dir(), code)
            if r is None:
                return None
            r['used'] = bool(r.get('used', False))
            return r
        except Exception as e:
            logger.log_error(f"get_auth_code: {e}")
            return None

    def delete_auth_code(self, code: str) -> bool:
        try:
            return file_store.delete(self._auth_dir(), code)
        except Exception as e:
            logger.log_error(f"delete_auth_code: {e}")
            return False

    def mark_auth_code_used(self, code: str) -> bool:
        try:
            r = file_store.load(self._auth_dir(), code)
            if not r:
                return False
            r['used'] = True
            file_store.save(self._auth_dir(), code, r)
            return True
        except Exception as e:
            logger.log_error(f"mark_auth_code_used: {e}")
            return False

    def cleanup_expired_codes(self) -> int:
        try:
            now = int(time.time())
            cnt = 0
            for r in file_store.load_all(self._auth_dir()):
                exp = r.get('expires_at')
                if exp is not None and int(exp) < now:
                    if file_store.delete(self._auth_dir(), r.get('code')):
                        cnt += 1
            if cnt:
                logger.log_info(f"Cleaned up {cnt} expired auth-codes")
            return cnt
        except Exception as e:
            logger.log_error(f"cleanup_expired_codes: {e}")
            return 0

    # ==================== Refresh Token ====================

    def _token_dir(self):
        return file_store.domain_dir(self._config, 'refresh_tokens')

    def save_refresh_token(self, token: str, data: dict) -> bool:
        try:
            obj = dict(data)
            obj['token_id'] = token
            obj['revoked'] = bool(obj.get('revoked', False))
            file_store.save(self._token_dir(), token, obj)
            return True
        except Exception as e:
            logger.log_error(f"save_refresh_token: {e}")
            return False

    def get_refresh_token(self, token: str) -> Optional[dict]:
        try:
            r = file_store.load(self._token_dir(), token)
            if r is None:
                return None
            r['revoked'] = bool(r.get('revoked', False))
            return r
        except Exception as e:
            logger.log_error(f"get_refresh_token: {e}")
            return None

    def revoke_refresh_token(self, token: str, rotated_to: Optional[str] = None) -> bool:
        try:
            r = file_store.load(self._token_dir(), token)
            if not r:
                return False
            r['revoked'] = True
            if rotated_to:
                r['rotated_to'] = rotated_to
            file_store.save(self._token_dir(), token, r)
            return True
        except Exception as e:
            logger.log_error(f"revoke_refresh_token: {e}")
            return False

    def cleanup_expired_tokens(self) -> int:
        try:
            now = int(time.time())
            cnt = 0
            for r in file_store.load_all(self._token_dir()):
                exp = r.get('expires_at')
                if (exp is not None and int(exp) < now) or r.get('revoked'):
                    if file_store.delete(self._token_dir(), r.get('token_id')):
                        cnt += 1
            if cnt:
                logger.log_info(f"Cleaned up {cnt} expired/revoked refresh-tokens")
            return cnt
        except Exception as e:
            logger.log_error(f"cleanup_expired_tokens: {e}")
            return 0
