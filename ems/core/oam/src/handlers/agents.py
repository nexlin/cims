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
    """config_template 의 string_list/ref_list/object_list 필드 값을 배열로 정규화.
    프론트 위젯 누락·raw API 우회에도 config.json 에 배열로 저장되게 하는 백엔드 방어.
    object_list 는 콤마문자열/["ip:port"] 레거시를 [{...}] 로 변환(dict 배열은 그대로)."""
    if not isinstance(values, dict):
        return values
    list_keys = set()
    obj_fields = {}  # key → item_schema.fields
    for sec in (template or {}).get("sections", []):
        for fld in sec.get("fields", []):
            t = (fld.get("type") or "").lower()
            if not fld.get("key"):
                continue
            if t in ("string_list", "ref_list"):
                list_keys.add(fld["key"])
            elif t == "object_list":
                obj_fields[fld["key"]] = (fld.get("item_schema") or {}).get("fields") or []
    if not list_keys and not obj_fields:
        return values
    out = dict(values)
    for k in list_keys:
        v = out.get(k)
        if isinstance(v, str):
            out[k] = [s.strip() for s in v.split(",") if s.strip()]
    if obj_fields:
        from handlers.modules import _coerce_object_list  # 지연 import (순환 회피)
        for k, item_fields in obj_fields.items():
            if k in out:
                out[k] = _coerce_object_list(out.get(k), item_fields)
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


# 배포 설정 조회 시 시크릿을 가리는 sentinel. 콘솔은 이 값을 그대로 표시(입력창은 dots)하고,
# 운영자가 손대지 않으면 저장 payload 에 포함되지 않는다(변경 키만 전송). 구 콘솔이 전체 값을
# 되돌려 보내는 경우에도 PUT 에서 이 값을 '변경 없음' 으로 걸러내 실제 시크릿을 덮지 않는다.
_SECRET_MASK = "••••••••"


def _password_keys(template) -> set:
    """config_template 에서 type=password 인 필드 키 집합."""
    out = set()
    if not isinstance(template, dict):
        return out
    for sec in template.get("sections") or []:
        for fld in sec.get("fields") or []:
            if isinstance(fld, dict) and fld.get("type") == "password" and fld.get("key"):
                out.add(fld["key"])
    return out


def _mask_secrets(overlay, template):
    """조회 응답용 — password 필드 값을 sentinel 로 치환 (비어있으면 그대로 빈 값)."""
    if not isinstance(overlay, dict):
        return overlay
    pw = _password_keys(template)
    if not pw:
        return overlay
    out = dict(overlay)
    for k in pw:
        if out.get(k):
            out[k] = _SECRET_MASK
    return out


def _strip_masked(values, template) -> dict:
    """저장 입력용 — sentinel 그대로 온 password 값은 '변경 없음' 이므로 제거."""
    if not isinstance(values, dict):
        return values
    pw = _password_keys(template)
    return {k: v for k, v in values.items()
            if not (k in pw and isinstance(v, str) and v == _SECRET_MASK)}


def _template_key_set(template) -> set:
    """config_template 이 선언한 모든 필드 키 (scope 무관)."""
    out: set = set()
    if not isinstance(template, dict):
        return out
    for s in template.get("sections") or []:
        for f in s.get("fields") or []:
            if f.get("key"):
                out.add(f["key"])
    return out


def _prune_to_template(values, pkg_file, *, where: str) -> tuple:
    """overlay 쓰기 마스크 — **템플릿에 선언된 키만 저장한다** (스키마가 계약).

    deployment.config overlay 는 "운영자가 정한 값"(desired state)이고, 렌더된
    `<pkg>/config.json` 은 그 파생물이다. 템플릿 밖 키가 overlay 에 앉으면
      - 그 패키지 화면에는 안 보이고(템플릿에 필드가 없으니 편집·조회 불가),
      - 자동 교정도 못 건드리며(교정은 템플릿 service 키만 순회),
      - 다른 패키지 템플릿에 얹히면 남의 필드로 오독된다.
    실측 사고: base oam overlay 에 얹힌 `ServiceLogging.Dir` 이 oam-svc/csc 화면에서
    유령 드리프트로 표시됐다.

    **템플릿이 없는 패키지는 프루닝하지 않는다** — 검증 근거가 없으면 판단하지 않는다
    (판단 불가 시 보수적으로, reconcile 의 원칙과 동일).

    반환 (pruned_values, dropped_keys). dropped 는 응답·로그로 드러낸다(조용히 버리지 않음).
    """
    if not isinstance(values, dict):
        return values, []
    template = pkg_file.get("config_template") if isinstance(pkg_file, dict) else None
    keys = _template_key_set(template)
    if not keys:
        return values, []
    dropped = sorted(k for k in values if k not in keys)
    if not dropped:
        return values, []
    logger.log_warning(f"[config] {where}: 템플릿 밖 키 {len(dropped)}개 미저장 {dropped} "
                       f"— 모듈이 읽는 값이면 config_template 에 선언해야 한다")
    return {k: v for k, v in values.items() if k in keys}, dropped


def _module_holds_lease(config, pkg_file) -> bool:
    """이 패키지의 모듈이 **관리 store 의 리스 보유자**인가 (descriptor `safety.
    requires_leader_lease`). 공유 store 경로를 줄 대상을 가르는 기준이다 — 서비스 모듈
    (csc 등)은 리스 획득 코드가 없어 경로만 받으면 펜싱 없는 두 번째 writer 가 된다.
    descriptor 를 못 읽으면 **주지 않는다**(보수적: 잘못 주는 쪽이 손상이다)."""
    name = ((pkg_file or {}).get("name") or "").lower().strip() if isinstance(pkg_file, dict) else ""
    if not name:
        return False
    try:
        from services import service_registry
        spec = (service_registry.all_modules(config) or {}).get(name) or {}
        return bool((spec.get("safety") or {}).get("requires_leader_lease"))
    except Exception as e:
        logger.log_warning(f"[config] {name}: 리스 보유 판정 실패({e}) — store 경로 미주입")
        return False


def _materialize_deploy_config(config, pkg_file, overlay):
    """배포 config 실체화 — agent 가 쓰는 config.json 이 항상 완전한 유효설정이 되도록
    config_template default 를 base 로 깔고 deployment overlay(사용자 변경분)를 병합.
    deployment 레코드는 sparse overlay 그대로 유지(사용자 의도 SoT) — template default
    변경은 다음 job 디스패치에서 자동 추종된다.

    **그룹 공통 신원 주입** — 대상은 (a) 게이트웨이 서비스 모듈(`meta.gateway.routes` 보유:
    csc/oam-svc, base 발급 토큰을 검증해야 함) + (b) `meta.shared_identity` 선언 모듈
    (base `oam` 자신 — 이중화된 두 번째 노드의 OAM 이 같은 신원으로 떠야 한다).
      - CimsAuth.JwtSecret / CimsRuntimeDir / Mgmt.Cidr — base 가 SoT, overlay 보다 우선
        (시크릿 회전·runtime 이동 시 base 현재값 추종).
      - ServiceLogging.Dir — template 소유(콘솔 편집 가능), 비어있을 때만 base 값 주입.
      - CimsAuth.BuiltinAccounts — shared_identity 모듈만. admin 계정이 노드마다 다르면
        절체 후 로그인이 깨진다(관리평면 이중화 전제, oam_ha.md §5)."""
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
    if isinstance(pkg_meta, dict):
        _gw = bool((pkg_meta.get("gateway") or {}).get("routes"))
        _shared = bool(pkg_meta.get("shared_identity"))
        if _gw or _shared:
            secret = (config.get("CimsAuth") or {}).get("JwtSecret")
            if secret:
                out["CimsAuth.JwtSecret"] = secret
            # 관리 store 경로는 **리스 보유 모듈에만** 준다. 공유 store 는 소유권 리스
            # (flock+epoch)를 쥔 하나만 write 하는 자원인데, csc 같은 서비스 모듈은 리스
            # 획득 코드가 없다 — 경로만 받아두면 IdMS 가 토큰을 발급하는 순간 **펜싱 없는
            # 두 번째 writer** 가 된다. 판별자는 descriptor 의 `safety.requires_leader_lease`
            # (= "이 모듈은 단일 writer 자원을 소유한다" 선언, oam/oam-svc 만 true).
            # 서비스 모듈은 노드 로컬 runtime 을 쓴다 — 절체 시 그 모듈의 로컬 상태
            # (csc IdMS 의 auth_codes·refresh_tokens 등)는 유실되고 단말이 재로그인한다.
            #
            # store 경로는 **overlay 명시값이 우선**이다. base 를 무조건 덮어쓰면 이관
            # (overlay 에 새 경로를 넣는 작업)이 무력화된다 — 실측 사고: 이관이 overlay 에
            # `CimsRuntimeDir=/NAS/runtime` + `CimsRuntimeMount=/NAS` 를 넣었는데 여기서
            # base(로컬 경로)가 Dir 만 되돌려, "store 가 마운트 하위가 아님" guard 에 걸려
            # OAM 이 기동을 거부했다(자가복구가 되돌려 콘솔은 살아남음).
            # overlay 에 값이 없을 때만 base 를 주입한다(= 아직 정하지 않은 노드에 그룹 값 전파).
            if _module_holds_lease(config, pkg_file) \
                    and config.get("CimsRuntimeDir") \
                    and not str(out.get("CimsRuntimeDir") or "").strip():
                out["CimsRuntimeDir"] = config["CimsRuntimeDir"]
            if (config.get("Mgmt") or {}).get("Cidr"):
                out["Mgmt.Cidr"] = config["Mgmt"]["Cidr"]
            if not out.get("ServiceLogging.Dir"):
                sld = (config.get("ServiceLogging") or {}).get("Dir")
                if sld:
                    out["ServiceLogging.Dir"] = sld
        if _shared:
            # admin 계정(해시 포함) — 노드마다 다르면 절체 후 로그인이 깨진다.
            accts = (config.get("CimsAuth") or {}).get("BuiltinAccounts")
            if isinstance(accts, list) and accts:
                out["CimsAuth.BuiltinAccounts"] = accts
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


def self_register_deployment_routes(config, dep: dict) -> int:
    """배포 레코드의 패키지 meta.gateway.routes 를 게이트웨이에 self-register(segment
    upsert — 멱등) + role=base 면 hot-mount. 배포 생성(_create_deployment)과 job 성공
    보고(install/upgrade/start/restart — agent_api._report) 양쪽에서 호출한다 —
    종전엔 생성 시 1회뿐이라 upgrade 후 라우트 미등록 상태를 복구할 경로가 없었다.
    포트/호스트 해석은 effective_server_port / effective_gateway_host (HA 헬스포트
    유도와 공유하는 단일 해석) — 프록시와 헬스체크가 다른 포트를 보는 드리프트 차단."""
    if not isinstance(dep, dict):
        return 0
    pkg = _pkg_load(config, dep.get("package_id")) if dep.get("package_id") else None
    meta = (pkg or {}).get("meta") if isinstance((pkg or {}).get("meta"), dict) else {}
    gw_meta = meta.get("gateway") or {}
    gw_routes = gw_meta.get("routes") or []
    if not gw_routes:
        return 0
    process_name = dep.get("process_name") or (pkg or {}).get("name")
    overlay = dep.get("config") if isinstance(dep.get("config"), dict) else {}
    port = effective_server_port(config, pkg, overlay)
    if not port:
        logger.log_warning(
            f"[deploy] {process_name}: gateway.routes 선언됐으나 Server.Port(config)·"
            f"gateway.default_port(pkg) 둘 다 없어 라우트 self-register skip "
            f"— 게이트웨이 프록시 404 위험. 배포 config 에 Server.Port 지정 또는 "
            f"pkg.json 에 gateway.default_port 선언 필요.")
        return 0
    # 게이트웨이 upstream host = Server.GatewayHost(분리 배치 명시) → 127.0.0.1(동거 기본).
    host = effective_gateway_host(config, pkg, overlay) or "127.0.0.1"
    import handlers.gateway as _gw
    return _gw.register_module_routes(config, process_name, host,
                                      int(port), gw_routes)


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


# ── 대기 job 인덱스 (agent_id → queued job id 목록) ─────────────────────────
#   `_job_pick_pending` 이 **전 job 을 읽어** 필터하던 것이 store 가 NFS 로 옮겨간 뒤
#   최대 비용이 됐다: job 103건 × NFS 5ms ≈ 0.5초를 agent 6대가 2초마다 = 상시 포화.
#   job 이 쌓일수록 선형으로 악화된다. 그래서 큐잉 시 인덱스에 id 만 적고, 픽 시 그
#   인덱스 1파일만 읽어 후보 job 만 로드한다(≤limit).
#   인덱스는 **캐시**다 — 없거나 깨졌으면 전체 스캔으로 재구축한다(정본은 job 파일).
_JOB_INDEX_DOMAIN = 'job_index'


def _job_index_dir(config):
    return file_store.domain_dir(config, _JOB_INDEX_DOMAIN)


# 인덱스 신선도 구간 확인의 상한 — 이보다 크면 전수 재구축이 더 싸다.
_JOB_INDEX_GAP_MAX = 200


def _jobs_seq(config) -> int:
    """jobs 도메인이 마지막으로 발급한 id (`.seq`). 인덱스 **신선도 판정용 O(1) 읽기**.

    job 은 발급(next_id → .seq 증가) → 저장 → 인덱스 등록 순서라, 인덱스에 기록해 둔
    seq 가 현재 .seq 와 다르면 **그 사이에 만들어진 job 이 인덱스에 없다**는 뜻이다."""
    try:
        with open(os.path.join(_job_dir(config), '.seq')) as f:
            v = f.read().strip()
        return int(v) if v.lstrip('-').isdigit() else -1
    except Exception:
        return -1


def _job_index_load(config, agent_id: int) -> "tuple[list | None, int]":
    """(ids, seq) — ids 가 None 이면 인덱스 부재/손상(재구축 필요), seq 는 기록된 신선도."""
    try:
        rec = file_store.load(_job_index_dir(config), int(agent_id))
    except Exception:
        return None, -1
    if not isinstance(rec, dict):
        return None, -1
    ids = rec.get('queued')
    seq = rec.get('seq')
    return ([int(x) for x in ids] if isinstance(ids, list) else None,
            int(seq) if isinstance(seq, int) else -1)


def _job_index_save(config, agent_id: int, ids: list, seq: "int | None" = None) -> None:
    """인덱스는 **캐시**다 — 저장에 실패해도 다음 픽에서 seq 불일치로 재구축된다."""
    try:
        file_store.save(_job_index_dir(config), int(agent_id),
                        {'id': int(agent_id), 'queued': sorted({int(x) for x in ids}),
                         'seq': _jobs_seq(config) if seq is None else int(seq)})
    except Exception as e:
        logger.log_warning(f"[job-index] agent#{agent_id} 저장 실패 — 다음 픽에서 "
                           f"seq 불일치로 자동 재구축된다: {e}")


def _job_index_add(config, agent_id: int, jid: int) -> None:
    ids, _seq = _job_index_load(config, agent_id)
    if ids is None:
        ids = _job_index_rebuild(config, agent_id)
    _job_index_save(config, agent_id, list(ids) + [jid])


def _job_index_rebuild(config, agent_id: int) -> list:
    """전체 스캔으로 인덱스 복원 — 인덱스 부재/손상, 구버전 store 이행 경로."""
    ids = [j.get('id') for j in _job_load_all(config)
           if j.get('agent_id') == agent_id and j.get('status') == 'queued' and j.get('id')]
    _job_index_save(config, agent_id, ids)
    return sorted(ids)


def _job_create(config, agent_id: int, job_type: str, params: dict,
                status: str = 'queued', not_before: str | None = None) -> int:
    """agent_job 1건 생성. lastrowid 호환을 위해 id 반환.

    not_before (ISO, 선택): 그 시각 전에는 agent 가 job 을 가져가지 않는다 —
    HA 개시 국면에서 start 된 멤버가 VIP 를 먼저 잡도록 나머지 멤버의 update_ha
    를 지연시키는 용도 (_job_pick_pending 이 필터)."""
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
    if not_before:
        obj['not_before'] = not_before
    file_store.save(d, new_id, obj)
    if status == 'queued' and agent_id is not None:
        _job_index_add(config, agent_id, new_id)      # 픽 경로가 전체 스캔을 피하도록
    return new_id


_JOB_TERMINAL = ('succeeded', 'failed', 'cancelled', 'canceled', 'timeout')


def sweep_stuck_deploying(config, stale_sec: int = 300) -> int:
    """`deploying` 에 고착된 배포 기록을 실제 상태로 정정한다. 정정 건수 반환.

    `deploying` 은 **과도 상태**다 — job 이 끝나면 그 결과가 status 를 확정한다. 그런데
    끝났다는 보고가 유실되거나(자기 업그레이드 중 재기동), job 이 아예 실행되지 못하면
    (인덱스 어긋남 등) **영원히 과도 상태로 남는다**. 그러면 콘솔은 실제로 도는 모듈을
    "배포 중" 으로 계속 표시하고, 운영자는 현실을 볼 통로가 없다(실측).

    정정은 **근거가 확실할 때만** 한다:
      - 마지막 job 이 성공/실패로 끝났다 → 그 결과로 확정(성공은 실측이 있으면 실측 우선)
      - 마지막 job 이 사라졌다(purge 등) → 실측으로 확정
      - job 이 아직 queued/running 이면 **건드리지 않는다** — 진짜 진행 중일 수 있다.
        단 stale_sec 이 지나도록 그대로면 실측이 있을 때만 정정한다(진행 중이라는 근거가
        시간이 갈수록 약해지므로).
    """
    from datetime import datetime as _dt, timedelta as _td
    fixed = 0
    try:
        rows = _deploy_load_all(config)
    except Exception as e:
        logger.log_warning(f"[deploy-sweep] 배포 목록 조회 실패: {e}")
        return 0
    rows = [r for r in rows if (r.get('status') or '') == 'deploying']
    if not rows:
        return 0
    _enrich_deploy(rows, config)                  # live_state 채우기(실측)
    now = _dt.now()
    for r in rows:
        live = r.get('live_state')
        jid = r.get('last_job_id')
        job = None
        if jid:
            try:
                job = _job_load(config, int(jid))
            except Exception:
                job = None
        jstatus = (job or {}).get('status')
        decided = None
        why = ''
        if job is None:
            decided = {'up': 'running', 'down': 'stopped'}.get(live)
            why = f'job#{jid} 없음'
        elif jstatus == 'succeeded':
            decided = {'up': 'running', 'down': 'stopped'}.get(live) or 'running'
            why = f'job#{jid} 성공'
        elif jstatus == 'failed':
            decided = 'failed'
            why = f'job#{jid} 실패'
        elif jstatus in ('queued', 'running'):
            # 진행 중 — stale 이고 실측이 있을 때만 정정한다.
            ts = str(job.get('update_time') or job.get('create_time') or '')
            try:
                old = _dt.fromisoformat(ts) < now - _td(seconds=stale_sec)
            except Exception:
                old = False
            if old and live in ('up', 'down'):
                decided = 'running' if live == 'up' else 'stopped'
                why = f'job#{jid} {jstatus} {stale_sec}s 초과 + 실측 {live}'
        if not decided or decided == r.get('status'):
            continue
        try:
            _deploy_update(config, r['id'], {'status': decided})
            fixed += 1
            logger.log_info(f"[deploy-sweep] deployment#{r['id']} "
                            f"({r.get('process_name')}) deploying → {decided} ({why})")
        except Exception as e:
            logger.log_warning(f"[deploy-sweep] deployment#{r.get('id')} 정정 실패: {e}")
    return fixed


def purge_old_jobs(config, retain_days: int = 2, retain_count: int = 200) -> int:
    """완료 job 정리 — 삭제 건수 반환.

    job 은 무한히 쌓인다. store 가 NFS 인 구성에서는 그 자체가 상시 비용이 되고
    (파일당 ~5ms), 콘솔 조회·픽 경로가 전부 느려진다. 종료 상태 job 만 대상으로,
    **개수 상한과 보존 기간을 함께** 적용한다(둘 중 하나라도 넘으면 오래된 것부터).

    미완(queued/running)은 절대 지우지 않는다 — 진행 중 작업을 잃으면 상태가 갈린다.
    """
    from datetime import datetime as _dt, timedelta as _td
    try:
        rows = _job_load_all(config)
    except Exception:
        return 0
    done = [j for j in rows if str(j.get('status') or '').lower() in _JOB_TERMINAL and j.get('id')]
    if not done:
        return 0
    cutoff = (_dt.now() - _td(days=max(0, int(retain_days)))).isoformat(timespec='seconds')

    def _ts(j):
        return str(j.get('completed_at') or j.get('update_time') or j.get('create_time') or '')

    done.sort(key=lambda j: (_ts(j), j.get('id') or 0))
    victims = [j for j in done if _ts(j) and _ts(j) < cutoff]
    keep_n = max(0, int(retain_count))
    if len(done) > keep_n:                      # 개수 상한 — 오래된 것부터 추가 삭제
        extra = done[:len(done) - keep_n]
        seen = {id(x) for x in victims}
        victims += [x for x in extra if id(x) not in seen]
    n = 0
    d = _job_dir(config)
    for j in victims:
        try:
            file_store.delete(d, j['id'])
            n += 1
        except Exception:
            pass
    return n


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
    now_iso = _dt.now().isoformat(timespec='seconds')
    # 인덱스는 **캐시**이지 정본이 아니다(정본 = control/jobs/*). 어긋나면 그 agent 의 job 이
    # 전부 조용히 무시되므로(실측: start job 이 큐에 갇혀 배포가 deploying 고착) 스스로
    # 복구해야 한다. 다만 **전수 스캔으로 복구하면 인덱스를 둔 이유가 사라진다**.
    #
    # job id 는 `.seq` 에서 단조 발급되므로, 인덱스에 적어둔 seq 이후 구간
    # `(idx_seq, cur_seq]` 이 곧 "인덱스가 모르는 job 후보" 다 — 그 몇 건만 확인해 흡수한다.
    # `.seq` 는 jobs 도메인 **공용**이라 다른 agent 의 job 만 늘어도 불일치가 나는데, 그때는
    # 구간 확인이 전부 miss 로 끝나고 seq 만 갱신된다(전수 스캔 없음).
    ids, idx_seq = _job_index_load(config, agent_id)
    cur_seq = _jobs_seq(config)
    if ids is None or idx_seq < 0:
        ids = _job_index_rebuild(config, agent_id)          # 부재/손상 — 1회 전수
    elif cur_seq >= 0 and idx_seq != cur_seq:
        gap = cur_seq - idx_seq
        if gap < 0 or gap > _JOB_INDEX_GAP_MAX:
            # seq 가 되돌아갔거나(store 교체·이관) 구간이 과도하게 크다 → 전수가 안전·저렴.
            logger.log_warning(f"[job-index] agent#{agent_id} seq 구간 이상"
                               f"(index={idx_seq} store={cur_seq}) — 전수 재구축")
            ids = _job_index_rebuild(config, agent_id)
        else:
            add = []
            for jid in range(idx_seq + 1, cur_seq + 1):
                j = _job_load(config, jid)
                if (j and j.get('agent_id') == agent_id
                        and j.get('status') == 'queued' and j.get('id')):
                    add.append(int(j['id']))
            if add:
                logger.log_info(f"[job-index] agent#{agent_id} 인덱스 누락 {add} 흡수 "
                                f"(seq {idx_seq}→{cur_seq})")
            ids = sorted(set(list(ids) + add))
            _job_index_save(config, agent_id, ids, seq=cur_seq)
    pending, stale = [], []
    for jid in sorted(ids):
        j = _job_load(config, jid)
        if not j or j.get('status') != 'queued' or j.get('agent_id') != agent_id:
            stale.append(jid)                         # 이미 처리됨/삭제됨 → 인덱스에서 제거
            continue
        if j.get('not_before') and str(j['not_before']) > now_iso:
            continue                                  # 지연 job — 인덱스에는 남긴다
        pending.append(j)
    if stale:
        _job_index_save(config, agent_id, [i for i in ids if i not in set(stale)])
        ids = [i for i in ids if i not in set(stale)]
    picked = pending[:limit]
    if picked:
        now = _dt.now().isoformat(timespec='seconds')
        for j in picked:
            j['status'] = 'running'
            j['dispatched_at'] = now
            file_store.save(_job_dir(config), j['id'], j)
        # 픽한 것은 더 이상 대기가 아니다 — 인덱스에서 뺀다.
        _picked_ids = {j['id'] for j in picked}
        _job_index_save(config, agent_id, [i for i in ids if i not in _picked_ids])
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


def resolve_pkg_file(config: dict, row: dict) -> str:
    """패키지 레코드의 **실제 파일 경로** — 기록된 절대경로가 아니라 현재 `Packages.Dir`
    기준으로 푼다. 찾지 못하면 빈 문자열.

    `Packages.Dir` 은 store 파생값(`{CimsRuntimeDir}/pkg_files`, oam_ha.md §4.0)이라 store
    이관과 함께 움직인다. 반면 레코드에는 등록 시점의 절대경로가 박히므로, 이관 뒤(특히
    **절체해 다른 노드에서 읽을 때**)에는 그 경로가 없어 "패키지 미등록"이 된다 — 파일은
    공유 store 에 그대로 있는데도. 실측 사고: 이관 후 standby 로 절체하면 `/agent-bundle.tar.gz`
    가 404 가 되어 agent·모듈 설치/업그레이드가 전면 불가.

    그래서 **파일명을 정본으로 보고 현재 저장소에서 찾는다.** 이러면 store 가 다시 옮겨져도
    레코드 이관이 필요 없다. 옛 레코드(`file_name` 없음)는 `file_path` 의 basename 으로 같은
    규칙에 태우고, 그래도 없으면 기록된 절대경로를 그대로 쓴다(단일 노드 legacy 보존).
    """
    fname = str(row.get("file_name") or "").strip() \
        or os.path.basename(str(row.get("file_path") or "").strip())
    if fname:
        pkg_dir, _ = _resolve_pkg_paths(config)
        cand = os.path.join(pkg_dir, fname)
        if os.path.isfile(cand):
            return cand
    legacy = str(row.get("file_path") or "").strip()
    return legacy if legacy and os.path.isfile(legacy) else ""


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
        # HA 판정 요약 {svc: {role,state,eligible,reasons,latched}} — 래치로 승격 불가인
        # 노드를 콘솔이 표시하는 근거(노드 로컬 파일이라 이 보고 없이는 OAM 이 모른다).
        "ha_state":        r.get("ha_state") if isinstance(r.get("ha_state"), dict) else None,
        # 이 agent 가 실제로 보고하는 OAM 주소 (heartbeat 보고값). 그룹 VIP 와 다르면
        # 절체 후 단절되므로 콘솔이 경고한다.
        "oam_url":         r.get("oam_url") if isinstance(r.get("oam_url"), str) else None,
        # **실제** 마운트 목록 [{target,fstype,source}] — cims-managed 아닌 기존 마운트 포함.
        # 공유 store 마운트 지점을 자유 입력 대신 이 목록에서 고르게 하는 근거 데이터.
        "mount_targets":   r.get("mount_targets") if isinstance(r.get("mount_targets"), list) else None,
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

    # POST /agents/oam-url — 전 agent 의 OAM 접속 주소 재지정 (이중화 전환: 노드 IP → VIP)
    if len(tail) == 1 and tail[0] == "oam-url" and method == "POST":
        return await _retarget_oam_url(handler_args, None, config)

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
        if action == "oam-url" and method == "POST":
            return await _retarget_oam_url(handler_args, aid, config)
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
        # 파일명이 위치의 정본 — 경로는 `Packages.Dir` 로 푼다 (resolve_pkg_file).
        # file_path 는 등록 시점 절대경로라 store 이관 후 무효해질 수 있어 참고용으로만 남긴다.
        'file_name': os.path.basename(fpath),
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
    moved = await asyncio.to_thread(_move_to_backup, resolve_pkg_file(config, row), backup_dir)
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
            # 시크릿(type=password)은 sentinel 로 가려 응답한다 — 콘솔 화면·API 로그에
            # 평문이 흐르지 않게. 저장 시 sentinel 은 '변경 없음' 으로 걸러진다(_strip_masked).
            "config":             _mask_secrets(cfg or {}, pkg.get("config_template")),
            "config_applied_at":  ca,
            "template":           pkg.get("config_template"),
            "meta":               pkg.get("meta"),
            "ha":                 ha_block,
        },
        media_type="application/json")


async def _put_deployment_config(handler_args, did: int, config):
    """설정 값 저장 — 항상 해당 deployment 에만. body = {
         "config":        {<key>: <value>, ...},   # 변경분 (기존 overlay 에 **병합**)
         "queue_update"?: bool (기본 true),
       }

    **병합 저장**(전체 교체 아님). 옛 동작은 body.config 로 overlay 를 통째로 교체해서,
    화면에 빈칸으로 보이던 값(예: 다른 노드에서 만들어진 `_infra` 시크릿)을 그대로 저장하면
    시크릿·런타임 경로가 overlay 에서 사라졌다 — 다음 update_config 에서 패키지 기본값으로
    회귀해 토큰 검증 불일치(전면 401)를 만드는 경로였다. 이제 온 키만 반영하고,
    **명시 삭제는 값 `null`** 로 표현한다(그룹 공통 저장 `_put_group_pkg_config` 와 동일 규칙).
    `type=password` 필드가 조회 sentinel 그대로 오면 '변경 없음' 으로 무시한다.

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
    _tmpl = None
    try:
        _pkg = await asyncio.to_thread(_pkg_load, config, dep.get("package_id"))
        _tmpl = (_pkg or {}).get("config_template") if isinstance(_pkg, dict) else None
        if isinstance(_tmpl, dict):
            values = _coerce_list_fields(_tmpl, values)
    except Exception as _e:
        logger.log_warning(f"deployment config list-coerce skip: {_e}")

    # 조회 sentinel 로 온 시크릿은 변경 없음 → 실제 저장값 보존.
    values = _strip_masked(values, _tmpl)

    # 스키마 마스크 — 템플릿 밖 키는 저장하지 않는다(들어오는 값 기준). 이미 저장된
    # 레거시 키는 여기서 건드리지 않는다: 평범한 저장이 다른 키를 조용히 지우면 안 되고,
    # 정리는 렌더 동치가 증명된 것만 하는 시작 시 스윕(_sweep_overlay_schema)이 맡는다.
    values, pruned_keys = _prune_to_template(values, _pkg or {},
                                             where=f"deployment#{dep['id']} 저장")

    # bind IP 가드 — `Server.Ip` 에 **VIP** 를 넣으면 그 VIP 를 보유하지 않은 노드에서
    #   bind 가 실패해 프로세스가 못 뜨고, watchdog 이 같은 설정으로 재기동을 반복한다
    #   (관리평면이면 콘솔이 사라져 수습 통로까지 없어진다). 콘솔 접속 주소는 설정값이
    #   아니라 "접속한 IP" 이므로 bind 는 0.0.0.0 이어야 한다 — oam_ha.md §8.
    _bind_ip = str(values.get("Server.Ip") or "").strip()
    if _bind_ip and _bind_ip not in ("0.0.0.0", "::", "127.0.0.1"):
        try:
            from services import ha_lookup
            _vips = set()
            for _g in ha_lookup.ha_groups_all(config):
                _vips |= set(ha_lookup.group_vip_set(_g) or [])
            if _bind_ip in _vips:
                return HandlerResult(status=400, body={
                    "error": "bind_ip_is_vip",
                    "detail": f"Server.Ip 에 VIP({_bind_ip})를 지정할 수 없습니다. VIP 를 보유하지 "
                              f"않은 노드에서 bind 가 실패해 기동 불능 루프가 됩니다. "
                              f"0.0.0.0 을 쓰세요 — 접속 주소는 설정값이 아니라 접속한 IP 입니다.",
                }, media_type="application/json")
        except Exception as _e:
            logger.log_warning(f"[deploy] bind IP VIP 검사 skip: {_e}")

    # 기존 overlay 에 병합 — 온 키만 반영, 값 null 은 명시 삭제.
    cur = dep.get("config")
    if not isinstance(cur, dict):
        cur = _safe_json(dep.get("config_json")) or {}
    new_overlay = dict(cur)
    removed = []
    for k, v in values.items():
        if v is None:
            if k in new_overlay:
                new_overlay.pop(k, None)
                removed.append(k)
        else:
            new_overlay[k] = v
    logger.log_info(f"deployment#{dep['id']} config 저장 — 병합 {len(values)}키"
                    + (f", 삭제 {removed}" if removed else ""))
    values = new_overlay

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
            # 템플릿에 없어 저장하지 않은 키 — 조용히 버리지 않고 호출자에게 돌려준다.
            "pruned_keys": pruned_keys,
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


def sweep_overlay_schema(config, *, apply: bool = True) -> dict:
    """저장된 overlay 에서 템플릿 밖 키 정리 — **렌더 동치가 증명된 것만** 지운다.

    write 경로(_prune_to_template)는 새 오염만 막는다. 이미 굳은 레거시 키는 여기서
    치우는데, 맹목적으로 지우면 안 된다 — 템플릿에 없어도 렌더 결과에 살아있는 키가 있다
    (예: `CimsRuntimeMount` 은 주입 대상이 아니라 overlay 가 유일한 출처였다. 지웠다면
    "store 가 마운트 하위가 아님" 가드에 걸려 OAM 이 기동을 거부한다).

    그래서 판정을 규칙으로 흉내내지 않고 **렌더 함수 자신을 오라클로** 쓴다:
    `_materialize_deploy_config(원본 overlay)` 와 `(정리된 overlay)` 의 결과가 완전히
    같을 때만 정리한다. 다르면 그 키는 살아있는 설정이므로 두고 경고만 남긴다
    (= config_template 에 선언되어야 한다는 신호).

    정리는 저장 레코드만 바꾼다 — 렌더 결과가 같으므로 job 을 큐잉하지 않는다(무중단).
    apply=False 면 판정만(dry-run). 반환 요약 dict.
    """
    out = {"scanned": 0, "cleaned": 0, "removed_keys": {}, "kept_keys": {}}
    for dep in _deploy_load_all(config) or []:
        overlay = _deploy_overlay(dep)
        if not overlay:
            continue
        out["scanned"] += 1
        pkg_file = _pkg_load(config, dep.get("package_id")) or {}
        keys = _template_key_set(pkg_file.get("config_template")
                                 if isinstance(pkg_file, dict) else None)
        if not keys:
            continue        # 템플릿 없는 패키지 — 검증 근거 없음, 손대지 않는다
        extra = [k for k in overlay if k not in keys]
        if not extra:
            continue
        did = dep.get("id")
        pruned = {k: v for k, v in overlay.items() if k in keys}
        before = _materialize_deploy_config(config, pkg_file, overlay)
        after = _materialize_deploy_config(config, pkg_file, pruned)
        if before != after:
            # 렌더에 영향 → 살아있는 설정. 지우지 않고 드러낸다.
            out["kept_keys"][did] = sorted(extra)
            logger.log_warning(
                f"[config-sweep] deployment#{did}: 템플릿 밖 키 {sorted(extra)} 가 렌더 결과에 "
                f"살아있어 정리하지 않음 — 해당 패키지 config_template 에 선언이 필요하다")
            continue
        out["removed_keys"][did] = sorted(extra)
        out["cleaned"] += 1
        if apply:
            _deploy_update(config, did, {"config": pruned})
            logger.log_info(f"[config-sweep] deployment#{did}: 템플릿 밖 키 {sorted(extra)} 정리 "
                            f"(렌더 결과 동일 — job 큐잉 없음)")
    return out


def _deploy_overlay(dep: dict) -> dict:
    """deployment 레코드의 overlay(사용자 의도 SoT) — dict 아니면 config_json 파싱."""
    cur = dep.get("config")
    if not isinstance(cur, dict):
        cur = _safe_json(dep.get("config_json")) or {}
    return cur


def _group_package_plan(config, group: dict, pkg_name: str) -> dict:
    """그룹×패키지 정합의 **전제 계산** — 판정(evaluate)·교정(reconcile) 공용, 읽기 전용.

    "무엇을 기준으로 무엇과 비교할 것인가" 를 한 곳에서만 정한다. 판정과 교정이 각자
    전제를 계산하면 콘솔이 보여주는 드리프트와 자동 교정이 실제로 바꾸는 것이 갈라진다.

    반환 {reason, active_agent_id, src, same_ver, deferred, svc_keys, src_overlay,
          pkg_file}. reason 이 None 일 때만 src/same_ver/svc_keys 가 유효하다.
    동기화 스위치는 여기서 보지 않는다 — 교정 여부만 좌우할 뿐, 정합 여부 판정은
    스위치 OFF 에서도 성립하므로 호출측(reconcile)이 따로 건다.
    """
    from services import ha_lookup
    plan: dict = {"reason": None, "active_agent_id": None, "src": None,
                  "same_ver": [], "deferred": [], "svc_keys": set(),
                  "src_overlay": {}, "pkg_file": {}, "deps": []}
    if group.get("mode") != "active_standby":
        plan["reason"] = "not_active_standby"
        return plan
    # 멤버 배포는 판정 불가 사유와 무관하게 먼저 채운다 — 판정이 보류돼도 화면은
    # 멤버 값을 나란히 보여줘야 한다(롤링 업그레이드 중 버전 혼재 창 등).
    deps = ha_lookup.deployments_in_group_for_package(config, group["id"], pkg_name)
    _enrich_deploy(deps, config)
    plan["deps"] = deps

    active_aid = ha_lookup.vip_observation(config, group)["active_agent_id"]
    plan["active_agent_id"] = active_aid
    if active_aid is None:
        plan["reason"] = "active_unknown"
        return plan

    src = next((d for d in deps if d.get("agent_id") == active_aid), None)
    if not src:
        plan["reason"] = "active_has_no_deployment"
        return plan
    targets = [d for d in deps if d.get("id") != src.get("id")]
    if not targets:
        plan["reason"] = "no_peers"
        return plan
    src_ver = src.get("package_version")
    plan["deferred"] = [{"deployment_id": t["id"],
                         "package_version": t.get("package_version")}
                        for t in targets if t.get("package_version") != src_ver]
    same_ver = [t for t in targets if t.get("package_version") == src_ver]
    if not same_ver:
        plan["reason"] = "version_mismatch"
        return plan

    _pkg = _pkg_load(config, src.get("package_id")) or {}
    plan.update({
        "src": src, "same_ver": same_ver, "pkg_file": _pkg,
        "svc_keys": _service_scope_keys(
            _pkg.get("config_template") if isinstance(_pkg, dict) else None),
        "src_overlay": _deploy_overlay(src),
    })
    return plan


def evaluate_group_package(config, group: dict, pkg_name: str) -> dict:
    """그룹×패키지 공통 설정 정합 **판정** (읽기 전용 dry-run — 쓰기·job 없음).

    reconcile_group_package 와 같은 전제(_group_package_plan)·같은 비교 규칙을 쓰므로
    "여기서 드리프트라고 표시된 것" = "자동 교정이 실제로 바꿀 것" 이 항상 일치한다.
    콘솔은 이 결과를 **표시만** 한다 — 멤버 설정을 각자 받아 브라우저에서 다시 비교하면
    판정 주체가 둘이 되어 어긋난다 (템플릿과 다른 패키지의 overlay 를 섞어 세는 유령
    드리프트가 그 사례).

    스위치 OFF 도 판정은 한다 — 멈추는 건 교정이지 정합 여부가 아니다. 호출측은
    auto_sync 필드로 "교정 대기" 와 "수동(교정 안 함)" 을 갈라 표시한다.

    AS 는 ACTIVE 를 기준으로 한 방향 판정(교정 가능), AA 는 기준 멤버가 없으므로
    "멤버 간 값이 같은가" 만 본다(교정 주체 없음 — action=None). 모드별로 판정 주체가
    갈리지 않도록 두 경우 모두 서버가 낸다.

    반환 {
      group_id, package, auto_sync,
      status:  'in_sync' | 'out_of_sync' | 'unknown',
      reason:  판정 불가 사유 (status='unknown' 일 때만),
      active_agent_id, compared_to: 판정 기준 멤버 | None(AA),
      drift:   [{key, action: 'copy'|'reset'|None, active, members[]}],
      deferred: 버전 혼재로 보류된 멤버,
      members: [{deployment_id, agent_id, agent_name, package_version,
                 values: {key: {v, src}}}]   # 표시용 실효값
    }
    action=copy 는 ACTIVE 값 복사, reset 은 overlay 제거(=템플릿 기본값 복귀)를 뜻한다.

    **members[].values 는 렌더 결과(실효값)** 다 — overlay + 템플릿 기본값 + 배포 시 주입을
    모두 반영한 `_materialize_deploy_config` 의 산출물. 화면이 overlay 만 보고 표시하면
    (a) 주입으로 채워지는 값(JwtSecret 등)이 빈칸으로 보여 "시크릿 없음"으로 오해되고,
    (b) 판정은 overlay 기준인데 표시는 다른 기준이라 "값이 같은데 드리프트"가 된다.
    `src` 가 그 차이를 드러낸다: overlay(운영자 지정) / injected(배포 시 주입) /
    default(템플릿 기본값 — overlay 미설정).
    """
    from services import ha_lookup
    out: dict = {"group_id": group.get("id"), "package": pkg_name,
                 "auto_sync": ha_lookup.auto_sync_enabled(group, pkg_name),
                 "status": "unknown", "reason": None, "active_agent_id": None,
                 "compared_to": None, "drift": [], "deferred": [], "members": []}

    def _member_values(dep):
        """멤버의 표시용 실효값 — 렌더 결과 + 값의 출처. 멤버마다 **자기 버전의
        템플릿**으로 계산한다(버전 혼재 창에서도 각자 맞는 필드로 보이게)."""
        pkg_file = _pkg_load(config, dep.get("package_id")) or {}
        tmpl = pkg_file.get("config_template") if isinstance(pkg_file, dict) else None
        show = _masker(pkg_file)
        overlay = _deploy_overlay(dep)
        mat = _materialize_deploy_config(config, pkg_file, overlay)
        defaults = _template_defaults(tmpl)
        vals = {}
        for k in _template_key_set(tmpl):
            v = mat.get(k)
            if k in overlay:
                src = "overlay"
            elif v is not None and v != defaults.get(k):
                src = "injected"        # 배포 시 주입 (base 신원·경로 등)
            else:
                src = "default"
            vals[k] = {"v": show(k, v), "src": src}
        return {"deployment_id": dep.get("id"), "agent_id": dep.get("agent_id"),
                "agent_name": dep.get("agent_name"),
                "package_version": dep.get("package_version"), "values": vals}

    def _masker(pkg_file):
        # 값 노출 — 시크릿은 조회 응답 관용대로 sentinel (_get_deployment_config 와 동일).
        tmpl = pkg_file.get("config_template") if isinstance(pkg_file, dict) else None
        pw_keys = _password_keys(tmpl)
        return lambda k, v: (_SECRET_MASK if (k in pw_keys and v) else v)

    def _member_view(dep, overlay, key, show):
        return {"deployment_id": dep.get("id"), "agent_id": dep.get("agent_id"),
                "agent_name": dep.get("agent_name"),
                "value": show(key, overlay.get(key)), "present": key in overlay}

    if group.get("mode") != "active_standby":
        # ── AA/standalone — 기준(ACTIVE) 없음: 멤버 간 동일성만 본다 ──
        deps = ha_lookup.deployments_in_group_for_package(config, group["id"], pkg_name)
        _enrich_deploy(deps, config)
        out["members"] = [_member_values(d) for d in deps]
        if len(deps) < 2:
            out["reason"] = "no_peers"
            return out
        ref = min(deps, key=lambda d: d.get("id") or 0)
        ref_ver = ref.get("package_version")
        out["deferred"] = [{"deployment_id": d["id"],
                            "package_version": d.get("package_version")}
                           for d in deps if d.get("package_version") != ref_ver]
        peers = [d for d in deps if d.get("package_version") == ref_ver]
        if len(peers) < 2:
            out["reason"] = "version_mismatch"
            return out
        pkg_file = _pkg_load(config, ref.get("package_id")) or {}
        show = _masker(pkg_file)
        svc_keys = _service_scope_keys(
            pkg_file.get("config_template") if isinstance(pkg_file, dict) else None)
        views = [(p, _deploy_overlay(p)) for p in peers]
        first = views[0][1]
        for k in sorted(svc_keys):
            if all((k in ov) == (k in first) and ov.get(k) == first.get(k)
                   for _p, ov in views[1:]):
                continue
            out["drift"].append({"key": k, "action": None, "active": None,
                                 "members": [_member_view(p, ov, k, show)
                                             for p, ov in views]})
        out["status"] = "out_of_sync" if out["drift"] else "in_sync"
        return out

    plan = _group_package_plan(config, group, pkg_name)
    out["active_agent_id"] = plan["active_agent_id"]
    out["deferred"] = plan["deferred"]
    out["members"] = [_member_values(d) for d in plan["deps"]]
    if plan["reason"]:
        out["reason"] = plan["reason"]
        return out

    src, src_overlay = plan["src"], plan["src_overlay"]
    out["compared_to"] = {"deployment_id": src.get("id"),
                          "agent_id": src.get("agent_id"),
                          "agent_name": src.get("agent_name"),
                          "package_version": src.get("package_version")}
    show = _masker(plan["pkg_file"])
    for k in sorted(plan["svc_keys"]):
        in_src = k in src_overlay
        members = []
        for t in plan["same_ver"]:
            cur = _deploy_overlay(t)
            # 교정 규칙과 1:1 — ACTIVE 에 있으면 값 비교, 없으면 '남아있는가' 가 곧 드리프트.
            differs = (cur.get(k) != src_overlay[k]) if in_src else (k in cur)
            if differs:
                members.append(_member_view(t, cur, k, show))
        if members:
            out["drift"].append({"key": k, "action": "copy" if in_src else "reset",
                                 "active": show(k, src_overlay.get(k)) if in_src else None,
                                 "members": members})
    out["status"] = "out_of_sync" if out["drift"] else "in_sync"
    return out


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
        # 스위치 OFF 는 교정만 멈춘다 — 정합 여부 판정은 evaluate_group_package 가 계속 한다.
        out["reason"] = "switch_off"
        return out
    plan = _group_package_plan(config, group, pkg_name)
    out["active_agent_id"] = plan["active_agent_id"]
    out["deferred"] = plan["deferred"]
    if plan["reason"]:
        out["reason"] = plan["reason"]
        return out

    src, same_ver = plan["src"], plan["same_ver"]
    svc_keys, src_overlay = plan["svc_keys"], plan["src_overlay"]
    active_aid = plan["active_agent_id"]
    _pkg = plan["pkg_file"]
    template = _pkg.get("config_template") if isinstance(_pkg, dict) else None

    # ── scalar 정합: ACTIVE overlay 의 service 키 기준 merge / 제거(기본값 복귀)
    saved: list[dict] = []
    synced_keys: set = set()
    removed_keys: set = set()
    for t in same_ver:
        cur = _deploy_overlay(t)
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
            # status(배포기록=**의도**)와 달리 실제 프로세스 생존을 반영 (metric 주기 지연).
            #
            # **과도 상태(deploying)에서도 판정한다.** 종전엔 status 가 running/stopped 일
            # 때만 계산해서, 배포 job 이 큐에 갇히면 프로세스가 멀쩡히 도는데도 화면이
            # 영원히 "배포 중" 이었다(실측). 과도 상태가 끝나지 않을 수 있다는 걸 전제해야
            # 한다 — 실측을 감추면 운영자가 현실을 볼 통로가 없어진다.
            # pending(미설치)은 제외 — 아직 그 노드에 없으므로 "없음" 이 정상이라 down 이
            # 의미를 갖지 않는다. removed 도 제외.
            lm = ag.get('live_modules')
            if (ag.get('status') == 'online' and isinstance(lm, list)
                    and r.get('status') in ('running', 'stopped', 'deploying', 'failed')):
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
    # enrich 는 package/agent 를 file_store 에서 읽는다(캐시하지만 여전히 파일 I/O) —
    # 콘솔이 2초마다 폴링하므로 이벤트 루프에서 돌리면 heartbeat·job 결과 처리가 밀린다.
    await asyncio.to_thread(_enrich_deploy, rows, config)
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
    # 스키마 마스크 — 등록 시점(부트스트랩·프로비저닝 포함)에 템플릿 밖 키를 막는다.
    # 부트스트랩이 기동 중 config.json 을 통째로 스냅샷해 보내면 템플릿에 없는 키까지
    # desired state 로 굳는데, 그 오염이 다른 패키지 화면으로 새는 경로였다.
    cfg_overlay, pruned_keys = _prune_to_template(
        cfg_overlay, pkg_file, where=f"deployment 등록(agent#{agent_id} pkg#{package_id})")
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

    # 단일 writer 자원 소유 모듈(requires_leader_lease)의 2번째 노드 설치 — 전제 검사.
    # 그룹 공통 신원 주입 확인 — 관리평면 모듈은 노드 경로·시크릿이 **반드시** 주입돼야 한다.
    # 빠지면 패키지 기본값으로 기동해 (a) 빌드 머신 경로에 store 를 만들려다 죽거나
    # (b) 노드 로컬 시크릿을 써서 절체 후 전 노드가 401 이 된다(둘 다 실측 사고).
    # 주입은 패키지 meta 의 선언에 의존하므로, 선언이 유실되면 **조용히** 누락된다 —
    # 그래서 여기서 확인하고 거부한다.
    try:
        if (pkg_meta or {}).get("shared_identity") or process_name in ("oam", "oam-svc"):
            _eff = _materialize_deploy_config(config, pkg_file, cfg_overlay)
            _missing = [k for k in ("CimsRuntimeDir", "CimsAuth.JwtSecret")
                        if not str(_eff.get(k) or "").strip()]
            if _missing:
                return HandlerResult(status=409, body={
                    "error": "shared_identity_missing",
                    "detail": (f"'{process_name}' 에 그룹 공통 신원이 주입되지 않았습니다: "
                               f"{_missing}. 패키지 meta 의 `shared_identity` 선언이 유실됐거나 "
                               f"현재 OAM 설정에 그 값이 없습니다. 이대로 설치하면 노드마다 "
                               f"다른 경로·시크릿으로 기동해 절체 시 인증이 전부 실패합니다. "
                               f"패키지를 다시 빌드·업로드하세요."),
                    "missing": _missing}, media_type="application/json")
    except HandlerResult:
        raise
    except Exception as _e:
        logger.log_warning(f"[deploy] 신원 주입 확인 skip({process_name}): {_e}")

    # 전제(공유 store) 미충족 알림 — **차단하지 않는다**. 설치만 된 standby 는 HA 편입에서
    # 제외돼(§6.3) 승격돼도 기동되지 않으므로 무해하고, 볼륨을 먼저 요구하면 이중화 구축
    # 순서가 뒤집힌다. 차단은 위험한 액션인 기동에서 한다(_leader_lease_start_guard).
    _lease_notice = await asyncio.to_thread(_leader_lease_install_notice,
                                            config, agent_id, process_name)
    if _lease_notice:
        logger.log_warning(f"[deploy] agent#{agent_id} {process_name}: {_lease_notice}")

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
    #    배포 config 의 Server.Port(SoT)로 게이트웨이에 등록+hot-mount(role base).
    #    공용 헬퍼 — job 성공 보고(agent_api._report)에서도 같은 경로로 재등록된다.
    try:
        await asyncio.to_thread(self_register_deployment_routes, config, r)
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

    _created = _deployment_to_json(r)
    if pruned_keys:
        _created["pruned_keys"] = pruned_keys
    if _lease_notice:
        # 조용히 성공시키면 운영자는 이중화가 된 줄 안다 — 응답에 사유를 싣는다.
        _created["warning"] = _lease_notice
        _created["warning_code"] = "leader_lease_precondition"
    return HandlerResult(status=201, body=_created, media_type="application/json")


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


def _leader_lease_unmet(config, agent_id: int, proc: str) -> bool:
    """`requires_leader_lease` 모듈인데 그 전제(공유 store)가 없는 A/S 그룹의 2번째 노드인가.

    선언을 키로 판정한다(모듈 이름 하드코딩 없음). 그룹 명세 오버라이드가 있으면 그것을
    따르고, 그룹에 이 노드 말고 다른 멤버가 없으면(단일 구성) 전제는 문제되지 않는다.
    """
    proc = (proc or "").lower().strip()
    if not proc:
        return False
    try:
        from services import ha_lookup, service_registry
        mods = service_registry.all_modules(config) or {}
        if not ((mods.get(proc) or {}).get("safety") or {}).get("requires_leader_lease"):
            return False
        from handlers.ha_groups import _normalize_shared_store, _module_spec
        for g in ha_lookup.ha_groups_all(config):
            if g.get("mode") != "active_standby":
                continue
            members = [m.get("agent_id") for m in ha_lookup.members_of(g)]
            if agent_id not in members or len(members) < 2:
                continue
            # 그룹 명세가 선언을 덮었으면 그것을 따른다(운영자 판단 우선).
            if not _module_spec(g, proc)["safety"].get("requires_leader_lease"):
                return False
            if _normalize_shared_store(g.get("shared_store")):
                return False                    # 전제 충족 — 정상 이중화 구성
            return True
    except Exception as e:
        logger.log_warning(f"[deploy] leader-lease 전제 검사 skip: {e}")
    return False


def _leader_lease_install_notice(config, agent_id: int, proc: str) -> "str | None":
    """설치는 **막지 않고 알린다** — 설치 자체는 위험하지 않다.

    위험한 상태는 "두 노드에서 각자 도는 OAM" 이지 "설치돼 있음" 이 아니다. 전제 미충족
    모듈은 이미 렌더 단계에서 HA 편입(cold·relevant·health)에서 제외되므로(§6.3), 설치만
    된 standby 노드는 **승격돼도 기동되지 않는** 무해한 상태다. 오히려 볼륨을 먼저 요구하면
    마운트 지점이 모듈 디렉터리 하위라 순서가 뒤집히고, 이중화 구축 자체가 막힌다.

    그래서 설치는 허용하고 (1) 서버 로그 (2) 응답 `warning` 으로 "아직 이중화되지 않는다"
    를 알린다. 실제 차단은 위험한 액션인 **기동**(`_leader_lease_start_guard`)에서 한다.
    """
    if not _leader_lease_unmet(config, agent_id, proc):
        return None
    return (f"'{proc}' 는 설치되지만 **아직 이중화되지 않습니다** — 이 그룹에 공유 store 가 "
            f"설정되지 않아 HA 편입(절체 대상)에서 제외됩니다. 설정 없이 두 노드에서 동시에 "
            f"기동하면 각자 자기 노드 디스크에 관리 데이터를 쌓아, 콘솔이 보는 내용이 VIP "
            f"위치에 따라 바뀝니다. HA 화면의 '공유 store' 를 설정하면 자동 편입됩니다.")


def _leader_lease_start_guard(config, dep: dict) -> "str | None":
    """`requires_leader_lease` 모듈의 **동시 기동 차단** — 전제 미충족 시.

    이것이 실제로 위험한 액션이다. 공유 store 가 없으면 노드마다 독립 store 를 가지므로,
    같은 그룹의 다른 노드에서 이미 도는 모듈을 이 노드에서 또 띄우면 **VIP 위치에 따라
    콘솔이 다른 데이터를 보여준다**(실측 사고 — 절체 후 서버·그룹이 전부 사라져 보임).
    리스는 같은 store 루트 안에서만 작동하므로 이 경우를 막아주지 못한다.

    상대 노드의 그 모듈이 정지 상태면 허용한다 — 공유 store 없이 수동 이관하는 경로(§9.4)이고,
    store 이관 책임은 운영자에게 있다. body `force: true` 로 우회 가능.
    """
    proc = (dep.get("process_name") or dep.get("package_name") or "").lower().strip()
    aid = dep.get("agent_id")
    if not _leader_lease_unmet(config, aid, proc):
        return None
    try:
        from services import ha_lookup
        for g in ha_lookup.ha_groups_all(config):
            if g.get("mode") != "active_standby":
                continue
            members = [m.get("agent_id") for m in ha_lookup.members_of(g)]
            if aid not in members:
                continue
            others = [a for a in members if a != aid]
            running = [d for d in _deploy_load_all(config)
                       if d.get("agent_id") in others
                       and (d.get("process_name") or "").lower().strip() == proc
                       and (d.get("status") or "") in ("running", "deploying")]
            if running:
                return (f"'{proc}' 가 같은 그룹의 다른 노드에서 이미 동작 중입니다. 이 그룹은 "
                        f"공유 store 가 없어 관리 store 가 노드마다 독립이므로, 동시에 기동하면 "
                        f"절체 시 콘솔이 보는 관리 데이터가 통째로 바뀝니다(서버·그룹이 사라져 "
                        f"보임). 이중화하려면 HA 화면에서 '공유 store' 를 설정하세요. "
                        f"수동 이관이면 먼저 상대 노드의 '{proc}' 를 정지하세요.")
    except Exception as e:
        logger.log_warning(f"[deploy] leader-lease 기동 가드 검사 skip: {e}")
    return None


def _plane_upgrade_order_guard(config, dep: dict) -> "str | None":
    """관리평면 모듈(oam/oam-svc)의 **Active 직접 upgrade** 차단 — 순서가 있다.

    관리평면은 자기 자신을 업그레이드하는 유일한 모듈이다. Active 를 먼저 올리면 새 버전이
    기동 실패할 때 **콘솔이 사라져** 롤백을 지시할 통로가 없다. 안전한 순서는
    **standby 먼저 → 계획 절체 → 구 Active** 다(oam_ha.md §11 · oam_self_upgrade.md).
    standby 가 없거나(단일 노드) 이 노드가 Active 가 아니면 제약하지 않는다.
    body `force: true` 로 우회 가능 — 판단은 운영자 몫이되, 기본은 안전한 순서다."""
    proc = (dep.get("process_name") or dep.get("package_name") or "").lower().strip()
    if proc not in ("oam", "oam-svc"):
        return None
    try:
        from services import ha_lookup
        aid = dep.get("agent_id")
        for g in ha_lookup.ha_groups_all(config):
            if g.get("mode") != "active_standby":
                continue
            members = [m.get("agent_id") for m in ha_lookup.members_of(g)]
            if aid not in members or len(members) < 2:
                continue
            obs = ha_lookup.vip_observation(config, g) or {}
            if obs.get("active_agent_id") == aid:
                peers = [m for m in members if m != aid]
                return (f"관리평면({proc}) 업그레이드는 **standby 먼저** 입니다. 이 서버는 현재 "
                        f"Active(VIP 보유)라 여기서 먼저 올리면 실패 시 콘솔이 사라져 롤백을 "
                        f"지시할 수 없습니다. 순서: ① standby(agent#{peers[0]}) upgrade → "
                        f"② 수동 절체 → ③ 이 서버 upgrade. 강제하려면 force:true.")
    except Exception as e:
        logger.log_warning(f"[upgrade] 순서 가드 검사 skip: {e}")
    return None


async def _retarget_oam_url(handler_args: HandlerArgs, aid, config):
    """agent 의 OAM 접속 주소 재지정 — body `{url, agent_ids?[]}`. aid 지정 시 그 agent 만.

    이중화 전환(노드 IP → VIP)의 정규 경로다. 옛 구조에서는 주소가 agent 의 systemd unit
    인자에만 있어 **재설치 말고는 바꿀 방법이 없었다**(oam_ha.md §8).

    안전장치 2중:
      1) 여기서 URL 형식·포트를 검증하고, 미지정 시 `Server.AgentOamUrl`(콘솔에 설정된 주소)을
         기본값으로 쓴다 — 오타를 손으로 다시 입력할 이유를 없앤다.
      2) **각 agent 가 전환 전에 그 주소로 /health 를 찔러 도달 확인**한다(job 내부). 도달
         불가면 주소를 바꾸지 않고 job 이 실패한다 — VIP 가 아직 없을 때 전 fleet 이
         OAM 과 단절되는 것을 막는다.
    """
    from urllib.parse import urlparse as _up
    body = _parse_body(handler_args) or {}
    url = str(body.get("url") or "").strip().rstrip("/")
    if not url:
        url = _oam_public_url(handler_args, config)
    p = _up(url)
    if p.scheme not in ("http", "https") or not p.hostname:
        return HandlerResult(status=400, body={"error": "invalid_url", "url": url},
                             media_type="application/json")

    # **도달성 사전 확인** — 각 agent 가 도달 확인 후에만 적용하므로 안전하지만, VIP 가 아직
    # 없으면 6개 job 이 전부 조용히 실패하고 콘솔은 "큐잉" 만 알려 운영자가 성공으로 오해한다
    # (실측: 첫 전환이 그렇게 실패했고 화면엔 아무 표시가 없었다). OAM 은 VIP 를 보유한
    # 노드에서 돌므로 여기서 먼저 찔러 실패를 즉시 알린다.
    try:
        import ssl as _ssl
        import urllib.request as _ur
        _ctx = _ssl.create_default_context()
        _ctx.check_hostname = False
        _ctx.verify_mode = _ssl.CERT_NONE
        await asyncio.to_thread(
            lambda: _ur.urlopen(_ur.Request(url + "/health"), timeout=5, context=_ctx).read(1))
    except Exception as _e:
        return HandlerResult(status=409, body={
            "error": "url_unreachable",
            "detail": (f"{url}/health 에 도달할 수 없습니다({type(_e).__name__}). VIP 가 아직 "
                       f"올라오지 않았거나 주소가 잘못됐습니다. 이대로 보내면 agent 들이 "
                       f"주소를 바꾸지 않고 job 만 실패합니다 — 그룹을 시작해 VIP 를 띄운 뒤 "
                       f"다시 실행하세요."),
        }, media_type="application/json")
    if aid is not None:
        targets = [aid]
    else:
        ids = body.get("agent_ids")
        if isinstance(ids, list) and ids:
            targets = [int(x) for x in ids]
        else:
            rows = await asyncio.to_thread(_agent_load_all, config)
            targets = [int(r["id"]) for r in rows if r.get("id") is not None
                       and r.get("status") != "revoked"]
    jobs = []
    for t in targets:
        try:
            jid = await asyncio.to_thread(_job_create, config, t, "set_oam_url", {"url": url})
            jobs.append({"agent_id": t, "job_id": jid})
        except Exception as e:
            logger.log_warning(f"[oam-url] agent#{t} job 큐잉 실패: {e}")
    logger.log_info(f"[oam-url] 재지정 요청 url={url} agents={len(jobs)}건 "
                    f"(각 agent 가 도달 확인 후 적용)")
    return HandlerResult(status=202, body={"url": url, "jobs": jobs},
                         media_type="application/json")


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
    # 관리평면 업그레이드 순서 가드 (standby 먼저) — force:true 로 우회 가능.
    if job_type == "upgrade" and not body.get("force"):
        _g = await asyncio.to_thread(_plane_upgrade_order_guard, config, dep)
        if _g:
            return HandlerResult(status=409, body={"error": "upgrade_order_active_first",
                                                   "detail": _g},
                                 media_type="application/json")
    # 단일 writer 자원 모듈의 **동시 기동** 가드 — 공유 store 없이 두 노드에서 도는 것을 막는다.
    # 설치는 막지 않는다(무해) — 위험한 액션은 기동이다. force:true 로 우회 가능.
    if job_type in ("start", "restart") and not body.get("force"):
        _g = await asyncio.to_thread(_leader_lease_start_guard, config, dep)
        if _g:
            return HandlerResult(status=409, body={"error": "leader_lease_precondition",
                                                   "detail": _g},
                                 media_type="application/json")
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
    # 롤백도 관리평면 버전 변경이므로 같은 순서 제약을 받는다(standby 먼저).
    # force:true 로 우회 가능.
    if not body.get("force"):
        _g = await asyncio.to_thread(_plane_upgrade_order_guard, config, dep)
        if _g:
            return HandlerResult(status=409, body={"error": "upgrade_order_active_first",
                                                   "detail": _g},
                                 media_type="application/json")
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
    # 경로는 현재 `Packages.Dir` 로 푼다 — 레코드의 절대경로는 이관 전 값일 수 있다.
    agent_pkgs = [(p, resolve_pkg_file(config, p)) for p in items if p.get("name") == "agent"]
    agent_pkgs = [(p, fp) for p, fp in agent_pkgs if fp]
    if not agent_pkgs:
        return None
    agent_pkgs.sort(key=lambda t: t[0].get("uploaded_at") or "", reverse=True)
    return agent_pkgs[0][1]


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


# ── API 문서 (개발자 모드) ──────────────────────────────────────────────────
#  이 모듈이 제공하는 엔드포인트 중 **노드 사양·자원 사용률 조회(읽기)만** 선언한다.
#  등록/승인/삭제/제어/패키지/배포/sync/drift 같은 내부 운영 API 는 외부 공유 대상이 아니므로 선언하지
#  않는다 (docs/design/features/api_docs.md §5). module=None — base OAM 상주라 항상 가용.
_AUTH_MONITOR = {'scheme': 'bearer', 'role': 'monitor', 'token_from': 'POST /api/v1/auth/login'}

_ERR_COMMON = [
    {'status': 401, 'when': 'Authorization 헤더 없음 / 토큰 만료', 'body': {'error': 'unauthorized'}},
    {'status': 403, 'when': '권한 등급 미달', 'body': {'error': 'forbidden'}},
]

CIMS_AGENT_API_DOCS = [
    {'id': 'nodes.list', 'module': None, 'method': 'GET', 'path': '/api/v1/agents',
     'summary': '노드 목록 + 사양·상태 (hostname/ip/OS, cpu_cores·memory_mb·disk_gb, 최근 heartbeat)',
     'params': [],
     'response': '{items:[{id, name, status, hostname, ip_address, os_info, cpu_cores, memory_mb, '
                 'disk_gb, agent_version, last_heartbeat, ...}]}',
     'response_fields': [
         {'name': 'items[].id', 'type': 'integer', 'desc': '노드 id — nodes.metrics 의 path 파라미터'},
         {'name': 'items[].name', 'type': 'string', 'desc': '노드 이름 (등록 시 지정)'},
         {'name': 'items[].status', 'type': 'string', 'enum': ['online', 'offline', 'pending'],
          'desc': 'heartbeat 기반 상태'},
         {'name': 'items[].hostname', 'type': 'string', 'desc': 'OS hostname'},
         {'name': 'items[].ip_address', 'type': 'string', 'desc': '관리 IP'},
         {'name': 'items[].os_info', 'type': 'string', 'desc': 'OS 배포판/커널 문자열'},
         {'name': 'items[].cpu_cores', 'type': 'integer', 'unit': '코어', 'desc': '논리 코어 수'},
         {'name': 'items[].memory_mb', 'type': 'integer', 'unit': 'MB', 'desc': '총 메모리'},
         {'name': 'items[].disk_gb', 'type': 'integer', 'unit': 'GB', 'desc': '총 디스크'},
         {'name': 'items[].agent_version', 'type': 'string', 'desc': '설치된 agent 버전'},
         {'name': 'items[].last_heartbeat', 'type': 'string', 'desc': 'ISO8601 — 마지막 heartbeat 수신 시각'},
         {'name': 'items[].last_metric', 'type': 'string', 'desc': 'ISO8601 — 마지막 자원 수집 시각'},
     ],
     'example': {'items': [{'id': 1, 'name': 'csc01', 'status': 'online', 'hostname': 'csc01',
                            'ip_address': '10.0.1.11', 'os_info': 'Ubuntu 24.04 / 6.8.0',
                            'cpu_cores': 8, 'memory_mb': 16384, 'disk_gb': 200,
                            'agent_version': '0.2.62',
                            'last_heartbeat': '2026-07-30T09:12:03',
                            'last_metric': '2026-07-30T09:12:00'}]},
     'errors': list(_ERR_COMMON),
     'notes': ['응답에는 운영용 필드(승인 상태·enrollment 등)도 함께 오지만, 사용량 연동에 필요한 것은 '
               '위 필드다.',
               'enrollment 토큰은 생성 직후에만 반환되고 목록에서는 마스킹된다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'nodes.metrics', 'module': None, 'method': 'GET', 'path': '/api/v1/agents/{id}/metrics',
     'summary': '노드 자원 사용률 시계열 (CPU/메모리/디스크/load, 프로세스·인터페이스·마운트별)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'integer', 'required': True,
                 'desc': '노드 id (nodes.list 의 items[].id)'}],
     'response': '{items:[{ts, cpu_pct, mem_pct, disk_pct, load_avg, processes[], per_iface[], mounts[]}]}',
     'response_fields': [
         {'name': 'items[].ts', 'type': 'string', 'desc': 'ISO8601 — 수집 시각 (오래된 것부터)'},
         {'name': 'items[].cpu_pct', 'type': 'number', 'unit': '%', 'desc': 'CPU 사용률'},
         {'name': 'items[].mem_pct', 'type': 'number', 'unit': '%', 'desc': '메모리 사용률'},
         {'name': 'items[].disk_pct', 'type': 'number', 'unit': '%', 'desc': '루트 파일시스템 사용률'},
         {'name': 'items[].load_avg', 'type': 'number', 'desc': '1분 load average'},
         {'name': 'items[].processes[]', 'type': 'object',
          'desc': '감시 대상 프로세스별 상태 (name/pid/cpu_pct/rss_mb)'},
         {'name': 'items[].per_iface[]', 'type': 'object',
          'desc': '네트워크 인터페이스별 송수신량 (iface/rx_bps/tx_bps)'},
         {'name': 'items[].mounts[]', 'type': 'object',
          'desc': '마운트별 디스크 사용량 (path/used_gb/total_gb/pct)'},
     ],
     'example': {'items': [{'ts': '2026-07-30T09:10:00', 'cpu_pct': 12.4, 'mem_pct': 38.1,
                            'disk_pct': 22.0, 'load_avg': 0.35,
                            'processes': [{'name': 'csc', 'pid': 1234, 'cpu_pct': 3.1, 'rss_mb': 210}],
                            'per_iface': [{'iface': 'eth0', 'rx_bps': 812000, 'tx_bps': 430000}],
                            'mounts': [{'path': '/', 'used_gb': 44, 'total_gb': 200, 'pct': 22.0}]}]},
     'errors': _ERR_COMMON + [
         {'status': 404, 'when': '해당 id 의 노드 없음', 'body': {'error': 'not_found'}},
     ],
     'notes': ['최근 120개 표본 / 최대 7일 범위를 반환한다 (수집 주기에 따라 구간이 달라진다).',
               '표본이 없으면 items 는 빈 배열이다 (오류 아님).'],
     'auth': dict(_AUTH_MONITOR)},
]

# 인증 없이 누구나 받을 수 있는 배포용 정적 에셋
CIMS_AGENT_PUBLIC_HANDLER_LIST = (
    ("/install-agent.sh",   _serve_install_script, {}),
    ("/cims_agent.py",      _serve_agent_binary,   {}),
    ("/agent-bundle.tar.gz", _serve_agent_bundle,  {}),
)
