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
from services import file_store

_DOMAIN = 'services'

# 코어(서비스 무지) — OAM 플랫폼 자체 패키지. 항상 유효한 모듈.
_CORE_MODULES = {'agent', 'oam', 'console'}
_CORE_CONTROLLABLE = {'console'}

# 전환 seed — CIMS 서비스 pack 기본 descriptor. store 비었을 때 1회 주입.
# 기존 ha_groups._MODULE_HEALTH_DEFAULTS / build._VALID_MODULES / service_control._ALLOWED 와 동일.
# TODO(5-6): CIMS service pack(csc) 으로 추출.
_CIMS_SEED = {
    'id': 'cims',
    'label': 'CIMS',
    'modules': [
        {'name': 'csp',    'port': 5060, 'proto': 'udp', 'controllable': True},
        {'name': 'isp',    'port': 5060, 'proto': 'udp'},
        {'name': 'psp',    'port': 5060, 'proto': 'udp'},
        {'name': 'cmp',    'port': 9000, 'proto': 'udp', 'controllable': True},
        {'name': 'imp',    'port': 9000, 'proto': 'udp'},
        {'name': 'pmp',    'port': 9000, 'proto': 'udp'},
        {'name': 'csc',    'port': 4420, 'proto': 'tcp', 'controllable': True},
        {'name': 'cwrtc',  'controllable': True},
        {'name': 'phone',  'controllable': True},
        {'name': 'cspsim'},
    ],
}

_CFG = None


def init(config: dict) -> None:
    """startup 시 config 캐시 (config 인자 없는 소비처용)."""
    global _CFG
    _CFG = config


def _cfg(config):
    return config if config is not None else _CFG


def seed_if_empty(config: dict = None) -> bool:
    """descriptor store 가 비어있으면 CIMS seed 주입. 주입했으면 True."""
    c = _cfg(config)
    if c is None:
        return False
    d = file_store.domain_dir(c, _DOMAIN)
    if file_store.load_all(d):
        return False
    file_store.save(d, _CIMS_SEED['id'], dict(_CIMS_SEED))
    return True


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
