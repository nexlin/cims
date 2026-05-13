"""
CSC Agent/Package/Deployment Admin API (P10).

  /api/v1/agents                GET list / POST create (enrollment token 발급)
  /api/v1/agents/{id}           GET / PUT / DELETE
  /api/v1/agents/{id}/approve   POST — pending → approved
  /api/v1/agents/{id}/revoke    POST — revoked
  /api/v1/agents/{id}/metrics   GET — 최근 리소스 메트릭

  /api/v1/packages              GET list / POST upload (multipart or base64)
  /api/v1/packages/{id}         GET / PUT (config_template/description) / DELETE

  /api/v1/deployments           GET list / POST create (agent × package)
  /api/v1/deployments/{id}      GET / PUT / DELETE
  /api/v1/deployments/{id}/job  POST — job (install/start/stop/restart/uninstall) 큐잉

Admin JWT 인증 (기존 pi_http pre-hook 재사용).
"""

from __future__ import annotations

import base64
import asyncio
import hashlib
import json
import os
import secrets
from typing import Optional
from urllib.parse import urlparse, unquote
from pathlib import PurePath

import pymysql
import pymysql.cursors

from httpsrv.handler import HandlerArgs, HandlerResult
from util.log_util import Logger

logger = Logger()

_AGENT_BASE       = "/api/v1/agents"
_PACKAGE_BASE     = "/api/v1/packages"
_DEPLOYMENT_BASE  = "/api/v1/deployments"

_DEFAULT_PKG_DIR    = "packages"
_DEFAULT_BACKUP_DIR = "packages_trash"
# CSC 루트 = 이 파일이 있는 handlers/ 의 두 단계 부모 (csc/src/handlers → csc/)
_COMPONENT_ROOT = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)


def _resolve_pkg_paths(config: dict) -> tuple:
    """csc.json 의 Packages.{Dir,BackupDir} 를 읽어 절대 경로 반환.

    우선순위:
      1) csc.json → Packages.Dir / Packages.BackupDir
      2) 환경변수 CIMS_PKG_STORE / CIMS_PKG_BACKUP
      3) 기본: <csc-root>/packages , <csc-root>/packages_trash

    상대 경로는 **CSC 컴포넌트 루트** (dist/csc/) 기준으로 해석 — `ConfigCacheDir`
    등 다른 설정과 일관된 방식.
    """
    pkg = config.get("Packages") or {}
    active  = pkg.get("Dir")       or os.environ.get("CIMS_PKG_STORE")  or _DEFAULT_PKG_DIR
    backup  = pkg.get("BackupDir") or os.environ.get("CIMS_PKG_BACKUP") or _DEFAULT_BACKUP_DIR
    if not os.path.isabs(active):  active = os.path.normpath(os.path.join(_COMPONENT_ROOT, active))
    if not os.path.isabs(backup):  backup = os.path.normpath(os.path.join(_COMPONENT_ROOT, backup))
    return active, backup


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


def _path_tail(full_path: str, base: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _dt(val):
    return val.isoformat() if val is not None else None


def _actor(handler_args: HandlerArgs) -> str:
    auth = (handler_args.headers or {}).get("Authorization") or \
           (handler_args.headers or {}).get("authorization") or ""
    if auth.lower().startswith("bearer "):
        token = auth[7:].strip()
        try:
            import jwt as _jwt
            d = _jwt.decode(token, options={"verify_signature": False})
            return d.get("sub") or d.get("login_id") or "admin"
        except Exception:
            pass
    return "admin"


# ════════════════════════════════════════════════════════════
#  Agents
# ════════════════════════════════════════════════════════════

def _agent_to_json(r: dict, ha_group: dict | None = None) -> dict:
    def _safe_load(raw):
        if not raw: return None
        try: return json.loads(raw)
        except (TypeError, ValueError): return None
    return {
        "id": r["id"],
        "name": r["name"],
        "status": r["status"],
        "hostname": r["hostname"],
        "ip_address": r["ip_address"],
        "os_info": r["os_info"],
        "cpu_cores": r["cpu_cores"],
        "memory_mb": r["memory_mb"],
        "disk_gb": r["disk_gb"],
        "agent_version": r["agent_version"],
        "last_heartbeat": _dt(r["last_heartbeat"]),
        "last_metric":    _dt(r["last_metric"]),
        "enrolled_at":    _dt(r["enrolled_at"]),
        "approved_at":    _dt(r["approved_at"]),
        "note": r["note"],
        "create_time": _dt(r["create_time"]),
        # 보안: enrollment_token 은 생성 직후에만 반환. 여기서는 masked
        "has_pending_enrollment": bool(r.get("enrollment_token")),
        # HA 그룹 정보 — 미정의 시 null
        "ha_group": ha_group,
        # HaServicesPage 용 확장 필드 (없으면 null)
        "interfaces":      _safe_load(r.get("interfaces_json")),
        "service_ip_rows": _safe_load(r.get("service_ip_rows_json")),
    }


async def handle_agents(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    tail = _path_tail(handler_args.full_path, _AGENT_BASE)
    method = handler_args.method.upper()

    if not tail:
        if method == "GET":  return await _list_agents(config)
        if method == "POST": return await _create_agent(handler_args, config)
        return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")

    try: aid = int(tail[0])
    except (TypeError, ValueError):
        return HandlerResult(status=400, body={"error": "invalid_id"}, media_type="application/json")

    if len(tail) == 1:
        if method == "GET":    return await _get_agent(aid, config)
        if method == "PUT":    return await _update_agent(handler_args, aid, config)
        if method == "DELETE": return await _delete_agent(handler_args, aid, config)
    elif len(tail) == 2:
        action = tail[1]
        if action == "approve" and method == "POST":
            return await _approve_agent(handler_args, aid, config)
        if action == "revoke" and method == "POST":
            return await _revoke_agent(handler_args, aid, config)
        if action == "metrics" and method == "GET":
            return await _agent_metrics(aid, config)
        if action == "upgrade" and method == "POST":
            return await _upgrade_agent_binary(handler_args, aid, config)
        if action == "apply-ip-config" and method == "POST":
            return await _apply_ip_config(aid, config)
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


def _ha_group_map_for_agents(cur) -> dict:
    """모든 agent 의 ha_group {id,name,mode,role} 매핑. dict[agent_id] = {...}"""
    cur.execute(
        "SELECT m.agent_id, g.id AS gid, g.name AS gname, g.mode AS gmode, "
        "       m.role AS grole "
        "FROM ha_group_members m JOIN ha_groups g ON g.id=m.group_id"
    )
    out = {}
    for r in cur.fetchall():
        out[r["agent_id"]] = {
            "id": r["gid"], "name": r["gname"], "mode": r["gmode"], "role": r["grole"],
        }
    return out


async def _list_agents(config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM cims_agent ORDER BY id")
            rows = cur.fetchall()
            ha_map = _ha_group_map_for_agents(cur)
    finally:
        conn.close()
    return HandlerResult(status=200,
                         body={"items": [_agent_to_json(r, ha_group=ha_map.get(r["id"])) for r in rows]},
                         media_type="application/json")


async def _get_agent(aid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM cims_agent WHERE id=%s", (aid,))
            r = cur.fetchone()
            ha_map = _ha_group_map_for_agents(cur) if r else {}
    finally:
        conn.close()
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200,
                         body=_agent_to_json(r, ha_group=ha_map.get(aid)),
                         media_type="application/json")


async def _create_agent(handler_args: HandlerArgs, config):
    """Agent 레코드 생성 + enrollment_token 발급 → install-agent.sh 에 전달용."""
    body = _parse_body(handler_args)
    name = (body.get("name") or "").strip()
    if not name:
        return HandlerResult(status=400, body={"error": "name required"}, media_type="application/json")
    enroll_token = secrets.token_hex(24)
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO cims_agent (name, enrollment_token, status, agent_token, note) "
                "VALUES (%s,%s,'pending',%s,%s)",
                (name, enroll_token, secrets.token_hex(32), body.get("note"))
            )
            new_id = cur.lastrowid
            cur.execute("SELECT * FROM cims_agent WHERE id=%s", (new_id,))
            row = cur.fetchone()
    except pymysql.err.IntegrityError as e:
        return HandlerResult(status=409, body={"error": "conflict", "detail": str(e)},
                             media_type="application/json")
    finally:
        conn.close()
    result = _agent_to_json(row)
    # enrollment_token 은 최초 생성 시만 반환
    result["enrollment_token"] = enroll_token
    csc_url = _csc_public_url(handler_args, config)
    result["install_command"]  = (
        f"curl -k {csc_url}/install-agent.sh | "
        f"bash -s -- --csc-url {csc_url} "
        f"--enrollment-token {enroll_token} --name {name}"
    )
    return HandlerResult(status=201, body=result, media_type="application/json")


def _csc_public_url(handler_args: HandlerArgs, config: dict) -> str:
    """install 명령에 쓸 CSC 외부 URL. 우선순위:
       1) config.Server.PublicUrl (명시적 설정 시)
       2) HTTP Host 헤더 (admin 이 접속한 주소 그대로)
       3) config.Server.Ip + Port (0.0.0.0 면 placeholder)
    """
    srv = (config.get("Server") or {})
    pu = (srv.get("PublicUrl") or "").strip()
    if pu:
        return pu.rstrip("/")
    hdr_host = ""
    for k, v in (handler_args.headers or {}).items():
        if k.lower() == "host":
            hdr_host = v.strip(); break
    scheme = "https"
    if hdr_host:
        return f"{scheme}://{hdr_host}"
    ip = srv.get("Ip") or "0.0.0.0"
    port = srv.get("Port") or 4420
    if ip == "0.0.0.0" or not ip:
        return f"https://<CSC_HOST>:{port}"
    return f"https://{ip}:{port}"


async def _update_agent(handler_args: HandlerArgs, aid: int, config):
    body = _parse_body(handler_args)
    fields = []; values = []
    for col in ("name", "note"):
        if col in body:
            fields.append(f"{col}=%s"); values.append(body[col])
    # HaServicesPage 운영자가 설정한 iface→slot 매핑 (서비스 IP rows)
    if "service_ip_rows" in body:
        rows = body.get("service_ip_rows")
        fields.append("service_ip_rows_json=%s")
        values.append(json.dumps(rows, ensure_ascii=False) if rows is not None else None)
    if not fields:
        return HandlerResult(status=400, body={"error": "no_updatable_fields"}, media_type="application/json")
    values.append(aid)
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(f"UPDATE cims_agent SET {', '.join(fields)} WHERE id=%s", values)
            cur.execute("SELECT * FROM cims_agent WHERE id=%s", (aid,))
            row = cur.fetchone()
    finally:
        conn.close()
    if not row:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body=_agent_to_json(row), media_type="application/json")


async def _delete_agent(handler_args: HandlerArgs, aid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("DELETE FROM cims_agent WHERE id=%s", (aid,))
    finally:
        conn.close()
    return HandlerResult(status=204, body=None, media_type="application/json")


async def _approve_agent(handler_args: HandlerArgs, aid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE cims_agent SET status='approved', approved_at=NOW() "
                "WHERE id=%s AND status='pending'", (aid,))
            changed = cur.rowcount
    finally:
        conn.close()
    return HandlerResult(status=200 if changed else 409,
                         body={"ok": bool(changed), "id": aid}, media_type="application/json")


async def _revoke_agent(handler_args: HandlerArgs, aid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("UPDATE cims_agent SET status='revoked' WHERE id=%s", (aid,))
    finally:
        conn.close()
    return HandlerResult(status=200, body={"ok": True, "id": aid}, media_type="application/json")


async def _apply_ip_config(aid: int, config):
    """ServiceIpPanel [적용] 진입점 — cims_agent.service_ip_rows_json 을 읽어
    apply_ip_config job 큐잉. Agent 가 ip addr add 로 secondary IP 적용."""
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT id, name, service_ip_rows_json FROM cims_agent WHERE id=%s", (aid,))
            row = cur.fetchone()
            if not row:
                return HandlerResult(status=404, body={"error": "agent_not_found"},
                                     media_type="application/json")
            raw = row.get("service_ip_rows_json")
            try:
                rows = json.loads(raw) if raw else []
            except (TypeError, ValueError):
                rows = []
            if not rows:
                return HandlerResult(status=400, body={"error": "no_service_ip_rows"},
                                     media_type="application/json")
            cur.execute(
                "INSERT INTO agent_job (agent_id, job_type, params, status) "
                "VALUES (%s, 'apply_ip_config', %s, 'queued')",
                (aid, json.dumps({"service_ip_rows": rows}, ensure_ascii=False))
            )
            job_id = cur.lastrowid
    finally:
        conn.close()
    return HandlerResult(status=202,
                         body={"agent_id": aid, "job_id": job_id, "rows": len(rows)},
                         media_type="application/json")


async def _upgrade_agent_binary(handler_args: HandlerArgs, aid: int, config):
    """Agent 자기 바이너리 업그레이드 job 큐잉.
    Agent 가 heartbeat 로 pickup → /cims_agent.py 다운로드 → 자기 교체 → 종료 → systemd 재기동."""
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT id, name FROM cims_agent WHERE id=%s", (aid,))
            row = cur.fetchone()
            if not row:
                return HandlerResult(status=404, body={"error": "agent_not_found"},
                                     media_type="application/json")
            cur.execute(
                "INSERT INTO agent_job (agent_id, job_type, params, status) "
                "VALUES (%s, 'upgrade_agent', %s, 'queued')",
                (aid, json.dumps({}))
            )
            job_id = cur.lastrowid
    finally:
        conn.close()
    logger.log_info(f"[agent-upgrade] queued job_id={job_id} agent_id={aid} name={row['name']}")
    return HandlerResult(status=202,
                         body={"ok": True, "agent_id": aid, "job_id": job_id,
                               "hint": "agent 가 다음 heartbeat 에서 pickup 후 재시작됩니다 (수 초 내)"},
                         media_type="application/json")


async def _agent_metrics(aid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT ts, cpu_pct, mem_pct, disk_pct, load_avg, processes_json "
                "FROM agent_metric WHERE agent_id=%s "
                "ORDER BY ts DESC LIMIT 120", (aid,))
            rows = cur.fetchall()
    finally:
        conn.close()
    def _row(r):
        return {
            "ts": _dt(r["ts"]),
            "cpu_pct": r["cpu_pct"], "mem_pct": r["mem_pct"], "disk_pct": r["disk_pct"],
            "load_avg": r["load_avg"],
            "processes": json.loads(r["processes_json"]) if r["processes_json"] else [],
        }
    return HandlerResult(status=200, body={"items": [_row(r) for r in rows]},
                         media_type="application/json")


# ════════════════════════════════════════════════════════════
#  Packages
# ════════════════════════════════════════════════════════════

def _package_to_json(r: dict, include_full: bool = True) -> dict:
    """Package row → JSON.

    include_full=True (default): meta_json / config_template_json 파싱하여 함께 반환.
      - 리스트 조회도 추가 모달에서 바로 써야 하므로 기본 포함.
      - 필요 시 include_full=False 로 최소 필드만 반환.
    """
    out = {
        "id": r["id"],
        "name": r["name"],
        "version": r["version"],
        "file_path": r["file_path"],
        "file_size": r["file_size"],
        "sha256": r["sha256"],
        "description": r["description"],
        "uploaded_by": r["uploaded_by"],
        "uploaded_at": _dt(r["uploaded_at"]),
    }
    if include_full:
        out["meta"]            = _safe_json(r.get("meta_json"))
        out["config_template"] = _safe_json(r.get("config_template_json"))
    return out


def _safe_json(raw):
    if not raw: return None
    if isinstance(raw, (dict, list)): return raw
    try: return json.loads(raw)
    except Exception: return None


async def handle_packages(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    tail = _path_tail(handler_args.full_path, _PACKAGE_BASE)
    method = handler_args.method.upper()

    if not tail:
        if method == "GET":  return await _list_packages(config)
        if method == "POST": return await _create_package(handler_args, config)
    else:
        try: pid = int(tail[0])
        except (TypeError, ValueError):
            return HandlerResult(status=400, body={"error": "invalid_id"}, media_type="application/json")
        if method == "GET":    return await _get_package(pid, config)
        if method == "PUT":    return await _update_package(handler_args, pid, config)
        if method == "DELETE": return await _delete_package(pid, config)
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


def _dist_root_for_packages() -> str:
    env = os.environ.get("CIMS_DIST_DIR")
    if env:
        return env
    return os.path.normpath(os.path.join(_COMPONENT_ROOT, ".."))


def _scan_dist_virtual_packages() -> list:
    """build/dist/<name>/pkg.json + config_template.json 을 읽어 synthetic package entry 로 반환.
       Phase 1 검증용 — 아직 tarball 을 만들지 않은 상태에서도 Console 모듈관리 가 버전/템플릿/설정을 표시하게 함.

       id 는 음수 (`-(offset+1)`) 로 발급 — 실제 cims_package row 와 충돌하지 않음.
       source='dist' 필드가 tarball upload 건과 구분된다.
    """
    import datetime
    root = _dist_root_for_packages()
    out = []
    try:
        entries = sorted(os.listdir(root))
    except FileNotFoundError:
        return out
    idx = 0
    for entry in entries:
        base = os.path.join(root, entry)
        pkg = os.path.join(base, "pkg.json")
        if not os.path.isfile(pkg):
            continue
        try:
            with open(pkg, "r", encoding="utf-8") as f:
                meta = json.load(f)
        except Exception:
            continue
        name = (meta.get("name") or entry).strip()
        if not name:
            continue
        # config_template.json 위치 (컴포넌트별 분기)
        tmpl = None
        for rel in ("config/config_template.json", "config_template.json"):
            tpath = os.path.join(base, rel)
            if os.path.isfile(tpath):
                try:
                    with open(tpath, "r", encoding="utf-8") as f:
                        tmpl = json.load(f)
                except Exception:
                    tmpl = None
                break
        try:
            mtime = datetime.datetime.utcfromtimestamp(os.path.getmtime(pkg)).isoformat(timespec="seconds") + "Z"
        except OSError:
            mtime = None
        idx += 1
        out.append({
            "id":            -idx,
            "name":          name,
            "version":       meta.get("version") or "",
            "file_path":     base,
            "file_size":     0,
            "sha256":        "",
            "description":   meta.get("description") or "",
            "uploaded_by":   "dist",
            "uploaded_at":   mtime,
            "source":        "dist",
            "meta":          meta,
            "config_template": tmpl,
        })
    return out


async def _list_packages(config):
    """cims_package (DB, tarball 업로드 분) + build/dist 디렉토리 스캔 결과를 합쳐 반환.
       동일 name 이 dist + DB 에 모두 있으면 둘 다 돌려준다 (frontend 에서 source 로 구분).
    """
    def _q():
        conn = _get_db(config)
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT * FROM cims_package ORDER BY name, id DESC")
                return cur.fetchall()
        finally:
            conn.close()
    try:
        rows = await asyncio.to_thread(_q)
    except Exception as e:
        logger.log_error(f"list_packages db fallback failed: {e}")
        rows = []
    db_items = [dict(_package_to_json(r), source="db") for r in rows]
    dist_items = await asyncio.to_thread(_scan_dist_virtual_packages)
    # dist 먼저 (같은 name 이면 DB 최신 버전이 그 아래 정렬되어 비교 가능)
    items = dist_items + db_items
    items.sort(key=lambda x: (x.get("name", ""), 0 if x.get("source") == "dist" else 1))
    return HandlerResult(status=200, body={"items": items}, media_type="application/json")


async def _get_package(pid: int, config):
    # pid < 0 → dist 가상 package. _list_packages 의 index 와 동일하게 스캔해서 찾는다.
    if pid < 0:
        items = await asyncio.to_thread(_scan_dist_virtual_packages)
        for it in items:
            if it.get("id") == pid:
                return HandlerResult(status=200, body=it, media_type="application/json")
        return HandlerResult(status=404, body={"error": "dist_not_found"}, media_type="application/json")
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM cims_package WHERE id=%s", (pid,))
            r = cur.fetchone()
    finally:
        conn.close()
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body=_package_to_json(r), media_type="application/json")


def _extract_meta_from_tarball(raw: bytes) -> dict | None:
    """tar.gz 최상위의 meta.json 을 파싱. 실패 시 None."""
    import io, tarfile
    try:
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:gz") as tf:
            m = tf.getmember("meta.json")
            f = tf.extractfile(m)
            if not f: return None
            return json.loads(f.read().decode("utf-8"))
    except Exception:
        return None


def _extract_config_template_from_tarball(raw: bytes) -> dict | None:
    """tar.gz 최상위의 config_template.json 을 파싱 (없으면 None)."""
    import io, tarfile
    try:
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:gz") as tf:
            try:
                m = tf.getmember("config_template.json")
            except KeyError:
                return None
            f = tf.extractfile(m)
            if not f: return None
            return json.loads(f.read().decode("utf-8"))
    except Exception:
        return None


# ─── 동기 블로킹 작업들 (thread executor 에서 호출) ─────────────
def _read_file(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def _write_and_hash(fpath: str, raw: bytes) -> tuple:
    """디스크 쓰기 + SHA256 한 번에. (sha_hex, size) 반환."""
    sha = hashlib.sha256()
    with open(fpath, "wb") as f:
        # 1MB chunk 로 나누어 해싱 + 기록 (CPU/IO 교차)
        mv = memoryview(raw)
        chunk = 1 << 20
        for i in range(0, len(mv), chunk):
            part = mv[i:i+chunk]
            f.write(part)
            sha.update(part)
    return sha.hexdigest(), len(raw)


def _pkg_existing(config, name: str, version: str):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT id, sha256, uploaded_at, uploaded_by FROM cims_package "
                        "WHERE name=%s AND version=%s", (name, version))
            return cur.fetchone()
    finally:
        conn.close()


def _pkg_upsert(config, name: str, version: str, fpath: str, fsize: int,
                fsha: str, full_desc: str, actor: str,
                meta_json: str | None, template_json: str | None):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO cims_package (name, version, file_path, file_size, sha256, "
                "                          description, uploaded_by, "
                "                          meta_json, config_template_json) "
                "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s) "
                "ON DUPLICATE KEY UPDATE file_path=VALUES(file_path), file_size=VALUES(file_size), "
                "  sha256=VALUES(sha256), description=VALUES(description), "
                "  uploaded_by=VALUES(uploaded_by), uploaded_at=NOW(), "
                "  meta_json=VALUES(meta_json), config_template_json=VALUES(config_template_json)",
                (name, version, fpath, fsize, fsha, full_desc, actor,
                 meta_json, template_json)
            )
            cur.execute("SELECT * FROM cims_package WHERE name=%s AND version=%s", (name, version))
            return cur.fetchone()
    finally:
        conn.close()


async def _create_package(handler_args: HandlerArgs, config):
    import time as _t
    _t_handler_start = _t.monotonic()
    """패키지 업로드 — tar.gz 루트의 meta.json 이 권위 소스.

    우선순위:
      1) 업로드된 tarball 안의 meta.json (name/version/description/changelog/build_date/git_*)
      2) body.name / body.version / body.description (fallback; meta 없는 레거시 패키지용)

    충돌 정책:
      - (name, version) 중복 시 기본 409 반환
      - body.force=true 이면 덮어쓰기
    """
    # 원시 바이너리 업로드 (application/octet-stream) 는 body == bytes
    if isinstance(handler_args.body, (bytes, bytearray)):
        body = {"file": bytes(handler_args.body)}
        # query string 에서 force / filename 파싱
        qp = handler_args.query_params or {}
        if "force" in qp: body["force"] = qp["force"]
    else:
        body = _parse_body(handler_args)
    # force: JSON 에선 bool, multipart/query 에선 문자열 "true"/"false"
    _f = body.get("force")
    if isinstance(_f, str):
        force = _f.lower() in ("true", "1", "yes", "on")
    else:
        force = bool(_f)

    # 1) 원본 바이트 확보 — multipart (file=bytes), JSON (file_base64), 로컬 경로 (file_path)
    raw: bytes = b""
    if isinstance(body.get("file"), (bytes, bytearray)):
        raw = body["file"] if isinstance(body["file"], bytes) else bytes(body["file"])
    elif body.get("file_base64"):
        # base64 디코딩도 대용량이면 blocking → thread 로 offload
        try:
            raw = await asyncio.to_thread(base64.b64decode, body["file_base64"])
        except Exception as e:
            return HandlerResult(status=400, body={"error": f"invalid_base64: {e}"},
                                 media_type="application/json")
    elif body.get("file_path"):
        src = body["file_path"]
        if not os.path.isfile(src):
            return HandlerResult(status=400, body={"error": "file_path not found"},
                                 media_type="application/json")
        raw = await asyncio.to_thread(_read_file, src)
    else:
        return HandlerResult(status=400,
                             body={"error": "file / file_base64 / file_path 중 하나 필요"},
                             media_type="application/json")


    # 2) meta.json + config_template.json 파싱 (tarball decompress → thread 로 offload)
    meta     = await asyncio.to_thread(_extract_meta_from_tarball, raw) or {}
    template = await asyncio.to_thread(_extract_config_template_from_tarball, raw)
    name        = (meta.get("name")        or body.get("name")        or "").strip()
    version     = (meta.get("version")     or body.get("version")     or "").strip()
    description = (meta.get("description") or body.get("description") or "")
    changelog   = (meta.get("changelog")   or body.get("changelog")   or "")
    build_date  = meta.get("build_date")
    git_sha     = meta.get("git_sha")
    git_branch  = meta.get("git_branch")

    if not name or not version:
        return HandlerResult(
            status=400,
            body={"error": "meta.json 또는 name/version 필요",
                  "hint": "cims.sh pkg -v <ver> 로 meta.json 포함 패키지 생성"},
            media_type="application/json")

    # 3) 중복 검사 (DB 쿼리 — event loop 블록 방지 위해 thread 로 offload)
    existing = await asyncio.to_thread(_pkg_existing, config, name, version)
    if existing and not force:
        return HandlerResult(
            status=409,
            body={"error": "version_conflict",
                  "name": name, "version": version,
                  "existing_id": existing["id"],
                  "existing_sha256": existing["sha256"],
                  "uploaded_at": _dt(existing["uploaded_at"]),
                  "uploaded_by": existing["uploaded_by"],
                  "hint": "버전을 올리거나 force=true 로 덮어쓰기"},
            media_type="application/json")

    # 4) 디스크 쓰기 + SHA256 한 패스 (blocking → thread)
    pkg_dir, _ = _resolve_pkg_paths(config)
    os.makedirs(pkg_dir, exist_ok=True)
    fname = f"{name}-{version}.tar.gz"
    fpath = os.path.join(pkg_dir, fname)
    fsha, fsize = await asyncio.to_thread(_write_and_hash, fpath, raw)
    actor = _actor(handler_args)

    # 5) description 에 meta 정보 조합
    desc_lines = []
    if description: desc_lines.append(description)
    if build_date:  desc_lines.append(f"build: {build_date}")
    if git_sha:     desc_lines.append(f"git: {git_sha}" + (f" ({git_branch})" if git_branch else ""))
    if changelog:   desc_lines.append(f"changelog: {changelog}")
    full_desc = " | ".join(desc_lines)[:255]

    # 6) DB insert/update (blocking → thread)
    meta_str     = json.dumps(meta, ensure_ascii=False) if meta else None
    template_str = json.dumps(template, ensure_ascii=False) if template else None
    row = await asyncio.to_thread(
        _pkg_upsert, config, name, version, fpath, fsize, fsha, full_desc, actor,
        meta_str, template_str,
    )

    result = _package_to_json(row)
    logger.log_info(f"[pkg-upload] done {name} {version} size={fsize} "
                    f"template={'yes' if template else 'no'} "
                    f"handler_ms={int((_t.monotonic()-_t_handler_start)*1000)}")
    return HandlerResult(status=201, body=result, media_type="application/json")


def _move_to_backup(file_path: str, backup_dir: str) -> str:
    """활성 파일을 백업 디렉토리로 이동. 파일명은 <원본>.<timestamp>.bak 형태.
    성공 시 새 경로 반환, 실패 시 빈 문자열."""
    import shutil, time as _t
    if not file_path or not os.path.isfile(file_path):
        return ""
    os.makedirs(backup_dir, exist_ok=True)
    ts = _t.strftime("%Y%m%d-%H%M%S")
    base = os.path.basename(file_path)
    dst = os.path.join(backup_dir, f"{base}.{ts}.bak")
    # 같은 초 내 중복 가능성: 숫자 suffix
    i = 1
    while os.path.exists(dst):
        dst = os.path.join(backup_dir, f"{base}.{ts}-{i}.bak")
        i += 1
    try:
        shutil.move(file_path, dst)
        return dst
    except Exception:
        return ""


async def _update_package(handler_args: HandlerArgs, pid: int, config):
    """패키지 메타/설정 템플릿 수정. 파일·sha256 은 불변 (재업로드로 교체)."""
    if pid < 0:
        return HandlerResult(status=400,
            body={"error": "dist_package_readonly",
                  "hint": "build/dist 모듈은 pkg.json / config_template.json 파일 수정 → cims.sh build 로 반영"},
            media_type="application/json")
    try:
        body = handler_args.body
        if isinstance(body, (bytes, bytearray)):
            body = body.decode("utf-8")
        if isinstance(body, str):
            body = json.loads(body) if body.strip() else {}
        body = body or {}
    except Exception as e:
        return HandlerResult(status=400, body={"error": f"invalid_body: {e}"}, media_type="application/json")

    updates = []
    params: list = []
    if "description" in body:
        d = body.get("description")
        if d is not None and not isinstance(d, str):
            return HandlerResult(status=400, body={"error": "description_must_be_string"}, media_type="application/json")
        updates.append("description=%s")
        params.append(d)
    if "config_template" in body:
        tmpl = body.get("config_template")
        if tmpl is None:
            updates.append("config_template_json=NULL")
        else:
            if not isinstance(tmpl, dict):
                return HandlerResult(status=400, body={"error": "config_template_must_be_object"}, media_type="application/json")
            updates.append("config_template_json=%s")
            params.append(json.dumps(tmpl, ensure_ascii=False))

    if not updates:
        return HandlerResult(status=400, body={"error": "nothing_to_update"}, media_type="application/json")

    params.append(pid)
    sql = f"UPDATE cims_package SET {', '.join(updates)} WHERE id=%s"

    def _run_update():
        conn = _get_db(config)
        try:
            with conn.cursor() as cur:
                cur.execute(sql, params)
                affected = cur.rowcount
            conn.commit()
            return affected
        finally:
            conn.close()

    affected = await asyncio.to_thread(_run_update)
    if affected == 0:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return await _get_package(pid, config)


async def _delete_package(pid: int, config):
    if pid < 0:
        return HandlerResult(status=400,
            body={"error": "dist_package_readonly",
                  "hint": "build/dist 모듈은 파일시스템에서 직접 삭제하세요"},
            media_type="application/json")
    # DB 에서 메타 조회 + 삭제 (blocking → thread)
    row = await asyncio.to_thread(_pkg_delete_row, config, pid)
    if row is None:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")

    # 실제 파일을 백업 디렉토리로 이동 (재업로드 시 충돌 방지)
    _, backup_dir = _resolve_pkg_paths(config)
    moved = await asyncio.to_thread(_move_to_backup, row.get("file_path") or "", backup_dir)
    if moved:
        logger.log_info(f"[pkg-delete] id={pid} {row.get('name')}-{row.get('version')} "
                        f"→ backup: {moved}")
    else:
        logger.log_info(f"[pkg-delete] id={pid} {row.get('name')}-{row.get('version')} "
                        f"(원본 파일 없음 or 이동 실패)")
    return HandlerResult(status=204, body=None, media_type="application/json")


def _pkg_delete_row(config, pid: int):
    """DB 에서 패키지 행 조회 + 삭제. 삭제된 row dict 반환 (없으면 None)."""
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM cims_package WHERE id=%s", (pid,))
            r = cur.fetchone()
            if not r:
                return None
            cur.execute("DELETE FROM cims_package WHERE id=%s", (pid,))
            return r
    finally:
        conn.close()


# ════════════════════════════════════════════════════════════
#  Deployments
# ════════════════════════════════════════════════════════════

def _deployment_to_json(r: dict) -> dict:
    return {
        "id": r["id"],
        "agent_id":     r["agent_id"],
        "agent_name":   r.get("agent_name"),
        "package_id":   r["package_id"],
        "package_name": r.get("package_name"),
        "package_version": r.get("package_version"),
        "instance_id":  r["instance_id"],
        "instance_name": r.get("instance_name"),
        "process_name": r.get("process_name"),
        "service_functions": _split_csv(r.get("service_functions")),
        "status":       r["status"],
        "install_path": r["install_path"],
        "deployed_at":  _dt(r["deployed_at"]),
        "last_job_id":  r["last_job_id"],
        "note":         r["note"],
        "config":       _safe_json(r.get("config_json")),
        "config_applied_at": _dt(r.get("config_applied_at")),
        "create_time":  _dt(r["create_time"]),
    }


def _split_csv(s):
    if not s: return []
    return [x.strip() for x in str(s).split(",") if x.strip()]


def _join_csv(lst):
    if not lst: return ""
    if isinstance(lst, str): return lst
    return ",".join(str(x).strip() for x in lst if str(x).strip())


async def handle_deployments(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    tail = _path_tail(handler_args.full_path, _DEPLOYMENT_BASE)
    method = handler_args.method.upper()

    if not tail:
        if method == "GET":  return await _list_deployments(config)
        if method == "POST": return await _create_deployment(handler_args, config)
    else:
        try: did = int(tail[0])
        except (TypeError, ValueError):
            return HandlerResult(status=400, body={"error": "invalid_id"}, media_type="application/json")
        if len(tail) == 1:
            if method == "GET":    return await _get_deployment(did, config)
            if method == "PUT":    return await _update_deployment(handler_args, did, config)
            if method == "DELETE": return await _delete_deployment(did, config)
        elif len(tail) == 2 and tail[1] == "job" and method == "POST":
            return await _queue_job(handler_args, did, config)
        elif len(tail) == 2 and tail[1] == "config":
            if method == "GET":  return await _get_deployment_config(did, config)
            if method == "PUT":  return await _put_deployment_config(handler_args, did, config)
        elif len(tail) == 3 and tail[1] == "collection":
            name = tail[2]
            if method == "GET":  return await _get_deployment_collection(did, name, config)
            if method == "PUT":  return await _put_deployment_collection(handler_args, did, name, config)
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


async def _get_deployment_config(did: int, config):
    """해당 배포의 현재 설정 값 + 참조 템플릿을 함께 반환."""
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT d.config_json, d.config_applied_at, "
                "       p.config_template_json, p.meta_json "
                "FROM agent_deployment d "
                "LEFT JOIN cims_package p ON d.package_id = p.id "
                "WHERE d.id=%s", (did,))
            r = cur.fetchone()
    finally:
        conn.close()
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200,
        body={
            "config":             _safe_json(r.get("config_json")) or {},
            "config_applied_at":  _dt(r.get("config_applied_at")),
            "template":           _safe_json(r.get("config_template_json")),
            "meta":               _safe_json(r.get("meta_json")),
        },
        media_type="application/json")


async def _put_deployment_config(handler_args, did: int, config):
    """설정 값 저장. body = { "config": {<key>: <value>, ...}, "queue_update"?: bool }

    queue_update=true (기본) 이면 update_config job 을 자동 큐잉.
    """
    body = _parse_body(handler_args)
    values = body.get("config")
    if not isinstance(values, dict):
        return HandlerResult(status=400, body={"error": "config dict required"},
                             media_type="application/json")
    queue_update = body.get("queue_update", True)

    conn = _get_db(config)
    job_id = None
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM agent_deployment WHERE id=%s", (did,))
            dep = cur.fetchone()
            if not dep:
                return HandlerResult(status=404, body={"error": "not_found"},
                                     media_type="application/json")
            cur.execute(
                "UPDATE agent_deployment SET config_json=%s WHERE id=%s",
                (json.dumps(values, ensure_ascii=False), did)
            )
            if queue_update:
                cur.execute(_SELECT_DEPLOY + " WHERE d.id=%s", (did,))
                dep_full = cur.fetchone()
                params = {
                    "deployment_id": did,
                    "package_id":    dep_full["package_id"],
                    "package_name":  dep_full["package_name"],
                    "package_version": dep_full["package_version"],
                    "process_name":  dep_full.get("process_name"),
                    "service_functions": _split_csv(dep_full.get("service_functions")),
                    "install_path":  dep_full["install_path"],
                    "instance_id":   dep_full["instance_id"],
                    "config":        values,
                }
                cur.execute(
                    "INSERT INTO agent_job (agent_id, job_type, params, status) "
                    "VALUES (%s, 'update_config', %s, 'queued')",
                    (dep["agent_id"], json.dumps(params))
                )
                job_id = cur.lastrowid
    finally:
        conn.close()
    return HandlerResult(status=200,
        body={"ok": True, "job_id": job_id},
        media_type="application/json")


_SELECT_DEPLOY = ("""
    SELECT d.*, a.name AS agent_name,
           p.name AS package_name, p.version AS package_version,
           i.name AS instance_name
    FROM agent_deployment d
    LEFT JOIN cims_agent    a ON d.agent_id    = a.id
    LEFT JOIN cims_package  p ON d.package_id  = p.id
    LEFT JOIN cims_instance i ON d.instance_id = i.id
""")


async def _list_deployments(config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(_SELECT_DEPLOY + " ORDER BY d.id")
            rows = cur.fetchall()
    finally:
        conn.close()
    return HandlerResult(status=200, body={"items": [_deployment_to_json(r) for r in rows]},
                         media_type="application/json")


async def _get_deployment(did: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(_SELECT_DEPLOY + " WHERE d.id=%s", (did,))
            r = cur.fetchone()
    finally:
        conn.close()
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body=_deployment_to_json(r), media_type="application/json")


async def _create_deployment(handler_args: HandlerArgs, config):
    body = _parse_body(handler_args)
    agent_id     = body.get("agent_id")
    package_id   = body.get("package_id")
    instance_id  = body.get("instance_id")
    process_name = (body.get("process_name") or body.get("service_kind") or "").strip()
    functions    = _join_csv(body.get("service_functions"))
    install_path = (body.get("install_path") or "").strip() or None
    cfg_overlay  = body.get("config")
    config_json  = json.dumps(cfg_overlay) if isinstance(cfg_overlay, dict) and cfg_overlay else None
    if not agent_id or not package_id:
        return HandlerResult(status=400, body={"error": "agent_id and package_id required"},
                             media_type="application/json")
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            # HA capability 검증: 패키지 ha_capability 와 agent 가 속한 ha_group.mode mismatch 시 reject.
            # - active_standby 모듈은 A/S 그룹의 agent 에만
            # - all_active   모듈은 AA   그룹의 agent 에만
            # - standalone   모듈은 어느 그룹 (또는 그룹 없음) 에든 OK
            cur.execute(
                "SELECT meta_json FROM cims_package WHERE id=%s", (package_id,)
            )
            pkg = cur.fetchone()
            pkg_meta = {}
            if pkg and pkg.get("meta_json"):
                try: pkg_meta = json.loads(pkg["meta_json"])
                except Exception: pkg_meta = {}
            ha_cap = (pkg_meta.get("ha_capability") or "standalone").lower()
            cur.execute(
                "SELECT g.mode FROM ha_group_members m "
                "JOIN ha_groups g ON g.id=m.group_id WHERE m.agent_id=%s", (agent_id,)
            )
            grp = cur.fetchone()
            grp_mode = grp.get("mode") if grp else None
            # ha_group 정의된 agent 만 strict 검증. ha_group 미정의 시에는 모든 모듈 install
            # 허용 (운영자 워크플로: agent 등록 직후 그룹 정의 전 임시 install). Console UI
            # 가 HaGroupsPage 안내 + DeploymentCreateModal 의 hint 로 운영 가이드.
            if grp_mode is not None and ha_cap != "standalone" and ha_cap != grp_mode:
                return HandlerResult(status=400, body={
                    "error": "ha_mismatch",
                    "detail": f"패키지 ha_capability={ha_cap} 가 agent 그룹 mode={grp_mode} 와 불일치 "
                              f"(이 그룹에는 {grp_mode} 모듈만 install 가능)"
                }, media_type="application/json")

            cur.execute(
                "INSERT INTO agent_deployment (agent_id, package_id, instance_id, "
                "                              process_name, service_functions, "
                "                              install_path, config_json, note) "
                "VALUES (%s,%s,%s,%s,%s,%s,%s,%s)",
                (agent_id, package_id, instance_id, process_name, functions,
                 install_path, config_json, body.get("note"))
            )
            new_id = cur.lastrowid
            cur.execute(_SELECT_DEPLOY + " WHERE d.id=%s", (new_id,))
            r = cur.fetchone()
    except pymysql.err.IntegrityError as e:
        return HandlerResult(status=409, body={"error": "conflict", "detail": str(e)},
                             media_type="application/json")
    finally:
        conn.close()
    return HandlerResult(status=201, body=_deployment_to_json(r), media_type="application/json")


async def _update_deployment(handler_args: HandlerArgs, did: int, config):
    body = _parse_body(handler_args)
    fields = []; values = []
    # service_kind 는 하위호환 별칭 → process_name 으로 매핑
    if "service_kind" in body and "process_name" not in body:
        body["process_name"] = body["service_kind"]
    for col in ("instance_id", "process_name", "install_path", "note"):
        if col in body:
            fields.append(f"{col}=%s"); values.append(body[col])
    if "service_functions" in body:
        fields.append("service_functions=%s")
        values.append(_join_csv(body["service_functions"]))
    if not fields:
        return HandlerResult(status=400, body={"error": "no_updatable_fields"}, media_type="application/json")
    values.append(did)
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(f"UPDATE agent_deployment SET {', '.join(fields)} WHERE id=%s", values)
            cur.execute(_SELECT_DEPLOY + " WHERE d.id=%s", (did,))
            r = cur.fetchone()
    finally:
        conn.close()
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body=_deployment_to_json(r), media_type="application/json")


async def _delete_deployment(did: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("DELETE FROM agent_deployment WHERE id=%s", (did,))
    finally:
        conn.close()
    return HandlerResult(status=204, body=None, media_type="application/json")


async def _queue_job(handler_args: HandlerArgs, did: int, config):
    """Deployment 대상으로 job 큐잉 (install/start/stop/restart/uninstall)."""
    body = _parse_body(handler_args)
    job_type = (body.get("job_type") or "").lower()
    if job_type not in ("install", "upgrade", "uninstall", "start", "stop",
                         "restart", "update_config", "collect_log", "health_check"):
        return HandlerResult(status=400, body={"error": "invalid_job_type"},
                             media_type="application/json")

    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(_SELECT_DEPLOY + " WHERE d.id=%s", (did,))
            dep = cur.fetchone()
            if not dep:
                return HandlerResult(status=404, body={"error": "deployment_not_found"},
                                     media_type="application/json")

            params = {
                "deployment_id": did,
                "package_id":    dep["package_id"],
                "package_name":  dep["package_name"],
                "package_version": dep["package_version"],
                "process_name":  dep.get("process_name"),
                "service_functions": _split_csv(dep.get("service_functions")),
                "install_path":  dep["install_path"],
                "instance_id":   dep["instance_id"],
                "config":        _safe_json(dep.get("config_json")),
                "extra":         body.get("extra") or {},
            }
            cur.execute(
                "INSERT INTO agent_job (agent_id, job_type, params, status) "
                "VALUES (%s,%s,%s,'queued')",
                (dep["agent_id"], job_type, json.dumps(params))
            )
            job_id = cur.lastrowid
            # deployment 상태 관측
            transition = {"install": "deploying", "upgrade": "deploying",
                          "uninstall": "deploying", "start": "deploying",
                          "stop": "deploying", "restart": "deploying"}
            if job_type in transition:
                cur.execute("UPDATE agent_deployment SET status=%s, last_job_id=%s WHERE id=%s",
                            (transition[job_type], job_id, did))
    finally:
        conn.close()
    return HandlerResult(status=202, body={"job_id": job_id, "status": "queued"},
                         media_type="application/json")


# ════════════════════════════════════════════════════════════
#  Public static routes — 설치 스크립트 / agent 바이너리 배포
#  인증 없음 (enrollment_token 이 페이로드의 인증 역할)
# ════════════════════════════════════════════════════════════

def _agent_asset_candidates():
    """Agent asset 탐색 후보. 실행 컨텍스트에 따라 다양.

    CSC 는 다음 중 한 곳에서 실행:
      1. 개발 소스: <repo>/csc/src/csc_app.py → asset at <repo>/agent/
      2. 빌드 스테이징: build/dist/csc/src/csc_app.py → build/dist/agent/
      3. Agent 배포: install_path/csc/src/csc_app.py → install_path/../../../agent/ (dist 원본)
      4. 환경 변수 CIMS_AGENT_ASSET_DIR 직접 지정
    """
    cands = []
    env = os.environ.get("CIMS_AGENT_ASSET_DIR")
    if env: cands.append(env)
    here = os.path.dirname(__file__)
    for up in (3, 4, 5, 6):
        cands.append(os.path.abspath(os.path.join(here, *([".."] * up), "agent")))
    # dedup 순서 유지
    seen = set(); out = []
    for c in cands:
        if c not in seen:
            seen.add(c); out.append(c)
    return tuple(out)

_AGENT_ASSET_CANDIDATES = _agent_asset_candidates()


def _find_agent_asset(filename: str) -> str | None:
    for root in _AGENT_ASSET_CANDIDATES:
        p = os.path.join(root, filename)
        if os.path.isfile(p):
            return p
    return None


async def _serve_install_script(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    p = _find_agent_asset("install-agent.sh")
    if not p:
        return HandlerResult(status=404, body="install-agent.sh not bundled",
                             media_type="text/plain")
    with open(p, "r", encoding="utf-8") as f:
        return HandlerResult(status=200, body=f.read(),
                             media_type="text/x-shellscript")


async def _serve_agent_binary(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    p = _find_agent_asset("cims_agent.py")
    if not p:
        return HandlerResult(status=404, body="cims_agent.py not bundled",
                             media_type="text/plain")
    with open(p, "rb") as f:
        return HandlerResult(status=200, body=f.read(),
                             media_type="text/x-python")


# ════════════════════════════════════════════════════════════
#  Collection proxy (CSC → Agent sync REST)
# ════════════════════════════════════════════════════════════

def _fetch_deployment_for_proxy(did: int, config):
    """proxy 에 필요한 deployment + agent 정보 동시 조회. (sync — asyncio.to_thread 로 호출)"""
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT d.id, d.install_path, d.package_id, "
                "       a.id AS agent_id, a.name AS agent_name, a.status AS agent_status, "
                "       a.ip_address, a.sync_port, a.agent_token, "
                "       p.config_template_json "
                "FROM agent_deployment d "
                "LEFT JOIN cims_agent    a ON d.agent_id   = a.id "
                "LEFT JOIN cims_package  p ON d.package_id = p.id "
                "WHERE d.id=%s", (did,))
            return cur.fetchone()
    finally:
        conn.close()


def _collection_schema(template_json, name: str):
    """template.collections 에서 key=name 인 항목의 schema 를 찾아 반환. 없으면 None."""
    tmpl = _safe_json(template_json)
    if not isinstance(tmpl, dict): return None, None
    for c in tmpl.get("collections") or []:
        if c.get("key") == name:
            return c.get("schema") or {}, c
    return None, None


def _validate_record(schema: dict, record: dict) -> list:
    """schema.fields 로 record 기본 검증. 오류 목록 반환 (빈 목록이면 OK)."""
    if not isinstance(record, dict): return ["record_must_be_object"]
    errors = []
    known = {f["key"]: f for f in schema.get("fields", [])}
    for key, fdef in known.items():
        if fdef.get("required") and record.get(key) in (None, ""):
            # auto 필드 (uuid 등) 는 서버가 채움
            if fdef.get("auto"):
                continue
            errors.append(f"{key}: required")
        val = record.get(key)
        if val is None: continue
        t = fdef.get("type")
        if t == "int" and not isinstance(val, bool) and not isinstance(val, int):
            try: int(val)
            except Exception: errors.append(f"{key}: int expected")
        elif t == "bool" and not isinstance(val, bool):
            errors.append(f"{key}: bool expected")
        elif t == "enum":
            opts = fdef.get("options") or []
            if opts and val not in opts:
                errors.append(f"{key}: must be one of {opts}")
    return errors


def _agent_proxy_call(method: str, agent: dict, path: str,
                      query: dict = None, body: dict = None,
                      timeout: int = 15, config: dict = None) -> tuple:
    """Agent 의 sync REST 로 TLS 요청. (status, json_body) 반환.

    per-agent mTLS: agent.mtls_enabled=1 인 레코드만 client cert 로 연결.
    그렇지 않은 agent (MtlsEnabled 활성화 전 enroll 된 레거시 포함) 는 기존처럼
    X-Agent-Token 단독 TLS (검증 없음) 로 통신.
    """
    import urllib.parse, urllib.request, ssl as _ssl
    ip = agent.get("ip_address") or "127.0.0.1"
    port = agent.get("sync_port")
    if not port:
        return 0, {"error": "agent sync_port unknown (아직 heartbeat 보고 전일 수 있음)"}
    qs = ("?" + urllib.parse.urlencode(query)) if query else ""
    url = f"https://{ip}:{port}{path}{qs}"
    data = None
    headers = {"X-Agent-Token": agent.get("agent_token") or ""}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)

    ctx = _ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = _ssl.CERT_NONE

    # per-agent mTLS: 레코드에 mtls_enabled=1 이면 CSC client cert 로 mTLS 연결
    if agent.get("mtls_enabled"):
        mtls_cfg = (config or {}).get("Agent") or {}
        mtls_dir = mtls_cfg.get("MtlsDir") or "cert/agent_mtls"
        if not os.path.isabs(mtls_dir):
            mtls_dir = os.path.abspath(mtls_dir)
        ca_crt     = os.path.join(mtls_dir, "ca.crt")
        client_crt = os.path.join(mtls_dir, "csc_client.crt")
        client_key = os.path.join(mtls_dir, "csc_client.key")
        if os.path.isfile(ca_crt) and os.path.isfile(client_crt) and os.path.isfile(client_key):
            ctx.verify_mode = _ssl.CERT_REQUIRED
            ctx.load_verify_locations(ca_crt)
            ctx.load_cert_chain(certfile=client_crt, keyfile=client_key)
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try: b = json.loads(e.read().decode("utf-8"))
        except Exception: b = {"error": f"HTTP {e.code}"}
        return e.code, b
    except Exception as e:
        return 0, {"error": str(e)}


async def _get_deployment_collection(did: int, name: str, config):
    dep = await asyncio.to_thread(_fetch_deployment_for_proxy, did, config)
    if not dep:
        return HandlerResult(status=404, body={"error": "deployment_not_found"},
                             media_type="application/json")
    if not dep.get("install_path"):
        return HandlerResult(status=409, body={"error": "not_installed",
                                                "hint": "install 먼저 실행"},
                             media_type="application/json")
    schema, _ = _collection_schema(dep.get("config_template_json"), name)
    if schema is None:
        return HandlerResult(status=404,
            body={"error": "collection_not_in_template", "name": name},
            media_type="application/json")

    status, resp = await asyncio.to_thread(
        _agent_proxy_call, "GET", dep,
        "/collection", {"install_path": dep["install_path"], "name": name},
        None, 15, config,
    )
    if status == 200:
        return HandlerResult(status=200,
            body={"records": resp.get("records") or [], "schema": schema},
            media_type="application/json")
    return HandlerResult(status=status or 502,
        body={"error": "agent_proxy_failed", "detail": resp},
        media_type="application/json")


async def _put_deployment_collection(handler_args, did: int, name: str, config):
    dep = await asyncio.to_thread(_fetch_deployment_for_proxy, did, config)
    if not dep:
        return HandlerResult(status=404, body={"error": "deployment_not_found"},
                             media_type="application/json")
    if not dep.get("install_path"):
        return HandlerResult(status=409, body={"error": "not_installed"},
                             media_type="application/json")
    schema, _ = _collection_schema(dep.get("config_template_json"), name)
    if schema is None:
        return HandlerResult(status=404,
            body={"error": "collection_not_in_template", "name": name},
            media_type="application/json")

    body = _parse_body(handler_args)
    records = body.get("records")
    if not isinstance(records, list):
        return HandlerResult(status=400, body={"error": "records array required"},
                             media_type="application/json")

    # validation + auto id 부여
    import uuid as _uuid
    id_field = schema.get("id_field") or "id"
    id_type  = schema.get("id_type") or "uuid"
    all_errors = []
    for i, r in enumerate(records):
        if not isinstance(r, dict):
            all_errors.append({"index": i, "errors": ["not_object"]})
            continue
        # auto id
        if id_type == "uuid" and not r.get(id_field):
            r[id_field] = _uuid.uuid4().hex[:16]
        errs = _validate_record(schema, r)
        if errs:
            all_errors.append({"index": i, "errors": errs})
    if all_errors:
        return HandlerResult(status=400,
            body={"error": "validation_failed", "details": all_errors},
            media_type="application/json")

    do_signal = body.get("signal", True)
    status, resp = await asyncio.to_thread(
        _agent_proxy_call, "PUT", dep,
        "/collection", {"install_path": dep["install_path"], "name": name},
        {"records": records, "signal": do_signal}, 15, config,
    )
    if status == 200:
        return HandlerResult(status=200,
            body={"ok": True, "count": resp.get("count"),
                  "signaled": resp.get("signaled") or []},
            media_type="application/json")
    return HandlerResult(status=status or 502,
        body={"error": "agent_proxy_failed", "detail": resp},
        media_type="application/json")


# ════════════════════════════════════════════════════════════
#  Handler list
# ════════════════════════════════════════════════════════════

CIMS_AGENT_ADMIN_HANDLER_LIST = (
    (_AGENT_BASE,      handle_agents,      {}),
    (_PACKAGE_BASE,    handle_packages,    {}),
    (_DEPLOYMENT_BASE, handle_deployments, {}),
)

# 인증 없이 누구나 받을 수 있는 배포용 정적 에셋
CIMS_AGENT_PUBLIC_HANDLER_LIST = (
    ("/install-agent.sh", _serve_install_script, {}),
    ("/cims_agent.py",    _serve_agent_binary,   {}),
)
