"""
CSC 설정 캐시 계층 (CSP 런타임 설정)
  - DB(primary) → 메모리(fast path) → 파일 스냅샷(DB 장애 대비) 3층 구조
  - write-through: admin API 의 CUD 는 DB 트랜잭션 → 메모리 갱신 → 파일 원자 쓰기 → CSP 알림
  - DB 부팅 실패 시 파일 스냅샷만 읽어 read-only 모드로 작동 (조회만 허용)

Entities:
  listener  — csp_listener
  trunk     — sip_trunk
  route     — routing_rule (+ routing_rule_match, routing_rule_transform)
  access    — routing_access_list

Files (atomic write: .tmp → fsync → rename):
  {cache_dir}/listeners.json
  {cache_dir}/trunks.json
  {cache_dir}/routes.json
  {cache_dir}/access.json
  {cache_dir}/_meta.json   — { entity: {etag, updated_at, source} }

Module-level singleton: CONFIG_CACHE (lazy init via init_config_cache(config)).
"""

from __future__ import annotations

import json
import os
import tempfile
import threading
import time
import hashlib
from typing import Any, Dict, List, Optional

from util.log_util import Logger
from services import file_store

logger = Logger()

ENTITIES = ("listener", "trunk", "route", "access", "service")

_FILE_BY_ENTITY = {
    "listener": "listeners.json",
    "trunk":    "trunks.json",
    "route":    "routes.json",
    "access":   "access.json",
    "service":  "services.json",
}


# ──────────────────────────────────────────────────────────────
#  atomic write helper
# ──────────────────────────────────────────────────────────────

def _atomic_write_json(path: str, data: Any) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".tmp.", dir=os.path.dirname(path))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except Exception:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def _compute_etag(payload: Any) -> str:
    raw = json.dumps(payload, sort_keys=True, ensure_ascii=False).encode("utf-8")
    return hashlib.md5(raw).hexdigest()[:16]


# ──────────────────────────────────────────────────────────────
#  DB DAO
# ──────────────────────────────────────────────────────────────

_DOMAIN_BY_ENTITY = {
    "listener": "csp_listener",
    "trunk":    "sip_trunk",
    "route":    "routing_rule",
    "access":   "routing_access_list",
    "service":  "sip_service",
}


def _row_listener(r: dict) -> dict:
    return {
        "id": r.get("id"),
        "name": r.get("name"),
        "enabled": bool(r.get("enabled")),
        "bind_ip": r.get("bind_ip"),
        "bind_port": r.get("bind_port"),
        "protocol": r.get("protocol"),
        "domain": r.get("domain") or "",
        "service": r.get("service"),
        "tls": {
            "cert": r.get("tls_cert_path"),
            "key":  r.get("tls_key_path"),
            "ca":   r.get("tls_ca_path"),
            "verify_peer": bool(r.get("tls_verify_peer")),
        } if r.get("protocol") in ("TLS", "WSS") else None,
        "max_connections": r.get("max_connections", 0),
        "thread_count": r.get("thread_count", 2),
        "note": r.get("note"),
        "etag": r.get("etag") or "",
    }


def _row_trunk(r: dict) -> dict:
    return {
        "id": r.get("id"),
        "name": r.get("name"),
        "enabled": bool(r.get("enabled")),
        "service_id": r.get("service_id"),
        "failover_priority": r.get("failover_priority", 100),
        "remote": {
            "ip": r.get("remote_ip"),
            "port": r.get("remote_port"),
            "domain": r.get("remote_domain") or "",
            "protocol": r.get("protocol"),
        },
        "outbound_proxy": (
            {"ip": r["outbound_proxy_ip"], "port": r["outbound_proxy_port"]}
            if r.get("outbound_proxy_ip") else None
        ),
        "auth": {
            "register": bool(r.get("register_to_remote")),
            "user":     r.get("auth_user"),
            "password": r.get("auth_password"),
            "realm":    r.get("auth_realm"),
            "expires":  r.get("register_expires", 3600),
        } if r.get("register_to_remote") else None,
        "health": {
            "options_ping_sec": r.get("options_ping_sec", 60),
            "dead_threshold":   r.get("options_dead_threshold", 3),
        },
        "transport": {
            "srv_lookup":   bool(r.get("srv_lookup")),
            "dns_fallback": bool(r.get("dns_fallback")),
        },
        "limits": {
            "max_concurrent_calls": r.get("max_concurrent_calls", 0),
            "cps_limit":            r.get("cps_limit", 0),
        },
        "note": r.get("note"),
        "etag": r.get("etag") or "",
    }


def _row_rule(r: dict, matches: List[dict], transforms: List[dict]) -> dict:
    tj = r.get("target_json")
    if isinstance(tj, str):
        try: tj = json.loads(tj)
        except Exception: tj = None
    return {
        "id": r.get("id"),
        "name": r.get("name"),
        "enabled": bool(r.get("enabled")),
        "priority": r.get("priority"),
        "description": r.get("description"),
        "match": [
            {
                "field":  m.get("field"),
                "op":     m.get("op"),
                "value":  m.get("value"),
                "invert": bool(m.get("invert")),
                "seq":    m.get("seq", 0),
            } for m in sorted(matches, key=lambda x: x.get("seq", 0))
        ],
        "transform": [
            {
                "action": t.get("action"),
                "target": t.get("target"),
                "value":  t.get("value"),
                "seq":    t.get("seq", 0),
            } for t in sorted(transforms, key=lambda x: x.get("seq", 0))
        ],
        "target": {
            "mode":        r.get("target_mode"),
            "trunk_id":    r.get("target_trunk_id"),
            "json":        tj,
        },
        "fail": {
            "action":       r.get("fail_action"),
            "code":         r.get("fail_code"),
            "reason":       r.get("fail_reason"),
            "fallback":     r.get("fallback_trunk_id"),
            "timeout_ms":   r.get("timeout_ms"),
            "retry_count":  r.get("retry_count"),
        },
        "etag": r.get("etag") or "",
    }


def _row_service(r: dict, listener_ids: list) -> dict:
    return {
        "id": r.get("id"),
        "name": r.get("name"),
        "kind": r.get("kind"),
        "domain": r.get("domain"),
        "auth_realm": r.get("auth_realm"),
        "inbound_policy": r.get("inbound_policy"),
        "priority": r.get("priority"),
        "enabled": bool(r.get("enabled")),
        "note": r.get("note"),
        "listeners": listener_ids,
        "etag": r.get("etag") or "",
    }


def _row_access(r: dict) -> dict:
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
    }


# ──────────────────────────────────────────────────────────────
#  Config cache (in-memory + file)
# ──────────────────────────────────────────────────────────────

class CscConfigCache:
    """CSC 설정 캐시 — 메모리/파일 3층 구조."""

    def __init__(self, db_cfg: dict, cache_dir: str, runtime_config: dict = None):
        self._db_cfg = db_cfg
        self._cache_dir = cache_dir
        self._runtime_config = runtime_config or {}
        self._lock = threading.RLock()
        self._data: Dict[str, List[dict]] = {e: [] for e in ENTITIES}
        self._meta: Dict[str, dict] = {e: {} for e in ENTITIES}
        self._read_only = False
        os.makedirs(cache_dir, exist_ok=True)

    # ── public API ────────────────────────────────────────────

    def startup(self) -> None:
        """file_store 에서 로드 → 파일 스냅샷 갱신. file_store 실패 시 파일 캐시 fallback."""
        try:
            self._load_all_from_store()
            self._flush_all_to_files()
            self._read_only = False
            logger.log_info("CscConfigCache: loaded from file_store, snapshots refreshed")
        except Exception as e:
            logger.log_warning(f"CscConfigCache: file_store load failed ({e}), fallback to cache files")
            try:
                self._load_all_from_files()
                self._read_only = False  # file_store 가 파일 기반이므로 항상 read-write
            except Exception as e2:
                logger.log_error(f"CscConfigCache: file cache load also failed: {e2}")
                self._data = {e: [] for e in ENTITIES}
                self._meta = {e: {"etag": "", "updated_at": time.time(), "source": "empty"} for e in ENTITIES}
                self._read_only = False

    def is_read_only(self) -> bool:
        return self._read_only

    def get_all(self, entity: str) -> List[dict]:
        with self._lock:
            return list(self._data.get(entity, []))

    def get_one(self, entity: str, entity_id: int) -> Optional[dict]:
        with self._lock:
            for row in self._data.get(entity, []):
                if int(row.get("id", -1)) == int(entity_id):
                    return dict(row)
        return None

    def get_meta(self, entity: str) -> dict:
        with self._lock:
            return dict(self._meta.get(entity, {}))

    def refresh_entity(self, entity: str) -> None:
        """특정 entity 만 file_store 에서 재조회 + 파일/메타 갱신. write-through 경로에서 사용."""
        if entity not in ENTITIES:
            raise ValueError(f"unknown entity: {entity}")
        with self._lock:
            self._data[entity] = self._load_entity_from_store(entity)
            self._flush_entity_to_file(entity)

    # ── internal file_store load ──────────────────────────────

    def _load_all_from_store(self) -> None:
        for e in ENTITIES:
            self._data[e] = self._load_entity_from_store(e)

    def _load_entity_from_store(self, entity: str) -> List[dict]:
        domain = _DOMAIN_BY_ENTITY.get(entity)
        if not domain:
            return []
        d = file_store.domain_dir(self._runtime_config, domain)
        rows = file_store.load_all(d)
        if entity == "listener":
            rows.sort(key=lambda r: r.get("id", 0))
            return [_row_listener(r) for r in rows]
        if entity == "trunk":
            rows.sort(key=lambda r: r.get("id", 0))
            return [_row_trunk(r) for r in rows]
        if entity == "route":
            rows.sort(key=lambda r: (r.get("priority") or 0, r.get("id") or 0))
            # match/transform 는 이미 임베드
            return [_row_rule(r, r.get("match") or [], r.get("transform") or []) for r in rows]
        if entity == "access":
            rows.sort(key=lambda r: (r.get("priority") or 0, r.get("id") or 0))
            return [_row_access(r) for r in rows]
        if entity == "service":
            rows.sort(key=lambda r: (r.get("priority") or 0, r.get("id") or 0))
            return [_row_service(r, r.get("listeners") or []) for r in rows]
        return []

    # ── internal file I/O ─────────────────────────────────────

    def _flush_all_to_files(self) -> None:
        for e in ENTITIES:
            self._flush_entity_to_file(e)
        _atomic_write_json(os.path.join(self._cache_dir, "_meta.json"), self._meta)

    def _flush_entity_to_file(self, entity: str) -> None:
        payload = self._data.get(entity, [])
        etag = _compute_etag(payload)
        self._meta[entity] = {
            "etag": etag,
            "updated_at": time.time(),
            "source": "db",
            "count": len(payload),
        }
        path = os.path.join(self._cache_dir, _FILE_BY_ENTITY[entity])
        _atomic_write_json(path, {
            "etag": etag,
            "updated_at": self._meta[entity]["updated_at"],
            "items": payload,
        })
        # _meta.json 은 전체 entity 업데이트 후 한 번에 쓰는 것이 일반적이지만
        # 여기서도 갱신해 둔다 (단일 entity 변경 시)
        _atomic_write_json(os.path.join(self._cache_dir, "_meta.json"), self._meta)

    def _load_all_from_files(self) -> None:
        for entity, fname in _FILE_BY_ENTITY.items():
            path = os.path.join(self._cache_dir, fname)
            if not os.path.exists(path):
                self._data[entity] = []
                self._meta[entity] = {"etag": "", "updated_at": 0, "source": "missing", "count": 0}
                continue
            with open(path, "r", encoding="utf-8") as f:
                doc = json.load(f)
            items = doc.get("items", [])
            self._data[entity] = items
            self._meta[entity] = {
                "etag": doc.get("etag", _compute_etag(items)),
                "updated_at": doc.get("updated_at", 0),
                "source": "file",
                "count": len(items),
            }


# ──────────────────────────────────────────────────────────────
#  Module singleton
# ──────────────────────────────────────────────────────────────

CONFIG_CACHE: Optional[CscConfigCache] = None


def init_config_cache(config: dict) -> CscConfigCache:
    """app.py 부팅 시 1회 호출. 이후 CONFIG_CACHE 로 접근."""
    global CONFIG_CACHE
    db_cfg = config.get("CimsDatabase", {})
    cache_dir = config.get("ConfigCacheDir") or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "cache"
    )
    cache_dir = os.path.abspath(cache_dir)
    cc = CscConfigCache(db_cfg, cache_dir, runtime_config=config)
    cc.startup()
    CONFIG_CACHE = cc
    return cc
