"""
CSC Agent API — 배포 에이전트용 엔드포인트 (P10).

Agent 와 CSC 간 통신 프로토콜:
  POST /api/agent/enroll     — 최초 등록 (enrollment token → session token + agent_id)
  POST /api/agent/heartbeat  — 주기 heartbeat (30s) — pending job 수신
  POST /api/agent/report     — 작업 결과 보고
  GET  /api/agent/package/{id} — 패키지 다운로드

인증:
  - /enroll: enrollment_token (Console 에서 발급한 1회용)
  - 나머지: X-Agent-Token (enroll 응답의 session token)
  - 향후 mTLS 로 강화 (P10 후속)
"""

from __future__ import annotations

import hashlib
import json
import os
import secrets
import socket
from datetime import datetime
from urllib.parse import urlparse

import pymysql
import pymysql.cursors

from httpsrv.handler import HandlerArgs, HandlerResult
from util.log_util import Logger

logger = Logger()

_AGENT_BASE = "/api/agent"

# PKG 파일 저장 루트 (admin API 에서도 공유)
_PKG_STORE = os.environ.get("CIMS_PKG_STORE",
                            "/home/nex/work/cims/build/dist/csc/packages")


def _get_db(config: dict):
    db = config.get("CimsDatabase", {})
    return pymysql.connect(
        host=db.get("Host", "127.0.0.1"), port=int(db.get("Port", 3306)),
        user=db.get("User", "cims"), password=db.get("Password", ""),
        database=db.get("Db", "cims"),
        charset="utf8mb4", cursorclass=pymysql.cursors.DictCursor, autocommit=True,
    )


def _parse_body(handler_args: HandlerArgs) -> dict:
    body = handler_args.body
    if body is None: return {}
    if isinstance(body, dict): return body
    if isinstance(body, (bytes, bytearray)):
        try: return json.loads(body.decode("utf-8"))
        except Exception: return {}
    if isinstance(body, str):
        try: return json.loads(body)
        except Exception: return {}
    return {}


def _new_token() -> str:
    return secrets.token_hex(32)


def _check_session(handler_args: HandlerArgs, config: dict):
    """X-Agent-Token 헤더 검증. 유효하면 (agent_row) 반환, 아니면 None."""
    headers_lower = {k.lower(): v for k, v in (handler_args.headers or {}).items()}
    token = headers_lower.get("x-agent-token")
    if not token:
        return None
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM cims_agent WHERE agent_token=%s AND status != 'revoked'",
                        (token,))
            return cur.fetchone()
    finally:
        conn.close()


# ──────────────────────────────────────────────────────────────
#  handler
# ──────────────────────────────────────────────────────────────

async def handle_agent(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    path = urlparse(handler_args.full_path).path
    rel = path[len(_AGENT_BASE):].strip("/")
    parts = rel.split("/") if rel else []
    method = handler_args.method.upper()

    if not parts:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")

    endpoint = parts[0]

    if endpoint == "enroll" and method == "POST":
        return await _enroll(handler_args, config)

    # 이하 엔드포인트는 session token 필수
    agent = _check_session(handler_args, config)
    if not agent:
        return HandlerResult(status=401, body={"error": "invalid_agent_token"},
                             media_type="application/json")

    if endpoint == "heartbeat" and method == "POST":
        return await _heartbeat(handler_args, config, agent)
    if endpoint == "report" and method == "POST":
        return await _report(handler_args, config, agent)
    if endpoint == "package" and len(parts) >= 2 and method == "GET":
        try: pkg_id = int(parts[1])
        except ValueError:
            return HandlerResult(status=400, body={"error": "invalid_package_id"},
                                 media_type="application/json")
        return await _package_download(pkg_id, config)
    if endpoint == "metric" and method == "POST":
        return await _metric(handler_args, config, agent)

    return HandlerResult(status=404, body={"error": "unknown_endpoint"},
                         media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  /enroll — enrollment token → session token
# ──────────────────────────────────────────────────────────────

async def _enroll(handler_args: HandlerArgs, config: dict) -> HandlerResult:
    body = _parse_body(handler_args)
    enroll_token = (body.get("enrollment_token") or "").strip()
    if not enroll_token:
        return HandlerResult(status=400, body={"error": "enrollment_token required"},
                             media_type="application/json")

    # 호스트 정보 수집
    info = {
        "hostname":      (body.get("hostname") or "").strip()[:128],
        "ip_address":    handler_args.client_ip,
        "os_info":       (body.get("os_info") or "").strip()[:255],
        "cpu_cores":     int(body.get("cpu_cores") or 0),
        "memory_mb":     int(body.get("memory_mb") or 0),
        "disk_gb":       int(body.get("disk_gb") or 0),
        "agent_version": (body.get("agent_version") or "").strip()[:32],
    }

    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            # enrollment_token 매칭
            cur.execute("SELECT * FROM cims_agent WHERE enrollment_token=%s "
                        "AND status IN ('pending','approved')", (enroll_token,))
            row = cur.fetchone()
            if not row:
                return HandlerResult(status=401, body={"error": "invalid_enrollment_token"},
                                     media_type="application/json")

            # session token 발급 + 상태 online
            session_token = _new_token()
            cur.execute(
                "UPDATE cims_agent SET "
                "  agent_token=%s, enrollment_token=NULL, "
                "  hostname=%s, ip_address=%s, os_info=%s, "
                "  cpu_cores=%s, memory_mb=%s, disk_gb=%s, agent_version=%s, "
                "  status=IF(status='approved','online',status), "
                "  enrolled_at=NOW(), last_heartbeat=NOW() "
                "WHERE id=%s",
                (session_token, info["hostname"], info["ip_address"], info["os_info"],
                 info["cpu_cores"], info["memory_mb"], info["disk_gb"], info["agent_version"],
                 row["id"])
            )
    finally:
        conn.close()

    logger.log_info(f"Agent enrolled: id={row['id']} name={row['name']} from={handler_args.client_ip}")
    return HandlerResult(status=200, body={
        "agent_id": row["id"],
        "name": row["name"],
        "session_token": session_token,
        "status": "online" if row["status"] == "approved" else row["status"],
        "heartbeat_interval_sec": 30,
    }, media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  /heartbeat — 주기 상태 보고 + pending job 조회
# ──────────────────────────────────────────────────────────────

async def _heartbeat(handler_args: HandlerArgs, config: dict, agent: dict) -> HandlerResult:
    body = _parse_body(handler_args)
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            # 상태 갱신
            cur.execute(
                "UPDATE cims_agent SET last_heartbeat=NOW(), "
                "  status=CASE "
                "    WHEN status IN ('offline','approved') THEN 'online' "
                "    ELSE status END "
                "WHERE id=%s", (agent["id"],)
            )
            # pending job 최대 10개 pick
            cur.execute(
                "SELECT id, job_type, params FROM agent_job "
                "WHERE agent_id=%s AND status='queued' "
                "ORDER BY id LIMIT 10", (agent["id"],)
            )
            jobs = cur.fetchall()
            # 디스패치 상태 마킹
            if jobs:
                ids = [j["id"] for j in jobs]
                cur.execute(
                    f"UPDATE agent_job SET status='running', dispatched_at=NOW() "
                    f"WHERE id IN ({','.join(['%s']*len(ids))})", ids
                )
    finally:
        conn.close()

    return HandlerResult(status=200, body={
        "ok": True,
        "jobs": [{"id": j["id"], "type": j["job_type"],
                   "params": json.loads(j["params"]) if j.get("params") else {}}
                  for j in jobs],
    }, media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  /report — 작업 결과 보고
# ──────────────────────────────────────────────────────────────

async def _report(handler_args: HandlerArgs, config: dict, agent: dict) -> HandlerResult:
    body = _parse_body(handler_args)
    job_id = body.get("job_id")
    status = (body.get("status") or "succeeded").lower()
    if status not in ("succeeded", "failed", "cancelled"):
        status = "failed"
    result_code   = int(body.get("result_code") or 0)
    result_stdout = (body.get("stdout") or "")[:65000]
    result_stderr = (body.get("stderr") or "")[:65000]

    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE agent_job SET status=%s, result_code=%s, "
                "  result_stdout=%s, result_stderr=%s, completed_at=NOW() "
                "WHERE id=%s AND agent_id=%s",
                (status, result_code, result_stdout, result_stderr, job_id, agent["id"])
            )
            changed = cur.rowcount
            # 배포 상태 업데이트 훅 (install 성공 시 deployment.status=running 전환)
            if changed and status == "succeeded":
                cur.execute("SELECT job_type, params FROM agent_job WHERE id=%s", (job_id,))
                j = cur.fetchone()
                if j and j["job_type"] in ("install", "start", "restart"):
                    try:
                        params = json.loads(j["params"]) if j.get("params") else {}
                        dep_id = params.get("deployment_id")
                        if dep_id:
                            cur.execute(
                                "UPDATE agent_deployment SET status='running', "
                                "  deployed_at=NOW(), last_job_id=%s WHERE id=%s",
                                (job_id, dep_id))
                    except Exception:
                        pass
                elif j and j["job_type"] in ("stop", "uninstall"):
                    try:
                        params = json.loads(j["params"]) if j.get("params") else {}
                        dep_id = params.get("deployment_id")
                        if dep_id:
                            new_status = "removed" if j["job_type"] == "uninstall" else "stopped"
                            cur.execute(
                                "UPDATE agent_deployment SET status=%s, last_job_id=%s WHERE id=%s",
                                (new_status, job_id, dep_id))
                    except Exception:
                        pass
    finally:
        conn.close()

    return HandlerResult(status=200, body={"ok": True, "updated": changed},
                         media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  /metric — 리소스 메트릭
# ──────────────────────────────────────────────────────────────

async def _metric(handler_args: HandlerArgs, config: dict, agent: dict) -> HandlerResult:
    body = _parse_body(handler_args)
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO agent_metric (agent_id, ts, cpu_pct, mem_pct, disk_pct, "
                "                          load_avg, processes_json) "
                "VALUES (%s, NOW(), %s, %s, %s, %s, %s)",
                (agent["id"],
                 body.get("cpu_pct"), body.get("mem_pct"), body.get("disk_pct"),
                 (body.get("load_avg") or "")[:32],
                 json.dumps(body.get("processes") or []))
            )
            cur.execute("UPDATE cims_agent SET last_metric=NOW() WHERE id=%s", (agent["id"],))
    finally:
        conn.close()
    return HandlerResult(status=200, body={"ok": True}, media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  /package/{id} — PKG 다운로드 (stream)
# ──────────────────────────────────────────────────────────────

async def _package_download(pkg_id: int, config: dict) -> HandlerResult:
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM cims_package WHERE id=%s", (pkg_id,))
            pkg = cur.fetchone()
    finally:
        conn.close()
    if not pkg:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    path = pkg["file_path"]
    if not os.path.isfile(path):
        return HandlerResult(status=500, body={"error": "file_missing", "path": path},
                             media_type="application/json")
    with open(path, "rb") as f:
        data = f.read()
    return HandlerResult(status=200, body=data,
                         headers={
                             "Content-Type": "application/octet-stream",
                             "X-Package-Name": pkg["name"],
                             "X-Package-Version": pkg["version"],
                             "X-Package-Sha256": pkg["sha256"],
                         },
                         media_type="application/octet-stream")


# ──────────────────────────────────────────────────────────────
#  Handler list
# ──────────────────────────────────────────────────────────────

CIMS_AGENT_API_HANDLER_LIST = (
    (_AGENT_BASE, handle_agent, {}),
)
