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
    'tracked_modules': [],
    'module_modes':    {},   # {module: 'cold'|'hot'} — 미지정 = cold (기본 cold-spare)
    'preempt':         'nopreempt',
    'preempt_delay':   0,
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

    tm = raw.get('tracked_modules') or []
    if isinstance(tm, list):
        out['tracked_modules'] = [str(x).strip().lower() for x in tm if str(x).strip()]
    else:
        out['tracked_modules'] = []

    # 모듈별 절체 모드 — cold(기본): standby 정지, MASTER 승격 시 notify 가 기동.
    # hot: 양쪽 상시 기동(VIP-only 절체). 미지정 모듈은 cold. 실제 daemon 모듈과의
    # 교차는 render(_render_ha_for_agent)에서 수행 — 여기선 값 정규화만.
    mm = raw.get('module_modes') if isinstance(raw.get('module_modes'), dict) else {}
    out['module_modes'] = {
        str(k).strip().lower(): ('hot' if str(v).strip().lower() == 'hot' else 'cold')
        for k, v in mm.items() if str(k).strip()
    }

    pe = raw.get('preempt') or _FAILOVER_DEFAULTS['preempt']
    out['preempt'] = pe if pe in ('preempt', 'nopreempt') else _FAILOVER_DEFAULTS['preempt']

    try:
        pd = int(raw.get('preempt_delay', 0))
        out['preempt_delay'] = pd if 0 <= pd <= 300 else 0
    except (TypeError, ValueError):
        out['preempt_delay'] = 0

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

    # 서비스 개시 게이트 — HA 는 운영자가 start 한 모듈만 관리한다. 설치만 된
    # 모듈로 헬스포트를 유도하거나 cold 절체 대상에 넣으면, 아무 서비스도 개시되지
    # 않은 그룹이 무장되어 미기동 포트 검사로 flap 하고 콘솔에 Active/Standby 가
    # 표시된다 (상태는 서비스가 개시된 그룹에만 존재해야 한다).
    started = _group_started_modules(members, config) if config else set()

    # cims-health 가 lookup 하는 port/proto — agent 의 개시된 deployment 로 추정.
    h_port, h_proto, h_module = _infer_health_port_proto(agent_id, config, allowed=started) if config else (None, None, None)

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

    # cold-spare 절체 대상 — AS 그룹의 daemon 모듈 중 module_modes 가 hot 이 아닌 전부
    # (기본 cold). cims-notify 가 MASTER 승격 시 start / BACKUP·FAULT 강등 시 stop.
    # hot 모듈과 oam(descriptor 비데몬 — 관리 평면 자신)은 양쪽 상시 기동 유지.
    # armed = 이 agent 의 daemon 배포 중 "개시된" 모듈만 — cold 절체 대상도 여기서만.
    daemon_mods = [m for m in (_agent_daemon_modules(agent_id, config) if config else [])
                   if m in started]
    cold_modules: list = []
    if group.get('mode') == 'active_standby':
        modes = failover_options.get('module_modes') or {}
        cold_modules = [m for m in daemon_mods if modes.get(m, 'cold') != 'hot']

    # 개시된 daemon 모듈이 없고 헬스포트도 (유도/수동 지정) 없는 멤버 — 빈 서버
    # 또는 설치만 된(미개시) 그룹 — vrrp_instance 를 내리지 않는다 (enabled=false →
    # cims-ha 렌더 스킵 + keepalived 정지 유지). VIP/Active 상태 자체가 생기지 않음.
    # 이후 모듈 설치·start/stop job 완료가 재렌더를 태워 자동 무장/해제된다.
    ha_enabled = bool(h_port or daemon_mods)

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


def _enqueue_update_ha_for_members(group_id: int, config: dict) -> int:
    """그룹 멤버들에게 update_ha job 큐잉. 큐잉된 job 수 반환."""
    group = _ha_load(config, group_id)
    if not group:
        return 0
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

    enqueued = 0
    for m in members:
        agent = agents.get(m['agent_id'])
        if not agent:
            continue
        peer = None
        for other in members:
            if other['agent_id'] != m['agent_id']:
                peer = agents.get(other['agent_id'])
                break
        ha_json = _render_ha_for_agent(group, members, m['agent_id'], agent, peer, vip_bindings, config)
        params = {
            # install_path 는 구 agent(flat 레이아웃) 호환용 잔재 — 신 agent 는 무시하고
            # <prefix>/run/keepalived/ 에 기록한다 (agent job_update_ha 참조).
            "install_path": f"/opt/cims/{agent.get('name','agent')}",
            "ha_json": ha_json,
        }
        _job_create(config, m['agent_id'], 'update_ha', params)
        enqueued += 1
    return enqueued


def enqueue_update_ha_for_agent(agent_id: int, config: dict) -> int:
    """agent 가 속한 모든 HA 그룹에 update_ha 재렌더 큐잉 — 배포 설정 변경으로
    헬스포트 등 렌더 입력이 바뀌었을 때 ha.json 이 자동 추종하는 경로.
    (그룹 렌더는 멤버 전체가 한 단위 — 해당 그룹 전 멤버에게 재푸시.)"""
    enqueued = 0
    for g in _ha_load_all(config):
        if any(m.get('agent_id') == agent_id for m in (g.get('members') or [])):
            enqueued += _enqueue_update_ha_for_members(g.get('id'), config)
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
    """file_store group dict → 응답용 (멤버 정렬 + agent_name enrich + role derive)."""
    out = dict(g)
    members = list(out.get('members') or [])
    # priority 우선 정렬, 동률 시 agent_id 오름 (UI 일관 표시)
    members.sort(key=lambda m: (-int(m.get('priority') or 0), int(m.get('agent_id') or 0)))
    members = _attach_derived_role(members)
    out['members'] = _attach_member_names(members, config)
    out.setdefault('vip_bindings', [])
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
        v = body.get('vip_bindings')
        existing['vip_bindings'] = v if isinstance(v, list) else []
    if 'failover_options' in body:
        existing['failover_options'] = _normalize_failover_options(body.get('failover_options'))
    if 'members' in body:
        existing['members'] = [_normalize_member(m, i) for i, m in enumerate(body['members'])]

    file_store.save(_ha_dir(config), gid, existing)
    _enqueue_update_ha_for_members(gid, config)
    return HandlerResult(status=200, body={'id': gid})


async def _delete_group(gid: int, config):
    if not file_store.delete(_ha_dir(config), gid):
        return HandlerResult(status=404, body={'error': 'Group not found'})
    return HandlerResult(status=200, body={'id': gid, 'deleted': True})


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
    """그룹의 모든 멤버에 update_ha job 큐잉 — VipPanel [적용] 진입점."""
    if not _ha_load(config, gid):
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
