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
from typing import Optional
from urllib.parse import urlparse, unquote
from pathlib import PurePath

import pymysql
import pymysql.cursors

from httpsrv.handler import HandlerArgs, HandlerResult
from util.log_util import Logger

from services.mcptt import notify_config_change, audit_config_change

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
#  Trunk handler
# ──────────────────────────────────────────────────────────────

_TRUNK_BASE = "/api/v1/csp/trunks"


def _trunk_row_to_json(r: dict) -> dict:
    return {
        "id": r["id"],
        "name": r["name"],
        "enabled": bool(r["enabled"]),
        "service_id": r.get("service_id"),
        "failover_priority": r.get("failover_priority", 100),
        "remote_ip": r["remote_ip"],
        "remote_port": r["remote_port"],
        "remote_domain": r["remote_domain"] or "",
        "protocol": r["protocol"],
        "outbound_proxy_ip": r.get("outbound_proxy_ip"),
        "outbound_proxy_port": r.get("outbound_proxy_port"),
        "register_to_remote": bool(r.get("register_to_remote")),
        "auth_user": r.get("auth_user"),
        "auth_realm": r.get("auth_realm"),
        "register_expires": r.get("register_expires", 3600),
        "options_ping_sec": r.get("options_ping_sec", 60),
        "options_dead_threshold": r.get("options_dead_threshold", 3),
        "srv_lookup": bool(r.get("srv_lookup")),
        "dns_fallback": bool(r.get("dns_fallback")),
        "max_concurrent_calls": r.get("max_concurrent_calls", 0),
        "cps_limit": r.get("cps_limit", 0),
        "note": r.get("note"),
        "etag": r.get("etag") or "",
        "create_time": r["create_time"].isoformat() if r.get("create_time") else None,
        "update_time": r["update_time"].isoformat() if r.get("update_time") else None,
    }


async def handle_trunks(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    tail = _path_tail(handler_args.full_path, _TRUNK_BASE)
    method = handler_args.method.upper()

    if len(tail) == 0:
        if method == "GET":   return await _list_trunks(config)
        if method == "POST":  return await _create_trunk(handler_args, config)
        return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")

    try: tid = int(tail[0])
    except (TypeError, ValueError):
        return HandlerResult(status=400, body={"error": "invalid_id"}, media_type="application/json")

    if method == "GET":    return await _get_trunk(tid, config)
    if method == "PUT":    return await _update_trunk(handler_args, tid, config)
    if method == "DELETE": return await _delete_trunk(handler_args, tid, config)
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


async def _list_trunks(config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM sip_trunk ORDER BY id")
            rows = cur.fetchall()
    finally:
        conn.close()
    return HandlerResult(status=200, body={"items": [_trunk_row_to_json(r) for r in rows]},
                         media_type="application/json")


async def _get_trunk(tid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM sip_trunk WHERE id=%s", (tid,))
            r = cur.fetchone()
    finally:
        conn.close()
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body=_trunk_row_to_json(r), media_type="application/json")


async def _create_trunk(handler_args: HandlerArgs, config):
    body = _parse_body(handler_args)
    name  = (body.get("name") or "").strip()
    ip    = (body.get("remote_ip") or "").strip()
    port  = int(body.get("remote_port") or 5060)
    proto = (body.get("protocol") or "UDP").upper()
    if not name or not ip:
        return HandlerResult(status=400, body={"error": "name and remote_ip required"},
                             media_type="application/json")
    if proto not in ("UDP", "TCP", "TLS"):
        return HandlerResult(status=400, body={"error": "invalid_protocol"}, media_type="application/json")

    etag = _compute_etag()
    enabled = 0 if body.get("enabled") in (False, "false", 0, "0") else 1
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            svc_id = body.get("service_id")
            if svc_id in (None, 0, "", "0"): svc_id = None
            else:
                try: svc_id = int(svc_id)
                except Exception: svc_id = None
            cur.execute(
                "INSERT INTO sip_trunk "
                "(name, enabled, service_id, failover_priority, "
                " remote_ip, remote_port, remote_domain, protocol, "
                " outbound_proxy_ip, outbound_proxy_port, "
                " register_to_remote, auth_user, auth_password, auth_realm, register_expires, "
                " options_ping_sec, options_dead_threshold, "
                " srv_lookup, dns_fallback, max_concurrent_calls, cps_limit, note, etag) "
                "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)",
                (name, enabled, svc_id, int(body.get("failover_priority") or 100),
                 ip, port, body.get("remote_domain", ""), proto,
                 body.get("outbound_proxy_ip"), body.get("outbound_proxy_port"),
                 1 if body.get("register_to_remote") else 0,
                 body.get("auth_user"), body.get("auth_password"), body.get("auth_realm"),
                 int(body.get("register_expires") or 3600),
                 int(body.get("options_ping_sec") or 60),
                 int(body.get("options_dead_threshold") or 3),
                 1 if body.get("srv_lookup") else 0,
                 0 if body.get("dns_fallback") in (False, "false", 0, "0") else 1,
                 int(body.get("max_concurrent_calls") or 0),
                 int(body.get("cps_limit") or 0),
                 body.get("note"), etag),
            )
            new_id = cur.lastrowid
            cur.execute("SELECT * FROM sip_trunk WHERE id=%s", (new_id,))
            r = cur.fetchone()
    except pymysql.err.IntegrityError as e:
        return HandlerResult(status=409, body={"error": "conflict", "detail": str(e)},
                             media_type="application/json")
    finally:
        conn.close()

    rowJson = _trunk_row_to_json(r)
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "trunk", new_id, "CREATE", after=rowJson, etag_after=etag)
    notify_config_change("trunk", new_id, "CREATE", actor=actor)
    return HandlerResult(status=201, body=rowJson, media_type="application/json")


async def _update_trunk(handler_args: HandlerArgs, tid: int, config):
    body = _parse_body(handler_args)
    if not body:
        return HandlerResult(status=400, body={"error": "empty_body"}, media_type="application/json")

    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM sip_trunk WHERE id=%s", (tid,))
            before = cur.fetchone()
            if not before:
                return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
            etag_before = before.get("etag", "")

            updatable_cols = ("name", "service_id", "failover_priority",
                              "remote_ip", "remote_port", "remote_domain", "protocol",
                              "outbound_proxy_ip", "outbound_proxy_port",
                              "auth_user", "auth_password", "auth_realm", "register_expires",
                              "options_ping_sec", "options_dead_threshold",
                              "max_concurrent_calls", "cps_limit", "note")
            fields = []; values = []
            for col in updatable_cols:
                if col in body:
                    fields.append(f"{col}=%s")
                    values.append(body[col])
            for bool_col in ("enabled", "register_to_remote", "srv_lookup", "dns_fallback"):
                if bool_col in body:
                    fields.append(f"{bool_col}=%s")
                    values.append(0 if body[bool_col] in (False, "false", 0, "0") else 1)
            if not fields:
                return HandlerResult(status=400, body={"error": "no_updatable_fields"},
                                     media_type="application/json")
            etag_after = _compute_etag()
            fields.append("etag=%s"); values.append(etag_after)
            values.append(tid)
            try:
                cur.execute(f"UPDATE sip_trunk SET {', '.join(fields)} WHERE id=%s", values)
            except pymysql.err.IntegrityError as e:
                return HandlerResult(status=409, body={"error": "conflict", "detail": str(e)},
                                     media_type="application/json")
            cur.execute("SELECT * FROM sip_trunk WHERE id=%s", (tid,))
            after = cur.fetchone()
    finally:
        conn.close()

    afterJson = _trunk_row_to_json(after)
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "trunk", tid, "UPDATE",
                        before=_trunk_row_to_json(before), after=afterJson,
                        etag_before=etag_before, etag_after=etag_after)
    notify_config_change("trunk", tid, "UPDATE", actor=actor)
    return HandlerResult(status=200, body=afterJson, media_type="application/json")


async def _delete_trunk(handler_args: HandlerArgs, tid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM sip_trunk WHERE id=%s", (tid,))
            before = cur.fetchone()
            if not before:
                return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
            cur.execute("DELETE FROM sip_trunk WHERE id=%s", (tid,))
    finally:
        conn.close()

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "trunk", tid, "DELETE",
                        before=_trunk_row_to_json(before),
                        etag_before=before.get("etag", ""))
    notify_config_change("trunk", tid, "DELETE", actor=actor)
    return HandlerResult(status=204, body=None, media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  Route rule handler
# ──────────────────────────────────────────────────────────────

_ROUTE_BASE = "/api/v1/csp/routes"


def _load_rule_full(cur, rule_id: int) -> Optional[dict]:
    cur.execute("SELECT * FROM routing_rule WHERE id=%s", (rule_id,))
    r = cur.fetchone()
    if not r:
        return None
    cur.execute("SELECT * FROM routing_rule_match WHERE rule_id=%s ORDER BY seq", (rule_id,))
    matches = cur.fetchall()
    cur.execute("SELECT * FROM routing_rule_transform WHERE rule_id=%s ORDER BY seq", (rule_id,))
    transforms = cur.fetchall()
    return _rule_to_json(r, matches, transforms)


def _rule_to_json(r: dict, matches: list, transforms: list) -> dict:
    return {
        "id": r["id"],
        "name": r["name"],
        "enabled": bool(r["enabled"]),
        "priority": r["priority"],
        "description": r.get("description"),
        "match": [
            {
                "field": m["field"], "op": m["op"], "value": m["value"],
                "invert": bool(m.get("invert")), "seq": m.get("seq", 0),
            } for m in matches
        ],
        "transform": [
            {
                "action": t["action"], "target": t.get("target"),
                "value": t.get("value"), "seq": t.get("seq", 0),
            } for t in transforms
        ],
        "target": {
            "mode": r["target_mode"],
            "trunk_id": r.get("target_trunk_id"),
            "service_id": r.get("target_service_id"),
            "json": json.loads(r["target_json"]) if r.get("target_json") else None,
        },
        "fail": {
            "action": r["fail_action"],
            "code": r["fail_code"],
            "reason": r["fail_reason"],
            "fallback": r.get("fallback_trunk_id"),
            "timeout_ms": r["timeout_ms"],
            "retry_count": r["retry_count"],
        },
        "hit_count": r.get("hit_count", 0),
        "last_hit_time": r["last_hit_time"].isoformat() if r.get("last_hit_time") else None,
        "etag": r.get("etag") or "",
        "create_time": r["create_time"].isoformat() if r.get("create_time") else None,
        "update_time": r["update_time"].isoformat() if r.get("update_time") else None,
    }


def _write_rule_subtables(cur, rule_id: int, body: dict):
    """match/transform 배열을 새로 쓴다 (기존 row 삭제 후 insert)."""
    cur.execute("DELETE FROM routing_rule_match     WHERE rule_id=%s", (rule_id,))
    cur.execute("DELETE FROM routing_rule_transform WHERE rule_id=%s", (rule_id,))
    for i, m in enumerate(body.get("match") or []):
        if not m.get("field"): continue
        cur.execute(
            "INSERT INTO routing_rule_match (rule_id, field, op, value, invert, seq) "
            "VALUES (%s,%s,%s,%s,%s,%s)",
            (rule_id, m["field"], m.get("op", "equals"), m.get("value", ""),
             1 if m.get("invert") else 0, m.get("seq", i)),
        )
    for i, t in enumerate(body.get("transform") or []):
        if not t.get("action"): continue
        cur.execute(
            "INSERT INTO routing_rule_transform (rule_id, action, target, value, seq) "
            "VALUES (%s,%s,%s,%s,%s)",
            (rule_id, t["action"], t.get("target"), t.get("value"), t.get("seq", i)),
        )


async def handle_routes(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    # sub-path: /dryrun, /{id}, /{id}/hits
    path = urlparse(handler_args.full_path).path
    rel = path[len(_ROUTE_BASE):].strip("/")
    parts = [unquote(p) for p in rel.split("/")] if rel else []
    method = handler_args.method.upper()

    # Special paths
    if parts and parts[0] == "dryrun":
        if method != "POST":
            return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")
        return await _dryrun(handler_args, config)

    if len(parts) >= 2 and parts[1] == "hits":
        if method != "GET":
            return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")
        try: rid = int(parts[0])
        except ValueError:
            return HandlerResult(status=400, body={"error": "invalid_id"}, media_type="application/json")
        return await _get_hits(rid, config)

    if len(parts) == 0:
        if method == "GET":  return await _list_routes(config)
        if method == "POST": return await _create_route(handler_args, config)
        return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")

    try: rid = int(parts[0])
    except (TypeError, ValueError):
        return HandlerResult(status=400, body={"error": "invalid_id"}, media_type="application/json")

    if method == "GET":    return await _get_route(rid, config)
    if method == "PUT":    return await _update_route(handler_args, rid, config)
    if method == "DELETE": return await _delete_route(handler_args, rid, config)
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


async def _list_routes(config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM routing_rule ORDER BY priority, id")
            rows = cur.fetchall()
            items = []
            for r in rows:
                cur.execute("SELECT * FROM routing_rule_match WHERE rule_id=%s ORDER BY seq", (r["id"],))
                matches = cur.fetchall()
                cur.execute("SELECT * FROM routing_rule_transform WHERE rule_id=%s ORDER BY seq", (r["id"],))
                transforms = cur.fetchall()
                items.append(_rule_to_json(r, matches, transforms))
    finally:
        conn.close()
    return HandlerResult(status=200, body={"items": items}, media_type="application/json")


async def _get_route(rid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            row = _load_rule_full(cur, rid)
    finally:
        conn.close()
    if not row:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body=row, media_type="application/json")


def _rule_fields_from_body(body: dict) -> tuple:
    name = (body.get("name") or "").strip()
    priority = int(body.get("priority") or 100)
    description = body.get("description") or ""
    enabled = 0 if body.get("enabled") in (False, "false", 0, "0") else 1
    target = body.get("target") or {}
    target_mode = (target.get("mode") or "trunk").lower()
    target_trunk_id = target.get("trunk_id") or None
    target_service_id = target.get("service_id") or None
    target_json_raw = target.get("json")
    target_json = json.dumps(target_json_raw) if target_json_raw is not None else None
    fail = body.get("fail") or {}
    fail_action = (fail.get("action") or "reject").lower()
    fail_code = int(fail.get("code") or 404)
    fail_reason = fail.get("reason") or "Not Found"
    fallback_trunk_id = fail.get("fallback") or None
    timeout_ms = int(fail.get("timeout_ms") or 4000)
    retry_count = int(fail.get("retry_count") or 0)
    return (name, enabled, priority, description, target_mode, target_trunk_id,
            target_service_id, target_json, fail_action, fail_code, fail_reason,
            fallback_trunk_id, timeout_ms, retry_count)


async def _create_route(handler_args: HandlerArgs, config):
    body = _parse_body(handler_args)
    (name, enabled, priority, description, target_mode, target_trunk_id,
     target_service_id, target_json, fail_action, fail_code, fail_reason,
     fallback_trunk_id, timeout_ms, retry_count) = _rule_fields_from_body(body)
    if not name:
        return HandlerResult(status=400, body={"error": "name required"}, media_type="application/json")
    etag = _compute_etag()
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO routing_rule "
                "(name, enabled, priority, description, "
                " target_mode, target_trunk_id, target_service_id, target_json, "
                " fail_action, fail_code, fail_reason, fallback_trunk_id, "
                " timeout_ms, retry_count, etag) "
                "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)",
                (name, enabled, priority, description,
                 target_mode, target_trunk_id, target_service_id, target_json,
                 fail_action, fail_code, fail_reason,
                 fallback_trunk_id, timeout_ms, retry_count, etag),
            )
            new_id = cur.lastrowid
            _write_rule_subtables(cur, new_id, body)
            full = _load_rule_full(cur, new_id)
    except pymysql.err.IntegrityError as e:
        return HandlerResult(status=409, body={"error": "conflict", "detail": str(e)},
                             media_type="application/json")
    finally:
        conn.close()

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "route", new_id, "CREATE", after=full, etag_after=etag)
    notify_config_change("route", new_id, "CREATE", actor=actor)
    return HandlerResult(status=201, body=full, media_type="application/json")


async def _update_route(handler_args: HandlerArgs, rid: int, config):
    body = _parse_body(handler_args)
    if not body:
        return HandlerResult(status=400, body={"error": "empty_body"}, media_type="application/json")
    (name, enabled, priority, description, target_mode, target_trunk_id,
     target_service_id, target_json, fail_action, fail_code, fail_reason,
     fallback_trunk_id, timeout_ms, retry_count) = _rule_fields_from_body(body)
    etag = _compute_etag()
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            before = _load_rule_full(cur, rid)
            if not before:
                return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
            cur.execute(
                "UPDATE routing_rule SET "
                "name=%s, enabled=%s, priority=%s, description=%s, "
                "target_mode=%s, target_trunk_id=%s, target_service_id=%s, target_json=%s, "
                "fail_action=%s, fail_code=%s, fail_reason=%s, fallback_trunk_id=%s, "
                "timeout_ms=%s, retry_count=%s, etag=%s "
                "WHERE id=%s",
                (name, enabled, priority, description,
                 target_mode, target_trunk_id, target_service_id, target_json,
                 fail_action, fail_code, fail_reason,
                 fallback_trunk_id, timeout_ms, retry_count, etag, rid),
            )
            # match/transform 는 항상 재작성
            _write_rule_subtables(cur, rid, body)
            after = _load_rule_full(cur, rid)
    except pymysql.err.IntegrityError as e:
        return HandlerResult(status=409, body={"error": "conflict", "detail": str(e)},
                             media_type="application/json")
    finally:
        conn.close()

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "route", rid, "UPDATE", before=before, after=after,
                        etag_before=before.get("etag", ""), etag_after=etag)
    notify_config_change("route", rid, "UPDATE", actor=actor)
    return HandlerResult(status=200, body=after, media_type="application/json")


async def _delete_route(handler_args: HandlerArgs, rid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            before = _load_rule_full(cur, rid)
            if not before:
                return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
            cur.execute("DELETE FROM routing_rule WHERE id=%s", (rid,))
    finally:
        conn.close()

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "route", rid, "DELETE", before=before,
                        etag_before=before.get("etag", ""))
    notify_config_change("route", rid, "DELETE", actor=actor)
    return HandlerResult(status=204, body=None, media_type="application/json")


async def _get_hits(rid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT id, name, hit_count, last_hit_time FROM routing_rule WHERE id=%s", (rid,))
            r = cur.fetchone()
    finally:
        conn.close()
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body={
        "id": r["id"], "name": r["name"],
        "hit_count": r.get("hit_count", 0),
        "last_hit_time": r["last_hit_time"].isoformat() if r.get("last_hit_time") else None,
    }, media_type="application/json")


async def _dryrun(handler_args: HandlerArgs, config):
    """샘플 SIP 메시지로 규칙 평가. 순수 Python 구현 — CSP 에 질의하지 않음.

    요청 body 예:
      {
        "sample": {
          "method": "INVITE",
          "req_uri_user": "82231112222",
          "req_uri_host": "example.com",
          "from_uri": "sip:1001@ims.mnc001...",
          "to_uri":   "sip:82231112222@ims.mnc001...",
          "source_ip": "1.2.3.4",
          "headers": {"P-Asserted-Identity": "..."}
        }
      }

    응답:
      {"matched": true, "rule_id":N, "rule_name":"...", "apply":[...], "target":{...}}
      또는 {"matched": false}
    """
    body = _parse_body(handler_args)
    sample = body.get("sample") or {}
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM routing_rule WHERE enabled=1 ORDER BY priority, id")
            rules = cur.fetchall()
            for r in rules:
                cur.execute("SELECT * FROM routing_rule_match WHERE rule_id=%s ORDER BY seq", (r["id"],))
                matches = cur.fetchall()
                if _matches_all(matches, sample):
                    cur.execute("SELECT * FROM routing_rule_transform WHERE rule_id=%s ORDER BY seq", (r["id"],))
                    transforms = cur.fetchall()
                    return HandlerResult(status=200, body={
                        "matched": True,
                        "rule_id": r["id"],
                        "rule_name": r["name"],
                        "apply": [
                            {"action": t["action"], "target": t.get("target"), "value": t.get("value")}
                            for t in transforms
                        ],
                        "target": {
                            "mode": r["target_mode"],
                            "trunk_id": r.get("target_trunk_id"),
                        },
                    }, media_type="application/json")
    finally:
        conn.close()
    return HandlerResult(status=200, body={"matched": False}, media_type="application/json")


def _matches_all(matches: list, sample: dict) -> bool:
    headers = sample.get("headers") or {}
    def _val(field: str) -> str:
        if field.startswith("header:"):
            name = field[7:]
            for k, v in headers.items():
                if k.lower() == name.lower():
                    return str(v)
            return ""
        return str(sample.get(field, ""))
    for m in matches:
        v = _val(m["field"])
        ok = False
        op = m.get("op", "equals")
        val = m.get("value", "")
        if op == "equals":       ok = v == val
        elif op == "not_equals": ok = v != val
        elif op == "prefix":     ok = v.startswith(val)
        elif op == "suffix":     ok = v.endswith(val)
        elif op == "contains":   ok = val in v
        elif op == "regex":
            import re
            try: ok = bool(re.search(val, v))
            except Exception: ok = False
        if m.get("invert"):
            ok = not ok
        if not ok:
            return False
    return True


# ──────────────────────────────────────────────────────────────
#  Access control handler
# ──────────────────────────────────────────────────────────────

_ACCESS_BASE = "/api/v1/csp/access"


def _access_row_to_json(r: dict) -> dict:
    return {
        "id": r["id"],
        "scope": r["scope"],
        "scope_ref_id": r.get("scope_ref_id"),
        "kind": r["kind"],
        "match_type": r["match_type"],
        "value": r["value"],
        "enabled": bool(r["enabled"]),
        "priority": r["priority"],
        "note": r.get("note"),
        "etag": r.get("etag") or "",
        "create_time": r["create_time"].isoformat() if r.get("create_time") else None,
        "update_time": r["update_time"].isoformat() if r.get("update_time") else None,
    }


async def handle_access(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    tail = _path_tail(handler_args.full_path, _ACCESS_BASE)
    method = handler_args.method.upper()

    if len(tail) == 0:
        if method == "GET":  return await _list_access(config)
        if method == "POST": return await _create_access(handler_args, config)
        return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")

    try: aid = int(tail[0])
    except (TypeError, ValueError):
        return HandlerResult(status=400, body={"error": "invalid_id"}, media_type="application/json")

    if method == "GET":    return await _get_access(aid, config)
    if method == "PUT":    return await _update_access(handler_args, aid, config)
    if method == "DELETE": return await _delete_access(handler_args, aid, config)
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


async def _list_access(config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM routing_access_list ORDER BY priority, id")
            rows = cur.fetchall()
    finally:
        conn.close()
    return HandlerResult(status=200, body={"items": [_access_row_to_json(r) for r in rows]},
                         media_type="application/json")


async def _get_access(aid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM routing_access_list WHERE id=%s", (aid,))
            r = cur.fetchone()
    finally:
        conn.close()
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body=_access_row_to_json(r), media_type="application/json")


async def _create_access(handler_args: HandlerArgs, config):
    body = _parse_body(handler_args)
    scope = (body.get("scope") or "global").lower()
    kind  = (body.get("kind")  or "allow").lower()
    mtype = (body.get("match_type") or "ip").lower()
    value = (body.get("value") or "").strip()
    if not value:
        return HandlerResult(status=400, body={"error": "value required"}, media_type="application/json")
    if scope not in ("global", "listener", "trunk"):
        return HandlerResult(status=400, body={"error": "invalid_scope"}, media_type="application/json")
    if kind not in ("allow", "deny"):
        return HandlerResult(status=400, body={"error": "invalid_kind"}, media_type="application/json")
    if mtype not in ("ip", "cidr", "ua_regex"):
        return HandlerResult(status=400, body={"error": "invalid_match_type"}, media_type="application/json")
    etag = _compute_etag()
    enabled = 0 if body.get("enabled") in (False, "false", 0, "0") else 1
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO routing_access_list "
                "(scope, scope_ref_id, kind, match_type, value, enabled, priority, note, etag) "
                "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s)",
                (scope, body.get("scope_ref_id") or None, kind, mtype, value,
                 enabled, int(body.get("priority") or 100),
                 body.get("note"), etag),
            )
            new_id = cur.lastrowid
            cur.execute("SELECT * FROM routing_access_list WHERE id=%s", (new_id,))
            r = cur.fetchone()
    finally:
        conn.close()
    rowJson = _access_row_to_json(r)
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "access", new_id, "CREATE", after=rowJson, etag_after=etag)
    notify_config_change("access", new_id, "CREATE", actor=actor)
    return HandlerResult(status=201, body=rowJson, media_type="application/json")


async def _update_access(handler_args: HandlerArgs, aid: int, config):
    body = _parse_body(handler_args)
    if not body:
        return HandlerResult(status=400, body={"error": "empty_body"}, media_type="application/json")
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM routing_access_list WHERE id=%s", (aid,))
            before = cur.fetchone()
            if not before:
                return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
            etag_before = before.get("etag", "")
            fields = []; values = []
            for col in ("scope", "scope_ref_id", "kind", "match_type", "value", "priority", "note"):
                if col in body:
                    fields.append(f"{col}=%s"); values.append(body[col])
            if "enabled" in body:
                fields.append("enabled=%s"); values.append(0 if body["enabled"] in (False, "false", 0, "0") else 1)
            if not fields:
                return HandlerResult(status=400, body={"error": "no_updatable_fields"}, media_type="application/json")
            etag_after = _compute_etag()
            fields.append("etag=%s"); values.append(etag_after)
            values.append(aid)
            cur.execute(f"UPDATE routing_access_list SET {', '.join(fields)} WHERE id=%s", values)
            cur.execute("SELECT * FROM routing_access_list WHERE id=%s", (aid,))
            after = cur.fetchone()
    finally:
        conn.close()
    afterJson = _access_row_to_json(after)
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "access", aid, "UPDATE",
                        before=_access_row_to_json(before), after=afterJson,
                        etag_before=etag_before, etag_after=etag_after)
    notify_config_change("access", aid, "UPDATE", actor=actor)
    return HandlerResult(status=200, body=afterJson, media_type="application/json")


async def _delete_access(handler_args: HandlerArgs, aid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM routing_access_list WHERE id=%s", (aid,))
            before = cur.fetchone()
            if not before:
                return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
            cur.execute("DELETE FROM routing_access_list WHERE id=%s", (aid,))
    finally:
        conn.close()
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "access", aid, "DELETE",
                        before=_access_row_to_json(before),
                        etag_before=before.get("etag", ""))
    notify_config_change("access", aid, "DELETE", actor=actor)
    return HandlerResult(status=204, body=None, media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  Service handler (P7)
# ──────────────────────────────────────────────────────────────

_SERVICE_BASE = "/api/v1/csp/services"


def _service_row_to_json(r: dict, listener_ids: list) -> dict:
    return {
        "id": r["id"],
        "name": r["name"],
        "kind": r["kind"],
        "domain": r["domain"],
        "auth_realm": r.get("auth_realm"),
        "inbound_policy": r["inbound_policy"],
        "priority": r["priority"],
        "enabled": bool(r["enabled"]),
        "listeners": listener_ids,
        "note": r.get("note"),
        "etag": r.get("etag") or "",
        "create_time": r["create_time"].isoformat() if r.get("create_time") else None,
        "update_time": r["update_time"].isoformat() if r.get("update_time") else None,
    }


def _load_service_full(cur, sid: int):
    cur.execute("SELECT * FROM sip_service WHERE id=%s", (sid,))
    r = cur.fetchone()
    if not r: return None
    cur.execute("SELECT listener_id FROM sip_service_listener WHERE service_id=%s", (sid,))
    listener_ids = [row["listener_id"] for row in cur.fetchall()]
    return _service_row_to_json(r, listener_ids)


def _write_service_listeners(cur, sid: int, listener_ids):
    cur.execute("DELETE FROM sip_service_listener WHERE service_id=%s", (sid,))
    for lid in (listener_ids or []):
        try: lid = int(lid)
        except Exception: continue
        cur.execute("INSERT IGNORE INTO sip_service_listener (service_id, listener_id) VALUES (%s,%s)",
                    (sid, lid))


async def handle_services(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    tail = _path_tail(handler_args.full_path, _SERVICE_BASE)
    method = handler_args.method.upper()

    if len(tail) == 0:
        if method == "GET":  return await _list_services(config)
        if method == "POST": return await _create_service(handler_args, config)
        return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")

    try: sid = int(tail[0])
    except (TypeError, ValueError):
        return HandlerResult(status=400, body={"error": "invalid_id"}, media_type="application/json")

    if method == "GET":    return await _get_service(sid, config)
    if method == "PUT":    return await _update_service(handler_args, sid, config)
    if method == "DELETE": return await _delete_service(handler_args, sid, config)
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


async def _list_services(config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM sip_service ORDER BY priority, id")
            rows = cur.fetchall()
            items = []
            for r in rows:
                cur.execute("SELECT listener_id FROM sip_service_listener WHERE service_id=%s", (r["id"],))
                lids = [lr["listener_id"] for lr in cur.fetchall()]
                items.append(_service_row_to_json(r, lids))
    finally:
        conn.close()
    return HandlerResult(status=200, body={"items": items}, media_type="application/json")


async def _get_service(sid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            row = _load_service_full(cur, sid)
    finally:
        conn.close()
    if not row:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body=row, media_type="application/json")


def _service_fields_from_body(body: dict):
    return (
        (body.get("name") or "").strip(),
        (body.get("kind") or "").strip(),
        (body.get("domain") or "").strip(),
        body.get("auth_realm"),
        (body.get("inbound_policy") or "any").lower(),
        int(body.get("priority") or 100),
        0 if body.get("enabled") in (False, "false", 0, "0") else 1,
        body.get("note"),
    )


async def _create_service(handler_args: HandlerArgs, config):
    body = _parse_body(handler_args)
    name, kind, domain, auth_realm, policy, priority, enabled, note = _service_fields_from_body(body)
    if not name or not kind or not domain:
        return HandlerResult(status=400, body={"error": "name/kind/domain required"},
                             media_type="application/json")
    if kind not in ("voip", "ptt", "ibcf", "system", "console"):
        return HandlerResult(status=400, body={"error": "invalid_kind"}, media_type="application/json")
    if policy not in ("any", "restricted"):
        return HandlerResult(status=400, body={"error": "invalid_inbound_policy"}, media_type="application/json")

    etag = _compute_etag()
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO sip_service (name, kind, domain, auth_realm, inbound_policy, priority, enabled, note, etag) "
                "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s)",
                (name, kind, domain, auth_realm, policy, priority, enabled, note, etag),
            )
            new_id = cur.lastrowid
            _write_service_listeners(cur, new_id, body.get("listeners"))
            full = _load_service_full(cur, new_id)
    except pymysql.err.IntegrityError as e:
        return HandlerResult(status=409, body={"error": "conflict", "detail": str(e)},
                             media_type="application/json")
    finally:
        conn.close()

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "service", new_id, "CREATE", after=full, etag_after=etag)
    notify_config_change("service", new_id, "CREATE", actor=actor)
    return HandlerResult(status=201, body=full, media_type="application/json")


async def _update_service(handler_args: HandlerArgs, sid: int, config):
    body = _parse_body(handler_args)
    if not body:
        return HandlerResult(status=400, body={"error": "empty_body"}, media_type="application/json")
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            before = _load_service_full(cur, sid)
            if not before:
                return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
            etag_before = before.get("etag", "")
            fields = []; values = []
            for col in ("name", "kind", "domain", "auth_realm", "inbound_policy",
                        "priority", "note"):
                if col in body:
                    fields.append(f"{col}=%s"); values.append(body[col])
            if "enabled" in body:
                fields.append("enabled=%s")
                values.append(0 if body["enabled"] in (False, "false", 0, "0") else 1)
            etag_after = _compute_etag()
            if fields:
                fields.append("etag=%s"); values.append(etag_after); values.append(sid)
                cur.execute(f"UPDATE sip_service SET {', '.join(fields)} WHERE id=%s", values)
            if "listeners" in body:
                _write_service_listeners(cur, sid, body["listeners"])
            after = _load_service_full(cur, sid)
    except pymysql.err.IntegrityError as e:
        return HandlerResult(status=409, body={"error": "conflict", "detail": str(e)},
                             media_type="application/json")
    finally:
        conn.close()

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "service", sid, "UPDATE", before=before, after=after,
                        etag_before=etag_before, etag_after=etag_after)
    notify_config_change("service", sid, "UPDATE", actor=actor)
    return HandlerResult(status=200, body=after, media_type="application/json")


async def _delete_service(handler_args: HandlerArgs, sid: int, config):
    conn = _get_db(config)
    try:
        with conn.cursor() as cur:
            before = _load_service_full(cur, sid)
            if not before:
                return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
            cur.execute("DELETE FROM sip_service WHERE id=%s", (sid,))
    finally:
        conn.close()

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "service", sid, "DELETE", before=before,
                        etag_before=before.get("etag", ""))
    notify_config_change("service", sid, "DELETE", actor=actor)
    return HandlerResult(status=204, body=None, media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  Handler list
# ──────────────────────────────────────────────────────────────

CIMS_CSP_RUNTIME_HANDLER_LIST = (
    (_LISTENER_BASE, handle_listeners, {}),
    (_TRUNK_BASE,    handle_trunks,    {}),
    (_ROUTE_BASE,    handle_routes,    {}),
    (_ACCESS_BASE,   handle_access,    {}),
    (_SERVICE_BASE,  handle_services,  {}),
)
