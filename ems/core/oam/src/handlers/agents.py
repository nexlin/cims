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
import sys
from datetime import datetime, timedelta
from typing import Optional
from urllib.parse import urlparse, unquote
from pathlib import PurePath


from httpsrv.handler import HandlerArgs, HandlerResult
from util.log_util import Logger
from services import file_store

logger = Logger()

# ──────────────────────────────────────────────────────────────
#  cims_package — 파일 기반 (file_store), 도메인 'packages'
#  파일 키 = "<name>__<version>"  (자연키), 'id' 필드도 같이 보관.
# ──────────────────────────────────────────────────────────────

_PKG_DOMAIN = 'packages'
_AGENT_DOMAIN = 'agents'
_DEPLOY_DOMAIN = 'deployments'
_JOB_DOMAIN = 'jobs'
_METRIC_DOMAIN = 'metrics'


def _pkg_dir(config):
    return file_store.domain_dir(config, _PKG_DOMAIN)


def _pkg_key(name: str, version: str) -> str:
    return f"{name}__{version}"


def _pkg_load(config, pid: int = None, name: str = None, version: str = None):
    """id 또는 (name, version) 으로 1건 조회."""
    d = _pkg_dir(config)
    if name and version:
        return file_store.load(d, _pkg_key(name, version))
    if pid is not None:
        return file_store.by_id(d, pid)
    return None


def _coerce_list_fields(template: dict, values: dict) -> dict:
    """config_template 의 string_list/ref_list 필드 값이 콤마 문자열이면 배열로 정규화.
    프론트 위젯 누락·raw API 우회에도 config.json 에 배열로 저장되게 하는 백엔드 방어."""
    if not isinstance(values, dict):
        return values
    list_keys = set()
    for sec in (template or {}).get("sections", []):
        for fld in sec.get("fields", []):
            if (fld.get("type") or "").lower() in ("string_list", "ref_list") and fld.get("key"):
                list_keys.add(fld["key"])
    if not list_keys:
        return values
    out = dict(values)
    for k in list_keys:
        v = out.get(k)
        if isinstance(v, str):
            out[k] = [s.strip() for s in v.split(",") if s.strip()]
    return out


def _template_defaults(template: dict) -> dict:
    """config_template 전 필드의 default 를 flat(dot-key) dict 로.
    빈 default(None/''/[])는 '미설정' 시맨틱(예: ServiceLogging.Dir 비움=상속) 보존을
    위해 제외한다."""
    out: dict = {}
    for sec in (template or {}).get("sections", []):
        for fld in sec.get("fields", []):
            k, d = fld.get("key"), fld.get("default")
            if k and d is not None and d != "" and d != []:
                out[k] = d
    return out


def _materialize_deploy_config(config, pkg_file, overlay):
    """배포 config 실체화 — agent 가 쓰는 config.json 이 항상 완전한 유효설정이 되도록
    config_template default 를 base 로 깔고 deployment overlay(사용자 변경분)를 병합.
    deployment 레코드는 sparse overlay 그대로 유지(사용자 의도 SoT) — template default
    변경은 다음 job 디스패치에서 자동 추종된다.

    게이트웨이 서비스 모듈(meta.gateway.routes 보유 — oam-svc)에는 base 소유 공유값도
    주입한다: oam-svc 의 base oam.json fallback 상속 폐지의 대체 경로.
      - CimsAuth.JwtSecret / CimsRuntimeDir / Mgmt.Cidr — base 가 SoT, overlay 보다 우선
        (시크릿 회전·runtime 이동 시 base 현재값 추종).
      - ServiceLogging.Dir — template 소유(콘솔 편집 가능), 비어있을 때만 base 값 주입."""
    overlay = overlay if isinstance(overlay, dict) else {}
    tmpl = (pkg_file or {}).get("config_template") if isinstance(pkg_file, dict) else None
    if isinstance(tmpl, dict):
        overlay = _coerce_list_fields(tmpl, overlay)
        out = _template_defaults(tmpl)
    else:
        out = {}
    for k, v in overlay.items():
        if v is None or v == "":   # 빈 overlay 값은 default/주입값을 지우지 않음 ([] 는 유효값)
            continue
        out[k] = v
    pkg_meta = (pkg_file or {}).get("meta") if isinstance(pkg_file, dict) else None
    if isinstance(pkg_meta, dict) and (pkg_meta.get("gateway") or {}).get("routes"):
        secret = (config.get("CimsAuth") or {}).get("JwtSecret")
        if secret:
            out["CimsAuth.JwtSecret"] = secret
        if config.get("CimsRuntimeDir"):
            out["CimsRuntimeDir"] = config["CimsRuntimeDir"]
        if (config.get("Mgmt") or {}).get("Cidr"):
            out["Mgmt.Cidr"] = config["Mgmt"]["Cidr"]
        if not out.get("ServiceLogging.Dir"):
            sld = (config.get("ServiceLogging") or {}).get("Dir")
            if sld:
                out["ServiceLogging.Dir"] = sld
    return out


def effective_server_port(config, pkg_file, overlay):
    """배포의 실효 admin 포트 — 게이트웨이 self-register 와 HA 헬스포트 유도가
    공유하는 단일 해석. 해석이 갈라지면 프록시와 헬스체크가 서로 다른 포트를 보는
    반쪽 장애가 되므로 여기 한 곳만 고친다.
      materialize(template default + overlay) 의 Server.Port — flat dot-key
      (배포 config.json 표준 형태) 우선, nested 수용 → pkg meta.gateway.default_port.
    실패/범위 밖은 None."""
    def _valid(p):
        try:
            p = int(p)
        except (TypeError, ValueError):
            return None
        return p if 0 < p < 65536 else None

    try:
        eff = _materialize_deploy_config(config, pkg_file, overlay)
        port = _valid(eff.get("Server.Port"))
        if port is None and isinstance(eff.get("Server"), dict):
            port = _valid(eff["Server"].get("Port"))
        if port:
            return port
    except Exception:
        pass
    meta = (pkg_file or {}).get("meta") if isinstance(pkg_file, dict) else None
    return _valid(((meta or {}).get("gateway") or {}).get("default_port"))


def effective_gateway_host(config, pkg_file, overlay):
    """배포의 게이트웨이 upstream host — 운영자가 배포 설정 `Server.GatewayHost` 로
    명시한 도달 주소. base OAM 과 모듈이 다른 호스트에 배치되는 분리 토폴로지에서
    그룹 VIP(권장) 또는 노드 IP 를 넣는다. 비우면 None — caller 가 127.0.0.1
    (동거 배치 기본) 사용. 해석 규칙은 effective_server_port 와 동일 (flat 우선,
    nested 수용)."""
    try:
        eff = _materialize_deploy_config(config, pkg_file, overlay)
        host = eff.get("Server.GatewayHost")
        if not host and isinstance(eff.get("Server"), dict):
            host = eff["Server"].get("GatewayHost")
        host = str(host or "").strip()
        if host:
            return host
    except Exception:
        pass
    return None


def deploy_config_hash(config, pkg_file, overlay) -> str:
    """배포 config 실체화본의 canonical hash 12hex — agent 가 metric.cfg_hashes 로
    보고하는 노드 실파일 hash (cims_agent._cfg_hash_for_module) 와 동일 규칙
    (parse→sort_keys canonical dump→sha256). 불일치 = config_out_of_sync 알람."""
    eff = _materialize_deploy_config(config, pkg_file, overlay)
    return hashlib.sha256(json.dumps(eff, sort_keys=True, ensure_ascii=False,
                                     separators=(",", ":")).encode("utf-8")).hexdigest()[:12]


def _pkg_load_all(config) -> list:
    return file_store.load_all(_pkg_dir(config))


def _pkg_load_latest_by_name(config, name: str):
    """name 이 같은 모든 버전 중 id 최대 (= 마지막 업로드) 1건."""
    rows = [p for p in _pkg_load_all(config) if p.get('name') == name]
    if not rows:
        return None
    rows.sort(key=lambda x: x.get('id', 0))
    return rows[-1]


# ── cims_agent file_store helpers ──────────────────────────────────────

def _agent_dir(config):
    return file_store.domain_dir(config, _AGENT_DOMAIN)


def _agent_load(config, aid: int = None, name: str = None,
                agent_token: str = None, enrollment_token: str = None):
    """id / name / agent_token / enrollment_token 중 하나로 1건 조회."""
    if aid is not None:
        return file_store.by_id(_agent_dir(config), aid)
    if name:
        return file_store.find_by(_agent_dir(config), lambda o: o.get('name') == name)
    if agent_token:
        return file_store.find_by(_agent_dir(config),
                                  lambda o: o.get('agent_token') == agent_token
                                  and o.get('status') != 'revoked')
    if enrollment_token:
        return file_store.find_by(_agent_dir(config),
                                  lambda o: o.get('enrollment_token') == enrollment_token)
    return None


def _agent_load_all(config) -> list:
    return file_store.load_all(_agent_dir(config))


def _agent_save(config, agent: dict) -> dict:
    """agent dict (id 필수) atomic 저장. 누락된 create_time 은 file_store 가 채움."""
    file_store.save(_agent_dir(config), int(agent['id']), agent)
    return agent


def _agent_update(config, aid: int, patches: dict) -> dict | None:
    """id 로 로드 → patches merge → 저장. 없으면 None."""
    existing = _agent_load(config, aid=aid)
    if not existing:
        return None
    existing.update(patches)
    file_store.save(_agent_dir(config), aid, existing)
    return existing


def _enrich_with_agent(rows: list, config, key_in='agent_id', key_out_prefix='agent_'):
    """rows 의 agent_id 를 agent name/ip_address 로 enrich.
    enriched 필드: agent_name. (호출자가 필요한 다른 필드는 직접 lookup 가능)"""
    if not rows:
        return rows
    cache: dict = {}
    for r in rows:
        aid = r.get(key_in)
        if aid is None:
            continue
        if aid not in cache:
            cache[aid] = _agent_load(config, aid=aid) or {}
        a = cache[aid]
        r[f'{key_out_prefix}name'] = a.get('name')
    return rows


# ── agent_deployment / agent_job / agent_metric file_store helpers ─────

def _deploy_dir(config):
    return file_store.domain_dir(config, _DEPLOY_DOMAIN)


def _deploy_load(config, did: int):
    return file_store.by_id(_deploy_dir(config), did)


def _deploy_load_all(config) -> list:
    return file_store.load_all(_deploy_dir(config))


def _deploy_save(config, dep: dict) -> dict:
    file_store.save(_deploy_dir(config), int(dep['id']), dep)
    return dep


def _deploy_update(config, did: int, patches: dict) -> dict | None:
    existing = _deploy_load(config, did)
    if not existing:
        return None
    existing.update(patches)
    file_store.save(_deploy_dir(config), did, existing)
    return existing


def _job_dir(config):
    return file_store.domain_dir(config, _JOB_DOMAIN)


def _job_load(config, jid: int):
    return file_store.by_id(_job_dir(config), jid)


def _job_load_all(config) -> list:
    return file_store.load_all(_job_dir(config))


def _job_create(config, agent_id: int, job_type: str, params: dict,
                status: str = 'queued') -> int:
    """agent_job 1건 생성. lastrowid 호환을 위해 id 반환."""
    from datetime import datetime as _dt
    d = _job_dir(config)
    new_id = file_store.next_id(d)
    now = _dt.now().isoformat(timespec='seconds')
    obj = {
        'id': new_id,
        'agent_id': agent_id,
        'job_type': job_type,
        'params': params if isinstance(params, (dict, list)) else {},
        'status': status,
        'result_code': None,
        'result_stdout': None,
        'result_stderr': None,
        'dispatched_at': None,
        'completed_at': None,
        'create_time': now,
        'update_time': now,
    }
    file_store.save(d, new_id, obj)
    return new_id


def _job_update(config, jid: int, patches: dict) -> dict | None:
    existing = _job_load(config, jid)
    if not existing:
        return None
    existing.update(patches)
    file_store.save(_job_dir(config), jid, existing)
    return existing


def _job_pick_pending(config, agent_id: int, limit: int = 10) -> list:
    """agent_id 의 status='queued' job 최대 limit 개 → status='running' 으로 전이.

    파일 잠금 없이 동시성 호출 가능하지만 단일 CSC 환경 기준 충돌 가능성 낮음.
    """
    from datetime import datetime as _dt
    all_jobs = _job_load_all(config)
    pending = [j for j in all_jobs if j.get('agent_id') == agent_id and j.get('status') == 'queued']
    pending.sort(key=lambda j: j.get('id', 0))
    picked = pending[:limit]
    if picked:
        now = _dt.now().isoformat(timespec='seconds')
        for j in picked:
            j['status'] = 'running'
            j['dispatched_at'] = now
            file_store.save(_job_dir(config), j['id'], j)
    return picked


def _metric_root(config):
    return file_store.domain_dir(config, _METRIC_DOMAIN)


def _metric_append(config, agent_id: int, record: dict):
    """agent_metric JSONL append — {CimsRuntimeDir}/metrics/<agent_id>/YYYY/MM/DD.jsonl"""
    from datetime import datetime as _dt
    record = dict(record)
    record.setdefault('ts', _dt.now().isoformat(timespec='seconds'))
    record['agent_id'] = agent_id
    file_store.jsonl_append(_metric_root(config), str(agent_id), record)


def _metric_load_recent(config, agent_id: int, limit: int = 120, days: int = 7) -> list:
    """최근 metric 시계열을 최신순으로 limit 개 반환.
    파일 끝에서부터 tail-read 하여 limit 를 채우면 조기 종료 — 2s 케이던스(하루 ~43K줄)
    에서 7일 전체를 list() 로 파싱하던 옛 구현(요청당 2~3s, 동시 4건 시 위젯 4s 타임아웃
    초과 → '시스템 리소스' 빈칸)을 해소."""
    rows = file_store.jsonl_tail_recent(_metric_root(config), str(agent_id), limit=limit, days=days)
    rows.sort(key=lambda r: r.get('ts', ''), reverse=True)
    return rows[:limit]

_AGENT_BASE       = "/api/v1/agents"
_PACKAGE_BASE     = "/api/v1/packages"
_DEPLOYMENT_BASE  = "/api/v1/deployments"
_SYNC_TXN_BASE    = "/api/v1/csp/sync"
_DRIFT_BASE       = "/api/v1/csp/drift"
_SIP_SERVICES_BASE = "/api/v1/csp/services"  # L5: csp_runtime/sip_service → deployment.collection/access_services 로 마이그레이션

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
    if val is None:
        return None
    # file_store 는 ISO 문자열로 저장 → 이미 str 이면 그대로. datetime 이면 isoformat.
    return val.isoformat() if hasattr(val, "isoformat") else val


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
        if raw is None: return None
        if isinstance(raw, (dict, list)): return raw
        try: return json.loads(raw)
        except (TypeError, ValueError): return None

    def _maybe_dt(v):
        if v is None: return None
        if hasattr(v, 'isoformat'): return v.isoformat()
        return v
    return {
        "id": r.get("id"),
        "name": r.get("name"),
        "status": r.get("status"),
        "hostname": r.get("hostname"),
        "ip_address": r.get("ip_address"),
        "os_info": r.get("os_info"),
        "cpu_cores": r.get("cpu_cores"),
        "memory_mb": r.get("memory_mb"),
        "disk_gb": r.get("disk_gb"),
        "agent_version": r.get("agent_version"),
        "agent_versions": r.get("agent_versions") if isinstance(r.get("agent_versions"), list) else [],
        "last_heartbeat": _maybe_dt(r.get("last_heartbeat")),
        "last_metric":    _maybe_dt(r.get("last_metric")),
        "enrolled_at":    _maybe_dt(r.get("enrolled_at")),
        "approved_at":    _maybe_dt(r.get("approved_at")),
        "note": r.get("note"),
        "create_time": _maybe_dt(r.get("create_time")),
        # 보안: enrollment_token 은 생성 직후에만 반환. 여기서는 masked
        "has_pending_enrollment": bool(r.get("enrollment_token")),
        # 만료 시각은 UI 표시용으로 노출 (토큰 자체는 마스킹 유지)
        "enrollment_token_expires_at": _maybe_dt(r.get("enrollment_token_expires_at")),
        # HA 그룹 정보 — 미정의 시 null
        "ha_group": ha_group,
        # HaServicesPage 용 확장 필드 (없으면 null)
        "interfaces":      r.get("interfaces") if isinstance(r.get("interfaces"), (dict, list))
                           else _safe_load(r.get("interfaces_json")),
        "service_ip_rows": r.get("service_ip_rows") if isinstance(r.get("service_ip_rows"), (dict, list))
                           else _safe_load(r.get("service_ip_rows_json")),
        # 운영자가 관리하는 specific route (default 제외). agent heartbeat 보고 + apply 갱신.
        "routes":          r.get("routes") if isinstance(r.get("routes"), (dict, list)) else None,
        # cims-managed 마운트 (fstab 영속). agent heartbeat 보고(mounted 상태 포함) + apply 갱신.
        "mounts":          r.get("mounts") if isinstance(r.get("mounts"), list) else None,
        # 서버별 네트워크 튜닝 desired-state ({sysctl:{...}, rps:[...]}). apply 시 저장.
        "net_tuning":      r.get("net_tuning") if isinstance(r.get("net_tuning"), dict) else None,
    }


def _console_rbac(handler_args, *, read_role: str = "monitor", write_role: str = "admin"):
    """콘솔용 API RBAC 게이트 — None=통과, HandlerResult=거부(401/403).

    배경(2026-06-10): /api/v1 콘솔 API 가 무인증 노출이던 것을 차단. JWT
    (Authorization Bearer, CimsAuth.JwtSecret) 필수. GET=read_role 이상,
    그 외 메서드=write_role 이상. 권한 모델: 패키지 설정(config/collection)
    =operator+, 인프라/설치 변이=admin (콘솔 UI 는 admin 패스워드 승격 지원).
    agent 통신(/api/agent/*, X-Agent-Token)과 공개 에셋은 본 게이트 무관.
    """
    from services.admin_auth import require_role
    need = read_role if handler_args.method.upper() == "GET" else write_role
    _payload, err = require_role(handler_args, need)
    return err


async def handle_agents(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    tail = _path_tail(handler_args.full_path, _AGENT_BASE)
    method = handler_args.method.upper()

    read_role = "admin" if (len(tail) >= 2 and tail[1] == "install-command") else "monitor"
    deny = _console_rbac(handler_args, read_role=read_role)
    if deny: return deny

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
        if action == "regenerate-token" and method == "POST":
            return await _regenerate_token(handler_args, aid, config)
        if action == "install-command" and method == "GET":
            return await _get_install_command(handler_args, aid, config)
        if action == "metrics" and method == "GET":
            return await _agent_metrics(aid, config)
        if action == "upgrade" and method == "POST":
            return await _upgrade_agent_binary(handler_args, aid, config)
        if action == "rollback" and method == "POST":
            return await _rollback_agent_binary(handler_args, aid, config)
        if action == "restart" and method == "POST":
            return await _restart_agent(handler_args, aid, config)
        if action == "apply-ip-config" and method == "POST":
            return await _apply_ip_config(handler_args, aid, config)
        if action == "apply-mounts" and method == "POST":
            return await _apply_mounts(handler_args, aid, config)
        if action == "apply-net-tuning" and method == "POST":
            return await _apply_net_tuning(handler_args, aid, config)
        if action == "health-check" and method == "POST":
            return await _agent_health_check(handler_args, aid, config)
        if action == "interface-roles" and method == "PUT":
            return await _put_interface_roles(handler_args, aid, config)
        if action == "interface-roles" and method == "GET":
            return await _get_interface_roles(aid, config)
    elif len(tail) == 3:
        # GET /agents/{aid}/jobs/{jid} — job 단건 조회 (result polling)
        if tail[1] == "jobs" and method == "GET":
            try: jid = int(tail[2])
            except (TypeError, ValueError):
                return HandlerResult(status=400, body={"error": "invalid_job_id"}, media_type="application/json")
            return await _get_agent_job(aid, jid, config)
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


async def _get_agent_job(aid: int, jid: int, config):
    """단일 job 조회 — agent_id 매칭 확인 후 result_code/stdout/stderr 포함 반환."""
    j = await asyncio.to_thread(_job_load, config, jid)
    if not j:
        return HandlerResult(status=404, body={"error": "job_not_found"}, media_type="application/json")
    if j.get("agent_id") != aid:
        return HandlerResult(status=404, body={"error": "job_not_for_agent"}, media_type="application/json")
    return HandlerResult(status=200, body=j, media_type="application/json")


def _ha_group_map_for_agents(config) -> dict:
    """모든 agent 의 ha_group {id,name,mode,role} 매핑. dict[agent_id] = {...}"""
    out = {}
    groups = file_store.load_all(file_store.domain_dir(config, 'ha_groups'))
    for g in groups:
        for m in (g.get('members') or []):
            aid = m.get('agent_id')
            if aid is None:
                continue
            out[aid] = {
                'id': g.get('id'), 'name': g.get('name'), 'mode': g.get('mode'),
                'role': m.get('role'),
            }
    return out


async def _list_agents(config):
    rows = await asyncio.to_thread(_agent_load_all, config)
    rows.sort(key=lambda x: x.get('id', 0))
    ha_map = await asyncio.to_thread(_ha_group_map_for_agents, config)
    return HandlerResult(status=200,
                         body={"items": [_agent_to_json(r, ha_group=ha_map.get(r.get("id"))) for r in rows]},
                         media_type="application/json")


async def _get_agent(aid: int, config):
    r = await asyncio.to_thread(_agent_load, config, aid)
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    ha_map = await asyncio.to_thread(_ha_group_map_for_agents, config)
    return HandlerResult(status=200,
                         body=_agent_to_json(r, ha_group=ha_map.get(aid)),
                         media_type="application/json")


async def _create_agent(handler_args: HandlerArgs, config):
    """Agent 레코드 생성 + enrollment_token 발급 → install-agent.sh 에 전달용."""
    body = _parse_body(handler_args)
    name = (body.get("name") or "").strip()
    if not name:
        return HandlerResult(status=400, body={"error": "name required"}, media_type="application/json")
    # 중복 name 검사 (옛 DB 의 UNIQUE 제약 대체)
    if await asyncio.to_thread(_agent_load, config, None, name):
        return HandlerResult(status=409, body={"error": "conflict", "detail": f"agent name '{name}' already exists"},
                             media_type="application/json")
    enroll_token = secrets.token_hex(24)
    ttl_sec = _enrollment_token_ttl_sec(config)
    now_dt = datetime.now()
    issued_at = now_dt.isoformat(timespec='seconds')
    expires_at = (now_dt + timedelta(seconds=ttl_sec)).isoformat(timespec='seconds')

    def _do_create():
        new_id = file_store.next_id(_agent_dir(config))
        row = {
            'id': new_id,
            'name': name,
            'enrollment_token': enroll_token,
            'enrollment_token_issued_at': issued_at,
            'enrollment_token_expires_at': expires_at,
            'agent_token': secrets.token_hex(32),
            'status': 'pending',
            'note': body.get('note'),
        }
        return _agent_save(config, row)

    row = await asyncio.to_thread(_do_create)
    result = _agent_to_json(row)
    # enrollment_token 은 최초 생성 시만 반환
    result["enrollment_token"] = enroll_token
    result["enrollment_token_expires_at"] = expires_at
    result["enrollment_token_ttl_sec"] = ttl_sec
    oam_url = _oam_public_url(handler_args, config)
    import shlex
    # name 에 space/특수문자 포함 가능 → shell-safe quote
    result["install_command"]  = (
        f"curl -k {oam_url}/install-agent.sh | "
        f"bash -s -- --oam-url {oam_url} "
        f"--enrollment-token {enroll_token} --name {shlex.quote(name)}"
    )
    return HandlerResult(status=201, body=result, media_type="application/json")


def _enrollment_token_ttl_sec(config: dict) -> int:
    """enrollment_token TTL (default 10분). config.Agent.EnrollmentTokenTtlSec 로 조정."""
    agent_cfg = (config.get("Agent") or {})
    try:
        v = int(agent_cfg.get("EnrollmentTokenTtlSec") or 600)
        return max(60, v)  # 최소 1분
    except (TypeError, ValueError):
        return 600


def _is_dev_mode(config: dict) -> bool:
    """DEV-CSC 여부. csc.json Server.DevMode=true 일 때 build/dist/packages 자동 등록 (register-from-dist) endpoint 활성."""
    srv = (config.get("Server") or {})
    return bool(srv.get("DevMode"))


def _oam_public_url(handler_args: HandlerArgs, config: dict) -> str:
    """install 명령에 박을 OAM URL — agent 들이 mgmt 망에서 OAM 으로 접속할 주소.
    Phase 3b 부터 agent 는 OAM (4419) 과 통신.
    우선순위:
       1) config.Server.AgentOamUrl (명시적 설정 — mgmt 망 IP 권장)
       2) config.Server.AgentCscUrl (옛 키 — deprecated, 호환성)
       3) HTTP Host 헤더 (admin 이 접속한 주소 그대로)
       4) config.Server.Ip + Port (0.0.0.0 면 placeholder)
    """
    srv = (config.get("Server") or {})
    pu = (srv.get("AgentOamUrl") or srv.get("AgentCscUrl") or "").strip()
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
    port = srv.get("Port") or 4419
    if ip == "0.0.0.0" or not ip:
        return f"https://<OAM_HOST>:{port}"
    return f"https://{ip}:{port}"


# 옛 함수명 호환 (외부 호출자 — 일단 alias 유지, Phase 3b 후속에서 정리).
_csc_public_url = _oam_public_url


async def _update_agent(handler_args: HandlerArgs, aid: int, config):
    body = _parse_body(handler_args)
    patches: dict = {}
    for col in ("name", "note"):
        if col in body:
            patches[col] = body[col]
    # HaServicesPage 운영자가 설정한 iface→slot 매핑 (서비스 IP rows)
    if "service_ip_rows" in body:
        patches['service_ip_rows'] = body.get('service_ip_rows')
    if not patches:
        return HandlerResult(status=400, body={"error": "no_updatable_fields"}, media_type="application/json")
    row = await asyncio.to_thread(_agent_update, config, aid, patches)
    if not row:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body=_agent_to_json(row), media_type="application/json")


async def _delete_agent(handler_args: HandlerArgs, aid: int, config):
    def _cascade_remove_from_ha_groups():
        ha_dir = file_store.domain_dir(config, 'ha_groups')
        for g in file_store.load_all(ha_dir):
            members = g.get('members') or []
            new_members = [m for m in members if m.get('agent_id') != aid]
            if len(new_members) != len(members):
                g['members'] = new_members
                file_store.save(ha_dir, int(g['id']), g)
    await asyncio.to_thread(_cascade_remove_from_ha_groups)
    await asyncio.to_thread(file_store.delete, _agent_dir(config), aid)
    return HandlerResult(status=204, body=None, media_type="application/json")


async def _regenerate_token(handler_args: HandlerArgs, aid: int, config):
    """enrollment_token 만 재발급 (id / agent_token / status / ha_group membership 보존).
    기존 토큰이 미만료면 409 conflict — 기존 토큰을 그대로 쓰도록 유도."""
    def _do():
        existing = _agent_load(config, aid=aid)
        if not existing:
            return ('not_found', None, None)
        prev_expires_at = existing.get('enrollment_token_expires_at')
        if prev_expires_at:
            try:
                if datetime.fromisoformat(prev_expires_at) > datetime.now():
                    return ('still_valid', existing, prev_expires_at)
            except (ValueError, TypeError):
                pass
        ttl_sec = _enrollment_token_ttl_sec(config)
        now_dt = datetime.now()
        existing['enrollment_token'] = secrets.token_hex(24)
        existing['enrollment_token_issued_at'] = now_dt.isoformat(timespec='seconds')
        existing['enrollment_token_expires_at'] = (
            now_dt + timedelta(seconds=ttl_sec)).isoformat(timespec='seconds')
        file_store.save(_agent_dir(config), aid, existing)
        return ('ok', existing, ttl_sec)
    status, row, extra = await asyncio.to_thread(_do)
    if status == 'not_found':
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    if status == 'still_valid':
        return HandlerResult(status=409, body={
            "error": "still_valid",
            "enrollment_token_expires_at": extra,
        }, media_type="application/json")
    ttl_sec = extra
    payload = _agent_to_json(row)
    payload["enrollment_token"] = row['enrollment_token']
    payload["enrollment_token_expires_at"] = row['enrollment_token_expires_at']
    payload["enrollment_token_ttl_sec"] = ttl_sec
    oam_url = _oam_public_url(handler_args, config)
    import shlex
    # 토큰 명령 = 다운로드 전용(sudo 불필요). install-agent.sh 가 비root 로 실행되면
    # 자신을 내려받고 'sudo bash install-agent.sh ...' 안내만 출력(설치는 그 단계에서 sudo).
    payload["install_command"] = (
        f"curl -fsSLk {oam_url}/install-agent.sh | bash -s -- --oam-url {oam_url} "
        f"--enrollment-token {row['enrollment_token']} --name {shlex.quote(row['name'])}"
    )
    return HandlerResult(status=200, body=payload, media_type="application/json")


async def _get_install_command(handler_args: HandlerArgs, aid: int, config):
    """현재 record 의 토큰으로 install_command 반환 (재발행 없이). 만료/없음 시 410."""
    def _do():
        existing = _agent_load(config, aid=aid)
        if not existing:
            return ('not_found', None)
        token = existing.get('enrollment_token')
        expires_at = existing.get('enrollment_token_expires_at')
        if not token:
            return ('no_token', existing)
        if expires_at:
            try:
                if datetime.fromisoformat(expires_at) < datetime.now():
                    return ('expired', existing)
            except (ValueError, TypeError):
                pass
        return ('ok', existing)
    status, row = await asyncio.to_thread(_do)
    if status == 'not_found':
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    oam_url = _oam_public_url(handler_args, config)
    import shlex
    if status in ('no_token', 'expired'):
        return HandlerResult(status=200, body={
            "install_command": None,
            "install_command_error": status,
            "enrollment_token_expires_at": row.get('enrollment_token_expires_at'),
        }, media_type="application/json")
    install_cmd = (
        f"curl -fsSLk {oam_url}/install-agent.sh | bash -s -- --oam-url {oam_url} "
        f"--enrollment-token {row['enrollment_token']} --name {shlex.quote(row['name'])}"
    )
    return HandlerResult(status=200, body={
        "install_command": install_cmd,
        "enrollment_token_expires_at": row.get('enrollment_token_expires_at'),
    }, media_type="application/json")


async def _approve_agent(handler_args: HandlerArgs, aid: int, config):
    from datetime import datetime
    def _do():
        existing = _agent_load(config, aid=aid)
        if not existing or existing.get('status') != 'pending':
            return False
        existing['status'] = 'approved'
        existing['approved_at'] = datetime.now().isoformat(timespec='seconds')
        file_store.save(_agent_dir(config), aid, existing)
        return True
    changed = await asyncio.to_thread(_do)
    return HandlerResult(status=200 if changed else 409,
                         body={"ok": bool(changed), "id": aid}, media_type="application/json")


async def _revoke_agent(handler_args: HandlerArgs, aid: int, config):
    await asyncio.to_thread(_agent_update, config, aid, {'status': 'revoked'})
    return HandlerResult(status=200, body={"ok": True, "id": aid}, media_type="application/json")


# Phase 4d — interface role 명시 API.
# admin 이 NIC IP 별 용도(role) 를 명시: "service" / "internal" / "mgmt".
# mgmt 는 자동 (oam.json Mgmt.Cidr + agent detect_mgmt_ip) 이지만 명시도 허용.
# HA group vip_bindings.slot 이 role 명과 매칭되면 memberIfaces 자동 추론에 활용.
async def _put_interface_roles(handler_args: HandlerArgs, aid: int, config):
    """PUT /api/v1/agents/<id>/interface-roles
    Body: {"<ip>": "<role>", ...} — IP 단위 mapping. 빈 string 으로 role 제거.
    """
    body = _parse_body(handler_args)
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={"error": "body_must_be_object"},
                             media_type="application/json")
    # role 값 정규화 (소문자, allowed set).
    _ALLOWED = {'mgmt', 'service', 'internal', ''}
    normalized = {}
    for ip, role in body.items():
        role_l = (role or '').strip().lower()
        if role_l not in _ALLOWED:
            return HandlerResult(status=400, body={
                "error": "invalid_role", "ip": ip, "role": role,
                "allowed": sorted(_ALLOWED - {''})},
                media_type="application/json")
        normalized[str(ip)] = role_l
    # 기존 record load + override 갱신.
    def _do():
        existing = _agent_load(config, aid=aid)
        if not existing:
            return None
        cur = (existing.get('interface_role_overrides') or {}).copy()
        for ip, role_l in normalized.items():
            if role_l:
                cur[ip] = role_l
            else:
                cur.pop(ip, None)
        existing['interface_role_overrides'] = cur
        # 동시에 interfaces[].role 도 갱신 (다음 heartbeat 전 일관성).
        for it in (existing.get('interfaces') or []):
            if not isinstance(it, dict): continue
            ip = it.get('ip')
            if ip in cur:
                it['role'] = cur[ip]
            elif ip in normalized and not normalized[ip]:
                it.pop('role', None)
        file_store.save(_agent_dir(config), aid, existing)
        return existing
    r = await asyncio.to_thread(_do)
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    return HandlerResult(status=200, body={
        "ok": True,
        "interface_role_overrides": r.get('interface_role_overrides') or {},
    }, media_type="application/json")


async def _get_interface_roles(aid: int, config):
    """GET /api/v1/agents/<id>/interface-roles — 현재 override + auto 결과 모두."""
    r = await asyncio.to_thread(_agent_load, config, aid=aid)
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    body = {
        "agent_id": aid,
        "overrides": r.get('interface_role_overrides') or {},
        "interfaces": [
            {"ip": it.get('ip'), "name": it.get('name'), "role": it.get('role'),
             "mgmt": bool(it.get('mgmt'))}
            for it in (r.get('interfaces') or []) if isinstance(it, dict)
        ],
    }
    return HandlerResult(status=200, body=body, media_type="application/json")


async def _apply_ip_config(handler_args: HandlerArgs, aid: int, config):
    """ServiceIp/Route Panel 의 [추가]/[삭제] 진입점 — agent sync REST 로 즉시 호출.

    Request body:
      {
        "service_ip_rows": [{"op": "add"|"del", "iface", "ip", "mask", "slot"?}, ...],
        "routes":          [{"op": "add"|"del", "dst", "via", "dev"}, ...]   (optional)
      }
    body 가 없으면 file_store 의 stored agent.service_ip_rows 를 모두 add 시도
    (backward compat — 옛 [적용] 흐름).

    agent 가 성공 응답 시 file_store 의 service_ip_rows / routes 자동 갱신
    (op 반영 — add 면 추가, del 면 제거. op 필드 자체는 stored 에 보존 안 함).
    """
    row = await asyncio.to_thread(_agent_load, config, aid)
    if not row:
        return HandlerResult(status=404, body={"error": "agent_not_found"}, media_type="application/json")

    body_in = _parse_body(handler_args)                                         # dict / bytes / str 모두 정규화

    rows_in = body_in.get("service_ip_rows")
    routes_in = body_in.get("routes")

    if not rows_in and not routes_in:
        # backward compat — file_store 의 desired state 를 통째로 add 시도
        rows_in = row.get("service_ip_rows")
        if isinstance(rows_in, str):
            try: rows_in = json.loads(rows_in)
            except (TypeError, ValueError): rows_in = []
        if not rows_in:
            return HandlerResult(status=400, body={"error": "no_operations"},
                                 media_type="application/json")
        rows_in = [{**r, "op": "add"} for r in rows_in if isinstance(r, dict)]

    payload = {}
    if rows_in:   payload["service_ip_rows"] = rows_in
    if routes_in: payload["routes"] = routes_in

    status, resp = await asyncio.to_thread(
        _agent_proxy_call, "POST", row, "/apply-ip-config",
        None, payload, 15, config)

    if status == 0:
        return HandlerResult(status=502,
                             body={"error": "agent_unreachable",
                                   "detail": (resp or {}).get("error"),
                                   "agent_id": aid},
                             media_type="application/json")
    if status != 200:
        return HandlerResult(status=status,
                             body={"error": "agent_error", "detail": resp,
                                   "agent_id": aid},
                             media_type="application/json")

    # file_store 갱신 — op 반영 (성공한 row 만 반영 어렵지만 1차는 일괄 적용).
    # agent 응답이 부분 실패라도 rc 가 200 인 경우만 여기 도달 (rc 비0 은 status!=200).
    if rows_in or routes_in:
        await asyncio.to_thread(_reconcile_stored_net_config, config, aid, rows_in or [], routes_in or [])

    return HandlerResult(status=200,
                         body={"agent_id": aid,
                               "rows": len(rows_in or []),
                               "routes": len(routes_in or []),
                               **(resp or {})},
                         media_type="application/json")


def _reconcile_stored_net_config(config: dict, aid: int, rows_in: list, routes_in: list) -> None:
    """apply 성공 후 file_store 의 service_ip_rows / routes 를 op 반영해 갱신.
    op 필드는 stored 에 보존하지 않음 (transient — 적용 시점 의도).
    """
    row = _agent_load(config, aid)
    if not row:
        return

    def _norm(s): return (s or "").strip()

    # service_ip_rows
    stored = row.get("service_ip_rows") or []
    if isinstance(stored, str):
        try: stored = json.loads(stored)
        except (TypeError, ValueError): stored = []
    # (iface, ip) key 로 dict 화 — op 적용 후 list 복원.
    by_key = {}
    for r in stored:
        if not isinstance(r, dict): continue
        k = (_norm(r.get("iface")), _norm(r.get("ip")))
        by_key[k] = {kk: vv for kk, vv in r.items() if kk != "op"}
    for r in rows_in or []:
        op = (r.get("op") or "add").lower()
        k = (_norm(r.get("iface")), _norm(r.get("ip")))
        if op == "add":
            base = {kk: vv for kk, vv in r.items() if kk != "op"}
            by_key[k] = base
        elif op == "del":
            by_key.pop(k, None)
    new_rows = [v for v in by_key.values() if v.get("ip")]

    # routes — (dst, via, dev) key
    stored_r = row.get("routes") or []
    if isinstance(stored_r, str):
        try: stored_r = json.loads(stored_r)
        except (TypeError, ValueError): stored_r = []
    by_rkey = {}
    for r in stored_r:
        if not isinstance(r, dict): continue
        k = (_norm(r.get("dst")), _norm(r.get("via")), _norm(r.get("dev")))
        by_rkey[k] = {kk: vv for kk, vv in r.items() if kk != "op"}
    for r in routes_in or []:
        op = (r.get("op") or "add").lower()
        dst = _norm(r.get("dst"))
        k = (dst, _norm(r.get("via")), _norm(r.get("dev")))
        if op == "add":
            # cims-priv 가 'ip route replace' 사용 → 같은 dst 의 옛 entry 제거 후 새 entry 추가
            # (default GW 변경 시 옛 via 가 stored 에 남는 buf 방지).
            for old_k in [kk for kk in by_rkey.keys() if kk[0] == dst]:
                del by_rkey[old_k]
            base = {kk: vv for kk, vv in r.items() if kk != "op"}
            # agent heartbeat 의 routes 와 동일 flag 부여 — UI sort/표시 정합 (다음 hb 까지 일관).
            if dst in ("default", "0.0.0.0/0"):
                base["is_default"] = True
            else:
                base["managed"] = True   # cims-priv 가 추가한 route → managed
            by_rkey[k] = base
        elif op == "del":
            by_rkey.pop(k, None)
    new_routes = list(by_rkey.values())

    _agent_update(config, aid, {
        "service_ip_rows": new_rows,
        "routes": new_routes,
    })


async def _apply_mounts(handler_args: HandlerArgs, aid: int, config):
    """MountPanel 의 [추가]/[삭제] 진입점 — agent sync REST /apply-mounts 즉시 호출.
    agent 가 fstab 에 기록(영속 → 재부팅 시 OS 자동 마운트) + 즉시 mount.

    Request body: { "mounts": [{op:'add'|'del', fstype, source, target, options?}, ...] }
    성공 시 file_store 의 agent.mounts(desired) 를 op 반영해 갱신.
    """
    row = await asyncio.to_thread(_agent_load, config, aid)
    if not row:
        return HandlerResult(status=404, body={"error": "agent_not_found"}, media_type="application/json")

    body_in = _parse_body(handler_args)
    mounts_in = body_in.get("mounts")
    if not mounts_in or not isinstance(mounts_in, list):
        return HandlerResult(status=400, body={"error": "no_operations"}, media_type="application/json")

    status, resp = await asyncio.to_thread(
        _agent_proxy_call, "POST", row, "/apply-mounts",
        None, {"mounts": mounts_in}, 45, config)

    if status == 0:
        return HandlerResult(status=502,
                             body={"error": "agent_unreachable", "detail": (resp or {}).get("error"),
                                   "agent_id": aid}, media_type="application/json")
    if status != 200:
        return HandlerResult(status=status,
                             body={"error": "agent_error", "detail": resp, "agent_id": aid},
                             media_type="application/json")

    await asyncio.to_thread(_reconcile_stored_mounts, config, aid, mounts_in)
    return HandlerResult(status=200,
                         body={"agent_id": aid, "mounts": len(mounts_in), **(resp or {})},
                         media_type="application/json")


# net.core.* 성능 sysctl allowlist (agent cims-priv 와 동일 — UI/검증 일관성).
_NET_TUNING_SYSCTL_KEYS = (
    "net.core.netdev_max_backlog", "net.core.netdev_budget", "net.core.netdev_budget_usecs",
    "net.core.rmem_max", "net.core.wmem_max", "net.core.rmem_default", "net.core.wmem_default",
    "net.core.optmem_max", "net.core.somaxconn",
)

async def _apply_net_tuning(handler_args: HandlerArgs, aid: int, config):
    """NetTuningPanel 진입점 — 서버별 네트워크 튜닝(RPS + sysctl) 저장 + agent job 큐잉.

    Request body: { "sysctl": {key: value, ...}, "rps": [{iface, cpus}, ...] }
    agent 가 sysctl 은 /etc/sysctl.d 로 영속, RPS 는 sysfs 적용 + 부팅 재적용.
    저장(agent.net_tuning, desired-state) 후 'apply_net_tuning' job 큐잉(heartbeat pickup).
    """
    row = await asyncio.to_thread(_agent_load, config, aid)
    if not row:
        return HandlerResult(status=404, body={"error": "agent_not_found"}, media_type="application/json")

    body_in = _parse_body(handler_args)
    sysctl_in = body_in.get("sysctl") or {}
    rps_in    = body_in.get("rps") or []
    if not isinstance(sysctl_in, dict) or not isinstance(rps_in, list):
        return HandlerResult(status=400, body={"error": "invalid_body"}, media_type="application/json")

    # sysctl 키 allowlist + 정수값 검증 (백엔드 게이트)
    clean_sysctl = {}
    for k, v in sysctl_in.items():
        if k not in _NET_TUNING_SYSCTL_KEYS:
            return HandlerResult(status=400, body={"error": "sysctl_key_not_allowed", "key": k},
                                 media_type="application/json")
        try:
            clean_sysctl[k] = int(v)
        except (TypeError, ValueError):
            return HandlerResult(status=400, body={"error": "sysctl_value_not_int", "key": k},
                                 media_type="application/json")
    clean_rps = []
    for r in rps_in:
        iface = (r.get("iface") or "").strip(); cpus = str(r.get("cpus") or "").strip()
        if not iface or not cpus:
            return HandlerResult(status=400, body={"error": "rps_iface_cpus_required"},
                                 media_type="application/json")
        clean_rps.append({"iface": iface, "cpus": cpus})

    tuning = {"sysctl": clean_sysctl, "rps": clean_rps}
    await asyncio.to_thread(_agent_update, config, aid, {"net_tuning": tuning})
    job_id = await asyncio.to_thread(_job_create, config, aid, "apply_net_tuning", tuning)
    return HandlerResult(status=202,
                         body={"agent_id": aid, "job_id": job_id, "status": "queued",
                               "sysctl": len(clean_sysctl), "rps": len(clean_rps)},
                         media_type="application/json")


def _reconcile_stored_mounts(config: dict, aid: int, mounts_in: list) -> None:
    """apply 성공 후 file_store 의 agent.mounts(desired) 를 op 반영. key=target."""
    row = _agent_load(config, aid)
    if not row:
        return
    stored = row.get("mounts") or []
    if isinstance(stored, str):
        try: stored = json.loads(stored)
        except (TypeError, ValueError): stored = []
    by_target = {}
    for m in stored:
        if isinstance(m, dict) and m.get("target"):
            by_target[m["target"].strip()] = {k: v for k, v in m.items() if k != "op"}
    for m in mounts_in or []:
        op = (m.get("op") or "add").lower()
        tgt = (m.get("target") or "").strip()
        if not tgt:
            continue
        if op == "add":
            by_target[tgt] = {k: v for k, v in m.items() if k != "op"}
        elif op == "del":
            by_target.pop(tgt, None)
    _agent_update(config, aid, {"mounts": list(by_target.values())})


async def _upgrade_agent_binary(handler_args: HandlerArgs, aid: int, config):
    """Agent 자기 바이너리 업그레이드 job 큐잉.
    Agent 가 heartbeat 로 pickup → /cims_agent.py 다운로드 → 자기 교체 → 종료 → systemd 재기동."""
    row = await asyncio.to_thread(_agent_load, config, aid)
    if not row:
        return HandlerResult(status=404, body={"error": "agent_not_found"},
                             media_type="application/json")
    job_id = await asyncio.to_thread(_job_create, config, aid, 'upgrade_agent', {})
    logger.log_info(f"[agent-upgrade] queued job_id={job_id} agent_id={aid} name={row.get('name')}")
    return HandlerResult(status=202,
                         body={"ok": True, "agent_id": aid, "job_id": job_id,
                               "hint": "agent 가 다음 heartbeat 에서 pickup 후 재시작됩니다 (수 초 내)"},
                         media_type="application/json")


async def _restart_agent(handler_args: HandlerArgs, aid: int, config):
    """Agent 자체 self-restart job 큐잉. agent 가 heartbeat 로 pickup → execv 로 self-exec.
    바이너리 다운로드/교체 없이 현재 image 그대로 재시작. die 한 agent 는 깨울 수 없음 (외부 supervisor 필요)."""
    row = await asyncio.to_thread(_agent_load, config, aid)
    if not row:
        return HandlerResult(status=404, body={"error": "agent_not_found"},
                             media_type="application/json")
    if row.get("status") != "online":
        return HandlerResult(status=409,
                             body={"error": "agent_not_online",
                                   "hint": "die 한 agent 는 CSC 에서 깨울 수 없음 — 호스트에서 ./start.sh 또는 systemd unit 의 자동 부활 필요"},
                             media_type="application/json")
    job_id = await asyncio.to_thread(_job_create, config, aid, 'agent_restart', {})
    logger.log_info(f"[agent-restart] queued job_id={job_id} agent_id={aid} name={row.get('name')}")
    return HandlerResult(status=202,
                         body={"ok": True, "agent_id": aid, "job_id": job_id,
                               "hint": "agent 가 다음 heartbeat 에서 pickup 후 self-exec 합니다 (수 초 내)"},
                         media_type="application/json")


async def _rollback_agent_binary(handler_args: HandlerArgs, aid: int, config):
    """Agent 롤백 job 큐잉 — agent/current 를 직전(또는 body.version) 버전으로 flip 후 execv.
    버전 디렉토리는 prune(최신 3개)까지 보존되므로 다운로드 불요. die 한 agent 는 깨울 수 없음."""
    row = await asyncio.to_thread(_agent_load, config, aid)
    if not row:
        return HandlerResult(status=404, body={"error": "agent_not_found"},
                             media_type="application/json")
    if row.get("status") != "online":
        return HandlerResult(status=409,
                             body={"error": "agent_not_online",
                                   "hint": "오프라인 agent 는 롤백 job 을 pickup 하지 못함 — 온라인 복귀 후 재시도"},
                             media_type="application/json")
    body = _parse_body(handler_args)
    params = {}
    ver = (body.get("version") or "").strip()
    if ver:
        params["version"] = ver
    job_id = await asyncio.to_thread(_job_create, config, aid, 'rollback_agent', params)
    logger.log_info(f"[agent-rollback] queued job_id={job_id} agent_id={aid} "
                    f"name={row.get('name')} target={ver or '직전'}")
    return HandlerResult(status=202,
                         body={"ok": True, "agent_id": aid, "job_id": job_id,
                               "target_version": ver or None,
                               "hint": "agent 가 다음 heartbeat 에서 pickup 후 current flip + self-exec 합니다 (수 초 내)"},
                         media_type="application/json")


async def _agent_health_check(handler_args: HandlerArgs, aid: int, config):
    """admin → agent sync REST /health-check 프록시. body 에 scope=ha|modules|all 지정 가능.
    agent 가 offline 또는 sync_port 미보고면 502."""
    body = _parse_body(handler_args) or {}
    scope = body.get("scope") or "all"
    if scope not in ("ha", "modules", "all"):
        return HandlerResult(status=400, body={"error": "invalid_scope",
                              "hint": "scope must be one of: ha, modules, all"},
                             media_type="application/json")
    agent = await asyncio.to_thread(_agent_load, config, aid=aid)
    if not agent:
        return HandlerResult(status=404, body={"error": "agent_not_found"},
                             media_type="application/json")
    if agent.get("status") != "online":
        return HandlerResult(status=409,
                             body={"error": "agent_not_online", "status": agent.get("status"),
                                   "hint": "agent 가 online 이 아니면 sync REST 호출 불가"},
                             media_type="application/json")
    status, b = await asyncio.to_thread(_agent_proxy_call, "GET", agent,
                                         "/health-check", {"scope": scope},
                                         None, 10, config)
    if status == 200:
        return HandlerResult(status=200, body=b, media_type="application/json")
    return HandlerResult(status=502 if status == 0 else status,
                         body={"error": "proxy_failed", "detail": b},
                         media_type="application/json")


async def _agent_metrics(aid: int, config):
    rows = await asyncio.to_thread(_metric_load_recent, config, aid, 120, 7)
    def _row(r):
        procs = r.get("processes")
        if isinstance(procs, str):
            try: procs = json.loads(procs)
            except Exception: procs = []
        elif not isinstance(procs, list):
            procs = []
        per_iface = r.get("per_iface")
        if not isinstance(per_iface, list):
            per_iface = []
        mounts = r.get("mounts")
        if not isinstance(mounts, list):
            mounts = []
        return {
            "ts": r.get("ts"),
            "cpu_pct": r.get("cpu_pct"),
            "mem_pct": r.get("mem_pct"),
            "disk_pct": r.get("disk_pct"),
            "load_avg": r.get("load_avg"),
            "processes": procs,
            "per_iface": per_iface,
            "mounts": mounts,
        }
    return HandlerResult(status=200, body={"items": [_row(r) for r in rows]},
                         media_type="application/json")


# ════════════════════════════════════════════════════════════
#  Packages
# ════════════════════════════════════════════════════════════

def _package_to_json(r: dict, include_full: bool = True) -> dict:
    """Package row(file_store dict 또는 legacy DB row) → JSON.

    include_full=True (default): meta / config_template 도 함께 반환.
      - 리스트 조회도 추가 모달에서 바로 써야 하므로 기본 포함.
      - 필요 시 include_full=False 로 최소 필드만 반환.
    """
    ua = r.get("uploaded_at")
    if hasattr(ua, "isoformat"):
        ua = ua.isoformat()
    out = {
        "id": r.get("id"),
        "name": r.get("name"),
        "version": r.get("version"),
        "file_path": r.get("file_path"),
        "file_size": r.get("file_size"),
        "sha256": r.get("sha256"),
        "description": r.get("description"),
        "uploaded_by": r.get("uploaded_by"),
        "uploaded_at": ua,
    }
    if include_full:
        # file_store: 'meta'/'config_template' 가 이미 dict.
        # legacy DB: 'meta_json'/'config_template_json' 이 JSON 문자열 — _safe_json 으로 정상화.
        out["meta"] = r.get("meta") if isinstance(r.get("meta"), (dict, list)) \
                      else _safe_json(r.get("meta_json"))
        out["config_template"] = r.get("config_template") if isinstance(r.get("config_template"), (dict, list)) \
                                 else _safe_json(r.get("config_template_json"))
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

    deny = _console_rbac(handler_args)
    if deny: return deny

    if not tail:
        if method == "GET":  return await _list_packages(config)
        if method == "POST": return await _create_package(handler_args, config)
    else:
        # 비-숫자 액션 분기 (예: register-from-dist)
        if tail[0] == 'register-from-dist' and method == 'POST':
            return await _register_packages_from_dist(handler_args, config)
        try: pid = int(tail[0])
        except (TypeError, ValueError):
            return HandlerResult(status=400, body={"error": "invalid_id"}, media_type="application/json")
        if method == "GET":    return await _get_package(pid, config)
        if method == "PUT":    return await _update_package(handler_args, pid, config)
        if method == "DELETE": return await _delete_package(pid, config)
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


async def _register_packages_from_dist(handler_args: HandlerArgs, config):
    """DEV 전용 — build/dist/packages/*.tar.gz 를 일괄 file_store 등록.

    상용 환경 (Server.DevMode=false) 에서는 403. 정식 흐름은 multipart 업로드 (_create_package).
    호출 시점: /release/package 의 ▶ 빌드 & 패키징 완료 후 자동.
    """
    if not _is_dev_mode(config):
        return HandlerResult(status=403, body={
            "error": "dev_only",
            "hint": "이 endpoint 는 Server.DevMode=true (개발 환경) 전용입니다. 상용은 ＋ 패키지 업로드 사용.",
        }, media_type="application/json")

    # build/dist/packages 디렉토리 — _COMPONENT_ROOT 가 .../build/dist/csc/ 이므로 sibling
    pkg_root = os.environ.get("CIMS_BUILD_PKG_DIR") or \
        os.path.normpath(os.path.join(_COMPONENT_ROOT, "..", "packages"))
    if not os.path.isdir(pkg_root):
        return HandlerResult(status=404, body={
            "error": "build_pkg_dir_not_found",
            "path": pkg_root,
            "hint": "build/dist/packages/ 가 없습니다 — cims.sh pkg 먼저 실행 필요",
        }, media_type="application/json")

    actor = _actor(handler_args)
    registered = []
    errors = []

    for fname in sorted(os.listdir(pkg_root)):
        if not fname.endswith(".tar.gz"):
            continue
        src = os.path.join(pkg_root, fname)
        try:
            raw = await asyncio.to_thread(_read_file, src)
            meta = await asyncio.to_thread(_extract_meta_from_tarball, raw) or {}
            template = await asyncio.to_thread(_extract_config_template_from_tarball, raw)
            name = (meta.get("name") or "").strip()
            version = (meta.get("version") or "").strip()
            if not name or not version:
                errors.append({"file": fname, "error": "meta.json missing name/version"})
                continue

            # 영구 저장 경로 — _create_package 와 동일 (Packages.Dir)
            pkg_dir, _ = _resolve_pkg_paths(config)
            os.makedirs(pkg_dir, exist_ok=True)
            dest = os.path.join(pkg_dir, f"{name}-{version}.tar.gz")
            fsha, fsize = await asyncio.to_thread(_write_and_hash, dest, raw)

            description = (meta.get("description") or "")
            desc_lines = [description] if description else []
            if meta.get("build_date"): desc_lines.append(f"build: {meta['build_date']}")
            if meta.get("git_sha"):
                gb = meta.get("git_branch")
                desc_lines.append(f"git: {meta['git_sha']}" + (f" ({gb})" if gb else ""))
            full_desc = " | ".join(desc_lines)[:255]

            row = await asyncio.to_thread(
                _pkg_upsert, config, name, version, dest, fsize, fsha, full_desc, actor,
                meta or None, template or None,
            )
            registered.append({"name": name, "version": version, "id": row.get("id")})
        except Exception as e:
            errors.append({"file": fname, "error": str(e)})

    logger.log_info(f"[pkg-register-from-dist] registered={len(registered)} errors={len(errors)} src={pkg_root}")
    return HandlerResult(status=200, body={
        "count": len(registered),
        "registered": registered,
        "errors": errors,
        "source_dir": pkg_root,
    }, media_type="application/json")


async def _list_packages(config):
    """업로드된 tarball 만 반환 (정식 배포 흐름). 개발용 build/dist 자동 스캔은 제거됨."""
    rows = await asyncio.to_thread(_pkg_load_all, config)
    items = [_package_to_json(r) for r in rows]
    items.sort(key=lambda x: (x.get("name", ""), -int(x.get("id", 0) or 0)))
    return HandlerResult(status=200, body={"items": items}, media_type="application/json")


async def _get_package(pid: int, config):
    r = await asyncio.to_thread(_pkg_load, config, pid)
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
    return _pkg_load(config, name=name, version=version)


def _pkg_upsert(config, name: str, version: str, fpath: str, fsize: int,
                fsha: str, full_desc: str, actor: str,
                meta: dict | None, template: dict | None):
    """파일 기반 upsert. 같은 (name, version) 이 있으면 id 보존, 없으면 next_id 할당."""
    from datetime import datetime
    d = _pkg_dir(config)
    existing = _pkg_load(config, name=name, version=version)
    pid = existing.get('id') if existing else file_store.next_id(d)
    now = datetime.now().isoformat(timespec='seconds')
    row = {
        'id': pid,
        'name': name,
        'version': version,
        'file_path': fpath,
        'file_size': fsize,
        'sha256': fsha,
        'description': full_desc,
        'uploaded_by': actor,
        'uploaded_at': now,
        'meta': meta,
        'config_template': template,
    }
    file_store.save(d, _pkg_key(name, version), row)
    return row


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
    _warn_missing_scope(template, name or "(unknown)")
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

    # 6) file_store upsert (blocking → thread)
    row = await asyncio.to_thread(
        _pkg_upsert, config, name, version, fpath, fsize, fsha, full_desc, actor,
        meta or None, template or None,
    )

    result = _package_to_json(row)
    logger.log_info(f"[pkg-upload] done {name} {version} size={fsize} "
                    f"template={'yes' if template else 'no'} "
                    f"handler_ms={int((_t.monotonic()-_t_handler_start)*1000)}")
    return HandlerResult(status=201, body=result, media_type="application/json")


def seed_packages_from_dir(config: dict, seed_dir: str) -> int:
    """시드 디렉토리의 *.tar.gz 를 file_store 패키지로 멱등 등록 (동기, startup 용).

    부트스트랩 인스톨러가 oam/console/agent/csc tarball 을 seed_packages/ 에
    떨궈 두면 OAM 첫 부팅 시 자동 등록 — /install-agent.sh, /agent-bundle.tar.gz
    (file_store 의 agent 패키지 필요)와 콘솔의 패키지 목록이 즉시 동작한다.
    (name, version) 이 이미 있으면 건너뜀. 등록 건수 반환.
    """
    import glob as _glob
    if not seed_dir or not os.path.isdir(seed_dir):
        return 0
    n = 0
    for src in sorted(_glob.glob(os.path.join(seed_dir, '*.tar.gz'))):
        try:
            raw = _read_file(src)
            meta = _extract_meta_from_tarball(raw) or {}
            name = (meta.get('name') or '').strip()
            version = (meta.get('version') or '').strip()
            if not name or not version:
                logger.log_error(f'[pkg-seed] skip {os.path.basename(src)} — meta.json name/version 없음')
                continue
            if _pkg_existing(config, name, version):
                continue
            template = _extract_config_template_from_tarball(raw)
            pkg_dir, _bak = _resolve_pkg_paths(config)
            os.makedirs(pkg_dir, exist_ok=True)
            fpath = os.path.join(pkg_dir, f'{name}-{version}.tar.gz')
            fsha, fsize = _write_and_hash(fpath, raw)
            desc_lines = [x for x in (
                meta.get('description'),
                f"build: {meta['build_date']}" if meta.get('build_date') else '',
                f"git: {meta['git_sha']}" if meta.get('git_sha') else '',
            ) if x]
            _pkg_upsert(config, name, version, fpath, fsize, fsha,
                        ' | '.join(desc_lines)[:255], 'seed',
                        meta or None, template or None)
            logger.log_info(f'[pkg-seed] registered {name} {version} ({fsize} bytes)')
            n += 1
        except Exception as e:
            logger.log_error(f'[pkg-seed] {os.path.basename(src)} 실패: {e}')
    return n


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

    patches: dict = {}
    if "description" in body:
        desc = body.get("description")
        if desc is not None and not isinstance(desc, str):
            return HandlerResult(status=400, body={"error": "description_must_be_string"}, media_type="application/json")
        patches['description'] = desc
    if "config_template" in body:
        tmpl = body.get("config_template")
        if tmpl is not None and not isinstance(tmpl, dict):
            return HandlerResult(status=400, body={"error": "config_template_must_be_object"}, media_type="application/json")
        patches['config_template'] = tmpl

    if not patches:
        return HandlerResult(status=400, body={"error": "nothing_to_update"}, media_type="application/json")

    def _run_update():
        existing = _pkg_load(config, pid=pid)
        if not existing:
            return False
        existing.update(patches)
        file_store.save(_pkg_dir(config),
                        _pkg_key(existing['name'], existing['version']),
                        existing)
        return True

    ok = await asyncio.to_thread(_run_update)
    if not ok:
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
    """file_store 에서 패키지 1건 조회 + 삭제. 삭제된 row dict 반환 (없으면 None)."""
    r = _pkg_load(config, pid=pid)
    if not r:
        return None
    file_store.delete(_pkg_dir(config), _pkg_key(r['name'], r['version']))
    return r


# ════════════════════════════════════════════════════════════
#  Deployments
# ════════════════════════════════════════════════════════════

def _deployment_to_json(r: dict) -> dict:
    def _maybe_dt(v):
        if v is None: return None
        if hasattr(v, 'isoformat'): return v.isoformat()
        return v
    # service_functions: file_store 는 list, 옛 DB 는 CSV 문자열
    sf = r.get("service_functions")
    if isinstance(sf, list):
        sf_list = [x for x in sf if x]
    else:
        sf_list = _split_csv(sf)
    # config: file_store 는 dict, 옛 DB 는 config_json (string)
    cfg = r.get("config")
    if not isinstance(cfg, (dict, list)):
        cfg = _safe_json(r.get("config_json"))
    return {
        "id": r.get("id"),
        "agent_id":     r.get("agent_id"),
        "agent_name":   r.get("agent_name"),
        "package_id":   r.get("package_id"),
        "package_name": r.get("package_name"),
        "package_version": r.get("package_version"),
        "process_name": r.get("process_name"),
        "service_functions": sf_list,
        "status":       r.get("status"),
        "live_state":   r.get("live_state"),
        "install_path": r.get("install_path"),
        "prev_install_path": r.get("prev_install_path"),
        "prev_package_version": r.get("prev_package_version"),
        "install_history": r.get("install_history") if isinstance(r.get("install_history"), list) else [],
        "deployed_at":  _maybe_dt(r.get("deployed_at")),
        "last_job_id":  r.get("last_job_id"),
        "note":         r.get("note"),
        "config":       cfg,
        "config_applied_at": _maybe_dt(r.get("config_applied_at")),
        "create_time":  _maybe_dt(r.get("create_time")),
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

    # 패키지 설정(탭3: config/collection/sync) = operator+ (read 도 — 설정값에 민감정보).
    # 그 외 변이(설치/잡/롤백/레코드) = admin.
    if len(tail) >= 2 and tail[1] in ("config", "collection", "sync"):
        deny = _console_rbac(handler_args, read_role="operator", write_role="operator")
    else:
        deny = _console_rbac(handler_args)
    if deny: return deny

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
        elif len(tail) == 2 and tail[1] == "rollback" and method == "POST":
            return await _rollback_deployment(handler_args, did, config)
        elif len(tail) == 2 and tail[1] == "config":
            if method == "GET":  return await _get_deployment_config(did, config)
            if method == "PUT":  return await _put_deployment_config(handler_args, did, config)
        elif len(tail) == 2 and tail[1] == "sync" and method == "POST":
            return await _sync_deployment_config(handler_args, did, config)
        elif len(tail) == 3 and tail[1] == "collection":
            name = tail[2]
            if method == "GET":  return await _get_deployment_collection(did, name, config)
            if method == "PUT":  return await _put_deployment_collection(handler_args, did, name, config)
    return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")


def _ha_group_for_deployment(config, dep: dict, pkg_name: str, strict: bool = False):
    """dep.agent_id 가 멤버로 소속된, pkg_name 을 호스팅하는 ha_group.

    동일 패키지를 여러 그룹이 호스팅해도 요청 dep 이 속한 그룹으로만 한정
    (오전파 방지). strict=False 면 멤버십 매치 실패 시 첫 매치 fallback
    (레거시 ha_group_for_package 동작 보존), strict=True 면 None."""
    from services import ha_lookup
    groups = ha_lookup.ha_groups_for_package(config, pkg_name)
    if not groups:
        return None
    aid = dep.get("agent_id")
    for g in groups:
        if any(m.get("agent_id") == aid for m in ha_lookup.members_of(g)):
            return g
    return None if strict else groups[0]


async def _get_deployment_config(did: int, config):
    """해당 배포의 현재 설정 값 + 참조 템플릿을 함께 반환.

    ha block: dep 이 소속된 ha_group 이 이 패키지를 호스팅하면
    {group_id, group_name, mode, members[]}. 소속 그룹 없으면(standalone) null —
    콘솔이 그룹 컨텍스트(공통/개별 탭 안내·설정 비교 링크) 노출 판단에 사용."""
    from services import ha_lookup

    r = await asyncio.to_thread(_deploy_load, config, did)
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    pkg = _pkg_load(config, pid=r.get("package_id")) or {}
    cfg = r.get("config")
    if not isinstance(cfg, (dict, list)):
        cfg = _safe_json(r.get("config_json"))
    ca = r.get("config_applied_at")
    if hasattr(ca, "isoformat"):
        ca = ca.isoformat()

    ha_block = None
    pkg_name = pkg.get("name")
    if pkg_name:
        g = await asyncio.to_thread(_ha_group_for_deployment, config, r, pkg_name, True)
        if g and g.get("id") is not None:
            member_rows = await asyncio.to_thread(
                ha_lookup.deployments_in_group_for_package, config, g["id"], pkg_name)
            _enrich_deploy(member_rows, config)   # agent_name + package_version (버전 가드 표시용)
            ha_block = {
                "group_id":   g.get("id"),
                "group_name": g.get("name"),
                "mode":       g.get("mode"),
                "members":    [{"deployment_id":   m.get("id"),
                                "agent_id":        m.get("agent_id"),
                                "agent_name":      m.get("agent_name"),
                                "package_version": m.get("package_version")} for m in member_rows],
            }

    return HandlerResult(status=200,
        body={
            "config":             cfg or {},
            "config_applied_at":  ca,
            "template":           pkg.get("config_template"),
            "meta":               pkg.get("meta"),
            "ha":                 ha_block,
        },
        media_type="application/json")


async def _put_deployment_config(handler_args, did: int, config):
    """설정 값 저장 — 항상 해당 deployment 에만. body = {
         "config":        {<key>: <value>, ...},   # 이 dep 의 새 overlay 전체
         "queue_update"?: bool (기본 true),
       }

    저장은 단일 deployment 대상 — HA 그룹 전파 없음. 멤버 간 정합은 그룹 동기화
    (POST /deployments/{id}/sync — 콘솔 그룹 [설정 비교] 뷰의 명시적 실행)로만
    맞춘다. 구 body 필드(propagate_to_ha_peers/sync_keys/sync_checked)는 어떤
    값이 오더라도 무시되며 피어에는 절대 쓰지 않는다.
    queue_update=true 이면 update_config job 1건 enqueue.
    """
    body = _parse_body(handler_args)
    values = body.get("config")
    if not isinstance(values, dict):
        return HandlerResult(status=400, body={"error": "config dict required"},
                             media_type="application/json")
    queue_update = body.get("queue_update", True)

    dep = await asyncio.to_thread(_deploy_load, config, did)
    if not dep:
        return HandlerResult(status=404, body={"error": "not_found"},
                             media_type="application/json")
    _enrich_deploy([dep], config)

    # string_list/ref_list 필드가 콤마 문자열로 오면 배열로 정규화(백엔드 coerce).
    #   프론트 위젯 누락·raw API 우회 시에도 config.json 에 항상 배열로 저장되게 하는
    #   최종 방어. (예: MediaServer.Endpoints "a:9000, b:9000" → ["a:9000","b:9000"])
    _pkg = None
    try:
        _pkg = await asyncio.to_thread(_pkg_load, config, dep.get("package_id"))
        _tmpl = (_pkg or {}).get("config_template") if isinstance(_pkg, dict) else None
        if isinstance(_tmpl, dict):
            values = _coerce_list_fields(_tmpl, values)
    except Exception as _e:
        logger.log_warning(f"deployment config list-coerce skip: {_e}")

    updated = await asyncio.to_thread(_deploy_update, config, dep["id"], {"config": values})
    if not updated:
        return HandlerResult(status=500, body={"error": "save_failed"},
                             media_type="application/json")

    # ── update_config job enqueue
    job_id = None
    members_resp: list[dict] = []
    if queue_update:
        # _deploy_update 반환 = raw 레코드 (package_name/version 은 패키지 join 필드라
        # 없음) → agent 의 pkg_subdir 해석(overlay 를 <pkg>/config.json 에 기록)에
        # 필요하므로 재-enrich. 누락 시 overlay 가 install_path 루트에 쓰여 CSP 의
        # SIGUSR1 즉시반영(_findDeploymentConfig = csp.json 부모×2)이 읽지 못한다.
        _enrich_deploy([updated], config)
        sf = updated.get("service_functions")
        if isinstance(sf, str):
            sf = _split_csv(sf)
        # 레코드는 sparse overlay 그대로, agent 로 나가는 job config 만 실체화 —
        #   template default + base 공유값 병합으로 config.json 을 완전한 유효설정으로.
        params = {
            "deployment_id":   updated["id"],
            "package_id":      updated.get("package_id"),
            "package_name":    updated.get("package_name"),
            "package_version": updated.get("package_version"),
            "process_name":    updated.get("process_name"),
            "service_functions": sf or [],
            "install_path":    updated.get("install_path"),
            "config":          _materialize_deploy_config(config, _pkg, updated.get("config")),
        }
        job_id = await asyncio.to_thread(_job_create, config, updated["agent_id"],
                                         "update_config", params)
        members_resp.append({"deployment_id": updated["id"],
                             "agent_id": updated.get("agent_id"), "job_id": job_id})

        # ── 실효 포트/게이트웨이 host 변경 전파 — 게이트웨이 라우트 재등록 + HA 재렌더.
        #   라우트는 배포 생성 시 1회 등록이라 Server.Port/Server.GatewayHost 변경 시
        #   여기서 재등록하지 않으면 프록시가 구 upstream 을 계속 본다. ha.json
        #   헬스포트는 포트 변경일 때만 재렌더 (host 는 HA 무관).
        #   (전파 실패는 저장 성공에 영향 없음.)
        try:
            old_port = effective_server_port(config, _pkg, dep.get("config"))
            new_port = effective_server_port(config, _pkg, updated.get("config"))
            old_host = effective_gateway_host(config, _pkg, dep.get("config")) or "127.0.0.1"
            new_host = effective_gateway_host(config, _pkg, updated.get("config")) or "127.0.0.1"
            if new_port and (new_port != old_port or new_host != old_host):
                _meta = (_pkg or {}).get("meta") if isinstance(_pkg, dict) else None
                gw_routes = ((_meta or {}).get("gateway") or {}).get("routes") or []
                if gw_routes and updated.get("process_name"):
                    import handlers.gateway as _gw
                    await asyncio.to_thread(_gw.register_module_routes, config,
                                            updated["process_name"], new_host,
                                            int(new_port), gw_routes)
                n = 0
                if new_port != old_port:
                    from handlers.ha_groups import enqueue_update_ha_for_agent
                    n = await asyncio.to_thread(enqueue_update_ha_for_agent,
                                                updated.get("agent_id"), config)
                logger.log_info(f"[deploy-config] dep={updated['id']} 실효 upstream "
                                f"{old_host}:{old_port}->{new_host}:{new_port}: gateway 재등록"
                                f"={'O' if gw_routes else 'X'}, update_ha {n}건")
        except Exception as e:
            logger.log_warning(f"[deploy-config] upstream 변경 전파 실패(dep={did}): {e}")

    return HandlerResult(status=200,
        body={
            "ok":      True,
            "job_id":  job_id,
            "members": members_resp,
        },
        media_type="application/json")


def _effective_scope(field: dict, section_scope) -> str:
    """필드 유효 scope — field.scope 가 섹션 scope 를 오버라이드. 기본 service.

    섹션 안에 공통값·노드별 값이 섞인 경우(예: csp media_server 의 LocalIp)를
    필드 단위로 표현하기 위한 규칙. 콘솔(effectiveScope)과 동일해야 한다."""
    s = field.get("scope") or section_scope or "service"
    return str(s).lower()


def _service_scope_keys(template) -> set:
    """유효 scope=service 인 필드 키 집합 — 그룹 동기화 복사 마스크.
    scope=system 필드(바인드 IP·노드 식별자 등)는 동기화로 절대 복사되지 않는다."""
    out: set = set()
    if not isinstance(template, dict):
        return out
    for s in template.get("sections") or []:
        sec_scope = s.get("scope")
        for f in s.get("fields") or []:
            if f.get("key") and _effective_scope(f, sec_scope) == "service":
                out.add(f["key"])
    return out


def _service_scope_collections(template) -> set:
    """scope=service 인 컬렉션 key 집합 — 그룹 동기화 복사 허용 컬렉션."""
    out: set = set()
    if not isinstance(template, dict):
        return out
    for c in template.get("collections") or []:
        if c.get("key") and str(c.get("scope") or "service").lower() == "service":
            out.add(c["key"])
    return out


async def _sync_deployment_config(handler_args, did: int, config):
    """그룹 설정 동기화 — 명시적 방향성 복사 (source=이 deployment → targets).

    body = {
      "targets":       [<deployment_id>, ...],  # 같은 HA 그룹·같은 패키지·같은 버전
      "keys"?:         [<key>, ...],            # 복사할 scalar 키 — 유효 scope=service 만
      "collections"?:  [<name>, ...],           # 복사할 컬렉션 — scope=service 만
      "queue_update"?: bool (기본 true),
    }

    설정 저장(PUT config/collection)은 단일 서버 대상이며, 멤버 간 정합은 이
    엔드포인트의 명시적 실행(콘솔 그룹 [설정 비교] 뷰 [동기화])으로만 맞춘다.
      - scalar: source overlay 에 있는 키는 값을 target overlay 에 merge,
        source overlay 에 없는 키는 target overlay 에서 제거(템플릿 기본값 복귀)
        → 유효값이 source 와 정확히 일치.
      - 버전 가드: package_version 불일치 target 이 있으면 409 — 롤링 업그레이드
        혼재 구간의 오동기화 차단.
      - scope=system 키/컬렉션은 요청에 있어도 복사하지 않고 skipped 로 보고.
    """
    from services import ha_lookup, sync_txn

    body = _parse_body(handler_args)
    target_ids = body.get("targets")
    keys = body.get("keys") or []
    coll_names = body.get("collections") or []
    queue_update = body.get("queue_update", True)
    if not isinstance(target_ids, list) or not target_ids:
        return HandlerResult(status=400, body={"error": "targets list required"},
                             media_type="application/json")
    if not isinstance(keys, list) or not isinstance(coll_names, list):
        return HandlerResult(status=400, body={"error": "keys/collections must be lists"},
                             media_type="application/json")
    if not keys and not coll_names:
        return HandlerResult(status=400, body={"error": "keys or collections required"},
                             media_type="application/json")

    src = await asyncio.to_thread(_deploy_load, config, did)
    if not src:
        return HandlerResult(status=404, body={"error": "not_found"},
                             media_type="application/json")
    _enrich_deploy([src], config)
    pkg_name = src.get("package_name")
    _pkg = await asyncio.to_thread(_pkg_load, config, src.get("package_id")) or {}
    template = _pkg.get("config_template") if isinstance(_pkg, dict) else None

    # ── 멤버십 가드: source 가 멤버로 소속된 그룹의 같은-패키지 deployment 만 target 허용
    g = await asyncio.to_thread(_ha_group_for_deployment, config, src, pkg_name, True)
    if not g or g.get("id") is None:
        return HandlerResult(status=409, body={"error": "not_in_ha_group"},
                             media_type="application/json")
    ha_group_id = g.get("id")
    member_rows = await asyncio.to_thread(
        ha_lookup.deployments_in_group_for_package, config, ha_group_id, pkg_name)
    member_ids = {m.get("id") for m in member_rows}

    targets: list[dict] = []
    for tid in target_ids:
        try:
            tid = int(tid)
        except (TypeError, ValueError):
            return HandlerResult(status=400, body={"error": "invalid_target", "target": tid},
                                 media_type="application/json")
        if tid == src["id"]:
            continue   # 자기 자신은 대상에서 제외
        if tid not in member_ids:
            return HandlerResult(status=409,
                body={"error": "target_not_in_group", "deployment_id": tid},
                media_type="application/json")
        t = await asyncio.to_thread(_deploy_load, config, tid)
        if not t:
            return HandlerResult(status=404,
                body={"error": "target_not_found", "deployment_id": tid},
                media_type="application/json")
        targets.append(t)
    if not targets:
        return HandlerResult(status=400, body={"error": "no_valid_targets"},
                             media_type="application/json")
    _enrich_deploy(targets, config)

    # ── 버전 가드 — 롤링 업그레이드 혼재 구간 오동기화 차단
    src_ver = src.get("package_version")
    mismatched = [{"deployment_id": t["id"], "package_version": t.get("package_version")}
                  for t in targets if t.get("package_version") != src_ver]
    if mismatched:
        return HandlerResult(status=409,
            body={"error": "version_mismatch", "source_version": src_ver,
                  "targets": mismatched},
            media_type="application/json")

    # ── scalar 복사 — 유효 scope=service 키만 (system 키는 skipped 보고)
    allowed = _service_scope_keys(template)
    apply_keys = [k for k in keys if k in allowed]
    skipped_keys = sorted(set(keys) - set(apply_keys))
    src_overlay = src.get("config")
    if not isinstance(src_overlay, dict):
        src_overlay = _safe_json(src.get("config_json")) or {}
    applied_keys = sorted(k for k in apply_keys if k in src_overlay)
    removed_keys = sorted(k for k in apply_keys if k not in src_overlay)

    saved: list[dict] = []
    if apply_keys:
        for t in targets:
            cur = t.get("config")
            if not isinstance(cur, dict):
                cur = _safe_json(t.get("config_json")) or {}
            new_overlay = dict(cur)
            for k in apply_keys:
                if k in src_overlay:
                    new_overlay[k] = src_overlay[k]
                else:
                    new_overlay.pop(k, None)
            updated = await asyncio.to_thread(_deploy_update, config, t["id"],
                                              {"config": new_overlay})
            if updated:
                saved.append(updated)

    # ── update_config job enqueue + sync_txn (scalar 대상만 — 컬렉션은 proxy 동기호출)
    sync_id = None
    members_resp: list[dict] = []
    if queue_update and saved:
        _enrich_deploy(saved, config)
        member_jobs: list[dict] = []
        for t in saved:
            sf = t.get("service_functions")
            if isinstance(sf, str):
                sf = _split_csv(sf)
            params = {
                "deployment_id":   t["id"],
                "package_id":      t.get("package_id"),
                "package_name":    t.get("package_name"),
                "package_version": t.get("package_version"),
                "process_name":    t.get("process_name"),
                "service_functions": sf or [],
                "install_path":    t.get("install_path"),
                "config":          _materialize_deploy_config(config, _pkg, t.get("config")),
            }
            jid = await asyncio.to_thread(_job_create, config, t["agent_id"],
                                          "update_config", params)
            member_jobs.append({"agent_id": t["agent_id"],
                                "deployment_id": t["id"], "job_id": jid})
            members_resp.append({"deployment_id": t["id"],
                                 "agent_id": t["agent_id"], "job_id": jid})
        if member_jobs:
            txn = await asyncio.to_thread(sync_txn.create, config,
                                          collection="config",
                                          op="group_sync",
                                          members=member_jobs,
                                          actor="console",
                                          ttl_sec=120,
                                          note=f"src_deployment#{did} ha_group#{ha_group_id}")
            sync_id = txn["id"]
            for m in member_jobs:
                j = await asyncio.to_thread(_job_load, config, m["job_id"])
                if not j:
                    continue
                p = j.get("params") or {}
                p["sync_id"] = sync_id
                await asyncio.to_thread(_job_update, config, m["job_id"], {"params": p})

    # ── 컬렉션 복사 — source agent 에서 records GET → target agent 들에 PUT (동기 proxy)
    coll_results: list[dict] = []
    colls_ok = True
    if coll_names:
        allowed_colls = _service_scope_collections(template)
        src_full = await asyncio.to_thread(_fetch_deployment_for_proxy, did, config)
        target_fulls = []
        for t in targets:
            tf = await asyncio.to_thread(_fetch_deployment_for_proxy, t["id"], config)
            if tf and tf.get("install_path"):
                target_fulls.append(tf)
        if not src_full or not src_full.get("install_path"):
            return HandlerResult(status=409, body={"error": "source_not_installed"},
                                 media_type="application/json")
        for name in coll_names:
            if name not in allowed_colls:
                coll_results.append({"name": name, "ok": False,
                                     "skipped": "scope_not_service"})
                continue
            status, resp = await asyncio.to_thread(
                _agent_proxy_call, "GET", src_full,
                "/collection", {"install_path": src_full["install_path"], "name": name},
                None, 15, config)
            if status != 200:
                coll_results.append({"name": name, "ok": False, "error": resp})
                colls_ok = False
                continue
            records = (resp or {}).get("records") or []
            peers = []
            all_ok = True
            for tf in target_fulls:
                st, rp = await asyncio.to_thread(
                    _agent_proxy_call, "PUT", tf,
                    "/collection", {"install_path": tf["install_path"], "name": name},
                    {"records": records, "signal": True}, 15, config)
                ok = (st == 200)
                peers.append({"deployment_id": tf["id"], "agent_id": tf.get("agent_id"),
                              "status": st, "ok": ok,
                              "error": None if ok else rp})
                if not ok:
                    all_ok = False
            coll_results.append({"name": name, "ok": all_ok,
                                 "count": len(records), "peers": peers})
            if not all_ok:
                colls_ok = False

    return HandlerResult(status=200 if colls_ok else 502,
        body={
            "ok":                   colls_ok,
            "source_deployment_id": src["id"],
            "ha_group_id":          ha_group_id,
            "applied_keys":         applied_keys,
            "removed_keys":         removed_keys,
            "skipped_keys":         skipped_keys,
            "members":              members_resp,
            "collections":          coll_results,
            "sync_id":              sync_id,
        },
        media_type="application/json")


def _enqueue_update_config_jobs(config, deps: list, pkg_file, *, op: str,
                                actor: str, note: str) -> tuple:
    """저장된 deployment 들에 update_config job enqueue + sync_txn 생성 + sync_id
    backfill (그룹 저장/자동 교정 공용 — deps 는 _enrich_deploy 된 레코드, sync 함수).
    반환 (members[{deployment_id, agent_id, job_id}], sync_id|None)."""
    from services import sync_txn
    members: list[dict] = []
    for t in deps:
        sf = t.get("service_functions")
        if isinstance(sf, str):
            sf = _split_csv(sf)
        params = {
            "deployment_id":   t["id"],
            "package_id":      t.get("package_id"),
            "package_name":    t.get("package_name"),
            "package_version": t.get("package_version"),
            "process_name":    t.get("process_name"),
            "service_functions": sf or [],
            "install_path":    t.get("install_path"),
            "config":          _materialize_deploy_config(config, pkg_file, t.get("config")),
        }
        jid = _job_create(config, t["agent_id"], "update_config", params)
        members.append({"agent_id": t["agent_id"], "deployment_id": t["id"], "job_id": jid})
    sync_id = None
    if members:
        txn = sync_txn.create(config, collection="config", op=op, members=members,
                              actor=actor, ttl_sec=120, note=note)
        sync_id = txn["id"]
        for m in members:
            j = _job_load(config, m["job_id"])
            if not j:
                continue
            p = j.get("params") or {}
            p["sync_id"] = sync_id
            _job_update(config, m["job_id"], {"params": p})
    return members, sync_id


def reconcile_group_package(config, group: dict, pkg_name: str, *,
                            include_collections: bool = True,
                            actor: str = "auto-sync") -> dict:
    """AS 그룹×패키지 자동 정합 (R4 자동 교정 코어 — sync 함수, thread offload 권장).

    실측 ACTIVE(ha_lookup.vip_observation) 멤버를 기준으로 STANDBY 의 공통(service)
    설정을 맞춘다. 호출처: oam_app 의 auto-sync 스위퍼(주기), 스위치 ON 전환,
    upgrade/start/restart job 성공 훅.

    안전 원칙 — 애매하면 복사하지 않는다:
      - 스위치 OFF / AS 아님 → skip
      - ACTIVE 판정 불가(0명·2명 보유·전원 stale) → skip
      - 버전 불일치 target → deferred (버전이 같아지는 다음 호출에서 자동 정합)
    """
    from services import ha_lookup
    out = {"group_id": group.get("id"), "package": pkg_name,
           "status": "skipped", "reason": None,
           "active_agent_id": None, "synced_keys": [], "removed_keys": [],
           "collections": [], "deferred": [], "members": [], "sync_id": None}
    if group.get("mode") != "active_standby":
        out["reason"] = "not_active_standby"
        return out
    if not ha_lookup.auto_sync_enabled(group, pkg_name):
        out["reason"] = "switch_off"
        return out
    obs = ha_lookup.vip_observation(config, group)
    active_aid = obs["active_agent_id"]
    out["active_agent_id"] = active_aid
    if active_aid is None:
        out["reason"] = "active_unknown"
        return out

    deps = ha_lookup.deployments_in_group_for_package(config, group["id"], pkg_name)
    _enrich_deploy(deps, config)
    src = next((d for d in deps if d.get("agent_id") == active_aid), None)
    if not src:
        out["reason"] = "active_has_no_deployment"
        return out
    targets = [d for d in deps if d.get("id") != src.get("id")]
    if not targets:
        out["reason"] = "no_peers"
        return out
    src_ver = src.get("package_version")
    same_ver = [t for t in targets if t.get("package_version") == src_ver]
    out["deferred"] = [{"deployment_id": t["id"],
                        "package_version": t.get("package_version")}
                       for t in targets if t.get("package_version") != src_ver]
    if not same_ver:
        out["reason"] = "version_mismatch"
        return out

    _pkg = _pkg_load(config, src.get("package_id")) or {}
    template = _pkg.get("config_template") if isinstance(_pkg, dict) else None
    svc_keys = _service_scope_keys(template)
    src_overlay = src.get("config")
    if not isinstance(src_overlay, dict):
        src_overlay = _safe_json(src.get("config_json")) or {}

    # ── scalar 정합: ACTIVE overlay 의 service 키 기준 merge / 제거(기본값 복귀)
    saved: list[dict] = []
    synced_keys: set = set()
    removed_keys: set = set()
    for t in same_ver:
        cur = t.get("config")
        if not isinstance(cur, dict):
            cur = _safe_json(t.get("config_json")) or {}
        new_overlay = dict(cur)
        changed = False
        for k in svc_keys:
            if k in src_overlay:
                if new_overlay.get(k) != src_overlay[k]:
                    new_overlay[k] = src_overlay[k]
                    synced_keys.add(k)
                    changed = True
            elif k in new_overlay:
                new_overlay.pop(k)
                removed_keys.add(k)
                changed = True
        if changed:
            updated = _deploy_update(config, t["id"], {"config": new_overlay})
            if updated:
                saved.append(updated)
    out["synced_keys"] = sorted(synced_keys)
    out["removed_keys"] = sorted(removed_keys)
    if saved:
        _enrich_deploy(saved, config)
        members, sync_id = _enqueue_update_config_jobs(
            config, saved, _pkg, op="auto_sync", actor=actor,
            note=f"auto-sync ha_group#{group.get('id')} pkg={pkg_name} "
                 f"active=agent#{active_aid}")
        out["members"] = members
        out["sync_id"] = sync_id

    # ── 컬렉션 정합: scope=service 컬렉션 records 를 ACTIVE 기준으로 복사
    #    (hash 동일하면 PUT 생략 — 매 라운드 무해)
    colls_changed = False
    if include_collections:
        svc_colls = _service_scope_collections(template)
        if svc_colls:
            import hashlib
            def _rhash(recs):
                try:
                    return hashlib.sha256(json.dumps(recs or [], ensure_ascii=False,
                                                     sort_keys=True).encode()).hexdigest()[:12]
                except Exception:
                    return ""
            src_full = _fetch_deployment_for_proxy(src["id"], config)
            target_fulls = [tf for tf in
                            (_fetch_deployment_for_proxy(t["id"], config) for t in same_ver)
                            if tf and tf.get("install_path")]
            if src_full and src_full.get("install_path") and target_fulls:
                for name in sorted(svc_colls):
                    st, resp = _agent_proxy_call(
                        "GET", src_full, "/collection",
                        {"install_path": src_full["install_path"], "name": name},
                        None, 15, config)
                    if st != 200:
                        out["collections"].append({"name": name, "ok": False, "error": resp})
                        continue
                    records = (resp or {}).get("records") or []
                    src_hash = _rhash(records)
                    peers = []
                    for tf in target_fulls:
                        gst, gresp = _agent_proxy_call(
                            "GET", tf, "/collection",
                            {"install_path": tf["install_path"], "name": name},
                            None, 15, config)
                        if gst == 200 and _rhash((gresp or {}).get("records") or []) == src_hash:
                            continue   # 이미 정합
                        pst, presp = _agent_proxy_call(
                            "PUT", tf, "/collection",
                            {"install_path": tf["install_path"], "name": name},
                            {"records": records, "signal": True}, 15, config)
                        peers.append({"deployment_id": tf["id"], "agent_id": tf.get("agent_id"),
                                      "ok": pst == 200,
                                      "error": None if pst == 200 else presp})
                        if pst == 200:
                            colls_changed = True
                    if peers:
                        out["collections"].append({"name": name,
                                                   "ok": all(p["ok"] for p in peers),
                                                   "count": len(records), "peers": peers})

    out["status"] = "synced" if (saved or colls_changed) else "in_sync"
    return out


# _SELECT_DEPLOY 는 더 이상 사용하지 않음 (agent_deployment 가 file_store 로 이전됨).
# 옛 _fetch_deployment_for_proxy 만 유일하게 deployment 일부 컬럼이 필요해 별도 처리.


def _enrich_deploy(rows, config):
    """deployment rows 에 package / agent 정보를 file_store 에서 enrich.

    추가 필드: package_name / package_version / agent_name.
    """
    if not rows:
        return rows
    pkg_cache: dict = {}
    agent_cache: dict = {}
    for r in rows:
        pid = r.get('package_id')
        if pid is not None:
            if pid not in pkg_cache:
                pkg_cache[pid] = _pkg_load(config, pid=pid) or {}
            r['package_name'] = pkg_cache[pid].get('name')
            r['package_version'] = pkg_cache[pid].get('version')
        aid = r.get('agent_id')
        if aid is not None:
            if aid not in agent_cache:
                agent_cache[aid] = _agent_load(config, aid=aid) or {}
            ag = agent_cache[aid]
            r['agent_name'] = ag.get('name')
            # 실측 프로세스 상태 — agent metric 의 live_modules 스냅샷과 대조.
            # status(배포기록=의도)와 달리 실제 프로세스 생존을 반영 (metric 주기 지연).
            # online + 보고 있음 + 설치됨(비 pending) 일 때만 판정, 그 외 None(모름).
            lm = ag.get('live_modules')
            if (ag.get('status') == 'online' and isinstance(lm, list)
                    and r.get('status') in ('running', 'stopped')):
                names = {str(x.get('name', '')).lower() for x in lm if isinstance(x, dict)}
                proc = (r.get('process_name') or r.get('package_name') or '').lower()
                r['live_state'] = ('up' if proc in names else 'down') if proc else None
    return rows


def _enrich_deploy_with_pkg(rows, config):
    """deprecated 호환 별칭 — _enrich_deploy 와 동일하게 동작."""
    return _enrich_deploy(rows, config)


async def _list_deployments(config):
    rows = await asyncio.to_thread(_deploy_load_all, config)
    rows.sort(key=lambda x: x.get('id', 0))
    _enrich_deploy(rows, config)
    return HandlerResult(status=200, body={"items": [_deployment_to_json(r) for r in rows]},
                         media_type="application/json")


async def _get_deployment(did: int, config):
    r = await asyncio.to_thread(_deploy_load, config, did)
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    _enrich_deploy([r], config)
    return HandlerResult(status=200, body=_deployment_to_json(r), media_type="application/json")


def _check_ha_capability(config, agent_id: int, ha_cap: str):
    """ha_group 의 mode 와 패키지 ha_capability 호환 검사. None 이면 OK."""
    groups = file_store.load_all(file_store.domain_dir(config, 'ha_groups'))
    grp_mode = None
    for g in groups:
        for m in (g.get('members') or []):
            if m.get('agent_id') == agent_id:
                grp_mode = g.get('mode')
                break
        if grp_mode:
            break
    if grp_mode is None:
        return None
    if ha_cap != "standalone" and ha_cap != grp_mode:
        return f"패키지 ha_capability={ha_cap} 가 agent 그룹 mode={grp_mode} 와 불일치 (이 그룹에는 {grp_mode} 모듈만 install 가능)"
    return None


async def _create_deployment(handler_args: HandlerArgs, config):
    body = _parse_body(handler_args)
    agent_id     = body.get("agent_id")
    package_id   = body.get("package_id")
    process_name = (body.get("process_name") or body.get("service_kind") or "").strip()
    functions    = body.get("service_functions") or []
    if isinstance(functions, str):
        functions = _split_csv(functions)
    install_path = (body.get("install_path") or "").strip() or None
    cfg_overlay  = body.get("config")
    if not agent_id or not package_id:
        return HandlerResult(status=400, body={"error": "agent_id and package_id required"},
                             media_type="application/json")

    pkg_file = await asyncio.to_thread(_pkg_load, config, package_id)
    pkg_file = pkg_file or {}
    pkg_meta = pkg_file.get("meta") if isinstance(pkg_file.get("meta"), dict) else {}
    ha_cap = (pkg_meta.get("ha_capability") or "standalone").lower()
    # Phase 4 fix: process_name 자동 추론 — package_name 그대로 (csc, oam, csp 등).
    # 비어있으면 agent 의 cims-svc 가 default 'all' fallback → 단일 모듈 install
    # 환경에서 cmp/csp 바이너리 못 찾아 fail. POST 시점 자동 채움.
    if not process_name:
        process_name = (pkg_meta.get("name") or "").strip()
    mismatch = await asyncio.to_thread(_check_ha_capability, config, agent_id, ha_cap)
    if mismatch:
        return HandlerResult(status=400, body={"error": "ha_mismatch", "detail": mismatch},
                             media_type="application/json")

    # JwtSecret 자동 공유 — 게이트웨이 프록시 서비스 모듈(meta.gateway.routes 보유)은
    #   base 가 발급한 토큰을 검증해야 하므로 base 의 CimsAuth.JwtSecret 와 같아야 한다.
    #   config_template 에서 JwtSecret 은 hidden 이라 콘솔 UI 로 못 넣으므로, 배포 시
    #   OAM 이 자기 시크릿을 deployment config 에 자동 주입(미지정 시). 콘솔 배포만으로 통일.
    try:
        gw = (pkg_meta.get("gateway") or {}).get("routes")
        if gw:
            base_secret = (config.get("CimsAuth") or {}).get("JwtSecret")
            if base_secret:
                if not isinstance(cfg_overlay, dict):
                    cfg_overlay = {}
                has = cfg_overlay.get("CimsAuth.JwtSecret") or \
                      (cfg_overlay.get("CimsAuth") or {}).get("JwtSecret")
                if not has:
                    cfg_overlay["CimsAuth.JwtSecret"] = base_secret
                    logger.log_info(f"[deploy] {process_name}: base JwtSecret 자동 주입(게이트웨이 토큰 검증 통일)")
    except Exception as e:
        logger.log_warning(f"[deploy] JwtSecret 자동주입 skip({process_name}): {e}")

    # 초기 status — 기본 'pending'. 부트스트랩이 이미 설치·기동된 모듈(oam/console)을
    # 등록할 때 'running' 등으로 명시 가능(화이트리스트). install_path 와 함께 쓰면
    # "이미 설치된 상태"로 콘솔 패키지설치 목록에 즉시 노출된다.
    _init_status = (body.get('status') or 'pending').lower()
    if _init_status not in ('pending', 'deploying', 'running', 'stopped', 'failed', 'removed'):
        _init_status = 'pending'
    _now_iso = datetime.now().isoformat(timespec='seconds')

    def _do_create():
        new_id = file_store.next_id(_deploy_dir(config))
        dep = {
            'id': new_id,
            'agent_id': agent_id,
            'package_id': package_id,
            'process_name': process_name,
            'service_functions': functions if isinstance(functions, list) else _split_csv(functions),
            'install_path': install_path,
            'status': _init_status,
            'note': body.get('note'),
            'config': cfg_overlay if isinstance(cfg_overlay, dict) and cfg_overlay else None,
            'config_applied_at': None,
            'deployed_at': (_now_iso if _init_status in ('running', 'stopped') else None),
            'last_job_id': None,
        }
        return _deploy_save(config, dep)

    r = await asyncio.to_thread(_do_create)
    _enrich_deploy([r], config)

    # ── self-register: 서비스 모듈이 선언한 게이트웨이 라우트(pkg meta.gateway.routes)를
    #    배포 config 의 Server.Port(SoT)+Ip 로 게이트웨이에 등록+hot-mount.
    #    base 가 서비스 모듈을 미리 알 필요 없음(시드 하드코딩 대체). role base 에서만 mount.
    try:
        gw_meta = pkg_meta.get("gateway") or {}
        gw_routes = gw_meta.get("routes") or []
        if gw_routes:
            # 포트 = effective_server_port (materialize Server.Port → gateway.default_port).
            #   HA 헬스포트 유도와 같은 해석 — 프록시/헬스가 다른 포트를 보는 드리프트 차단.
            _port = effective_server_port(config, pkg_file, cfg_overlay)
            # 게이트웨이 upstream host = 배포 설정 Server.GatewayHost (운영자 명시 —
            #   base 와 모듈이 다른 호스트인 분리 배치에서 그룹 VIP/노드 IP).
            #   비우면 127.0.0.1 (동거 배치 기본 — 모듈 bind Ip 0.0.0.0 과 무관하게 도달).
            _ip = effective_gateway_host(config, pkg_file, cfg_overlay) or "127.0.0.1"
            if _port:
                import handlers.gateway as _gw
                await asyncio.to_thread(_gw.register_module_routes, config,
                                        process_name, _ip, int(_port), gw_routes)
            else:
                logger.log_warning(
                    f"[deploy] {process_name}: gateway.routes 선언됐으나 Server.Port(config)·"
                    f"gateway.default_port(pkg) 둘 다 없어 라우트 self-register skip "
                    f"— 게이트웨이 프록시 404 위험. 배포 config 에 Server.Port 지정 또는 pkg.json 에 gateway.default_port 선언 필요.")
    except Exception as e:
        logger.log_warning(f"[deploy] self-register routes 실패({process_name}): {e}")

    # ── HA ha.json 재렌더 전파 — 헬스포트/cold_modules 는 렌더 시점의 배포 목록에서
    #   유도되는 파생값이라, 정본 흐름(그룹 구성 → 설치)에서는 그룹 적용 시점에 배포가
    #   없어 port 미기재 ha.json 이 만들어진다. 배포 생성이 렌더 입력을 바꾸므로
    #   여기서 재렌더를 태워 ha.json 이 자동 추종하게 한다. (전파 실패는 생성 성공에
    #   영향 없음 — flap 방어는 cims-health 쪽 안전망이 담당.)
    try:
        from handlers.ha_groups import enqueue_update_ha_for_agent
        n = await asyncio.to_thread(enqueue_update_ha_for_agent, agent_id, config)
        if n:
            logger.log_info(f"[deploy] {process_name}: 배포 생성 → update_ha {n}건 재렌더 큐잉")
    except Exception as e:
        logger.log_warning(f"[deploy] 배포 생성 ha 재렌더 전파 실패(agent={agent_id}): {e}")

    return HandlerResult(status=201, body=_deployment_to_json(r), media_type="application/json")


async def _update_deployment(handler_args: HandlerArgs, did: int, config):
    body = _parse_body(handler_args)
    patches: dict = {}
    if "service_kind" in body and "process_name" not in body:
        body["process_name"] = body["service_kind"]
    for col in ("process_name", "install_path", "note", "package_id"):
        if col in body:
            patches[col] = body[col]
    if "service_functions" in body:
        sf = body["service_functions"]
        if isinstance(sf, str):
            sf = _split_csv(sf)
        patches["service_functions"] = sf
    if not patches:
        return HandlerResult(status=400, body={"error": "no_updatable_fields"}, media_type="application/json")
    # runtime store v2 P5 — package_id 전환(버전 업/다운) 전 old package_id 확보.
    _old_pkg_id = None
    if "package_id" in patches:
        _old_dep = await asyncio.to_thread(_deploy_load, config, did)
        _old_pkg_id = (_old_dep or {}).get("package_id")
    r = await asyncio.to_thread(_deploy_update, config, did, patches)
    if not r:
        return HandlerResult(status=404, body={"error": "not_found"}, media_type="application/json")
    # P5 — 버전 전환 시 컬렉션 SoT 를 대상 버전 schema 로 정합(멱등·예외 무해).
    if "package_id" in patches and patches["package_id"] != _old_pkg_id:
        try:
            from services import collection_schema
            newp = await asyncio.to_thread(_pkg_load, config, patches["package_id"])
            oldp = await asyncio.to_thread(_pkg_load, config, _old_pkg_id) if _old_pkg_id else None
            if newp and (not oldp or newp.get("name") == oldp.get("name")):
                def _tmpl(p):
                    t = (p or {}).get("config_template")
                    return t if isinstance(t, dict) else _safe_json((p or {}).get("config_template_json"))
                new_tmpl = _tmpl(newp); old_tmpl = _tmpl(oldp) if oldp else None
                new_ver = (new_tmpl or {}).get("version")
                owner = newp.get("name")
                if new_tmpl and owner and new_ver is not None:
                    migrated = await asyncio.to_thread(
                        collection_schema.migrate_module_collections,
                        config, owner, old_tmpl, new_tmpl, new_ver)
                    if migrated:
                        logger.log_info(f"runtime store v2 P5: '{owner}' 컬렉션 schema 정합 v{new_ver}: {migrated}")
        except Exception as _e:
            logger.log_warning(f"runtime store v2 P5 schema 정합 skip: {_e}")
    # 실효 포트 전파 — package_id 전환으로 template default 포트가 바뀌면
    # 게이트웨이 라우트/HA 헬스포트가 추종 (deployment config 변경 경로와 동일).
    if "package_id" in patches and patches["package_id"] != _old_pkg_id:
        try:
            newp = await asyncio.to_thread(_pkg_load, config, patches["package_id"])
            oldp = await asyncio.to_thread(_pkg_load, config, _old_pkg_id) if _old_pkg_id else None
            old_port = effective_server_port(config, oldp, r.get("config"))
            new_port = effective_server_port(config, newp, r.get("config"))
            if new_port and new_port != old_port:
                _meta = (newp or {}).get("meta") if isinstance(newp, dict) else None
                gw_routes = ((_meta or {}).get("gateway") or {}).get("routes") or []
                if gw_routes and r.get("process_name"):
                    import handlers.gateway as _gw
                    _host = effective_gateway_host(config, newp, r.get("config")) or "127.0.0.1"
                    await asyncio.to_thread(_gw.register_module_routes, config,
                                            r["process_name"], _host,
                                            int(new_port), gw_routes)
                from handlers.ha_groups import enqueue_update_ha_for_agent
                n = await asyncio.to_thread(enqueue_update_ha_for_agent, r.get("agent_id"), config)
                logger.log_info(f"[deploy-update] dep={did} 실효포트 {old_port}->{new_port} "
                                f"(pkg 전환): gateway 재등록={'O' if gw_routes else 'X'}, update_ha {n}건")
        except Exception as e:
            logger.log_warning(f"[deploy-update] 포트 변경 전파 실패(dep={did}): {e}")
    _enrich_deploy([r], config)
    return HandlerResult(status=200, body=_deployment_to_json(r), media_type="application/json")


async def _delete_deployment(did: int, config):
    # runtime store v2 P4 — 모듈의 마지막 deployment 제거 시 그 모듈 컬렉션 SoT prune.
    dep = await asyncio.to_thread(_deploy_load, config, did)
    pkg = (dep or {}).get("package_name") or (dep or {}).get("package")
    _proc = (dep or {}).get("process_name") or pkg
    await asyncio.to_thread(file_store.delete, _deploy_dir(config), did)
    # self-register 해제: 라우트는 모듈(process) 단위 공유라, 같은 process 의 다른
    # 배포(AS 그룹 피어 등)가 남아 있으면 유지 — 한쪽 멤버 제거/재설치가 모듈 라우트를
    # 전멸시키던 것 방지. 마지막 배포일 때만 deregister+unmount.
    if _proc:
        try:
            siblings = [d for d in await asyncio.to_thread(_deploy_load_all, config)
                        if (d.get("process_name") or d.get("package_name")) == _proc]
            if not siblings:
                import handlers.gateway as _gw
                await asyncio.to_thread(_gw.deregister_module_routes, config, _proc)
            else:
                logger.log_info(f"[deploy] dep={did} 제거 — '{_proc}' 배포 {len(siblings)}개 잔존, 라우트 유지")
        except Exception as e:
            logger.log_warning(f"[deploy] deregister routes 실패({_proc}): {e}")
    if pkg:
        try:
            remaining = [d for d in await asyncio.to_thread(_deploy_load_all, config)
                         if (d.get("package_name") or d.get("package")) == pkg and d.get("id") != did]
            if not remaining:
                from services import ha_lookup
                if await asyncio.to_thread(ha_lookup.prune_module_collections, config, pkg):
                    logger.log_info(f"runtime store v2: '{pkg}' 마지막 deployment 제거 → 컬렉션 SoT prune")
        except Exception as _e:
            logger.log_warning(f"runtime store v2 prune skip (pkg={pkg}): {_e}")
    # 배포 제거도 렌더 입력 변경 — ha.json 재렌더 전파 (생성 경로와 대칭).
    if dep and dep.get("agent_id") is not None:
        try:
            from handlers.ha_groups import enqueue_update_ha_for_agent
            n = await asyncio.to_thread(enqueue_update_ha_for_agent, dep["agent_id"], config)
            if n:
                logger.log_info(f"[deploy] dep={did} 제거 → update_ha {n}건 재렌더 큐잉")
        except Exception as e:
            logger.log_warning(f"[deploy] 배포 제거 ha 재렌더 전파 실패(dep={did}): {e}")
    return HandlerResult(status=204, body=None, media_type="application/json")


async def _queue_job(handler_args: HandlerArgs, did: int, config):
    """Deployment 대상으로 job 큐잉 (install/start/stop/restart/uninstall)."""
    body = _parse_body(handler_args)
    job_type = (body.get("job_type") or "").lower()
    if job_type not in ("install", "upgrade", "uninstall", "start", "stop",
                         "restart", "update_config", "collect_log", "health_check"):
        return HandlerResult(status=400, body={"error": "invalid_job_type"},
                             media_type="application/json")

    dep = await asyncio.to_thread(_deploy_load, config, did)
    if not dep:
        return HandlerResult(status=404, body={"error": "deployment_not_found"},
                             media_type="application/json")
    _enrich_deploy([dep], config)
    cfg = dep.get("config") if isinstance(dep.get("config"), (dict, list)) \
          else _safe_json(dep.get("config_json"))
    # 레코드의 sparse overlay 를 실체화 — install/upgrade/update_config 가 agent 에
    #   전달하는 config 는 template default + base 공유값이 병합된 완전한 유효설정.
    if isinstance(cfg, dict) or cfg is None:
        try:
            _pkg = await asyncio.to_thread(_pkg_load, config, dep.get("package_id"))
            cfg = _materialize_deploy_config(config, _pkg, cfg)
        except Exception as _e:
            logger.log_warning(f"job config materialize skip (dep={did}): {_e}")
    sf = dep.get("service_functions")
    if isinstance(sf, str):
        sf = _split_csv(sf)
    params = {
        "deployment_id": did,
        "package_id":    dep.get("package_id"),
        "package_name":  dep.get("package_name"),
        "package_version": dep.get("package_version"),
        "process_name":  dep.get("process_name"),
        "service_functions": sf or [],
        "install_path":  dep.get("install_path"),
        "config":        cfg,
        "extra":         body.get("extra") or {},
    }
    job_id = await asyncio.to_thread(_job_create, config, dep["agent_id"], job_type, params)
    transition = {"install": "deploying", "upgrade": "deploying",
                  "uninstall": "deploying", "start": "deploying",
                  "stop": "deploying", "restart": "deploying"}
    if job_type in transition:
        await asyncio.to_thread(_deploy_update, config, did,
                                {'status': transition[job_type], 'last_job_id': job_id})
    return HandlerResult(status=202, body={"job_id": job_id, "status": "queued"},
                         media_type="application/json")


async def _rollback_deployment(handler_args: HandlerArgs, did: int, config):
    """POST /deployments/{id}/rollback — 버전 단위 설치 롤백.

    body (선택): { "install_path": str, "version": str }
      미지정 시 install_history 의 직전 항목 → prev_install_path 순으로 자동 선택.

    수행: deployment 레코드의 install_path/package_version 을 대상 버전으로 전환
    → collection 재동기 (v3 collection 의 SoT = 활성 deployment 의 jsonl 이므로,
    현 버전 디렉토리의 jsonl 을 sync REST 로 읽어 대상 버전 config/ 에 PUT —
    구버전 설치 후 변경된 collection 의 stale 방지) → restart job 큐잉
    (agent 가 supervised 경로 비교로 현재 버전 인스턴스를 먼저 stop).
    실제 파일은 agent 가 보존 중인 버전 디렉토리 — 없으면 start 가 fail-fast.
    """
    body = _parse_body(handler_args)
    dep = await asyncio.to_thread(_deploy_load, config, did)
    if not dep:
        return HandlerResult(status=404, body={"error": "deployment_not_found"},
                             media_type="application/json")
    _enrich_deploy([dep], config)
    current = dep.get("install_path") or ""

    # ── 대상 결정
    target_path = (body.get("install_path") or "").strip()
    target_ver  = (body.get("version") or "").strip()
    hist = dep.get("install_history") if isinstance(dep.get("install_history"), list) else []
    if not target_path and target_ver:
        for h in reversed(hist):
            if h.get("version") == target_ver and h.get("install_path") != current:
                target_path = h.get("install_path") or ""
                break
        # 이력에 없으면 관례 경로 (<module_root>/<version>) 추정
        if not target_path and current:
            base = current.rstrip("/")
            parent = os.path.dirname(base)
            if dep.get("package_name") and os.path.basename(parent) == dep.get("package_name"):
                target_path = os.path.join(parent, target_ver)
    if not target_path:
        for h in reversed(hist):
            if h.get("install_path") and h.get("install_path") != current:
                target_path = h["install_path"]
                target_ver = target_ver or h.get("version") or ""
                break
    if not target_path:
        prev = dep.get("prev_install_path")
        if prev and prev != current:
            target_path = prev
            target_ver = target_ver or dep.get("prev_package_version") or ""
    if not target_path or target_path == current:
        return HandlerResult(status=409,
            body={"error": "no_rollback_target",
                  "hint": "install_history/prev_install_path 없음 — body.install_path 로 명시 가능",
                  "current": current},
            media_type="application/json")

    # 대상 버전 미상이면 경로 basename 에서 유추 (<module_root>/<version>)
    if not target_ver:
        bn = os.path.basename(target_path.rstrip("/"))
        import re as _re2
        if _re2.match(r"^\d+(\.\d+){1,3}", bn):
            target_ver = bn

    # ── 레코드 전환 (버전 단위 설치: 롤백 = install_path 전환, 02_deployment.md §2)
    patches = {"install_path": target_path, "status": "deploying",
               "prev_install_path": current,
               "prev_package_version": dep.get("package_version")}
    if target_ver:
        patches["package_version"] = target_ver
        # 같은 (이름, 버전) 의 패키지가 등록돼 있으면 package_id 도 함께 전환
        try:
            p = await asyncio.to_thread(_pkg_load, config, None,
                                        dep.get("package_name"), target_ver)
            if p and p.get("id"):
                patches["package_id"] = p.get("id")
        except Exception:
            pass
    await asyncio.to_thread(_deploy_update, config, did, patches)

    cfg = dep.get("config") if isinstance(dep.get("config"), (dict, list)) \
          else _safe_json(dep.get("config_json"))
    # job 으로 나가는 config 는 실체화 (record 는 sparse overlay 유지) — 다른
    # 디스패치 경로와 동일. agent job_health_check 포트 유도가 template default
    # 를 보게 한다.
    if cfg is None or isinstance(cfg, dict):
        try:
            _rb_pkg = await asyncio.to_thread(_pkg_load, config,
                                              patches.get("package_id") or dep.get("package_id"))
            cfg = _materialize_deploy_config(config, _rb_pkg, cfg)
        except Exception as _e:
            logger.log_warning(f"[deployment-rollback] config 실체화 skip: {_e}")
    sf = dep.get("service_functions")
    if isinstance(sf, str):
        sf = _split_csv(sf)
    base_params = {
        "deployment_id": did,
        "package_id":    dep.get("package_id"),
        "package_name":  dep.get("package_name"),
        "package_version": target_ver or dep.get("package_version"),
        "process_name":  dep.get("process_name"),
        "service_functions": sf or [],
        "install_path":  target_path,
        "config":        cfg,
    }
    # collection 재동기 — 현 버전 디렉토리의 jsonl (SoT) 을 대상 버전 config/ 로 복사.
    # restart 전에 동기 수행 (PUT 의 SIGUSR1 은 미기동 프로세스에 무해).
    synced = []
    dep_proxy = await asyncio.to_thread(_fetch_deployment_for_proxy, did, config)
    if dep_proxy and current:
        tpl = dep_proxy.get("config_template_json")
        tpl = tpl if isinstance(tpl, dict) else _safe_json(tpl)
        col_keys = [c.get("key") for c in (tpl or {}).get("collections") or []
                    if isinstance(c, dict) and c.get("key")]
        for name in col_keys:
            st, b = await asyncio.to_thread(
                _agent_proxy_call, "GET", dep_proxy, "/collection",
                {"install_path": current, "name": name}, None, 15, config)
            recs = (b or {}).get("records") if isinstance(b, dict) else None
            if st == 200 and isinstance(recs, list) and recs:
                st2, _b2 = await asyncio.to_thread(
                    _agent_proxy_call, "PUT", dep_proxy, "/collection",
                    {"install_path": target_path, "name": name},
                    {"records": recs, "signal": False}, 15, config)
                synced.append(f"{name}({len(recs)}):{st2}")

    restart_id = await asyncio.to_thread(_job_create, config, dep["agent_id"], "restart",
                                         dict(base_params, extra={"rollback_from": current}))
    await asyncio.to_thread(_deploy_update, config, did, {"last_job_id": restart_id})
    logger.log_info(f"[deployment-rollback] dep={did} {current} -> {target_path} "
                    f"(ver={target_ver or '?'}) synced={synced} restart_job={restart_id}")
    return HandlerResult(status=202,
        body={"ok": True, "job_ids": [restart_id], "restart_job_id": restart_id,
              "collections_synced": synced,
              "install_path": target_path, "version": target_ver or None},
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


def _latest_agent_pkg_path(config: dict) -> str | None:
    """패키지 저장소의 최신 agent tarball 경로 (없으면 None)."""
    pkgs_dir = file_store.domain_dir(config, "packages")
    items = file_store.load_all(pkgs_dir) if os.path.isdir(pkgs_dir) else []
    agent_pkgs = [p for p in items if p.get("name") == "agent" and p.get("file_path")
                  and os.path.isfile(p.get("file_path") or "")]
    if not agent_pkgs:
        return None
    agent_pkgs.sort(key=lambda p: p.get("uploaded_at") or "", reverse=True)
    return agent_pkgs[0]["file_path"]


def _read_agent_pkg_member(config: dict, member: str) -> bytes | None:
    """최신 agent 패키지 tarball 에서 단일 파일(agent/<member>) 추출.

    agent 설치 에셋의 SoT = 패키지 저장소 (버전별 보관·등록/삭제로 롤백 가능).
    /install-agent.sh, /cims_agent.py 가 /agent-bundle.tar.gz 와 항상 같은
    버전에서 나가도록 통일 — 별도 asset 디렉토리와의 버전 불일치 제거.
    """
    import tarfile as _tarfile
    path = _latest_agent_pkg_path(config)
    if not path:
        return None
    try:
        with _tarfile.open(path, "r:gz") as tf:
            f = tf.extractfile(f"agent/{member}")
            return f.read() if f else None
    except Exception:
        return None


async def _serve_install_script(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    # 1순위: 패키지 저장소의 최신 agent 패키지에서 추출 (bundle 과 동일 버전 보장)
    data = await asyncio.to_thread(_read_agent_pkg_member, config, "install-agent.sh")
    if data is not None:
        return HandlerResult(status=200, body=data.decode("utf-8"),
                             media_type="text/x-shellscript")
    # fallback: dev 환경 (저장소 미등록) — repo/dist 의 agent 디렉토리
    p = _find_agent_asset("install-agent.sh")
    if not p:
        return HandlerResult(status=404, body="install-agent.sh not bundled",
                             media_type="text/plain")
    with open(p, "r", encoding="utf-8") as f:
        return HandlerResult(status=200, body=f.read(),
                             media_type="text/x-shellscript")


async def _serve_agent_bundle(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """최신 agent tarball (build/dist/packages/agent-*.tar.gz) 반환.
    install-agent.sh 가 cims_agent.py + bin/ + lib/ + keepalived/ + systemd/ 한 번에 받기 위함."""
    config = kwargs.get("config", {})
    tarball_path = _latest_agent_pkg_path(config)
    if not tarball_path:
        return HandlerResult(status=404, body="agent package not registered",
                             media_type="text/plain")
    with open(tarball_path, "rb") as f:
        return HandlerResult(status=200, body=f.read(),
                             media_type="application/gzip")


async def _serve_agent_binary(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    data = await asyncio.to_thread(_read_agent_pkg_member, config, "cims_agent.py")
    if data is not None:
        return HandlerResult(status=200, body=data, media_type="text/x-python")
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
    """proxy 에 필요한 deployment + agent 정보 + 패키지 config_template 동시 조회. (sync — asyncio.to_thread 로 호출)"""
    dep = _deploy_load(config, did)
    if not dep:
        return None
    r = {
        'id': dep.get('id'),
        'install_path': dep.get('install_path'),
        'package_id': dep.get('package_id'),
        'agent_id': dep.get('agent_id'),
    }
    agent = _agent_load(config, aid=r.get('agent_id')) or {}
    r['agent_name'] = agent.get('name')
    r['agent_status'] = agent.get('status')
    r['ip_address'] = agent.get('ip_address')
    r['sync_port'] = agent.get('sync_port')
    r['agent_token'] = agent.get('agent_token')
    pkg = _pkg_load(config, pid=r.get('package_id')) or {}
    r['config_template_json'] = pkg.get('config_template')  # 옛 이름 그대로 (downstream _collection_schema)
    r['package_name'] = dep.get('package_name') or pkg.get('name')
    return r


def _collection_schema(template_json, name: str):
    """template.collections 에서 key=name 인 항목의 schema 를 찾아 반환. 없으면 None."""
    tmpl = template_json if isinstance(template_json, dict) else _safe_json(template_json)
    if not isinstance(tmpl, dict): return None, None
    for c in tmpl.get("collections") or []:
        if c.get("key") == name:
            return c.get("schema") or {}, c
    return None, None


def _warn_missing_scope(template, pkg_name: str) -> list:
    """config_template 의 section/collection 에 scope 누락 entry 목록을 반환.
    SoT: docs/design/csc_config_server.md §2.3. 1 릴리스 후 fatal 승격 예정."""
    if not isinstance(template, dict): return []
    missing = []
    for s in template.get("sections") or []:
        if s.get("scope") not in ("system", "service"):
            missing.append(f"section:{s.get('key')}")
    for c in template.get("collections") or []:
        if c.get("scope") not in ("system", "service"):
            missing.append(f"collection:{c.get('key')}")
    if missing:
        sys.stderr.write(f"[WARN] package '{pkg_name}': config_template scope 누락 — {missing}\n")
    return missing


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
    """deployment 의 jsonl 컬렉션 GET.

    HA drift 감지 (T2): query/body 와 무관하게 ha_group 멤버 deployment 들의
    records 도 비교. 멤버끼리 records hash 가 다르면 drift_detected=true 를
    응답에 포함 — UI 가 경고/재동기화 유도.

    응답 형태:
      records:  요청 대상 deployment 의 현재 records (옛 호환)
      schema:   template 의 schema (옛 호환)
      peers:    [{deployment_id, agent_id, status, count, hash, records?}, ...] (없으면 빈 리스트)
      drift_detected: bool
      ha_group_id / ha_group_mode / scope
    """
    from services import ha_lookup
    import hashlib

    dep = await asyncio.to_thread(_fetch_deployment_for_proxy, did, config)
    if not dep:
        return HandlerResult(status=404, body={"error": "deployment_not_found"},
                             media_type="application/json")
    if not dep.get("install_path"):
        return HandlerResult(status=409, body={"error": "not_installed",
                                                "hint": "install 먼저 실행"},
                             media_type="application/json")
    schema, coll = _collection_schema(dep.get("config_template_json"), name)
    if schema is None:
        return HandlerResult(status=404,
            body={"error": "collection_not_in_template", "name": name},
            media_type="application/json")

    scope = ((coll or {}).get("scope") or "service").lower()
    pkg_name = dep.get("package_name")

    # ── 멤버 deployment 들 결정 (자기 자신 포함)
    targets: list[dict] = [dep]
    ha_group_id = None
    ha_mode = None
    if pkg_name:
        g = await asyncio.to_thread(ha_lookup.ha_group_for_package, config, pkg_name)
        if g:
            ha_group_id = g.get("id")
            ha_mode = g.get("mode")
            peers = await asyncio.to_thread(
                ha_lookup.deployments_in_group_for_package, config, ha_group_id, pkg_name)
            seen = {dep.get("id")}
            for p in peers:
                pid = p.get("id")
                if pid in seen:
                    continue
                full = await asyncio.to_thread(_fetch_deployment_for_proxy, pid, config)
                if full and full.get("install_path"):
                    targets.append(full)
                    seen.add(pid)

    # ── 각 target 에 동시 GET
    async def _get_one(t):
        return await asyncio.to_thread(
            _agent_proxy_call, "GET", t,
            "/collection", {"install_path": t["install_path"], "name": name},
            None, 15, config,
        )
    results = await asyncio.gather(*[_get_one(t) for t in targets])

    # ── 기준 (요청 dep) records + peer 비교
    def _records_hash(recs):
        try:
            payload = json.dumps(recs or [], ensure_ascii=False, sort_keys=True).encode("utf-8")
            return hashlib.sha256(payload).hexdigest()[:12]
        except Exception:
            return ""

    base_records: list = []
    peers_resp: list[dict] = []
    base_hash = ""
    for idx, (t, (status, resp)) in enumerate(zip(targets, results)):
        ok = (status == 200)
        recs = (resp or {}).get("records") or [] if ok else []
        h = _records_hash(recs) if ok else ""
        if idx == 0:
            base_records = recs
            base_hash = h
        peers_resp.append({
            "deployment_id": t["id"],
            "agent_id":      t.get("agent_id"),
            "status":        status,
            "ok":            ok,
            "count":         len(recs) if ok else None,
            "hash":          h,
            "error":         None if ok else resp,
        })

    # ── drift 결정: 양 멤버 모두 ok 인 경우만 hash 비교 (proxy 실패는 drift 아님)
    drift = False
    if len(peers_resp) > 1 and base_hash:
        for p in peers_resp[1:]:
            if p["ok"] and p["hash"] and p["hash"] != base_hash:
                drift = True
                break

    return HandlerResult(status=200,
        body={
            "records":        base_records,        # 옛 호환
            "schema":         schema,              # 옛 호환
            "peers":          peers_resp,
            "drift_detected": drift,
            "ha_group_id":    ha_group_id,
            "ha_group_mode":  ha_mode,
            "scope":          scope,
        },
        media_type="application/json")


async def _put_deployment_collection(handler_args, did: int, name: str, config):
    """deployment 의 jsonl 컬렉션 PUT — 항상 해당 deployment 에만.

    HA 그룹 전파 없음 — 멤버 간 정합은 그룹 동기화(POST /deployments/{id}/sync)의
    명시적 실행으로만 맞춘다. 구 body.propagate_to_ha_peers 는 무시된다.
    드리프트는 GET 의 멤버 hash 비교(drift_detected)와 drift_sweeper 가 감지해
    콘솔 그룹 [설정 비교] 뷰가 경고로 노출한다.
    """
    dep = await asyncio.to_thread(_fetch_deployment_for_proxy, did, config)
    if not dep:
        return HandlerResult(status=404, body={"error": "deployment_not_found"},
                             media_type="application/json")
    if not dep.get("install_path"):
        return HandlerResult(status=409, body={"error": "not_installed"},
                             media_type="application/json")
    schema, coll = _collection_schema(dep.get("config_template_json"), name)
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
    scope = ((coll or {}).get("scope") or "service").lower()

    # ── 해당 deployment 의 agent 에만 PUT
    status, resp = await asyncio.to_thread(
        _agent_proxy_call, "PUT", dep,
        "/collection", {"install_path": dep["install_path"], "name": name},
        {"records": records, "signal": do_signal}, 15, config,
    )
    ok = (status == 200)
    peers_resp = [{
        "deployment_id": dep["id"],
        "agent_id":      dep.get("agent_id"),
        "status":        status,
        "ok":            ok,
        "count":         (resp or {}).get("count")    if ok else None,
        "signaled":      (resp or {}).get("signaled") if ok else [],
        "error":         None                          if ok else resp,
    }]

    # ── 응답 (옛 다중-peer 호출자 호환 위해 peers/propagated 형태 유지)
    return HandlerResult(status=200 if ok else 502,
        body={
            "ok":            ok,
            "count":         peers_resp[0].get("count"),
            "signaled":      peers_resp[0].get("signaled") or [],
            "peers":         peers_resp,
            "scope":         scope,
            "propagated":    False,
            "detail":        None if ok else [resp],
        },
        media_type="application/json")


# ════════════════════════════════════════════════════════════
#  Handler list
# ════════════════════════════════════════════════════════════

async def handle_sync_txn(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """sync_txn 폴링 endpoint (L2 + L3).

    Routes:
      GET  /api/v1/csp/sync                — 최근 N건 (?limit=50, ?status=pending|partial|success|failed)
      GET  /api/v1/csp/sync/<sid>          — 단일 트랜잭션 + 멤버 ack 상태
      POST /api/v1/csp/sync/sweep          — timeout sweeper 수동 트리거 (L3, body 없음)
    """
    from services import sync_txn

    config = kwargs.get("config", {})
    deny = _console_rbac(handler_args)
    if deny: return deny
    tail = _path_tail(handler_args.full_path, _SYNC_TXN_BASE)
    method = handler_args.method.upper()

    if len(tail) == 1 and tail[0] == "sweep" and method == "POST":
        n = await asyncio.to_thread(sync_txn.sweep_timeouts, config)
        return HandlerResult(status=200,
            body={"ok": True, "timed_out": n},
            media_type="application/json")

    if method != "GET":
        return HandlerResult(status=405, body={"error": "method_not_allowed"},
                             media_type="application/json")

    if not tail:
        qp = handler_args.query_params or {}
        try:    limit = max(1, min(500, int(qp.get("limit", "50"))))
        except: limit = 50
        status_filter = qp.get("status") or None
        rows = await asyncio.to_thread(sync_txn.list_recent, config, limit)
        if status_filter:
            rows = [r for r in rows if (r.get("status") or "") == status_filter]
        return HandlerResult(status=200,
            body={"items": rows, "count": len(rows)},
            media_type="application/json")

    try: sid = int(tail[0])
    except (TypeError, ValueError):
        return HandlerResult(status=400, body={"error": "invalid_id"},
                             media_type="application/json")
    txn = await asyncio.to_thread(sync_txn.get, config, sid)
    if not txn:
        return HandlerResult(status=404, body={"error": "not_found"},
                             media_type="application/json")
    return HandlerResult(status=200, body=txn, media_type="application/json")


async def handle_drift(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """drift scan + resync endpoint (L4).

    Routes:
      GET  /api/v1/csp/drift            — 전체 ha_group * collection drift scan
      POST /api/v1/csp/drift/resync     — drift 있는 컬렉션 master records 로 자동 PUT
    """
    from services import drift_sweeper

    config = kwargs.get("config", {})
    deny = _console_rbac(handler_args)
    if deny: return deny
    tail = _path_tail(handler_args.full_path, _DRIFT_BASE)
    method = handler_args.method.upper()

    if not tail and method == "GET":
        results = await asyncio.to_thread(drift_sweeper.scan_all, config)
        drift_only = (handler_args.query_params or {}).get("drift_only") in ("1", "true")
        items = [r for r in results if r.get('drift')] if drift_only else results
        # records 본문은 응답에서 제외 (UI 가 별도 GET 으로 가져가게)
        slim = []
        for r in items:
            slim.append({
                **{k: r[k] for k in r if k != 'members'},
                'members': [{k2: m.get(k2) for k2 in ('deployment_id','agent_id',
                            'status','ok','count','hash')} for m in r.get('members') or []],
            })
        drift_count = sum(1 for r in results if r.get('drift'))
        return HandlerResult(status=200,
            body={"items": slim, "count": len(slim),
                  "total_scanned": len(results), "drift_count": drift_count},
            media_type="application/json")

    if len(tail) == 1 and tail[0] == "resync" and method == "POST":
        results = await asyncio.to_thread(drift_sweeper.scan_all, config)
        drift_rows = [r for r in results if r.get('drift')]
        summary = await asyncio.to_thread(drift_sweeper.auto_resync, config, drift_rows)
        return HandlerResult(status=200,
            body={"ok": True, "drift_count": len(drift_rows), **summary},
            media_type="application/json")

    return HandlerResult(status=405, body={"error": "method_not_allowed"},
                         media_type="application/json")


async def handle_sip_services(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """SipService 목록 조회 (L5).

    옛 csp_runtime/sip_service file_store → 진짜 SoT 인 첫 csp deployment 의
    access_services 컬렉션 으로 마이그레이션. UI (VolteMsisdnPage/PttMsisdnPage)
    의 cspRuntimeApi.listServices() 호환을 위해 같은 경로/응답 형태 유지.

    Routes:
      GET /api/v1/csp/services             — 통합 목록 (volte/ptt 둘 다)

    POST/PUT/DELETE 는 410 Gone — 편집은 deployments collection PUT 으로.
    """
    config = kwargs.get("config", {})
    deny = _console_rbac(handler_args)
    if deny: return deny
    tail = _path_tail(handler_args.full_path, _SIP_SERVICES_BASE)
    method = handler_args.method.upper()

    if method != "GET":
        return HandlerResult(status=410,
            body={"error": "deprecated",
                  "hint": "Use PUT /api/v1/deployments/<did>/collection/access_services"},
            media_type="application/json")

    # ── csp deployment 1건 결정 (HA 그룹이면 첫 멤버 — drift 는 GET /drift 로 별도 확인)
    deps = await asyncio.to_thread(_agent_load_all_deployments, config)
    csp_dep = None
    for d in deps:
        if d.get('package_name') == 'csp':
            csp_dep = d; break
        pkg = _pkg_load(config, pid=d.get('package_id')) or {}
        if pkg.get('name') == 'csp':
            d = dict(d); d['package_name'] = 'csp'
            csp_dep = d; break
    if not csp_dep:
        return HandlerResult(status=200, body={"items": []}, media_type="application/json")

    agent = _agent_load(config, aid=csp_dep.get('agent_id')) or {}
    status, body = await asyncio.to_thread(_agent_proxy_call,
        "GET", agent, "/collection",
        {"install_path": csp_dep.get('install_path'), "name": "access_services"},
        None, 10, config)

    items: list = []
    if status == 200 and isinstance(body, dict):
        for r in body.get('records') or []:
            items.append({
                "id":             r.get("id"),
                "name":           r.get("name"),
                "kind":           r.get("kind"),
                "domain":         r.get("domain"),
                "auth_realm":     r.get("auth_realm"),
                "inbound_policy": r.get("inbound_policy"),
                "priority":       r.get("priority"),
                "enabled":        bool(r.get("enabled")),
                "listeners":      r.get("allowed_local_node_refs") or [],
                "note":           r.get("note"),
                "etag":           "",
                "create_time":    None,
                "update_time":    None,
            })

    if single := (tail[0] if tail else None):
        try: sid = int(single)
        except (TypeError, ValueError):
            return HandlerResult(status=400, body={"error": "invalid_id"},
                                 media_type="application/json")
        for it in items:
            if it["id"] == sid:
                return HandlerResult(status=200, body=it, media_type="application/json")
        return HandlerResult(status=404, body={"error": "not_found"},
                             media_type="application/json")

    return HandlerResult(status=200, body={"items": items}, media_type="application/json")


def _agent_load_all_deployments(config):
    """SipService 마이그레이션용 — 모든 deployment row 반환."""
    from services import file_store as _fs
    return _fs.load_all(_fs.domain_dir(config, 'deployments'))


CIMS_AGENT_ADMIN_HANDLER_LIST = (
    (_AGENT_BASE,         handle_agents,       {}),
    (_PACKAGE_BASE,       handle_packages,     {}),
    (_DEPLOYMENT_BASE,    handle_deployments,  {}),
    (_SYNC_TXN_BASE,      handle_sync_txn,     {}),
    (_DRIFT_BASE,         handle_drift,        {}),
    (_SIP_SERVICES_BASE,  handle_sip_services, {}),
)

# 인증 없이 누구나 받을 수 있는 배포용 정적 에셋
CIMS_AGENT_PUBLIC_HANDLER_LIST = (
    ("/install-agent.sh",   _serve_install_script, {}),
    ("/cims_agent.py",      _serve_agent_binary,   {}),
    ("/agent-bundle.tar.gz", _serve_agent_bundle,  {}),
)
