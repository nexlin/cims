"""
CSP 런타임 설정 Admin API.

Routes (prefix-matched, admin JWT required):
  /api/v1/csp/listeners                         GET list  / POST create
  /api/v1/csp/listeners/{id}                    GET / PUT / DELETE

변경 시 흐름:
  1. DB CUD
  2. csc_config_cache.refresh_entity("listener") — 메모리+파일 재조회
  3. audit_config_change(...) — csp_config_audit 에 변경 기록
  4. notify_config_change("listener", id, action) — CSP 에 UDP notify → HTTP pull

P2 는 listener 엔티티만. trunk/route/access 는 P3/P4/P5 에서 동일 패턴으로 추가.
"""

from __future__ import annotations

import json
import secrets
from urllib.parse import urlparse, unquote
from pathlib import PurePath

import pymysql
import pymysql.cursors

from util.pi_http.http_handler import HandlerArgs, HandlerResult
from util.log_util import Logger

from csc_service import notify_config_change, audit_config_change

logger = Logger()

# ──────────────────────────────────────────────────────────────
#  DB helper
# ──────────────────────────────────────────────────────────────

def _get_db(config: dict):
    db = config.get("CimsDatabase", {})
    return pymysql.connect(
        host=db.get("Host", "127.0.0.1"),
        port=int(db.get("Port", 3306)),
        user=db.get("User", "root"),
        password=db.get("Password", ""),
        database=db.get("Db", "cims"),
        charset="utf8mb4",
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=True,
    )


def _path_tail(full_path: str, base: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _actor_from_headers(headers: dict) -> str:
    """JWT 미들웨어가 이미 검증했다고 가정 — Authorization 에서 sub 추출."""
    auth = headers.get("Authorization") or headers.get("authorization") or ""
    if auth.lower().startswith("bearer "):
        token = auth[7:].strip()
        try:
            import jwt as _jwt
            # 서명 검증 없이 payload 만 파싱 (상위에서 이미 검증됨)
            data = _jwt.decode(token, options={"verify_signature": False})
            return data.get("sub") or data.get("login_id") or "admin"
        except Exception:
            pass
    return "admin"


def _row_to_json(r: dict) -> dict:
    return {
        "id": r["id"],
        "name": r["name"],
        "enabled": bool(r["enabled"]),
        "bind_ip": r["bind_ip"],
        "bind_port": r["bind_port"],
        "protocol": r["protocol"],
        "domain": r["domain"] or "",
        "service": r["service"],
        "tls_cert_path": r.get("tls_cert_path"),
        "tls_key_path":  r.get("tls_key_path"),
        "tls_ca_path":   r.get("tls_ca_path"),
        "tls_verify_peer": bool(r.get("tls_verify_peer")),
        "max_connections": r.get("max_connections", 0),
        "thread_count":    r.get("thread_count", 2),
        "note": r.get("note"),
        "etag": r.get("etag") or "",
        "create_time": r["create_time"].isoformat() if r.get("create_time") else None,
        "update_time": r["update_time"].isoformat() if r.get("update_time") else None,
    }


def _compute_etag() -> str:
    return secrets.token_hex(8)


# ──────────────────────────────────────────────────────────────
#  Handler
# ──────────────────────────────────────────────────────────────

_LISTENER_BASE = "/api/v1/csp/listeners"


async def handle_listeners(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    tail = _path_tail(handler_args.full_path, _LISTENER_BASE)
    method = handler_args.method.upper()

    if len(tail) == 0:
        if method == "GET":
            return await _list_listeners(config)
        if method == "POST":
            return await _create_listener(handler_args, config)
        return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")

    # /api/v1/csp/listeners/{id}
    try:
        lid = int(tail[0])
    except (TypeError, ValueError):
        return HandlerResult(status=400, body={"error": "invalid_id"}, media_type="application/json")

    if method == "GET":
        return await _get_listener(lid, config)
    if method == "PUT":
        return await _update_listener(handler_args, lid, config)
    if method == "DELETE":
        return await _delete_listener(handler_args, lid, config)
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


async def _list_listeners(config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM csp_listener ORDER BY id")
            rows = cur.fetchall()
    finally:
        conn.close()
    return HandlerResult(status=200, body={"items": [_row_to_json(r) for r in rows]},
                         media_type="application/json")


async def _get_listener(lid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM csp_listener WHERE id=%s", (lid,))
            r = cur.fetchone()
    finally:
        conn.close()
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body=_row_to_json(r), media_type="application/json")


def _parse_body(handler_args: HandlerArgs) -> dict:
    body = handler_args.body
    if body is None:
        return {}
    if isinstance(body, dict):
        return body
    if isinstance(body, (bytes, bytearray)):
        try: return json.loads(body.decode("utf-8"))
        except Exception: return {}
    if isinstance(body, str):
        try: return json.loads(body)
        except Exception: return {}
    return {}


async def _create_listener(handler_args: HandlerArgs, config):
    body = _parse_body(handler_args)
    name   = (body.get("name") or "").strip()
    ip     = body.get("bind_ip") or "0.0.0.0"
    port   = int(body.get("bind_port") or 0)
    proto  = (body.get("protocol") or "UDP").upper()
    domain = body.get("domain") or ""
    svc    = body.get("service") or "system"
    note   = body.get("note")
    threadCount = int(body.get("thread_count") or 2)
    enabled = 0 if body.get("enabled") in (False, "false", 0, "0") else 1

    if not name or port <= 0:
        return HandlerResult(status=400, body={"error": "name and bind_port required"},
                             media_type="application/json")
    if proto not in ("UDP", "TCP", "TLS", "WS", "WSS"):
        return HandlerResult(status=400, body={"error": "invalid_protocol"}, media_type="application/json")

    etag = _compute_etag()
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO csp_listener "
                "(name, enabled, bind_ip, bind_port, protocol, domain, service, "
                " tls_cert_path, tls_key_path, tls_ca_path, tls_verify_peer, "
                " max_connections, thread_count, note, etag) "
                "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)",
                (name, enabled, ip, port, proto, domain, svc,
                 body.get("tls_cert_path"), body.get("tls_key_path"), body.get("tls_ca_path"),
                 1 if body.get("tls_verify_peer") else 0,
                 int(body.get("max_connections") or 0), threadCount,
                 note, etag),
            )
            new_id = cur.lastrowid
            cur.execute("SELECT * FROM csp_listener WHERE id=%s", (new_id,))
            r = cur.fetchone()
    except pymysql.err.IntegrityError as e:
        return HandlerResult(status=409, body={"error": "conflict", "detail": str(e)},
                             media_type="application/json")
    finally:
        conn.close()

    rowJson = _row_to_json(r)
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "listener", new_id, "CREATE", after=rowJson,
                        etag_after=etag)
    notify_config_change("listener", new_id, "CREATE", actor=actor)
    return HandlerResult(status=201, body=rowJson, media_type="application/json")


async def _update_listener(handler_args: HandlerArgs, lid: int, config):
    body = _parse_body(handler_args)
    if not body:
        return HandlerResult(status=400, body={"error": "empty_body"}, media_type="application/json")

    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM csp_listener WHERE id=%s", (lid,))
            before = cur.fetchone()
            if not before:
                return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
            etag_before = before.get("etag", "")

            # dynamic partial update
            fields = []
            values = []
            for col in ("name", "bind_ip", "bind_port", "protocol", "domain", "service",
                        "tls_cert_path", "tls_key_path", "tls_ca_path",
                        "max_connections", "thread_count", "note"):
                if col in body:
                    fields.append(f"{col}=%s")
                    values.append(body[col])
            if "enabled" in body:
                fields.append("enabled=%s")
                values.append(0 if body["enabled"] in (False, "false", 0, "0") else 1)
            if "tls_verify_peer" in body:
                fields.append("tls_verify_peer=%s")
                values.append(1 if body["tls_verify_peer"] else 0)

            if not fields:
                return HandlerResult(status=400, body={"error": "no_updatable_fields"}, media_type="application/json")

            etag_after = _compute_etag()
            fields.append("etag=%s")
            values.append(etag_after)
            values.append(lid)

            try:
                cur.execute(f"UPDATE csp_listener SET {', '.join(fields)} WHERE id=%s", values)
            except pymysql.err.IntegrityError as e:
                return HandlerResult(status=409, body={"error": "conflict", "detail": str(e)},
                                     media_type="application/json")

            cur.execute("SELECT * FROM csp_listener WHERE id=%s", (lid,))
            after = cur.fetchone()
    finally:
        conn.close()

    afterJson = _row_to_json(after)
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "listener", lid, "UPDATE",
                        before=_row_to_json(before), after=afterJson,
                        etag_before=etag_before, etag_after=etag_after)
    notify_config_change("listener", lid, "UPDATE", actor=actor)
    return HandlerResult(status=200, body=afterJson, media_type="application/json")


async def _delete_listener(handler_args: HandlerArgs, lid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM csp_listener WHERE id=%s", (lid,))
            before = cur.fetchone()
            if not before:
                return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
            cur.execute("DELETE FROM csp_listener WHERE id=%s", (lid,))
    finally:
        conn.close()

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "listener", lid, "DELETE",
                        before=_row_to_json(before),
                        etag_before=before.get("etag", ""))
    notify_config_change("listener", lid, "DELETE", actor=actor)
    return HandlerResult(status=204, body=None, media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  Handler list
# ──────────────────────────────────────────────────────────────

CIMS_CSP_RUNTIME_HANDLER_LIST = (
    (_LISTENER_BASE, handle_listeners, {}),
)
