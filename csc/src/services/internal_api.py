"""
CSC 내부 API — CSP 전용.

CSP 가 기동/변경 시 전체 또는 단건 설정을 끌어가기 위한 엔드포인트.
외부 노출 차단을 위해 두 가지 제약을 겹친다:
  1. 호출자 IP 가 loopback(127.0.0.0/8 또는 ::1)이어야 함
  2. X-Csp-Internal-Token 헤더가 config 의 InternalToken 과 일치해야 함

경로:
  GET /api/internal/config/meta
  GET /api/internal/config/{entity}                 entity ∈ {listener,trunk,route,access}
  GET /api/internal/config/{entity}/{id}

응답 포맷:
  전체 조회: {"etag": "...", "updated_at": <ts>, "source": "db|file", "items": [...]}
  단건:     해당 entity row (못 찾으면 404)
"""

from __future__ import annotations

import ipaddress
import json
from typing import Optional

from httpsrv.handler import HandlerArgs, HandlerResult
from util.log_util import Logger

from services import config_cache as _cfg_cache

logger = Logger()

_INTERNAL_BASE = "/api/internal/config"

# 프로세스 기동 시 init() 에서 설정
_INTERNAL_TOKEN: Optional[str] = None


def init(config: dict) -> None:
    global _INTERNAL_TOKEN
    _INTERNAL_TOKEN = config.get("InternalToken") or config.get("CspInternal", {}).get("Token")
    if not _INTERNAL_TOKEN:
        logger.log_warning("csc_internal: InternalToken not configured — internal API will reject all requests")


def _is_loopback(ip: str) -> bool:
    if not ip:
        return False
    try:
        return ipaddress.ip_address(ip).is_loopback
    except ValueError:
        # IPv6 scope 등 특수 포맷은 문자열 매칭 fallback
        return ip in ("127.0.0.1", "::1", "localhost")


def _auth_check(ha: HandlerArgs) -> Optional[HandlerResult]:
    if not _INTERNAL_TOKEN:
        return HandlerResult(status=503, body={"error": "internal_api_disabled"}, media_type="application/json")
    if not _is_loopback(ha.client_ip):
        logger.log_warning(f"csc_internal: denied non-loopback client {ha.client_ip}")
        return HandlerResult(status=403, body={"error": "forbidden_non_loopback"}, media_type="application/json")
    # 헤더 이름 대소문자 상관없이 매칭
    headers_lower = {k.lower(): v for k, v in (ha.headers or {}).items()}
    supplied = headers_lower.get("x-csp-internal-token")
    if supplied != _INTERNAL_TOKEN:
        return HandlerResult(status=401, body={"error": "invalid_token"}, media_type="application/json")
    return None


def _parse_path(full_path: str) -> tuple:
    """/api/internal/config/{entity}/{id?} → (entity, id_or_None) 또는 (None, None)"""
    path = full_path.split("?", 1)[0]
    if not path.startswith(_INTERNAL_BASE):
        return (None, None)
    rest = path[len(_INTERNAL_BASE):].strip("/")
    if not rest:
        return ("", None)  # meta 경로용 placeholder 는 별도 처리
    parts = rest.split("/")
    entity = parts[0] if parts else None
    eid = parts[1] if len(parts) > 1 else None
    return (entity, eid)


async def handle_meta(ha: HandlerArgs, kwargs: dict) -> HandlerResult:
    err = _auth_check(ha)
    if err:
        return err
    cc = _cfg_cache.CONFIG_CACHE
    if cc is None:
        return HandlerResult(status=503, body={"error": "cache_not_ready"}, media_type="application/json")
    body = {
        "read_only": cc.is_read_only(),
        "entities": {e: cc.get_meta(e) for e in _cfg_cache.ENTITIES},
    }
    return HandlerResult(status=200, body=body, media_type="application/json")


async def handle_entity(ha: HandlerArgs, kwargs: dict) -> HandlerResult:
    err = _auth_check(ha)
    if err:
        return err
    cc = _cfg_cache.CONFIG_CACHE
    if cc is None:
        return HandlerResult(status=503, body={"error": "cache_not_ready"}, media_type="application/json")

    entity, eid = _parse_path(ha.full_path)
    if entity not in _cfg_cache.ENTITIES:
        return HandlerResult(status=404, body={"error": "unknown_entity", "entity": entity}, media_type="application/json")

    # ETag 비교 — If-None-Match 가 현재 etag 와 같으면 304
    meta = cc.get_meta(entity)
    current_etag = meta.get("etag", "")
    headers_lower = {k.lower(): v for k, v in (ha.headers or {}).items()}
    inm = headers_lower.get("if-none-match")

    if eid is None:
        if inm and inm == current_etag:
            return HandlerResult(status=304, body=None, headers={"ETag": current_etag})
        payload = {
            "etag": current_etag,
            "updated_at": meta.get("updated_at"),
            "source": meta.get("source"),
            "items": cc.get_all(entity),
        }
        return HandlerResult(status=200, body=payload, media_type="application/json",
                             headers={"ETag": current_etag})

    # 단건 조회
    try:
        eid_int = int(eid)
    except (TypeError, ValueError):
        return HandlerResult(status=400, body={"error": "invalid_id"}, media_type="application/json")
    row = cc.get_one(entity, eid_int)
    if row is None:
        return HandlerResult(status=404, body={"error": "not_found", "entity": entity, "id": eid_int},
                             media_type="application/json")
    return HandlerResult(status=200, body=row, media_type="application/json")


# Handler list — app.py 에서 CIMS_ADMIN_HANDLER_LIST 와 함께 등록됨
CSC_INTERNAL_HANDLER_LIST = (
    (_INTERNAL_BASE + "/meta", handle_meta,   {}),
    (_INTERNAL_BASE,           handle_entity, {}),
)
