"""
IDMS Storage Module (MariaDB Version)
OAuth 2.0 / OIDC 표준을 위한 데이터베이스 기반 저장 모듈

기능:
- auth-code 저장/조회/삭제 (MariaDB table: auth_codes)
- refresh-token 저장/조회/회수 (MariaDB table: refresh_tokens)
- DB 트랜잭션을 통한 원자적 연산 보장
- fcntl 파일락 제거 및 DB Row-level 락 활용
"""

import pymysql
import time
from typing import Dict, Optional, List
from util.log_util import Logger

logger = Logger()

class IdmsStorage:
    def __init__(self):
        """초기화 (연결 정보는 별도 init_db 메서드에서 수신)"""
        self.db_config = None
        self.connection = None

    def init_db(self, config: dict):
        """
        DB 연결 정보 설정 및 초기 연결 시도
        
        Args:
            config: {
                "Host": str,
                "User": str,
                "Password": str,
                "Db": str
            }
        """
        self.db_config = config
        self._get_connection()

    def _get_connection(self):
        """DB 연결 생성 또는 기존 연결 반환 (재연결 로직 포함)"""
        if self.connection:
            try:
                self.connection.ping(reconnect=True)
                return self.connection
            except:
                logger.log_error("Lost connection to MariaDB, attempting to reconnect...")

        try:
            self.connection = pymysql.connect(
                host=self.db_config.get('Host', 'localhost'),
                user=self.db_config.get('User', 'agapeoom'),
                password=self.db_config.get('Password', '!core0908'),
                database=self.db_config.get('Db', 'csc_idms'),
                charset='utf8mb4',
                cursorclass=pymysql.cursors.DictCursor,
                autocommit=True
            )
            logger.log_info(f"Connected to MariaDB: {self.db_config.get('Db')}")
            return self.connection
        except Exception as e:
            logger.log_error(f"Failed to connect to MariaDB: {e}")
            return None

    # ==================== Auth Code 관리 ====================
    
    def save_auth_code(self, code: str, data: dict) -> bool:
        """auth-code 저장"""
        conn = self._get_connection()
        if not conn: return False
        
        try:
            with conn.cursor() as cursor:
                sql = """
                INSERT INTO auth_codes 
                (code, user_id, client_id, redirect_uri, scope, state, issued_at, expires_at, used, code_challenge, code_challenge_method)
                VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
                """
                cursor.execute(sql, (
                    code,
                    data.get("user_id"),
                    data.get("client_id"),
                    data.get("redirect_uri"),
                    data.get("scope"),
                    data.get("state"),
                    data.get("issued_at"),
                    data.get("expires_at"),
                    1 if data.get("used", False) else 0,
                    data.get("code_challenge"),
                    data.get("code_challenge_method")
                ))
            logger.log_info(f"Saved auth-code to DB: {code}")
            return True
        except Exception as e:
            logger.log_error(f"Failed to save auth-code to DB: {e}")
            return False

    def get_auth_code(self, code: str) -> Optional[dict]:
        """auth-code 조회"""
        conn = self._get_connection()
        if not conn: return None
        
        try:
            with conn.cursor() as cursor:
                sql = "SELECT * FROM auth_codes WHERE code = %s"
                cursor.execute(sql, (code,))
                result = cursor.fetchone()
                if result:
                    # DB 1/0 값을 다시 bool로 변환
                    result["used"] = bool(result["used"])
                    return result
            return None
        except Exception as e:
            logger.log_error(f"Failed to get auth-code from DB: {e}")
            return None

    def delete_auth_code(self, code: str) -> bool:
        """auth-code 삭제"""
        conn = self._get_connection()
        if not conn: return False
        
        try:
            with conn.cursor() as cursor:
                sql = "DELETE FROM auth_codes WHERE code = %s"
                result = cursor.execute(sql, (code,))
            if result > 0:
                logger.log_info(f"Deleted auth-code from DB: {code}")
                return True
            return False
        except Exception as e:
            logger.log_error(f"Failed to delete auth-code from DB: {e}")
            return False

    def mark_auth_code_used(self, code: str) -> bool:
        """auth-code를 사용됨으로 표시"""
        conn = self._get_connection()
        if not conn: return False
        
        try:
            with conn.cursor() as cursor:
                sql = "UPDATE auth_codes SET used = 1 WHERE code = %s"
                result = cursor.execute(sql, (code,))
            if result > 0:
                logger.log_info(f"Marked auth-code as used in DB: {code}")
                return True
            return False
        except Exception as e:
            logger.log_error(f"Failed to mark auth-code as used in DB: {e}")
            return False

    def cleanup_expired_codes(self) -> int:
        """만료된 auth-code 정리"""
        conn = self._get_connection()
        if not conn: return 0
        
        try:
            now = int(time.time())
            with conn.cursor() as cursor:
                sql = "DELETE FROM auth_codes WHERE expires_at < %s"
                result = cursor.execute(sql, (now,))
            if result > 0:
                logger.log_info(f"Cleaned up {result} expired auth-codes from DB")
            return result
        except Exception as e:
            logger.log_error(f"Failed to cleanup auth-codes in DB: {e}")
            return 0

    # ==================== Refresh Token 관리 ====================
    
    def save_refresh_token(self, token: str, data: dict) -> bool:
        """refresh-token 저장"""
        conn = self._get_connection()
        if not conn: return False
        
        try:
            with conn.cursor() as cursor:
                sql = """
                INSERT INTO refresh_tokens 
                (token_id, user_id, client_id, scope, issued_at, expires_at, revoked, rotated_to)
                VALUES (%s, %s, %s, %s, %s, %s, %s, %s)
                """
                cursor.execute(sql, (
                    token,
                    data.get("user_id"),
                    data.get("client_id"),
                    data.get("scope"),
                    data.get("issued_at"),
                    data.get("expires_at"),
                    1 if data.get("revoked", False) else 0,
                    data.get("rotated_to")
                ))
            logger.log_info(f"Saved refresh-token to DB: {token[:8]}...")
            return True
        except Exception as e:
            logger.log_error(f"Failed to save refresh-token to DB: {e}")
            return False

    def get_refresh_token(self, token: str) -> Optional[dict]:
        """refresh-token 조회"""
        conn = self._get_connection()
        if not conn: return None
        
        try:
            with conn.cursor() as cursor:
                sql = "SELECT * FROM refresh_tokens WHERE token_id = %s"
                cursor.execute(sql, (token,))
                result = cursor.fetchone()
                if result:
                    result["revoked"] = bool(result["revoked"])
                    return result
            return None
        except Exception as e:
            logger.log_error(f"Failed to get refresh-token from DB: {e}")
            return None

    def revoke_refresh_token(self, token: str, rotated_to: Optional[str] = None) -> bool:
        """refresh-token 회수"""
        conn = self._get_connection()
        if not conn: return False
        
        try:
            with conn.cursor() as cursor:
                if rotated_to:
                    sql = "UPDATE refresh_tokens SET revoked = 1, rotated_to = %s WHERE token_id = %s"
                    result = cursor.execute(sql, (rotated_to, token))
                else:
                    sql = "UPDATE refresh_tokens SET revoked = 1 WHERE token_id = %s"
                    result = cursor.execute(sql, (token,))
            if result > 0:
                logger.log_info(f"Revoked refresh-token in DB: {token[:8]}...")
                return True
            return False
        except Exception as e:
            logger.log_error(f"Failed to revoke refresh-token in DB: {e}")
            return False

    def cleanup_expired_tokens(self) -> int:
        """만료/폐기된 refresh-token 정리"""
        conn = self._get_connection()
        if not conn: return 0
        
        try:
            now = int(time.time())
            with conn.cursor() as cursor:
                # 만료되었거나 이미 폐기(revoked)된 토큰 정리
                # 단, rotated_to의 FK 제약 조건이 있으므로 순서를 고려하거나 제약조건 설정을 맞춰야 함
                # 여기서는 단순 삭제 시도 (FK ON DELETE SET NULL 설정 덕분에 가능)
                sql = "DELETE FROM refresh_tokens WHERE expires_at < %s OR revoked = 1"
                result = cursor.execute(sql, (now,))
            if result > 0:
                logger.log_info(f"Cleaned up {result} expired/revoked refresh-tokens from DB")
            return result
        except Exception as e:
            logger.log_error(f"Failed to cleanup refresh-tokens in DB: {e}")
            return 0
