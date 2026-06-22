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
    {'type': 'threshold_crossed', 'code': 'CIMS-QOS-001', 'perceived_severity': 'warning',
     'event_type': 'qualityOfService', 'probable_cause': 'storageCapacityProblem', 'mo_class': 'host',
     'check': 'disk_high', 'scope': 'agent', 'threshold': 90, 'unit': '%', 'metric': '디스크 사용률',
     'msg_open': '{mo} 디스크 사용률 {pct}% ({threshold}% 초과)', 'msg_close': '{mo} 디스크 사용률 {pct}% (정상)',
     'effect': '디스크 용량 임계 근접 — 로그/녹취 적재 실패 위험',
     'recommended_action': '사용량 원인 파악, 오래된 파일/로그 정리, 용량 증설'},
    {'type': 'process_down', 'code': 'CIMS-PRC-001', 'perceived_severity': 'critical',
     'event_type': 'processingError', 'probable_cause': 'softwareError', 'mo_class': 'software',
     'check': 'module_down', 'scope': 'agent', 'metric': '프로세스 가용성',
     'msg_open': '{mo} 프로세스 응답 없음', 'msg_close': '{mo} 정상화',
     'effect': '해당 호스트의 모듈 기능 중단',
     'recommended_action': '프로세스 재기동, 로그/코어 확인, HA 절체 점검'},
]

# 알람 클래스 기본값 — check → 표준 분류 필드. 규칙에 명시값 있으면 우선(setdefault).
_ALERT_CLASS_DEFAULTS = {
    'process_down': {'type': 'process_down', 'code': 'CIMS-PRC-001', 'event_type': 'processingError',
                     'probable_cause': 'softwareError', 'mo_class': 'software', 'perceived_severity': 'critical'},
    'module_down':  {'type': 'process_down', 'code': 'CIMS-PRC-001', 'event_type': 'processingError',
                     'probable_cause': 'softwareError', 'mo_class': 'software', 'perceived_severity': 'critical'},
    'db_down':      {'type': 'connection_lost', 'code': 'CIMS-COM-001', 'event_type': 'communications',
                     'probable_cause': 'communicationsSubsystemFailure', 'mo_class': 'service', 'perceived_severity': 'critical'},
    'rtp_pct_gte':  {'type': 'threshold_crossed', 'code': 'CIMS-QOS-001', 'event_type': 'qualityOfService',
                     'probable_cause': 'resourceAtOrNearingCapacity', 'mo_class': 'service', 'perceived_severity': 'warning'},
    'disk_high':    {'type': 'threshold_crossed', 'code': 'CIMS-QOS-001', 'event_type': 'qualityOfService',
                     'probable_cause': 'storageCapacityProblem', 'mo_class': 'host', 'perceived_severity': 'warning'},
}

# 옛 per-process/리소스 type → (조건클래스, code). 구 이벤트/규칙 read 시 alias.
_OLD_TYPE_ALIAS = {
    'csp_down': ('process_down', 'CIMS-PRC-001'), 'cmp_down': ('process_down', 'CIMS-PRC-001'),
    'module_down': ('process_down', 'CIMS-PRC-001'), 'db_down': ('connection_lost', 'CIMS-COM-001'),
    'rtp_high': ('threshold_crossed', 'CIMS-QOS-001'), 'disk_high': ('threshold_crossed', 'CIMS-QOS-001'),
}


def normalize_alert_rule(r: dict) -> dict:
    """규칙에 표준 알람 필드(type 클래스/code/event_type/probable_cause/mo_class/perceived_severity)를 채움.
    명시값 우선, 없으면 check 기반 기본값. severity→perceived_severity 하위호환."""
    out = dict(r)
    for k, v in _ALERT_CLASS_DEFAULTS.get(r.get('check'), {}).items():
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
    out.setdefault('type', 'event'); out.setdefault('code', 'CIMS-GEN-000')
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
