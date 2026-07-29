"""START phase — 선언된 순서로 프로세스를 기동한다.

**이 phase 만 직렬이다.** 두 가지 순서를 지켜야 하기 때문:
  1. 시스템 간 — `start_order` (예: CMP 가 CSP 보다 먼저. CSP 는 기동 시 미디어 서버를
     붙잡으므로 역순이면 초기 호가 실패한다)
  2. A/S 멤버 간 — master 를 먼저. keepalived 가 무장되는 순간 VIP 를 master 가 선점해야
     한다. backup 이 먼저 뜨면 VIP 가 backup 에 붙은 채로 시작해 절체 상태가 뒤집힌다.

plan() 이 이미 이 순서로 step 을 뱉으므로 엔진은 리스트 순서대로 실행하기만 하면 된다.
"""

from __future__ import annotations

from ..oam_client import OamError

KEY = 'START'
TITLE = '서비스 기동'
SERIAL = True          # start_order · master 우선 — 병렬 금지


def _targets(ctx):
    """기동 순서대로 (system, module, server) 전개."""
    for s in ctx.blueprint.systems_in_start_order():
        for mod in s.modules:
            if not mod.start:
                continue
            for m in s.master_first():
                yield s, mod, m['server']


def _target_key(module, server: str) -> str:
    return f'{server}/{module.package}'


def plan(ctx) -> list:
    steps = []
    for i, (s, mod, srv) in enumerate(_targets(ctx)):
        role = next((m.get('role') for m in s.members if m['server'] == srv), None)
        steps.append({
            'target': _target_key(mod, srv),
            'action': '기동',
            'order': i + 1,
            'system': s.name,
            'server': srv + (f' ({role})' if role else ''),
            'package': mod.package,
        })
    return steps


def _lookup(ctx, target: str):
    for s, mod, srv in _targets(ctx):
        if _target_key(mod, srv) == target:
            return s, mod, srv
    raise OamError('step_target_unknown', f"'{target}' 에 해당하는 기동 대상이 없음")


def execute(ctx, step) -> dict:
    system, module, server = _lookup(ctx, step['target'])

    a = ctx.oam.find_agent(server)
    if not a:
        raise OamError('agent_not_found', f"'{server}' agent 레코드가 없음")
    agent_id = a['id']

    dep = ctx.oam.find_deployment(agent_id, module.package)
    if not dep:
        raise OamError('deployment_not_found',
                       f"{server}/{module.package} 배포가 없음 — INSTALL phase 확인")
    did = dep['id']

    # 이미 running 이면 재기동하지 않는다 — resume 이 서비스를 끊지 않게.
    if dep.get('status') == 'running':
        return {'status': 'skipped', 'detail': f'배포#{did} 이미 running',
                'deployment_id': did}

    timeout = int(ctx.run_cfg.get('JobTimeoutSec', 600) or 600)
    poll = int(ctx.run_cfg.get('JobPollIntervalSec', 2) or 2)
    j = ctx.oam.run_job(deployment_id=did, agent_id=agent_id, job_type='start',
                        timeout_sec=timeout, poll_sec=poll)

    return {'status': 'done',
            'detail': f'배포#{did} {module.package} 기동 ({system.name})',
            'deployment_id': did, 'job_id': j.get('job_id')}
