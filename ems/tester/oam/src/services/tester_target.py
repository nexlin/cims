"""대상(SUT) OAM 클라이언트 — CSP 컬렉션 시드/복원 (test_instrument.md §3.2·§6.2).

피어 풀은 CSP 가 "아는" 상대여야 한다 — remote_nodes(피어 주소)·routes(접속점↔피어)·route_sets(failover/round_robin)·
rules/rule_sets/routing_policies(Request-URI 도메인 → RouteSet)·acl_policies(소스 IP allow/deny). 계측기는 토폴로지의
피어 풀 정의에서 이 레코드를 **파생**해 run 전에 대상 OAM 의 컬렉션 API(`PUT /api/v1/deployments/{id}/collection/{name}`)로
넣고(SIGUSR1 reload), run 이 끝나면 저장해 둔 원본으로 되돌린다. 레코드 스키마는 CSP 의 것 그대로(sip_service_model.md §2) —
계측기 쪽 번역 계층을 두지 않는다. 시드 레코드는 tags 에 `cims-tester` 를 달아 재실행 시 남은 것을 걷어낸다.

접속점(LocalNode) = 피어 풀이 가리킨 수신점(`sip.listeners[<id>]` — edge 는 무엇이든) 이다. 항목의 `local_node` 이름이 대상에 있으면
그 레코드, 없으면 같은 protocol·port 의 기존 레코드, 그것도 없으면 그 이름(비면 `cims-tester-<id>`)·그 edge 로 시드한다(복원 시 함께
사라진다). 피어 신뢰는 시드한 Route(`inbound_auth=none`) 가 세우고 ACL 은 `scope=route` 로 그 피어에만 건다 — 같은 접속점의 UE 트래픽에는
걸리지 않는다. 주소는 수신점 ip(→ 노드 addr → 호스트), 피어 수신점 ip 는 bind.ip(→ 워커 호스트)에서 파생한다(§4).

표준 라이브러리만 쓴다(관리망 안 HTTPS, 요청은 작고 드물다).
"""
from __future__ import annotations

import json
import os
import ssl
import urllib.error
import urllib.request
from typing import Dict, List, Optional, Set, Tuple

from services.tester_models import Topology, PeerPool, SipListener

SEED_TAG = 'cims-tester'
COLLECTIONS = ('local_nodes', 'remote_nodes', 'routes', 'route_sets', 'rules', 'rule_sets', 'routing_policies', 'acl_policies')


class TargetError(Exception):
    pass


class OamClient:
    def __init__(self, url: str, token: str):
        self.url = url.rstrip('/')
        self.token = token
        self._ctx = ssl.create_default_context()
        self._ctx.check_hostname = False
        self._ctx.verify_mode = ssl.CERT_NONE

    def _req(self, method: str, path: str, body=None, timeout: float = 20):
        data = json.dumps(body).encode('utf-8') if body is not None else None
        req = urllib.request.Request(self.url + path, data=data, method=method)
        req.add_header('Authorization', f'Bearer {self.token}')
        if data is not None:
            req.add_header('Content-Type', 'application/json')
        try:
            with urllib.request.urlopen(req, timeout=timeout, context=self._ctx) as r:
                raw = r.read().decode('utf-8', 'replace')
                return r.status, (json.loads(raw) if raw else {})
        except urllib.error.HTTPError as e:
            raw = e.read().decode('utf-8', 'replace')
            try:
                return e.code, json.loads(raw)
            except Exception:
                return e.code, {'error': raw[:200]}
        except Exception as e:
            raise TargetError(f'target oam {method} {path}: {e}')

    def deployments(self) -> List[dict]:
        st, out = self._req('GET', '/api/v1/deployments')
        if st != 200:
            raise TargetError(f'deployments {st}: {out}')
        rows = out.get('items') if isinstance(out, dict) else out
        return list(rows or [])

    def find_csp_deployment(self, dep_id: Optional[int] = None) -> int:
        rows = self.deployments()
        if dep_id is not None:
            if not any(int(r.get('id') or 0) == int(dep_id) for r in rows):
                raise TargetError(f'csp_deployment_id {dep_id} 가 대상 배포 목록에 없다')
            return int(dep_id)
        csp = [r for r in rows if str(r.get('package_name') or r.get('package') or '') == 'csp']
        if not csp:
            raise TargetError('대상 배포 목록에 패키지 csp 가 없다 — target.oam.csp_deployment_id 를 준다')
        # 설치·기동된 것을 우선
        csp.sort(key=lambda r: (str(r.get('status') or '') != 'running', -int(r.get('id') or 0)))
        return int(csp[0]['id'])

    def get_collection(self, dep_id: int, name: str) -> List[dict]:
        st, out = self._req('GET', f'/api/v1/deployments/{dep_id}/collection/{name}')
        if st != 200:
            raise TargetError(f'GET collection {name} {st}: {out}')
        return list((out or {}).get('records') or [])

    def put_collection(self, dep_id: int, name: str, records: List[dict], signal: bool = True) -> dict:
        st, out = self._req('PUT', f'/api/v1/deployments/{dep_id}/collection/{name}',
                            {'records': records, 'signal': signal}, timeout=40)
        if st != 200:
            raise TargetError(f'PUT collection {name} {st}: {out}')
        return out or {}


def resolve_token(oam, requester_token: Optional[str] = None) -> str:
    """대상 OAM 토큰 — ① 환경변수(`oam.token_env`, 기본 TESTER_OAM_TOKEN: 독립 형태처럼 대상 OAM 이 남의 것일 때)
    ② 없으면 **이 요청을 낸 운영자의 토큰**(동거 형태 — 대상 OAM 이 자기 base 라 그 토큰이 그대로 통한다. 시드·복원이 그 운영자의
    권한·감사 신원으로 수행된다). 요청자 토큰은 메모리에서만 쓰고 run 기록에 남기지 않는다."""
    env = getattr(oam, 'token_env', None) or 'TESTER_OAM_TOKEN'
    tok = (os.environ.get(env) or '').strip() or (requester_token or '').strip()
    if not tok:
        raise TargetError(f'target.oam 토큰이 없다 — 환경변수 {env} 에 대상 OAM 로그인 토큰을 두거나(독립 형태), '
                          f'콘솔/CLI 로그인 토큰으로 요청한다(동거 형태)')
    return tok


def token_from_env(oam) -> str:
    return resolve_token(oam, None)


# ──────────────────────────────────────────────────────────────────────────
#  시드 파생 — 토폴로지 → CSP 컬렉션 레코드
# ──────────────────────────────────────────────────────────────────────────

def csp_build(topology: Topology, requester_token: Optional[str] = None) -> Optional[str]:
    """대상 CSP 배포의 패키지 버전 문자열(예: 'csp 0.2.126 (dep 34)') — run 색인 `target_build`(비교 화면의 회귀 축).
    대상 OAM 이 없거나 토큰이 없으면 None. 실패는 run 을 막지 않는다."""
    oam = topology.oam_ref()
    if oam is None:
        return None
    try:
        client = OamClient(oam.url, resolve_token(oam, requester_token))
        rows = client.deployments()
        dep_id = client.find_csp_deployment(oam.csp_deployment_id)
        row = next((r for r in rows if int(r.get('id') or 0) == dep_id), None)
        if row is None:
            return None
        pkg = row.get('package') if isinstance(row.get('package'), dict) else {}
        ver = row.get('package_version') or row.get('version') or pkg.get('version')
        return f"csp {ver or '?'} (dep {dep_id})"
    except Exception:
        return None


def _tagged(rec: dict) -> bool:
    return SEED_TAG in (rec.get('tags') or [])


def _peer_pools(topology: Topology) -> Dict[str, PeerPool]:
    return {n: p for n, p in topology.pools.items() if getattr(p, 'kind', None) == 'peer'}


def seed_pools(topology: Topology, used_pools: Set[str]) -> Dict[str, PeerPool]:
    """시드할 피어 풀 — 시나리오 역할이 선언한 피어 풀만(seed.enabled). 같은 route_set 의 형제라도 역할로 선언하지 않으면
    시드하지 않는다 — 선언하지 않은 풀은 워커가 열지 않으므로 CSP 가 그쪽을 고르면 호가 죽는다(failover 시나리오는 dead 역할을
    명시해 무응답 피어를 연다)."""
    peers = _peer_pools(topology)
    return {n: peers[n] for n in used_pools if n in peers and peers[n].seed.enabled}


def pick_local_node(topology: Topology, current_local_nodes: List[dict], node_id: str, listener_id: str) -> Tuple[str, Optional[dict]]:
    """(local_node_ref, 새로 시드할 LocalNode 레코드 또는 None) — 피어 풀이 가리킨 수신점 하나에 대해.
    ① 항목 local_node 이름이 대상에 있으면 그 이름 ② 같은 protocol·port(bind_ip 가 그 주소 또는 0.0.0.0)의 enabled 레코드가 있으면
    그 이름 ③ 없으면 항목 그대로(edge 포함) 시드."""
    node = topology.target.nodes[node_id]
    l: SipListener = node.sip.listeners[listener_id]
    ip = topology.listener_ip(node_id, listener_id)
    name = Topology.local_node_name(listener_id, l)
    if l.local_node:
        for r in current_local_nodes:
            if r.get('name') == name:
                return name, None
    for r in current_local_nodes:
        if (str(r.get('protocol') or '').upper() == l.protocol.upper() and int(r.get('bind_port') or 0) == int(l.port)
                and r.get('enabled', True) and str(r.get('bind_ip') or '0.0.0.0') in (ip, '0.0.0.0', '')):
            return str(r['name']), None
    return name, {
        'name': name, 'enabled': True, 'is_primary': False, 'edge': l.edge,
        'bind_ip': ip, 'bind_port': int(l.port), 'protocol': l.protocol.upper(),
        'tags': [SEED_TAG], 'note': f'cims-tester {l.edge} listener {node_id}:{listener_id}',
    }


def number_range(p: PeerPool) -> Optional[str]:
    """신원 범위(e164_range/did_range) → pbx/mgcf 번호 대역 라우팅 규칙(`req_uri_user in_range`, sip_service_model.md §2-5)의 값 "lo-hi".
    접두(공통 prefix)가 아니라 정확한 대역이라 10진 경계에 맞지 않는 블록(1010~1014 / 1015~1019)을 두 풀로 나눠도 규칙이 겹치지 않는다.
    CSP 는 번역 뒤 +E.164 착신과 비교하므로 범위도 국제형으로 적는 것이 맞다(국내형 DID 범위는 국내형 다이얼에만 걸린다)."""
    rng = p.identities.e164_range or p.identities.did_range
    if not rng:
        return None
    return f'{rng[0]}-{rng[1]}'


def number_prefix(p: PeerPool) -> Optional[str]:
    """신원 범위의 공통 접두 — 표시·검산용(시드 규칙은 number_range 를 쓴다)."""
    rng = p.identities.e164_range or p.identities.did_range
    if not rng:
        return None
    lo, hi = str(rng[0]), str(rng[1])
    n = 0
    while n < min(len(lo), len(hi)) and lo[n] == hi[n]:
        n += 1
    return lo[:n] or None


def derive_records(topology: Topology, pools: Dict[str, PeerPool], local_node_refs: Dict[str, str]) -> Dict[str, List[dict]]:
    """피어 풀 → 컬렉션별 새 레코드(태그 cims-tester). 이름 규약: tester-<종류>-<풀|route_set>. local_node_refs = 풀 → 접속점 이름.
    Route 는 `inbound_auth: none`(신뢰 피어 — CSP 가 이 Route 로 식별한 요청은 Digest 없이 받는다), ACL 은 `scope=route` 로 그 피어에만.
    pbx `register` 가 있으면 등록형 트렁크 — `inbound_auth: digest` + 트렁크 계정(auth_user·auth_ha1|auth_password·auth_realm·register_expires).

    매칭 규칙 = 도메인(`req_uri_host eq` — ibcf, UE 가 user@피어도메인 을 다이얼) OR 번호 대역(`req_uri_user in_range lo-hi` — pbx/mgcf,
    UE 가 DID/E.164 를 그대로 다이얼, BGCF 식 번호 라우팅 — 번역 뒤 +E.164 와 비교). 두 규칙을 같은 RouteSet 의 match 집합에 OR 로 넣는다."""
    out: Dict[str, List[dict]] = {c: [] for c in COLLECTIONS}
    by_set: Dict[str, List[Tuple[str, PeerPool]]] = {}
    match_rules: Dict[str, List[str]] = {}
    for name, p in pools.items():
        rn = f'tester-rn-{name}'
        rt = f'tester-r-{name}'
        bind_ip = topology.pool_bind_ip(name)
        rn_rec = {
            'name': rn, 'enabled': True, 'ip': bind_ip, 'port': int(p.bind.port), 'protocol': p.bind.protocol.upper(),
            'remote_domain': p.domain, 'srv_lookup': False, 'dns_fallback': False, 'tls_verify': False,
            'tags': [SEED_TAG, p.profile], 'note': f'cims-tester peer pool {name}',
        }
        if p.profile == 'pbx':
            # IP-PBX 는 G.711 필수(SIPconnect 2.0) — 가입자→PBX 오퍼에 CSP 가 끼워 넣을 코덱(cmp.md §11.2). 피어 오퍼 코덱 목록의 G.711 만(AMR-WB 는 서비스 코덱)
            g711 = [c.upper() for c in (p.codecs or ['PCMA', 'PCMU']) if c.upper() in ('PCMA', 'PCMU')]
            if g711:
                rn_rec['transcode_codecs'] = g711
        out['remote_nodes'].append(rn_rec)
        local_node_ref = local_node_refs[name]
        route = {
            'name': rt, 'enabled': True, 'local_node_ref': local_node_ref, 'remote_node_ref': rn, 'inbound_auth': 'none',
            'register_to_remote': False, 'tags': [SEED_TAG], 'note': f'cims-tester {local_node_ref} ↔ {name}',
        }
        if p.dial_plan is not None:
            # 인바운드 다이얼 플랜(sip_service_model.md §2-10) — 이 피어가 국내형 DID 로 보낸 착신을 CSP 가 +E.164 로 번역
            route.update({'country_code': p.dial_plan.country_code, 'national_prefix': p.dial_plan.national_prefix,
                          'international_prefix': p.dial_plan.international_prefix})
        if p.trunk_register is not None:
            # 등록형 트렁크(SIPconnect 2.0 §8) — 피어가 이 계정으로 REGISTER 한다: Route 는 inbound_auth=digest + 트렁크 계정(auth_user·H(A1)|password·realm).
            #   CSP 는 바인딩(REGISTER 소스)을 다음 홉으로 쓰고 바인딩 없으면 Route dead — 비밀은 환경변수에서(컴파일과 같은 해석)
            from services.tester_compile import trunk_register_for
            tr = trunk_register_for(name, p, None)
            route.update({'inbound_auth': 'digest', 'auth_user': tr.user, 'auth_realm': tr.realm or '',
                          'auth_ha1': tr.ha1 or '', 'auth_password': tr.password or '', 'register_expires': int(tr.expires)})
        out['routes'].append(route)
        out['rules'].append({
            'name': f'tester-rule-{name}-domain', 'enabled': True, 'field': 'req_uri_host', 'op': 'eq', 'value': p.domain,
            'tags': [SEED_TAG, 'routing'],
        })
        rules = [f'tester-rule-{name}-domain']
        rng = number_range(p) if p.dial == 'number' else None
        if rng:
            out['rules'].append({
                'name': f'tester-rule-{name}-range', 'enabled': True, 'field': 'req_uri_user', 'op': 'in_range', 'value': rng,
                'tags': [SEED_TAG, 'routing'],
            })
            rules.append(f'tester-rule-{name}-range')
        match_rules[name] = rules
        if p.seed.acl:
            out['rules'].append({
                'name': f'tester-rule-{name}-src', 'enabled': True, 'field': 'src_ip', 'op': 'eq', 'value': bind_ip,
                'tags': [SEED_TAG, 'acl'],
            })
            out['rule_sets'].append({
                'name': f'tester-rs-{name}-src', 'enabled': True, 'combinator': 'AND',
                'members': [{'rule_ref': f'tester-rule-{name}-src', 'negate': False}], 'tags': [SEED_TAG],
            })
            # 이 피어의 Route 에만(scope=route) — CSP 가 (접속점, 소스 주소) 로 인바운드 Route 를 식별하므로 같은 접속점의 UE 나
            #   다른 피어에는 걸리지 않는다(local_node/global 이면 같은 호스트의 다른 트래픽까지 걸린다)
            out['acl_policies'].append({
                'name': f'tester-acl-{name}', 'enabled': True, 'priority': 10, 'match_rule_set_ref': f'tester-rs-{name}-src',
                'scope': 'route', 'scope_ref': rt, 'action': p.seed.acl, 'tags': [SEED_TAG],
            })
        by_set.setdefault(p.seed.route_set or name, []).append((name, p))
    for rs_name, members in by_set.items():
        first = members[0][1]
        out['route_sets'].append({
            'name': f'tester-rs-{rs_name}', 'enabled': True, 'distribution_policy': first.seed.distribution,
            'members': [{'route_ref': f'tester-r-{n}', 'priority': int(p.seed.priority), 'weight': int(p.seed.weight)}
                        for n, p in sorted(members, key=lambda x: x[1].seed.priority)],
            'health_check_mode': 'none', 'fallback_policy': 'reject', 'tags': [SEED_TAG],
        })
        out['rule_sets'].append({
            'name': f'tester-rs-{rs_name}-match', 'enabled': True, 'combinator': 'OR',
            'members': [{'rule_ref': r, 'negate': False} for n, _ in members for r in match_rules[n]], 'tags': [SEED_TAG],
        })
        out['routing_policies'].append({
            'name': f'tester-rp-{rs_name}', 'enabled': True, 'priority': 50, 'match_rule_set_ref': f'tester-rs-{rs_name}-match',
            'target_type': 'route_set', 'target_ref': f'tester-rs-{rs_name}', 'transform_rule_set_refs': [],
            'fail_action': 'reject', 'tags': [SEED_TAG],
        })
    return out


class CspSeeder:
    """run 하나의 시드 수명 — apply() 로 넣고 restore() 로 되돌린다. 컬렉션 순서 = COLLECTIONS(참조 대상이 먼저), 마지막 PUT 만 signal."""

    def __init__(self, client: OamClient, dep_id: int, topology: Topology, pools: Dict[str, PeerPool]):
        self.client, self.dep_id, self.topology, self.pools = client, dep_id, topology, pools
        self.snapshot: Dict[str, List[dict]] = {}
        self.applied: Dict[str, int] = {}
        self.local_node_refs: Dict[str, str] = {}   # 풀 → 접속점(LocalNode) 이름

    @property
    def local_node_ref(self) -> str:
        """run 노트용 — 쓰인 접속점 이름들(중복 제거, 순서 유지)."""
        seen = []
        for v in self.local_node_refs.values():
            if v not in seen:
                seen.append(v)
        return ','.join(seen)

    @classmethod
    def for_run(cls, topology: Topology, used_pools: Set[str], requester_token: Optional[str] = None) -> Optional['CspSeeder']:
        pools = seed_pools(topology, used_pools)
        if not pools:
            return None
        if topology.target.kind != 'cims':
            return None   # ims/pbx 대상은 시드 없음 — 라우팅은 대상 쪽에서 미리(§4)
        oam = topology.oam_ref()
        if oam is None:
            raise TargetError('피어 풀 시드에는 대상 oam 노드가 필요하다 (또는 풀의 seed.enabled=false 로 수동 구성)')
        client = OamClient(oam.url, resolve_token(oam, requester_token))
        dep = client.find_csp_deployment(oam.csp_deployment_id)
        return cls(client, dep, topology, pools)

    def apply(self) -> Dict[str, int]:
        for c in COLLECTIONS:
            self.snapshot[c] = self.client.get_collection(self.dep_id, c)
        ln_new: Dict[str, dict] = {}
        try:
            for pn, (nid, lid, _l) in self.topology.peer_listeners(list(self.pools)).items():
                ref, rec = pick_local_node(self.topology, self.snapshot['local_nodes'], nid, lid)
                self.local_node_refs[pn] = ref
                if rec is not None:
                    ln_new.setdefault(ref, rec)
        except ValueError as e:
            raise TargetError(str(e))
        new = derive_records(self.topology, self.pools, self.local_node_refs)
        new['local_nodes'].extend(ln_new.values())
        puts: List[Tuple[str, List[dict]]] = []
        for c in COLLECTIONS:
            keep = [r for r in self.snapshot[c] if not _tagged(r)]
            merged = keep + new[c]
            if not new[c] and merged == self.snapshot[c]:
                continue
            puts.append((c, merged))
        for i, (c, merged) in enumerate(puts):
            self.client.put_collection(self.dep_id, c, merged, signal=(i == len(puts) - 1))
            self.applied[c] = len(new[c])
        return dict(self.applied)

    def restore(self) -> List[str]:
        errs = []
        touched = [c for c in COLLECTIONS if c in self.applied]
        for i, c in enumerate(touched):
            try:
                self.client.put_collection(self.dep_id, c, self.snapshot[c], signal=(i == len(touched) - 1))
            except TargetError as e:
                errs.append(str(e))
        return errs
