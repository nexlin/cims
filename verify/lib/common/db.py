"""DB 연결 helper.

CSP 의 csp.json Setup.Database 섹션에서 접속 정보 추출.
"""
from __future__ import annotations

import json
import os
from typing import Optional


def csp_db_config(dist_dir: str) -> dict:
    """build/dist/csp/config/csp.json 의 Database 섹션 반환."""
    csp_json = os.path.join(dist_dir, "csp", "config", "csp.json")
    try:
        with open(csp_json) as f:
            return json.load(f).get("Setup", {}).get("Database", {}) or {}
    except Exception:
        return {}


def connect(db: dict, *, autocommit: bool = True):
    """pymysql 커넥션 — 호출자가 close 책임.

    `autocommit` default True — verify 코드가 짧은 tx 단위로 UPDATE 직후
    SELECT 폴링하는 패턴이 많아 autocommit 가 안전. (기본 False 시 UPDATE
    후 commit 누락하면 close 시 rollback 되어 변경 사라짐.)
    """
    import pymysql                                              # type: ignore
    return pymysql.connect(
        host=db["Host"], port=int(db.get("Port", 3306)),
        user=db["User"], password=db["Password"], database=db["DbName"],
        autocommit=autocommit,
    )


class SqlScriptError(Exception):
    """SQL 스크립트 실행 실패. errno = MySQL 오류 번호(없으면 0).

    permission_denied — 실행 계정의 권한 부족(1044 DB 접근 거부·1142 명령 거부·1227 특권 필요).
    검증 항목은 이를 '스크립트 결함'이 아니라 '이 계정으로는 판정 불가'로 구분해 보고한다.
    """
    _PERMISSION_ERRNOS = (1044, 1142, 1227)

    def __init__(self, path: str, errno: int, message: str):
        super().__init__(f"{os.path.basename(path)}: [{errno}] {message}" if errno else
                         f"{os.path.basename(path)}: {message}")
        self.path = path
        self.errno = errno
        self.message = message

    @property
    def permission_denied(self) -> bool:
        return self.errno in self._PERMISSION_ERRNOS


def run_sql_script(db: dict, path: str) -> None:
    """SQL 스크립트 파일을 pymysql 로 실행 (MULTI_STATEMENTS — SET/PREPARE/EXECUTE 묶음 그대로).

    mysql CLI 의존 없이 마이그레이션 스크립트(sql/migrate_*.sql)를 돌린다. 실패 시 SqlScriptError
    (MySQL errno 동봉 — 권한 부족은 permission_denied). 결과 집합은 전부 소비한다(버퍼 잔류 시
    다음 쿼리 오류).
    """
    import pymysql                                              # type: ignore
    from pymysql.constants import CLIENT                        # type: ignore
    try:
        with open(path, encoding="utf-8") as f:
            script = f.read()
    except OSError as e:
        raise SqlScriptError(path, 0, f"{type(e).__name__}: {e}")
    try:
        conn = pymysql.connect(
            host=db["Host"], port=int(db.get("Port", 3306)),
            user=db["User"], password=db["Password"], database=db["DbName"],
            autocommit=True, client_flag=CLIENT.MULTI_STATEMENTS,
        )
    except pymysql.MySQLError as e:
        raise SqlScriptError(path, int(e.args[0]) if e.args and isinstance(e.args[0], int) else 0, str(e))
    try:
        cur = conn.cursor()
        cur.execute(script)
        while cur.nextset():
            pass
    except pymysql.MySQLError as e:
        raise SqlScriptError(path, int(e.args[0]) if e.args and isinstance(e.args[0], int) else 0,
                             str(e.args[1]) if len(e.args) > 1 else str(e))
    finally:
        try: conn.close()
        except Exception: pass


def column_exists(db: dict, table: str, column: str) -> bool:
    """현재 DB 의 table 에 column 이 있는지 (information_schema)."""
    conn = connect(db)
    try:
        cur = conn.cursor()
        cur.execute("SELECT COUNT(*) FROM information_schema.COLUMNS "
                    "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = %s AND COLUMN_NAME = %s",
                    (table, column))
        r = cur.fetchone()
        return bool(r and r[0])
    finally:
        conn.close()
