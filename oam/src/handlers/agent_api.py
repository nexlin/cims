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
    from handlers.agents import _agent_load
    headers_lower = {k.lower(): v for k, v in (handler_args.headers or {}).items()}
    token = headers_lower.get("x-agent-token")
    if not token:
        return None
    return _agent_load(config, agent_token=token)


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
    if endpoint == "csp-config" and len(parts) >= 2 and method == "GET":
        return await _csp_config_pull(parts[1], config, agent)
    if endpoint == "sync" and len(parts) >= 3 and parts[2] == "ack" and method == "POST":
        try:
            sid = int(parts[1])
        except ValueError:
            return HandlerResult(status=400, body={"error": "invalid_sync_id"},
                                 media_type="application/json")
        return await _sync_ack(handler_args, sid, config, agent)

    return HandlerResult(status=404, body={"error": "unknown_endpoint"},
                         media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  HA fan-out: 컬렉션 pull + sync ack
# ──────────────────────────────────────────────────────────────

# agent 가 sync_config job 처리 시 pull 할 수 있는 컬렉션 화이트리스트.
# 키 = URL 의 <collection> 토큰, 값 = file_store 도메인 이름.
_AGENT_PULL_COLLECTIONS = {
    "csp_listener":        "csp_listener",
    "sip_trunk":           "sip_trunk",
    "routing_rule":        "routing_rule",
    "routing_access_list": "routing_access_list",
    "sip_service":         "sip_service",
}


async def _csp_config_pull(collection: str, config: dict, agent: dict) -> HandlerResult:
    """GET /api/agent/csp-config/<collection>

    응답: { "collection": str, "items": [...], "count": int, "served_at": iso }
    헤더: ETag = items 의 결합 hash (caller 가 변경 감지에 사용)

    items 는 원본 file_store row 그대로 (CSP 가 jsonl 로 직렬화해 install_path 에 쓸 수 있음).
    """
    import hashlib
    from services import file_store

    dom = _AGENT_PULL_COLLECTIONS.get(collection)
    if not dom:
        return HandlerResult(status=404, body={"error": "unknown_collection", "collection": collection},
                             media_type="application/json")
    rows = await asyncio.to_thread(file_store.load_all, file_store.domain_dir(config, dom))
    # id 정렬 (csp 가 동일 순서로 jsonl 을 쓸 수 있도록)
    rows.sort(key=lambda r: r.get("id") or 0)
    # 직렬화된 본문 hash → etag
    payload = json.dumps(rows, ensure_ascii=False, sort_keys=True).encode("utf-8")
    etag = hashlib.sha256(payload).hexdigest()[:16]
    body = {
        "collection": collection,
        "items":      rows,
        "count":      len(rows),
        "etag":       etag,
        "served_at":  datetime.now().isoformat(timespec="seconds"),
    }
    return HandlerResult(status=200, body=body,
                         headers={"ETag": etag},
                         media_type="application/json")


async def _sync_ack(handler_args: HandlerArgs, sid: int,
                    config: dict, agent: dict) -> HandlerResult:
    """POST /api/agent/sync/<sync_id>/ack

    body = { "status": "ack"|"nack", "error"?: str }
    agent 자신 (X-Agent-Token) 의 slot 만 갱신. 다른 agent 의 slot 은 변경 불가.
    """
    from services import sync_txn
    try:
        body = json.loads(handler_args.body or b"{}")
        if not isinstance(body, dict):
            raise ValueError("body must be object")
    except Exception as e:
        return HandlerResult(status=400, body={"error": "invalid_json", "detail": str(e)},
                             media_type="application/json")
    status = (body.get("status") or "ack").lower()
    if status not in ("ack", "nack"):
        return HandlerResult(status=400, body={"error": "invalid_status",
                                                "detail": "status must be 'ack' or 'nack'"},
                             media_type="application/json")
    error = body.get("error") or None
    aid = agent.get("id")
    txn = await asyncio.to_thread(sync_txn.ack, config, sid, aid,
                                  status=status, error=error)
    if not txn:
        return HandlerResult(status=404, body={"error": "sync_not_found", "sync_id": sid},
                             media_type="application/json")
    return HandlerResult(status=200,
                         body={"ok": True, "sync_id": sid,
                               "status": txn.get("status"),
                               "members": txn.get("members")},
                         media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  /enroll — enrollment token → session token
# ──────────────────────────────────────────────────────────────

async def _enroll(handler_args: HandlerArgs, config: dict) -> HandlerResult:
    from handlers.agents import _agent_load, _agent_update
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

    row = await asyncio.to_thread(_agent_load, config, None, None, None, enroll_token)
    # status: pending/approved/online/offline 모두 허용 (offline = re-install 시나리오).
    # revoked 만 차단 — 명시적으로 차단된 record 는 token 재발급 받아도 enroll 불가.
    if not row or row.get('status') == 'revoked':
        return HandlerResult(status=401, body={"error": "invalid_enrollment_token"},
                             media_type="application/json")
    # TTL 검사 — expires_at 이 설정되어 있고 현재 시각보다 이전이면 만료
    expires_at_iso = row.get('enrollment_token_expires_at')
    if expires_at_iso:
        try:
            if datetime.fromisoformat(expires_at_iso) < datetime.now():
                return HandlerResult(status=401, body={"error": "enrollment_token_expired",
                                     "detail": "토큰 만료 — Console 에서 재발급 필요"},
                                     media_type="application/json")
        except Exception:
            pass

    # session token 발급. status 는 그대로 유지 — 'online' 으로의 승격은 첫 heartbeat
    # 도착 시 heartbeat handler 가 수행 (init.sh enroll-only 만 했을 때 false-online 방지).
    session_token = _new_token()
    now = datetime.now().isoformat(timespec='seconds')
    patches = {
        'agent_token': session_token,
        'enrollment_token': None,
        'hostname': info['hostname'],
        'ip_address': info['ip_address'],
        'os_info': info['os_info'],
        'cpu_cores': info['cpu_cores'],
        'memory_mb': info['memory_mb'],
        'disk_gb': info['disk_gb'],
        'agent_version': info['agent_version'],
        # status 는 enroll 시점에 전환하지 않음. 'approved' 그대로 → 첫 hb 시 'online'.
        'enrolled_at': now,
        # last_heartbeat 는 실제 heartbeat 도착 시에만 update — enrollment 자체로 채우면
        # init.sh (enroll-only) 만 했는데도 hb 가 있는 것처럼 보이는 부수효과 → 명시적 분리.
    }
    if isinstance(ifaces, list):
        patches['interfaces'] = _normalize_interface_roles(ifaces, config, row)
    row = await asyncio.to_thread(_agent_update, config, row['id'], patches)

    logger.log_info(f"Agent enrolled: id={row['id']} name={row['name']} from={handler_args.client_ip}")

    # 신규/재 enroll 시 agent 가 멤버인 ha-group 에 update_ha job 자동 큐잉.
    # 옛 동작은 그룹/멤버 변경 시점에만 큐잉 → fresh install 후 ha.json sync 안 됐던 버그.
    try:
        from services import file_store
        from handlers.ha_groups import _ha_dir, _enqueue_update_ha_for_members
        def _sync_ha():
            groups = file_store.load_all(_ha_dir(config))
            n = 0
            for g in groups:
                if any(m.get('agent_id') == row['id'] for m in (g.get('members') or [])):
                    n += _enqueue_update_ha_for_members(g['id'], config)
            return n
        n_jobs = await asyncio.to_thread(_sync_ha)
        if n_jobs:
            logger.log_info(f"[enroll] ha sync: queued {n_jobs} update_ha job(s) for agent {row['id']}")
    except Exception as e:
        logger.log_warning(f"[enroll] ha sync trigger failed for agent {row['id']}: {e}")

    resp_body = {
        "agent_id": row["id"],
        "name": row["name"],
        "session_token": session_token,
        "status": row.get("status"),
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
            await asyncio.to_thread(_agent_update, config, row['id'], {
                'mtls_enabled': 1,
                'cert_issued_at': datetime.now().isoformat(timespec='seconds'),
                'cert_expires_at': expires_at.isoformat(timespec='seconds'),
            })
            logger.log_info(f"Agent mTLS cert issued: id={row['id']} expires_at={expires_at:%Y-%m-%d}")
        except Exception as e:
            logger.log_info(f"Agent mTLS cert issue failed: {e}")

    return HandlerResult(status=200, body=resp_body, media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  /heartbeat — 주기 상태 보고 + pending job 조회
# ──────────────────────────────────────────────────────────────

def _normalize_interface_roles(ifaces: list, config: dict, agent: dict | None) -> list:
    """interfaces 의 role 정규화.

    Phase 4d 정책:
    - mgmt 자동: agent 가 보낸 mgmt:true (cims_agent.detect_mgmt_ip 결과)
      또는 IP 가 Mgmt.Cidr 안 → role='mgmt'.
    - admin override 유지: agent.interface_role_overrides 에 박힌 {ip: role}
      이 agent 가 보낸 자동 mgmt 보다 우선 (단 mgmt 망에서 mgmt 가 아닌 role
      박는 건 비정상이므로 mgmt 자동이 최종 winner).
    - service/internal 등 다른 role 은 override 에서만 결정 (agent 자율 추론 없음).
    """
    if not isinstance(ifaces, list):
        return ifaces
    import ipaddress as _ipaddress
    mgmt_net = config.get('_mgmt_net') if isinstance(config, dict) else None
    overrides = (agent or {}).get('interface_role_overrides') or {}
    out = []
    for it in ifaces:
        if not isinstance(it, dict):
            out.append(it); continue
        new_it = dict(it)
        ip = new_it.get('ip')
        # admin override 우선
        ov = overrides.get(ip)
        # mgmt 자동: agent 의 mgmt:true 또는 IP ∈ Mgmt.Cidr
        is_mgmt = bool(new_it.get('mgmt'))
        if not is_mgmt and ip and mgmt_net is not None:
            try:
                if _ipaddress.ip_address(ip) in mgmt_net:
                    is_mgmt = True
                    new_it['mgmt'] = True  # 정규화 — agent 자율 도출 실패해도 server 강제
            except (ValueError, TypeError):
                pass
        # 최종 role 결정
        if is_mgmt:
            new_it['role'] = 'mgmt'
        elif ov:
            new_it['role'] = ov
        # 그 외 role 미지정 (admin 명시 안 한 IP)
        out.append(new_it)
    return out


async def _heartbeat(handler_args: HandlerArgs, config: dict, agent: dict) -> HandlerResult:
    from handlers.agents import _agent_update
    body = _parse_body(handler_args)
    # agent 가 보고한 sync_port 저장
    sync_port = body.get("sync_port")
    try:
        sync_port = int(sync_port) if sync_port is not None else None
    except (TypeError, ValueError):
        sync_port = None
    ifaces = body.get("interfaces")
    routes = body.get("routes")
    ver = (body.get("agent_version") or "").strip()
    now = datetime.now().isoformat(timespec='seconds')
    patches = {'last_heartbeat': now}
    if sync_port:
        patches['sync_port'] = sync_port
    if isinstance(ifaces, list):
        patches['interfaces'] = _normalize_interface_roles(ifaces, config, agent)
    if isinstance(routes, list):
        patches['routes'] = routes
    # agent_version 도 매 heartbeat 시 갱신 — update.sh 후 새 버전 즉시 반영.
    if ver:
        patches['agent_version'] = ver[:32]
    # heartbeat 가 도착했다는 건 enrollment 가 끝났고 token 검증을 통과했다는 의미 →
    # pending 도 즉시 online 으로 자동 전환 (별도 admin approve 절차 불필요).
    if agent.get('status') in ('offline', 'approved', 'pending'):
        patches['status'] = 'online'
        if not agent.get('approved_at'):
            patches['approved_at'] = now

    from handlers.agents import _job_pick_pending
    def _update_and_pick():
        updated = _agent_update(config, agent['id'], patches) or {}
        cert_rotate = bool(updated.get('cert_rotate_pending'))
        # pending job pick — file_store
        jobs = _job_pick_pending(config, agent['id'], limit=10)
        return cert_rotate, jobs

    cert_rotate, jobs = await asyncio.to_thread(_update_and_pick)

    def _job_params(p):
        if isinstance(p, (dict, list)):
            return p
        if isinstance(p, str) and p:
            try: return json.loads(p)
            except Exception: return {}
        return {}
    resp = {
        "ok": True,
        "jobs": [{"id": j["id"], "type": j["job_type"],
                   "params": _job_params(j.get("params"))}
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
    from handlers.agents import _agent_update
    await asyncio.to_thread(_agent_update, config, agent['id'], {
        'cert_issued_at': datetime.now().isoformat(timespec='seconds'),
        'cert_expires_at': expires_at.isoformat(timespec='seconds'),
        'cert_rotate_pending': 0,
    })
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
    from handlers.agents import _job_load, _job_update, _deploy_update
    body = _parse_body(handler_args)
    job_id = body.get("job_id")
    status = (body.get("status") or "succeeded").lower()
    if status not in ("succeeded", "failed", "cancelled"):
        status = "failed"
    result_code   = int(body.get("result_code") or 0)
    result_stdout = (body.get("stdout") or "")[:65000]
    result_stderr = (body.get("stderr") or "")[:65000]

    def _do_report():
        j = _job_load(config, job_id) if job_id else None
        if not j or j.get('agent_id') != agent['id']:
            return 0, None
        from datetime import datetime as _dt
        now = _dt.now().isoformat(timespec='seconds')
        _job_update(config, job_id, {
            'status': status,
            'result_code': result_code,
            'result_stdout': result_stdout,
            'result_stderr': result_stderr,
            'completed_at': now,
        })
        return 1, j

    changed, j = await asyncio.to_thread(_do_report)

    # 배포 상태 업데이트 훅 (install 성공 시 deployment.status=running 전환)
    if changed and status == "succeeded" and j:
        jt = j.get("job_type")
        params = j.get("params") if isinstance(j.get("params"), (dict, list)) else {}
        if isinstance(params, str):
            try: params = json.loads(params)
            except Exception: params = {}
        dep_id = params.get("deployment_id") if isinstance(params, dict) else None
        # OAM 분리 Phase 4 fix: 'upgrade' 도 install 처럼 status=stopped 전이.
        # 누락 시 upgrade 후 status=deploying 으로 stuck → 다음 job 안 만들어짐.
        if dep_id and jt in ("install", "upgrade", "start", "restart"):
            new_install_path = None
            if jt in ("install", "upgrade") and result_stdout:
                import re as _re
                m = _re.search(r"at\s+(\S+?)\s+\(", result_stdout)
                if m: new_install_path = m.group(1)
            new_status = "running" if jt in ("start","restart") else "stopped"
            patches = {'status': new_status, 'last_job_id': job_id}
            from datetime import datetime as _dt
            patches['deployed_at'] = _dt.now().isoformat(timespec='seconds')
            if new_install_path:
                patches['install_path'] = new_install_path
            await asyncio.to_thread(_deploy_update, config, dep_id, patches)
        elif dep_id and jt in ("stop", "uninstall"):
            new_status = "removed" if jt == "uninstall" else "stopped"
            await asyncio.to_thread(_deploy_update, config, dep_id,
                                    {'status': new_status, 'last_job_id': job_id})

    return HandlerResult(status=200, body={"ok": True, "updated": changed},
                         media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  /metric — 리소스 메트릭
# ──────────────────────────────────────────────────────────────

async def _metric(handler_args: HandlerArgs, config: dict, agent: dict) -> HandlerResult:
    body = _parse_body(handler_args)
    from handlers.agents import _metric_append, _agent_update
    procs = body.get("processes") or []
    per_iface = body.get("per_iface") or []
    modules = body.get("modules") or []
    mounts = body.get("mounts") or []
    record = {
        'cpu_pct': body.get("cpu_pct"),
        'mem_pct': body.get("mem_pct"),
        'disk_pct': body.get("disk_pct"),
        'load_avg': (body.get("load_avg") or "")[:32],
        'processes': procs if isinstance(procs, list) else [],
        'per_iface': per_iface if isinstance(per_iface, list) else [],
        'modules':   modules if isinstance(modules, list) else [],
        'mounts':    mounts if isinstance(mounts, list) else [],
    }
    await asyncio.to_thread(_metric_append, config, agent['id'], record)
    await asyncio.to_thread(_agent_update, config, agent['id'], {
        'last_metric': datetime.now().isoformat(timespec='seconds'),
    })
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
