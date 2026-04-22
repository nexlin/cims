"""
CSC Module Config API (Phase 1 검증용 — local dist 대상).

  GET  /api/v1/modules                         — 등록된 모듈 목록 (cims_package name 기준)
  GET  /api/v1/modules/{name}/config           — 해당 모듈의 현재 overlay 값 + template
  PUT  /api/v1/modules/{name}/config           — overlay 파일에 해당 모듈 키만 merge 저장

Overlay 파일:
  - 환경변수 CIMS_OVERLAY_FILE > 기본 <dist-root>/config.json (= <csc-root>/../config.json)
  - flat dot-path key → { "Setup.Sip.AuthRealm": "csp", "Setup.Roles.CSCF": true }
  - CSP/CSC 모두 실행 시 같은 파일을 overlay 로 merge (키 prefix 로 자연 분리)
  - Phase 1 로컬 전용. 실제 배포는 agent_deployment.config_json → install_path/config.json.

소유권 판단:
  - 각 패키지 config_template_json 의 sections[*].fields[*].key 집합이 그 모듈의 소유 키.
  - PUT 시 소유 키 외의 키는 400. 저장 시 overlay 파일의 타 모듈 키는 보존.

타입 검증 (template field.type 기반):
  - int       → int / str(int) 허용
  - bool      → bool / "true"/"false" 허용
  - enum      → options 안에 있어야 함
  - string/password/path → str
  - 값이 None 이면 해당 키 overlay 에서 제거 (default 로 fallback)
"""

from __future__ import annotations

import asyncio
import json
import os
import signal
import uuid as _uuid
from typing import Optional
from urllib.parse import urlparse, unquote
from pathlib import PurePath

import pymysql
import pymysql.cursors

from httpsrv.handler import HandlerArgs, HandlerResult
from util.log_util import Logger

from handlers.agents import (
    _COMPONENT_ROOT, _get_db, _safe_json, _actor,
    _collection_schema, _validate_record,
)

logger = Logger()

_MODULE_BASE = "/api/v1/modules"


def _overlay_path() -> str:
    env = os.environ.get("CIMS_OVERLAY_FILE")
    if env:
        return env
    return os.path.normpath(os.path.join(_COMPONENT_ROOT, "..", "config.json"))


def _path_tail(full_path: str, base: str) -> tuple:
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


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


def _load_latest_template(name: str, config: dict) -> Optional[dict]:
    """cims_package 에서 name 의 가장 최신 버전 config_template_json 반환.
    version 을 문자열 내림차순으로 단순 정렬 (id DESC 로 최근 업로드 우선)."""
    def _q():
        conn = _get_db(config)
        try:
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT version, config_template_json FROM cims_package "
                    "WHERE name=%s ORDER BY id DESC LIMIT 1",
                    (name,),
                )
                return cur.fetchone()
        finally:
            conn.close()
    row = _q()
    if not row:
        return None
    tmpl = _safe_json(row.get("config_template_json"))
    return {"version": row.get("version"), "template": tmpl or {}}


def _template_field_map(template: dict) -> dict:
    """template.sections[*].fields[*] 를 key → field 맵으로 펼침. collections 은 제외."""
    out: dict[str, dict] = {}
    for sec in (template.get("sections") or []):
        for fld in (sec.get("fields") or []):
            k = fld.get("key")
            if isinstance(k, str) and k:
                out[k] = fld
    return out


def _coerce_value(field: dict, raw):
    """template field type 에 맞게 값을 강제 변환. 실패 시 (None, 에러메시지) 반환."""
    t = (field.get("type") or "string").lower()
    if raw is None:
        return (None, None)
    if t == "int":
        if isinstance(raw, bool):
            return (None, "bool_not_allowed_for_int")
        try:
            v = int(raw)
        except (TypeError, ValueError):
            return (None, "not_int")
        mn = field.get("min"); mx = field.get("max")
        if isinstance(mn, (int, float)) and v < mn: return (None, f"below_min({mn})")
        if isinstance(mx, (int, float)) and v > mx: return (None, f"above_max({mx})")
        return (v, None)
    if t == "bool":
        if isinstance(raw, bool): return (raw, None)
        if isinstance(raw, str):
            if raw.lower() == "true":  return (True, None)
            if raw.lower() == "false": return (False, None)
        if isinstance(raw, (int, float)) and not isinstance(raw, bool):
            return (bool(raw), None)
        return (None, "not_bool")
    if t == "enum":
        opts = field.get("options") or []
        if not isinstance(raw, str) or raw not in opts:
            return (None, f"not_in_enum({opts})")
        return (raw, None)
    # string / password / path / 기타 → 문자열
    if not isinstance(raw, str):
        return (None, "not_string")
    return (raw, None)


def _read_overlay(path: str) -> dict:
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict):
            return data
    except FileNotFoundError:
        return {}
    except Exception as e:
        logger.log_error(f"overlay read failed: {path}: {e}")
    return {}


def _write_overlay(path: str, data: dict) -> None:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


async def handle_modules(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    tail = _path_tail(handler_args.full_path, _MODULE_BASE)
    method = handler_args.method.upper()

    if not tail:
        if method == "GET":
            return await _list_modules(config)
        return HandlerResult(status=405, body={"error": "method_not_allowed"},
                             media_type="application/json")

    # /api/v1/modules/{name}/config
    if len(tail) == 2 and tail[1] == "config":
        name = tail[0]
        if method == "GET":
            return await _get_module_config(name, config)
        if method == "PUT":
            return await _put_module_config(handler_args, name, config)

    # /api/v1/modules/{name}/collection/{coll_key}
    if len(tail) == 3 and tail[1] == "collection":
        name = tail[0]
        coll_key = tail[2]
        if method == "GET":
            return await _get_module_collection(name, coll_key, config)
        if method == "PUT":
            return await _put_module_collection(handler_args, name, coll_key, config)

    return HandlerResult(status=404, body={"error": "not_found"},
                         media_type="application/json")


async def _list_modules(config: dict) -> HandlerResult:
    """등록된 패키지의 모듈명 + 최신 버전 목록."""
    def _q():
        conn = _get_db(config)
        try:
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT name, MAX(id) AS max_id FROM cims_package GROUP BY name"
                )
                by_name = {r["name"]: r["max_id"] for r in cur.fetchall()}
                if not by_name:
                    return []
                ids = tuple(by_name.values())
                placeholders = ",".join(["%s"] * len(ids))
                cur.execute(
                    f"SELECT id, name, version FROM cims_package WHERE id IN ({placeholders})",
                    ids,
                )
                return cur.fetchall()
        finally:
            conn.close()
    rows = await asyncio.to_thread(_q)
    items = [
        {"name": r["name"], "latest_version": r["version"], "package_id": r["id"]}
        for r in rows
    ]
    items.sort(key=lambda x: x["name"])
    return HandlerResult(status=200, body={"items": items, "overlay": _overlay_path()},
                         media_type="application/json")


async def _get_module_config(name: str, config: dict) -> HandlerResult:
    t = await asyncio.to_thread(_load_latest_template, name, config)
    if not t:
        return HandlerResult(status=404, body={"error": "module_not_found",
                                               "module": name},
                             media_type="application/json")
    template = t["template"] or {}
    field_map = _template_field_map(template)
    overlay = await asyncio.to_thread(_read_overlay, _overlay_path())
    current = {k: v for k, v in overlay.items() if k in field_map}
    return HandlerResult(
        status=200,
        body={
            "module": name,
            "version": t["version"],
            "template": template,
            "current": current,
            "overlay_path": _overlay_path(),
            "owned_keys": sorted(field_map.keys()),
        },
        media_type="application/json",
    )


async def _put_module_config(handler_args: HandlerArgs, name: str, config: dict) -> HandlerResult:
    body = _parse_body(handler_args)
    values = body.get("values")
    if not isinstance(values, dict):
        return HandlerResult(status=400, body={"error": "values_required_object"},
                             media_type="application/json")

    t = await asyncio.to_thread(_load_latest_template, name, config)
    if not t:
        return HandlerResult(status=404, body={"error": "module_not_found",
                                               "module": name},
                             media_type="application/json")
    field_map = _template_field_map(t["template"] or {})

    # 1) validation + coerce
    coerced: dict = {}
    to_remove: list = []
    errors: list = []
    for k, raw in values.items():
        if k not in field_map:
            errors.append({"key": k, "error": "not_owned_by_module"})
            continue
        if raw is None:
            to_remove.append(k)
            continue
        v, err = _coerce_value(field_map[k], raw)
        if err:
            errors.append({"key": k, "error": err})
            continue
        coerced[k] = v
    if errors:
        return HandlerResult(status=400,
            body={"error": "validation_failed", "details": errors},
            media_type="application/json")

    # 2) merge + persist
    path = _overlay_path()
    def _merge_and_write():
        overlay = _read_overlay(path)
        for k in to_remove:
            overlay.pop(k, None)
        overlay.update(coerced)
        _write_overlay(path, overlay)
        return overlay

    new_overlay = await asyncio.to_thread(_merge_and_write)
    current = {k: v for k, v in new_overlay.items() if k in field_map}

    actor = _actor(handler_args)
    applied_count = len(coerced)
    removed_count = len(to_remove)
    logger.log_info(
        f"module_config PUT: module={name} actor={actor} "
        f"applied={applied_count} removed={removed_count} overlay={path}"
    )

    # 3) restart 필요 여부 안내 (applied/removed 키 중 하나라도 restart=true 면 true)
    need_restart = any(
        (field_map[k].get("restart") is True)
        for k in list(coerced.keys()) + to_remove
        if k in field_map
    )

    return HandlerResult(status=200, body={
        "ok": True,
        "module": name,
        "applied": applied_count,
        "removed": removed_count,
        "current": current,
        "overlay_path": path,
        "restart_required": need_restart,
    }, media_type="application/json")


# ──────────────── Collection (jsonl) helpers ────────────────

def _dist_root() -> str:
    """Phase 1 로컬 모듈의 install_path 공용 루트 (= overlay 파일 디렉토리).

    우선순위:
      1) CIMS_DIST_DIR 환경변수
      2) overlay 파일 경로의 디렉토리 (기본 <csc-root>/../)
    """
    env = os.environ.get("CIMS_DIST_DIR")
    if env:
        return env
    return os.path.normpath(os.path.join(_COMPONENT_ROOT, ".."))


def _collection_file_path(module_name: str, storage_file: Optional[str]) -> str:
    """collection storage.file 을 해당 모듈의 install_path 기준으로 해석.

    Phase 1 로컬에서 install_path = <dist-root> (CSP 의 3-up 추정과 일치).
    template 의 storage.file 예: "config/listeners.jsonl"
      → <dist-root>/config/listeners.jsonl

    module_name 은 현재 경로 분기에 쓰지 않음 (모든 모듈이 <dist-root>/config 공용).
    collection key 가 모듈간에 유일하므로 충돌 없음. (현재는 listeners/trunks/
    routes/acl 모두 csp 전용)
    """
    rel = storage_file or "config"
    _ = module_name  # reserved for future per-module dir policy
    return os.path.join(_dist_root(), rel)


def _module_pid_file(module_name: str) -> str:
    """cims.sh 가 기록한 PID 파일 경로. <dist-root>/run/{module}.pid"""
    return os.path.join(_dist_root(), "run", f"{module_name}.pid")


def _read_jsonl(path: str) -> list:
    """한 줄 = 한 레코드. 파싱 실패한 줄은 skip."""
    out = []
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line: continue
                try:
                    obj = json.loads(line)
                    if isinstance(obj, dict): out.append(obj)
                except Exception:
                    continue
    except FileNotFoundError:
        return []
    return out


def _write_jsonl(path: str, records: list) -> None:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        for r in records:
            f.write(json.dumps(r, ensure_ascii=False))
            f.write("\n")
    os.replace(tmp, path)


def _signal_module(module_name: str) -> list:
    """모듈 PID 파일을 읽어 SIGUSR1 을 전송. 성공한 PID 목록 반환."""
    pid_file = _module_pid_file(module_name)
    signaled = []
    try:
        with open(pid_file, "r") as f:
            raw = f.read().strip()
        if not raw:
            return []
        pid = int(raw)
        if pid <= 1:
            return []
        os.kill(pid, signal.SIGUSR1)
        signaled.append(pid)
    except FileNotFoundError:
        logger.log_info(f"signal skip: pid file not found ({pid_file})")
    except ProcessLookupError:
        logger.log_info(f"signal skip: process not running (pid={pid})")
    except Exception as e:
        logger.log_error(f"signal failed for {module_name}: {e}")
    return signaled


def _load_latest_package_template(name: str, config: dict) -> Optional[dict]:
    """cims_package 최신 버전의 config_template_json (dict) 반환. 없으면 None."""
    def _q():
        conn = _get_db(config)
        try:
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT config_template_json FROM cims_package "
                    "WHERE name=%s ORDER BY id DESC LIMIT 1",
                    (name,),
                )
                return cur.fetchone()
        finally:
            conn.close()
    row = _q()
    if not row:
        return None
    return row.get("config_template_json")


async def _get_module_collection(name: str, coll_key: str, config: dict) -> HandlerResult:
    tmpl_raw = await asyncio.to_thread(_load_latest_package_template, name, config)
    if tmpl_raw is None:
        return HandlerResult(status=404, body={"error": "module_not_found",
                                               "module": name},
                             media_type="application/json")
    schema, coll = _collection_schema(tmpl_raw, coll_key)
    if schema is None:
        return HandlerResult(status=404,
            body={"error": "collection_not_in_template", "collection": coll_key,
                  "module": name},
            media_type="application/json")
    storage_file = (coll.get("storage") or {}).get("file")
    path = _collection_file_path(name, storage_file)
    records = await asyncio.to_thread(_read_jsonl, path)
    return HandlerResult(status=200, body={
        "records": records,
        "schema":  schema,
        "file":    path,
    }, media_type="application/json")


async def _put_module_collection(handler_args: HandlerArgs, name: str,
                                 coll_key: str, config: dict) -> HandlerResult:
    tmpl_raw = await asyncio.to_thread(_load_latest_package_template, name, config)
    if tmpl_raw is None:
        return HandlerResult(status=404, body={"error": "module_not_found",
                                               "module": name},
                             media_type="application/json")
    schema, coll = _collection_schema(tmpl_raw, coll_key)
    if schema is None:
        return HandlerResult(status=404,
            body={"error": "collection_not_in_template", "collection": coll_key,
                  "module": name},
            media_type="application/json")

    body = _parse_body(handler_args)
    records = body.get("records")
    if not isinstance(records, list):
        return HandlerResult(status=400, body={"error": "records array required"},
                             media_type="application/json")

    # 1) validation + auto id (uuid)
    id_field = schema.get("id_field") or "id"
    id_type  = schema.get("id_type") or "uuid"
    all_errors = []
    for i, r in enumerate(records):
        if not isinstance(r, dict):
            all_errors.append({"index": i, "errors": ["not_object"]})
            continue
        if id_type == "uuid" and not r.get(id_field):
            r[id_field] = _uuid.uuid4().hex[:16]
        errs = _validate_record(schema, r)
        if errs:
            all_errors.append({"index": i, "errors": errs})
    if all_errors:
        return HandlerResult(status=400,
            body={"error": "validation_failed", "details": all_errors},
            media_type="application/json")

    # 2) write jsonl
    storage_file = (coll.get("storage") or {}).get("file")
    path = _collection_file_path(name, storage_file)
    await asyncio.to_thread(_write_jsonl, path, records)

    # 3) SIGUSR1 (signal=true 기본)
    do_signal = bool(body.get("signal", True))
    signaled: list = []
    if do_signal:
        signaled = await asyncio.to_thread(_signal_module, name)

    actor = _actor(handler_args)
    logger.log_info(
        f"module_collection PUT: module={name} coll={coll_key} "
        f"actor={actor} count={len(records)} signaled={signaled} file={path}"
    )

    return HandlerResult(status=200, body={
        "ok":       True,
        "count":    len(records),
        "signaled": signaled,
        "file":     path,
    }, media_type="application/json")


CIMS_MODULES_HANDLER_LIST = (
    (_MODULE_BASE, handle_modules, {}),
)
