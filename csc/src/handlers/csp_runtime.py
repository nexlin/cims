"""
CSP 런타임 설정 Admin API — ⚠️ **DEPRECATED 경로**.

Routes (prefix-matched, admin JWT required):
  /api/v1/csp/listeners                         GET list  / POST create
  /api/v1/csp/listeners/{id}                    GET / PUT / DELETE
  /api/v1/csp/trunks                            (동일 패턴)
  /api/v1/csp/routes                            (동일 패턴)
  /api/v1/csp/access                            (동일 패턴)
  /api/v1/csp/services                          (동일 패턴)

상태 (T5 — 2026-05-18 정리):
  - 현재 cims-console UI 가 부르지 않음 (deployment.ts 에 호출 없음)
  - 진짜 운영 경로는 /api/v1/deployments/<id>/collection/<name>
    (agents.py:_put_deployment_collection — install_path 의 jsonl 을 agent proxy
     로 직접 편집 + HA fan-out)
  - 본 모듈의 file_store 도메인 (csp_listener / sip_trunk / routing_rule /
    routing_access_list / sip_service) 은 옛 DB 시대 캐시 잔존.
    CSP 본체가 읽는 jsonl 과 별개 — write 해도 CSP 가 반영하지 않음.

남겨두는 이유:
  - 단위 테스트 / migration 스크립트 (csc/scripts/migrate_csp_runtime_db_to_file.py)
    가 도메인 이름을 참조 — 동시 제거는 위험.
  - sync_dispatch.py 의 fan-out 단위 테스트가 본 모듈의 도메인 매핑을 사용.

차후 정리 (별도 트랙):
  1. cims-console / docs 에서 이 경로 안 부르는 게 확실해지면 dispatcher 등록 제거.
  2. file_store 도메인 폐기 마이그레이션 스크립트 — install_path 의 jsonl 이 SoT.
  3. config_cache.py 도 같이 정리.

변경 시 흐름 (옛 — 참고용):
  1. file_store CUD
  2. audit_config_change(...) — csp_config_audit 에 변경 기록
  3. notify_config_change("listener", id, action) — 단일 host 호환 UDP notify
  4. _fanout() — HA 멤버에 sync_config job (T1 commit 9b5699b 후속). 운영 경로
                 는 위 deployments/<id>/collection/<name> 이지만 본 경로도 fan-out
                 동일하게 동작 (호환).
"""

from __future__ import annotations

import json
import secrets
from typing import Optional
from urllib.parse import urlparse, unquote
from pathlib import PurePath

from httpsrv.handler import HandlerArgs, HandlerResult
from util.log_util import Logger
from services import file_store

import asyncio

from services.mcptt import notify_config_change, audit_config_change
from services.sync_dispatch import enqueue_collection_sync


async def _fanout(config, *, entity: str, op: str, row_id: int, actor: str):
    """ha_group fan-out — 멤버들에 sync_config job 일괄 enqueue + sync_txn 생성.
    csc 와 동일 호스트의 CSP 1대만 있는 환경은 (멤버 0건) sync_id=None 반환.
    """
    return await asyncio.to_thread(enqueue_collection_sync, config,
                                   entity=entity, op=op, row_id=row_id, actor=actor)

logger = Logger()

# ──────────────────────────────────────────────────────────────
#  file_store domains
# ──────────────────────────────────────────────────────────────

_DOM_LISTENER = 'csp_listener'
_DOM_TRUNK    = 'sip_trunk'
_DOM_ROUTE    = 'routing_rule'      # match/transform 임베드
_DOM_ACCESS   = 'routing_access_list'
_DOM_SERVICE  = 'sip_service'       # listener_ids 임베드


def _dom_dir(config, domain):
    return file_store.domain_dir(config, domain)


def _now_iso():
    from datetime import datetime as _dt
    return _dt.now().isoformat(timespec='seconds')


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


def _iso_or_none(v):
    if v is None: return None
    if hasattr(v, "isoformat"): return v.isoformat()
    return v


def _row_to_json(r: dict) -> dict:
    return {
        "id": r.get("id"),
        "name": r.get("name"),
        "enabled": bool(r.get("enabled")),
        "bind_ip": r.get("bind_ip"),
        "bind_port": r.get("bind_port"),
        "protocol": r.get("protocol"),
        "domain": r.get("domain") or "",
        "service": r.get("service"),
        "tls_cert_path": r.get("tls_cert_path"),
        "tls_key_path":  r.get("tls_key_path"),
        "tls_ca_path":   r.get("tls_ca_path"),
        "tls_verify_peer": bool(r.get("tls_verify_peer")),
        "max_connections": r.get("max_connections", 0),
        "thread_count":    r.get("thread_count", 2),
        "note": r.get("note"),
        "etag": r.get("etag") or "",
        "create_time": _iso_or_none(r.get("create_time")),
        "update_time": _iso_or_none(r.get("update_time")),
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
    rows = file_store.load_all(_dom_dir(config, _DOM_LISTENER))
    rows.sort(key=lambda r: r.get("id", 0))
    return HandlerResult(status=200, body={"items": [_row_to_json(r) for r in rows]},
                         media_type="application/json")


async def _get_listener(lid: int, config):
    r = file_store.by_id(_dom_dir(config, _DOM_LISTENER), lid)
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
    d = _dom_dir(config, _DOM_LISTENER)
    # 중복 name 검사 (옛 UNIQUE 대체)
    if file_store.find_by(d, lambda o: o.get("name") == name):
        return HandlerResult(status=409, body={"error": "conflict", "detail": f"name '{name}' exists"},
                             media_type="application/json")
    new_id = file_store.next_id(d)
    r = {
        "id": new_id,
        "name": name, "enabled": enabled, "bind_ip": ip, "bind_port": port,
        "protocol": proto, "domain": domain, "service": svc,
        "tls_cert_path": body.get("tls_cert_path"),
        "tls_key_path":  body.get("tls_key_path"),
        "tls_ca_path":   body.get("tls_ca_path"),
        "tls_verify_peer": 1 if body.get("tls_verify_peer") else 0,
        "max_connections": int(body.get("max_connections") or 0),
        "thread_count": threadCount,
        "note": note, "etag": etag,
    }
    file_store.save(d, new_id, r)

    rowJson = _row_to_json(r)
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "listener", new_id, "CREATE", after=rowJson,
                        etag_after=etag)
    notify_config_change("listener", new_id, "CREATE", actor=actor)
    sync_id = await _fanout(config, entity="listener", op="CREATE", row_id=new_id, actor=actor)
    if sync_id is not None:
        rowJson["_sync_id"] = sync_id
    return HandlerResult(status=201, body=rowJson, media_type="application/json")


async def _update_listener(handler_args: HandlerArgs, lid: int, config):
    body = _parse_body(handler_args)
    if not body:
        return HandlerResult(status=400, body={"error": "empty_body"}, media_type="application/json")

    d = _dom_dir(config, _DOM_LISTENER)
    before = file_store.by_id(d, lid)
    if not before:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    etag_before = before.get("etag", "")

    patches: dict = {}
    for col in ("name", "bind_ip", "bind_port", "protocol", "domain", "service",
                "tls_cert_path", "tls_key_path", "tls_ca_path",
                "max_connections", "thread_count", "note"):
        if col in body:
            patches[col] = body[col]
    if "enabled" in body:
        patches["enabled"] = 0 if body["enabled"] in (False, "false", 0, "0") else 1
    if "tls_verify_peer" in body:
        patches["tls_verify_peer"] = 1 if body["tls_verify_peer"] else 0
    if not patches:
        return HandlerResult(status=400, body={"error": "no_updatable_fields"}, media_type="application/json")

    # name 변경 시 중복 검사
    if "name" in patches and patches["name"] != before.get("name"):
        if file_store.find_by(d, lambda o: o.get("id") != lid and o.get("name") == patches["name"]):
            return HandlerResult(status=409, body={"error": "conflict", "detail": "duplicate name"},
                                 media_type="application/json")

    etag_after = _compute_etag()
    patches["etag"] = etag_after
    after = dict(before); after.update(patches)
    file_store.save(d, lid, after)

    afterJson = _row_to_json(after)
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "listener", lid, "UPDATE",
                        before=_row_to_json(before), after=afterJson,
                        etag_before=etag_before, etag_after=etag_after)
    notify_config_change("listener", lid, "UPDATE", actor=actor)
    sync_id = await _fanout(config, entity="listener", op="UPDATE", row_id=lid, actor=actor)
    if sync_id is not None:
        afterJson["_sync_id"] = sync_id
    return HandlerResult(status=200, body=afterJson, media_type="application/json")


async def _delete_listener(handler_args: HandlerArgs, lid: int, config):
    d = _dom_dir(config, _DOM_LISTENER)
    before = file_store.by_id(d, lid)
    if not before:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    file_store.delete(d, lid)

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "listener", lid, "DELETE",
                        before=_row_to_json(before),
                        etag_before=before.get("etag", ""))
    notify_config_change("listener", lid, "DELETE", actor=actor)
    sync_id = await _fanout(config, entity="listener", op="DELETE", row_id=lid, actor=actor)
    hdrs = {"X-CIMS-Sync-Id": str(sync_id)} if sync_id is not None else {}
    return HandlerResult(status=204, body=None, headers=hdrs, media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  Trunk handler
# ──────────────────────────────────────────────────────────────

_TRUNK_BASE = "/api/v1/csp/trunks"


def _trunk_row_to_json(r: dict) -> dict:
    return {
        "id": r.get("id"),
        "name": r.get("name"),
        "enabled": bool(r.get("enabled")),
        "service_id": r.get("service_id"),
        "failover_priority": r.get("failover_priority", 100),
        "remote_ip": r.get("remote_ip"),
        "remote_port": r.get("remote_port"),
        "remote_domain": r.get("remote_domain") or "",
        "protocol": r.get("protocol"),
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
        "create_time": _iso_or_none(r.get("create_time")),
        "update_time": _iso_or_none(r.get("update_time")),
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
    rows = file_store.load_all(_dom_dir(config, _DOM_TRUNK))
    rows.sort(key=lambda r: r.get("id", 0))
    return HandlerResult(status=200, body={"items": [_trunk_row_to_json(r) for r in rows]},
                         media_type="application/json")


async def _get_trunk(tid: int, config):
    r = file_store.by_id(_dom_dir(config, _DOM_TRUNK), tid)
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

    d = _dom_dir(config, _DOM_TRUNK)
    if file_store.find_by(d, lambda o: o.get("name") == name):
        return HandlerResult(status=409, body={"error": "conflict", "detail": f"name '{name}' exists"},
                             media_type="application/json")

    svc_id = body.get("service_id")
    if svc_id in (None, 0, "", "0"): svc_id = None
    else:
        try: svc_id = int(svc_id)
        except Exception: svc_id = None
    etag = _compute_etag()
    enabled = 0 if body.get("enabled") in (False, "false", 0, "0") else 1
    new_id = file_store.next_id(d)
    r = {
        "id": new_id,
        "name": name, "enabled": enabled, "service_id": svc_id,
        "failover_priority": int(body.get("failover_priority") or 100),
        "remote_ip": ip, "remote_port": port,
        "remote_domain": body.get("remote_domain", ""), "protocol": proto,
        "outbound_proxy_ip": body.get("outbound_proxy_ip"),
        "outbound_proxy_port": body.get("outbound_proxy_port"),
        "register_to_remote": 1 if body.get("register_to_remote") else 0,
        "auth_user": body.get("auth_user"),
        "auth_password": body.get("auth_password"),
        "auth_realm": body.get("auth_realm"),
        "register_expires": int(body.get("register_expires") or 3600),
        "options_ping_sec": int(body.get("options_ping_sec") or 60),
        "options_dead_threshold": int(body.get("options_dead_threshold") or 3),
        "srv_lookup": 1 if body.get("srv_lookup") else 0,
        "dns_fallback": 0 if body.get("dns_fallback") in (False, "false", 0, "0") else 1,
        "max_concurrent_calls": int(body.get("max_concurrent_calls") or 0),
        "cps_limit": int(body.get("cps_limit") or 0),
        "note": body.get("note"), "etag": etag,
    }
    file_store.save(d, new_id, r)

    rowJson = _trunk_row_to_json(r)
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "trunk", new_id, "CREATE", after=rowJson, etag_after=etag)
    notify_config_change("trunk", new_id, "CREATE", actor=actor)
    sync_id = await _fanout(config, entity="trunk", op="CREATE", row_id=new_id, actor=actor)
    if sync_id is not None:
        rowJson["_sync_id"] = sync_id
    return HandlerResult(status=201, body=rowJson, media_type="application/json")


async def _update_trunk(handler_args: HandlerArgs, tid: int, config):
    body = _parse_body(handler_args)
    if not body:
        return HandlerResult(status=400, body={"error": "empty_body"}, media_type="application/json")

    d = _dom_dir(config, _DOM_TRUNK)
    before = file_store.by_id(d, tid)
    if not before:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    etag_before = before.get("etag", "")

    updatable_cols = ("name", "service_id", "failover_priority",
                      "remote_ip", "remote_port", "remote_domain", "protocol",
                      "outbound_proxy_ip", "outbound_proxy_port",
                      "auth_user", "auth_password", "auth_realm", "register_expires",
                      "options_ping_sec", "options_dead_threshold",
                      "max_concurrent_calls", "cps_limit", "note")
    patches: dict = {}
    for col in updatable_cols:
        if col in body:
            patches[col] = body[col]
    for bool_col in ("enabled", "register_to_remote", "srv_lookup", "dns_fallback"):
        if bool_col in body:
            patches[bool_col] = 0 if body[bool_col] in (False, "false", 0, "0") else 1
    if not patches:
        return HandlerResult(status=400, body={"error": "no_updatable_fields"},
                             media_type="application/json")
    if "name" in patches and patches["name"] != before.get("name"):
        if file_store.find_by(d, lambda o: o.get("id") != tid and o.get("name") == patches["name"]):
            return HandlerResult(status=409, body={"error": "conflict", "detail": "duplicate name"},
                                 media_type="application/json")
    etag_after = _compute_etag()
    patches["etag"] = etag_after
    after = dict(before); after.update(patches)
    file_store.save(d, tid, after)

    afterJson = _trunk_row_to_json(after)
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "trunk", tid, "UPDATE",
                        before=_trunk_row_to_json(before), after=afterJson,
                        etag_before=etag_before, etag_after=etag_after)
    notify_config_change("trunk", tid, "UPDATE", actor=actor)
    sync_id = await _fanout(config, entity="trunk", op="UPDATE", row_id=tid, actor=actor)
    if sync_id is not None:
        afterJson["_sync_id"] = sync_id
    return HandlerResult(status=200, body=afterJson, media_type="application/json")


async def _delete_trunk(handler_args: HandlerArgs, tid: int, config):
    d = _dom_dir(config, _DOM_TRUNK)
    before = file_store.by_id(d, tid)
    if not before:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    file_store.delete(d, tid)

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "trunk", tid, "DELETE",
                        before=_trunk_row_to_json(before),
                        etag_before=before.get("etag", ""))
    notify_config_change("trunk", tid, "DELETE", actor=actor)
    sync_id = await _fanout(config, entity="trunk", op="DELETE", row_id=tid, actor=actor)
    hdrs = {"X-CIMS-Sync-Id": str(sync_id)} if sync_id is not None else {}
    return HandlerResult(status=204, body=None, headers=hdrs, media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  Route rule handler
# ──────────────────────────────────────────────────────────────

_ROUTE_BASE = "/api/v1/csp/routes"


def _rule_to_json(r: dict) -> dict:
    """file_store rule dict → API JSON. match/transform 가 이미 임베드 형태."""
    matches = r.get("match") or []
    transforms = r.get("transform") or []
    return {
        "id": r.get("id"),
        "name": r.get("name"),
        "enabled": bool(r.get("enabled")),
        "priority": r.get("priority"),
        "description": r.get("description"),
        "match": [
            {
                "field": m.get("field"), "op": m.get("op"), "value": m.get("value"),
                "invert": bool(m.get("invert")), "seq": m.get("seq", 0),
            } for m in matches
        ],
        "transform": [
            {
                "action": t.get("action"), "target": t.get("target"),
                "value": t.get("value"), "seq": t.get("seq", 0),
            } for t in transforms
        ],
        "target": {
            "mode": r.get("target_mode"),
            "trunk_id": r.get("target_trunk_id"),
            "service_id": r.get("target_service_id"),
            "json": r.get("target_json") if isinstance(r.get("target_json"), (dict, list))
                    else (json.loads(r["target_json"]) if r.get("target_json") else None),
        },
        "fail": {
            "action": r.get("fail_action"),
            "code": r.get("fail_code"),
            "reason": r.get("fail_reason"),
            "fallback": r.get("fallback_trunk_id"),
            "timeout_ms": r.get("timeout_ms"),
            "retry_count": r.get("retry_count"),
        },
        "hit_count": r.get("hit_count", 0),
        "last_hit_time": _iso_or_none(r.get("last_hit_time")),
        "etag": r.get("etag") or "",
        "create_time": _iso_or_none(r.get("create_time")),
        "update_time": _iso_or_none(r.get("update_time")),
    }


def _normalize_subtables(body: dict) -> tuple:
    """body 의 match/transform 을 정규화 (action 없는 entry 는 skip)."""
    matches = []
    for i, m in enumerate(body.get("match") or []):
        if not m.get("field"): continue
        matches.append({
            "field": m["field"], "op": m.get("op", "equals"),
            "value": m.get("value", ""), "invert": 1 if m.get("invert") else 0,
            "seq": m.get("seq", i),
        })
    transforms = []
    for i, t in enumerate(body.get("transform") or []):
        if not t.get("action"): continue
        transforms.append({
            "action": t["action"], "target": t.get("target"),
            "value": t.get("value"), "seq": t.get("seq", i),
        })
    return matches, transforms


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
    rows = file_store.load_all(_dom_dir(config, _DOM_ROUTE))
    rows.sort(key=lambda r: (r.get("priority") or 0, r.get("id") or 0))
    return HandlerResult(status=200, body={"items": [_rule_to_json(r) for r in rows]},
                         media_type="application/json")


async def _get_route(rid: int, config):
    row = file_store.by_id(_dom_dir(config, _DOM_ROUTE), rid)
    if not row:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body=_rule_to_json(row), media_type="application/json")


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


def _build_rule_record(rid: int, body: dict, etag: str, existing: dict = None) -> dict:
    """body → file_store 저장용 rule record. match/transform 임베드."""
    target = body.get("target") or {}
    fail = body.get("fail") or {}
    matches, transforms = _normalize_subtables(body)
    rec = {
        "id": rid,
        "name": (body.get("name") or "").strip(),
        "enabled": 0 if body.get("enabled") in (False, "false", 0, "0") else 1,
        "priority": int(body.get("priority") or 100),
        "description": body.get("description") or "",
        "target_mode": (target.get("mode") or "trunk").lower(),
        "target_trunk_id": target.get("trunk_id") or None,
        "target_service_id": target.get("service_id") or None,
        "target_json": target.get("json") if isinstance(target.get("json"), (dict, list)) else None,
        "fail_action": (fail.get("action") or "reject").lower(),
        "fail_code": int(fail.get("code") or 404),
        "fail_reason": fail.get("reason") or "Not Found",
        "fallback_trunk_id": fail.get("fallback") or None,
        "timeout_ms": int(fail.get("timeout_ms") or 4000),
        "retry_count": int(fail.get("retry_count") or 0),
        "match": matches,
        "transform": transforms,
        "etag": etag,
        # hit_count / last_hit_time 은 기존 값 보존 (없으면 0)
        "hit_count": (existing or {}).get("hit_count", 0),
        "last_hit_time": (existing or {}).get("last_hit_time"),
    }
    return rec


async def _create_route(handler_args: HandlerArgs, config):
    body = _parse_body(handler_args)
    name = (body.get("name") or "").strip()
    if not name:
        return HandlerResult(status=400, body={"error": "name required"}, media_type="application/json")
    d = _dom_dir(config, _DOM_ROUTE)
    if file_store.find_by(d, lambda o: o.get("name") == name):
        return HandlerResult(status=409, body={"error": "conflict", "detail": f"name '{name}' exists"},
                             media_type="application/json")
    etag = _compute_etag()
    new_id = file_store.next_id(d)
    rec = _build_rule_record(new_id, body, etag)
    file_store.save(d, new_id, rec)
    full = _rule_to_json(rec)

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "route", new_id, "CREATE", after=full, etag_after=etag)
    notify_config_change("route", new_id, "CREATE", actor=actor)
    sync_id = await _fanout(config, entity="route", op="CREATE", row_id=new_id, actor=actor)
    if sync_id is not None:
        full["_sync_id"] = sync_id
    return HandlerResult(status=201, body=full, media_type="application/json")


async def _update_route(handler_args: HandlerArgs, rid: int, config):
    body = _parse_body(handler_args)
    if not body:
        return HandlerResult(status=400, body={"error": "empty_body"}, media_type="application/json")
    d = _dom_dir(config, _DOM_ROUTE)
    existing = file_store.by_id(d, rid)
    if not existing:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    before = _rule_to_json(existing)
    etag = _compute_etag()
    rec = _build_rule_record(rid, body, etag, existing=existing)
    file_store.save(d, rid, rec)
    after = _rule_to_json(rec)

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "route", rid, "UPDATE", before=before, after=after,
                        etag_before=before.get("etag", ""), etag_after=etag)
    notify_config_change("route", rid, "UPDATE", actor=actor)
    sync_id = await _fanout(config, entity="route", op="UPDATE", row_id=rid, actor=actor)
    if sync_id is not None:
        after["_sync_id"] = sync_id
    return HandlerResult(status=200, body=after, media_type="application/json")


async def _delete_route(handler_args: HandlerArgs, rid: int, config):
    d = _dom_dir(config, _DOM_ROUTE)
    existing = file_store.by_id(d, rid)
    if not existing:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    before = _rule_to_json(existing)
    file_store.delete(d, rid)

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "route", rid, "DELETE", before=before,
                        etag_before=before.get("etag", ""))
    notify_config_change("route", rid, "DELETE", actor=actor)
    sync_id = await _fanout(config, entity="route", op="DELETE", row_id=rid, actor=actor)
    hdrs = {"X-CIMS-Sync-Id": str(sync_id)} if sync_id is not None else {}
    return HandlerResult(status=204, body=None, headers=hdrs, media_type="application/json")


async def _get_hits(rid: int, config):
    r = file_store.by_id(_dom_dir(config, _DOM_ROUTE), rid)
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body={
        "id": r.get("id"), "name": r.get("name"),
        "hit_count": r.get("hit_count", 0),
        "last_hit_time": _iso_or_none(r.get("last_hit_time")),
    }, media_type="application/json")


async def _dryrun(handler_args: HandlerArgs, config):
    """샘플 SIP 메시지로 규칙 평가. 순수 Python 구현."""
    body = _parse_body(handler_args)
    sample = body.get("sample") or {}
    rules = file_store.load_all(_dom_dir(config, _DOM_ROUTE))
    rules = [r for r in rules if r.get("enabled")]
    rules.sort(key=lambda r: (r.get("priority") or 0, r.get("id") or 0))
    for r in rules:
        matches = r.get("match") or []
        if _matches_all(matches, sample):
            transforms = r.get("transform") or []
            return HandlerResult(status=200, body={
                "matched": True,
                "rule_id": r.get("id"),
                "rule_name": r.get("name"),
                "apply": [
                    {"action": t.get("action"), "target": t.get("target"), "value": t.get("value")}
                    for t in transforms
                ],
                "target": {
                    "mode": r.get("target_mode"),
                    "trunk_id": r.get("target_trunk_id"),
                },
            }, media_type="application/json")
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
        "id": r.get("id"),
        "scope": r.get("scope"),
        "scope_ref_id": r.get("scope_ref_id"),
        "kind": r.get("kind"),
        "match_type": r.get("match_type"),
        "value": r.get("value"),
        "enabled": bool(r.get("enabled")),
        "priority": r.get("priority"),
        "note": r.get("note"),
        "etag": r.get("etag") or "",
        "create_time": _iso_or_none(r.get("create_time")),
        "update_time": _iso_or_none(r.get("update_time")),
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
    rows = file_store.load_all(_dom_dir(config, _DOM_ACCESS))
    rows.sort(key=lambda r: (r.get("priority") or 0, r.get("id") or 0))
    return HandlerResult(status=200, body={"items": [_access_row_to_json(r) for r in rows]},
                         media_type="application/json")


async def _get_access(aid: int, config):
    r = file_store.by_id(_dom_dir(config, _DOM_ACCESS), aid)
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
    d = _dom_dir(config, _DOM_ACCESS)
    new_id = file_store.next_id(d)
    r = {
        "id": new_id,
        "scope": scope, "scope_ref_id": body.get("scope_ref_id") or None,
        "kind": kind, "match_type": mtype, "value": value,
        "enabled": enabled, "priority": int(body.get("priority") or 100),
        "note": body.get("note"), "etag": etag,
    }
    file_store.save(d, new_id, r)
    rowJson = _access_row_to_json(r)
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "access", new_id, "CREATE", after=rowJson, etag_after=etag)
    notify_config_change("access", new_id, "CREATE", actor=actor)
    sync_id = await _fanout(config, entity="access", op="CREATE", row_id=new_id, actor=actor)
    if sync_id is not None:
        rowJson["_sync_id"] = sync_id
    return HandlerResult(status=201, body=rowJson, media_type="application/json")


async def _update_access(handler_args: HandlerArgs, aid: int, config):
    body = _parse_body(handler_args)
    if not body:
        return HandlerResult(status=400, body={"error": "empty_body"}, media_type="application/json")
    d = _dom_dir(config, _DOM_ACCESS)
    before = file_store.by_id(d, aid)
    if not before:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    etag_before = before.get("etag", "")
    patches: dict = {}
    for col in ("scope", "scope_ref_id", "kind", "match_type", "value", "priority", "note"):
        if col in body:
            patches[col] = body[col]
    if "enabled" in body:
        patches["enabled"] = 0 if body["enabled"] in (False, "false", 0, "0") else 1
    if not patches:
        return HandlerResult(status=400, body={"error": "no_updatable_fields"}, media_type="application/json")
    etag_after = _compute_etag()
    patches["etag"] = etag_after
    after = dict(before); after.update(patches)
    file_store.save(d, aid, after)
    afterJson = _access_row_to_json(after)
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "access", aid, "UPDATE",
                        before=_access_row_to_json(before), after=afterJson,
                        etag_before=etag_before, etag_after=etag_after)
    notify_config_change("access", aid, "UPDATE", actor=actor)
    sync_id = await _fanout(config, entity="access", op="UPDATE", row_id=aid, actor=actor)
    if sync_id is not None:
        afterJson["_sync_id"] = sync_id
    return HandlerResult(status=200, body=afterJson, media_type="application/json")


async def _delete_access(handler_args: HandlerArgs, aid: int, config):
    d = _dom_dir(config, _DOM_ACCESS)
    before = file_store.by_id(d, aid)
    if not before:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    file_store.delete(d, aid)
    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "access", aid, "DELETE",
                        before=_access_row_to_json(before),
                        etag_before=before.get("etag", ""))
    notify_config_change("access", aid, "DELETE", actor=actor)
    sync_id = await _fanout(config, entity="access", op="DELETE", row_id=aid, actor=actor)
    hdrs = {"X-CIMS-Sync-Id": str(sync_id)} if sync_id is not None else {}
    return HandlerResult(status=204, body=None, headers=hdrs, media_type="application/json")


# ──────────────────────────────────────────────────────────────
#  Service handler (P7)
# ──────────────────────────────────────────────────────────────

_SERVICE_BASE = "/api/v1/csp/services"


def _service_row_to_json(r: dict, listener_ids: list = None) -> dict:
    if listener_ids is None:
        listener_ids = r.get("listeners") or []
    return {
        "id": r.get("id"),
        "name": r.get("name"),
        "kind": r.get("kind"),
        "domain": r.get("domain"),
        "auth_realm": r.get("auth_realm"),
        "inbound_policy": r.get("inbound_policy"),
        "priority": r.get("priority"),
        "enabled": bool(r.get("enabled")),
        "listeners": listener_ids,
        "note": r.get("note"),
        "etag": r.get("etag") or "",
        "create_time": _iso_or_none(r.get("create_time")),
        "update_time": _iso_or_none(r.get("update_time")),
    }


def _normalize_listener_ids(listener_ids):
    out = []
    for lid in (listener_ids or []):
        try: out.append(int(lid))
        except Exception: continue
    # dedup
    seen = set()
    result = []
    for x in out:
        if x not in seen:
            seen.add(x); result.append(x)
    return result


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
    rows = file_store.load_all(_dom_dir(config, _DOM_SERVICE))
    rows.sort(key=lambda r: (r.get("priority") or 0, r.get("id") or 0))
    return HandlerResult(status=200, body={"items": [_service_row_to_json(r) for r in rows]},
                         media_type="application/json")


async def _get_service(sid: int, config):
    r = file_store.by_id(_dom_dir(config, _DOM_SERVICE), sid)
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body=_service_row_to_json(r), media_type="application/json")


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
    if kind not in ("volte", "ptt", "ibcf", "system", "console"):
        return HandlerResult(status=400, body={"error": "invalid_kind"}, media_type="application/json")
    if policy not in ("any", "restricted"):
        return HandlerResult(status=400, body={"error": "invalid_inbound_policy"}, media_type="application/json")

    d = _dom_dir(config, _DOM_SERVICE)
    if file_store.find_by(d, lambda o: o.get("name") == name):
        return HandlerResult(status=409, body={"error": "conflict", "detail": f"name '{name}' exists"},
                             media_type="application/json")
    etag = _compute_etag()
    new_id = file_store.next_id(d)
    listeners = _normalize_listener_ids(body.get("listeners"))
    r = {
        "id": new_id,
        "name": name, "kind": kind, "domain": domain, "auth_realm": auth_realm,
        "inbound_policy": policy, "priority": priority, "enabled": enabled,
        "note": note, "etag": etag, "listeners": listeners,
    }
    file_store.save(d, new_id, r)
    full = _service_row_to_json(r)

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "service", new_id, "CREATE", after=full, etag_after=etag)
    notify_config_change("service", new_id, "CREATE", actor=actor)
    sync_id = await _fanout(config, entity="service", op="CREATE", row_id=new_id, actor=actor)
    if sync_id is not None:
        full["_sync_id"] = sync_id
    return HandlerResult(status=201, body=full, media_type="application/json")


async def _update_service(handler_args: HandlerArgs, sid: int, config):
    body = _parse_body(handler_args)
    if not body:
        return HandlerResult(status=400, body={"error": "empty_body"}, media_type="application/json")
    d = _dom_dir(config, _DOM_SERVICE)
    existing = file_store.by_id(d, sid)
    if not existing:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    before = _service_row_to_json(existing)
    etag_before = before.get("etag", "")

    patches: dict = {}
    for col in ("name", "kind", "domain", "auth_realm", "inbound_policy",
                "priority", "note"):
        if col in body:
            patches[col] = body[col]
    if "enabled" in body:
        patches["enabled"] = 0 if body["enabled"] in (False, "false", 0, "0") else 1
    if "listeners" in body:
        patches["listeners"] = _normalize_listener_ids(body["listeners"])

    if "name" in patches and patches["name"] != existing.get("name"):
        if file_store.find_by(d, lambda o: o.get("id") != sid and o.get("name") == patches["name"]):
            return HandlerResult(status=409, body={"error": "conflict", "detail": "duplicate name"},
                                 media_type="application/json")

    etag_after = _compute_etag()
    patches["etag"] = etag_after
    after_rec = dict(existing); after_rec.update(patches)
    file_store.save(d, sid, after_rec)
    after = _service_row_to_json(after_rec)

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "service", sid, "UPDATE", before=before, after=after,
                        etag_before=etag_before, etag_after=etag_after)
    notify_config_change("service", sid, "UPDATE", actor=actor)
    sync_id = await _fanout(config, entity="service", op="UPDATE", row_id=sid, actor=actor)
    if sync_id is not None:
        after["_sync_id"] = sync_id
    return HandlerResult(status=200, body=after, media_type="application/json")


async def _delete_service(handler_args: HandlerArgs, sid: int, config):
    d = _dom_dir(config, _DOM_SERVICE)
    existing = file_store.by_id(d, sid)
    if not existing:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    before = _service_row_to_json(existing)
    file_store.delete(d, sid)

    actor = _actor_from_headers(handler_args.headers)
    audit_config_change(config.get("CimsDatabase", {}), actor, handler_args.client_ip,
                        "service", sid, "DELETE", before=before,
                        etag_before=before.get("etag", ""))
    notify_config_change("service", sid, "DELETE", actor=actor)
    sync_id = await _fanout(config, entity="service", op="DELETE", row_id=sid, actor=actor)
    hdrs = {"X-CIMS-Sync-Id": str(sync_id)} if sync_id is not None else {}
    return HandlerResult(status=204, body=None, headers=hdrs, media_type="application/json")


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
