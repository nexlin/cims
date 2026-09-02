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


def run_sql_script(db: dict, path: str) -> int:
    """SQL 스크립트 파일을 pymysql 로 실행 (MULTI_STATEMENTS — SET/PREPARE/EXECUTE 묶음 그대로).

    mysql CLI 의존 없이 마이그레이션 스크립트(sql/migrate_*.sql)를 돌린다. 반환 0=성공, 1=실패
    (예외 시 stderr 에 사유). 결과 집합은 전부 소비한다(버퍼 잔류 시 다음 쿼리 오류).
    """
    import sys
    try:
        import pymysql                                          # type: ignore
        from pymysql.constants import CLIENT                    # type: ignore
        with open(path, encoding="utf-8") as f:
            script = f.read()
        conn = pymysql.connect(
            host=db["Host"], port=int(db.get("Port", 3306)),
            user=db["User"], password=db["Password"], database=db["DbName"],
            autocommit=True, client_flag=CLIENT.MULTI_STATEMENTS,
        )
        try:
            cur = conn.cursor()
            cur.execute(script)
            while cur.nextset():
                pass
            return 0
        finally:
            try: conn.close()
            except Exception: pass
    except Exception as e:
        print(f"[db] run_sql_script {os.path.basename(path)} 실패: {type(e).__name__}: {e}", file=sys.stderr)
        return 1
