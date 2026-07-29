"""VERIFY phase — 배포 결과가 실제로 서비스 가능한 상태인지 확인한다.

step 단위는 **시스템 1개**다. 모듈별 health_check 결과와 (A/S 라면) VIP 실보유 상태를
한 줄로 묶어 보여주는 편이, 운영자가 "이 시스템은 올라왔나"를 판단하기 쉽다.

확인 항목:
  1. 각 모듈의 `health_check` job — agent 가 프로세스/포트를 실측
  2. A/S 시스템의 실측 ACTIVE — OAM 이 heartbeat 의 interfaces[] 로 VIP 보유자를 판정
     (`active_agent_id`). 절체 직후 관측 창에서는 None 이 될 수 있어 **경고**로만 다룬다.

이 phase 는 아무것도 바꾸지 않는다. 실패해도 배포물은 그대로 남으므로, 조사 후 [재개]로
검증만 다시 돌릴 수 있다.
"""

from __future__ import annotations

from ..oam_client import OamError

KEY = 'VERIFY'
TITLE = '검증'
SERIAL = False

# VIP 판정은 heartbeat(2s) 가 몇 번 돌아야 안정된다. 기동 직후엔 아직 None 일 수 있어
# 이 phase 에서는 실패로 취급하지 않는다.
_VIP_GRACE_NOTE = 'heartbeat 관측 창 — 콘솔 [시스템/서버 구성] 에서 ACTIVE 뱃지 확인'


def plan(ctx) -> list:
    steps = []
    for s in ctx.blueprint.systems:
        mods = [m.package for m in s.modules]
        steps.append({
            'target': s.name,
            'action': '헬스·VIP 확인',
            'mode': s.mode,
            'modules': ','.join(mods),
            'members': ','.join(m['server'] for m in s.members),
        })
    return steps


def execute(ctx, step) -> dict:
    system = ctx.blueprint.system(step['target'])
    if system is None:
        raise OamError('system_not_found', f"블루프린트에 '{step['target']}' 없음")

    timeout = int(ctx.run_cfg.get('JobTimeoutSec', 600) or 600)
    poll = int(ctx.run_cfg.get('JobPollIntervalSec', 2) or 2)

    ok, bad = [], []
    for m in system.members:
        server = m['server']
        a = ctx.oam.find_agent(server)
        if not a:
            bad.append(f'{server}: agent 레코드 없음')
            continue
        for mod in system.modules:
            if not mod.start:
                continue                      # 기동하지 않은 모듈은 헬스 대상 아님
            dep = ctx.oam.find_deployment(a['id'], mod.package)
            if not dep:
                bad.append(f'{server}/{mod.package}: 배포 없음')
                continue
            try:
                ctx.oam.run_job(deployment_id=dep['id'], agent_id=a['id'],
                                job_type='health_check', timeout_sec=timeout, poll_sec=poll)
                ok.append(f'{server}/{mod.package}')
            except OamError as e:
                bad.append(f'{server}/{mod.package}: {e.message}')

    detail = f'헬스 {len(ok)}/{len(ok) + len(bad)}'

    # A/S — 실측 ACTIVE 확인 (경고 수준)
    warn = ''
    if system.mode == 'active_standby':
        g = ctx.oam.find_ha_group(system.name)
        if g:
            full = ctx.oam.get(f"/api/v1/ha-groups/{g['id']}") or {}
            aid = full.get('active_agent_id')
            if aid:
                holder = next((m['server'] for m in system.members
                               if (ctx.oam.find_agent(m['server']) or {}).get('id') == aid),
                              f'agent#{aid}')
                detail += f' · ACTIVE={holder}'
            else:
                warn = f' · ACTIVE 미확정 ({_VIP_GRACE_NOTE})'

    if bad:
        raise OamError('verify_failed',
                       f"{system.name} 검증 실패 — {detail}{warn} | " + ' ; '.join(bad[:4]))
    return {'status': 'done', 'detail': detail + warn}
