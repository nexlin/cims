"""run 컴파일 — 시나리오 + 토폴로지 + 프로파일 → 워커별 PoolCreate·RunStart (test_instrument.md §4·§6.1).

워커는 YAML 을 모르고 컴파일된 단계만 받는다. 여기서 정하는 것:
  · 역할 → 풀 해석 — `roles.X.pool` 은 토폴로지 풀 **이름 또는 group**(논리 풀 이름). 워커마다 그 워커의 로컬 풀 하나로
    해석하고, **모든 역할이 해석되는 워커만** run 에 참여한다(§4). 워커 사이의 신원 분할은 컨트롤러가 하지 않고 풀 정의가 한다.
  · 역할 → 풀 신원 인덱스 범위(disjoint_from 은 같은 풀 안에서 서로 겹치지 않는 창) — 워커 로컬 인덱스.
  · `${ht}` 같은 바인딩 해석(요청 bindings > profile.ht), seconds 정수화, `media_hold.during` 을 평평한 단계열로 풀기.
  · 발생율 — 프로파일 initial rate 를 워커 몫(cpus 가중)으로 나눔. 단발(프로파일 없음)은 max_instances 배분.
  · 피어 풀(kind=peer) — 신원은 e164_range/did_range 를 펼친 것, 풀은 자기 `worker` 에 고정된다(수신점이 하나이므로).
    피어 풀을 쓰는 시나리오는 그 워커 한 대에서만 돈다(인스턴스의 역할들이 한 워커에 있어야 하므로).
  · 워커 계약(`PoolCreate.target_csp`·`peer.bind.ip`)은 토폴로지 노드·호스트 참조에서 **파생**한다 — 워커 계약은 그대로다.
신원 원천: creds JSONL(cspsim -creds 승계) 또는 대상 DB(`source.db` — db 노드 접속 + 환경변수 자격, `db_identities`).
"""
from __future__ import annotations

import json
import os
import re
from typing import Dict, List, Optional, Tuple

from services import tester_store as store
from services.tester_models import (LoadProfile, Scenario, Topology, PoolCreate, RunStart, CompiledStep, Identity,
                                    TrunkRegister, WorkerPeer, WorkerPeerBind, WORKER_STEPS, Role)


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


DB_TABLES = ('volte_subscriptions', 'voip_subscriptions', 'ptt_subscriptions')


def db_identities(pool_name: str, src: dict, topology: Topology, transport: str) -> List[dict]:
    """`source.db` — 대상 DB 에서 H(A1) 보유 가입자를 읽는다(`cims-tester creds-from-db` 와 같은 질의). 접속 = db 노드(주소·port·name) +
    자격은 환경변수(`db.user_env`·`db.password_env` — 토폴로지에 비밀을 적지 않는다). 그 풀의 transport 로 접속할 수 있는 가입자만
    (`sip_transport` 가 비었거나 같은 것). H(A1) 은 메모리에서 워커로만 가고 run 기록에 남지 않는다."""
    nid = str(src.get('db') or '')
    node = topology.target.nodes.get(nid)
    if node is None or node.db is None:
        raise CompileError(f'pool {pool_name}: source.db={nid!r} 는 db 블록이 있는 노드가 아니다')
    table = str(src.get('table') or '')
    if table not in DB_TABLES:
        raise CompileError(f'pool {pool_name}: source.table 은 {list(DB_TABLES)} 중 하나')
    user = os.environ.get(node.db.user_env or '', '').strip() if node.db.user_env else ''
    pw = os.environ.get(node.db.password_env or '', '') if node.db.password_env else ''
    if not user:
        raise CompileError(f'pool {pool_name}: DB 자격이 없다 — 노드 {nid} 의 db.user_env/password_env 가 가리키는 환경변수를 컨트롤러에 준다')
    try:
        import pymysql
    except ImportError:
        raise CompileError('pymysql 을 찾지 못했다 — base oam vendor 경로 확인')
    try:
        conn = pymysql.connect(host=topology.node_ip(nid), port=int(node.db.port), user=user, password=pw,
                               database=node.db.name, connect_timeout=6, read_timeout=15)
    except Exception as e:
        raise CompileError(f'pool {pool_name}: DB 접속 실패({topology.node_ip(nid)}:{node.db.port}/{node.db.name}) — {e}')
    try:
        cur = conn.cursor()
        cur.execute(f"SELECT id, imsi, ha1 FROM {table} WHERE ha1 IS NOT NULL AND ha1<>'' "
                    f"AND (sip_transport IS NULL OR sip_transport='' OR sip_transport=%s) ORDER BY id LIMIT %s OFFSET %s",
                    ((transport or 'udp').upper(), int(src.get('count') or 0), int(src.get('offset') or 0)))
        rows = cur.fetchall()
    except Exception as e:
        raise CompileError(f'pool {pool_name}: DB 질의 실패({table}) — {e}')
    finally:
        conn.close()
    ids = []
    for uid, imsi, ha1 in rows:
        ident = {'user': str(uid), 'domain': '', 'ha1': str(ha1)}
        if imsi:
            ident['auth_id'] = str(imsi)
        ids.append(ident)
    if not ids:
        raise CompileError(f'pool {pool_name}: {table} 에 H(A1) 보유 가입자가 없다(offset {src.get("offset") or 0}, transport {transport})')
    return ids


def load_identities(pool_name: str, pool_doc: dict, topology: Optional[Topology] = None) -> List[dict]:
    """풀 신원 목록 — Identity(dict). kind=ue(creds JSONL | 대상 DB) · kind=peer(번호 범위)."""
    kind = pool_doc.get('kind')
    if kind == 'peer':
        return peer_identities(pool_name, pool_doc)
    if kind != 'ue':
        raise CompileError(f'pool {pool_name}: kind={kind} 는 워커가 지원하지 않는다 (real-ue = F 단계)')
    src = pool_doc.get('source') or {}
    if 'db' in src:
        if topology is None:
            raise CompileError(f'pool {pool_name}: db 원천은 토폴로지(db 노드)가 있어야 읽는다')
        return db_identities(pool_name, src, topology, str(pool_doc.get('transport') or 'udp'))
    path = _creds_path(str(src.get('creds') or ''))
    count = src.get('count')
    offset = int(src.get('offset') or 0)
    seen = 0
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
            seen += 1
            if seen <= offset:
                continue
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
        raise CompileError(f'{path}: 신원이 없다' + (f' (offset {offset} 뒤)' if offset else ''))
    return ids


def _default_domain(topology: Topology, pname: str) -> str:
    """creds 에 domain 이 없을 때 — 접속점 노드의 도메인(풀 이름에 ptt 가 있으면 PTT 도메인). 피어 풀은 자기 domain."""
    p = topology.pools[pname]
    if p.kind == 'peer':
        return p.domain
    return topology.default_domain(p.access, ptt='ptt' in pname) or topology.default_domain(p.access)


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
    """단계 → CompiledStep(dict). `media_hold.during` 은 `hold at_s → 동작 → hold 나머지` 로 푼다(마지막 조각이 기대치를 갖는다).
    idx 는 시나리오 flow 인덱스(원 단계) — 풀린 조각은 같은 idx 를 공유하고 `src` 로 원 단계를 가리킨다."""
    out = []
    unsupported = sorted({s.step for s in scenario.flow if s.step not in WORKER_STEPS})
    if unsupported:
        raise CompileError(f'워커가 지원하지 않는 단계 {unsupported} — 지원: {sorted(WORKER_STEPS)}')

    def emit(i, step, who=None, from_=None, to=None, after_ms=0, seconds=None, media=None, group=None,
             payload=None, cause=None, expect=None, sample=None, loop=None):
        cs = CompiledStep(idx=len(out), step=step, who=list(who or []), **{'from': from_}, to=to,
                          after_ms=int(after_ms or 0), seconds=seconds, media=media, group=group,
                          payload=payload, cause=cause, sample=sample, loop=loop, expect=expect or {})
        d = cs.model_dump(by_alias=True, exclude_none=True)
        d['src'] = i
        out.append(d)

    for i, s in enumerate(scenario.flow):
        seconds = int(bind_value(s.seconds, bindings)) if s.seconds is not None else None
        if s.step == 'media_hold' and s.during:
            cur = 0.0
            for d in sorted(s.during, key=lambda x: x.at_s):
                if d.at_s > seconds:
                    raise CompileError(f'flow[{i}] during at_s={d.at_s} 가 seconds={seconds} 를 넘는다')
                piece = int(round(d.at_s - cur))
                if piece > 0:
                    emit(i, 'media_hold', seconds=piece)
                emit(i, d.step, who=d.who, from_=d.from_, to=d.to, payload=d.payload, expect=d.expect,
                     sample=d.sample, loop=d.loop)
                cur = d.at_s
            emit(i, 'media_hold', seconds=max(1, int(round(seconds - cur))), expect=s.expect)
            continue
        emit(i, s.step, who=s.who, from_=s.from_, to=s.to, after_ms=s.after_ms, seconds=seconds, media=s.media,
             group=s.group, payload=s.payload, cause=s.cause, expect=s.expect, sample=s.sample, loop=s.loop)
    return out


def run_samples(scenario: Scenario, topology: Topology) -> Dict[str, Dict[str, str]]:
    """시나리오가 참조하는 샘플 → 토폴로지 샘플 라이브러리 발췌(RunStart.samples). 라이브러리에 없는 id 는 컴파일 오류."""
    lib = topology.media.samples if topology.media else {}
    out: Dict[str, Dict[str, str]] = {}
    for sid in scenario.sample_refs():
        if sid not in lib:
            raise CompileError(f'샘플 {sid!r} 가 토폴로지 media.samples 에 없다 — 정의된 샘플: {sorted(lib) or "없음"}')
        out[sid] = dict(lib[sid])
    return out


def phases(scenario: Scenario) -> Dict[str, List[int]]:
    """flow 인덱스를 prelude(앞쪽 register/wait) · body · epilogue(끝 deregister) 로 나눈다 — 워커 실행 의미(§4)."""
    n = len(scenario.flow)
    pre = 0
    while pre < n and scenario.flow[pre].step in ('register', 'wait'):
        pre += 1
    epi = n
    while epi > pre and scenario.flow[epi - 1].step == 'deregister':
        epi -= 1
    return {'prelude': list(range(0, pre)), 'body': list(range(pre, epi)), 'epilogue': list(range(epi, n))}


def trunk_register_for(pool_name: str, peer, realm_default: Optional[str]) -> Optional[TrunkRegister]:
    """피어 풀의 register(트렁크 계정) → 워커용 값 — 비밀은 환경변수에서 푼다(없으면 컴파일 오류, 조용히 빈 값으로 보내지 않는다)."""
    reg = getattr(peer, 'trunk_register', None)
    if reg is None:
        return None
    ha1 = os.environ.get(reg.ha1_env, '').strip() if reg.ha1_env else ''
    pw = os.environ.get(reg.password_env, '').strip() if reg.password_env else ''
    if not ha1 and not pw:
        env = reg.ha1_env or reg.password_env
        raise CompileError(f'pool {pool_name}: 트렁크 REGISTER 비밀이 없다 — 환경변수 {env} 에 H(A1)/비밀번호를 둔다')
    return TrunkRegister(user=reg.user, realm=reg.realm or realm_default, ha1=ha1 or None, password=pw or None, expires=reg.expires)


def resolve_roles(scenario: Scenario, topology: Topology) -> Tuple[Dict[str, Dict[str, str]], List[str]]:
    """워커별 역할 해석 — {worker: {role: 로컬 풀 이름}} (모든 역할이 해석되는 워커만) + 해석 실패 사유 목록."""
    out: Dict[str, Dict[str, str]] = {}
    why: List[str] = []
    for w in topology.workers:
        m: Dict[str, str] = {}
        ok = True
        for role, r in scenario.roles.items():
            cands = [pn for pn, p in topology.pools.items() if p.worker == w.name and (pn == r.pool or p.group == r.pool)]
            if len(cands) == 1:
                m[role] = cands[0]
            else:
                ok = False
                why.append(f'{w.name}: roles.{role} pool {r.pool!r} → 로컬 풀 {len(cands)}개')
        if ok:
            out[w.name] = m
    return out, why


def check_register_roles(scenario: Scenario, topology: Topology, role_pool: Dict[str, str]) -> None:
    """register 단계의 역할이 피어 풀이면 트렁크 계정(register)이 있어야 한다 — 피어 신원은 개별 등록이 없다(§3.2)."""
    for i, st in enumerate(scenario.flow):
        if st.step not in ('register', 'deregister'):
            continue
        for role in st.who or []:
            pool = topology.pools.get(role_pool.get(role, ''))
            if pool is not None and pool.kind == 'peer' and pool.trunk_register is None:
                raise CompileError(f'flow[{i}] {st.step}: 역할 {role!r} 의 피어 풀에는 register(트렁크 계정)가 없다 — '
                                   f'고정 IP 피어링 피어는 등록하지 않는다')


def check_kind_gates(scenario: Scenario, topology: Topology, role_pool: Dict[str, str]) -> None:
    """단계의 행위자 kind 게이트(STEP_VOCAB.kind) — progress/refer 는 피어, PTT 단계는 UE."""
    from services.tester_models import STEP_VOCAB
    for i, st in enumerate(scenario.flow):
        gate = (STEP_VOCAB.get(st.step) or {}).get('kind')
        if not gate:
            continue
        actors = [st.from_] if st.from_ else list(st.who or [])
        for role in actors:
            pool = topology.pools.get(role_pool.get(role, ''))
            if pool is None:
                continue
            if gate == 'peer' and pool.kind != 'peer':
                raise CompileError(f'flow[{i}] {st.step}: 역할 {role!r} 은 피어 풀이어야 한다')
            if gate == 'ue' and pool.kind == 'peer':
                raise CompileError(f'flow[{i}] {st.step}: 역할 {role!r} 은 UE 풀이어야 한다')


def role_ranges(roles: Dict[str, Role], role_pool: Dict[str, str], pool_sizes: Dict[str, int]) -> Dict[str, Tuple[str, int, int]]:
    """역할 → (풀, begin, end). disjoint_from 체인은 같은 풀에서 이어붙인 창, 그 외는 0 부터.

    count 생략 = 풀 전체. 단, 같은 풀에서 서로 disjoint 인 역할들이 count 없이 있으면 풀을 **균등 분할**한다
    (caller/callee 가 한 풀을 나눠 쓰는 흔한 꼴 — "전체" 둘은 성립할 수 없다). 명시 count 가 있는 역할은
    그 몫을 먼저 빼고 나머지를 나눈다."""
    out: Dict[str, Tuple[str, int, int]] = {}
    cursor: Dict[str, int] = {}
    auto_count: Dict[str, int] = {}
    by_pool: Dict[str, List[str]] = {}
    for name in roles:
        by_pool.setdefault(role_pool[name], []).append(name)
    for pool, names in by_pool.items():
        related = set()
        for n in names:
            d = roles[n].disjoint_from
            if d and d in roles and role_pool.get(d) == pool:
                related.add(n)
                related.add(d)
        uncounted = [n for n in names if n in related and not roles[n].count]
        if not uncounted:
            continue
        size = pool_sizes.get(pool) or 0
        taken = sum(int(roles[n].count) for n in related if roles[n].count)
        share = (size - taken) // len(uncounted)
        if share < 1:
            raise CompileError(f'pool {pool}: 신원 {size} 개를 disjoint 역할 {sorted(related)} 에 나눌 수 없다 — count 를 줄이거나 신원을 늘린다')
        for n in uncounted:
            auto_count[n] = share
    pending = dict(roles)
    guard = 0
    while pending and guard < 100:
        guard += 1
        for name, r in list(pending.items()):
            if r.disjoint_from and r.disjoint_from in pending:
                continue
            pool = role_pool[name]
            size = pool_sizes.get(pool)
            if size is None:
                raise CompileError(f'roles.{name}: pool {pool!r} 신원을 모른다')
            want = int(r.count) if r.count else auto_count.get(name)
            if r.disjoint_from:
                base_pool, _b, base_end = out[r.disjoint_from]
                begin = 0 if base_pool != pool else max(base_end, cursor.get(pool, 0))
            else:
                begin = 0
            end = begin + want if want else size
            if end > size or end <= begin:
                raise CompileError(f'roles.{name}: pool {pool} 신원 {size} 개로는 [{begin},{end}) 를 채울 수 없다')
            out[name] = (pool, begin, end)
            cursor[pool] = max(cursor.get(pool, 0), end)
            del pending[name]
    if pending:
        raise CompileError(f'roles: disjoint_from 순환 — {sorted(pending)}')
    return out


def worker_peer(topology: Topology, pname: str) -> WorkerPeer:
    p = topology.pools[pname]
    return WorkerPeer(profile=p.profile, bind=WorkerPeerBind(ip=topology.pool_bind_ip(pname), port=p.bind.port, protocol=p.bind.protocol),
                      domain=p.domain, identities=p.identities, codecs=p.codecs, answer=p.answer, prack=p.prack, dtmf=p.dtmf)


def initial_rate(profile: LoadProfile) -> float:
    if profile.model in ('constant', 'soak'):
        return float(profile.rate)
    if profile.model in ('step', 'ramp'):
        return float(profile.start)
    if profile.model == 'burst':
        return float(profile.burst_size)
    return 0.0


def compile_run(run_id: str, scenario: Scenario, topology: Topology, topology_doc: dict,
                profile: Optional[LoadProfile], bindings: Dict[str, object],
                workers: List[object], stream_for, instances: Optional[int], rate_saps: Optional[float]):
    """반환 plan = {'workers': {name: {'pools': [PoolCreate dict], 'run': RunStart dict, 'share', 'roles': {role: [pool,b,e]}}},
    'rate_total', 'roles'(역할→요청 풀·kind·워커별 창), 'steps', 'phases', 'bindings', 'max_instances', 'identities', 'peer_pools', 'pinned'}.
    `workers` = 발견된 WorkerClient(health 있으면 cpus 가중에 쓴다) — 후보에 없는 워커는 plan 에서 빠진다."""
    if not topology.workers:
        raise CompileError('워커가 없다 — 토폴로지 workers 에 cims-tester-worker(호스트+포트)를 적는다')
    bindings = dict(bindings or {})
    if profile is not None and profile.ht is not None and 'ht' not in bindings:
        bindings['ht'] = profile.ht
    steps = compile_steps(scenario, bindings)
    samples = run_samples(scenario, topology)

    per_worker_roles, why = resolve_roles(scenario, topology)
    if not per_worker_roles:
        raise CompileError('모든 역할이 해석되는 워커가 없다 — 풀의 worker/group 을 확인: ' + '; '.join(why[:6]))
    by_name = {w.name: w for w in workers}
    # 피어 풀 고정 — 후보 워커들이 해석한 피어 풀은 하나의 워커에 있어야 한다
    peer_pools = sorted({pn for m in per_worker_roles.values() for pn in m.values() if topology.pools[pn].kind == 'peer'})
    pinned = None
    if peer_pools:
        ws = {topology.pools[pn].worker for pn in peer_pools}
        if len(ws) > 1:
            raise CompileError(f'피어 풀들이 서로 다른 워커({sorted(ws)})에 있다 — 한 시나리오의 피어는 한 워커에')
        pinned = next(iter(ws))
        if pinned not in per_worker_roles:
            raise CompileError(f'피어 고정 워커 {pinned} 에 모든 UE 역할의 로컬 풀이 없다')
        per_worker_roles = {pinned: per_worker_roles[pinned]}
    cand = [w for w in topology.workers if w.name in per_worker_roles]
    missing = [w.name for w in cand if w.name not in by_name]
    if missing:
        raise CompileError(f'워커 {missing} 의 클라이언트가 없다(호스트 주소 누락)')

    # 신원(풀 단위 캐시) + 워커별 역할 창
    pools_doc = topology_doc.get('pools') or {}
    identities: Dict[str, List[dict]] = {}

    def ids_of(pname: str) -> List[dict]:
        if pname not in identities:
            pdoc = dict(pools_doc.get(pname) or {})
            ids = load_identities(pname, pdoc, topology)
            dom = _default_domain(topology, pname)
            for ident in ids:
                if not ident.get('domain'):
                    if not dom:
                        raise CompileError(f'pool {pname}: creds 에 domain 이 없고 접속점 노드에 domains 도 없다')
                    ident['domain'] = dom
                Identity.model_validate(ident)
            identities[pname] = ids
        return identities[pname]

    per_worker: Dict[str, dict] = {}
    for w in cand:
        role_pool = per_worker_roles[w.name]
        check_register_roles(scenario, topology, role_pool)
        check_kind_gates(scenario, topology, role_pool)
        sizes = {pn: len(ids_of(pn)) for pn in set(role_pool.values())}
        ranges = role_ranges(scenario.roles, role_pool, sizes)
        per_worker[w.name] = {'role_pool': role_pool, 'ranges': ranges}

    # 워커 배분(율·단발 인스턴스) — cpus 가중(없으면 health max_endpoints/200, 그것도 없으면 1)
    def weight(w) -> float:
        c = by_name[w.name]
        return float(w.cpus or ((c.health or {}).get('max_endpoints') or 0) / 200.0 or 1)
    weights = [weight(w) for w in cand]
    total_w = sum(weights) or 1.0
    max_instances = None
    if profile is None:
        max_instances = int(instances or 1)
        rate_total = float(rate_saps or max_instances)
    else:
        rate_total = initial_rate(profile)

    plan_workers = {}
    for w, wt in zip(cand, weights):
        pw = per_worker[w.name]
        share = wt / total_w
        pools = []
        for pname in sorted(set(pw['role_pool'].values())):
            p = topology.pools[pname]
            tc = topology.target_csp_for(pname)
            if p.kind == 'peer':
                pc = PoolCreate(pool=pname, kind='peer', identities=ids_of(pname), transport=p.bind.protocol,
                                target_csp=tc, peer=worker_peer(topology, pname),
                                trunk_register=trunk_register_for(pname, p, tc.domain_volte))
            else:
                pc = PoolCreate(pool=pname, kind='ue', identities=ids_of(pname), transport=p.transport, srtp=p.srtp,
                                prack=bool(p.prack), dtmf=bool(p.dtmf), target_csp=tc)
            pools.append(pc.model_dump(by_alias=True, exclude_none=True))
        slices = {role: [b, e] for role, (_p, b, e) in pw['ranges'].items()}
        rs = RunStart(run_id=run_id, scenario_id=scenario.id, roles=dict(pw['role_pool']), role_slices=slices,
                      steps=[CompiledStep.model_validate({k: v for k, v in s.items() if k != 'src'}) for s in steps],
                      samples=samples, rate_saps=rate_total * share,
                      max_instances=(max(1, int(round(max_instances * share))) if max_instances else None),
                      stream=stream_for(by_name[w.name]))
        plan_workers[w.name] = {'pools': pools, 'run': rs.model_dump(by_alias=True, exclude_none=True), 'share': share,
                                'roles': {r: [p, b, e] for r, (p, b, e) in pw['ranges'].items()}}
    if max_instances:
        tot = sum(p['run'].get('max_instances', 0) for p in plan_workers.values())
        first = plan_workers[cand[0].name]['run']
        first['max_instances'] = max(1, first.get('max_instances', 0) + (max_instances - tot))
    # 같은 신원을 두 풀(= 두 워커)이 쓰면 등록 바인딩이 서로를 덮는다 — 워커마다 다른 신원(creds 파일을 나누거나 source.offset/count)
    owner: Dict[str, str] = {}
    for pn, ids in identities.items():
        if topology.pools[pn].kind != 'ue':
            continue
        for it in ids:
            k = f"{it['user']}@{it['domain']}"
            if owner.setdefault(k, pn) != pn:
                raise CompileError(f'신원 {k} 가 풀 {owner[k]} 와 {pn} 에 겹친다 — 워커마다 다른 신원을 준다(source.offset/count 또는 creds 파일 분리)')
    roles_out = {}
    for role, r in scenario.roles.items():
        per = {wn: plan_workers[wn]['roles'][role] for wn in plan_workers}
        pname0 = next(iter(per.values()))[0]
        p0 = topology.pools[pname0]
        roles_out[role] = {'pool': r.pool, 'kind': p0.kind, 'profile': getattr(p0, 'profile', None),
                           'disjoint_from': r.disjoint_from, 'count': r.count,
                           'workers': per, 'total': sum(e - b for (_p, b, e) in per.values())}
    return {'workers': plan_workers, 'rate_total': rate_total, 'roles': roles_out, 'steps': steps, 'phases': phases(scenario),
            'bindings': bindings, 'max_instances': max_instances,
            'identities': {p: len(v) for p, v in identities.items()}, 'peer_pools': peer_pools, 'pinned': pinned,
            'samples': samples, 'resolve_notes': why}
