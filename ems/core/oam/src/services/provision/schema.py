"""inventory.yaml / blueprint.yaml 파싱 + 검증 (auto_deployment.md §3).

두 문서의 역할 분리:
  inventory  — 서버가 어디 있고 어떻게 로그인하나 (사이트 고유, 비밀 포함)
  blueprint  — 무엇을 어떤 구조로 깔 것인가 (사이트 무관, 형상 관리 대상)
연결고리는 논리명 하나다: blueprint.systems[].members[].server → inventory.servers[].name

설계 원칙:
  - `yaml.safe_load` 만 사용 (임의 객체 역직렬화 차단).
  - 모르는 키는 무시하지 않고 **오류**로 만든다. 오타로 설정이 조용히 누락되는 사고가
    배포 도구에서는 치명적이라, silent drop 을 허용하지 않는다.
  - 검증은 첫 오류에서 멈추지 않고 **전부 모아서** 반환한다 — 콘솔이 필드별 인라인
    표시를 하려면 목록이 필요하다.
  - 파서는 네트워크·파일시스템에 의존하지 않는다(문자열 in → 구조 out). 패키지 존재
    여부처럼 OAM 조회가 필요한 검증은 상위(engine)에서 수행한다.
"""

from __future__ import annotations

import ipaddress
import os
import re
import sys

# oam vendor 에 PyYAML 동봉 (services/provision → src → oam/vendor).
# oam_app.py 가 이미 vendor 를 sys.path 에 올리지만, CLI·단독 import 경로에서도
# 동작하도록 여기서도 보강한다.
_HERE = os.path.dirname(os.path.abspath(__file__))
_VENDOR = os.path.normpath(os.path.join(_HERE, '..', '..', '..', 'vendor'))
if os.path.isdir(_VENDOR) and _VENDOR not in sys.path:
    sys.path.append(_VENDOR)

import yaml   # noqa: E402  (vendor 경로 확보 후 import)


# ──────────────────────────────────────────────────────────────
#  결과 타입
# ──────────────────────────────────────────────────────────────

class Issue:
    """검증 지적 1건. path 는 콘솔이 해당 필드를 하이라이트하는 데 쓴다."""

    __slots__ = ('level', 'path', 'message')

    def __init__(self, level: str, path: str, message: str):
        self.level = level          # 'error' | 'warning'
        self.path = path            # 예: 'servers[0].ssh'
        self.message = message

    def as_dict(self) -> dict:
        return {'level': self.level, 'path': self.path, 'message': self.message}

    def __repr__(self):
        return f'<{self.level} {self.path}: {self.message}>'


class ParseError(Exception):
    """YAML 문법 오류 — 구조 검증 이전 단계에서 실패."""

    def __init__(self, message: str, line: int | None = None):
        super().__init__(message)
        self.message = message
        self.line = line

    def as_dict(self) -> dict:
        return {'error': 'yaml_parse_error', 'message': self.message, 'line': self.line}


# ──────────────────────────────────────────────────────────────
#  공통 헬퍼
# ──────────────────────────────────────────────────────────────

_NAME_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')

# 지원 스키마 버전 — 상위 버전 문서는 거부한다(모르는 의미를 추측 실행하지 않음).
SUPPORTED_VERSION = 1


def load_yaml(text: str) -> dict:
    """YAML 원문 → dict. 문법 오류는 줄번호와 함께 ParseError."""
    try:
        doc = yaml.safe_load(text)
    except yaml.YAMLError as e:
        line = None
        mark = getattr(e, 'problem_mark', None)
        if mark is not None:
            line = mark.line + 1
        raise ParseError(str(e), line) from None
    if doc is None:
        raise ParseError('빈 문서')
    if not isinstance(doc, dict):
        raise ParseError(f'최상위는 매핑이어야 함 (현재 {type(doc).__name__})')
    return doc


def dump_yaml(doc: dict) -> str:
    """구조 → YAML 원문. 구성 뷰 편집 결과를 되직렬화할 때 사용.
    주석은 보존되지 않는다(§3.0) — 호출부가 원문을 별도 보관한다."""
    return yaml.safe_dump(doc, allow_unicode=True, sort_keys=False, default_flow_style=False)


def _unknown_keys(obj: dict, allowed: set, path: str, out: list):
    for k in obj:
        if k not in allowed:
            out.append(Issue('error', f'{path}.{k}' if path else k,
                             f"알 수 없는 키 '{k}' — 오타 여부 확인 (허용: {', '.join(sorted(allowed))})"))


def _require_version(doc: dict, path: str, out: list):
    v = doc.get('version')
    if v is None:
        out.append(Issue('error', 'version', 'version 키 필수'))
    elif not isinstance(v, int):
        out.append(Issue('error', 'version', f'정수여야 함 (현재 {v!r})'))
    elif v > SUPPORTED_VERSION:
        out.append(Issue('error', 'version',
                         f'지원하지 않는 스키마 버전 {v} (이 provisioner 는 {SUPPORTED_VERSION} 까지)'))


def _check_name(val, path: str, out: list, what: str = '이름') -> bool:
    if not isinstance(val, str) or not val:
        out.append(Issue('error', path, f'{what} 필수 (문자열)'))
        return False
    if not _NAME_RE.match(val):
        out.append(Issue('error', path,
                         f"{what} '{val}' 형식 오류 — 영숫자로 시작, 영숫자/./_/- 만, 64자 이하"))
        return False
    return True


def _check_ip(val, path: str, out: list, allow_hostname: bool = False) -> bool:
    if not isinstance(val, str) or not val:
        out.append(Issue('error', path, 'IP 주소 필수'))
        return False
    try:
        ipaddress.ip_address(val)
        return True
    except ValueError:
        if allow_hostname and re.match(r'^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?$', val):
            return True
        out.append(Issue('error', path, f"IP 주소 형식 오류: '{val}'"))
        return False


def _check_port(val, path: str, out: list, default_ok: bool = True) -> bool:
    if val is None:
        return default_ok
    if not isinstance(val, int) or isinstance(val, bool) or not (1 <= val <= 65535):
        out.append(Issue('error', path, f'포트는 1~65535 정수 (현재 {val!r})'))
        return False
    return True


def _as_list(val):
    return val if isinstance(val, list) else []


# ──────────────────────────────────────────────────────────────
#  inventory
# ──────────────────────────────────────────────────────────────

_INV_TOP = {'version', 'defaults', 'servers'}
_INV_DEFAULTS = {'ssh', 'sudo', 'install_dir', 'svc_user'}
_INV_SERVER = {'name', 'host', 'ssh', 'sudo', 'install_dir', 'svc_user',
               'agent_preinstalled'}
_INV_SSH = {'user', 'port', 'password'}
_INV_SUDO = {'method', 'password'}

# 비밀값이 담기는 경로 — 마스킹·로그필터의 SoT. 다른 모듈이 이 목록을 참조한다.
SECRET_FIELDS = (('ssh', 'password'), ('sudo', 'password'))


class Server:
    """인벤토리의 서버 1건 — defaults 가 병합된 실효값."""

    __slots__ = ('name', 'host', 'ssh_user', 'ssh_port', 'ssh_password',
                 'sudo_method', 'sudo_password',
                 'install_dir', 'svc_user', 'agent_preinstalled')

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))

    @property
    def auth_mode(self) -> str:
        """실제로 쓸 인증 수단. 비밀번호 인증만 지원한다(운영 환경에 SSH 키 미사용).

        agent_preinstalled 노드는 SSH 를 하지 않으므로 'preinstalled' 를 돌려준다.
        """
        if self.agent_preinstalled:
            return 'preinstalled'
        if self.ssh_password:
            return 'password'
        return 'none'

    def secrets(self) -> list:
        """로그 마스킹 대상 문자열."""
        return [s for s in (self.ssh_password, self.sudo_password) if s]

    def as_dict(self, mask: bool = True) -> dict:
        pw = '••••' if (mask and self.ssh_password) else self.ssh_password
        spw = '••••' if (mask and self.sudo_password) else self.sudo_password
        return {
            'name': self.name, 'host': self.host,
            'ssh': {'user': self.ssh_user, 'port': self.ssh_port, 'password': pw},
            'sudo': {'method': self.sudo_method, 'password': spw},
            'install_dir': self.install_dir, 'svc_user': self.svc_user,
            'agent_preinstalled': self.agent_preinstalled,
            'auth_mode': self.auth_mode,
        }


class Inventory:
    def __init__(self, servers: list):
        self.servers = servers
        self._by_name = {s.name: s for s in servers}

    def get(self, name: str):
        return self._by_name.get(name)

    def names(self) -> list:
        return [s.name for s in self.servers]

    def all_secrets(self) -> list:
        out = []
        for s in self.servers:
            out.extend(s.secrets())
        return out

    def as_dict(self, mask: bool = True) -> dict:
        return {'version': SUPPORTED_VERSION,
                'servers': [s.as_dict(mask) for s in self.servers]}


def parse_inventory(text: str) -> tuple:
    """YAML 원문 → (Inventory | None, [Issue]).

    오류가 하나라도 있으면 Inventory 는 None — 부분 실행을 막기 위해 관대한 복구를 하지 않는다.
    """
    issues: list = []
    doc = load_yaml(text)           # ParseError 는 호출부로 전파

    _unknown_keys(doc, _INV_TOP, '', issues)
    _require_version(doc, 'version', issues)

    defaults = doc.get('defaults') or {}
    if not isinstance(defaults, dict):
        issues.append(Issue('error', 'defaults', '매핑이어야 함'))
        defaults = {}
    else:
        _unknown_keys(defaults, _INV_DEFAULTS, 'defaults', issues)

    d_ssh = defaults.get('ssh') or {}
    d_sudo = defaults.get('sudo') or {}
    if not isinstance(d_ssh, dict):
        issues.append(Issue('error', 'defaults.ssh', '매핑이어야 함')); d_ssh = {}
    else:
        _unknown_keys(d_ssh, _INV_SSH, 'defaults.ssh', issues)
    if not isinstance(d_sudo, dict):
        issues.append(Issue('error', 'defaults.sudo', '매핑이어야 함')); d_sudo = {}
    else:
        _unknown_keys(d_sudo, _INV_SUDO, 'defaults.sudo', issues)

    raw_servers = doc.get('servers')
    if not isinstance(raw_servers, list) or not raw_servers:
        issues.append(Issue('error', 'servers', '서버를 1개 이상 정의해야 함'))
        raw_servers = []

    servers: list = []
    seen_names: set = set()
    seen_hosts: dict = {}

    for i, raw in enumerate(raw_servers):
        p = f'servers[{i}]'
        if not isinstance(raw, dict):
            issues.append(Issue('error', p, '매핑이어야 함'))
            continue
        _unknown_keys(raw, _INV_SERVER, p, issues)

        name = raw.get('name')
        if _check_name(name, f'{p}.name', issues, '서버 논리명'):
            if name in seen_names:
                issues.append(Issue('error', f'{p}.name', f"서버명 중복: '{name}'"))
            seen_names.add(name)

        host = raw.get('host')
        _check_ip(host, f'{p}.host', issues, allow_hostname=True)
        if host:
            if host in seen_hosts:
                issues.append(Issue('warning', f'{p}.host',
                                    f"host '{host}' 가 {seen_hosts[host]} 와 중복 — "
                                    f'같은 물리 노드에 agent 를 두 번 설치하게 된다'))
            else:
                seen_hosts[host] = p

        ssh = raw.get('ssh')
        if ssh is None:
            ssh = {}
        elif not isinstance(ssh, dict):
            issues.append(Issue('error', f'{p}.ssh', '매핑이어야 함')); ssh = {}
        else:
            _unknown_keys(ssh, _INV_SSH, f'{p}.ssh', issues)

        sudo = raw.get('sudo')
        if sudo is None:
            sudo = {}
        elif not isinstance(sudo, dict):
            issues.append(Issue('error', f'{p}.sudo', '매핑이어야 함')); sudo = {}
        else:
            _unknown_keys(sudo, _INV_SUDO, f'{p}.sudo', issues)

        # agent_preinstalled — 이 노드는 agent 가 이미 설치·enroll 되어 있으므로
        # provisioner 가 SSH 하지 않는다. **OAM(부트스트랩) 노드가 항상 이 상태**다:
        # install.sh 가 로컬 agent 를 깔고 enroll 까지 끝내놓기 때문. 이 노드에까지
        # SSH 자격증명을 적게 만들면 쓰지도 않는 비밀번호를 파일에 남기게 된다.
        preinstalled = bool(raw.get('agent_preinstalled'))

        ssh_user = ssh.get('user', d_ssh.get('user'))
        ssh_port = ssh.get('port', d_ssh.get('port')) or 22
        password = ssh.get('password', d_ssh.get('password'))
        sudo_method = sudo.get('method', d_sudo.get('method')) or 'password'
        sudo_password = sudo.get('password', d_sudo.get('password'))

        if preinstalled:
            if password or sudo_password:
                issues.append(Issue('warning', p,
                                    'agent_preinstalled 노드에는 SSH 하지 않으므로 '
                                    '비밀번호가 쓰이지 않는다 — 파일에서 지우는 편이 안전하다'))
        else:
            if not ssh_user:
                issues.append(Issue('error', f'{p}.ssh.user',
                                    'SSH 계정 필수 (여기 또는 defaults.ssh.user)'))
            _check_port(ssh_port, f'{p}.ssh.port', issues)

            if not password:
                issues.append(Issue('error', f'{p}.ssh.password',
                                    'SSH 비밀번호 필수 — 접속 수단이 없으면 run 을 시작할 수 '
                                    '없다. agent 가 이미 설치된 노드면 '
                                    'agent_preinstalled: true 로 표시한다'))

            if sudo_method not in ('password', 'nopasswd'):
                issues.append(Issue('error', f'{p}.sudo.method',
                                    f"'password' 또는 'nopasswd' (현재 {sudo_method!r})"))
            if sudo_method == 'password' and not sudo_password:
                issues.append(Issue('error', f'{p}.sudo.password',
                                    'sudo 비밀번호 필수 — NOPASSWD 환경이면 '
                                    'sudo.method: nopasswd'))

            # root 로그인 거부 — install-agent.sh 가 SUDO_USER 기반이라 root 직접 로그인은
            # 부분 설치를 낳는다 (02_deployment.md §2.1 권한 정책).
            if ssh_user == 'root':
                issues.append(Issue('error', f'{p}.ssh.user',
                                    'root 직접 로그인 불가 — 일반 계정 + sudo 로 접속해야 한다 '
                                    '(설치 스크립트가 SUDO_USER 로 서비스 계정을 판별)'))

        servers.append(Server(
            name=name, host=host,
            ssh_user=ssh_user, ssh_port=ssh_port, ssh_password=password,
            sudo_method=sudo_method, sudo_password=sudo_password,
            install_dir=raw.get('install_dir', defaults.get('install_dir')) or '/opt/cims-agent',
            svc_user=raw.get('svc_user', defaults.get('svc_user')) or ssh_user,
            agent_preinstalled=preinstalled,
        ))

    if any(i.level == 'error' for i in issues):
        return None, issues
    return Inventory(servers), issues


# ──────────────────────────────────────────────────────────────
#  blueprint
# ──────────────────────────────────────────────────────────────

_BP_TOP = {'version', 'name', 'description', 'systems', 'start_order'}
_BP_SYSTEM = {'name', 'mode', 'vips', 'failover', 'members', 'modules',
              'ha_group', 'auth_pass'}
_BP_VIP = {'ip', 'prefix', 'interface', 'slot'}
_BP_MEMBER = {'server', 'role'}
_BP_MODULE = {'package', 'version', 'process_name', 'config', 'per_server',
              'collections', 'start'}

MODES = ('active_standby', 'all_active', 'standalone')

_VERSION_RE = re.compile(r'^\d+(\.\d+){1,3}$')


class Module:
    __slots__ = ('package', 'version', 'process_name', 'config', 'per_server',
                 'collections', 'start')

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))

    def config_for(self, server_name: str) -> dict:
        """공통 config + 해당 서버 per_server 를 병합한 실효 overlay."""
        merged = dict(self.config or {})
        merged.update((self.per_server or {}).get(server_name) or {})
        return merged

    def as_dict(self) -> dict:
        return {'package': self.package, 'version': self.version,
                'process_name': self.process_name, 'config': self.config,
                'per_server': self.per_server, 'collections': self.collections,
                'start': self.start}


class System:
    __slots__ = ('name', 'mode', 'vips', 'failover', 'members', 'modules',
                 'ha_group', 'auth_pass')

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))

    def server_names(self) -> list:
        return [m['server'] for m in self.members]

    def master_first(self) -> list:
        """A/S 는 master 를 먼저 — VIP 선점 순서를 보장한다."""
        if self.mode != 'active_standby':
            return list(self.members)
        return sorted(self.members, key=lambda m: 0 if m.get('role') == 'master' else 1)

    def as_dict(self) -> dict:
        return {'name': self.name, 'mode': self.mode, 'vips': self.vips,
                'failover': self.failover, 'members': self.members,
                'ha_group': self.ha_group, 'auth_pass': self.auth_pass,
                'modules': [m.as_dict() for m in self.modules]}


class Blueprint:
    def __init__(self, name: str, description: str, systems: list, start_order: list):
        self.name = name
        self.description = description
        self.systems = systems
        self.start_order = start_order

    def system(self, name: str):
        for s in self.systems:
            if s.name == name:
                return s
        return None

    def systems_in_start_order(self) -> list:
        ordered = [self.system(n) for n in self.start_order]
        ordered = [s for s in ordered if s is not None]
        rest = [s for s in self.systems if s.name not in self.start_order]
        return ordered + rest

    def referenced_servers(self) -> list:
        out = []
        for s in self.systems:
            for n in s.server_names():
                if n not in out:
                    out.append(n)
        return out

    def as_dict(self) -> dict:
        return {'version': SUPPORTED_VERSION, 'name': self.name,
                'description': self.description,
                'systems': [s.as_dict() for s in self.systems],
                'start_order': self.start_order}


def parse_blueprint(text: str) -> tuple:
    """YAML 원문 → (Blueprint | None, [Issue]). inventory 참조 검증은 cross_validate 가 별도로 한다."""
    issues: list = []
    doc = load_yaml(text)

    _unknown_keys(doc, _BP_TOP, '', issues)
    _require_version(doc, 'version', issues)
    _check_name(doc.get('name'), 'name', issues, '블루프린트 이름')

    raw_systems = doc.get('systems')
    if not isinstance(raw_systems, list) or not raw_systems:
        issues.append(Issue('error', 'systems', '시스템을 1개 이상 정의해야 함'))
        raw_systems = []

    systems: list = []
    seen_sys: set = set()
    seen_members: dict = {}     # server → system name (1 agent = 1 group)
    seen_vips: dict = {}

    for i, raw in enumerate(raw_systems):
        p = f'systems[{i}]'
        if not isinstance(raw, dict):
            issues.append(Issue('error', p, '매핑이어야 함'))
            continue
        _unknown_keys(raw, _BP_SYSTEM, p, issues)

        sname = raw.get('name')
        if _check_name(sname, f'{p}.name', issues, '시스템 이름'):
            if sname in seen_sys:
                issues.append(Issue('error', f'{p}.name', f"시스템명 중복: '{sname}'"))
            seen_sys.add(sname)

        mode = raw.get('mode') or 'standalone'
        if mode not in MODES:
            issues.append(Issue('error', f'{p}.mode',
                                f"mode 는 {' | '.join(MODES)} (현재 {mode!r})"))

        # ── members ──
        raw_members = raw.get('members')
        if not isinstance(raw_members, list) or not raw_members:
            issues.append(Issue('error', f'{p}.members', '멤버를 1개 이상 정의해야 함'))
            raw_members = []
        members: list = []
        for j, m in enumerate(raw_members):
            mp = f'{p}.members[{j}]'
            if not isinstance(m, dict):
                issues.append(Issue('error', mp, '매핑이어야 함'))
                continue
            _unknown_keys(m, _BP_MEMBER, mp, issues)
            srv = m.get('server')
            if not _check_name(srv, f'{mp}.server', issues, '서버 참조'):
                continue
            if srv in seen_members:
                issues.append(Issue('error', f'{mp}.server',
                                    f"서버 '{srv}' 가 이미 '{seen_members[srv]}' 에 속함 "
                                    f'— 1 서버는 1 시스템에만 편입된다'))
            else:
                seen_members[srv] = sname
            role = m.get('role')
            if role is not None and role not in ('master', 'backup'):
                issues.append(Issue('error', f'{mp}.role',
                                    f"role 은 master | backup (현재 {role!r})"))
            members.append({'server': srv, 'role': role})

        if mode == 'active_standby':
            if len(members) != 2:
                issues.append(Issue('error', f'{p}.members',
                                    f'active_standby 는 멤버 2개여야 함 (현재 {len(members)})'))
            roles = [m.get('role') for m in members]
            if roles.count('master') != 1 or roles.count('backup') != 1:
                issues.append(Issue('error', f'{p}.members',
                                    'active_standby 는 master 1 + backup 1 을 명시해야 함 '
                                    '— VIP 선점 순서 판정에 필요'))
        elif mode == 'standalone' and len(members) != 1:
            issues.append(Issue('error', f'{p}.members',
                                f'standalone 은 멤버 1개여야 함 (현재 {len(members)})'))
        elif mode == 'all_active':
            if any(m.get('role') for m in members):
                issues.append(Issue('warning', f'{p}.members',
                                    'all_active 에서는 role 이 무시된다'))

        # ── vips ──
        vips: list = []
        raw_vips = raw.get('vips')
        if raw_vips is not None and not isinstance(raw_vips, list):
            issues.append(Issue('error', f'{p}.vips', '배열이어야 함'))
            raw_vips = []
        for j, v in enumerate(_as_list(raw_vips)):
            vp = f'{p}.vips[{j}]'
            if not isinstance(v, dict):
                issues.append(Issue('error', vp, '매핑이어야 함'))
                continue
            _unknown_keys(v, _BP_VIP, vp, issues)
            _check_ip(v.get('ip'), f'{vp}.ip', issues)
            prefix = v.get('prefix', 24)
            if not isinstance(prefix, int) or isinstance(prefix, bool) or not (1 <= prefix <= 32):
                issues.append(Issue('error', f'{vp}.prefix', f'1~32 정수 (현재 {prefix!r})'))
            if not v.get('interface'):
                issues.append(Issue('error', f'{vp}.interface',
                                    'VIP 를 올릴 인터페이스명 필수 (예: eth1)'))
            ip = v.get('ip')
            if ip:
                if ip in seen_vips:
                    issues.append(Issue('error', f'{vp}.ip',
                                        f"VIP '{ip}' 가 {seen_vips[ip]} 와 중복"))
                else:
                    seen_vips[ip] = vp
            # slot = VIP 의 용도 라벨. OAM vip_bindings 가 이 키로 VIP↔NIC 를 잇고,
            # 빈 slot 은 keepalived 렌더에서 조용히 버려지므로 기본값을 채운다.
            vips.append({'ip': ip, 'prefix': prefix, 'interface': v.get('interface'),
                         'slot': (v.get('slot') or 'service')})

        if mode == 'standalone' and vips:
            issues.append(Issue('warning', f'{p}.vips',
                                'standalone 시스템에는 VIP 가 적용되지 않는다'))
        # A/S 에 VIP 가 없는 것은 지적하지 않는다 — VIP 구성은 콘솔
        # [시스템/서버 구성] 의 일이고, 설치만 하는 블루프린트에서는 없는 게 정상이다.
        # (그룹만 만들고 keepalived 는 무장하지 않는 상태로 남는다.)

        failover = raw.get('failover')
        if failover is not None and not isinstance(failover, dict):
            issues.append(Issue('error', f'{p}.failover', '매핑이어야 함'))
            failover = None

        # ── HA 그룹 생성 여부 ────────────────────────────────────────
        # 그룹 생성은 **배포의 일**이다 — 콘솔 `＋ 시스템 추가` 가 하는 것과 같고,
        # 이게 없으면 A/S 로 선언한 서버들이 트리에서 SA 로 떨어진다.
        # 반면 VIP·절체조건은 그룹 탭에서 나중에 설정하는 값이라 여기서 요구하지 않는다.
        #   mode: standalone → 그룹 없음 (OAM ha-groups 가 AS/AA 만 받는다)
        #   ha_group: false  → 명시적으로 그룹 생성 생략
        auth_pass = raw.get('auth_pass')
        if auth_pass is not None:
            if not isinstance(auth_pass, str) or not auth_pass:
                issues.append(Issue('error', f'{p}.auth_pass', '문자열이어야 함'))
                auth_pass = None
            elif len(auth_pass) > 8:
                issues.append(Issue('error', f'{p}.auth_pass',
                                    f'keepalived VRRP 제한으로 8자 이하 (현재 {len(auth_pass)}자)'))

        ha_group = raw.get('ha_group')
        if ha_group is None:
            ha_group = (mode != 'standalone')
        elif not isinstance(ha_group, bool):
            issues.append(Issue('error', f'{p}.ha_group', f'true/false (현재 {ha_group!r})'))
            ha_group = (mode != 'standalone')

        # auth_pass 를 요구하지 않는다 — VRRP 인증값은 미지정 시 TOPOLOGY phase 가
        # 자동 생성하고, 운영자는 콘솔 그룹 탭에서 바꿀 수 있다. 블루프린트에 HA 값을
        # 적게 만들지 않는 것이 목적.

        # ── modules ──
        raw_modules = raw.get('modules')
        if not isinstance(raw_modules, list) or not raw_modules:
            issues.append(Issue('error', f'{p}.modules', '모듈을 1개 이상 정의해야 함'))
            raw_modules = []
        modules: list = []
        member_names = {m['server'] for m in members}
        seen_pkg: set = set()
        for j, m in enumerate(raw_modules):
            mp = f'{p}.modules[{j}]'
            if not isinstance(m, dict):
                issues.append(Issue('error', mp, '매핑이어야 함'))
                continue
            _unknown_keys(m, _BP_MODULE, mp, issues)

            pkg = m.get('package')
            if _check_name(pkg, f'{mp}.package', issues, '패키지명'):
                if pkg in seen_pkg:
                    issues.append(Issue('error', f'{mp}.package',
                                        f"같은 시스템에 패키지 '{pkg}' 중복"))
                seen_pkg.add(pkg)

            ver = m.get('version') or 'latest'
            if ver != 'latest' and not _VERSION_RE.match(str(ver)):
                issues.append(Issue('error', f'{mp}.version',
                                    f"'latest' 또는 N.N[.N[.N]] 형식 (현재 {ver!r})"))

            cfg = m.get('config')
            if cfg is not None and not isinstance(cfg, dict):
                issues.append(Issue('error', f'{mp}.config', '매핑이어야 함')); cfg = None

            per = m.get('per_server')
            if per is not None and not isinstance(per, dict):
                issues.append(Issue('error', f'{mp}.per_server', '매핑이어야 함'))
                per = None
            elif per:
                for sn, sv in per.items():
                    if sn not in member_names:
                        issues.append(Issue('error', f'{mp}.per_server.{sn}',
                                            f"'{sn}' 은 이 시스템의 멤버가 아님 "
                                            f"(멤버: {', '.join(sorted(member_names)) or '없음'})"))
                    if not isinstance(sv, dict):
                        issues.append(Issue('error', f'{mp}.per_server.{sn}', '매핑이어야 함'))

            cols = m.get('collections')
            if cols is not None and not isinstance(cols, dict):
                issues.append(Issue('error', f'{mp}.collections', '매핑이어야 함'))
                cols = None
            elif cols:
                for cn, rows in cols.items():
                    if not isinstance(rows, list):
                        issues.append(Issue('error', f'{mp}.collections.{cn}',
                                            '행 배열이어야 함'))
                        continue
                    for k, row in enumerate(rows):
                        if not isinstance(row, dict):
                            issues.append(Issue('error', f'{mp}.collections.{cn}[{k}]',
                                                '매핑이어야 함'))

            # start: false — 데몬이 아니거나 수동 기동할 모듈은 START phase 에서 제외.
            start = m.get('start')
            if start is None:
                start = True
            elif not isinstance(start, bool):
                issues.append(Issue('error', f'{mp}.start', f'true/false (현재 {start!r})'))
                start = True

            modules.append(Module(
                package=pkg, version=str(ver), process_name=m.get('process_name'),
                config=cfg or {}, per_server=per or {}, collections=cols or {},
                start=start))

        systems.append(System(name=sname, mode=mode, vips=vips,
                              failover=failover, members=members, modules=modules,
                              ha_group=ha_group, auth_pass=auth_pass))

    # ── start_order ──
    start_order = doc.get('start_order')
    if start_order is None:
        start_order = []
    elif not isinstance(start_order, list):
        issues.append(Issue('error', 'start_order', '시스템 이름 배열이어야 함'))
        start_order = []
    else:
        for i, n in enumerate(start_order):
            if n not in seen_sys:
                issues.append(Issue('error', f'start_order[{i}]',
                                    f"정의되지 않은 시스템 '{n}'"))

    if any(i.level == 'error' for i in issues):
        return None, issues
    return Blueprint(doc.get('name'), doc.get('description') or '',
                     systems, start_order), issues


# ──────────────────────────────────────────────────────────────
#  교차 검증
# ──────────────────────────────────────────────────────────────

def normalize_inventory_doc(view: dict) -> dict:
    """콘솔 구성 뷰가 돌려준 인벤토리 표현 → **스키마 유효 문서**.

    `Server.as_dict()` 는 화면용이라 파생 필드(auth_mode)와 미설정 None 을 포함한다.
    그대로 YAML 로 뽑으면 파서가 '알 수 없는 키'/빈 값으로 거부하므로, 저장 직전에
    허용 키만 남기고 빈 값을 떨어뜨린다.
    """
    def _clean(d: dict, allowed: set) -> dict:
        out = {}
        for k, v in (d or {}).items():
            if k not in allowed or v is None or v == '' or v is False:
                continue
            out[k] = v
        return out

    servers = []
    for s in (view or {}).get('servers') or []:
        row = {k: v for k, v in s.items()
               if k in _INV_SERVER and v is not None and v != ''}
        ssh = _clean(s.get('ssh') or {}, _INV_SSH)
        sudo = _clean(s.get('sudo') or {}, _INV_SUDO)
        if ssh:
            row['ssh'] = ssh
        else:
            row.pop('ssh', None)
        if sudo:
            row['sudo'] = sudo
        else:
            row.pop('sudo', None)
        servers.append(row)
    return {'version': SUPPORTED_VERSION, 'servers': servers}


def cross_validate(bp: Blueprint, inv: Inventory) -> list:
    """blueprint 가 참조한 서버가 inventory 에 있는지 — 두 문서를 잇는 유일한 계약.

    반환 Issue 의 path 는 **이미 문서 접두(blueprint:/inventory:)가 붙은 형태**다.
    지적이 어느 문서에 속하는지가 항목마다 다르기 때문(참조 실패는 blueprint 쪽,
    미사용 서버는 inventory 쪽).
    """
    issues: list = []
    known = set(inv.names())
    for si, s in enumerate(bp.systems):
        for mi, m in enumerate(s.members):
            if m['server'] not in known:
                issues.append(Issue('error',
                                    f'blueprint:systems[{si}].members[{mi}].server',
                                    f"'{m['server']}' 가 인벤토리에 없음 "
                                    f"(인벤토리 보유: {', '.join(sorted(known)) or '없음'})"))
    # 미사용 서버 지적 — 단 agent_preinstalled(= OAM 부트스트랩 노드)는 제외한다.
    # 그 노드는 서비스 모듈을 얹지 않는 게 통상이라, 포함하면 사실상 모든 인벤토리에서
    # 경고가 떠 신호가 아니라 잡음이 된다.
    unused = sorted(n for n in (known - set(bp.referenced_servers()))
                    if not getattr(inv.get(n), 'agent_preinstalled', False))
    if unused:
        issues.append(Issue('warning', 'inventory:servers',
                            f"블루프린트가 쓰지 않는 서버: {', '.join(unused)} "
                            f'— agent 도 설치되지 않는다'))
    return issues


def validate(blueprint_text: str, inventory_text: str) -> dict:
    """콘솔 /validate 용 진입점. 두 문서를 함께 검증해 지적 목록을 반환한다."""
    out = {'ok': False, 'issues': [], 'blueprint': None, 'inventory': None}
    try:
        bp, bp_issues = parse_blueprint(blueprint_text)
    except ParseError as e:
        out['issues'].append({'level': 'error', 'path': 'blueprint',
                              'message': f'YAML 문법 오류: {e.message}'
                                         + (f' (line {e.line})' if e.line else '')})
        bp, bp_issues = None, []
    try:
        inv, inv_issues = parse_inventory(inventory_text)
    except ParseError as e:
        out['issues'].append({'level': 'error', 'path': 'inventory',
                              'message': f'YAML 문법 오류: {e.message}'
                                         + (f' (line {e.line})' if e.line else '')})
        inv, inv_issues = None, []

    for i in bp_issues:
        d = i.as_dict(); d['path'] = f"blueprint:{d['path']}"; out['issues'].append(d)
    for i in inv_issues:
        d = i.as_dict(); d['path'] = f"inventory:{d['path']}"; out['issues'].append(d)

    if bp and inv:
        # cross_validate 는 문서 접두를 스스로 붙여 반환한다 — 재접두하지 않는다.
        out['issues'].extend(i.as_dict() for i in cross_validate(bp, inv))
        out['blueprint'] = bp.as_dict()
        out['inventory'] = inv.as_dict(mask=True)

    out['ok'] = not any(i['level'] == 'error' for i in out['issues'])
    return out
