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

import asyncio
import hashlib
import json
import os
import secrets
import socket
from datetime import datetime, timedelta
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

# Agent server cert 유효기간 — 1 년. CA (ca.crt) 는 10 년 유지.
# 짧게 두어야 rotation 로직이 실제로 의미를 가짐.
_AGENT_CERT_VALIDITY_DAYS = 365
# Rotation 임계치: 만료까지 남은 기간이 이 값 이하이면 heartbeat 응답에 rotate 지시.
_AGENT_CERT_ROTATE_THRESHOLD_DAYS = 30


# ──────────────────────────────────────────────────────────────
#  mTLS 인증서 유틸 — Phase C
#
#  CSC 가 자체 CA 를 생성/관리하며, 각 agent 에 개별 server-cert 를 발급.
#  Agent 는 그 cert 로 Sync REST 서버를 구동 + CA 를 신뢰 피어로 등록.
#  CSC 는 같은 CA 로 서명된 자기 client-cert 로 agent 서버에 mTLS 연결.
#
#  활성화 조건: csc.json 의 `AgentMtlsEnabled: true`.
#  인증서 경로: <csc-root>/cert/agent_mtls/{ca.crt, ca.key, csc_client.crt, csc_client.key}
# ──────────────────────────────────────────────────────────────

def _agent_mtls_dir(config: dict) -> str:
    base = (config.get("Agent") or {}).get("MtlsDir") or "cert/agent_mtls"
    if not os.path.isabs(base):
        # csc 바이너리 기준 상대 경로 → 현 프로세스 cwd 가 csc root
        base = os.path.abspath(base)
    return base


def _mtls_enabled(config: dict) -> bool:
    return bool((config.get("Agent") or {}).get("MtlsEnabled", False))


def _ensure_mtls_ca(config: dict) -> tuple:
    """CA 가 없으면 생성. (ca_cert_path, ca_key_path, csc_client_cert_path, csc_client_key_path) 반환."""
    import subprocess
    d = _agent_mtls_dir(config)
    os.makedirs(d, mode=0o700, exist_ok=True)
    ca_crt = os.path.join(d, "ca.crt")
    ca_key = os.path.join(d, "ca.key")
    client_crt = os.path.join(d, "csc_client.crt")
    client_key = os.path.join(d, "csc_client.key")
    if not (os.path.isfile(ca_crt) and os.path.isfile(ca_key)):
        # 10 년짜리 CA
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-days", "3650", "-subj", "/CN=CIMS Agent CA",
            "-keyout", ca_key, "-out", ca_crt,
        ], check=True, capture_output=True)
        os.chmod(ca_key, 0o600)
    if not (os.path.isfile(client_crt) and os.path.isfile(client_key)):
        # CSC 의 client cert 발급
        csr = os.path.join(d, "csc_client.csr")
        subprocess.run(["openssl", "genrsa", "-out", client_key, "2048"],
                       check=True, capture_output=True)
        os.chmod(client_key, 0o600)
        subprocess.run([
            "openssl", "req", "-new", "-key", client_key,
            "-subj", "/CN=csc-client", "-out", csr,
        ], check=True, capture_output=True)
        subprocess.run([
            "openssl", "x509", "-req", "-in", csr,
            "-CA", ca_crt, "-CAkey", ca_key, "-CAcreateserial",
            "-days", "3650", "-out", client_crt,
        ], check=True, capture_output=True)
        try: os.unlink(csr)
        except Exception: pass
    return ca_crt, ca_key, client_crt, client_key


def _issue_agent_server_cert(config: dict, agent_id: int, hostname: str) -> dict:
    """CA 로 서명된 agent server cert 발급. PEM 문자열 3개 (cert, key, ca) 반환."""
    import subprocess, tempfile
    ca_crt, ca_key, _, _ = _ensure_mtls_ca(config)
    d = _agent_mtls_dir(config)
    tmp = tempfile.mkdtemp(dir=d, prefix=f"agent_{agent_id}_")
    try:
        key_path  = os.path.join(tmp, "agent.key")
        csr_path  = os.path.join(tmp, "agent.csr")
        crt_path  = os.path.join(tmp, "agent.crt")
        subprocess.run(["openssl", "genrsa", "-out", key_path, "2048"],
                       check=True, capture_output=True)
        # CN = agent-<id>, SAN = hostname
        san = hostname if hostname else f"agent-{agent_id}"
        conf = (
            "[req]\ndistinguished_name=dn\nreq_extensions=v3_req\nprompt=no\n"
            f"[dn]\nCN=agent-{agent_id}\n"
            "[v3_req]\nsubjectAltName=@alt\n"
            f"[alt]\nDNS.1={san}\nIP.1=127.0.0.1\n"
        )
        conf_path = os.path.join(tmp, "req.cnf")
        with open(conf_path, "w") as f: f.write(conf)
        subprocess.run([
            "openssl", "req", "-new", "-key", key_path,
            "-config", conf_path, "-out", csr_path,
        ], check=True, capture_output=True)
        subprocess.run([
            "openssl", "x509", "-req", "-in", csr_path,
            "-CA", ca_crt, "-CAkey", ca_key, "-CAcreateserial",
            "-days", str(_AGENT_CERT_VALIDITY_DAYS),
            "-extensions", "v3_req", "-extfile", conf_path,
            "-out", crt_path,
        ], check=True, capture_output=True)

        def _read(p):
            with open(p, "r") as f: return f.read()
        return {
            "cert": _read(crt_path),
            "key":  _read(key_path),
            "ca":   _read(ca_crt),
        }
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)


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
    if endpoint == "cert" and len(parts) >= 2 and parts[1] == "rotate" and method == "POST":
        return await _cert_rotate(handler_args, config, agent)

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
    ifaces = body.get("interfaces")
    interfaces_json = json.dumps(ifaces, ensure_ascii=False) if isinstance(ifaces, list) else None

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
                "  interfaces_json=COALESCE(%s, interfaces_json), "
                "  status=IF(status='approved','online',status), "
                "  enrolled_at=NOW(), last_heartbeat=NOW() "
                "WHERE id=%s",
                (session_token, info["hostname"], info["ip_address"], info["os_info"],
                 info["cpu_cores"], info["memory_mb"], info["disk_gb"], info["agent_version"],
                 interfaces_json, row["id"])
            )
    finally:
        conn.close()

    logger.log_info(f"Agent enrolled: id={row['id']} name={row['name']} from={handler_args.client_ip}")

    resp_body = {
        "agent_id": row["id"],
        "name": row["name"],
        "session_token": session_token,
        "status": "online" if row["status"] == "approved" else row["status"],
        "heartbeat_interval_sec": 30,
    }
    # mTLS 활성화 시 agent 서버용 cert 발급해 함께 전달 + 레코드에 플래그/만료 기록
    if _mtls_enabled(config):
        try:
            mtls = await asyncio.to_thread(
                _issue_agent_server_cert, config, row["id"], info["hostname"])
            expires_at = datetime.now() + timedelta(days=_AGENT_CERT_VALIDITY_DAYS)
            resp_body["mtls"] = {
                "server_cert": mtls["cert"],
                "server_key":  mtls["key"],
                "ca_cert":     mtls["ca"],
            }
            conn2 = _get_db(config)
            try:
                with conn2.cursor() as cur2:
                    cur2.execute(
                        "UPDATE cims_agent SET mtls_enabled=1, "
                        "  cert_issued_at=NOW(), cert_expires_at=%s WHERE id=%s",
                        (expires_at, row["id"])
                    )
            finally:
                conn2.close()
            logger.log_info(f"Agent mTLS cert issued: id={row['id']} expires_at={expires_at:%Y-%m-%d}")
        except Exception as e:
            logger.log_info(f"Agent mTLS cert issue failed: {e}")

    return HandlerResult(status=200, body=resp_body, media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  /heartbeat — 주기 상태 보고 + pending job 조회
# ──────────────────────────────────────────────────────────────

async def _heartbeat(handler_args: HandlerArgs, config: dict, agent: dict) -> HandlerResult:
    body = _parse_body(handler_args)
    # agent 가 보고한 sync_port 저장
    sync_port = body.get("sync_port")
    try:
        sync_port = int(sync_port) if sync_port is not None else None
    except (TypeError, ValueError):
        sync_port = None
    ifaces = body.get("interfaces")
    interfaces_json = json.dumps(ifaces, ensure_ascii=False) if isinstance(ifaces, list) else None
    conn = _get_db(config)
    cert_rotate = False
    try:
        with conn.cursor() as cur:
            # 상태 갱신 + sync_port + interfaces 갱신
            if sync_port:
                cur.execute(
                    "UPDATE cims_agent SET last_heartbeat=NOW(), sync_port=%s, "
                    "  interfaces_json=COALESCE(%s, interfaces_json), "
                    "  status=CASE "
                    "    WHEN status IN ('offline','approved') THEN 'online' "
                    "    ELSE status END "
                    "WHERE id=%s", (sync_port, interfaces_json, agent["id"])
                )
            else:
                cur.execute(
                    "UPDATE cims_agent SET last_heartbeat=NOW(), "
                    "  interfaces_json=COALESCE(%s, interfaces_json), "
                    "  status=CASE "
                    "    WHEN status IN ('offline','approved') THEN 'online' "
                    "    ELSE status END "
                    "WHERE id=%s", (interfaces_json, agent["id"])
                )
            # cert rotation 플래그 조회
            cur.execute("SELECT cert_rotate_pending FROM cims_agent WHERE id=%s", (agent["id"],))
            r = cur.fetchone()
            cert_rotate = bool(r and r.get("cert_rotate_pending"))
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

    resp = {
        "ok": True,
        "jobs": [{"id": j["id"], "type": j["job_type"],
                   "params": json.loads(j["params"]) if j.get("params") else {}}
                  for j in jobs],
    }
    if cert_rotate:
        resp["cert_rotate"] = True
    return HandlerResult(status=200, body=resp, media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  /cert/rotate — agent 가 현재 session token 으로 새 mTLS cert 요청
# ──────────────────────────────────────────────────────────────

async def _cert_rotate(handler_args: HandlerArgs, config: dict, agent: dict) -> HandlerResult:
    if not _mtls_enabled(config):
        return HandlerResult(status=409, body={"error": "mtls_not_enabled_on_csc"},
                             media_type="application/json")
    if not agent.get("mtls_enabled"):
        return HandlerResult(status=409, body={"error": "mtls_not_enabled_on_agent"},
                             media_type="application/json")
    try:
        mtls = await asyncio.to_thread(
            _issue_agent_server_cert, config, agent["id"], agent.get("hostname") or "")
    except Exception as e:
        logger.log_error(f"Agent cert rotate failed: id={agent['id']} err={e}")
        return HandlerResult(status=500, body={"error": "cert_issue_failed"},
                             media_type="application/json")

    expires_at = datetime.now() + timedelta(days=_AGENT_CERT_VALIDITY_DAYS)
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE cims_agent SET cert_issued_at=NOW(), cert_expires_at=%s, "
                "  cert_rotate_pending=0 WHERE id=%s",
                (expires_at, agent["id"])
            )
    finally:
        conn.close()
    logger.log_info(f"Agent mTLS cert rotated: id={agent['id']} expires_at={expires_at:%Y-%m-%d}")
    return HandlerResult(status=200, body={
        "ok": True,
        "mtls": {
            "server_cert": mtls["cert"],
            "server_key":  mtls["key"],
            "ca_cert":     mtls["ca"],
        },
        "expires_at": expires_at.strftime("%Y-%m-%dT%H:%M:%S"),
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
                            # install 결과 stdout 에서 "at <path> (" 패턴으로 실제 install_path 추출
                            new_install_path = None
                            if j["job_type"] == "install" and result_stdout:
                                import re as _re
                                m = _re.search(r"at\s+(\S+?)\s+\(", result_stdout)
                                if m: new_install_path = m.group(1)
                            new_status = "running" if j["job_type"] in ("start","restart") else "stopped"
                            # install 직후는 "실행 전"이므로 stopped 로 보는 게 맞음
                            if new_install_path:
                                cur.execute(
                                    "UPDATE agent_deployment SET status=%s, "
                                    "  install_path=%s, deployed_at=NOW(), last_job_id=%s "
                                    "WHERE id=%s",
                                    (new_status, new_install_path, job_id, dep_id))
                            else:
                                cur.execute(
                                    "UPDATE agent_deployment SET status=%s, "
                                    "  deployed_at=NOW(), last_job_id=%s WHERE id=%s",
                                    (new_status, job_id, dep_id))
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
    from handlers.agents import _pkg_load
    pkg = _pkg_load(config, pid=pkg_id)
    if not pkg:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    path = pkg.get("file_path") or ""
    if not os.path.isfile(path):
        return HandlerResult(status=500, body={"error": "file_missing", "path": path},
                             media_type="application/json")
    with open(path, "rb") as f:
        data = f.read()
    return HandlerResult(status=200, body=data,
                         headers={
                             "Content-Type": "application/octet-stream",
                             "X-Package-Name": pkg.get("name"),
                             "X-Package-Version": pkg.get("version"),
                             "X-Package-Sha256": pkg.get("sha256"),
                         },
                         media_type="application/octet-stream")


# ──────────────────────────────────────────────────────────────
#  Handler list
# ──────────────────────────────────────────────────────────────

CIMS_AGENT_API_HANDLER_LIST = (
    (_AGENT_BASE, handle_agent, {}),
)
