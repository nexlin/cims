"""AGENT phase — SSH 로 각 노드에 agent 를 설치하고 enroll 완료까지 확인한다.

자동 배포에서 유일하게 SSH 를 쓰는 단계다. 이후 phase(TOPOLOGY/INSTALL/CONFIG/START/
VERIFY)는 전부 OAM REST 로 동작한다.

서버 1대의 흐름:
  1. enrollment 토큰 확보  (POST /api/v1/agents — 이미 online 이면 skip)
  2. install-agent.sh 전송 (scp → /tmp)
  3. sudo 로 실행           (--oam-url --enrollment-token --name --install-dir --svc-user)
  4. online 전이 대기       (heartbeat 도달)

토큰 TTL 이 짧아(기본 10분) **서버마다 설치 직전에 발급**한다 — run 시작 시 전부 미리
발급하면 뒤쪽 서버에서 만료된다.

install-agent.sh 는 이미 완전 비대화형이라 스크립트를 고치지 않는다
(`agent/install-agent.sh` 의 인자 파싱 참조).
"""

from __future__ import annotations

import os
import shlex

from .. import ssh as sshmod
from ..oam_client import OamError

KEY = 'AGENT'
TITLE = 'agent 설치'
SERIAL = False          # 서버 간 독립 — 병렬

_REMOTE_SCRIPT = '/tmp/cims-install-agent.sh'


def _installer_path(ctx) -> str:
    """동봉된 install-agent.sh 위치.

    설치본에서는 provisioner 패키지에 동봉된 사본을, 개발 트리에서는 레포의 `agent/` 를 쓴다.
    """
    cand = []
    explicit = (ctx.config.get('AgentInstallerPath') or '').strip()
    if explicit:
        cand.append(explicit)
    # .../src/services/provision/phases/agent.py → 위로 4단계 = oam 컴포넌트 루트
    here = os.path.abspath(__file__)
    for _ in range(5):
        here = os.path.dirname(here)                     # phases → provision → services → src → oam
    cand.append(os.path.join(here, 'assets', 'install-agent.sh'))    # oam 패키지 동봉본
    # dev(레포 트리): ems/core/oam → 레포 루트의 agent/
    cand.append(os.path.join(here, '..', '..', '..', 'agent', 'install-agent.sh'))
    for p in cand:
        p = os.path.normpath(p)
        if os.path.isfile(p):
            return p
    raise FileNotFoundError(
        'install-agent.sh 를 찾을 수 없음 — provisioner/assets/ 에 동봉되어야 한다 '
        f"(탐색: {', '.join(os.path.normpath(c) for c in cand)})")


def _enroll_url(ctx) -> str:
    """대상 노드의 agent 가 실제로 닿을 수 있는 OAM 주소.

    provisioner 는 OAM 을 loopback 으로 호출할 수 있지만, 원격 노드에게 127.0.0.1 을
    알려주면 enroll 이 자기 자신을 향한다. AgentEnrollUrl 이 있으면 그것을 쓴다.
    """
    url = (ctx.config.get('AgentEnrollUrl') or '').strip()
    if url:
        return url.rstrip('/')
    url = (ctx.config.get('OamUrl') or '').rstrip('/')
    return url


def plan(ctx) -> list:
    steps = []
    for name in ctx.blueprint.referenced_servers():
        srv = ctx.inventory.get(name)
        pre = bool(srv and srv.agent_preinstalled)
        steps.append({
            'target': name,
            'host': srv.host if srv else '?',
            'auth': srv.auth_mode if srv else '?',
            'install_dir': srv.install_dir if srv else '',
            'action': 'agent 이미 설치됨 — 상태만 확인' if pre else 'agent 설치 + enroll',
        })
    return steps


def execute(ctx, step) -> dict:
    name = step['target']
    srv = ctx.inventory.get(name)
    if srv is None:
        raise OamError('server_not_in_inventory', f"'{name}' 이 인벤토리에 없음")

    # agent_preinstalled — SSH 하지 않고 OAM 레코드 상태만 확인한다.
    # OAM(부트스트랩) 노드가 항상 이 경우다 (install.sh 가 로컬 agent 를 이미 enroll).
    if srv.agent_preinstalled:
        a = ctx.oam.find_agent(name)
        if not a:
            raise OamError(
                'preinstalled_agent_missing',
                f"'{name}' 이 agent_preinstalled 인데 OAM 에 그 이름의 agent 가 없다 "
                f'— 인벤토리의 논리명이 실제 등록명과 같은지 확인 '
                f'(콘솔 [시스템/서버 구성] 의 서버 이름)')
        if a.get('status') != 'online':
            raise OamError(
                'preinstalled_agent_offline',
                f"'{name}' agent#{a.get('id')} 상태가 {a.get('status')} — online 이어야 한다. "
                f'대상 노드에서 `systemctl --user status cims-agent` 확인')
        return {'status': 'skipped',
                'detail': f"기설치 — agent#{a.get('id')} online "
                          f"(v{a.get('agent_version') or '?'})",
                'agent_id': a.get('id')}

    enroll_url = _enroll_url(ctx)
    if not enroll_url:
        raise OamError('oam_url_missing',
                       'OamUrl / AgentEnrollUrl 미설정 — agent 가 enroll 할 주소가 없다')

    # 1. 토큰 확보 (이미 online 이면 설치 자체를 건너뛴다 — 멱등)
    agent_id, token, already = ctx.oam.ensure_enrollment_token(name)
    if already:
        return {'status': 'skipped', 'detail': f'이미 online (agent#{agent_id})',
                'agent_id': agent_id}

    runtime_dir = ctx.config.get('_runtime_dir')
    local_script = _installer_path(ctx)

    # 2~3. 전송 + 설치
    with sshmod.SshTarget(srv, **ctx.ssh_kwargs(runtime_dir)) as t:
        t.put(local_script, _REMOTE_SCRIPT, mode='0755')
        args = [
            'bash', _REMOTE_SCRIPT,
            '--oam-url', enroll_url,
            '--enrollment-token', token,
            '--name', name,
            '--install-dir', srv.install_dir,
            '--svc-user', srv.svc_user,
        ]
        cmd = ' '.join(shlex.quote(a) for a in args)
        try:
            r = t.run(cmd, sudo=True)
        finally:
            # 토큰이 박힌 스크립트 사본을 원격에 남기지 않는다.
            try:
                t.run(f'rm -f {shlex.quote(_REMOTE_SCRIPT)}')
            except sshmod.SshError:
                pass

        if r.rc != 0:
            tail = (r.stderr.strip() or r.stdout.strip() or '').splitlines()
            raise sshmod.SshError(
                'agent_install_failed',
                f'{name}: install-agent.sh rc={r.rc} — '
                + (' / '.join(tail[-3:]) if tail else '출력 없음'))

    # 4. enroll 대기
    timeout = int(ctx.run_cfg.get('EnrollTimeoutSec', 180) or 180)
    a = ctx.oam.wait_agent_online(name, timeout)
    ver = a.get('agent_version') or '?'
    return {'status': 'done',
            'detail': f"enrolled — agent#{a.get('id')} v{ver} ({srv.host})",
            'agent_id': a.get('id')}
