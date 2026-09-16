"""run 컴파일 — 시나리오 + 토폴로지 + 프로파일 → 워커별 PoolCreate·RunStart (test_instrument.md §4·§6.1).

워커는 YAML 을 모르고 컴파일된 단계만 받는다. 여기서 정하는 것:
  · 역할 → 풀 신원 인덱스 범위(disjoint_from 은 같은 풀 안에서 서로 겹치지 않는 창)
  · 워커 배분 — 역할 창을 워커 수(cpus 가중)로 나눠 각 워커에 **자기 몫의 신원만** 보낸다(풀 create 도 그 부분집합).
    role_slices 는 워커 로컬 인덱스.
  · `${ht}` 같은 바인딩 해석(요청 bindings > profile.ht), seconds 정수화
  · 발생율 — 프로파일 initial rate 를 워커 몫으로 나눔. 단발(프로파일 없음)은 max_instances 배분.
  · 피어 풀(kind=peer) — 신원은 e164_range/did_range 를 펼친 것, 풀은 bind.ip 와 같은 호스트의 워커 **하나**에 고정된다
    (수신점이 하나이므로). 피어 풀을 쓰는 시나리오는 그 워커 한 대에서만 돈다(인스턴스의 역할들이 한 워커에 있어야 하므로).
신원 원천: creds JSONL(cspsim -creds 승계). db 원천(대상 CSC 위임)은 후속 — 지금은 명시적으로 거절한다.
"""
from __future__ import annotations

import json
import os
import re
from typing import Dict, List, Optional, Tuple

from services import tester_store as store
from services.tester_models import LoadProfile, Scenario, Topology, PoolCreate, RunStart, CompiledStep, Identity


class CompileError(Exception):
    pass


_BIND = re.compile(r'^\$\{(\w+)\}$')


def _creds_path(rel: str) -> str:
    if os.path.isabs(rel):
        return rel
    for base in (store.user_scenarios_dir(), store.bundled_scenarios_dir(), store.data_dir()):
        p = os.path.join(base, rel)
        if os.path.isfile(p):
            return p
    raise CompileError(f'creds 파일 없음: {rel} (scenarios/·DataDir 상대 또는 절대 경로)')


def expand_range(lo: str, hi: str, count: Optional[int] = None) -> List[str]:
    """번호 범위 펼치기 — 앞 비숫자 접두(+)·자릿수(0 채움) 보존. ["+8221234000","+8221234009"] → 10 개."""
    pre = ''
    i = 0
    while i < len(lo) and not lo[i].isdigit():
        pre += lo[i]
        i += 1
    a, b = lo[i:], hi[len(pre):]
    if not a.isdigit() or not b.isdigit():
        raise CompileError(f'번호 범위가 숫자가 아니다: {lo}~{hi}')
    if int(b) < int(a):
        raise CompileError(f'번호 범위 역순: {lo}~{hi}')
    n = int(b) - int(a) + 1
    if count:
        n = min(n, int(count))
    if n > 100000:
        raise CompileError(f'번호 범위가 너무 크다({n}) — identities.count 로 줄인다')
    return [pre + str(int(a) + k).zfill(len(a)) for k in range(n)]


def peer_identities(pool_name: str, pool_doc: dict) -> List[dict]:
    ids = pool_doc.get('identities') or {}
    rng = ids.get('e164_range') or ids.get('did_range')
    if not rng or len(rng) != 2:
        raise CompileError(f'pool {pool_name}: identities.e164_range 또는 did_range 가 필요하다')
    domain = str(pool_doc.get('domain') or '')
    return [{'user': u, 'domain': domain} for u in expand_range(str(rng[0]), str(rng[1]), ids.get('count'))]


def load_identities(pool_name: str, pool_doc: dict) -> List[dict]:
    """풀 신원 목록 — Identity(dict). kind=ue(creds) · kind=peer(번호 범위)."""
    kind = pool_doc.get('kind')
    if kind == 'peer':
        return peer_identities(pool_name, pool_doc)
    if kind != 'ue':
        raise CompileError(f'pool {pool_name}: kind={kind} 는 워커가 지원하지 않는다 (real-ue = F 단계)')
    src = pool_doc.get('source') or {}
    if 'db' in src:
        raise CompileError(f'pool {pool_name}: db 원천은 C 단계(대상 CSC 위임). 지금은 `cims-tester creds-from-db` 로 '
                           f'creds JSONL 을 만들어 source.creds 로 지정한다')
    path = _creds_path(str(src.get('creds') or ''))
    count = src.get('count')
    ids: List[dict] = []
    with open(path, 'r', encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            try:
                d = json.loads(line)
            except Exception as e:
                raise CompileError(f'{path}:{n}: JSON 파싱 실패 — {e}')
            user = str(d.get('user') or '')
            if not user:
                raise CompileError(f'{path}:{n}: user 누락')
            domain = str(d.get('domain') or '')
            ident = {'user': user, 'domain': domain}
            if d.get('authId'):
                ident['auth_id'] = str(d['authId'])
            if d.get('ha1'):
                ident['ha1'] = str(d['ha1'])
            if d.get('password'):
                ident['password'] = str(d['password'])
            if d.get('k'):
                ident['auth_scheme'] = 'aka'
                ident['aka_k'] = str(d['k'])
                ident['aka_opc'] = str(d.get('opc') or '')
            ids.append(ident)
            if count and len(ids) >= int(count):
                break
    if not ids:
        raise CompileError(f'{path}: 신원이 없다')
    return ids


def _default_domain(topology: Topology, pool_doc: dict) -> str:
    """creds 에 domain 이 없을 때 — 풀 이름/표기로 volte·ptt 도메인을 고른다 (target.csp.domain_*). 피어 풀은 자기 domain."""
    if pool_doc.get('kind') == 'peer':
        return str(pool_doc.get('domain') or '')
    csp = topology.target.csp
    name = str(pool_doc.get('_name') or '')
    if 'ptt' in name and csp.domain_ptt:
        return csp.domain_ptt
    return csp.domain_volte or csp.domain_ptt or ''


def bind_value(v, bindings: Dict[str, object]):
    if isinstance(v, str):
        m = _BIND.match(v.strip())
        if m:
            key = m.group(1)
            if key not in bindings:
                raise CompileError(f'바인딩 ${{{key}}} 값이 없다 — profile.ht 또는 요청 bindings 로 준다')
            return bindings[key]
        if v.isdigit():
            return int(v)
    return v


def compile_steps(scenario: Scenario, bindings: Dict[str, object]) -> List[dict]:
    out = []
    for i, s in enumerate(scenario.flow):
        cs = CompiledStep(
            idx=i, step=s.step, who=list(s.who or []), **{'from': s.from_}, to=s.to,
            after_ms=int(s.after_ms or 0),
            seconds=(int(bind_value(s.seconds, bindings)) if s.seconds is not None else None),
            media=s.media, group=s.group, payload=s.payload, expect=s.expect,
        )
        out.append(cs.model_dump(by_alias=True, exclude_none=True))
    return out


def role_ranges(scenario: Scenario, pool_sizes: Dict[str, int]) -> Dict[str, Tuple[str, int, int]]:
    """역할 → (풀, begin, end). disjoint_from 체인은 같은 풀에서 이어붙인 창, 그 외는 0 부터.

    count 생략 = 풀 전체. 단, 같은 풀에서 서로 disjoint 인 역할들이 count 없이 있으면 풀을 **균등 분할**한다
    (caller/callee 가 한 풀을 나눠 쓰는 흔한 꼴 — "전체" 둘은 성립할 수 없다). 명시 count 가 있는 역할은
    그 몫을 먼저 빼고 나머지를 나눈다."""
    out: Dict[str, Tuple[str, int, int]] = {}
    cursor: Dict[str, int] = {}   # 풀별 다음 begin (disjoint 체인용)
    # 풀별 disjoint 관계에 얽힌 역할 — count 없는 것끼리 균등 분할
    auto_count: Dict[str, int] = {}
    by_pool: Dict[str, List[str]] = {}
    for name, r in scenario.roles.items():
        by_pool.setdefault(r.pool, []).append(name)
    for pool, names in by_pool.items():
        related = set()
        for n in names:
            d = scenario.roles[n].disjoint_from
            if d and scenario.roles.get(d) and scenario.roles[d].pool == pool:
                related.add(n)
                related.add(d)
        uncounted = [n for n in names if n in related and not scenario.roles[n].count]
        if not uncounted:
            continue
        size = pool_sizes.get(pool) or 0
        taken = sum(int(scenario.roles[n].count) for n in related if scenario.roles[n].count)
        share = (size - taken) // len(uncounted)
        if share < 1:
            raise CompileError(f'pool {pool}: 신원 {size} 개를 disjoint 역할 {sorted(related)} 에 나눌 수 없다 — count 를 줄이거나 신원을 늘린다')
        for n in uncounted:
            auto_count[n] = share
    # disjoint_from 이 가리키는 역할을 먼저 확정한다(간단한 위상 정렬)
    pending = dict(scenario.roles)
    guard = 0
    while pending and guard < 100:
        guard += 1
        for name, r in list(pending.items()):
            if r.disjoint_from and r.disjoint_from in pending:
                continue
            size = pool_sizes.get(r.pool)
            if size is None:
                raise CompileError(f'roles.{name}: 토폴로지에 pool {r.pool!r} 이 없다')
            want = int(r.count) if r.count else auto_count.get(name)
            if r.disjoint_from:
                base_pool, _b, base_end = out[r.disjoint_from]
                if base_pool != r.pool:
                    begin = 0
                else:
                    begin = max(base_end, cursor.get(r.pool, 0))
            else:
                begin = 0
            end = begin + want if want else size
            if end > size or end <= begin:
                raise CompileError(f'roles.{name}: pool {r.pool} 신원 {size} 개로는 [{begin},{end}) 를 채울 수 없다')
            out[name] = (r.pool, begin, end)
            cursor[r.pool] = max(cursor.get(r.pool, 0), end)
            del pending[name]
    if pending:
        raise CompileError(f'roles: disjoint_from 순환 — {sorted(pending)}')
    return out


def split_range(begin: int, end: int, weights: List[float]) -> List[Tuple[int, int]]:
    """[begin,end) 를 가중치로 연속 분할 — 각 워커 몫 (b,e). 총량이 워커 수보다 작으면 앞 워커부터 1개씩."""
    n = end - begin
    total = sum(weights) or 1.0
    out = []
    cur = begin
    for i, w in enumerate(weights):
        share = int(round(n * w / total)) if i < len(weights) - 1 else end - cur
        share = max(0, min(share, end - cur))
        out.append((cur, cur + share))
        cur += share
    return out


def compile_run(run_id: str, scenario: Scenario, topology: Topology, topology_doc: dict,
                profile: Optional[LoadProfile], bindings: Dict[str, object],
                workers: List[object], stream_for, instances: Optional[int], rate_saps: Optional[float]):
    """반환 plan = {'workers': {name: {'pools': [PoolCreate dict], 'run': RunStart dict}}, 'rate_total', 'roles', 'steps'}"""
    if not workers:
        raise CompileError('워커가 없다 — 토폴로지 workers 에 cims-tester-worker 주소를 적는다')
    bindings = dict(bindings or {})
    if profile is not None and profile.ht is not None and 'ht' not in bindings:
        bindings['ht'] = profile.ht
    steps = compile_steps(scenario, bindings)

    # 풀 신원
    pools_doc = topology_doc.get('pools') or {}
    used_pools = {r.pool for r in scenario.roles.values()}
    identities: Dict[str, List[dict]] = {}
    for pname in used_pools:
        pdoc = dict(pools_doc.get(pname) or {})
        pdoc['_name'] = pname
        ids = load_identities(pname, pdoc)
        dom = _default_domain(topology, pdoc)
        for ident in ids:
            if not ident.get('domain'):
                if not dom:
                    raise CompileError(f'pool {pname}: creds 에 domain 이 없고 target.csp.domain_* 도 없다')
                ident['domain'] = dom
            Identity.model_validate(ident)
        identities[pname] = ids
    ranges = role_ranges(scenario, {p: len(v) for p, v in identities.items()})

    # 피어 풀 — bind.ip 호스트의 워커 하나에 고정. 피어를 쓰는 시나리오는 그 워커에서만 돈다.
    peer_pools = [p for p in used_pools if (pools_doc.get(p) or {}).get('kind') == 'peer']
    if peer_pools:
        pinned = None
        for pname in peer_pools:
            bind_ip = str(((pools_doc.get(pname) or {}).get('bind') or {}).get('ip') or '')
            host = None
            for w in workers:
                if w.host == bind_ip or str((w.health or {}).get('local_ip') or '') == bind_ip:
                    host = w
                    break
            if host is None:
                raise CompileError(f'pool {pname}: bind.ip {bind_ip} 인 워커가 토폴로지 workers 에 없다 — 피어 수신점은 워커 호스트여야 한다')
            if pinned is not None and pinned is not host:
                raise CompileError(f'피어 풀들이 서로 다른 워커({pinned.name}, {host.name})에 있다 — 한 시나리오의 피어는 한 워커에')
            pinned = host
        workers = [pinned]

    # 워커 배분
    weights = [float(getattr(w, 'cpus', None) or ((w.health or {}).get('max_endpoints') or 1) / 200.0 or 1) for w in workers]
    per_worker: Dict[str, dict] = {w.name: {'pools': {}, 'roles': {}, 'slices': {}} for w in workers}
    for role, (pname, b, e) in ranges.items():
        parts = split_range(b, e, weights)
        for w, (pb, pe) in zip(workers, parts):
            pw = per_worker[w.name]
            pw['roles'][role] = pname
            # 워커 로컬 풀 신원 = 이 워커가 맡는 인덱스의 합집합 (전역 인덱스 → 로컬 인덱스)
            local = pw['pools'].setdefault(pname, {'global': []})
            for gi in range(pb, pe):
                if gi not in local['global']:
                    local['global'].append(gi)
            pw['slices'][role] = (pb, pe)
    rate_total = 0.0
    max_instances = None
    if profile is None:
        max_instances = int(instances or 1)
        rate_total = float(rate_saps or max_instances)
    else:
        rate_total = initial_rate(profile)

    plan_workers = {}
    for w in workers:
        pw = per_worker[w.name]
        pools = []
        gmap: Dict[str, Dict[int, int]] = {}
        for pname, local in pw['pools'].items():
            glist = sorted(local['global'])
            gmap[pname] = {g: i for i, g in enumerate(glist)}
            pdoc = pools_doc.get(pname) or {}
            if pdoc.get('kind') == 'peer':
                pc = PoolCreate(pool=pname, kind='peer', identities=[identities[pname][g] for g in glist],
                                transport=str((pdoc.get('bind') or {}).get('protocol') or 'udp'),
                                target_csp=topology.target.csp, peer=topology.pools[pname])
            else:
                pc = PoolCreate(pool=pname, kind='ue', identities=[identities[pname][g] for g in glist],
                                transport=pdoc.get('transport', 'udp'), srtp=pdoc.get('srtp', 'off'),
                                target_csp=topology.target.csp)
            pools.append(pc.model_dump(by_alias=True, exclude_none=True))
        slices = {}
        for role, (pb, pe) in pw['slices'].items():
            pname = pw['roles'][role]
            if pe <= pb:
                slices[role] = [0, 0]
            else:
                slices[role] = [gmap[pname][pb], gmap[pname][pe - 1] + 1]
        share = weights[workers.index(w)] / (sum(weights) or 1.0)
        rs = RunStart(run_id=run_id, scenario_id=scenario.id, roles=pw['roles'], role_slices=slices,
                      steps=[CompiledStep.model_validate(s) for s in steps],
                      rate_saps=rate_total * share,
                      max_instances=(max(1, int(round(max_instances * share))) if max_instances else None),
                      stream=stream_for(w))
        plan_workers[w.name] = {'pools': pools, 'run': rs.model_dump(by_alias=True, exclude_none=True),
                                'share': share}
    # 단발 배분 합이 instances 를 넘거나 모자라면 첫 워커에서 보정
    if max_instances:
        tot = sum(p['run'].get('max_instances', 0) for p in plan_workers.values())
        first = plan_workers[workers[0].name]['run']
        first['max_instances'] = max(1, first.get('max_instances', 0) + (max_instances - tot))
    return {'workers': plan_workers, 'rate_total': rate_total, 'roles': {r: list(v) for r, v in ranges.items()},
            'steps': steps, 'bindings': bindings, 'max_instances': max_instances,
            'identities': {p: len(v) for p, v in identities.items()}, 'peer_pools': peer_pools}


def initial_rate(profile: LoadProfile) -> float:
    if profile.model in ('constant', 'soak'):
        return float(profile.rate)
    if profile.model in ('step', 'ramp'):
        return float(profile.start)
    if profile.model == 'burst':
        return float(profile.burst_size)
    return 0.0
