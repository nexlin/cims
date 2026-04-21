"""
CSC Agent/Package/Deployment Admin API (P10).

  /api/v1/agents                GET list / POST create (enrollment token 발급)
  /api/v1/agents/{id}           GET / PUT / DELETE
  /api/v1/agents/{id}/approve   POST — pending → approved
  /api/v1/agents/{id}/revoke    POST — revoked
  /api/v1/agents/{id}/metrics   GET — 최근 리소스 메트릭

  /api/v1/packages              GET list / POST upload (multipart or base64)
  /api/v1/packages/{id}         GET / DELETE

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
# CSC 루트 = 이 파일이 있는 src/ 의 부모 디렉토리 (= csc/)
_COMPONENT_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


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

def _agent_to_json(r: dict) -> dict:
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
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


async def _list_agents(config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM cims_agent ORDER BY id")
            rows = cur.fetchall()
    finally:
        conn.close()
    return HandlerResult(status=200, body={"items": [_agent_to_json(r) for r in rows]},
                         media_type="application/json")


async def _get_agent(aid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM cims_agent WHERE id=%s", (aid,))
            r = cur.fetchone()
    finally:
        conn.close()
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body=_agent_to_json(r), media_type="application/json")


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

def _package_to_json(r: dict) -> dict:
    return {
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
        if method == "DELETE": return await _delete_package(pid, config)
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


async def _list_packages(config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM cims_package ORDER BY name, id DESC")
            rows = cur.fetchall()
    finally:
        conn.close()
    return HandlerResult(status=200, body={"items": [_package_to_json(r) for r in rows]},
                         media_type="application/json")


async def _get_package(pid: int, config):
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
                fsha: str, full_desc: str, actor: str):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO cims_package (name, version, file_path, file_size, sha256, "
                "                          description, uploaded_by) "
                "VALUES (%s,%s,%s,%s,%s,%s,%s) "
                "ON DUPLICATE KEY UPDATE file_path=VALUES(file_path), file_size=VALUES(file_size), "
                "  sha256=VALUES(sha256), description=VALUES(description), "
                "  uploaded_by=VALUES(uploaded_by), uploaded_at=NOW()",
                (name, version, fpath, fsize, fsha, full_desc, actor)
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


    # 2) meta.json 파싱 (tarball decompress → thread 로 offload)
    meta = await asyncio.to_thread(_extract_meta_from_tarball, raw) or {}
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
    row = await asyncio.to_thread(
        _pkg_upsert, config, name, version, fpath, fsize, fsha, full_desc, actor
    )

    result = _package_to_json(row)
    # 원본 meta 필드도 응답에 포함 (UI 가 preview 표시)
    if meta:
        result["meta"] = {
            "build_date": build_date, "git_sha": git_sha, "git_branch": git_branch,
            "changelog": changelog, "packaged_at": meta.get("packaged_at"),
            "packaged_by": meta.get("packaged_by"),
        }
    logger.log_info(f"[pkg-upload] done {name} {version} size={fsize} "
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


async def _delete_package(pid: int, config):
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
        "service_kind": r["service_kind"],
        "status":       r["status"],
        "install_path": r["install_path"],
        "deployed_at":  _dt(r["deployed_at"]),
        "last_job_id":  r["last_job_id"],
        "note":         r["note"],
        "create_time":  _dt(r["create_time"]),
    }


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
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


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
    service_kind = (body.get("service_kind") or "").strip()
    install_path = (body.get("install_path") or "").strip() or None
    if not agent_id or not package_id:
        return HandlerResult(status=400, body={"error": "agent_id and package_id required"},
                             media_type="application/json")
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO agent_deployment (agent_id, package_id, instance_id, "
                "                              service_kind, install_path, note) "
                "VALUES (%s,%s,%s,%s,%s,%s)",
                (agent_id, package_id, instance_id, service_kind, install_path, body.get("note"))
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
    for col in ("instance_id", "service_kind", "install_path", "note"):
        if col in body:
            fields.append(f"{col}=%s"); values.append(body[col])
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
                "service_kind":  dep["service_kind"],
                "install_path":  dep["install_path"],
                "instance_id":   dep["instance_id"],
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

_AGENT_ASSET_CANDIDATES = (
    # 배포: build/dist/agent/  (현재 파일=csc/src/handlers/agents.py 기준 ../../../agent)
    os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "agent")),
    # 개발: repo_root/agent/   (csc/src/handlers/ 기준 ../../../../agent)
    os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "agent")),
)


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
