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


def connect(db: dict):
    """pymysql 커넥션 — 호출자가 close 책임."""
    import pymysql                                              # type: ignore
    return pymysql.connect(
        host=db["Host"], port=int(db.get("Port", 3306)),
        user=db["User"], password=db["Password"], database=db["DbName"],
    )
