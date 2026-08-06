"""TOPOLOGY phase — HA 그룹 생성 + 멤버 편입 + VIP + 절체조건.

블루프린트의 `systems[]` 를 OAM 의 ha_group 으로 실체화한다.

그룹 생성은 콘솔 `＋ 시스템 추가` 와 같은 일이다 — 이걸 하지 않으면 A/S 로 선언한 서버들이
트리에서 SA(standalone)로 떨어진다. 반면 **VIP·절체조건은 만들지 않는다**: 그것들은 그룹
생성 후 콘솔 [시스템/서버 구성] 에서 설정하는 값이다.

`auth_pass`(keepalived VRRP 인증, 8자)는 OAM 이 active_standby 그룹 생성에 요구하지만
블루프린트에 적게 하지 않는다 — 미지정이면 여기서 생성하고, 운영자가 그룹 탭에서 바꾼다.
기존 그룹을 재사용할 때는 그 그룹의 값을 보존한다(재생성으로 keepalived 를 흔들지 않기 위해).

주의할 매핑 두 가지:
  1. **standalone 은 HA 그룹이 아니다.** OAM `POST /api/v1/ha-groups` 는 mode 를
     active_standby | all_active 로만 받는다. 단일 노드 시스템은 그룹 없이 agent 에
     바로 배포하므로 이 phase 에서 skip 한다.
  2. **role → priority.** OAM 은 role 을 저장하지 않고 priority(master 100 / backup 90)로
     환산해 보관한다. 여기서는 role 을 그대로 넘기고 OAM 의 `_normalize_member` 가
     환산하게 둔다 — 환산 규칙을 두 곳에 두지 않기 위함.

멱등: 같은 이름의 그룹이 이미 있으면 재사용하고, 달라진 필드만 PUT 한다.
"""

from __future__ import annotations

import secrets

from ..oam_client import OamError

KEY = 'TOPOLOGY'
TITLE = '시스템(HA 그룹) 구성'
SERIAL = False


def _vip_bindings(system, agent_ids: dict) -> list:
    """블루프린트 vips[] → OAM vip_bindings[].

    blueprint 는 VIP 당 인터페이스 1개를 쓰므로 전 멤버에 같은 iface 를 매핑한다.
    멤버마다 NIC 이름이 다른 사이트는 콘솔 [시스템/서버 구성] 에서 조정한다.
    """
    out = []
    for v in system.vips or []:
        iface = v.get('interface')
        out.append({
            'slot': v.get('slot') or 'service',
            'ip': v['ip'],
            'mask': v.get('prefix', 24),
            'memberIfaces': {str(aid): iface for aid in agent_ids.values()} if iface else {},
        })
    return out


def _auto_auth_pass(system, existing) -> str:
    """VRRP auth_pass 결정. active_standby 만 의미 있다(AA 는 keepalived 미사용).

    우선순위: 블루프린트 명시 → 기존 그룹 값 보존 → 자동 생성.
    자동 생성이라도 운영자가 콘솔 그룹 탭에서 언제든 바꿀 수 있다.
    """
    if system.mode != 'active_standby':
        return ''
    if system.auth_pass:
        return system.auth_pass
    if existing and (existing.get('auth_pass') or '').strip():
        return existing['auth_pass']          # 기존 값 보존 — keepalived 재적용 방지
    # 8자 제한(keepalived) — url-safe 문자에서 생성
    return secrets.token_urlsafe(8).replace('-', 'x').replace('_', 'y')[:8]


def _desired(system, agent_ids: dict, auth_pass: str) -> dict:
    vips = system.vips or []
    body = {
        'name': system.name,
        'mode': system.mode,
        'members': [{'agent_id': agent_ids[m['server']], 'role': m.get('role')}
                    for m in system.master_first()],
        'vip_bindings': _vip_bindings(system, agent_ids),
        'note': 'provisioner',
    }
    if vips:
        # 대표 VIP — OAM 은 단일 vip 필드도 함께 보관한다(구 UI·keepalived 기본값).
        body['vip'] = vips[0]['ip']
        body['vip_mask'] = vips[0].get('prefix', 24)
    if system.failover:
        body['failover_options'] = system.failover
    if auth_pass:
        body['auth_pass'] = auth_pass          # keepalived VRRP 인증 (8자 이하)
    return body


def plan(ctx) -> list:
    steps = []
    for s in ctx.blueprint.systems:
        if not s.ha_group:
            steps.append({'target': s.name,
                          'action': 'skip (ha_group: false)',
                          'mode': s.mode, 'members': len(s.members)})
            continue
        if s.mode == 'standalone':
            steps.append({'target': s.name, 'action': 'skip (standalone — HA 그룹 불필요)',
                          'mode': s.mode, 'members': len(s.members)})
            continue
        steps.append({
            'target': s.name,
            'action': 'HA 그룹 생성/갱신',
            'mode': s.mode,
            'members': ','.join(m['server'] for m in s.master_first()),
            'vips': ','.join(v['ip'] for v in (s.vips or [])) or '-',
        })
    return steps


def execute(ctx, step) -> dict:
    system = ctx.blueprint.system(step['target'])
    if system is None:
        raise OamError('system_not_found', f"블루프린트에 '{step['target']}' 없음")

    if not system.ha_group:
        return {'status': 'skipped', 'detail': 'ha_group: false — 그룹을 만들지 않는다'}
    if system.mode == 'standalone':
        return {'status': 'skipped', 'detail': 'standalone — HA 그룹을 만들지 않는다'}

    # 멤버 agent id 해석 — AGENT phase 가 끝났으므로 전부 존재해야 한다.
    agent_ids: dict = {}
    for m in system.members:
        a = ctx.oam.find_agent(m['server'])
        if not a:
            raise OamError('agent_not_found',
                           f"'{m['server']}' agent 레코드가 없음 — AGENT phase 확인")
        agent_ids[m['server']] = a['id']

    existing = ctx.oam.find_ha_group(system.name)
    body = _desired(system, agent_ids, _auto_auth_pass(system, existing))

    if existing is None:
        g = ctx.oam.create_ha_group(body)
        gid = (g or {}).get('id')
        ctx.record_created('ha_group', gid, system.name)
        return {'status': 'done',
                'detail': f"그룹#{gid} 생성 — {system.mode}, 멤버 {len(agent_ids)}, "
                          f"VIP {len(body.get('vip_bindings') or [])}개"}

    gid = existing['id']
    if existing.get('mode') != system.mode:
        raise OamError(
            'ha_mode_conflict',
            f"기존 그룹#{gid} '{system.name}' 의 mode 가 {existing.get('mode')} 인데 "
            f'블루프린트는 {system.mode} — 모드 변경은 자동으로 하지 않는다'
            f'(멤버/VIP 재구성이 필요하므로 콘솔에서 처리)')

    # 달라진 것만 갱신 — 운영자가 콘솔에서 손댄 값을 불필요하게 되돌리지 않는다.
    cur_members = sorted(int(m.get('agent_id')) for m in (existing.get('members') or []))
    new_members = sorted(agent_ids.values())
    cur_vips = [(b.get('slot'), b.get('ip')) for b in (existing.get('vip_bindings') or [])]
    new_vips = [(b['slot'], b['ip']) for b in body.get('vip_bindings') or []]

    if cur_members == new_members and cur_vips == new_vips:
        return {'status': 'skipped',
                'detail': f'그룹#{gid} 이미 일치 (멤버 {len(new_members)}, VIP {len(new_vips)})'}

    ctx.oam.update_ha_group(gid, body)
    return {'status': 'done',
            'detail': f'그룹#{gid} 갱신 — 멤버 {len(new_members)}, VIP {len(new_vips)}'}
