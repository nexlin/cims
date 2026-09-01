"""
Service Descriptor 레지스트리 — OAM 플랫폼화 5-4.

OAM 코어의 CIMS 하드코딩(ha_groups 모듈맵 / build 화이트리스트 / service_control 허용목록)을
데이터(서비스 descriptor)로 분리. descriptor 는 file_store 'services' 도메인에 저장.

descriptor 스키마:
  { "id": "cims", "label": "CIMS",
    "modules": [ { "name": "csp", "port": 5060, "proto": "udp", "controllable": true }, ... ] }

코어(서비스 무지) 모듈(agent/oam/console)은 어떤 descriptor 와도 무관하게 항상 유효.
store 가 비어있으면 _CIMS_SEED 를 1회 주입 → 기존 하드코딩과 동일 동작 보장(전환 seed).
(향후 CIMS service pack(csc) 으로 seed 추출 예정.)

build.py 의 일부 함수는 config 를 인자로 받지 않으므로, init(config) 로 startup config 를
캐시해 두고 함수 호출 시 config 생략 가능 (소비처가 config 가지면 전달, 아니면 캐시 사용).
"""
import os
import glob
import json

from services import file_store

_DOMAIN = 'services'

# 코어(서비스 무지) — OAM 플랫폼 자체 패키지. 항상 유효한 모듈.
_CORE_MODULES = {'agent', 'oam', 'console'}
_CORE_CONTROLLABLE = {'console'}

# 코어(서비스 무지) host 관측 alert 규칙 — 어떤 서비스 descriptor 와도 무관하게 항상 평가.
# 알람 표준화(X.733/32.111, docs/design/alarm_standardization.md): type=조건클래스, 객체는 source.
# scope='agent' → sweeper 가 online agent 별로 평가, mo_instance 는 런타임 합성(<host>/disk, <host>/<module>).
_CORE_ALERT_RULES = [
    {'type': 'threshold_crossed', 'code': 'A-QOS-001', 'perceived_severity': 'warning',
     'event_type': 'qualityOfService', 'probable_cause': 'storageCapacityProblem', 'mo_class': 'host',
     'check': 'disk_high', 'scope': 'agent', 'unit': '%', 'metric': '디스크 사용률',
     # 단계 임계 (X.733 severity 승격 — 도달 단계가 severity, 승격/완화는 action=change)
     'thresholds': {'minor': 80, 'major': 90, 'critical': 95},
     'msg_open': '{mo} 디스크 사용률 {pct}% ({threshold}% 초과)', 'msg_close': '{mo} 디스크 사용률 {pct}% (정상)',
     'effect': '디스크 용량 임계 근접 — 로그/녹취 적재 실패 위험',
     'recommended_action': '사용량 원인 파악, 오래된 파일/로그 정리, 용량 증설'},
    {'type': 'process_down', 'code': 'A-PRC-001', 'perceived_severity': 'critical',
     'event_type': 'processingError', 'probable_cause': 'softwareError', 'mo_class': 'software',
     'check': 'module_down', 'scope': 'agent', 'metric': '프로세스 가용성',
     'msg_open': '{mo} 프로세스 응답 없음', 'msg_close': '{mo} 정상화',
     'effect': '해당 호스트의 모듈 기능 중단',
     'recommended_action': '프로세스 재기동, 로그/코어 확인, HA 절체 점검'},
    # agent 가 VIP 아닌 주소로 OAM 에 보고 — 절체하면 그 agent 가 OAM 과 단절된다(fleet
    # misdirect). **VIP 가 실제로 붙은 뒤에만** 판정한다(_agents_not_on_vip) — 개시 전에는
    # 전 agent 가 노드 IP 로 보고하는 것이 정상이라, 그때 잡으면 상시 경고가 되어 무의미하다.
    # 절체 자체는 정상 동작하므로(HA 판정은 노드 로컬) 잃는 것은 **관리 가시성**이다.
    {'type': 'config_out_of_sync', 'code': 'A-PRC-003', 'perceived_severity': 'warning',
     'event_type': 'processingError', 'probable_cause': 'configurationOrCustomizationError',
     'mo_class': 'software', 'check': 'oam_url_misdirect', 'scope': 'agent',
     'metric': 'OAM 접속 주소 정합',
     'msg_open': '{mo} agent 가 VIP 아닌 주소로 OAM 에 보고 (보고={actual}, 기대=VIP {expected})',
     'msg_close': '{mo} OAM 접속 주소 정상 (VIP)',
     'effect': '절체 후 이 서버가 OAM 과 단절 — 콘솔에 offline·모듈 상태 고착',
     'recommended_action': '시스템/서버 구성 > 서버 > OAM 접속 주소 에서 VIP 로 전환'},
    {'type': 'config_out_of_sync', 'code': 'A-PRC-003', 'perceived_severity': 'warning',
     'event_type': 'processingError', 'probable_cause': 'configurationOrCustomizationError', 'mo_class': 'software',
     'check': 'config_drift', 'scope': 'agent', 'metric': '배포 설정 정합',
     'msg_open': '{mo} 노드 설정 파일이 배포 기록과 불일치 (node={actual}, 기대={expected})',
     'msg_close': '{mo} 배포 설정 정합 회복',
     'effect': '모듈이 OAM 기록과 다른 설정(포트 등)으로 동작 — 게이트웨이 프록시/HA 헬스 오동작 위험',
     'recommended_action': '해당 배포 설정 재적용(update_config)으로 노드 파일 정렬, 수기 편집 여부 확인'},
    {'type': 'connection_lost', 'code': 'A-COM-015', 'perceived_severity': 'critical',
     'event_type': 'communications', 'probable_cause': 'communicationsSubsystemFailure',
     'mo_class': 'host', 'check': 'agent_lost', 'scope': 'agent', 'metric': '노드 관측성',
     'msg_open': 'Agent on {host} unreachable — node observation lost',
     'msg_close': 'Agent on {host} reachable again',
     'effect': '노드 관측 불능 — 그 노드의 agent 계열 알람은 판정 불가(두절이 정상 해소로 위장되지 않도록 별도 알람)',
     'recommended_action': '노드 전원/네트워크/agent 프로세스 확인'},
    {'type': 'threshold_crossed', 'code': 'A-QOS-023', 'perceived_severity': 'warning',
     'event_type': 'qualityOfService', 'probable_cause': 'thresholdCrossed', 'mo_class': 'service',
     'check': 'ha_flap', 'scope': 'agent', 'threshold': 6, 'unit': '회/10분', 'metric': 'HA 전이 빈도',
     'msg_open': '{mo} keepalived 상태 전이 {count}회/10분 ({threshold}회 이상) — VIP flap 의심',
     'msg_close': '{mo} HA 전이 빈도 정상',
     'effect': 'VIP 반복 이동 — 해당 서비스 간헐 단절',
     'recommended_action': '헬스체크 포트/모듈 상태 확인, notify_<svc>.log·keepalived journal 점검'},
]

# 알람 클래스 기본값 — check → 표준 분류 필드. 규칙에 명시값 있으면 우선(setdefault).
# process_unresponsive 는 check 개정 이행 규칙(_CHECK_REVISIONS)이 명시값을 폐기하고 오므로
# 메시지·runbook 까지 기본값으로 보유한다.
_ALERT_CLASS_DEFAULTS = {
    'process_unresponsive': {'type': 'process_unresponsive', 'code': 'A-PRC-004',
                     'event_type': 'processingError', 'probable_cause': 'responseTimeExcessive',
                     'mo_class': 'service', 'perceived_severity': 'major', 'metric': '프로세스 응답성',
                     'msg_open': '{mo} 관리 프로브(STATS) 무응답', 'msg_close': '{mo} 응답 정상화',
                     'effect': '제어/관측 불가 — hang·과부하 의심, 호처리 영향 가능',
                     'recommended_action': '프로세스 상태·부하 확인(process_down 동반 여부), 필요 시 재기동'},
    'module_down':  {'type': 'process_down', 'code': 'A-PRC-001', 'event_type': 'processingError',
                     'probable_cause': 'softwareError', 'mo_class': 'software', 'perceived_severity': 'critical'},
    'db_down':      {'type': 'connection_lost', 'code': 'A-COM-001', 'event_type': 'communications',
                     'probable_cause': 'communicationsSubsystemFailure', 'mo_class': 'service', 'perceived_severity': 'critical'},
    'rtp_pct_gte':  {'type': 'threshold_crossed', 'code': 'A-QOS-024', 'event_type': 'qualityOfService',
                     'probable_cause': 'resourceAtOrNearingCapacity', 'mo_class': 'service', 'perceived_severity': 'warning'},
    'disk_high':    {'type': 'threshold_crossed', 'code': 'A-QOS-001', 'event_type': 'qualityOfService',
                     'probable_cause': 'storageCapacityProblem', 'mo_class': 'host', 'perceived_severity': 'warning'},
    'config_drift': {'type': 'config_out_of_sync', 'code': 'A-PRC-003', 'event_type': 'processingError',
                     'probable_cause': 'configurationOrCustomizationError', 'mo_class': 'software', 'perceived_severity': 'warning'},
    'ha_flap':      {'type': 'threshold_crossed', 'code': 'A-QOS-023', 'event_type': 'qualityOfService',
                     'probable_cause': 'thresholdCrossed', 'mo_class': 'service', 'perceived_severity': 'warning'},
}

# 옛 per-process/리소스·개명 전 type → (조건클래스, code). 구 이벤트/규칙 read 시 alias.
# service_unresponsive 는 클래스 슬러그 개명(→process_unresponsive — 'service_' 접두가
# 서비스 감시로 오독됨, 실체는 프로세스 생존+무응답. process_down 과 대칭).
_OLD_TYPE_ALIAS = {
    'csp_down': ('process_down', 'A-PRC-001'), 'cmp_down': ('process_down', 'A-PRC-001'),
    'module_down': ('process_down', 'A-PRC-001'), 'db_down': ('connection_lost', 'A-COM-001'),
    'rtp_high': ('threshold_crossed', 'A-QOS-024'), 'disk_high': ('threshold_crossed', 'A-QOS-001'),
    'service_unresponsive': ('process_unresponsive', 'A-PRC-004'),
}

# 코드 개정 이력 — 옛 code → 현행 code. 코드는 불변이 원칙(§3.4 코드 문법 — NMS 사전 키)
# 이며, northbound 연동 전에 한해 개정 가능. 옛 code 규칙/활성 알람은 read 시 alias +
# 스윕 이행 종결(alarm_sweeper.close_legacy_code/close_migrated_keys) 로 흡수.
# - CIMS-CFG-001: CFG 는 DOMAIN=eventType 약어 규칙 위반 — PRC 로 정정 (구 개정 이력).
# - CIMS-<DOMAIN>-<SEQ> 클래스 코드 → flat 정의 코드 A-<DOMAIN>-NNN (표준화 §3.4(a)
#   번호 승계). 구 CIMS-QOS-001 이 여러 정의로 갈라진 rtp/ha_flap rule 은 check 기반
#   기본값(_ALERT_CLASS_DEFAULTS — A-QOS-024/A-QOS-023)이 배정하고, 이 dict 는 구
#   레코드 read alias 의 대표 정의(disk, A-QOS-001)로만 쓴다.
_CODE_REVISIONS = {
    'CIMS-CFG-001': 'A-PRC-003',
    'CIMS-PRC-001': 'A-PRC-001',
    'CIMS-PRC-002': 'A-PRC-002',
    'CIMS-PRC-003': 'A-PRC-003',
    'CIMS-PRC-004': 'A-PRC-004',
    'CIMS-COM-001': 'A-COM-001',
    'CIMS-QOS-001': 'A-QOS-001',
    'CIMS-QOS-002': 'A-QOS-002',
}

# check 개정 — 감지 3계층 분리(표준화 §3.4(b)): 구 probe check 'process_down' 은 프로세스
# 생존이 아니라 관리 응답성을 보므로 'process_unresponsive' 클래스로 개정. 규칙 **정체성**이
# 바뀌는 케이스라, 저장된 descriptor 의 구 클래스 명시값(type/code/severity/메시지·runbook)은
# read 시 폐기하고 새 클래스 기본값을 적용한다 — target/mo_instance 는 유지.
# (프로세스 생존의 process_down/PRC-001 은 agent module_down 정본으로 존속.)
# 구 'service_unresponsive' 는 같은 규칙의 개명 전 check 명 — 정체성 동일하나 명시값이
# 구 슬러그를 담고 있어 같은 개정 경로(기본값 재적용)로 흡수한다.
_CHECK_REVISIONS = {'process_down': 'process_unresponsive',
                    'service_unresponsive': 'process_unresponsive'}
_CHECK_REVISION_DROP = ('type', 'code', 'event_type', 'probable_cause', 'mo_class',
                        'perceived_severity', 'severity', 'metric', 'msg_open', 'msg_close',
                        'effect', 'recommended_action')

# check 개정으로 규칙의 code 가 교체된 경우의 옛 code — 스윕의 **targeted** 이행 종결용.
# 코드 개정(_CODE_REVISIONS)이 아니다: 옛 code 가 다른 규칙(module_down)으로 존속하므로
# code 전량 종결(close_legacy_code)을 쓰면 안 되고, 해당 규칙의 mo 공간으로 한정한다.
# (현행 잔여분(probe 계열 CIMS-PRC-001)은 구 mo(cims/*)에만 존재 — mo 루트 이행 종결
# (sweep_service_rules 의 close_migrated_keys)이 함께 흡수하므로 별도 스윕은 없다.)
_CHECK_LEGACY_CODES = {'process_unresponsive': ('CIMS-PRC-001',)}


def current_code(code: str) -> str:
    """옛 code → 현행 정의 코드 alias (read/수신 정규화용). 미개정 code 는 그대로."""
    return _CODE_REVISIONS.get(code or '', code)


def legacy_codes(code: str) -> list:
    """현행 code 의 옛 code 목록 — 스윕의 이행 종결(close_legacy_code) 용."""
    return [old for old, new in _CODE_REVISIONS.items() if new == code]


def legacy_check_codes(check: str) -> tuple:
    """check 개정으로 code 가 교체된 규칙의 옛 code 목록 — mo 한정 이행 종결용."""
    return _CHECK_LEGACY_CODES.get(check or '', ())


def normalize_alert_rule(r: dict) -> dict:
    """규칙에 표준 알람 필드(type 클래스/code/event_type/probable_cause/mo_class/perceived_severity)를 채움.
    명시값 우선, 없으면 check 기반 기본값. severity→perceived_severity 하위호환.
    저장된 descriptor 의 구 포맷 잔재(code 'CIMS-*', mo_instance 'cims/*')는 read 시
    폐기해 현행 기본값/런타임 mo 합성이 적용되게 한다 (표준화 §6 — store 는 무수정)."""
    out = dict(r)
    new_chk = _CHECK_REVISIONS.get(out.get('check'))
    if new_chk:
        out['check'] = new_chk
        for k in _CHECK_REVISION_DROP:
            out.pop(k, None)
    # 구 포맷 이행 — 클래스 코드는 check 기본값이 정의 코드를 배정(QOS-001 분할 대응),
    # 구 mo 루트(cims/*)는 스윕의 관측 신원 합성이 대체.
    if str(out.get('code') or '').startswith('CIMS-') and out.get('check') in _ALERT_CLASS_DEFAULTS:
        out.pop('code', None)
    if str(out.get('mo_instance') or '').startswith('cims/'):
        out.pop('mo_instance', None)
    for k, v in _ALERT_CLASS_DEFAULTS.get(out.get('check'), {}).items():
        out.setdefault(k, v)
    # 옛 type 슬러그(csp_down 등) → 클래스/코드 보정
    alias = _OLD_TYPE_ALIAS.get(out.get('type'))
    if alias:
        out['type'], _code = alias
        out.setdefault('code', _code)
    if not out.get('perceived_severity') and out.get('severity'):
        out['perceived_severity'] = out['severity']
    out.setdefault('perceived_severity', 'warning')
    out.setdefault('severity', out['perceived_severity'])  # 구 reader 호환
    out.setdefault('type', 'event'); out.setdefault('code', 'A-GEN-000')
    out['code'] = _CODE_REVISIONS.get(out['code'], out['code'])   # 개정된 옛 code 보정
    out.setdefault('event_type', 'processingError'); out.setdefault('probable_cause', '')
    out.setdefault('mo_class', 'service')
    return out

# seed descriptor 디렉토리 — 서비스 pack 이 자기 *.json 을 여기에 둔다 (CIMS = cims.json).
# 코어 코드엔 CIMS 데이터가 없음 (5-6: 데이터로 추출). store 비면 이 JSON 들을 1회 주입.
_SEED_DIR = os.path.join(os.path.dirname(__file__), 'service_descriptors_seed')

_CFG = None


def init(config: dict) -> None:
    """startup 시 config 캐시 (config 인자 없는 소비처용)."""
    global _CFG
    _CFG = config


def _cfg(config):
    return config if config is not None else _CFG


def _load_seed_files() -> list:
    """seed 디렉토리의 *.json descriptor 들 로드 (서비스 pack 제공)."""
    out = []
    for p in sorted(glob.glob(os.path.join(_SEED_DIR, '*.json'))):
        try:
            with open(p, encoding='utf-8') as f:
                doc = json.load(f)
            if isinstance(doc, dict) and doc.get('id'):
                out.append(doc)
        except Exception:
            continue
    return out


def seed_if_empty(config: dict = None) -> int:
    """descriptor store 가 비어있으면 seed JSON 들을 주입. 주입한 개수 반환."""
    c = _cfg(config)
    if c is None:
        return 0
    d = file_store.domain_dir(c, _DOMAIN)
    if file_store.load_all(d):
        return 0
    n = 0
    for doc in _load_seed_files():
        file_store.save(d, doc['id'], doc)
        n += 1
    return n


def merge_seed_updates(config: dict = None) -> int:
    """seed descriptor 에만 있는 **모듈**을 store 의 같은 id descriptor 에 추가 (기동 마이그레이션).

    `seed_if_empty` 는 store 가 비었을 때만 주입하므로, 이미 운용 중인 노드에는 새 모듈
    (예: 관리평면 `oam`/`oam-svc`)이 영구히 반영되지 않는다. 모듈이 descriptor 에 없으면
    `_agent_daemon_modules` 가 그 모듈을 daemon 으로 보지 않아 HA 의 cold/relevant/헬스
    대상에서 빠진다 → 이중화 대상이 될 수 없다.

    같은 이유로 **데이터 소스(`data_sources`)** 도 병합한다 — 서비스 pack 이 새 소스나 새 shape
    을 추가해도 이미 seed 된 노드에는 영원히 닿지 않아, 그 소스를 쓰는 화면이 빈 화면이 된다.
    shape/map 은 운영자 정책이 아니라 **렌더러와의 계약**이라 코드가 정본이다.

    운영자 편집을 보존한다: **이름이 없는 모듈만 추가**하고, 기존 모듈 엔트리·alert_rules·
    label 은 건드리지 않는다. 예외로 기존 모듈의 `health` 블록에는 **없는 키만 채운다**
    (`startup_grace_sec` 처럼 뒤에 추가된 안전 필드가 기존 설치본에 영구히 빠지지 않게 —
    값이 이미 있으면 운영자 판단으로 보고 덮지 않는다). 추가/보강 건수 반환."""
    c = _cfg(config)
    if c is None:
        return 0
    d = file_store.domain_dir(c, _DOMAIN)
    rows = {r.get('id'): r for r in file_store.load_all(d) if isinstance(r, dict) and r.get('id')}
    if not rows:
        return 0            # 비어 있으면 seed_if_empty 가 전체 주입 — 여기서 할 일 없음
    added = 0
    for doc in _load_seed_files():
        cur = rows.get(doc.get('id'))
        if not cur:
            continue        # store 에 없는 서비스 pack — 전체 주입은 seed_if_empty 의 몫
        by_name = {m.get('name'): m for m in (cur.get('modules') or []) if isinstance(m, dict)}
        new_mods = [m for m in (doc.get('modules') or [])
                    if isinstance(m, dict) and m.get('name') and m['name'] not in by_name]
        # 기존 모듈의 health 블록 보강 — 없는 키만 (안전 필드 소급 적용)
        filled = 0
        for sm in (doc.get('modules') or []):
            if not isinstance(sm, dict):
                continue
            tgt = by_name.get(sm.get('name'))
            sh = sm.get('health')
            if not tgt or not isinstance(sh, dict) or not isinstance(tgt.get('health'), dict):
                continue
            for k, v in sh.items():
                if k not in tgt['health']:
                    tgt['health'][k] = v
                    filled += 1
        # 데이터 소스 — seed 가 가진 소스는 **seed 를 그대로 정본으로 삼는다**(shapes·map·endpoint).
        # 이 값들은 운영자 정책이 아니라 렌더러와의 계약이다(어느 필드를 어떤 축으로 읽는가).
        # "없는 것만 채우기"로 두면 두 방향 모두 막힌다 — 매핑을 고쳐도 옛 노드에 안 닿고,
        # shape 를 빼도 store 에 남아 빈 화면을 만든다. 실제로 kpi 를 뺐는데 목록에만 남는 일이 있었다.
        # 운영자가 **직접 추가한 소스**(seed 에 없는 id)는 건드리지 않는다.
        by_id = {x.get('id'): x for x in (cur.get('data_sources') or []) if isinstance(x, dict)}
        new_srcs, filled_shapes = [], 0
        for sd in (doc.get('data_sources') or []):
            if not isinstance(sd, dict) or not sd.get('id'):
                continue
            tgt = by_id.get(sd['id'])
            if not tgt:
                new_srcs.append(sd)
                continue
            for k in ('shapes', 'map', 'endpoint', 'query', 'label', 'needsControls'):
                want = sd.get(k)
                if want is None:
                    if k in tgt:
                        del tgt[k]
                        filled_shapes += 1
                elif tgt.get(k) != want:
                    tgt[k] = want
                    filled_shapes += 1

        if not new_mods and not filled and not new_srcs and not filled_shapes:
            continue
        if new_mods:
            cur.setdefault('modules', []).extend(new_mods)
        if new_srcs:
            cur.setdefault('data_sources', []).extend(new_srcs)
        file_store.save(d, cur['id'], cur)
        added += len(new_mods) + filled + len(new_srcs) + filled_shapes
    return added


def load_descriptors(config: dict = None) -> list:
    c = _cfg(config)
    if c is None:
        return []
    return file_store.load_all(file_store.domain_dir(c, _DOMAIN))


def all_modules(config: dict = None) -> dict:
    """{name: {name, port?, proto?, controllable?, service_id}} — 전 descriptor 의 모듈 병합."""
    out = {}
    for d in load_descriptors(config):
        for m in (d.get('modules') or []):
            nm = m.get('name')
            if nm:
                out[nm] = {**m, 'service_id': d.get('id')}
    return out


def module_health_defaults(config: dict = None) -> dict:
    """ha_groups 용 {name: (port, proto)} — port 가 있는 모듈만."""
    res = {}
    for nm, m in all_modules(config).items():
        if m.get('port'):
            res[nm] = (int(m['port']), m.get('proto', 'tcp'))
    return res


def module_health_specs(config: dict = None) -> dict:
    """ha_groups 용 {name: health} — descriptor 모듈의 `health` 블록만 추린다.

    `health` 는 "리슨 포트를 descriptor 상수가 아니라 노드의 실제 설정에서 유도하라"는
    선언이다. 두 형태를 지원한다 (둘 다 agent 가 검사 시점에 노드 로컬 파일을 직접 읽음
    — 배포기록↔실파일 드리프트가 나도 HA 는 실제 bind 포트를 본다):

      { "config_key": "Server.Port" }
          스칼라 config.json 의 단일 키 (csc 처럼 포트가 설정키 하나로 정해지는 모듈).

      { "collection_file": "config/local_nodes.jsonl",
        "field": "bind_port",
        "match": { "enabled": true, "is_primary": true, "protocol": "UDP" } }
          컬렉션 jsonl 에서 match 를 만족하는 첫 레코드의 field (csp 처럼 리슨
          엔드포인트가 컬렉션에 있는 모듈). 파일/레코드가 없으면 descriptor port 로 폴백.
    """
    res = {}
    for nm, m in all_modules(config).items():
        h = m.get('health')
        if isinstance(h, dict) and h:
            res[nm] = h
    return res


def valid_module_names(config: dict = None) -> set:
    """build 용 — 코어 + 전 descriptor 모듈명."""
    return set(_CORE_MODULES) | set(all_modules(config).keys())


def controllable_modules(config: dict = None) -> set:
    """service_control 용 — 코어 controllable + descriptor controllable 모듈."""
    res = set(_CORE_CONTROLLABLE)
    for nm, m in all_modules(config).items():
        if m.get('controllable'):
            res.add(nm)
    return res


def alert_rules(config: dict = None) -> list:
    """alert sweeper / /alerts/rules 용 — 코어 host 규칙 + 전 descriptor 의 alert_rules 병합.
    표준 알람 필드(type 클래스/code/event_type/probable_cause/perceived_severity)로 정규화."""
    out = list(_CORE_ALERT_RULES)
    for d in load_descriptors(config):
        out.extend(d.get('alert_rules') or [])
    return [normalize_alert_rule(r) for r in out]


def alarm_catalog(config: dict = None) -> list:
    """알람 클래스 카탈로그 — code 별 1개(정의). GET /alerts/catalog 용."""
    seen = {}
    for r in alert_rules(config):
        code = r.get('code')
        if not code or code in seen:
            continue
        seen[code] = {
            'code': code, 'type': r.get('type'),
            'perceived_severity': r.get('perceived_severity'),
            'event_type': r.get('event_type'), 'probable_cause': r.get('probable_cause'),
            'mo_class': r.get('mo_class'), 'metric': r.get('metric'),
            'effect': r.get('effect'), 'recommended_action': r.get('recommended_action'),
        }
    return list(seen.values())


def data_sources(config: dict = None) -> list:
    """콘솔 shape 위젯(차트/표/KPI/분포)용 — 전 descriptor 의 data_sources 병합.
    각 항목은 선언적 스펙(endpoint + shape별 필드 매핑) — 프론트 범용 로더가 해석."""
    out = []
    for d in load_descriptors(config):
        for s in (d.get('data_sources') or []):
            out.append({**s, 'service_id': d.get('id')})
    return out
