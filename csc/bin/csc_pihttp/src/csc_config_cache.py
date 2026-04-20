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

import pymysql
import pymysql.cursors

from util.log_util import Logger

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

def _connect(db_cfg: dict):
    return pymysql.connect(
        host=db_cfg.get("Host", "127.0.0.1"),
        port=int(db_cfg.get("Port", 3306)),
        user=db_cfg.get("User", "root"),
        password=db_cfg.get("Password", ""),
        database=db_cfg.get("Db", "cims"),
        charset="utf8mb4",
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=True,
        connect_timeout=3,
    )


def _row_listener(r: dict) -> dict:
    return {
        "id": r["id"],
        "name": r["name"],
        "enabled": bool(r["enabled"]),
        "bind_ip": r["bind_ip"],
        "bind_port": r["bind_port"],
        "protocol": r["protocol"],
        "domain": r["domain"] or "",
        "service": r["service"],
        "tls": {
            "cert": r.get("tls_cert_path"),
            "key":  r.get("tls_key_path"),
            "ca":   r.get("tls_ca_path"),
            "verify_peer": bool(r.get("tls_verify_peer")),
        } if r["protocol"] in ("TLS", "WSS") else None,
        "max_connections": r.get("max_connections", 0),
        "thread_count": r.get("thread_count", 2),
        "note": r.get("note"),
        "etag": r.get("etag") or "",
    }


def _row_trunk(r: dict) -> dict:
    return {
        "id": r["id"],
        "name": r["name"],
        "enabled": bool(r["enabled"]),
        "service_id": r.get("service_id"),
        "failover_priority": r.get("failover_priority", 100),
        "remote": {
            "ip": r["remote_ip"],
            "port": r["remote_port"],
            "domain": r["remote_domain"] or "",
            "protocol": r["protocol"],
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
    return {
        "id": r["id"],
        "name": r["name"],
        "enabled": bool(r["enabled"]),
        "priority": r["priority"],
        "description": r.get("description"),
        "match": [
            {
                "field":  m["field"],
                "op":     m["op"],
                "value":  m["value"],
                "invert": bool(m.get("invert")),
                "seq":    m.get("seq", 0),
            } for m in sorted(matches, key=lambda x: x.get("seq", 0))
        ],
        "transform": [
            {
                "action": t["action"],
                "target": t.get("target"),
                "value":  t.get("value"),
                "seq":    t.get("seq", 0),
            } for t in sorted(transforms, key=lambda x: x.get("seq", 0))
        ],
        "target": {
            "mode":        r["target_mode"],
            "trunk_id":    r.get("target_trunk_id"),
            "json":        json.loads(r["target_json"]) if r.get("target_json") else None,
        },
        "fail": {
            "action":       r["fail_action"],
            "code":         r["fail_code"],
            "reason":       r["fail_reason"],
            "fallback":     r.get("fallback_trunk_id"),
            "timeout_ms":   r["timeout_ms"],
            "retry_count":  r["retry_count"],
        },
        "etag": r.get("etag") or "",
    }


def _row_service(r: dict, listener_ids: list) -> dict:
    return {
        "id": r["id"],
        "name": r["name"],
        "kind": r["kind"],
        "domain": r["domain"],
        "auth_realm": r.get("auth_realm"),
        "inbound_policy": r["inbound_policy"],
        "priority": r["priority"],
        "enabled": bool(r["enabled"]),
        "note": r.get("note"),
        "listeners": listener_ids,
        "etag": r.get("etag") or "",
    }


def _row_access(r: dict) -> dict:
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
    }


# ──────────────────────────────────────────────────────────────
#  Config cache (in-memory + file)
# ──────────────────────────────────────────────────────────────

class CscConfigCache:
    """CSC 설정 캐시 — 메모리/파일 3층 구조."""

    def __init__(self, db_cfg: dict, cache_dir: str):
        self._db_cfg = db_cfg
        self._cache_dir = cache_dir
        self._lock = threading.RLock()
        self._data: Dict[str, List[dict]] = {e: [] for e in ENTITIES}
        self._meta: Dict[str, dict] = {e: {} for e in ENTITIES}
        self._read_only = False  # DB 장애 시 True
        os.makedirs(cache_dir, exist_ok=True)

    # ── public API ────────────────────────────────────────────

    def startup(self) -> None:
        """DB 로드 → 파일 스냅샷 갱신. DB 실패 시 파일만 읽고 read-only 모드."""
        try:
            self._load_all_from_db()
            self._flush_all_to_files()
            self._read_only = False
            logger.log_info("CscConfigCache: loaded from DB, snapshots refreshed")
        except Exception as e:
            logger.log_warning(f"CscConfigCache: DB load failed ({e}), fallback to file cache (read-only)")
            try:
                self._load_all_from_files()
                self._read_only = True
            except Exception as e2:
                logger.log_error(f"CscConfigCache: file cache load also failed: {e2}")
                # 모든게 실패해도 빈 상태로 살아 있게 함
                self._data = {e: [] for e in ENTITIES}
                self._meta = {e: {"etag": "", "updated_at": time.time(), "source": "empty"} for e in ENTITIES}
                self._read_only = True

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
        """특정 entity 만 DB 에서 재조회 + 파일/메타 갱신. write-through 경로에서 사용."""
        if entity not in ENTITIES:
            raise ValueError(f"unknown entity: {entity}")
        if self._read_only:
            raise RuntimeError("read-only mode (DB unavailable)")
        with self._lock:
            self._data[entity] = self._load_entity_from_db(entity)
            self._flush_entity_to_file(entity)

    # ── internal DB load ──────────────────────────────────────

    def _load_all_from_db(self) -> None:
        for e in ENTITIES:
            self._data[e] = self._load_entity_from_db(e)

    def _load_entity_from_db(self, entity: str) -> List[dict]:
        conn = _connect(self._db_cfg)
        try:
            with conn.cursor() as cur:
                if entity == "listener":
                    cur.execute("SELECT * FROM csp_listener ORDER BY id")
                    rows = [_row_listener(r) for r in cur.fetchall()]
                elif entity == "trunk":
                    cur.execute("SELECT * FROM sip_trunk ORDER BY id")
                    rows = [_row_trunk(r) for r in cur.fetchall()]
                elif entity == "route":
                    cur.execute("SELECT * FROM routing_rule ORDER BY priority, id")
                    rule_rows = cur.fetchall()
                    rows = []
                    for rr in rule_rows:
                        cur.execute("SELECT * FROM routing_rule_match WHERE rule_id=%s ORDER BY seq", (rr["id"],))
                        matches = cur.fetchall()
                        cur.execute("SELECT * FROM routing_rule_transform WHERE rule_id=%s ORDER BY seq", (rr["id"],))
                        transforms = cur.fetchall()
                        rows.append(_row_rule(rr, matches, transforms))
                elif entity == "access":
                    cur.execute("SELECT * FROM routing_access_list ORDER BY priority, id")
                    rows = [_row_access(r) for r in cur.fetchall()]
                elif entity == "service":
                    cur.execute("SELECT * FROM sip_service ORDER BY priority, id")
                    svc_rows = cur.fetchall()
                    rows = []
                    for sr in svc_rows:
                        cur.execute(
                            "SELECT listener_id FROM sip_service_listener WHERE service_id=%s",
                            (sr["id"],))
                        listener_ids = [row["listener_id"] for row in cur.fetchall()]
                        rows.append(_row_service(sr, listener_ids))
                else:
                    rows = []
            return rows
        finally:
            conn.close()

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
    cc = CscConfigCache(db_cfg, cache_dir)
    cc.startup()
    CONFIG_CACHE = cc
    return cc
