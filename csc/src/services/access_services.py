"""접속 서비스(`access_services`) 읽기 — CSC 소비자의 단일 진입점 (sip_service_model.md §2-9, sip_statistics.md §3.1).

SoT 는 CSP 의 `<install_path>/config/access_services.jsonl` 이다. 콘솔 편집은 OAM → agent 로 그 파일에만 쓰고, OAM 이 관리
store 에 읽기 전용 미러(`ha_lookup.collection_dir(config, 'access_services')`)를 따라 갱신한다. CSC 에는 agent 클라이언트가 없어
이 미러가 유일한 경로다 — 미러를 고쳐도 CSP 에는 반영되지 않는다(두 번째 쓰기 경로가 아니다).

CSC 가 접속 서비스에서 얻는 것은 두 겹이다.
  · **정의(identity·정책)** — name·kind·domain·auth_realm·media_srtp·sec_mechanisms·pickup_feature_code·transfer_allowed…:
    미러 레코드가 정본. 가입 행 `service_ref`(= name) 로 고르고, 없으면 종류(kind)의 첫 enabled 레코드(priority 오름차순 —
    CSP `CCspServiceMap::GetForUser(user, kind)` 와 같은 순서).
  · **단말 도달 정보** — host·port·tcp_port·tls_port·transport(권장 기본)·ipsec 포트쌍·sms_gateway·payload 상한: CSP 레코드에
    없는 값이라 csc.json `Provisioning.Services.<kind>` 가 가진다(`entry()` 가 두 겹을 합친다).
미러가 없거나 비어 있으면(미배포·store 비공유·구 배포) csc.json 항목의 domain 등을 **폴백**으로 쓴다 — 폴백 값은 운영
규약으로 CSP 와 맞춰야 하며, 미러와 어긋나면 드리프트 경고를 남긴다(이름당 1회).

왜 하나로 모으는가: 소비자(프로비저닝 `service_entry`·H(A1) 결박 `_service_realm`·관제 앱 번호 개설 후보 `_services`·IdMS
도메인 유도)가 각자 경로를 갖다 한쪽만 미러를 보게 됐고, `Provisioning.Services.ptt.name`(키 `ptt`)과 CSP name(`mcptt`)이
어긋나는 문제의 뿌리가 됐다. 읽기 경로는 여기 하나다.
"""
from __future__ import annotations

import threading
from typing import Optional

from services import file_store, ha_lookup
from util.log_util import Logger

logger = Logger()

COLLECTION = 'access_services'

# 접속환경 클래스(kind) = 가입 테이블 하나(services.subscriptions — volte/voip/ptt). service_ref 매칭은 **exact kind**
#   (ptt≡mcptt) 안에서만 한다: voip 회선이 volte 서비스 name 을 가리키면 해당 없음(쓰기 게이트 400 service_kind_mismatch).
#   전화 가족(volte∪voip)은 `family()` — 로그/통계 축·전화번호부 합산 같은 축 용도이며 서비스 해석에는 쓰지 않는다.
PHONE_KINDS = ('volte', 'voip')
PTT_KINDS = ('ptt', 'mcptt')

# 미러 레코드에서 csc.json 항목 위로 덧쓰는 정의 필드(CSP 소유 값). 도달 정보(host·포트·transport 등)는 덧쓰지 않는다.
IDENTITY_FIELDS = ('name', 'kind', 'domain', 'auth_realm', 'media_srtp', 'sec_mechanisms',
                   'pickup_feature_code', 'transfer_allowed', 'media_nat_mode')

_config: dict = {}                 # configure() 로 받은 csc 설정 — collection_dir 해석용(CimsRuntimeDir/ServiceLogging.Dir)
_lock = threading.Lock()
_warned: set = set()               # 드리프트·미도달 경고 1회 게이트


def configure(config: dict) -> None:
    """apply_config 에서 1회 — 이후 config 없는 호출(`entry`·`ptt_domain`)이 이 설정으로 미러 경로를 푼다."""
    global _config
    _config = config or {}
    with _lock:
        _warned.clear()


def family(kind: str) -> str:
    k = (kind or '').lower()
    if k in PHONE_KINDS:
        return 'phone'
    if k in PTT_KINDS:
        return 'ptt'
    return ''


def _warn_once(key: str, msg: str) -> None:
    with _lock:
        if key in _warned:
            return
        _warned.add(key)
    logger.log_warning(msg)


def records(config: Optional[dict] = None) -> list:
    """미러의 enabled 레코드(name·domain 있는 것만), priority 오름차순. 미도달·손상은 빈 목록(폴백은 호출자)."""
    cfg = config if config is not None else _config
    try:
        rows = file_store.load_all(ha_lookup.collection_dir(cfg, COLLECTION)) or []
    except Exception as e:                      # 경로 해석 실패·권한 — 읽기 자체를 깨뜨리지 않는다
        _warn_once('load', f"access_services mirror unreadable ({e}) — Provisioning.Services fallback")
        return []
    out = [r for r in rows
           if isinstance(r, dict) and r.get('enabled') is not False
           and (r.get('name') or '').strip() and (r.get('domain') or '').strip()]
    out.sort(key=lambda r: (r.get('priority') if isinstance(r.get('priority'), int) else 1 << 30, str(r.get('name'))))
    return out


def _norm_kind(kind: str) -> str:
    k = (kind or '').strip().lower()
    return 'ptt' if k in PTT_KINDS else k


def find(config: Optional[dict], service_ref: str, kind: str) -> Optional[dict]:
    """service_ref(= access_services.name) 로 찾는다 — kind exact(ptt≡mcptt) 안에서만. 없으면 None(종류 폴백은 하지 않는다 —
    H(A1) 결박처럼 '그 서비스' 가 필요한 호출자용). kind 없는 레코드는 이름으로 인정한다."""
    ref = (service_ref or '').strip()
    if not ref:
        return None
    k = _norm_kind(kind)
    for r in records(config):
        if r.get('name') != ref:
            continue
        rk = _norm_kind(r.get('kind'))
        if k and rk and rk != k:                    # 테이블 = kind 불변식 — 다른 kind 의 같은 이름은 해당 없음
            continue
        return r
    return None


def find_by_name(config: Optional[dict], service_ref: str) -> Optional[dict]:
    """이름만으로 찾는다(kind 무관) — 쓰기 게이트가 '이 이름의 서비스는 어느 kind 인가' 를 묻는 용도."""
    ref = (service_ref or '').strip()
    if not ref:
        return None
    for r in records(config):
        if r.get('name') == ref:
            return r
    return None


def pick_by_kind(config: Optional[dict], kind: str) -> Optional[dict]:
    """kind 의 첫 enabled 레코드(priority 오름차순) — CSP GetByKind 와 같은 선택. `ptt` 는 별칭 `mcptt` 도 받는다."""
    k = (kind or '').lower()
    wanted = PTT_KINDS if k in PTT_KINDS else (k,)
    for r in records(config):
        if (r.get('kind') or '').lower() in wanted:
            return r
    return None


def resolve(config: Optional[dict], service_ref: str, kind: str) -> Optional[dict]:
    """가입 행 → 접속 서비스: service_ref 이름 매칭 → 없으면 kind 폴백(CSP GetForUser 와 같은 순서)."""
    return find(config, service_ref, kind) or pick_by_kind(config, kind)


def _overlay(services: dict, name: str, kinds: tuple) -> tuple:
    """csc.json Provisioning.Services 에서 (키, 항목) — 같은 kind 키의 name 일치 항목 우선, 없으면 kind 키 순서대로."""
    if name:
        for k, v in services.items():
            if isinstance(v, dict) and (v.get('name') or k) == name and _norm_kind(k) == _norm_kind(kinds[0]):
                return k, v
    for k in kinds:
        v = services.get(k)
        if isinstance(v, dict):
            return k, v
    return kinds[0], {}


def entry(kind: str, service_ref: str, provisioning: dict, config: Optional[dict] = None) -> tuple:
    """(와이어 kind, 서비스 항목) — `/provisioning/me` 가 내리는 한 서비스의 설정.

    미러 레코드(정의) 위에 csc.json 항목(단말 도달 정보)을 합친다. 와이어 kind 는 레코드의 kind(volte|voip|ptt) — 단말이 회선
    종류를 안다. 레코드가 없으면 csc.json 만으로: service_ref 와 name 이 같은 항목(같은 kind 키) → kind 키.
    """
    services = (provisioning.get('Services') or {}) if isinstance(provisioning, dict) else {}
    kind = (kind or '').lower()
    rec = resolve(config, service_ref, kind)
    if rec is None:
        ref = (service_ref or '').strip()
        if ref:
            for k, v in services.items():
                if isinstance(v, dict) and (v.get('name') or k) == ref and _norm_kind(k) == _norm_kind(kind):
                    return k, v
        return kind, (services.get(kind) or {})
    rec_kind = (rec.get('kind') or kind).lower()
    if rec_kind in PTT_KINDS:
        rec_kind = 'ptt'
    key, base = _overlay(services, rec.get('name') or '', (rec_kind, kind))
    svc = dict(base)
    cfg_domain = (base.get('domain') or '').strip()
    if cfg_domain and cfg_domain != (rec.get('domain') or '').strip():
        _warn_once(f"drift:{rec.get('name')}",
                   f"access_services '{rec.get('name')}' domain={rec.get('domain')} ≠ csc.json Provisioning.Services.{key}.domain="
                   f"{cfg_domain} — 미러(CSP 정본)를 쓴다. csc.json 값을 맞추라")
    for f in IDENTITY_FIELDS:
        if f in rec and rec[f] not in (None, ''):
            svc[f] = rec[f]
    svc['kind'] = rec_kind
    return rec_kind, svc


def ptt_domain(provisioning: dict, config: Optional[dict] = None) -> str:
    """PTT 접속 서비스 도메인 — 미러(kind=ptt 첫 레코드) → csc.json `Provisioning.Services.ptt.domain` 폴백. 없으면 ''."""
    rec = pick_by_kind(config, 'ptt')
    if rec:
        return str(rec.get('domain') or '').strip()
    services = (provisioning.get('Services') or {}) if isinstance(provisioning, dict) else {}
    return str(((services.get('ptt') or {}).get('domain')) or '').strip()
