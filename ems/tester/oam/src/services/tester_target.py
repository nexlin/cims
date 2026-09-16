"""대상(SUT) OAM 클라이언트 — CSP 컬렉션 시드/복원 (test_instrument.md §3.2·§6.2).

피어 풀은 CSP 가 "아는" 상대여야 한다 — remote_nodes(피어 주소)·routes(접속점↔피어)·route_sets(failover/round_robin)·
rules/rule_sets/routing_policies(Request-URI 도메인 → RouteSet)·acl_policies(소스 IP allow/deny). 계측기는 토폴로지의
피어 풀 정의에서 이 레코드를 **파생**해 run 전에 대상 OAM 의 컬렉션 API(`PUT /api/v1/deployments/{id}/collection/{name}`)로
넣고(SIGUSR1 reload), run 이 끝나면 저장해 둔 원본으로 되돌린다. 레코드 스키마는 CSP 의 것 그대로(sip_service_model.md §2) —
계측기 쪽 번역 계층을 두지 않는다. 시드 레코드는 tags 에 `cims-tester` 를 달아 재실행 시 남은 것을 걷어낸다.

접속점(LocalNode) = `target.csp.peering` — 이름이 대상에 있으면 그 레코드, 없으면 그 이름으로 edge=peering 접속점을 시드한다
(복원 시 함께 사라진다). peering 이 없으면 access UDP 접속점(csp.udp 포트의 UDP LocalNode)을 route 의 local_node_ref 로 쓴다.

표준 라이브러리만 쓴다(관리망 안 HTTPS, 요청은 작고 드물다).
"""
from __future__ import annotations

import json
import os
import ssl
import urllib.error
import urllib.request
from typing import Dict, List, Optional, Set, Tuple

from services.tester_models import Topology, PeerPool

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


def token_from_env(oam) -> str:
    env = getattr(oam, 'token_env', None) or 'TESTER_OAM_TOKEN'
    tok = (os.environ.get(env) or '').strip()
    if not tok:
        raise TargetError(f'target.oam 토큰이 없다 — 환경변수 {env} 에 대상 OAM 로그인 토큰을 둔다')
    return tok


# ──────────────────────────────────────────────────────────────────────────
#  시드 파생 — 토폴로지 → CSP 컬렉션 레코드
# ──────────────────────────────────────────────────────────────────────────

def csp_build(topology: Topology) -> Optional[str]:
    """대상 CSP 배포의 패키지 버전 문자열(예: 'csp 0.2.126 (dep 34)') — run 색인 `target_build`(비교 화면의 회귀 축).
    대상 OAM 이 없거나 토큰이 없으면 None. 실패는 run 을 막지 않는다."""
    oam = topology.target.oam
    if oam is None:
        return None
    try:
        client = OamClient(oam.url, token_from_env(oam))
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


def pick_local_node(topology: Topology, current_local_nodes: List[dict]) -> Tuple[str, Optional[dict]]:
    """(local_node_ref, 새로 시드할 LocalNode 레코드 또는 None)."""
    csp = topology.target.csp
    pr = csp.peering
    if pr is not None:
        for r in current_local_nodes:
            if r.get('name') == pr.local_node:
                return pr.local_node, None
        return pr.local_node, {
            'name': pr.local_node, 'enabled': True, 'is_primary': False, 'edge': 'peering',
            'bind_ip': pr.ip or csp.ip, 'bind_port': int(pr.port), 'protocol': pr.protocol.upper(),
            'tags': [SEED_TAG], 'note': 'cims-tester peering listener',
        }
    for r in current_local_nodes:
        if str(r.get('protocol') or '').upper() == 'UDP' and int(r.get('bind_port') or 0) == int(csp.udp) and r.get('enabled', True):
            return str(r['name']), None
    for r in current_local_nodes:
        if r.get('is_primary'):
            return str(r['name']), None
    raise TargetError('대상 local_nodes 에서 route 의 접속점을 고를 수 없다 — target.csp.peering 을 준다')


def number_prefix(p: PeerPool) -> Optional[str]:
    """신원 범위(e164_range/did_range)의 공통 접두 — pbx/mgcf 번호 라우팅 규칙(req_uri_user prefix)의 값. 접두가 비면 None."""
    rng = p.identities.e164_range or p.identities.did_range
    if not rng:
        return None
    lo, hi = str(rng[0]), str(rng[1])
    n = 0
    while n < min(len(lo), len(hi)) and lo[n] == hi[n]:
        n += 1
    return lo[:n] or None


def derive_records(topology: Topology, pools: Dict[str, PeerPool], local_node_ref: str) -> Dict[str, List[dict]]:
    """피어 풀 → 컬렉션별 새 레코드(태그 cims-tester). 이름 규약: tester-<종류>-<풀|route_set>.

    매칭 규칙 = 도메인(`req_uri_host eq` — ibcf, UE 가 user@피어도메인 을 다이얼) OR 번호 접두(`req_uri_user prefix` — pbx/mgcf,
    UE 가 DID/E.164 를 그대로 다이얼, BGCF 식 번호 라우팅). 두 규칙을 같은 RouteSet 의 match 집합에 OR 로 넣는다."""
    out: Dict[str, List[dict]] = {c: [] for c in COLLECTIONS}
    by_set: Dict[str, List[Tuple[str, PeerPool]]] = {}
    match_rules: Dict[str, List[str]] = {}
    for name, p in pools.items():
        rn = f'tester-rn-{name}'
        rt = f'tester-r-{name}'
        out['remote_nodes'].append({
            'name': rn, 'enabled': True, 'ip': p.bind.ip, 'port': int(p.bind.port), 'protocol': p.bind.protocol.upper(),
            'remote_domain': p.domain, 'srv_lookup': False, 'dns_fallback': False, 'tls_verify': False,
            'tags': [SEED_TAG, p.profile], 'note': f'cims-tester peer pool {name}',
        })
        out['routes'].append({
            'name': rt, 'enabled': True, 'local_node_ref': local_node_ref, 'remote_node_ref': rn,
            'register_to_remote': False, 'tags': [SEED_TAG], 'note': f'cims-tester {local_node_ref} → {name}',
        })
        out['rules'].append({
            'name': f'tester-rule-{name}-domain', 'enabled': True, 'field': 'req_uri_host', 'op': 'eq', 'value': p.domain,
            'tags': [SEED_TAG, 'routing'],
        })
        rules = [f'tester-rule-{name}-domain']
        prefix = number_prefix(p) if p.dial == 'number' else None
        if prefix:
            out['rules'].append({
                'name': f'tester-rule-{name}-prefix', 'enabled': True, 'field': 'req_uri_user', 'op': 'prefix', 'value': prefix,
                'tags': [SEED_TAG, 'routing'],
            })
            rules.append(f'tester-rule-{name}-prefix')
        match_rules[name] = rules
        if p.seed.acl:
            out['rules'].append({
                'name': f'tester-rule-{name}-src', 'enabled': True, 'field': 'src_ip', 'op': 'eq', 'value': p.bind.ip,
                'tags': [SEED_TAG, 'acl'],
            })
            out['rule_sets'].append({
                'name': f'tester-rs-{name}-src', 'enabled': True, 'combinator': 'AND',
                'members': [{'rule_ref': f'tester-rule-{name}-src', 'negate': False}], 'tags': [SEED_TAG],
            })
            # 피어 신뢰는 피어링 접속점에서 판정한다(scope=local_node) — global 이면 같은 호스트의 UE 트래픽까지 걸린다
            out['acl_policies'].append({
                'name': f'tester-acl-{name}', 'enabled': True, 'priority': 10, 'match_rule_set_ref': f'tester-rs-{name}-src',
                'scope': 'local_node', 'scope_ref': local_node_ref, 'action': p.seed.acl, 'tags': [SEED_TAG],
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
        self.local_node_ref: Optional[str] = None

    @classmethod
    def for_run(cls, topology: Topology, used_pools: Set[str]) -> Optional['CspSeeder']:
        pools = seed_pools(topology, used_pools)
        if not pools:
            return None
        oam = topology.target.oam
        if oam is None:
            raise TargetError('피어 풀 시드에는 target.oam 이 필요하다 (또는 풀의 seed.enabled=false 로 수동 구성)')
        client = OamClient(oam.url, token_from_env(oam))
        dep = client.find_csp_deployment(oam.csp_deployment_id)
        return cls(client, dep, topology, pools)

    def apply(self) -> Dict[str, int]:
        for c in COLLECTIONS:
            self.snapshot[c] = self.client.get_collection(self.dep_id, c)
        ln_ref, ln_new = pick_local_node(self.topology, self.snapshot['local_nodes'])
        self.local_node_ref = ln_ref
        new = derive_records(self.topology, self.pools, ln_ref)
        if ln_new is not None:
            new['local_nodes'].append(ln_new)
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
