"""INSTALL phase — deployment 생성 + install job 큐잉 + 완료 대기.

step 단위는 **(시스템, 모듈, 서버)** 하나 = deployment 하나다. 같은 모듈이 A/S 두 멤버에
깔리면 step 도 2개 — 한 멤버 실패가 다른 멤버를 막지 않게 하기 위함이다.

패키지 버전 pin: 블루프린트의 `version: latest` 는 **run 첫 실행 시** 저장소 최신으로
확정해 run 레코드에 남긴다. resume 이 며칠 뒤여도 같은 버전이 깔린다.

멱등: 같은 (agent, package) 배포가 이미 있고 install_path 가 잡혀 있으면 재설치하지 않는다.
"""

from __future__ import annotations

from ..oam_client import OamError

KEY = 'INSTALL'
TITLE = '패키지 설치'
SERIAL = False


def _targets(ctx):
    """(system, module, server_name) 전개 — plan 과 execute 가 같은 순서를 본다."""
    for s in ctx.blueprint.systems:
        for mod in s.modules:
            for m in s.master_first():
                yield s, mod, m['server']


def _target_key(system, module, server: str) -> str:
    return f'{server}/{module.package}'


def _pinned(ctx, module) -> str:
    """run 레코드에 고정된 버전. 없으면 지금 확정해 기록한다."""
    pins = ctx.run.setdefault('package_pins', {})
    key = f'{module.package}@{module.version}'
    if key not in pins:
        pkg = ctx.oam.resolve_package(module.package, module.version)
        pins[key] = {'version': str(pkg['version']), 'package_id': pkg['id']}
    return pins[key]


def plan(ctx) -> list:
    steps = []
    for s, mod, srv in _targets(ctx):
        steps.append({
            'target': _target_key(s, mod, srv),
            'action': '설치',
            'system': s.name,
            'server': srv,
            'package': mod.package,
            'version': mod.version,
            'process': mod.process_name or mod.package.upper(),
        })
    return steps


def _lookup(ctx, target: str):
    for s, mod, srv in _targets(ctx):
        if _target_key(s, mod, srv) == target:
            return s, mod, srv
    raise OamError('step_target_unknown', f"'{target}' 에 해당하는 모듈 배포가 없음")


def execute(ctx, step) -> dict:
    system, module, server = _lookup(ctx, step['target'])

    a = ctx.oam.find_agent(server)
    if not a:
        raise OamError('agent_not_found', f"'{server}' agent 레코드가 없음")
    agent_id = a['id']

    pin = _pinned(ctx, module)
    version, package_id = pin['version'], pin['package_id']
    process_name = module.process_name or module.package.upper()

    existing = ctx.oam.find_deployment(agent_id, module.package)
    if existing and existing.get('install_path') \
            and str(existing.get('package_version') or '') == version:
        return {'status': 'skipped',
                'detail': f"배포#{existing['id']} 이미 {version} 설치됨",
                'deployment_id': existing['id'], 'version': version}

    if existing:
        did = existing['id']
        # 버전만 다른 경우 — 같은 배포를 재설치(업그레이드)로 몰아 install_history 를 잇는다.
        ctx.oam.update_deployment(did, {'package_id': package_id,
                                        'config': module.config_for(server)})
    else:
        d = ctx.oam.create_deployment(
            agent_id=agent_id, package_id=package_id, process_name=process_name,
            config=module.config_for(server))
        did = (d or {}).get('id')
        if not did:
            raise OamError('deployment_create_failed', f'배포 생성 응답에 id 없음: {d}')
        ctx.record_created('deployment', did, f'{server}/{module.package}')

    timeout = int(ctx.run_cfg.get('JobTimeoutSec', 600) or 600)
    poll = int(ctx.run_cfg.get('JobPollIntervalSec', 2) or 2)
    j = ctx.oam.run_job(deployment_id=did, agent_id=agent_id, job_type='install',
                        timeout_sec=timeout, poll_sec=poll)

    return {'status': 'done',
            'detail': f'배포#{did} {module.package} {version} 설치 완료',
            'deployment_id': did, 'job_id': j.get('job_id'), 'version': version}
