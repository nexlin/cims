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
import os
import re

from httpsrv.handler import HandlerArgs, HandlerResult
from services import file_store, service_registry
from services.lease import LeaseLostError
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
_SAFETY_CLASSES = ('stateless', 'read_only', 'shared_writer', 'unknown')
_MODULE_SPEC_DEFAULT = {
    'supervision': {'watchdog': True},
    'ha':          {'failover_mode': 'cold', 'failover_relevant': True},
    'health':      {},   # {port,proto,config_key} 오버라이드 — 미지정 시 배포 유도
    'safety':      {'class': 'unknown', 'latch_clear_mode': 'manual'},
}


def _normalize_module_spec(raw) -> dict:
    """입력 dict → 검증된 모듈 운영 명세. 잘못된 값은 default. (ha_service_model.md §5·§14)"""
    raw = raw if isinstance(raw, dict) else {}
    sup = raw.get('supervision') if isinstance(raw.get('supervision'), dict) else {}
    ha  = raw.get('ha') if isinstance(raw.get('ha'), dict) else {}
    hl  = raw.get('health') if isinstance(raw.get('health'), dict) else {}
    sf  = raw.get('safety') if isinstance(raw.get('safety'), dict) else {}
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
    if isinstance(hl.get('profile'), str) and hl['profile'].strip():
        health['profile'] = hl['profile'].strip()
    # 안전 등급 — shared_writer/unknown 은 자동 래치 해제 금지(수동). stateless/read_only 만 auto 허용.
    sclass = str(sf.get('class') or 'unknown').lower()
    if sclass not in _SAFETY_CLASSES:
        sclass = 'unknown'
    lcm = str(sf.get('latch_clear_mode') or '').lower()
    if lcm not in ('auto', 'manual'):
        lcm = 'auto' if sclass in ('stateless', 'read_only') else 'manual'
    safety = {'class': sclass, 'latch_clear_mode': lcm}
    # 리더 리스 요구 — 공유 볼륨 단일 writer 가 전제인 모듈(관리평면). 소유권 리스를
    # 못 잡으면 write 를 거부해야 하므로 명세로 노출한다(oam_ha.md §4.4·§6.2).
    if sf.get('requires_leader_lease') is not None:
        safety['requires_leader_lease'] = bool(sf.get('requires_leader_lease'))
    return {
        'supervision': {'watchdog': bool(sup.get('watchdog', True))},
        'ha': {
            'failover_mode':     'hot' if ha.get('failover_mode') == 'hot' else 'cold',
            'failover_relevant': bool(ha.get('failover_relevant', True)),
        },
        'health': health,
        'safety': safety,
    }


def _descriptor_module_spec_defaults(mod: str) -> dict:
    """service descriptor 가 데이터로 선언한 모듈 기본 명세 (현재 safety 블록).

    코드 상수가 아니라 descriptor(데이터)가 SoT — 관리평면처럼 `shared_writer` 로 다뤄야
    하는 모듈이 그룹마다 수동 설정 없이도 올바른 등급을 갖게 한다."""
    try:
        m = (service_registry.all_modules() or {}).get(mod) or {}
    except Exception:
        return {}
    out = {}
    if isinstance(m.get('safety'), dict):
        out['safety'] = dict(m['safety'])
    return out


def _normalize_shared_store(raw) -> dict:
    """공유 store 스펙 정규화 — 관리평면 store 가 놓인 **공유 마운트 지점**.

    `{mount_point}` 하나다. 마운트 자체(NFS/CIFS 소스·옵션·fstab 영속)는 서버별 마운트
    관리가 담당하므로 그룹은 **어느 경로를 store 로 쓰는지**만 안다. 양 노드가 이 경로를
    상시 마운트하고, VIP 를 가진 노드만 소유권 리스를 잡아 write 한다 (oam_ha.md §4).

    절대경로여야 하고 `..` 는 불허. 유효하지 않으면 빈 dict(= 공유 store 미사용)."""
    if not isinstance(raw, dict):
        return {}
    mp = str(raw.get('mount_point') or '').strip().rstrip('/')
    if not (mp.startswith('/') and '..' not in mp):
        return {}
    return {'mount_point': mp or '/'}


def _mount_point_unverified(config: dict, group: dict, store: dict) -> dict:
    """공유 store 의 `mount_point` 가 **각 멤버의 실제 마운트**인지 확인.

    자유 입력을 그대로 받으면 마운트가 아닌 하위 경로가 저장되고, OAM 은 mount guard 로
    기동을 거부한다 — 콘솔이 사라져 되돌릴 통로까지 없어진다(실측 사고:
    `/NAS/cims_johnyim/oam_store` 를 지정했으나 실제 마운트는 `/NAS` 하나였다).

    판정 근거는 agent 가 heartbeat 로 보고하는 `mount_targets`(cims-managed 아닌 기존
    마운트 포함)다. 아직 보고가 없는 노드는 판정하지 않는다(신규 노드 차단 방지).

    반환: {'bad': [{agent_id, name, available:[...]}], 'checked': n} — bad 가 비면 정합.
    """
    from handlers.agents import _agent_load
    mp = (store or {}).get('mount_point') or ''
    out = {'bad': [], 'checked': 0}
    if not mp:
        return out
    for m in (group.get('members') or []):
        try:
            ag = _agent_load(config, aid=m.get('agent_id')) or {}
        except Exception:
            continue
        mts = ag.get('mount_targets')
        if not isinstance(mts, list) or not mts:
            continue                       # 보고 없음 — 판정 유보
        out['checked'] += 1
        targets = {str(x.get('target') or '').rstrip('/') for x in mts if isinstance(x, dict)}
        if mp not in targets:
            out['bad'].append({'agent_id': m.get('agent_id'),
                               'name': ag.get('name') or f"agent#{m.get('agent_id')}",
                               'available': sorted(t for t in targets if t)})
    return out


def _group_hosts_oam(config: dict, group: dict, all_deps: list | None = None) -> bool:
    """이 그룹이 **관리평면(oam)** 을 호스팅하는가 — 멤버에 oam 배포가 있는지로 판정."""
    from handlers.agents import _deploy_load_all
    aids = {m.get('agent_id') for m in (group.get('members') or [])}
    for d in (all_deps if all_deps is not None else _deploy_load_all(config)):
        if d.get('agent_id') in aids and d.get('status') != 'removed' \
                and (d.get('process_name') or '').lower().strip() == 'oam':
            return True
    return False


def _agents_not_on_vip(config: dict, group: dict, all_agents: list | None = None,
                       all_deps: list | None = None) -> list:
    """그룹 VIP 가 아닌 주소로 OAM 에 보고하는 agent 목록.

    절체는 VIP 를 옮기는 것이므로, agent 가 **구 Active 의 노드 IP** 를 보고 있으면 절체 후
    그 주소가 죽어 **fleet 전체가 OAM 과 단절**된다(실측: 절체는 성공했는데 콘솔에 전 노드
    offline, 모듈 상태는 절체 직전 값으로 고착). 판정 근거는 agent 가 heartbeat 로 보고하는
    `oam_url` 이다.

    **관리평면(oam)을 호스팅하는 그룹에서만** 의미가 있다. agent 는 OAM 주소 하나만 보므로,
    Signaling·Media 처럼 oam 이 없는 그룹의 VIP 와 비교하면 전원이 "어긋남" 으로 잡혀
    그 그룹의 절체까지 막힌다(실측). 그런 그룹은 절체해도 OAM 주소와 무관하다.

    그룹에 VIP 가 없으면(단일 노드) 대상이 아니다. 보고가 없는 agent(구 버전)는 판정 유보.
    반환: [{agent_id, name, oam_url}] — 비어 있으면 전원 정상.
    """
    from services import ha_lookup
    from handlers.agents import _agent_load_all
    vips = set(ha_lookup.group_vip_set(group) or [])
    if not vips:
        return []
    if not _group_hosts_oam(config, group, all_deps):
        return []
    out = []
    # `all_agents` 를 넘기면 재조회하지 않는다 — 여러 그룹을 직렬화할 때 그룹마다 전 agent 를
    # 다시 읽으면 **O(그룹수 × agent수)** 가 된다(2초 폴링 × NFS 5ms/파일 = 체감 지연).
    # 배포 목록에 이미 같은 실수를 했고 프리페치로 고쳤다 — 같은 규칙을 여기에도 적용한다.
    # 그룹 멤버뿐 아니라 **전 agent** 가 대상이다 — 관리평면 주소는 fleet 공통이다.
    for r in (all_agents if all_agents is not None else _agent_load_all(config)):
        if (r.get('status') or '') == 'revoked':
            continue
        url = (r.get('oam_url') or '').strip()
        if not url:
            continue                       # 보고 없음(구 agent) — 판정 유보
        try:
            from urllib.parse import urlparse as _up
            host = _up(url).hostname or ''
        except Exception:
            host = ''
        if host in vips:
            continue
        # loopback 은 그 노드 자신의 OAM — Active 가 바뀌면 역시 끊긴다
        out.append({'agent_id': r.get('id'), 'name': r.get('name'), 'oam_url': url})
    return out


def _store_path_conflicts(config: dict, group: dict, store: dict) -> list:
    """공유 store 경로와 **실제 배포설정이 어긋난** oam/oam-svc 목록.

    그룹에 공유 store 만 지정하고 배포설정(`CimsRuntimeDir`)은 노드 로컬로 남겨두면,
    oam/oam-svc 는 HA 편입되지만(절체 대상) 데이터는 노드마다 따로 있다 — 절체하면 신
    Active 가 **빈 콘솔**로 뜬다. 정확히 과거 사고 상태이므로 만들 수 없게 막는다.

    반환: [{agent_id, process_name, runtime_dir}] — 비어 있으면 정합.
    """
    from handlers.agents import _deploy_load_all, _pkg_load, _materialize_deploy_config
    mnt = (store or {}).get('mount_point') or ''
    if not mnt:
        return []
    aids = {m.get('agent_id') for m in (group.get('members') or [])}
    bad = []
    for d in _deploy_load_all(config):
        mod = (d.get('process_name') or '').lower().strip()
        if d.get('agent_id') not in aids or d.get('status') == 'removed':
            continue
        if mod not in ('oam', 'oam-svc'):
            continue
        try:
            pkg = _pkg_load(config, d.get('package_id'))
            eff = _materialize_deploy_config(config, pkg, d.get('config')) or {}
        except Exception:
            eff = d.get('config') if isinstance(d.get('config'), dict) else {}
        rt = str(eff.get('CimsRuntimeDir') or '').rstrip('/')
        if not rt or not (rt == mnt or rt.startswith(mnt + '/')):
            bad.append({'agent_id': d.get('agent_id'), 'process_name': mod,
                        'runtime_dir': rt or '(미지정 — 노드 로컬)'})
    return bad


def _lease_precondition_unmet(group: dict, mod: str) -> "str | None":
    """`requires_leader_lease` 선언의 **집행** — 전제 미충족 사유. 충족이면 None.

    `safety.requires_leader_lease` 는 "이 모듈은 단일 writer 자원(관리 store)을 소유하므로
    **그 자원이 노드 간 이동 가능해야** 절체가 성립한다" 는 선언이다. 전제가 없으면 절체는
    '서비스 이관' 이 아니라 **상태 상실**이 된다(관리평면이면 빈 콘솔).

    옛 구현은 이 선언을 저장·전달만 하고 **아무도 검사하지 않았다** — 그래서 공유 store 가
    없는 상태에서도 oam/oam-svc 가 cold 모듈로 편입돼 절체 대상이 됐고, 실제로 절체 후
    관리 데이터가 없는 노드가 Active 가 되는 사고가 났다. 선언과 집행을 여기서 잇는다.

    특정 모듈 이름을 하드코딩하지 않는다 — 같은 선언을 가진 모든 모듈이 같은 보호를 받는다.
    """
    if not _module_spec(group, mod)['safety'].get('requires_leader_lease'):
        return None
    if group.get('mode') != 'active_standby':
        return None                     # 절체가 없는 모드 — 전제 불필요
    if not _normalize_shared_store(group.get('shared_store')):
        return 'no_shared_store'        # 공유 store 미설정 → 상태가 노드에 묶여 있다
    return None


def _module_spec(group: dict, mod: str) -> dict:
    """group.module_specs[mod] 의 실효 명세.

    우선순위: 운영자 설정(group.module_specs) > service descriptor 기본값 > 전역 default.
    (sub-dict 는 키 단위 병합 — 운영자가 safety.class 만 지정해도 descriptor 의 나머지
    안전 속성이 유지된다.)"""
    specs = group.get('module_specs') if isinstance(group.get('module_specs'), dict) else {}
    raw = specs.get(mod) if isinstance(specs.get(mod), dict) else {}
    base = _descriptor_module_spec_defaults(mod)
    merged = dict(base)
    for k, v in (raw or {}).items():
        if isinstance(v, dict) and isinstance(merged.get(k), dict):
            merged[k] = {**merged[k], **v}
        else:
            merged[k] = v
    return _normalize_module_spec(merged)


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


def _migrate_drop_ha_mode(group: dict) -> bool:
    """단일 모델 전환 — 구 record 의 ha_mode/_ha_mode_v2(legacy↔supervisor 모드) 잔재 제거.
    모드 개념 자체가 사라졌으므로 필드가 있으면 1회 제거한다. 변경 시 True."""
    changed = False
    for k in ('ha_mode', '_ha_mode_v2'):
        if k in group:
            group.pop(k, None)
            changed = True
    return changed


def _ensure_group_migrated(group: dict, config: dict) -> bool:
    """구 record 를 신 스키마(service_intent + module_specs)로 승격. 변경 시 True.
    호출부가 True 면 file_store.save 로 영속화한다 (1회성 — 이후 no-op)."""
    changed = False
    if _migrate_service_intent(group, config):
        changed = True
    if _migrate_module_specs(group):
        changed = True
    if _migrate_drop_ha_mode(group):
        changed = True
    # DRBD 시절의 `volume` 키 제거 — 지금 store 스펙은 `shared_store` 다. 남겨두면
    # 콘솔·API 응답에 의미 없는 값이 보여 운영자가 이중화된 줄 오해한다.
    if 'volume' in group:
        group.pop('volume', None)
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
    'csp':     (5060, 'udp'),
    'isp':     (5060, 'udp'),
    'psp':     (5060, 'udp'),
    'csc':     (4421, 'tcp'),
    'cmp':     (9000, 'udp'),
    'imp':     (9000, 'udp'),
    'pmp':     (9000, 'udp'),
    'oam':     (4419, 'tcp'),
    'oam-svc': (4480, 'tcp'),
}
# 동일 그룹에 여러 daemon module 이 deployed 되어 있을 때의 우선순위.
# Control: csp 가 핵심 (SIP signaling) — psp/isp/csc 는 부수.
# Media: cmp 가 핵심 (RTP relay).
# 관리평면(oam/oam-svc)은 **맨 뒤** — 서비스 모듈과 동거하는 그룹에서 대표를 가로채지
# 않게. 모듈별 감시는 service 레벨 대표가 아니라 `module_health` 맵이 담당한다(§3.1).
_HEALTH_MODULE_PRIORITY = ['csp', 'cmp', 'csc', 'psp', 'isp', 'pmp', 'imp', 'oam', 'oam-svc']


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
    # 대표 헬스 모듈 선정에서도 전제 미충족 모듈은 제외한다(제외 모듈은 HA 관리 대상이 아님).
    _allowed_health = {m for m in intent_running if not _lease_precondition_unmet(group, m)}
    h_port, h_proto, h_module = _infer_health_port_proto(agent_id, config, allowed=_allowed_health) if config else (None, None, None)

    failover_options = _normalize_failover_options(group.get('failover_options'))
    # 그룹 옵션의 수동 오버라이드가 최우선 (운영자 명시 > 배포 실효설정 유도 > descriptor 기본).
    fo_health = failover_options.get('health') or {}
    if fo_health.get('port'):
        h_port = fo_health['port']
        h_proto = fo_health.get('proto') or h_proto or 'tcp'
    elif fo_health.get('proto'):
        h_proto = fo_health['proto']

    # 진실 기반 헬스체크 힌트 (ha_service_model.md §6.1) — descriptor 상수 포트 대신
    # "노드의 실제 설정에서 리슨 포트를 유도하라" 는 선언을 agent 로 내려보낸다. agent 가
    # 검사 시점에 노드 로컬 파일을 직접 읽으므로 배포기록↔실파일 드리프트가 나도 HA 는
    # 실제 bind 포트를 본다 (드리프트 자체는 config_out_of_sync 알람이 노출).
    #   config_key      : 스칼라 config.json 단일 키          (csc = Server.Port)
    #   collection_file : 컬렉션 jsonl 의 match 레코드 field  (csp = local_nodes.bind_port)
    # 운영자가 health.port 를 수동 지정하면 힌트를 내리지 않아 오버라이드가 그대로 최우선.
    h_cfg_key = None
    h_coll = None
    if h_module and not fo_health.get('port'):
        # ① service descriptor 의 health 블록 (데이터 선언 — service_registry.module_health_specs)
        _dh = {}
        try:
            _dh = service_registry.module_health_specs(config).get(h_module) or {}
        except Exception:
            _dh = {}
        # ② csc 전환 안전망 — descriptor 가 아직 health 를 안 가진 구(舊) store 대비
        if not _dh and h_module == 'csc':
            _dh = {'config_key': 'Server.Port'}
        # ③ 모듈 운영 명세(그룹×모듈)가 있으면 최우선
        _mh = _module_spec(group, h_module).get('health') or {}
        src = _mh if (_mh.get('config_key') or _mh.get('collection_file')) else _dh
        if src.get('config_key'):
            h_cfg_key = src['config_key']
        if src.get('collection_file'):
            h_coll = {'file': src['collection_file'],
                      'field': src.get('field') or 'bind_port',
                      'match': src.get('match') or {}}

    # armed daemon 모듈 = 이 agent 에 배포된 daemon 모듈 ∩ running 의도.
    daemon_mods = [m for m in (_agent_daemon_modules(agent_id, config) if config else [])
                   if m in intent_running]
    # 선언 집행 — 전제(단일 writer 볼륨) 미충족 모듈은 **HA 관리에서 제외**한다.
    #   제외 = cold/relevant/health 대상 아님 = 절체로 이동하지 않는다. 조용히 빠지면
    #   운영자는 이중화가 되는 줄 아므로 **사유를 ha.json·그룹 응답에 노출**한다.
    ha_excluded: dict = {}
    _kept: list = []
    for _m in daemon_mods:
        _why = _lease_precondition_unmet(group, _m)
        if _why:
            ha_excluded[_m] = _why
        else:
            _kept.append(_m)
    if ha_excluded:
        logger.log_warning(
            f"[ha-render] group#{group.get('id')}({group.get('name')}) agent#{agent_id} — "
            f"전제 미충족으로 HA 편입 제외: {ha_excluded} "
            f"(공유 store 설정 후 자동 편입. 상세: docs/design/features/oam_ha.md §4)")
    daemon_mods = _kept
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

    # ── 모듈별 health 맵 (oam_ha.md §3.1) ────────────────────────────────
    # service 레벨 대표(health_module) 하나만 포트를 갖던 옛 렌더는, 한 그룹에 데몬이
    # 여럿이면 나머지 모듈의 readiness 가 "프로세스 존재" 로 대체돼 **소켓만 살아있는
    # 좀비를 영구히 놓친다**. 관리 모듈 전부에 자기 포트/해석 힌트를 내려보낸다.
    #   우선순위: 운영자 module_specs.health > descriptor health(config_key/collection/
    #   http_path) > descriptor 상수 port/proto. 실제 포트 해석은 agent 가 검사 시점에
    #   노드 로컬 파일로 수행한다(배포기록↔실파일 드리프트에도 실제 bind 포트를 본다).
    module_health: dict = {}
    try:
        _mh_defaults = service_registry.module_health_defaults(config) or _MODULE_HEALTH_DEFAULTS
        _mh_specs = service_registry.module_health_specs(config) or {}
    except Exception:
        _mh_defaults, _mh_specs = _MODULE_HEALTH_DEFAULTS, {}
    for _m in sorted(set(relevant_modules) | set(cold_modules) | ({h_module} if h_module else set())):
        if not _m:
            continue
        _e: dict = {}
        _dp = _mh_defaults.get(_m)
        if _dp:
            _e['port'], _e['proto'] = int(_dp[0]), _dp[1]
        _dh = _mh_specs.get(_m) or {}
        _oh = _module_spec(group, _m).get('health') or {}
        if _dh.get('config_key'):
            _e['config_key'] = _dh['config_key']
        if _dh.get('collection_file'):
            _e['collection'] = {'file': _dh['collection_file'],
                                'field': _dh.get('field') or 'bind_port',
                                'match': _dh.get('match') or {}}
        if _dh.get('http_path'):
            _e['http_path'] = _dh['http_path']
        # 모듈별 **기동 유예** — 이 시간 안의 readiness 실패는 좀비가 아니라 '기동 중'이다.
        # 관리평면은 콜드스타트(인증서 재발급 등)가 20초를 넘겨 3초 상수로는 좀비 오판이
        # 나고, 그 오판이 절체 래치를 걸어 콘솔이 사라졌다(실측 데드락).
        if _dh.get('startup_grace_sec'):
            _e['startup_grace_sec'] = int(_dh['startup_grace_sec'])
        if _oh.get('startup_grace_sec'):
            _e['startup_grace_sec'] = int(_oh['startup_grace_sec'])
        # 운영자 오버라이드가 최우선 — 지정 시 포트 해석 힌트는 무시(명시 포트를 그대로 찔러야 함).
        if _oh.get('port'):
            _e['port'] = int(_oh['port'])
            _e.pop('config_key', None)
            _e.pop('collection', None)
        if _oh.get('proto'):
            _e['proto'] = _oh['proto']
        elif not _e.get('proto'):
            _e['proto'] = 'tcp'
        if _oh.get('config_key'):
            _e['config_key'] = _oh['config_key']
        if _e.get('port') or _e.get('config_key') or _e.get('collection'):
            module_health[_m] = _e

    # **복구 통로**를 제공하는 모듈 — 콘솔을 서빙하는 base `oam`. agent 는 이 모듈만
    # "상대 노드가 실제로 서비스 중일 때" 정지한다(자기보존). cold 규칙을 그대로 적용하면
    # 래치·FAULT 상태에서 어느 노드에서도 콘솔이 뜨지 못해 설정을 고칠 통로가 사라진다
    # (실측 데드락 — oam_ha.md §6.4). 동시 기동 위험은 소유권 리스가 담당한다.
    # `oam-svc` 는 게이트웨이 뒤의 서비스라 그것만 살아도 콘솔이 열리지 않으므로 제외한다.
    console_modules = sorted(
        _m for _m in (set(relevant_modules) | set(cold_modules)) if _m == 'oam')

    # 공유 store — 그룹 스코프(양 노드 동일 경로). agent 는 마운트를 조작하지 않고
    # 승격 전 **마운트·write 가능 여부만 확인**한다(마운트는 fstab 이 영속).
    shared_store = _normalize_shared_store(group.get('shared_store'))

    # running 의도 daemon 모듈이 없고 헬스포트도 없으면 미개시/빈 서버 — vrrp_instance
    # 를 내리지 않는다 (enabled=false → cims-ha 렌더 스킵 + keepalived 정지 유지).
    # 이후 서비스 의도 변경(일괄/서버별 start)이 재렌더를 태워 자동 무장/해제된다.
    ha_enabled = bool(h_port or daemon_mods)
    restart_limit = failover_options.get('restart_limit') or {}
    # 모듈 안전 등급 — 자동 래치 해제 가능 여부(shared_writer/unknown=manual). 콘솔/래치 판정용.
    safety_map = {m: _module_spec(group, m)['safety'] for m in daemon_mods}

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
            if h_cfg_key or h_coll:
                entry['health_module'] = h_module
            if h_cfg_key: entry['health_config_key'] = h_cfg_key
            if h_coll:    entry['health_collection'] = h_coll
            if cold_modules: entry['cold_modules'] = cold_modules
            if relevant_modules: entry['relevant_modules'] = relevant_modules
            if restart_limit: entry['restart_limit'] = restart_limit
            if safety_map: entry['module_safety'] = safety_map
            if module_health: entry['module_health'] = module_health
            if shared_store: entry['shared_store'] = shared_store
            if console_modules: entry['console_modules'] = console_modules
            if ha_excluded: entry['ha_excluded'] = ha_excluded
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
        if h_cfg_key or h_coll:
            entry['health_module'] = h_module
        if h_cfg_key: entry['health_config_key'] = h_cfg_key
        if h_coll:    entry['health_collection'] = h_coll
        if cold_modules: entry['cold_modules'] = cold_modules
        if relevant_modules: entry['relevant_modules'] = relevant_modules
        if restart_limit: entry['restart_limit'] = restart_limit
        if safety_map: entry['module_safety'] = safety_map
        if module_health: entry['module_health'] = module_health
        if shared_store: entry['shared_store'] = shared_store
        if console_modules: entry['console_modules'] = console_modules
        if ha_excluded: entry['ha_excluded'] = ha_excluded
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


# 개시 국면에서 나머지 멤버 update_ha 를 미루는 시간 — 선행 멤버가 arm→VIP 선점→MASTER
# 승격을 마치기까지의 worst case(job 회수 + apply + 승격 ~수초)를 덮는 값. 이 창 동안
# 피어는 미무장이라 초기 개시 중 단일 장애점이지만, 개시 직후 1회뿐이라 수용한다.
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

    # 개시 국면 선착 방지 — AS 그룹이 무장 렌더되는데 아직 아무도 VIP 를 보유하지
    # 않았다면(최초 개시·전면 재기동), **기준 멤버에게 먼저** update_ha 를 내리고 나머지는
    # not_before 로 지연시킨다. 동시에 뿌리면 양쪽 keepalived 가 함께 콜드스타트해 우선순위
    # 높은 노드(놀던 standby 여도)가 선착하는데, 기준 멤버 먼저 arm → VIP 선점 → nopreempt
    # 로 유지되어 "운영자가 start 누른 노드가 Active" 가 보장된다. 기준 멤버 =
    # prefer_first(호출부 명시 — 서버별/개별 start 시 그 노드) > record running 멤버 >
    # 지정 마스터. VIP 보유자가 이미 있으면(운영 중 재렌더) 지연 없음(apply 멱등).
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
            f"[ha-group] group#{group_id} 개시 국면 — 선행 멤버 {sorted(stagger_first)} 먼저, "
            f"나머지 update_ha {_STAGGER_DELAY_SEC}s 지연 (선착 노드 Active 보장)")

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


def enqueue_module_spec_for_agent(agent_id: int, config: dict, module: str | None = None) -> int:
    """이 agent 에 배포된 daemon 모듈의 운영 명세(service.json)를 push — 소속 그룹 기준.

    **신규 설치 시딩**: 옛 동작은 명세 변경(_update_group) 시에만 push 해서, 갓 설치된
    모듈은 service.json 이 없는 상태로 남았다(agent 는 watchdog=on default 로 동작 —
    운영자가 감시 off 로 저장해둔 그룹에서도 새 노드만 on 으로 도는 비대칭). 배포 생성·
    install 완료 시 이 함수로 해당 모듈 명세를 즉시 내려보낸다.
    module 지정 시 그 모듈만, 없으면 이 agent 의 daemon 모듈 전부. 큐잉된 job 수 반환."""
    groups = _ha_load_all(config)
    group = next((g for g in groups
                  if any(m.get('agent_id') == agent_id for m in (g.get('members') or []))), None)
    if not group:
        return 0        # HA 그룹 미소속 — 명세는 그룹×모듈 스코프라 내려보낼 근거가 없다
    from handlers.agents import _job_create
    mods = [module.lower().strip()] if module else _agent_daemon_modules(agent_id, config)
    enqueued = 0
    for mod in mods:
        if not mod:
            continue
        _job_create(config, agent_id, 'update_module_spec',
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

        # 관리 store 를 공유 마운트로 이관 (AS 전용) — admin. 콘솔에서 원클릭.
        if sub == 'shared-store' and member == 'migrate' and method == 'POST':
            return await _migrate_shared_store(gid, handler_args.body, config)

        # 노드 유지보수(EXCLUDE_NODE) 토글 (AS 전용) — admin.
        if sub == 'maintenance' and method == 'POST':
            return await _maintenance_group(gid, handler_args.body, config)

        if sub == 'collections':
            if not member:
                return HandlerResult(status=400, body={'error': 'collection name required'})
            if method == 'GET':
                return await _get_group_collection(gid, member, handler_args, config)
            if method == 'PUT':
                return await _put_group_collection(gid, member, handler_args, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        # 그룹×패키지 공통 설정 (R4) — /ha-groups/{gid}/packages/{pkg}/config|auto-sync|sync
        if sub == 'packages':
            if not member:
                return HandlerResult(status=400, body={'error': 'package name required'})
            action = parts[3] if len(parts) > 3 else None
            if action == 'sync' and method == 'GET':
                return await _get_group_pkg_sync(gid, member, config)
            if action == 'config' and method == 'PUT':
                return await _put_group_pkg_config(gid, member, handler_args, config)
            if action == 'auto-sync' and method == 'PUT':
                return await _put_group_auto_sync(gid, member, handler_args, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        return HandlerResult(status=404, body={'error': 'Not Found'})
    except LeaseLostError as e:
        # 관리 store 소유권 없음 → read-only (oam_ha.md §4.4). 조회는 여기 오지 않는다.
        return HandlerResult(status=409, body={'error': 'not_lease_owner', 'detail': str(e)})
    except Exception as e:
        # store 는 file_store(파일)다 — DB 예외 타입으로 잡으면 이 핸들러의 모든 오류가
        # NameError 로 뒤바뀌어 실제 사유가 사라진다(과거 DB 시절 잔재).
        logger.log_error(f"[ha-group] 처리 실패 {method} {handler_args.full_path}: {e}")
        return HandlerResult(status=500, body={'error': 'internal_error', 'detail': str(e)})


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


def _serialize_group(g: dict, config: dict,
                     all_deps: list | None = None,
                     health_defaults: dict | None = None,
                     all_agents: list | None = None) -> dict:
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
    # 선언 집행 결과 노출 — `requires_leader_lease` 전제 미충족으로 **HA 편입에서 제외된**
    # 모듈과 사유. 조용히 빠지면 운영자는 이중화가 되는 줄 안다(실측 사고). 그룹에 배포된
    # daemon 모듈 전체를 대상으로 계산한다(멤버별 렌더와 동일 기준).
    try:
        _excl = {}
        for _m in sorted({(d.get('process_name') or '').lower().strip()
                          for d in _group_member_daemon_deps(out, config,
                                                              all_deps, health_defaults)
                          if d.get('process_name')}):
            _why = _lease_precondition_unmet(out, _m)
            if _why:
                _excl[_m] = _why
        out['ha_excluded'] = _excl
        # agent 주소가 VIP 가 아니면 절체 후 fleet 이 단절된다 — 조용히 두면 정상인 줄 안다.
        out['agents_not_on_vip'] = _agents_not_on_vip(config, out, all_agents, all_deps)
    except Exception as e:
        logger.log_warning(f"[ha-group] group#{out.get('id')} ha_excluded 계산 skip: {e}")
        out['ha_excluded'] = {}
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
        # 진행 중/최근 계획 절체 operation (콘솔 진행표시). 없으면 None.
        try:
            op = _op_active_for_group(config, g.get('id'))
            if op:
                out['failover_op'] = {k: op.get(k) for k in
                                      ('id', 'state', 'source_agent_id', 'target_agent_id',
                                       'note', 'error', 'updated_at')}
        except Exception:
            pass
    return out


def _build_group_list(config) -> list:
    """목록 직렬화 — 배포·health defaults 를 **한 번만** 읽어 전 그룹이 공유한다."""
    from handlers.agents import _deploy_load_all, _agent_load_all
    groups = _ha_load_all(config)
    groups.sort(key=lambda g: g.get('id', 0))
    deps = _deploy_load_all(config)
    agents = _agent_load_all(config)
    defaults = service_registry.module_health_defaults(config) or _MODULE_HEALTH_DEFAULTS
    return [_serialize_group(g, config, deps, defaults, agents) for g in groups]


async def _migrate_shared_store(gid: int, body_raw, config: dict) -> HandlerResult:
    """POST /ha-groups/{id}/shared-store/migrate — 관리 store 를 공유 마운트로 이관.

    body: { "mount_point": "/NAS/.../oam_store" }

    콘솔 한 번의 조작으로 끝나야 하는 작업이다. 운영자가 SSH 로 나눠 하면 순서를 틀리기
    쉽고(설정을 먼저 바꾸면 빈 콘솔, 프로세스를 SSH 로 죽이면 watchdog 이 되살림),
    무엇보다 OAM 은 **자기 store 를 자기가 옮길 수 없다**. 그래서:

      1. 그룹에 `shared_store` 저장 (이 시점부터 oam/oam-svc 가 HA 편입 대상)
      2. 그룹 멤버의 oam/oam-svc 배포 overlay 에 `CimsRuntimeDir`/`CimsRuntimeMount` 병합
         → **현재 store 에 기록**되므로 3단계 복사에 함께 실려 간다(신 store 와 일관)
      3. store 를 들고 있는 노드(현재 oam 이 running 인 노드)에 `migrate_oam_store` job
         → agent 가 정지 → 복사 → config.json 기록 → 기동 을 수행
      4. 나머지 멤버는 `update_config` 만 (그 노드는 같은 공유 store 를 읽게 된다)

    응답은 202 다 — 3단계에서 OAM 이 재기동되므로 **콘솔이 잠깐 끊긴다**(정상).
    """
    from handlers.agents import (_deploy_load_all, _deploy_update, _pkg_load, _job_create,
                                 _materialize_deploy_config, _split_csv, _enrich_deploy)
    body = body_raw if isinstance(body_raw, dict) else (json.loads(body_raw or '{}') or {})
    store = _normalize_shared_store(body)
    if not store:
        return HandlerResult(status=400, body={
            'error': 'invalid_mount_point',
            'detail': 'mount_point 는 절대경로여야 합니다 (.. 불가).'})
    mnt = store['mount_point']
    target_dir = f"{mnt}/runtime"

    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if g.get('mode') != 'active_standby':
        return HandlerResult(status=400, body={
            'error': 'not_active_standby',
            'detail': '공유 store 는 Active/Standby 그룹에서만 의미가 있습니다.'})
    _mv = _mount_point_unverified(config, g, store)
    if _mv['bad']:
        _lines = '; '.join(f"{b['name']}: 실제 마운트 {b['available'] or '(없음)'}"
                           for b in _mv['bad'])
        return HandlerResult(status=400, body={
            'error': 'not_a_mount_point',
            'detail': (f"'{mnt}' 는 마운트 지점이 아닙니다 — mount guard 가 기동을 거부합니다. "
                       f"{_lines}. 마운트 지점을 고르고 store 는 그 하위로 두세요."),
            'nodes': _mv['bad']})

    aids = {m.get('agent_id') for m in (g.get('members') or [])}
    targets = [d for d in _deploy_load_all(config)
               if d.get('agent_id') in aids
               and (d.get('process_name') or '').lower().strip() in ('oam', 'oam-svc')
               and d.get('status') != 'removed']
    if not targets:
        return HandlerResult(status=400, body={
            'error': 'no_oam_deployment',
            'detail': '이 그룹 멤버에 oam/oam-svc 배포가 없습니다. 먼저 설치하세요.'})

    # ── 1) 그룹에 공유 store 저장
    g['shared_store'] = store
    file_store.save(_ha_dir(config), gid, g)
    logger.log_info(f"[ha-group] group#{gid} 공유 store 설정 → {mnt}")

    # 이관(복사) 대상 1건 선정 — 이 OAM 이 도는 노드의 oam 배포.
    import socket as _sock
    _host = (_sock.gethostname() or '').split('.')[0].lower()
    from handlers.agents import _agent_load
    _mig_target_id = None
    _cands = [d for d in targets if (d.get('process_name') or '').lower() == 'oam'] or targets
    for d in _cands:                                   # ① hostname 일치 (가장 정확)
        try:
            ag = _agent_load(config, aid=d.get('agent_id')) or {}
        except Exception:
            ag = {}
        for nm in (ag.get('hostname'), ag.get('name')):
            if nm and str(nm).split('.')[0].lower() == _host:
                _mig_target_id = d['id']
                break
        if _mig_target_id:
            break
    if _mig_target_id is None:                         # ② 도는 노드
        for d in _cands:
            if (d.get('status') or '') == 'running':
                _mig_target_id = d['id']
                break
    if _mig_target_id is None:                         # ③ 최후 — 첫 배포
        _mig_target_id = _cands[0]['id']
        logger.log_warning(f"[ha-group] group#{gid} 이관 대상 노드를 특정하지 못해 "
                           f"deployment#{_mig_target_id} 로 진행합니다(복사 누락 방지)")

    # ── 2)~4) 배포별 overlay 병합 + job 큐잉
    jobs: list = []
    for dep in targets:
        cur = dep.get('config') if isinstance(dep.get('config'), dict) else {}
        overlay = dict(cur)
        overlay['CimsRuntimeDir'] = target_dir
        overlay['CimsRuntimeMount'] = mnt
        updated = _deploy_update(config, dep['id'], {'config': overlay}) or dep
        _enrich_deploy([updated], config)
        pkg = _pkg_load(config, updated.get('package_id'))
        sf = updated.get('service_functions')
        if isinstance(sf, str):
            sf = _split_csv(sf)
        params = {
            'deployment_id': updated['id'],
            'package_id': updated.get('package_id'),
            'package_name': updated.get('package_name'),
            'package_version': updated.get('package_version'),
            'process_name': updated.get('process_name'),
            'service_functions': sf or [],
            'install_path': updated.get('install_path'),
            'config': _materialize_deploy_config(config, pkg, updated.get('config')),
        }
        # store 를 실제로 들고 있는 노드 = **이 OAM 이 도는 노드**. 그 노드만 복사가 필요하다.
        # 판정은 hostname 우선(정확), 없으면 status=running (배포기록 기준). 둘 다 못 찾으면
        # 아래에서 첫 배포를 대상으로 삼는다 — 아무도 대상이 아니면 복사가 조용히 빠져
        # 절체 시 빈 콘솔이 되므로, 대상 0건은 허용하지 않는다.
        if updated['id'] == _mig_target_id:
            params['module'] = (updated.get('process_name') or '').lower().strip()
            params['source_dir'] = file_store.runtime_root(config)
            params['target_dir'] = target_dir
            params['target_mount'] = mnt
            jt = 'migrate_oam_store'
        else:
            jt = 'update_config'
        jid = _job_create(config, updated['agent_id'], jt, params)
        jobs.append({'deployment_id': updated['id'], 'agent_id': updated.get('agent_id'),
                     'process_name': updated.get('process_name'), 'job_type': jt,
                     'job_id': jid})
        logger.log_info(f"[ha-group] group#{gid} {updated.get('process_name')} "
                        f"agent#{updated.get('agent_id')} {jt} job#{jid} 큐잉")

    _enqueue_update_ha_for_members(gid, config)   # 공유 store 반영 → HA 편입 재렌더
    return HandlerResult(status=202, body={
        'shared_store': store, 'runtime_dir': target_dir, 'jobs': jobs,
        'detail': ('이관을 시작했습니다. store 를 들고 있는 노드의 OAM 이 정지 → 복사 → '
                   '재기동되므로 콘솔이 30초 내외 끊깁니다. 돌아오면 새 경로로 동작합니다.'),
    })


async def _list_groups(config):
    # 직렬화는 파일 store 를 여러 번 읽으므로 **워커 스레드**에서 수행한다 — 이벤트 루프에서
    # 돌면 그동안 heartbeat·job 결과 POST 가 대기해 배포 상태 전이가 늦어진다.
    groups = await asyncio.to_thread(_build_group_list, config)
    return HandlerResult(status=200, body={'groups': groups})


async def _get_group(gid: int, config):
    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    return HandlerResult(status=200,
                         body=await asyncio.to_thread(_serialize_group, g, config))


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
        'shared_store': _normalize_shared_store(body.get('shared_store')),
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
    if 'shared_store' in body:
        # 공유 store — 그룹 스코프. 잘못된 형식은 미사용({})으로 정규화되므로 저장 후
        # 렌더가 store 단계를 건너뛴다(조용한 부분 적용 방지 = 값이 응답에 그대로 보임).
        _new_store = _normalize_shared_store(body.get('shared_store'))
        # 경로만 저장하고 실제 데이터를 옮기지 않으면 **절체 시 빈 콘솔**이 된다
        # (HA 편입은 되는데 store 는 노드별 로컬). 그 상태를 만들 수 없게 막고
        # 이관 경로(POST .../shared-store/migrate)로 안내한다 — oam_ha.md §9.4.
        if _new_store:
            _mv = _mount_point_unverified(config, existing, _new_store)
            if _mv['bad']:
                _lines = '; '.join(
                    f"{b['name']}: 실제 마운트 {b['available'] or '(없음)'}" for b in _mv['bad'])
                return HandlerResult(status=400, body={
                    'error': 'not_a_mount_point',
                    'detail': (f"'{_new_store['mount_point']}' 는 마운트 지점이 아닙니다. "
                               f"mount guard 는 /proc/mounts 와 **정확히 일치**하는 경로만 "
                               f"통과시킵니다(하위 디렉터리는 불가) — 지금 저장하면 OAM 이 "
                               f"기동을 거부합니다. {_lines}. 마운트 지점을 고르고, store 위치는 "
                               f"그 하위 경로로 지정하세요."),
                    'nodes': _mv['bad']})
            _bad = _store_path_conflicts(config, existing, _new_store)
            if _bad:
                _who = ', '.join(f"agent#{b['agent_id']} {b['process_name']}"
                                 f"({b['runtime_dir']})" for b in _bad)
                return HandlerResult(status=409, body={
                    'error': 'store_path_not_shared',
                    'detail': (f"공유 store 경로만 저장하면 이 모듈들의 관리 데이터가 아직 "
                               f"노드 로컬에 있어, 절체 시 빈 콘솔이 됩니다: {_who}. "
                               f"'이 경로로 이관' 을 사용하세요 — 경로 저장·배포설정 갱신·"
                               f"데이터 복사·재기동을 한 번에 처리합니다."),
                    'conflicts': _bad})
        existing['shared_store'] = _new_store
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

def _group_member_daemon_deps(group: dict, config: dict,
                              all_deps: list | None = None,
                              defaults: dict | None = None) -> list:
    """그룹 멤버들의 daemon 배포(status != removed) 목록 — 일괄 제어 대상.
    (process_name 이 health defaults 에 있는 리슨 데몬만; cspsim/console 등 제외.)

    `all_deps`/`defaults` 를 넘기면 재조회하지 않는다 — 여러 그룹을 한 번에 직렬화할 때
    그룹마다 전체 배포 목록을 다시 읽으면 **O(그룹수 × 배포수)** 파일 I/O 가 된다
    (조회 지연이 heartbeat·job 결과 처리까지 밀어 배포가 deploying 에 머문 실측 사고)."""
    from handlers.agents import _deploy_load_all
    if defaults is None:
        defaults = service_registry.module_health_defaults(config) or _MODULE_HEALTH_DEFAULTS
    aids = {m.get('agent_id') for m in (group.get('members') or [])}
    out = []
    for d in (all_deps if all_deps is not None else _deploy_load_all(config)):
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
        # cold 모듈(AS): **지정 마스터에게만** start job 을 보낸다 — 마스터는 deploying→running
        # 표시·desired 해제·실제 기동을 한다. 백업엔 start 대신 홀드 해제(ha_clear_holds)만
        # 보내 dual-active 를 막고 백업의 stale desired/latch 를 정리한다(향후 절체 대비).
        # hot(AS)·AA 모듈은 양쪽 상시라 모두 start. 마스터 사망 시엔 절체로 백업(신 마스터)
        # reconcile 이 기동.
        from handlers.agents import _job_create as _jc
        is_as = g.get('mode') == 'active_standby'
        n_job = 0
        for d in deps:
            mod = (d.get('process_name') or '').lower().strip()
            is_cold = is_as and _module_spec(g, mod)['ha']['failover_mode'] != 'hot'
            if is_cold and d.get('agent_id') != base:
                continue          # cold on 백업 — start 안 보냄(아래에서 홀드만 해제)
            await asyncio.to_thread(_queue_lifecycle_job, config, d, 'start')
            n_job += 1
        # 백업들의 홀드 정리 (cold 서비스만, 멤버 중 마스터 아닌 노드)
        n_clr = 0
        if is_as and any(_module_spec(g, m)['ha']['failover_mode'] != 'hot' for m in daemon_mods):
            for m in (g.get('members') or []):
                aid = m.get('agent_id')
                if aid is None or aid == base:
                    continue
                await asyncio.to_thread(_jc, config, aid, 'ha_clear_holds', {'service': g.get('name')})
                n_clr += 1
        logger.log_info(f"[ha-group] group#{gid} 일괄 시작 — 의도 running {daemon_mods}, "
                        f"update_ha {n_ha}, start job {n_job}(cold 은 마스터#{base}만), "
                        f"백업 clear_holds {n_clr}")
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


async def _maintenance_group(gid: int, body, config):
    """노드 유지보수(EXCLUDE_NODE) 토글 — body = {"agent_id": int, "on": bool}.

    지정 멤버 노드를 이 그룹 서비스의 승격 대상에서 제외(on)/복귀(off)시킨다. agent 가
    state/ha/maintenance/<svc> 마커로 반영 → Evaluator eligible=false(MAINTENANCE) +
    reconcile 모듈 정지. off → 마커 제거 → role 기반 자동 재합류. 상세: ha_service_model.md §16."""
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if g.get('mode') != 'active_standby':
        return HandlerResult(status=409, body={'error': 'not_active_standby',
            'hint': '유지보수(EXCLUDE_NODE)는 Active/Standby 그룹만'})
    try:
        aid = int(body.get('agent_id'))
    except (TypeError, ValueError):
        return HandlerResult(status=400, body={'error': 'agent_id (int) required'})
    on = bool(body.get('on'))
    if not any(m.get('agent_id') == aid for m in (g.get('members') or [])):
        return HandlerResult(status=404, body={'error': 'agent not a group member'})
    from handlers.agents import _job_create
    svc = g.get('name')
    _job_create(config, aid, 'ha_maintenance', {'service': svc, 'on': on})
    logger.log_info(f"[ha-group] group#{gid} 유지보수 {'set' if on else 'clear'} — "
                    f"agent#{aid} svc={svc} (EXCLUDE_NODE)")
    return HandlerResult(status=202, body={'group_id': gid, 'agent_id': aid,
                                           'service': svc, 'maintenance': on})


# ── 계획 절체(스위치오버) v2 — OAM operation 상태머신 (ha_service_model.md §12) ──
# POST /failover 는 operation 을 생성하고 즉시 202 반환. 실제 절체는 sweep 루프
# (_sweep_ha_operations)가 RELEASING→WAIT_VIP_MOVE→VERIFYING→COMMITTED / ROLLING_BACK
# / FAILED 로 구동한다. 영속 record 라 OAM 재시작에도 이어서 처리(resume). keepalived
# 프로세스를 직접 stop/start 하지 않고 agent 의 planned_release(verdict eligible=false)로
# VIP 를 반납시킨다 — 실제 role/VIP 이동을 관측하며 진행(고정 sleep 없음).
_HA_OP_DOMAIN = 'ha_operations'
_OP_RELEASE_TIMEOUT = 30      # VIP 가 target 으로 이동하기까지 최대 대기(초)
_OP_VERIFY_SEC = 15          # target 이 VIP 를 안정 보유해야 하는 검증 창(초)
# 관측 불가(판정 None) 유예 — 이 창 안에서는 타임아웃을 세지 않는다.
#   관리평면 그룹의 절체는 **source 가 오케스트레이터 자신**이다: release 후 그 노드의 OAM 이
#   정지되고 신 Active 의 OAM 이 이 operation 을 이어받는다(공유 store). 그 직후에는 heartbeat
#   수집 창 때문에 vip_observation 이 판정 불가(None)를 낼 수 있는데, 옛 구현은 그것을 그냥
#   타임아웃으로 세어 **이미 정상 완료된 절체를 ROLLED_BACK 으로 오기록**했다.
#   원칙: "target 이 VIP 를 못 잡았다" 는 **확정 관측**이 있을 때만 롤백 사유다. 관측 자체가
#   불가하면 기다리고, 이 창을 넘기면 롤백이 아니라 관측 실패(FAILED)로 종결한다.
_OP_OBSERVE_GRACE = 180


def _op_dir(config):
    return file_store.domain_dir(config, _HA_OP_DOMAIN)


def _op_active_for_group(config, gid: int):
    """그룹의 진행 중(비종결) operation — 중복 절체 방지."""
    _TERMINAL = {'COMMITTED', 'ROLLED_BACK', 'FAILED'}
    for op in file_store.load_all(_op_dir(config)):
        if op.get('group_id') == gid and op.get('state') not in _TERMINAL:
            return op
    return None


async def _failover_group(gid: int, body, config):
    """수동 계획 절체 — AS 전용. operation 을 생성하고 sweep 루프가 구동한다."""
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
    if _op_active_for_group(config, gid):
        return HandlerResult(status=409, body={'error': 'failover_in_progress',
            'hint': '이미 진행 중인 절체 operation 이 있습니다'})
    # 사전 점검 — agent 가 구 Active 노드 IP 를 보고 있으면 절체 후 전 fleet 이 단절된다.
    # 절체 자체는 성공하는데 콘솔에 전 노드 offline 으로 보이는 상태가 되므로 먼저 막는다.
    _body = body if isinstance(body, dict) else (json.loads(body or '{}') or {})
    if not _body.get('force'):
        _stray = _agents_not_on_vip(config, g)
        if _stray:
            return HandlerResult(status=409, body={
                'error': 'agents_not_on_vip',
                'detail': (f"{len(_stray)}개 agent 가 VIP 가 아닌 주소로 OAM 에 보고하고 "
                           f"있습니다. 이대로 절체하면 그 agent 들은 구 Active 주소가 죽어 "
                           f"OAM 과 단절되고, 콘솔에는 전 노드 offline·모듈 상태 고착으로 "
                           f"보입니다. 먼저 'OAM 주소 VIP 전환' 을 실행하세요."),
                'agents': _stray})

    from services import ha_lookup
    from handlers.agents import _agent_load
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
    ta = _agent_load(config, aid=target_aid) or {}
    if ta.get('status') != 'online':
        return HandlerResult(status=409, body={'error': 'target_offline',
            'hint': f'standby agent#{target_aid} 오프라인 — 절체 불가'})

    from datetime import datetime
    now_iso = datetime.now().isoformat(timespec='seconds')
    oid = file_store.next_id(_op_dir(config))
    # 관리평면 자기 절체 여부 — source 가 오케스트레이터(OAM) 자신인 그룹. 표시·로그용이며
    # 관측 유예는 모든 op 에 공통 적용된다(관측 불가 ≠ 롤백 사유).
    _self_orch = 'oam' in {str(m).lower() for m in (g.get('service_intent') or {})}
    op = {
        'id': oid, 'group_id': gid, 'service': g.get('name'),
        'source_agent_id': active_aid, 'target_agent_id': target_aid,
        'state': 'RELEASING', 'release_sent': False, 'clear_sent': False,
        'self_orchestrated': _self_orch,
        'created_at': now_iso, 'updated_at': now_iso, 'note': None, 'error': None,
    }
    file_store.save(_op_dir(config), oid, op)
    logger.log_info(f"[ha-op] group#{gid} 계획 절체 operation#{oid} 생성 "
                    f"— source#{active_aid} → target#{target_aid}")
    # Fix3 — 타겟의 절체 홀드(desired=stopped·latch·planned_release) 선해제. 타겟에 이전
    # stop/홀드가 고착돼 있으면 승격돼도 reconcile 이 모듈을 못 켜므로(이슈5), 절체 시작
    # 시점에 타겟에서 지운다. (RELEASE 로 VIP 가 넘어가기 전에 도착하도록 먼저 큐잉.)
    try:
        from handlers.agents import _job_create
        _job_create(config, target_aid, 'ha_clear_holds', {'service': g.get('name')})
    except Exception as e:
        logger.log_warning(f"[ha-op] operation#{oid} target#{target_aid} clear_holds 큐잉 실패: {e}")
    # 즉시 첫 스텝 구동(202 응답 전 release job 큐잉 — 이후는 sweep 이 이어감).
    try:
        _advance_ha_operation(config, op)
    except Exception as e:
        logger.log_warning(f"[ha-op] operation#{oid} 초기 구동 실패(sweep 이 재시도): {e}")
    return HandlerResult(status=202, body={'group_id': gid, 'operation_id': oid,
                                           'from_agent_id': active_aid, 'to_agent_id': target_aid,
                                           'state': op['state']})


def _op_planned_release(config, agent_id: int, service: str, release: bool):
    from handlers.agents import _job_create
    _job_create(config, agent_id, 'ha_planned_release',
                {'service': service, 'release': bool(release)})


def _advance_ha_operation(config, op: dict) -> bool:
    """operation 을 현재 상태 + VIP 관측에 따라 한 스텝 전진. 변경 시 True(저장은 caller).
    sweep 루프와 생성 시점 양쪽에서 호출된다(멱등적 스텝)."""
    from datetime import datetime
    from services import ha_lookup
    gid = op.get('group_id')
    g = _ha_load(config, gid)
    if not g:
        op['state'] = 'FAILED'; op['error'] = 'group_deleted'; return True
    svc = op.get('service') or g.get('name')
    src, tgt = op.get('source_agent_id'), op.get('target_agent_id')
    now = datetime.now()
    def _age(field):
        try:
            return (now - datetime.fromisoformat(op.get(field))).total_seconds()
        except Exception:
            return 0.0
    obs = ha_lookup.vip_observation(config, g) or {}
    active = obs.get('active_agent_id')
    state = op.get('state')

    if state == 'RELEASING':
        if not op.get('release_sent'):
            _op_planned_release(config, src, svc, True)      # source verdict eligible=false
            op['release_sent'] = True
            op['note'] = 'planned_release 전송 — VIP 이동 대기'
        op['state'] = 'WAIT_VIP_MOVE'
        op['release_at'] = now.isoformat(timespec='seconds')
        return True

    def _clear_source_once():
        """source planned_release 해제 (멱등). **종결 전이 시점에 인라인**으로 호출한다 —
        '다음 호출이 처리'에 의존하면 안 된다. COMMITTED/FAILED 는 terminal 이라 sweep 이
        skip(_advance 재호출 안 함) → 그 방식이면 COMMIT 절체가 source 마커를 영영 안 지워
        source 노드가 영구 부적격이 된다(역방향 절체 불가)."""
        if not op.get('clear_sent'):
            _op_planned_release(config, src, svc, False)
            op['clear_sent'] = True

    if state == 'WAIT_VIP_MOVE':
        if active == tgt:
            op['state'] = 'VERIFYING'
            op['verify_since'] = now.isoformat(timespec='seconds')
            op['note'] = 'target VIP 인수 — 안정 검증 중'
            return True
        age = _age('release_at')
        if active is None:
            # 관측 불가 — 판정 유예. (신 Active OAM 이 막 뜬 직후, 전원 heartbeat stale 등)
            if age > _OP_OBSERVE_GRACE:
                _clear_source_once()
                op['state'] = 'FAILED'
                op['error'] = 'observation_unavailable'
                op['note'] = (f'VIP 보유 판정을 {int(age)}초간 확정할 수 없었다 — 롤백이 아니라 '
                              f'관측 실패로 종결. 실제 VIP 위치를 확인하라(콘솔 멤버 상태).')
                return True
            if not op.get('obs_wait_logged'):
                op['obs_wait_logged'] = True
                op['note'] = 'VIP 보유 판정 대기 (관측 불가 — 유예 중)'
                return True
            return False
        if age > _OP_RELEASE_TIMEOUT:
            # **확정 관측**으로 target 이 아닌 노드가 VIP 를 갖고 있다 → 롤백.
            _clear_source_once()
            op['state'] = 'ROLLED_BACK'
            op['error'] = 'target_not_promoted'
            return True
        return False

    if state == 'VERIFYING':
        if active is None:
            # 관측 불가는 실패가 아니다 — 유예 안에서는 기다린다(관리평면 self-절체 직후 등).
            if _age('verify_since') > _OP_OBSERVE_GRACE:
                _clear_source_once()
                op['state'] = 'FAILED'
                op['error'] = 'observation_unavailable'
                return True
            return False
        if active != tgt:
            # 검증 중 target 이 VIP 를 놓침 → 실패. source 해제(재인수 or nopreempt 유지).
            _clear_source_once()
            op['state'] = 'FAILED'
            op['error'] = 'target_unstable'
            return True
        if _age('verify_since') >= _OP_VERIFY_SEC:
            # switchover 성공 — **COMMIT 도 반드시 source 해제**(롤백과 대칭). 이전엔 여기서
            # 안 지워 source 가 영구 planned_release 로 남던 버그.
            _clear_source_once()
            op['state'] = 'COMMITTED'
            op['note'] = 'switchover 완료'
            return True
        return False

    # 구 record(ROLLING_BACK) resume 안전망 — 신 코드는 전이 시점에 이미 해제한다.
    if state == 'ROLLING_BACK':
        _clear_source_once()
        op['state'] = 'ROLLED_BACK'
        return True
    return False


# 종결 operation 을 이 시간(초) 뒤 정리 — 콘솔에서 결과 확인할 여유.
_OP_RETENTION_SEC = 600


def sweep_ha_operations(config) -> int:
    """진행 중 계획 절체 operation 을 한 스텝씩 전진(OAM sweep 루프가 주기 호출).
    OAM 재시작 후에도 영속 record 를 읽어 이어서 처리한다. 처리 건수 반환."""
    from datetime import datetime
    _TERMINAL = {'COMMITTED', 'ROLLED_BACK', 'FAILED'}
    n = 0
    for op in file_store.load_all(_op_dir(config)):
        state = op.get('state')
        if state in _TERMINAL:
            # 오래된 종결 record 정리
            try:
                age = (datetime.now() - datetime.fromisoformat(op.get('updated_at'))).total_seconds()
                if age > _OP_RETENTION_SEC:
                    file_store.delete(_op_dir(config), op.get('id'))
            except Exception:
                pass
            continue
        try:
            changed = _advance_ha_operation(config, op)
        except Exception as e:
            op['state'] = 'FAILED'; op['error'] = f'sweep_exc: {e}'; changed = True
        if changed:
            op['updated_at'] = datetime.now().isoformat(timespec='seconds')
            file_store.save(_op_dir(config), op.get('id'), op)
            logger.log_info(f"[ha-op] operation#{op.get('id')} → {op.get('state')}"
                            + (f" ({op.get('error')})" if op.get('error') else ""))
            n += 1
    return n


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

async def _get_group_pkg_sync(gid: int, pkg_name: str, config):
    """그룹×패키지 공통 설정 정합 상태 조회 (읽기 전용) — 콘솔 드리프트 표시의 정본.

    판정은 서버가 소유한다. 콘솔이 멤버별 설정을 받아 브라우저에서 직접 비교하면
    자동 교정 데몬과 판정 주체가 둘로 갈라져, 데몬이 손대지 않을 것을 "교정 대기"로
    표시하거나 그 반대가 생긴다. 여기서 내려주는 status/drift 만 그리면 된다.
    응답 스키마는 handlers.agents.evaluate_group_package 참조."""
    from handlers.agents import evaluate_group_package
    g = _ha_load(config, gid)
    if not g:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    r = await asyncio.to_thread(evaluate_group_package, config, g, pkg_name)
    return HandlerResult(status=200, body=r)


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
