"""계획 미리보기 — `POST /runs/plan` 과 `POST /scenarios/compile-check` 가 같이 쓰는 compile_run 드라이런 (test_instrument.md §7).

run 을 걸지 않고 컴파일 결과를 사람이 읽는 꼴로 돌려준다: 역할→풀→워커·신원 창, 워커 배분(율·인스턴스·용량), 구간(prelude/body/
epilogue), 절차표, 대상 시드 예정 컬렉션, 필요한 환경변수, 예상 소요·피크 율·동시 세션(SDT), **Little 검산**(SApS × SDT ≤ 역할 신원 —
넘는 단계에서 slot skipped), 워커 SIP/RTP 용량 경고, 알려진 CSP 과제(§12). 워커 health 는 병렬 probe(6 s 상한) — probe=False 면 생략.
"""
from __future__ import annotations

import math
from typing import Dict, List, Optional

from services import tester_compile, tester_target, tester_workers
from services.tester_models import LoadProfile, Scenario, Topology, STEP_VOCAB

# §12 — 시험이 드러낸 CIMS 측 과제. 시나리오가 그 경로를 밟으면 계획 미리보기에 경고로 붙인다.
KNOWN_TARGET_ISSUES = [
    {'when': lambda sc, topo: any(s.cause for s in sc.flow), 'text': 'Reason: Q.850 은 CSP 가 상대 leg 에 복사하지 않는다(§12) — q850_rx_pct 기대치는 FAIL 이 예상된다'},
    {'when': lambda sc, topo: any(s.step == 'reject' and str(s.payload or '').startswith('5') for s in sc.flow),
     'text': '트렁크 5xx 거절은 발신자에 603 으로 매핑된다(§12) — 응답 코드 기대치를 확인한다'},
    {'when': lambda sc, topo: any(s.step == 'register' and any(getattr(topo.pools.get(sc.roles[r].pool, None), 'kind', '') == 'peer'
                                                              for r in (s.who or []) if r in sc.roles) for s in sc.flow),
     'text': '트렁크 REGISTER 는 CSP 가 계정을 받지 않아 403(§12) — 등록형 트렁크 시나리오는 attempts 0 으로 닫힌다'},
    {'when': lambda sc, topo: any(getattr(p, 'answer', '') == 'silent' for p in topo.pools.values()),
     'text': 'RouteSet 헬스체크가 없어 무응답 피어에서 Timer B(32 s)까지 기다린다(§12) — failover 는 실측 fail'},
]


def _sdt_seconds(scenario: Scenario, steps: List[dict], phases: Dict[str, List[int]]) -> float:
    """세션 지속 추정(SDT) — body 단계의 after_ms + seconds 합 + 시그널링 여유 1 s. 컴파일된(바인딩 해석·during 풀린) 단계로 센다."""
    body = set(phases.get('body') or [])
    sdt = 1.0
    for s in steps:
        if s.get('src') not in body:
            continue
        sdt += float(s.get('after_ms') or 0) / 1000.0
        sdt += float(s.get('seconds') or 0)
    return round(sdt, 1)


def _rates(profile: Optional[LoadProfile], rate_total: float, max_instances: Optional[int]) -> List[float]:
    if profile is None:
        return [float(rate_total)]
    if profile.model in ('constant', 'soak'):
        return [float(profile.rate)]
    if profile.model in ('step', 'ramp'):
        out = []
        r = float(profile.start)
        step = float(profile.step) if profile.model == 'step' and profile.step else max(1.0, (float(profile.max) - r) / 10)
        while r < float(profile.max) and len(out) < 60:
            out.append(round(r, 2))
            r += step
        out.append(float(profile.max))
        return out
    if profile.model == 'burst':
        return [float(profile.burst_size)]
    return [rate_total]


def estimate_duration(profile: Optional[LoadProfile], rate_total: float, max_instances: Optional[int], sdt: float) -> int:
    if profile is None:
        n = int(max_instances or 1)
        return int(math.ceil(n / max(0.1, rate_total)) + sdt + 5)
    if profile.model in ('constant', 'soak', 'burst'):
        return int(profile.duration_s) + int(sdt) + 5
    if profile.model == 'step':
        n = int((float(profile.max) - float(profile.start)) // float(profile.step)) + 1
        return n * int(profile.hold_s) + int(sdt) + 5
    if profile.model == 'ramp':
        return int(profile.ramp_s) + int(profile.hold_s) + int(sdt) + 5
    return 0


def procedure(scenario: Scenario, phases: Dict[str, List[int]]) -> List[dict]:
    """절차표 — 보고서(절차·예상 결과·확인 방법)와 같은 행: {idx, phase, step, actors, summary, expect}."""
    ph = {}
    for name, idxs in phases.items():
        for i in idxs:
            ph[i] = name
    rows = []
    for i, s in enumerate(scenario.flow):
        actors = [s.from_] if s.from_ else list(s.who or [])
        parts = []
        if s.from_ and s.to:
            parts.append(f'{s.from_} → {s.to}')
        elif actors:
            parts.append(', '.join(actors))
        if s.media:
            parts.append(f"{s.media.audio or ''}{'+' + s.media.video if s.media.video else ''}"
                         + (f' · rtp {s.media.rtp}' if s.media.rtp != 'auto' else ''))
        if s.step == 'media_send':
            parts.append(f"sample {s.sample}" if s.sample else '기본 원천')
            if s.loop is False:
                parts.append('한 번 재생')
        if s.after_ms:
            parts.append(f'after {s.after_ms} ms')
        if s.seconds is not None:
            parts.append(f'{s.seconds} s')
        if s.payload:
            parts.append(f'payload {s.payload}')
        if s.cause:
            parts.append(f'Q.850 {s.cause}')
        if s.during:
            parts.append('during ' + ', '.join(f'{d.at_s}s {d.step}' + (f'({d.sample})' if d.sample else '') for d in s.during))
        rows.append({'idx': i, 'phase': ph.get(i, 'body'), 'step': s.step, 'actors': actors, 'summary': ' · '.join(parts),
                     'expect': {k: (v if isinstance(v, (int, float)) else v.model_dump(exclude_none=True)) for k, v in (s.expect or {}).items()},
                     'desc': (STEP_VOCAB.get(s.step) or {}).get('desc')})
    return rows


def build_plan(scenario: Scenario, topology: Topology, topology_doc: dict, profile: Optional[LoadProfile],
               bindings: Dict[str, object], instances: Optional[int], rate_saps: Optional[float],
               probe: bool = True, stream_port: int = 7110) -> dict:
    """compile_run 드라이런 → 계획 미리보기 문서. 컴파일 오류는 errors[] 로(예외 아님). 워커에 아무것도 보내지 않는다."""
    out: dict = {'ok': True, 'errors': [], 'warnings': [], 'notes': [], 'scenario_id': scenario.id, 'topology': topology.name,
                 'profile': (profile.name if profile else None), 'target': {'name': topology.target.name, 'kind': topology.target.kind}}
    workers = tester_workers.discover(topology_doc)
    if probe:
        tester_workers.probe_all(workers)
    try:
        plan = tester_compile.compile_run('plan', scenario, topology, topology_doc, profile, bindings, workers,
                                          lambda w: f'<controller>:{stream_port}', instances, rate_saps)
    except tester_compile.CompileError as e:
        out['ok'] = False
        out['errors'].append(str(e))
        try:
            out['phases'] = tester_compile.phases(scenario)
            out['procedure'] = procedure(scenario, out['phases'])
        except Exception:
            pass
        out['workers'] = [{'name': w.name, 'host': w.host_id, 'url': w.url, 'up': w.health is not None, 'error': w.health_error,
                           'in_run': False} for w in workers]
        return out
    steps, phases = plan['steps'], plan['phases']
    sdt = _sdt_seconds(scenario, steps, phases)
    rates = _rates(profile, plan['rate_total'], plan['max_instances'])
    peak = max(rates) if rates else 0.0
    by_name = {w.name: w for w in workers}
    # 워커 행 — 배분·용량
    wrows = []
    for w in topology.workers:
        c = by_name.get(w.name)
        h = (c.health if c else None) or {}
        pw = plan['workers'].get(w.name)
        row = {'name': w.name, 'host': w.host, 'ip': topology.worker_host(w.name), 'url': (c.url if c else None),
               'cpus': w.cpus, 'up': bool(h), 'error': (c.health_error if c else None), 'in_run': pw is not None,
               'capacity': {'max_endpoints': h.get('max_endpoints'), 'max_saps': h.get('max_saps'),
                            'active_endpoints': h.get('active_endpoints'), 'active_run': h.get('active_run'),
                            'clock_skew_ms': h.get('clock_skew_ms'),
                            'rtp': (h.get('media') or {}).get('max_rtp_streams', (w.media.max_rtp_streams if w.media else None))}}
        if pw is not None:
            need = sum(len(p['identities']) for p in pw['pools'])
            row.update({'share': round(pw['share'], 3), 'rate_saps': round(pw['run']['rate_saps'], 3),
                        'max_instances': pw['run'].get('max_instances'),
                        'pools': [{'pool': p['pool'], 'kind': p['kind'], 'identities': len(p['identities']),
                                   'transport': p.get('transport'), 'target': p['target_csp']['ip']} for p in pw['pools']],
                        'endpoints_needed': need, 'roles': pw['roles']})
            cap = int(h.get('max_endpoints') or 0)
            if cap and need > cap:
                out['errors'].append(f'{w.name}: 필요 단말 {need} > 용량 {cap}')
            ms = float(h.get('max_saps') or 0)
            if ms and peak * pw['share'] > ms:
                out['warnings'].append(f'{w.name}: 피크 {peak * pw["share"]:.1f} SApS > 워커 최대 {ms:g} — 워커를 늘려 분산(풀 group)')
            if h.get('active_run'):
                out['errors'].append(f'{w.name}: 다른 run {h.get("active_run")} 진행 중')
            if abs(int(h.get('clock_skew_ms') or 0)) > 50:
                out['warnings'].append(f'{w.name}: 시계 오차 {h.get("clock_skew_ms")} ms > 50 — 지연 지표가 흔들린다')
            if not h and probe:
                out['errors'].append(f'{w.name}: 워커 미응답 — {c.health_error if c else "주소 없음"}')
            # 미디어 평면 — 샘플 보유(선언·실제 파일)·RTP 동시 상한
            hm = h.get('media') or {}
            declared = list(w.media.samples) if (w.media and w.media.samples) else None
            files = set(hm.get('files') or [])
            for sid, codecs in (plan.get('samples') or {}).items():
                if declared is not None and sid not in declared:
                    out['errors'].append(f'{w.name}: 샘플 {sid!r} 를 보유하지 않는다(토폴로지 workers.media.samples 선언)')
                    continue
                if h and 'media' in h:
                    miss = sorted(f for f in codecs.values() if f != 'synthetic' and f not in files and '/' not in f)
                    if miss:
                        out['errors'].append(f'{w.name}: 샘플 {sid!r} 파일 없음 {miss} — 워커 샘플 디렉터리 {hm.get("sample_dir") or "(미설정)"}')
                elif h and any(f != 'synthetic' for f in codecs.values()):
                    out['warnings'].append(f'{w.name}: 워커가 media 상태를 보고하지 않는다(구버전) — 샘플 {sid!r} 파일을 확인할 수 없다')
            rtp_cap = int(hm.get('max_rtp_streams') or (w.media.max_rtp_streams if w.media and w.media.max_rtp_streams else 0) or 0)
            row['capacity']['rtp'] = rtp_cap or None
            row['capacity']['rtp_streams'] = hm.get('rtp_streams')
            row['_rtp_cap'] = rtp_cap
        wrows.append(row)
    if plan.get('resolve_notes'):
        out['notes'] += [f'제외 워커: {n}' for n in plan['resolve_notes']]
    if plan.get('pinned'):
        out['notes'].append(f'피어 풀 {", ".join(plan["peer_pools"])} → 워커 {plan["pinned"]} 고정(수신점 하나) — 이 run 은 그 워커에서만')
    # 시드·환경변수
    seed = []
    env = []
    oam = topology.oam_ref()
    if plan['peer_pools']:
        if topology.target.kind == 'cims':
            used = set(plan['peer_pools'])
            pools = tester_target.seed_pools(topology, used)
            if pools:
                try:
                    lns = {pn: topology.local_node_name(lid, l) for pn, (_nid, lid, l) in topology.peer_listeners(list(pools)).items()}
                    recs = tester_target.derive_records(topology, pools, lns)
                    seed = [{'collection': c, 'count': len(v), 'names': [r['name'] for r in v]} for c, v in recs.items() if v]
                    names = sorted(set(lns.values()))
                    seed.insert(0, {'collection': 'local_nodes', 'count': len(names), 'names': names,
                                    'note': '피어가 닿는 접속점 — 대상에 같은 이름·같은 포트 레코드가 있으면 재사용, 없으면 시드'})
                except (ValueError, tester_target.TargetError) as e:
                    out['errors'].append(str(e))
                if oam is None:
                    out['errors'].append('피어 풀 시드에는 대상 oam 노드가 필요하다(또는 seed.enabled=false)')
                else:
                    env.append({'env': oam.token_env or 'TESTER_OAM_TOKEN', 'for': f'{oam.node}: 대상 OAM 토큰(시드·target_build) — 없으면 요청자 로그인 토큰을 쓴다(동거 형태)',
                                'optional': True})
            for pn in used - set(pools):
                out['notes'].append(f'{pn}: seed.enabled=false — 대상 라우팅을 수동 구성')
        else:
            out['warnings'].append(f'대상 kind={topology.target.kind} 는 컬렉션 시드가 없다 — 피어 수신점으로의 라우팅은 대상 쪽에서 미리')
    for pn in sorted({p['pool'] for w in plan['workers'].values() for p in w['pools']}):
        src = getattr(topology.pools[pn], 'source', None)
        nid = getattr(src, 'db', None)
        node = topology.target.nodes.get(nid) if nid else None
        if node is not None and node.db is not None:
            env.append({'env': f'{node.db.user_env or "(user_env 미지정)"} · {node.db.password_env or "(password_env 미지정)"}',
                        'for': f'{pn}: 대상 DB({nid}) 접속 자격 — H(A1) 보유 가입자 원천'})
    if plan['peer_pools']:
        for pn in plan['peer_pools']:
            reg = topology.pools[pn].trunk_register
            if reg is not None:
                env.append({'env': reg.ha1_env or reg.password_env, 'for': f'{pn}: 트렁크 REGISTER {reg.user} 비밀'})
    # Little 검산 — 역할(UE·피어 모두)마다 rate × SDT ≤ 신원. 단발은 동시 인스턴스가 instances 를 넘지 않는다
    little_rows = []
    first_short = None
    cap = int(plan['max_instances']) if plan['max_instances'] else None
    for r in rates:
        need = int(math.ceil(r * sdt))
        if cap is not None:
            need = min(need, cap)
        short = [role for role, x in plan['roles'].items() if x['total'] < need]
        little_rows.append({'rate': r, 'need': need, 'ok': not short, 'short': short})
        if short and first_short is None:
            first_short = r
    concurrent = int(math.ceil(peak * sdt)) if cap is None else min(cap, int(math.ceil(peak * sdt)))
    ok_rates = [x['rate'] for x in little_rows if x['ok']]
    little = {'sdt_s': sdt, 'peak_rate': peak, 'concurrent': concurrent, 'rows': little_rows,
              'first_short_rate': first_short,
              'recommend_max': (max(ok_rates) if ok_rates else None) if first_short is not None else None}
    if first_short is not None:
        mins = min(x['total'] for x in plan['roles'].values())
        need0 = next(x['need'] for x in little_rows if x['rate'] == first_short)
        hint = (f'max 를 {little["recommend_max"]:g} 이하로 두거나 신원을 늘린다' if little['recommend_max']
                else ('율을 낮추거나(instances 를 SDT 동안 다 못 소화) 신원을 늘린다' if cap else '율을 낮추거나 신원을 늘린다'))
        out['warnings'].append(f'Little 검산: {first_short:g} SApS × SDT {sdt} s ≈ 동시 {need0} > 역할 신원 {mins} — 그 단계부터 slot skipped. {hint}')
    # 미디어 평면 — 모드 요약·RTP 상한 검산·시그널링 전용 호의 RTP 기대치
    modes = [(s.media.rtp if s.media else 'auto') for s in scenario.flow if s.step == 'invite']
    uses_rtp = any(m != 'none' for m in modes) if modes else False
    n_roles = len(scenario.roles)
    for row in wrows:
        rtp_cap = row.pop('_rtp_cap', 0)
        if uses_rtp and rtp_cap and row.get('in_run'):
            need_rtp = int(math.ceil(concurrent * float(row.get('share') or 0))) * n_roles
            if need_rtp > rtp_cap:
                out['warnings'].append(f'{row["name"]}: 동시 RTP 단말 ≈ {need_rtp} > 상한 {rtp_cap}(Media.MaxRtpStreams) — 넘는 슬롯은 skipped')
    if modes and not uses_rtp:
        out['notes'].append('시그널링 전용(media.rtp: none) — RTP 를 송수신하지 않는다')
        rtp_keys = ('rtp_loss_pct', 'jitter_ms', 'mos')
        bad = sorted({k for s in scenario.flow for k in (s.expect or {}) if k in rtp_keys})
        if bad:
            out['warnings'].append(f'시그널링 전용 호에 RTP 기대치 {bad} — 표본이 없어 판정되지 않는다')
    elif 'explicit' in modes and not scenario.sample_refs() and not any(
            x.step == 'media_send' for s in scenario.flow for x in [s, *(s.during or [])]):
        out['warnings'].append('media.rtp: explicit 인데 media_send 가 없다 — 아무도 송출하지 않는다(RTP 표본 0)')
    for issue in KNOWN_TARGET_ISSUES:
        try:
            if issue['when'](scenario, topology):
                out['warnings'].append('알려진 CSP 과제: ' + issue['text'])
        except Exception:
            pass
    out.update({
        'roles': plan['roles'], 'workers': wrows, 'steps': steps, 'phases': phases, 'procedure': procedure(scenario, phases),
        'bindings': plan['bindings'], 'rate_total': plan['rate_total'], 'max_instances': plan['max_instances'],
        'identities': plan['identities'], 'peer_pools': plan['peer_pools'], 'pinned': plan['pinned'],
        'samples': plan.get('samples') or {}, 'media': {'modes': modes, 'uses_rtp': uses_rtp},
        'seed': seed, 'env': env, 'little': little,
        'estimate': {'duration_s': estimate_duration(profile, plan['rate_total'], plan['max_instances'], sdt),
                     'sdt_s': sdt, 'peak_rate': peak, 'concurrent': concurrent,
                     'model': (profile.model if profile else 'single')},
    })
    out['ok'] = not out['errors']
    return out
