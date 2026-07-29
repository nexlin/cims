"""CONFIG phase — 배포 overlay(config.json) + collection(jsonl) 주입.

두 종류의 설정이 저장 위치가 다르다 (02_deployment.md §4):
  - **scalar overlay** → deployment.config (OAM DB) → `update_config` job 이 노드에 렌더
  - **collection**     → 노드의 `install_path/config/*.jsonl` (OAM 은 프록시만)

overlay 는 INSTALL 단계에서 이미 배포 레코드에 실려 있다. 여기서는 그 값이 노드 파일에
실제로 반영되도록 `update_config` job 을 돌리고, collection 을 밀어 넣는다.

collection PUT 은 배포가 **설치된 뒤에만** 받는다(OAM 이 미설치 배포에 409 not_installed).
그래서 이 phase 는 INSTALL 뒤에 온다.
"""

from __future__ import annotations

from ..oam_client import OamError

KEY = 'CONFIG'
TITLE = '설정 주입'
SERIAL = False


def _targets(ctx):
    for s in ctx.blueprint.systems:
        for mod in s.modules:
            for m in s.master_first():
                yield s, mod, m['server']


def _target_key(module, server: str) -> str:
    return f'{server}/{module.package}'


def plan(ctx) -> list:
    steps = []
    for s, mod, srv in _targets(ctx):
        overlay = mod.config_for(srv)
        cols = mod.collections or {}
        steps.append({
            'target': _target_key(mod, srv),
            'action': '설정 적용',
            'server': srv,
            'package': mod.package,
            'overlay_keys': len(overlay),
            'collections': ','.join(f'{k}({len(v)})' for k, v in cols.items()) or '-',
        })
    return steps


def _lookup(ctx, target: str):
    for s, mod, srv in _targets(ctx):
        if _target_key(mod, srv) == target:
            return s, mod, srv
    raise OamError('step_target_unknown', f"'{target}' 에 해당하는 모듈이 없음")


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
    if not dep.get('install_path'):
        raise OamError('not_installed',
                       f'배포#{did} 가 아직 설치되지 않음 — collection 을 쓸 수 없다')

    timeout = int(ctx.run_cfg.get('JobTimeoutSec', 600) or 600)
    poll = int(ctx.run_cfg.get('JobPollIntervalSec', 2) or 2)
    done = []

    # 1. scalar overlay — 배포 레코드를 맞추고 노드 파일에 렌더
    overlay = module.config_for(server)
    if overlay:
        cur = dep.get('config') or {}
        if any(cur.get(k) != v for k, v in overlay.items()):
            merged = dict(cur)
            merged.update(overlay)
            ctx.oam.update_deployment(did, {'config': merged})
        ctx.oam.run_job(deployment_id=did, agent_id=agent_id, job_type='update_config',
                        timeout_sec=timeout, poll_sec=poll)
        done.append(f'overlay {len(overlay)}키')

    # 2. collection — 노드의 jsonl 에 직접 (SIGUSR1 로 즉시 반영)
    for name, records in (module.collections or {}).items():
        try:
            ctx.oam.put_collection(did, name, records, signal=True)
        except OamError as e:
            raise OamError('collection_put_failed',
                           f"배포#{did} collection '{name}' 저장 실패 — {e.message}") from None
        done.append(f'{name} {len(records)}행')

    if not done:
        return {'status': 'skipped', 'detail': '적용할 설정 없음', 'deployment_id': did}
    return {'status': 'done', 'detail': ' · '.join(done), 'deployment_id': did}
