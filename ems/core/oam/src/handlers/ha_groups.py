"""
CIMS HA Groups REST API.

Routes (prefix-matched, mounted at /api/v1/ha-groups):
  /api/v1/ha-groups                              GET list / POST create
  /api/v1/ha-groups/{id}                         GET / PUT / DELETE
  /api/v1/ha-groups/{id}/members                 GET list / POST add
  /api/v1/ha-groups/{id}/members/{agent_id}      DELETE

ha_groups 정책 (sql/migrate_ha_groups.sql):
  - 1 agent = 1 group (uk_agent UNIQUE)
  - mode = 'active_standby' | 'all_active'
  - VRID 는 51-255 range 자동 할당, VIP 는 운영자 수동 입력

그룹 생성/수정/멤버 변경 시 update_ha job 자동 큐잉 (services.ha_render).
"""
from __future__ import annotations

from urllib.parse import urlparse, unquote
from pathlib import PurePath
import asyncio
import json

from httpsrv.handler import HandlerArgs, HandlerResult
from services import file_store, service_registry
from util.log_util import Logger

logger = Logger()


_HA_GROUPS_BASE = '/api/v1/ha-groups'
_HA_DOMAIN = 'ha_groups'

_VRID_MIN = 51
_VRID_MAX = 255


# failover_options 의 default — 현재 hardcoded 동작과 동일.
# 옛 record (failover_options 미존재) 도 _normalize_failover_options 가 이 default 로 채워
# 동작 변경 없음 (호환성 보장).
_FAILOVER_DEFAULTS = {
    'advert_int':      1.0,
    'health': {
        'interval':    2,
        'fall':        2,
        'rise':        2,
        'timeout':     3,
        'grace_sec':   30,   # MASTER 승격 후 헬스 유예 — cold 모듈 기동 시간 흡수
    },
    'track_interface': False,
    # 재기동 임계 (그룹/시스템 스코프) — watchdog 이 연속 max_fails 회 재기동 실패
    # (window_sec 윈도우 내) 하면 cims-health 가 FAULT → 절체. 로컬 복구 소진 후에만
    # VIP 를 옮긴다 (Pacemaker migration-threshold 계열). 상세: ha_service_model.md §5.
    'restart_limit':   {'max_fails': 3, 'window_sec': 300},
    'preempt':         'nopreempt',
    'preempt_delay':   0,
    # module_modes/tracked_modules 는 모듈 운영 명세(group.module_specs)로 이관됨.
    # 구 record 읽기 호환을 위해 normalize 는 여전히 수용하되(마이그레이션 입력),
    # 신규 저장 경로는 module_specs 를 쓴다.
}


def _normalize_failover_options(raw) -> dict:
    """입력 dict → 검증된 failover_options. 잘못된 값은 default 로 대체.

    AS 만 의미 있으나, 다른 mode 도 같은 dict 형태로 저장 — UI 가 mode 로 분기.
    range:
      advert_int: 0.5~5 (float, sec)
      health.interval / fall / rise / timeout: 1~60 (int)
      health.port: 1~65535 / health.proto: tcp|udp (선택 — 수동 오버라이드)
      module_modes: {module: 'cold'|'hot'} — 미지정 모듈은 cold
      preempt: 'preempt' | 'nopreempt'
      preempt_delay: 0~300 (int, sec)
    """
    if not isinstance(raw, dict):
        raw = {}
    out = {}

    try:
        ai = float(raw.get('advert_int', _FAILOVER_DEFAULTS['advert_int']))
        if 0.5 <= ai <= 5:
            out['advert_int'] = ai
        else:
            out['advert_int'] = _FAILOVER_DEFAULTS['advert_int']
    except (TypeError, ValueError):
        out['advert_int'] = _FAILOVER_DEFAULTS['advert_int']

    health_in = raw.get('health') if isinstance(raw.get('health'), dict) else {}
    health = {}
    for k in ('interval', 'fall', 'rise', 'timeout'):
        try:
            v = int(health_in.get(k, _FAILOVER_DEFAULTS['health'][k]))
            if 1 <= v <= 60:
                health[k] = v
            else:
                health[k] = _FAILOVER_DEFAULTS['health'][k]
        except (TypeError, ValueError):
            health[k] = _FAILOVER_DEFAULTS['health'][k]
    # port/proto 수동 오버라이드 (선택) — 미지정 시 배포 실효설정/descriptor 로 유도.
    try:
        hp = int(health_in.get('port', 0) or 0)
        if 0 < hp < 65536:
            health['port'] = hp
    except (TypeError, ValueError):
        pass
    hproto = health_in.get('proto')
    if hproto in ('tcp', 'udp'):
        health['proto'] = hproto
    # 승격 grace (0=유예 없음) — cims-health 가 VIP 취득 직후 cold 모듈이 뜨는 동안
    # 검사 실패를 유예하는 윈도. 초과 범위는 default.
    try:
        gs = int(health_in.get('grace_sec', _FAILOVER_DEFAULTS['health']['grace_sec']))
        health['grace_sec'] = gs if 0 <= gs <= 600 else _FAILOVER_DEFAULTS['health']['grace_sec']
    except (TypeError, ValueError):
        health['grace_sec'] = _FAILOVER_DEFAULTS['health']['grace_sec']
    out['health'] = health

    out['track_interface'] = bool(raw.get('track_interface', False))

    # 재기동 임계 — max_fails 1~20, window_sec 10~3600.
    rl_in = raw.get('restart_limit') if isinstance(raw.get('restart_limit'), dict) else {}
    rl = {}
    try:
        mf = int(rl_in.get('max_fails', _FAILOVER_DEFAULTS['restart_limit']['max_fails']))
        rl['max_fails'] = mf if 1 <= mf <= 20 else _FAILOVER_DEFAULTS['restart_limit']['max_fails']
    except (TypeError, ValueError):
        rl['max_fails'] = _FAILOVER_DEFAULTS['restart_limit']['max_fails']
    try:
        ws = int(rl_in.get('window_sec', _FAILOVER_DEFAULTS['restart_limit']['window_sec']))
        rl['window_sec'] = ws if 10 <= ws <= 3600 else _FAILOVER_DEFAULTS['restart_limit']['window_sec']
    except (TypeError, ValueError):
        rl['window_sec'] = _FAILOVER_DEFAULTS['restart_limit']['window_sec']
    out['restart_limit'] = rl

    # module_modes/tracked_modules — 구 record 마이그레이션 입력으로만 수용(보존).
    # 최종 SoT 는 group.module_specs (_migrate_module_specs 가 1회 변환). 이미
    # module_specs 로 넘어간 그룹은 이 필드가 비어 있어 무해.
    tm = raw.get('tracked_modules') or []
    if isinstance(tm, list):
        _tm = [str(x).strip().lower() for x in tm if str(x).strip()]
        if _tm:
            out['tracked_modules'] = _tm
    mm = raw.get('module_modes') if isinstance(raw.get('module_modes'), dict) else {}
    _mm = {
        str(k).strip().lower(): ('hot' if str(v).strip().lower() == 'hot' else 'cold')
        for k, v in mm.items() if str(k).strip()
    }
    if _mm:
        out['module_modes'] = _mm

    pe = raw.get('preempt') or _FAILOVER_DEFAULTS['preempt']
    out['preempt'] = pe if pe in ('preempt', 'nopreempt') else _FAILOVER_DEFAULTS['preempt']

    try:
        pd = int(raw.get('preempt_delay', 0))
        out['preempt_delay'] = pd if 0 <= pd <= 300 else 0
    except (TypeError, ValueError):
        out['preempt_delay'] = 0

    return out


# ── 모듈 운영 명세 (group.module_specs) ────────────────────────────────
# 모듈 스코프 운영 설정의 SoT — agent 가 modules/<mod>/service.json 으로 받아
# watchdog·제어 게이팅에 쓰고, OAM 렌더가 cold_modules/relevant_modules/헬스 힌트를
# 여기서 유도한다. 앱 config.json 과 물리 분리. 상세: ha_service_model.md §3.
_MODULE_SPEC_DEFAULT = {
    'supervision': {'watchdog': True},
    'ha':          {'failover_mode': 'cold', 'failover_relevant': True},
    'health':      {},   # {port,proto,config_key} 오버라이드 — 미지정 시 배포 유도
}


def _normalize_module_spec(raw) -> dict:
    """입력 dict → 검증된 모듈 운영 명세. 잘못된 값은 default."""
    raw = raw if isinstance(raw, dict) else {}
    sup = raw.get('supervision') if isinstance(raw.get('supervision'), dict) else {}
    ha  = raw.get('ha') if isinstance(raw.get('ha'), dict) else {}
    hl  = raw.get('health') if isinstance(raw.get('health'), dict) else {}
    health = {}
    try:
        hp = int(hl.get('port', 0) or 0)
        if 0 < hp < 65536:
            health['port'] = hp
    except (TypeError, ValueError):
        pass
    if hl.get('proto') in ('tcp', 'udp'):
        health['proto'] = hl['proto']
    if isinstance(hl.get('config_key'), str) and hl['config_key'].strip():
        health['config_key'] = hl['config_key'].strip()
    return {
        'supervision': {'watchdog': bool(sup.get('watchdog', True))},
        'ha': {
            'failover_mode':     'hot' if ha.get('failover_mode') == 'hot' else 'cold',
            'failover_relevant': bool(ha.get('failover_relevant', True)),
        },
        'health': health,
    }


def _module_spec(group: dict, mod: str) -> dict:
    """group.module_specs[mod] 의 실효 명세 (미지정 모듈은 default)."""
    specs = group.get('module_specs') if isinstance(group.get('module_specs'), dict) else {}
    return _normalize_module_spec(specs.get(mod))


def _migrate_module_specs(group: dict) -> bool:
    """failover_options.module_modes/tracked_modules → module_specs (1회). 변경 시 True.

    이미 module_specs 가 있으면 no-op. 구 그룹은 module_modes(hot 여부)와
    tracked_modules(절체 관여)를 명세로 승계 — 렌더 결과 동일."""
    if isinstance(group.get('module_specs'), dict):
        return False
    fo = group.get('failover_options') or {}
    modes = fo.get('module_modes') if isinstance(fo.get('module_modes'), dict) else {}
    tracked = {str(m).strip().lower() for m in (fo.get('tracked_modules') or []) if str(m).strip()}
    specs = {}
    for m in (set(modes.keys()) | tracked):
        specs[m] = {
            'supervision': {'watchdog': True},
            'ha': {
                'failover_mode':     'hot' if modes.get(m) == 'hot' else 'cold',
                # 구 tracked = pgrep 검사 대상 = 절체 관여. 그 외는 default(관여).
                'failover_relevant': True,
            },
        }
    group['module_specs'] = specs
    return True


def _migrate_service_intent(group: dict, config: dict) -> bool:
    """service_intent 부재(구 record) → 현재 record running 모듈로 1회 시드. 변경 시 True.

    이것이 유일한 record→의도 유추 지점이며 마이그레이션 전용이다. 시드 후엔
    의도가 명시 SoT — 운영 중이던 그룹은 무장 유지, 설치만 된 그룹은 미개시."""
    if isinstance(group.get('service_intent'), dict):
        return False
    started = _group_started_modules(group.get('members') or [], config)
    group['service_intent'] = {m: 'running' for m in started}
    return True


def _ensure_group_migrated(group: dict, config: dict) -> bool:
    """구 record 를 신 스키마(service_intent + module_specs)로 승격. 변경 시 True.
    호출부가 True 면 file_store.save 로 영속화한다 (1회성 — 이후 no-op)."""
    changed = False
    if _migrate_service_intent(group, config):
        changed = True
    if _migrate_module_specs(group):
        changed = True
    return changed


def _normalize_service_intent(raw) -> dict:
    """service_intent 입력 정규화 — {module(lower): 'running'|'stopped'}."""
    if not isinstance(raw, dict):
        return {}
    out = {}
    for k, v in raw.items():
        mk = str(k).strip().lower()
        if not mk:
            continue
        out[mk] = 'running' if str(v).strip().lower() == 'running' else 'stopped'
    return out


def _ha_dir(config):
    return file_store.domain_dir(config, _HA_DOMAIN)


def _ha_load(config, gid: int):
    return file_store.by_id(_ha_dir(config), gid)


def _ha_load_all(config) -> list:
    return file_store.load_all(_ha_dir(config))


def _path_parts(full_path: str, base: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _alloc_vrid(config) -> int:
    """51-255 range 에서 next available VRID 반환. 없으면 RuntimeError."""
    used = {g.get('vrid') for g in _ha_load_all(config) if g.get('vrid') is not None}
    for v in range(_VRID_MIN, _VRID_MAX + 1):
        if v not in used:
            return v
    raise RuntimeError(f"VRID pool exhausted ({_VRID_MIN}-{_VRID_MAX})")


def _pick_default_iface(vip_bindings: list, agent_id: int, agent_row: dict | None = None) -> str:
    """vrrp_instance 의 advert NIC 결정.

    우선순위:
      1) vip_bindings.memberIfaces[agent_id] 첫 명시.
      2) agent.interfaces 의 role='mgmt' (unicast_src_ip 와 같은 NIC 자연 선택).
      3) agent.interfaces 의 첫 NIC.
      4) 빈 문자열 (caller 가 'eth0' fallback).
    """
    for b in (vip_bindings or []):
        iface = (b.get('memberIfaces') or {}).get(str(agent_id)) \
                or (b.get('memberIfaces') or {}).get(agent_id)
        if iface:
            return iface
    if isinstance(agent_row, dict):
        # mgmt NIC 우선 — unicast_src_ip = mgmt IP 와 정합.
        for it in (agent_row.get('interfaces') or []):
            if isinstance(it, dict) and (it.get('role') == 'mgmt' or it.get('mgmt')):
                if it.get('name'):
                    return it['name']
        # fallback — 첫 NIC.
        for it in (agent_row.get('interfaces') or []):
            if isinstance(it, dict) and it.get('name'):
                return it['name']
    return ''


def _iface_ip(agent_row: dict, iface_name: str) -> str:
    """agent_row.interfaces[] 에서 iface_name 에 해당하는 IP 반환. 없으면 빈 문자열."""
    if not iface_name:
        return ''
    for it in (agent_row.get('interfaces') or []):
        if it.get('name') == iface_name and it.get('ip'):
            return it['ip']
    return ''


# cims-health 가 ha.json 의 services.<group>.port/proto 를 lookup. 누락 시
# default 가 csc/csp/psp 만 정의되어 있어 그룹명(예: "Control-Server") 으로는
# 찾지 못해 health 가 fail → keepalived 가 BACKUP 강제 → VIP 미할당.
# 해결: ha.json render 시 그룹 멤버 deployment 들의 daemon module 을 보고
# 대표 module 의 default port/proto 를 services.<group> 에 자동 채워준다.
_MODULE_HEALTH_DEFAULTS = {
    'csp':   (5060, 'udp'),
    'isp':   (5060, 'udp'),
    'psp':   (5060, 'udp'),
    'csc':   (4421, 'tcp'),
    'cmp':   (9000, 'udp'),
    'imp':   (9000, 'udp'),
    'pmp':   (9000, 'udp'),
}
# 동일 그룹에 여러 daemon module 이 deployed 되어 있을 때의 우선순위.
# Control: csp 가 핵심 (SIP signaling) — psp/isp/csc 는 부수.
# Media: cmp 가 핵심 (RTP relay).
_HEALTH_MODULE_PRIORITY = ['csp', 'cmp', 'csc', 'psp', 'isp', 'pmp', 'imp']


def _csc_effective_health_port(dep: dict, config: dict):
    """csc 배포의 실효 admin 포트 — 게이트웨이 self-register 와 동일한 단일 해석
    (handlers.agents.effective_server_port: materialize Server.Port flat/nested →
    pkg gateway.default_port). 운영자가 콘솔에서 Server.Port 를 바꿔도 다음 render
    가 자동 추종한다. 실패 시 None (caller 가 descriptor 기본값 사용)."""
    try:
        from handlers.agents import effective_server_port
        pkg = file_store.by_id(file_store.domain_dir(config, 'packages'),
                               dep.get('package_id')) or {}
        return effective_server_port(config, pkg, dep.get('config'))
    except Exception:
        pass
    return None


def _group_started_modules(members: list, config: dict) -> set:
    """그룹에서 "서비스가 개시된" daemon 모듈 집합 — 멤버 배포 중 record
    status=='running' 이 하나라도 있는 모듈 (그룹 레벨 OR: 절체로 standby 기록이
    stopped 인 채 notify 기동되는 비대칭 흡수).

    HA 무장(armed)의 게이트 — 설치만 되고 운영자가 start 하지 않은 모듈은
    keepalived 가 관리(승격 기동/헬스 검사) 대상이 아니다. 미개시 그룹은
    vrrp_instance 자체가 생성되지 않아 Active/Standby 상태도 존재하지 않는다."""
    try:
        from handlers.agents import _deploy_load_all
        aids = {m.get('agent_id') for m in (members or [])}
        return {(d.get('process_name') or d.get('package_name') or '').lower().strip()
                for d in _deploy_load_all(config)
                if d.get('agent_id') in aids and d.get('status') == 'running'}
    except Exception:
        return set()


def _infer_health_port_proto(agent_id: int, config: dict, allowed: set | None = None) -> tuple:
    """agent 의 daemon deployment 들 중 가장 적합한 module 로 (port, proto, module) 추정.

    allowed 가 주어지면 그 집합(서비스 개시 모듈)에 속한 모듈만 후보 —
    설치만 된 모듈로 헬스포트를 유도하면 미기동 포트를 검사해 FAULT 가 된다.
    찾지 못하면 (None, None, None) 반환 — 이 경우 services entry 에 port/proto 미기재.
    """
    try:
        from handlers.agents import _deploy_load_all
        deps = [d for d in _deploy_load_all(config)
                if d.get('agent_id') == agent_id]
    except Exception:
        return (None, None, None)
    # deployment file 에는 package_name 이 없고 process_name 만 있는 케이스가 있음.
    # process_name 우선 (CSP/CMP/CSC 등 대문자 → lowercase). cspsim 등 non-daemon 제외.
    # service descriptor 의 모듈 health 맵 (없으면 하드코딩 fallback — 전환 안전망).
    defaults = service_registry.module_health_defaults(config) or _MODULE_HEALTH_DEFAULTS
    daemon_modules: dict = {}
    for d in deps:
        mod = (d.get('process_name') or '').lower().strip()
        if mod in defaults and (allowed is None or mod in allowed):
            daemon_modules.setdefault(mod, d)
    # descriptor 모듈 순서 + 기존 우선순위 휴리스틱 병합 (priority 우선, 그 외 descriptor 순).
    order = _HEALTH_MODULE_PRIORITY + [m for m in defaults if m not in _HEALTH_MODULE_PRIORITY]
    for mod in order:
        if mod in daemon_modules:
            # csc 는 단일 설정키(Server.Port)로 리슨 포트가 정해지므로 실효 설정 우선.
            # (csp 계열은 local_nodes 컬렉션 기반이라 descriptor 기본값 유지.)
            if mod == 'csc':
                port = _csc_effective_health_port(daemon_modules[mod], config)
                if port:
                    return (port, 'tcp', mod)
            return defaults[mod] + (mod,)
    return (None, None, None)


def _agent_daemon_modules(agent_id: int, config: dict) -> list:
    """agent 의 daemon deployment 모듈 목록 (health 우선순위 순) — cold_modules 렌더용.
    descriptor 에 port 가 있는 모듈(=리슨 데몬)만. cspsim/console 등 비데몬 제외."""
    try:
        from handlers.agents import _deploy_load_all
        deps = [d for d in _deploy_load_all(config)
                if d.get('agent_id') == agent_id]
    except Exception:
        return []
    defaults = service_registry.module_health_defaults(config) or _MODULE_HEALTH_DEFAULTS
    present = {(d.get('process_name') or '').lower().strip() for d in deps}
    present = {m for m in present if m in defaults}
    order = _HEALTH_MODULE_PRIORITY + [m for m in defaults if m not in _HEALTH_MODULE_PRIORITY]
    return [m for m in order if m in present]


def _compute_master_aid(members: list) -> int | None:
    """VRRP 본래 모델 — priority 가 단일 결정자.
    그룹 내 priority 최대값 멤버가 Master. 동률이면 agent_id 작은 쪽 (안정적 tie-break)."""
    if not members:
        return None
    return min(
        (m for m in members if m.get('agent_id') is not None),
        key=lambda m: (-int(m.get('priority') or 0), int(m.get('agent_id'))),
        default=None,
    ).get('agent_id') if members else None


def _render_ha_for_agent(group: dict, members: list, agent_id: int,
                         agent_row: dict, peer_row: dict | None,
                         vip_bindings: list | None = None,
                         config: dict | None = None) -> dict:
    """그룹 + 멤버 → 특정 agent 의 ha.json 내용.

    vip_bindings 가 있으면 multi-VIP 한 vrrp_instance (services.<group_name>.vips[]).
    없으면 legacy 단일 vip path (group.vip).

    priority 는 멤버 record 값 그대로 ha.json 에 박힘. initial_state 는 전원 BACKUP —
    MASTER 는 priority 차등으로 선출 (VRRP 본래 모델, nopreempt 정합).
    """
    master_aid = _compute_master_aid(members)
    is_master = (master_aid == agent_id)
    # 이 agent 의 priority — 멤버 record 의 값 그대로. 누락 시 100/90 default (호환성).
    my_priority = next(
        (int(m.get('priority') or 0) for m in members if m.get('agent_id') == agent_id),
        100 if is_master else 90,
    )
    vip_bindings = vip_bindings or []
    default_iface = _pick_default_iface(vip_bindings, agent_id, agent_row) or "eth0"

    # 무장 게이트 = 서비스 의도 (선언적). HA 는 운영자가 의도적으로 running 으로
    # 둔 모듈만 관리한다 — record status 유추가 아니라 group.service_intent 명시값.
    # 재설치·스토어유실·예외로 record 가 어떻게 되든 의도가 running 이면 무장 유지
    # (장애 시 승격이 cold 모듈 재기동 = 자가 회복). 상세: ha_service_model.md §2.
    intent = group.get('service_intent') if isinstance(group.get('service_intent'), dict) else {}
    intent_running = {m for m, s in intent.items() if s == 'running'}

    # cims-health 가 lookup 하는 port/proto — running 의도 모듈의 배포로 유도.
    h_port, h_proto, h_module = _infer_health_port_proto(agent_id, config, allowed=intent_running) if config else (None, None, None)

    failover_options = _normalize_failover_options(group.get('failover_options'))
    # 그룹 옵션의 수동 오버라이드가 최우선 (운영자 명시 > 배포 실효설정 유도 > descriptor 기본).
    fo_health = failover_options.get('health') or {}
    if fo_health.get('port'):
        h_port = fo_health['port']
        h_proto = fo_health.get('proto') or h_proto or 'tcp'
    elif fo_health.get('proto'):
        h_proto = fo_health['proto']

    # 진실 기반 헬스체크 힌트 — csc(설정 단일키로 리슨 포트가 정해지는 모듈)는
    # cims-health 가 검사 시점에 노드 로컬 배포 config.json 의 Server.Port 를 직접
    # 읽어 검사한다 (배포기록↔실파일 드리프트가 나도 HA 는 실제 bind 포트를 봄 —
    # 드리프트 자체는 config_out_of_sync 알람이 노출). 운영자가 health.port 를
    # 수동 지정하면 힌트를 내리지 않아 오버라이드가 그대로 최우선.
    h_cfg_key = None
    if h_module == 'csc' and not fo_health.get('port'):
        h_cfg_key = 'Server.Port'
    # 모듈 운영 명세의 health.config_key 오버라이드 (있으면 우선 — 수동 그룹 override 제외).
    if h_module and not fo_health.get('port'):
        _mh = _module_spec(group, h_module).get('health') or {}
        if _mh.get('config_key'):
            h_cfg_key = _mh['config_key']

    # armed daemon 모듈 = 이 agent 에 배포된 daemon 모듈 ∩ running 의도.
    daemon_mods = [m for m in (_agent_daemon_modules(agent_id, config) if config else [])
                   if m in intent_running]
    # cold-spare 절체 대상 — AS 그룹의 armed daemon 중 명세 failover_mode 가 hot 이 아닌
    # 전부(기본 cold). cims-notify 가 MASTER 승격 시 start / BACKUP·FAULT 강등 시 stop.
    # relevant_modules = 실패가 절체 사유가 되는 모듈 (cims-health 가 재기동 임계 판정).
    cold_modules: list = []
    relevant_modules: list = []
    if group.get('mode') == 'active_standby':
        cold_modules = [m for m in daemon_mods
                        if _module_spec(group, m)['ha']['failover_mode'] != 'hot']
        relevant_modules = [m for m in daemon_mods
                            if _module_spec(group, m)['ha']['failover_relevant']]

    # running 의도 daemon 모듈이 없고 헬스포트도 없으면 미개시/빈 서버 — vrrp_instance
    # 를 내리지 않는다 (enabled=false → cims-ha 렌더 스킵 + keepalived 정지 유지).
    # 이후 서비스 의도 변경(일괄/서버별 start)이 재렌더를 태워 자동 무장/해제된다.
    ha_enabled = bool(h_port or daemon_mods)
    restart_limit = failover_options.get('restart_limit') or {}

    services: dict = {}
    if vip_bindings:
        vips = []
        # vrrp_instance.interface = unicast_src_ip 와 같은 NIC (vrrp advert
        # 송수신 채널). top-level ha.json.interface 사용. 각 VIP 의 dev 는
        # binding 별로 따로 (multi-망 multi-VIP 한 vrrp_instance 패턴).
        svc_iface = default_iface
        for b in vip_bindings:
            slot = (b.get('slot') or '').strip()
            ip   = (b.get('ip')   or '').strip()
            if not slot or not ip:
                continue
            mask = int(b.get('mask') or group.get('vip_mask') or 24)
            iface = (b.get('memberIfaces') or {}).get(str(agent_id)) \
                    or (b.get('memberIfaces') or {}).get(agent_id)
            # VIP→NIC 매핑은 용도(slot) 단일 키로 결정 (망/role 모델 폐지).
            # memberIfaces 미명시 시: 이 agent 의 service_ip_rows 중 용도(slot) 가
            # 동일한 항목의 iface 를 사용. (VIP 바인딩 slot == NIC 용도 라벨)
            if not iface:
                for r in (agent_row.get('service_ip_rows') or []):
                    if not isinstance(r, dict): continue
                    if (r.get('slot') or '').strip() == slot:
                        iface = r.get('iface')
                        break
            # 각 VIP 가 어느 NIC 에 attach 될지 명시. 누락 시 svc_iface fallback.
            vips.append({'slot': slot, 'ip': ip, 'mask': mask, 'dev': iface or svc_iface})
        if vips:
            entry = {
                'enabled':  ha_enabled,
                'vrid':     group['vrid'],
                'interface': svc_iface,
                'vips':     vips,
                'priority': my_priority,
                'failover_options': failover_options,
            }
            if h_port:  entry['port']  = h_port
            if h_proto: entry['proto'] = h_proto
            if h_cfg_key:
                entry['health_module'] = h_module
                entry['health_config_key'] = h_cfg_key
            if cold_modules: entry['cold_modules'] = cold_modules
            if relevant_modules: entry['relevant_modules'] = relevant_modules
            if restart_limit: entry['restart_limit'] = restart_limit
            services[group['name']] = entry
    elif group.get('vip') and group['vip'] not in ('', '0.0.0.0'):
        # legacy 단일 vip
        entry = {
            'enabled':  ha_enabled,
            'vrid':     group['vrid'],
            'interface': default_iface,
            'vip':      group['vip'],
            'priority': my_priority,
            'failover_options': failover_options,
        }
        if h_port:  entry['port']  = h_port
        if h_proto: entry['proto'] = h_proto
        if h_cfg_key:
            entry['health_module'] = h_module
            entry['health_config_key'] = h_cfg_key
        if cold_modules: entry['cold_modules'] = cold_modules
        if relevant_modules: entry['relevant_modules'] = relevant_modules
        if restart_limit: entry['restart_limit'] = restart_limit
        services[group['name']] = entry

    # local_ip / peer_ip 은 VRRP advertise 가 송신되는 interface 의 IP 여야 함.
    # interface=svc 인데 agent.ip_address=mgmt 망이면 split brain 발생.
    local_ip = _iface_ip(agent_row, default_iface) or agent_row.get('ip_address') or "127.0.0.1"
    peer_ip = ''
    if peer_row:
        peer_ip = _iface_ip(peer_row, default_iface) or peer_row.get('ip_address') or ''

    return {
        "node_name":     agent_row.get('name') or f"agent-{agent_id}",
        "interface":     default_iface,
        "local_ip":      local_ip,
        "peer_ip":       peer_ip,
        # 전원 BACKUP 시작 — priority 차등이 MASTER 를 결정 (VRRP 본래 모델).
        # state MASTER + nopreempt 는 keepalived 가 "will not work" 경고와 함께
        # nopreempt 를 무시하는 모순 조합이라 initial_state 로 쓰지 않는다.
        "initial_state": "BACKUP",
        "vip_mask":      group['vip_mask'],
        "auth_pass":     group['auth_pass'],
        "ha_log_dir":    "/var/log/cims-ha",
        "cims_home":     "/opt/cims",
        "cims_user":     "cims",
        "services":      services,
    }


def _agents_with_started_modules(members: list, config: dict) -> set:
    """record status=='running' 배포를 가진 멤버 agent_id 집합 — "운영자가 start 한
    서버" 판별 (개시 국면 선행 대상)."""
    try:
        from handlers.agents import _deploy_load_all
        aids = {m.get('agent_id') for m in (members or [])}
        return {d.get('agent_id') for d in _deploy_load_all(config)
                if d.get('agent_id') in aids and d.get('status') == 'running'}
    except Exception:
        return set()


# 개시 국면에서 나머지 멤버 update_ha 를 미루는 시간 — 선행 멤버의 job 회수
# (heartbeat 정상 2s 주기지만 OAM 불통 직후엔 backoff 로 최대 60s 벌어질 수 있음)
# + apply + MASTER 승격(~4s)을 worst case 로 덮고도 여유가 남는 값.
_STAGGER_DELAY_SEC = 75


def _enqueue_update_ha_for_members(group_id: int, config: dict,
                                   prefer_first: set | None = None) -> int:
    """그룹 멤버들에게 update_ha job 큐잉. 큐잉된 job 수 반환.

    개시 국면 선착 방지 — AS 그룹이 무장 상태로 렌더되는데 아직 아무도 VIP 를
    보유하지 않았다면(최초 개시·전면 재기동), **기준 멤버에게 먼저** 내리고 나머지
    멤버는 not_before 로 지연시킨다. 동시에 뿌리면 양쪽 keepalived 가 함께
    콜드스타트해 선거 레이스가 되는데, start 를 실행한 노드가 그 job 처리 탓에
    자기 update_ha 를 늦게 가져가면 놀고 있던 standby 가 구조적으로 선착한다.
    기준 멤버 = prefer_first(호출부 명시 — 서버별 start 시 그 노드) > record running
    멤버 > 지정 마스터(priority 최대). VIP 보유자가 이미 있으면(운영 중 재렌더)
    지연 없음 — apply 는 멱등이라 순서 무관.

    호출 시 구 record 를 신 스키마(service_intent/module_specs)로 1회 마이그레이션·
    영속화한다 (렌더 단일 관문)."""
    group = _ha_load(config, group_id)
    if not group:
        return 0
    if _ensure_group_migrated(group, config):
        file_store.save(_ha_dir(config), group_id, group)
    members = list(group.get('members') or [])
    if not members:
        return 0
    vip_bindings = group.get('vip_bindings') or []

    from handlers.agents import _agent_load, _job_create
    agents = {}
    for m in members:
        a = _agent_load(config, aid=m.get('agent_id'))
        if a:
            agents[m['agent_id']] = {'id': a.get('id'), 'name': a.get('name'),
                                     'ip_address': a.get('ip_address'),
                                     'interfaces': a.get('interfaces') or []}

    # 멤버별 렌더를 먼저 완성 — 지연 대상 판단(무장 여부)에 렌더 결과가 필요하다.
    renders = []   # [(agent_id, ha_json)]
    for m in members:
        agent = agents.get(m['agent_id'])
        if not agent:
            continue
        peer = None
        for other in members:
            if other['agent_id'] != m['agent_id']:
                peer = agents.get(other['agent_id'])
                break
        renders.append((m['agent_id'],
                        _render_ha_for_agent(group, members, m['agent_id'], agent, peer, vip_bindings, config)))

    # 선행 멤버 결정 — start 된 멤버 우선, 없으면 지정 마스터(priority 최대).
    # 전원 start 상태면 순서가 무의미하므로 지연 없음.
    stagger_first: set = set()
    if group.get('mode') == 'active_standby' and len(renders) >= 2:
        armed = any(((hj.get('services') or {}).get(group.get('name')) or {}).get('enabled')
                    for _, hj in renders)
        if armed:
            from services import ha_lookup
            obs = ha_lookup.vip_observation(config, group)
            nobody_holds = not any(v is True for v in (obs.get('observed') or {}).values())
            if nobody_holds:
                if prefer_first:
                    firsts = [aid for aid, _ in renders if aid in prefer_first]
                else:
                    started = _agents_with_started_modules(members, config)
                    firsts = [aid for aid, _ in renders if aid in started]
                if not firsts:
                    ma = _compute_master_aid(members)
                    firsts = [ma] if ma is not None else []
                if firsts and len(firsts) < len(renders):
                    stagger_first = set(firsts)

    not_before = None
    if stagger_first:
        from datetime import datetime, timedelta
        not_before = (datetime.now() + timedelta(seconds=_STAGGER_DELAY_SEC)) \
            .isoformat(timespec='seconds')
        logger.log_info(
            f"[ha-group] group#{group_id} 개시 국면 — start 멤버 {sorted(stagger_first)} 선행, "
            f"나머지 update_ha {_STAGGER_DELAY_SEC}s 지연 (선거 선점 방지)")

    enqueued = 0
    for aid, ha_json in renders:
        agent = agents.get(aid) or {}
        params = {
            # install_path 는 구 agent(flat 레이아웃) 호환용 잔재 — 신 agent 는 무시하고
            # <prefix>/run/keepalived/ 에 기록한다 (agent job_update_ha 참조).
            "install_path": f"/opt/cims/{agent.get('name','agent')}",
            "ha_json": ha_json,
        }
        delayed = bool(stagger_first) and aid not in stagger_first
        _job_create(config, aid, 'update_ha', params,
                    not_before=not_before if delayed else None)
        enqueued += 1
    return enqueued


def _enqueue_disarm_for_agent(agent_id: int, config: dict) -> int:
    """그룹 이탈(멤버 제거·그룹 삭제) agent 에 빈 services ha.json 을 푸시 — keepalived 해제.

    그룹 렌더는 현 멤버 기준이라 이탈한 agent 는 재렌더 대상에서 빠지고, 노드에는
    구 vrid/VIP 로 무장된 keepalived 가 영구 잔존한다 (유령 VIP·vrid 충돌 경로).
    agent 의 job_update_ha 는 services 가 비면 cims-ha uninstall 로 정리한다.
    다른 그룹 소속이 남아 있으면 (1 agent = 1 group 이라 정상 흐름에선 없음)
    그 그룹의 정상 재렌더로 대신한다. 큐잉된 job 수 반환."""
    for g in _ha_load_all(config):
        if any(m.get('agent_id') == agent_id for m in (g.get('members') or [])):
            return _enqueue_update_ha_for_members(g.get('id'), config)
    from handlers.agents import _agent_load, _job_create
    a = _agent_load(config, aid=agent_id)
    if not a:
        return 0
    ha_json = {
        "node_name":     a.get('name') or f"agent-{agent_id}",
        "interface":     "",
        "local_ip":      a.get('ip_address') or "",
        "peer_ip":       "",
        "initial_state": "BACKUP",
        "vip_mask":      24,
        "auth_pass":     "",
        "ha_log_dir":    "/var/log/cims-ha",
        "cims_home":     "/opt/cims",
        "cims_user":     "cims",
        "services":      {},
    }
    _job_create(config, agent_id, 'update_ha', {
        "install_path": f"/opt/cims/{a.get('name', 'agent')}",
        "ha_json": ha_json,
    })
    return 1


def enqueue_update_ha_for_agent(agent_id: int, config: dict) -> int:
    """agent 가 속한 모든 HA 그룹에 update_ha 재렌더 큐잉 — 배포 설정 변경으로
    헬스포트 등 렌더 입력이 바뀌었을 때 ha.json 이 자동 추종하는 경로.
    (그룹 렌더는 멤버 전체가 한 단위 — 해당 그룹 전 멤버에게 재푸시.)"""
    enqueued = 0
    for g in _ha_load_all(config):
        if any(m.get('agent_id') == agent_id for m in (g.get('members') or [])):
            enqueued += _enqueue_update_ha_for_members(g.get('id'), config)
    return enqueued


def note_module_started(config: dict, agent_id: int, module: str) -> "int | None":
    """서버별/모듈 start 성공 → 그 모듈의 그룹 서비스 의도를 running 으로 승격.

    운영자의 명시적 start 는 "이 모듈은 떠 있어야 한다"는 의도 선언이다. 그룹이
    미개시(의도 stopped)였으면 running 으로 승격해 keepalived 를 무장시킨다. 이미
    running 이면 no-op. agent 가 어느 그룹에도 없으면(standalone) None.
    반환: 승격된 그룹 id (변경 없으면 None). 상세: ha_service_model.md §6."""
    module = (module or '').strip().lower()
    if not module:
        return None
    for g in _ha_load_all(config):
        if not any(m.get('agent_id') == agent_id for m in (g.get('members') or [])):
            continue
        _ensure_group_migrated(g, config)   # 구 record 승격 (intent 시드)
        intent = g.get('service_intent') if isinstance(g.get('service_intent'), dict) else {}
        if intent.get(module) == 'running':
            file_store.save(_ha_dir(config), g['id'], g)   # 마이그레이션 결과 영속
            return None
        intent[module] = 'running'
        g['service_intent'] = intent
        file_store.save(_ha_dir(config), g['id'], g)
        logger.log_info(f"[ha-group] group#{g['id']} 서비스 의도 승격: {module}=running "
                        f"(agent#{agent_id} start)")
        return g['id']
    return None


def _enqueue_module_spec_for_members(group_id: int, config: dict) -> int:
    """그룹의 각 멤버에게 배포된 daemon 모듈의 운영 명세(service.json)를 push.

    agent 는 update_module_spec job 을 받아 modules/<mod>/service.json 을 기록한다.
    service.json 이 conveying 하는 유일한 신 값은 supervision.watchdog (모듈별 감시
    on/off) — cold/hot·relevant·health 는 ha.json 으로도 전달되지만, service.json
    을 daemon 모듈의 권위 파일로 함께 유지해 노드 로컬 판단(watchdog)이 일관되게
    한다. 명세 변경(_update_group module_specs) 시에만 호출 — 부재 시 agent 는
    watchdog=on default 로 종전과 동일 동작. 큐잉된 job 수 반환."""
    group = _ha_load(config, group_id)
    if not group:
        return 0
    from handlers.agents import _job_create
    enqueued = 0
    for m in (group.get('members') or []):
        aid = m.get('agent_id')
        if aid is None:
            continue
        for mod in _agent_daemon_modules(aid, config):
            _job_create(config, aid, 'update_module_spec',
                        {'module': mod, 'spec': _module_spec(group, mod)})
            enqueued += 1
    return enqueued


async def handle_ha_groups(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """Dispatch /api/v1/ha-groups/* routes."""
    config = kwargs.get('config', {})
    from handlers.agents import _console_rbac
    parts = _path_parts(handler_args.full_path, _HA_GROUPS_BASE)
    # 그룹×패키지 공통 설정/동기화 스위치 = operator+ (deployment config 와 동일 권한).
    # 그 외: GET=monitor+, 변이=admin (무인증 차단, 2026-06-10).
    if len(parts) > 1 and parts[1] == 'packages':
        deny = _console_rbac(handler_args, read_role='operator', write_role='operator')
    else:
        deny = _console_rbac(handler_args)
    if deny: return deny
    group_id = parts[0] if len(parts) > 0 else None
    sub      = parts[1] if len(parts) > 1 else None
    member   = parts[2] if len(parts) > 2 else None
    method = handler_args.method.upper()

    try:
        if group_id is None:
            if method == 'GET':
                return await _list_groups(config)
            elif method == 'POST':
                return await _create_group(handler_args.body, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        try:
            gid = int(group_id)
        except (TypeError, ValueError):
            return HandlerResult(status=400, body={'error': 'invalid group id'})

        if sub is None:
            if method == 'GET':
                return await _get_group(gid, config)
            elif method == 'PUT':
                return await _update_group(gid, handler_args.body, config)
            elif method == 'DELETE':
                return await _delete_group(gid, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        if sub == 'members':
            if member is None:
                if method == 'GET':
                    return await _list_members(gid, config)
                elif method == 'POST':
                    return await _add_member(gid, handler_args.body, config)
                return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
            try:
                aid = int(member)
            except (TypeError, ValueError):
                return HandlerResult(status=400, body={'error': 'invalid agent id'})
            if method == 'DELETE':
                return await _remove_member(gid, aid, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        if sub == 'apply' and method == 'POST':
            return await _apply_group(gid, config)

        # 그룹 일괄 제어 (서비스 시작/중지/재시작) — admin.
        if sub == 'control' and method == 'POST':
            return await _control_group(gid, handler_args.body, config)

        # 수동 절체 (스위치오버, AS 전용) — admin.
        if sub == 'failover' and method == 'POST':
            return await _failover_group(gid, handler_args.body, config)

        if sub == 'collections':
            if not member:
                return HandlerResult(status=400, body={'error': 'collection name required'})
            if method == 'GET':
                return await _get_group_collection(gid, member, handler_args, config)
            if method == 'PUT':
                return await _put_group_collection(gid, member, handler_args, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        # 그룹×패키지 공통 설정 (R4) — /ha-groups/{gid}/packages/{pkg}/config|auto-sync
        if sub == 'packages':
            if not member:
                return HandlerResult(status=400, body={'error': 'package name required'})
            action = parts[3] if len(parts) > 3 else None
            if action == 'config' and method == 'PUT':
                return await _put_group_pkg_config(gid, member, handler_args, config)
            if action == 'auto-sync' and method == 'PUT':
                return await _put_group_auto_sync(gid, member, handler_args, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        return HandlerResult(status=404, body={'error': 'Not Found'})
    except pymysql.IntegrityError as e:
        # uk_agent (1 agent = 1 group) / uk_vrid 위반 등
        return HandlerResult(status=409, body={'error': 'conflict', 'detail': str(e)})
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})


def _attach_member_names(members: list, config: dict) -> list:
    """members rows 에 agent_name 을 file_store 에서 채워준다."""
    from handlers.agents import _agent_load
    cache: dict = {}
    for m in members:
        aid = m.get('agent_id')
        if aid is None:
            continue
        if aid not in cache:
            cache[aid] = _agent_load(config, aid=aid) or {}
        m['agent_name'] = cache[aid].get('name')
    return members


def _attach_derived_role(members: list) -> list:
    """role 은 derived — priority 최대값 멤버가 'master', 나머지 'backup'.
    동률 시 agent_id 작은 쪽 (안정적 tie-break, _compute_master_aid 와 동일).
    옛 record 의 저장된 role 필드는 무시 (응답에서 redundant).
    """
    master_aid = _compute_master_aid(members)
    for m in members:
        m['role'] = 'master' if m.get('agent_id') == master_aid else 'backup'
    return members


def _serialize_group(g: dict, config: dict) -> dict:
    """file_store group dict → 응답용 (멤버 정렬 + agent_name enrich + role derive).

    구 record 는 여기서(GET 경로) 신 스키마로 1회 마이그레이션·영속화한다 — 렌더
    관문(_enqueue_update_ha_for_members)과 동일 시드라 결과 일관."""
    if _ensure_group_migrated(g, config):
        try:
            file_store.save(_ha_dir(config), g.get('id'), g)
        except Exception as e:
            logger.log_warning(f"[ha-group] group#{g.get('id')} 마이그레이션 저장 실패: {e}")
    out = dict(g)
    members = list(out.get('members') or [])
    # priority 우선 정렬, 동률 시 agent_id 오름 (UI 일관 표시)
    members.sort(key=lambda m: (-int(m.get('priority') or 0), int(m.get('agent_id') or 0)))
    members = _attach_derived_role(members)
    out['members'] = _attach_member_names(members, config)
    out.setdefault('vip_bindings', [])
    out['service_intent'] = dict(out.get('service_intent') or {})
    out['module_specs'] = dict(out.get('module_specs') or {})
    # 옛 record (failover_options 미존재) 도 UI 가 매번 채울 필요 없도록 default 응답에 포함.
    out['failover_options'] = _normalize_failover_options(out.get('failover_options'))
    # 실측 ACTIVE (R4) — heartbeat interfaces[] 의 VIP 보유 관측. 정적 role 과 별개로
    # 콘솔이 실제 ACTIVE/STANDBY 를 상시 표시. AS 만 의미 (AA/SA 는 null 생략).
    if out.get('mode') == 'active_standby':
        from services import ha_lookup
        obs = ha_lookup.vip_observation(config, g)
        out['active_agent_id'] = obs['active_agent_id']
        for m in out['members']:
            m['vip_observed'] = obs['observed'].get(m.get('agent_id'))
        # 패키지별 자동 동기화 스위치 (부재 = 기본 ON — 콘솔은 auto_sync[pkg] ?? true)
        out['auto_sync'] = dict(out.get('auto_sync') or {})
    return out


async def _list_groups(config):
    groups = _ha_load_all(config)
    groups.sort(key=lambda g: g.get('id', 0))
    return HandlerResult(status=200,
                         body={'groups': [_serialize_group(g, config) for g in groups]})


async def _get_group(gid: int, config):
    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    return HandlerResult(status=200, body=_serialize_group(g, config))


def _normalize_member(m: dict, idx: int) -> dict:
    """role 은 derived (priority 의 결과). 입력은 role 또는 priority 중 하나로 의도 표현.
    저장은 priority 만 — UI 가 'Master' 선택 → role='master' → priority=100, 나머지 90.

    호환성: 옛 client 가 priority 직접 보내면 그대로 사용. 없으면 role 보고 100/90.
    """
    aid = int(m.get('agent_id'))
    if 'priority' in m and m.get('priority') is not None:
        priority = int(m['priority'])
    else:
        # role 또는 idx 로 default — UI 는 보통 명시 priority 보냄.
        role = m.get('role') or ('master' if idx == 0 else 'backup')
        priority = 100 if role == 'master' else 90
    return {'agent_id': aid, 'priority': priority}


async def _create_group(body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    name = (body.get('name') or '').strip()
    mode = (body.get('mode') or '').strip()
    vip  = (body.get('vip')  or '').strip() or None
    auth_pass = (body.get('auth_pass') or '').strip()
    vip_mask = int(body.get('vip_mask', 24))
    note = body.get('note', '')
    members_in = body.get('members', [])

    if not name:
        return HandlerResult(status=400, body={'error': 'name required'})
    if mode not in ('active_standby', 'all_active'):
        return HandlerResult(status=400, body={'error': 'mode must be active_standby or all_active'})
    # auth_pass — VRRP 인증. active_standby (단일 VIP master/backup) 에서만 필수.
    # all_active 는 VIP 없는 multi-active 시나리오 가정 → 빈 값 허용 (vip_bindings 추가 시점에 갱신).
    if mode == 'active_standby':
        if not auth_pass or len(auth_pass) > 8:
            return HandlerResult(status=400, body={'error': 'auth_pass required for active_standby (max 8 chars)'})
    else:
        if len(auth_pass) > 8:
            return HandlerResult(status=400, body={'error': 'auth_pass max 8 chars'})
    if mode == 'active_standby' and len(members_in) not in (0, 2):
        return HandlerResult(status=400,
                             body={'error': 'active_standby requires exactly 2 members (or 0 for late add)'})

    vip_bindings = body.get('vip_bindings')
    if vip_bindings is not None and not isinstance(vip_bindings, list):
        vip_bindings = None

    vrid = _alloc_vrid(config)
    gid = file_store.next_id(_ha_dir(config))
    members = [_normalize_member(m, i) for i, m in enumerate(members_in)]
    failover_options = _normalize_failover_options(body.get('failover_options'))
    group = {
        'id': gid,
        'name': name,
        'mode': mode,
        'vip': vip,
        'vrid': vrid,
        'vip_mask': vip_mask,
        'auth_pass': auth_pass,
        'note': note,
        'vip_bindings': vip_bindings or [],
        'failover_options': failover_options,
        # 신규 그룹은 미개시(빈 의도) — 서비스 시작 시 무장. 모듈 명세는 default.
        'service_intent': _normalize_service_intent(body.get('service_intent')),
        'module_specs': {
            str(k).strip().lower(): _normalize_module_spec(v)
            for k, v in (body.get('module_specs') or {}).items() if str(k).strip()
        } if isinstance(body.get('module_specs'), dict) else {},
        'members': members,
    }
    file_store.save(_ha_dir(config), gid, group)
    _enqueue_update_ha_for_members(gid, config)
    return HandlerResult(status=201, body={'id': gid, 'vrid': vrid})


async def _update_group(gid: int, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    if 'mode' in body:
        return HandlerResult(status=400, body={'error': 'mode 변경 불가 (그룹 재생성 필요)'})

    existing = _ha_load(config, gid)
    if not existing:
        return HandlerResult(status=404, body={'error': 'Group not found'})

    # mode 변경 차단 — 시스템 유형은 생성 후 변경 불가. 변경 원하면 삭제 후 재생성.
    if 'mode' in body and body['mode'] != existing.get('mode'):
        return HandlerResult(status=400, body={
            'error': 'mode_change_not_allowed',
            'hint': '시스템 유형 (mode) 은 생성 후 변경 불가. 삭제 후 재생성으로 변경하세요.',
        })
    for k in ('name', 'vip', 'auth_pass', 'note'):
        if k in body:
            existing[k] = body[k]
    if 'vip_mask' in body:
        existing['vip_mask'] = int(body['vip_mask'])
    # auth_pass — active_standby 만 1~8자 required, 그 외 mode 는 (빈값 포함) 8자 이하 OK.
    mode_eff = existing.get('mode')
    auth_eff = existing.get('auth_pass') or ''
    if mode_eff == 'active_standby':
        if not auth_eff or len(auth_eff) > 8:
            return HandlerResult(status=400, body={'error': 'auth_pass required for active_standby (max 8 chars)'})
    else:
        if len(auth_eff) > 8:
            return HandlerResult(status=400, body={'error': 'auth_pass max 8 chars'})
    if 'vip_bindings' in body:
        # VIP 적용 시점 자유 — 서비스 의도와 VIP 는 독립 축이다 (구 no_started_modules
        # 409 게이트 폐지). 미개시(의도 stopped) 그룹은 VIP 가 저장돼 있어도 비무장
        # 렌더라 아무 일도 일어나지 않고, 서비스 시작(의도 running) 시 자동 무장한다.
        # 상세: ha_service_model.md §2.
        v = body.get('vip_bindings')
        existing['vip_bindings'] = v if isinstance(v, list) else []
    if 'service_intent' in body:
        existing['service_intent'] = _normalize_service_intent(body.get('service_intent'))
    module_specs_changed = False
    if 'module_specs' in body and isinstance(body.get('module_specs'), dict):
        existing['module_specs'] = {
            str(k).strip().lower(): _normalize_module_spec(v)
            for k, v in body['module_specs'].items() if str(k).strip()
        }
        module_specs_changed = True
    if 'failover_options' in body:
        existing['failover_options'] = _normalize_failover_options(body.get('failover_options'))
        # 전환기 호환 — 구 콘솔이 module_specs 없이 module_modes/tracked_modules 만
        # 보내면 명세로 폴딩(그렇지 않으면 render 가 module_specs SoT 만 보므로 무시됨).
        # 신 콘솔은 module_specs 를 직접 보내 이 경로를 타지 않는다.
        if 'module_specs' not in body:
            fo = existing['failover_options']
            modes = fo.get('module_modes') or {}
            tracked = {str(m).strip().lower() for m in (fo.get('tracked_modules') or [])}
            if modes or tracked:
                specs = dict(existing.get('module_specs') or {})
                for m in (set(modes.keys()) | tracked):
                    sp = _normalize_module_spec(specs.get(m))
                    sp['ha']['failover_mode'] = 'hot' if modes.get(m) == 'hot' else 'cold'
                    specs[m] = sp
                existing['module_specs'] = specs
                module_specs_changed = True
    dropped_aids: list = []
    if 'members' in body:
        old_aids = {m.get('agent_id') for m in (existing.get('members') or [])
                    if m.get('agent_id') is not None}
        existing['members'] = [_normalize_member(m, i) for i, m in enumerate(body['members'])]
        new_aids = {m['agent_id'] for m in existing['members']}
        dropped_aids = sorted(old_aids - new_aids)

    file_store.save(_ha_dir(config), gid, existing)
    _enqueue_update_ha_for_members(gid, config)
    # 모듈 운영 명세 변경 시 service.json 을 멤버에 push (watchdog on/off 등).
    if module_specs_changed:
        n = _enqueue_module_spec_for_members(gid, config)
        if n:
            logger.log_info(f"[ha-group] group#{gid} module_specs 변경 → update_module_spec {n}건 큐잉")
    # 멤버 교체로 이탈한 agent 는 재렌더 대상에서 빠진다 — 빈 services 로 keepalived 해제.
    for aid in dropped_aids:
        _enqueue_disarm_for_agent(aid, config)
    return HandlerResult(status=200, body={'id': gid})


async def _delete_group(gid: int, config):
    # 삭제 전 멤버 확보 — 삭제 후에는 렌더 대상에서 빠져 disarm 을 보낼 수 없다.
    g = _ha_load(config, gid)
    if not file_store.delete(_ha_dir(config), gid):
        return HandlerResult(status=404, body={'error': 'Group not found'})
    disarmed = 0
    for m in (g.get('members') or []) if g else []:
        if m.get('agent_id') is not None:
            disarmed += _enqueue_disarm_for_agent(m['agent_id'], config)
    if disarmed:
        logger.log_info(f"[ha-group] group#{gid} 삭제 → 이탈 멤버 disarm {disarmed}건 큐잉")
    return HandlerResult(status=200, body={'id': gid, 'deleted': True})


# ════════════════════════════════════════════════════════════
#  그룹 일괄 제어 + 수동 절체 (ha_service_model.md §6·§7)
# ════════════════════════════════════════════════════════════

def _group_member_daemon_deps(group: dict, config: dict) -> list:
    """그룹 멤버들의 daemon 배포(status != removed) 목록 — 일괄 제어 대상.
    (process_name 이 health defaults 에 있는 리슨 데몬만; cspsim/console 등 제외.)"""
    from handlers.agents import _deploy_load_all
    defaults = service_registry.module_health_defaults(config) or _MODULE_HEALTH_DEFAULTS
    aids = {m.get('agent_id') for m in (group.get('members') or [])}
    out = []
    for d in _deploy_load_all(config):
        if d.get('agent_id') not in aids or d.get('status') == 'removed':
            continue
        mod = (d.get('process_name') or '').lower().strip()
        if mod in defaults:
            out.append(d)
    return out


def _queue_lifecycle_job(config, dep: dict, job_type: str, not_before: str | None = None) -> int:
    """단일 배포에 start/stop/restart job 큐잉 — agents._queue_job 과 동일 params 형태.
    일괄 제어용 (handler_args 없이 배포 레코드에서 직접 구성)."""
    from handlers.agents import (_job_create, _deploy_update, _enrich_deploy,
                                 _pkg_load, _materialize_deploy_config, _safe_json,
                                 _split_csv)
    _enrich_deploy([dep], config)
    cfg = dep.get("config") if isinstance(dep.get("config"), (dict, list)) \
          else _safe_json(dep.get("config_json"))
    if isinstance(cfg, dict) or cfg is None:
        try:
            pkg = _pkg_load(config, dep.get("package_id"))
            cfg = _materialize_deploy_config(config, pkg, cfg)
        except Exception:
            pass
    sf = dep.get("service_functions")
    if isinstance(sf, str):
        sf = _split_csv(sf)
    params = {
        "deployment_id":   dep.get("id"),
        "package_id":      dep.get("package_id"),
        "package_name":    dep.get("package_name"),
        "package_version": dep.get("package_version"),
        "process_name":    dep.get("process_name"),
        "service_functions": sf or [],
        "install_path":    dep.get("install_path"),
        "config":          cfg,
    }
    jid = _job_create(config, dep["agent_id"], job_type, params, not_before=not_before)
    _deploy_update(config, dep["id"], {'status': 'deploying', 'last_job_id': jid})
    return jid


async def _control_group(gid: int, body, config):
    """그룹 일괄 제어 — body = {"action": "start"|"stop"|"restart"}.

    start   : 서비스 의도 running (전 daemon 모듈) → 무장 재렌더(기준 멤버 선행) +
              각 멤버 daemon 배포 start job.
    stop    : 서비스 의도 stopped → 비무장 재렌더(update_ha 먼저) + stop job (뒤). agent
              큐 순서 처리라 노드별 마지막 말이 stop — 절체 레이스가 살려도 최종 정지.
    restart : 의도 불변. AS 는 standby 먼저·active 지연(op_grace 로 절체 억제). 순단 1회.
    상세: ha_service_model.md §6."""
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    action = (body.get('action') or '').strip().lower()
    if action not in ('start', 'stop', 'restart'):
        return HandlerResult(status=400, body={'error': "action must be start|stop|restart"})
    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    _ensure_group_migrated(g, config)

    deps = await asyncio.to_thread(_group_member_daemon_deps, g, config)
    daemon_mods = sorted({(d.get('process_name') or '').lower().strip() for d in deps
                          if d.get('process_name')})

    if action == 'start':
        intent = dict(g.get('service_intent') or {})
        for m in daemon_mods:
            intent[m] = 'running'
        g['service_intent'] = intent
        file_store.save(_ha_dir(config), gid, g)
        base = _compute_master_aid(g.get('members') or [])
        prefer = {base} if base is not None else None
        n_ha = await asyncio.to_thread(_enqueue_update_ha_for_members, gid, config, prefer)
        n_job = 0
        for d in deps:
            await asyncio.to_thread(_queue_lifecycle_job, config, d, 'start')
            n_job += 1
        logger.log_info(f"[ha-group] group#{gid} 일괄 시작 — 의도 running {daemon_mods}, "
                        f"update_ha {n_ha}, start job {n_job}")
        return HandlerResult(status=202, body={'action': 'start', 'group_id': gid,
                                               'modules': daemon_mods, 'jobs': n_job})

    if action == 'stop':
        g['service_intent'] = {m: 'stopped' for m in (g.get('service_intent') or {})}
        for m in daemon_mods:
            g['service_intent'][m] = 'stopped'
        file_store.save(_ha_dir(config), gid, g)
        # update_ha(비무장) 먼저 큐잉 → 낮은 job id → agent 가 먼저 처리(keepalived 정지).
        n_ha = await asyncio.to_thread(_enqueue_update_ha_for_members, gid, config)
        n_job = 0
        for d in deps:
            await asyncio.to_thread(_queue_lifecycle_job, config, d, 'stop')
            n_job += 1
        logger.log_info(f"[ha-group] group#{gid} 일괄 중지 — 의도 stopped, "
                        f"update_ha(disarm) {n_ha}, stop job {n_job}")
        return HandlerResult(status=202, body={'action': 'stop', 'group_id': gid,
                                               'modules': daemon_mods, 'jobs': n_job})

    # restart — 의도 불변. AS 는 active 를 지연시켜 standby 준비 후 재기동(op_grace 억제).
    active_aid = None
    if g.get('mode') == 'active_standby':
        from services import ha_lookup
        active_aid = (ha_lookup.vip_observation(config, g) or {}).get('active_agent_id')
    not_before_active = None
    if active_aid is not None:
        from datetime import datetime, timedelta
        not_before_active = (datetime.now() + timedelta(seconds=15)).isoformat(timespec='seconds')
    n_job = 0
    for d in deps:
        nb = not_before_active if (active_aid is not None and d.get('agent_id') == active_aid) else None
        await asyncio.to_thread(_queue_lifecycle_job, config, d, 'restart', nb)
        n_job += 1
    logger.log_info(f"[ha-group] group#{gid} 일괄 재시작 — restart job {n_job} "
                    f"(active#{active_aid} 15s 지연)")
    return HandlerResult(status=202, body={'action': 'restart', 'group_id': gid,
                                           'modules': daemon_mods, 'jobs': n_job})


async def _failover_group(gid: int, body, config):
    """수동 절체(스위치오버) — AS 전용. body 무시(옵션).

    현 Active 의 keepalived 를 정지(priority-0 → peer 즉시 승격)한 뒤, 지연 후
    재기동(nopreempt → BACKUP 복귀). 두 개의 ha_keepalived job 으로 오케스트레이션.
    상세: ha_service_model.md §7."""
    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if g.get('mode') != 'active_standby':
        return HandlerResult(status=409, body={'error': 'not_active_standby',
                                               'hint': '수동 절체는 Active/Standby 그룹만'})
    _ensure_group_migrated(g, config)
    intent_running = {m for m, s in (g.get('service_intent') or {}).items() if s == 'running'}
    if not intent_running:
        return HandlerResult(status=409, body={'error': 'not_armed',
            'hint': '미개시 그룹 — 서비스 시작(무장) 후 절체 가능'})

    from services import ha_lookup
    from handlers.agents import _agent_load, _job_create
    obs = ha_lookup.vip_observation(config, g) or {}
    active_aid = obs.get('active_agent_id')
    members = g.get('members') or []
    if active_aid is None:
        return HandlerResult(status=409, body={'error': 'active_unresolved',
            'hint': 'Active 판정 불가(관측 창/전원 stale) — 잠시 후 재시도'})
    targets = [m.get('agent_id') for m in members if m.get('agent_id') != active_aid]
    if not targets:
        return HandlerResult(status=409, body={'error': 'no_standby_target'})
    target_aid = targets[0]
    # 대상 standby 승격 자격 — online + 이탈 오버라이드 없음(간이 검사: agent online).
    ta = _agent_load(config, aid=target_aid) or {}
    if ta.get('status') != 'online':
        return HandlerResult(status=409, body={'error': 'target_offline',
            'hint': f'standby agent#{target_aid} 오프라인 — 절체 불가'})

    # 1) 현 Active keepalived 정지 (즉시) → peer 승격.
    _job_create(config, active_aid, 'ha_keepalived', {'action': 'stop'})
    # 2) 구 Active keepalived 재기동 (지연) → nopreempt BACKUP 복귀 + cold 모듈 정지.
    from datetime import datetime, timedelta
    nb = (datetime.now() + timedelta(seconds=20)).isoformat(timespec='seconds')
    _job_create(config, active_aid, 'ha_keepalived', {'action': 'start'}, not_before=nb)
    logger.log_info(f"[ha-group] group#{gid} 수동 절체 — active#{active_aid} keepalived "
                    f"stop→(20s)start, standby#{target_aid} 승격")
    return HandlerResult(status=202, body={'group_id': gid, 'from_agent_id': active_aid,
                                           'to_agent_id': target_aid,
                                           'note': 'switchover queued (stop→start)'})


async def _list_members(gid: int, config):
    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    members = sorted(g.get('members') or [], key=lambda m: -int(m.get('priority') or 0))
    return HandlerResult(status=200, body={'members': _attach_member_names(members, config)})


async def _add_member(gid: int, body, config):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    aid = int(body.get('agent_id', 0))
    if not aid:
        return HandlerResult(status=400, body={'error': 'agent_id required'})
    # role 또는 priority 둘 중 하나로 의도 표현 — _normalize_member 가 priority 로 통일.
    norm = _normalize_member({'agent_id': aid,
                              'role': body.get('role'),
                              'priority': body.get('priority')}, 0)

    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    members = list(g.get('members') or [])
    # 동일 agent_id 가 이미 있으면 priority 갱신
    found = False
    for m in members:
        if m.get('agent_id') == aid:
            m['priority'] = norm['priority']
            m.pop('role', None)  # 옛 'role' 잔재 제거 (derived 가 SoT)
            found = True
            break
    if not found:
        members.append(norm)
    g['members'] = members
    file_store.save(_ha_dir(config), gid, g)
    _enqueue_update_ha_for_members(gid, config)
    return HandlerResult(status=201, body={'group_id': gid, 'agent_id': aid})


async def _apply_group(gid: int, config):
    """그룹의 모든 멤버에 update_ha job 큐잉 — VipPanel [적용] 진입점.

    VIP 적용 시점 자유 — 게이트 없음 (ha_service_model.md §2). 미개시 그룹은
    비무장 렌더라 apply 해도 keepalived 정지 유지, 서비스 시작 시 자동 무장."""
    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    count = _enqueue_update_ha_for_members(gid, config)
    return HandlerResult(status=202, body={'group_id': gid, 'jobs_queued': count})


async def _remove_member(gid: int, aid: int, config):
    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    members = list(g.get('members') or [])
    new_members = [m for m in members if m.get('agent_id') != aid]
    if len(new_members) == len(members):
        return HandlerResult(status=404, body={'error': 'Member not found'})
    g['members'] = new_members
    file_store.save(_ha_dir(config), gid, g)
    _enqueue_update_ha_for_members(gid, config)
    # 이탈한 멤버는 위 재렌더 대상에서 빠진다 — 빈 services 로 keepalived 해제.
    _enqueue_disarm_for_agent(aid, config)
    return HandlerResult(status=200, body={'group_id': gid, 'agent_id': aid, 'removed': True})


def _find_group_member_deployments(gid: int, name: str, package_id, config) -> tuple:
    """그룹 멤버들의 deployment 중 (선택적 package_id 필터 + collection name 정의됨) 매칭.

    반환: (group, [matched_deployment, ...], schema). 매칭 0개면 schema=None.
    schema 는 첫 매칭 deployment 의 template 에서 추출 (멤버간 일관 가정).
    """
    from handlers.agents import _deploy_load_all, _fetch_deployment_for_proxy, _collection_schema
    g = _ha_load(config, gid)
    if not g:
        return None, [], None
    member_ids = {int(m.get('agent_id')) for m in (g.get('members') or [])
                  if m.get('agent_id') is not None}
    all_deps = _deploy_load_all(config)
    matched = []
    schema = None
    for d in all_deps:
        if int(d.get('agent_id', 0)) not in member_ids:
            continue
        if package_id is not None and int(d.get('package_id', 0)) != int(package_id):
            continue
        # template 에 해당 collection 정의 있는지 확인
        dep = _fetch_deployment_for_proxy(int(d['id']), config)
        if not dep:
            continue
        s, _ = _collection_schema(dep.get('config_template_json'), name)
        if s is None:
            continue
        if schema is None:
            schema = s
        matched.append(dep)
    return g, matched, schema


def _parse_package_id(handler_args) -> "int | None":
    qp = handler_args.query_params or {}
    raw = qp.get('package_id')
    if isinstance(raw, list):
        raw = raw[0] if raw else None
    if raw is None or raw == '':
        return None
    try:
        return int(raw)
    except (TypeError, ValueError):
        return None


async def _get_group_collection(gid: int, name: str, handler_args, config):
    """그룹 멤버의 첫 매칭 deployment 에서 collection records fetch."""
    from handlers.agents import _agent_proxy_call
    package_id = _parse_package_id(handler_args)
    g, matched, schema = await asyncio.to_thread(
        _find_group_member_deployments, gid, name, package_id, config)
    if g is None:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if not matched:
        return HandlerResult(status=404,
            body={'error': 'no_matching_deployment',
                  'hint': '그룹 멤버에 collection 정의된 패키지 미배포'})

    dep = matched[0]
    status, resp = await asyncio.to_thread(
        _agent_proxy_call, 'GET', dep,
        '/collection', {'install_path': dep['install_path'], 'name': name},
        None, 15, config)
    if status == 200:
        return HandlerResult(status=200,
            body={'records': resp.get('records') or [], 'schema': schema,
                  'source_deployment_id': dep['id'], 'member_count': len(matched)})
    return HandlerResult(status=status or 502,
        body={'error': 'agent_proxy_failed', 'detail': resp,
              'source_deployment_id': dep['id']})


async def _put_group_collection(gid: int, name: str, handler_args, config):
    """그룹 멤버 deployment 전체에 fan-out PUT. per-member 결과 array 반환."""
    from handlers.agents import _agent_proxy_call, _validate_record, _parse_body
    import uuid as _uuid

    package_id = _parse_package_id(handler_args)
    g, matched, schema = await asyncio.to_thread(
        _find_group_member_deployments, gid, name, package_id, config)
    if g is None:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if not matched:
        return HandlerResult(status=404,
            body={'error': 'no_matching_deployment'})

    body = _parse_body(handler_args)
    records = body.get('records')
    if not isinstance(records, list):
        return HandlerResult(status=400, body={'error': 'records array required'})

    # validation + auto id (deployment PUT 와 동일 로직)
    id_field = schema.get('id_field') or 'id'
    id_type  = schema.get('id_type') or 'uuid'
    all_errors = []
    for i, r in enumerate(records):
        if not isinstance(r, dict):
            all_errors.append({'index': i, 'errors': ['not_object']})
            continue
        if id_type == 'uuid' and not r.get(id_field):
            r[id_field] = _uuid.uuid4().hex[:16]
        errs = _validate_record(schema, r)
        if errs:
            all_errors.append({'index': i, 'errors': errs})
    if all_errors:
        return HandlerResult(status=400,
            body={'error': 'validation_failed', 'details': all_errors})

    do_signal = body.get('signal', True)
    results = []
    for dep in matched:
        status, resp = await asyncio.to_thread(
            _agent_proxy_call, 'PUT', dep,
            '/collection', {'install_path': dep['install_path'], 'name': name},
            {'records': records, 'signal': do_signal}, 15, config)
        if status == 200:
            results.append({'deployment_id': dep['id'],
                            'agent_id': dep.get('agent_id'),
                            'count': resp.get('count'),
                            'signaled': resp.get('signaled') or []})
        else:
            results.append({'deployment_id': dep['id'],
                            'agent_id': dep.get('agent_id'),
                            'error': resp, 'status': status})

    overall_ok = all('error' not in r for r in results)
    return HandlerResult(status=200 if overall_ok else 207,
        body={'ok': overall_ok, 'members': results})


# ════════════════════════════════════════════════════════════
#  그룹×패키지 공통 설정 + 자동 동기화 스위치 (R4)
# ════════════════════════════════════════════════════════════

async def _put_group_pkg_config(gid: int, pkg_name: str, handler_args, config):
    """그룹 공통(service) 설정 저장 — 콘솔 그룹 탭 편집기 (AS 그룹 전용).

    body = { "values": {<key>: <value>, ...},   # 유효 scope=service 키만 (아니면 400)
             "target_deployment_id"?: int,      # 스위치 OFF 의 멤버 선택 편집
             "queue_update"?: bool }

    스위치 ON:  target 없이 호출 — 전 멤버 overlay 에 merge (버전 혼재면 409).
                target 지정은 400 — ON 상태의 멤버별 저장은 자동 교정이 곧 되돌리므로
                편집 모델에서 배제 (스위치 OFF 후 수정하도록 안내).
    스위치 OFF: target_deployment_id 필수 — 그 멤버에만 merge (업그레이드 창 편집).
    """
    from services import ha_lookup
    from handlers.agents import (_enrich_deploy, _pkg_load, _safe_json,
                                 _service_scope_keys, _coerce_list_fields,
                                 _deploy_update, _enqueue_update_config_jobs)

    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if g.get('mode') != 'active_standby':
        return HandlerResult(status=409, body={'error': 'not_active_standby',
            'hint': 'AA/standalone 은 그룹 공통 설정 편집이 없음 — 각 서버에서 편집'})

    body = handler_args.body if isinstance(handler_args.body, dict) else {}
    values = body.get('values')
    if not isinstance(values, dict) or not values:
        return HandlerResult(status=400, body={'error': 'values dict required'})
    queue_update = body.get('queue_update', True)
    target_id = body.get('target_deployment_id')
    sync_on = ha_lookup.auto_sync_enabled(g, pkg_name)
    if sync_on and target_id is not None:
        return HandlerResult(status=400, body={'error': 'target_not_allowed_while_sync_on',
            'hint': '멤버별 저장은 동기화 스위치 OFF 상태에서만'})
    if not sync_on and target_id is None:
        return HandlerResult(status=409, body={'error': 'target_required_while_sync_off',
            'hint': '동기화 OFF — 편집할 멤버(target_deployment_id)를 선택'})

    deps = await asyncio.to_thread(
        ha_lookup.deployments_in_group_for_package, config, gid, pkg_name)
    if not deps:
        return HandlerResult(status=404, body={'error': 'package_not_deployed_in_group'})
    _enrich_deploy(deps, config)

    if target_id is not None:
        targets = [d for d in deps if d.get('id') == int(target_id)]
        if not targets:
            return HandlerResult(status=404,
                body={'error': 'target_not_in_group', 'deployment_id': target_id})
    else:
        # ON: 전 멤버 — 버전 혼재면 어느 템플릿 기준인지 모호 → 409 (스위치 OFF 유도)
        vers = {d.get('package_version') for d in deps}
        if len(vers) > 1:
            return HandlerResult(status=409,
                body={'error': 'version_mismatch', 'versions': sorted(v or '?' for v in vers),
                      'hint': '버전 혼재 — 스위치 OFF 후 멤버별로 편집'})
        targets = deps

    # 키 검증 — 각 target 의 템플릿 기준 유효 scope=service 만 허용
    saved = []
    applied_keys: set = set()
    first_pkg, first_old, first_new = None, None, None   # upstream 전파 비교용
    for t in targets:
        pkg = await asyncio.to_thread(_pkg_load, config, t.get('package_id')) or {}
        template = pkg.get('config_template') if isinstance(pkg, dict) else None
        allowed = _service_scope_keys(template)
        bad = [k for k in values.keys() if k not in allowed]
        if bad:
            return HandlerResult(status=400,
                body={'error': 'non_service_keys', 'keys': sorted(bad),
                      'hint': 'scope=system(서버 개별) 키는 각 서버의 설정 탭에서'})
        vals = _coerce_list_fields(template, dict(values)) if isinstance(template, dict) else dict(values)
        cur = t.get('config')
        if not isinstance(cur, dict):
            cur = _safe_json(t.get('config_json')) or {}
        new_overlay = {**cur, **vals}
        updated = await asyncio.to_thread(_deploy_update, config, t['id'],
                                          {'config': new_overlay})
        if updated:
            saved.append(updated)
            applied_keys.update(vals.keys())
            if first_pkg is None:
                first_pkg, first_old, first_new = pkg, cur, new_overlay

    # 실효 upstream(Server.Port/Server.GatewayHost) 변경 전파 — 개별 배포 설정 저장
    # (_put_deployment_config)과 동일 규칙. GatewayHost/Port 는 scope=service 라 주로
    # 이 그룹 공통 경로로 저장되는데, 여기서 재등록을 안 태우면 게이트웨이가 구
    # upstream 을 계속 본다. service 키는 멤버 간 동일하므로 첫 target 기준 1회 비교.
    if first_pkg is not None:
        try:
            from handlers.agents import effective_server_port, effective_gateway_host
            old_port = effective_server_port(config, first_pkg, first_old)
            new_port = effective_server_port(config, first_pkg, first_new)
            old_host = effective_gateway_host(config, first_pkg, first_old) or '127.0.0.1'
            new_host = effective_gateway_host(config, first_pkg, first_new) or '127.0.0.1'
            if new_port and (new_port != old_port or new_host != old_host):
                _meta = first_pkg.get('meta') if isinstance(first_pkg, dict) else None
                gw_routes = ((_meta or {}).get('gateway') or {}).get('routes') or []
                proc = saved[0].get('process_name')
                if gw_routes and proc:
                    import handlers.gateway as _gw
                    await asyncio.to_thread(_gw.register_module_routes, config,
                                            proc, new_host, int(new_port), gw_routes)
                n = 0
                if new_port != old_port:
                    for aid in {d.get('agent_id') for d in saved if d.get('agent_id') is not None}:
                        n += await asyncio.to_thread(enqueue_update_ha_for_agent, aid, config)
                logger.log_info(f"[group-config] group#{gid} pkg={pkg_name} 실효 upstream "
                                f"{old_host}:{old_port}->{new_host}:{new_port}: gateway 재등록"
                                f"={'O' if gw_routes else 'X'}, update_ha {n}건")
        except Exception as e:
            logger.log_warning(f"[group-config] upstream 변경 전파 실패(group#{gid} pkg={pkg_name}): {e}")

    members, sync_id = [], None
    if queue_update and saved:
        _enrich_deploy(saved, config)
        pkg = await asyncio.to_thread(_pkg_load, config, saved[0].get('package_id')) or {}
        members, sync_id = await asyncio.to_thread(
            _enqueue_update_config_jobs, config, saved, pkg,
            op='group_config', actor='console',
            note=f"group_config ha_group#{gid} pkg={pkg_name}"
                 f"{f' target#{target_id}' if target_id is not None else ''}")

    return HandlerResult(status=200, body={
        'ok': True,
        'applied_keys': sorted(applied_keys),
        'sync_on': sync_on,
        'members': members,
        'sync_id': sync_id,
    })


async def _put_group_auto_sync(gid: int, pkg_name: str, handler_args, config):
    """그룹×패키지 자동 동기화 스위치 (R4). body = {"enabled": bool}

    ha_group.auto_sync[pkg] 영속 (부재 = 기본 ON). ON 전환 시 즉시 정합 1회 실행 —
    ACTIVE 판정 불가·버전 혼재면 정합은 보류되고 사유가 응답에 담긴다 (스위퍼가
    조건 충족 시 자동 재시도)."""
    from services import ha_lookup
    from handlers.agents import reconcile_group_package

    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if g.get('mode') != 'active_standby':
        return HandlerResult(status=409, body={'error': 'not_active_standby'})
    body = handler_args.body if isinstance(handler_args.body, dict) else {}
    if not isinstance(body.get('enabled'), bool):
        return HandlerResult(status=400, body={'error': 'enabled bool required'})
    enabled = body['enabled']

    def _persist():
        g2 = ha_lookup.ha_group_by_id(config, gid)
        au = dict(g2.get('auto_sync') or {})
        au[pkg_name] = enabled
        g2['auto_sync'] = au
        ha_lookup.save_group(config, g2)
        return g2
    g = await asyncio.to_thread(_persist)

    reconcile = None
    if enabled:
        reconcile = await asyncio.to_thread(
            reconcile_group_package, config, g, pkg_name,
            include_collections=True, actor='switch-on')
    return HandlerResult(status=200, body={
        'ok': True, 'package': pkg_name, 'enabled': enabled,
        'reconcile': reconcile,
    })


CIMS_HA_GROUPS_HANDLER_LIST = (
    (_HA_GROUPS_BASE, handle_ha_groups, {}),
)
