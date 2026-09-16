"""계측기 계약 모델 — 시나리오 3층(토폴로지·시나리오·부하 프로파일) + 컨트롤러↔워커 메시지.

test_instrument.md §4·§6 의 정본. 여기 pydantic 모델이 SoT 이고 `schema/*.json` 은
`bin/gen-schemas` 가 이 모델에서 생성한 파생물이다(단위시험이 동기화를 검사한다).
워커(C++)는 JSON 스키마를 읽어 같은 계약을 검증한다.

원칙: 모든 모델은 `extra='forbid'` — 오타 난 키가 조용히 무시되면 "기대치가 안 걸린 시험"이
PASS 로 보인다. 지표 이름은 RFC 6076 어휘(`rrd_ms`·`srd_ms`·`sdd_ms`·`ser_pct`·`scr_pct`)를
그대로 쓴다(§5).
"""
from __future__ import annotations

import re
from typing import Dict, List, Literal, Optional, Tuple, Union

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator


class _Strict(BaseModel):
    model_config = ConfigDict(extra='forbid', populate_by_name=True)


# ──────────────────────────────────────────────────────────────────────────
#  1. 토폴로지 — 호스트 › 워커·대상 노드 › 풀 (§4)
#     주소의 기본값은 hosts{} 에서 온다 — 워커 URL·노드 API/OAM/DB 포트·피어 수신점 ip 는 호스트에서 파생되고,
#     대상 노드는 `addr`(VIP — A/S 이중화·다중 IP 호스트), SIP 수신점은 항목별 `ip` 로 덮어쓸 수 있다
#     (사슬: listener.ip → node.addr → host.ip). SIP 노드의 수신점은 N 개(`sip.listeners{}` — CSP local_nodes 와 1:1,
#     edge=access 는 UE 가 등록·발신하는 곳, edge=peering 은 피어의 다음 홉. 피어 풀은 어느 항목이든 가리킬 수 있다 —
#     CSP 는 접속점 edge 가 아니라 Route 로 피어를 신뢰한다, sip_service_model.md §4).
#     대상은 역할별 노드 집합(sip/tas/media/subscriber/oam/db) — CIMS·타 IMS·IP-PBX 가 같은 레코드 구조다.
#     풀 하나 = 워커 하나(`worker`). 워커 여럿은 워커마다 풀 + 같은 `group`(논리 풀 이름).
# ──────────────────────────────────────────────────────────────────────────

Transport = Literal['udp', 'tcp', 'tls']
ListenerEdge = Literal['access', 'peering']
SrtpMode = Literal['off', 'optional', 'required']
PeerProfile = Literal['ibcf', 'pbx', 'mgcf']
ObserveSource = Literal['oam_stats', 'oam_alarms', 'agent_heartbeat', 'ssh_proc']
NodeRole = Literal['sip', 'tas', 'media', 'subscriber', 'oam', 'db']
TargetKind = Literal['cims', 'ims', 'pbx']

_ID_RE = r'^[a-z][a-z0-9_]*$'


class HostSsh(_Strict):
    user: str
    key_env: str = Field(description='SSH 개인키 경로를 담은 환경변수 이름 — 비밀은 레코드에 두지 않는다')
    port: int = Field(default=22, ge=1, le=65535)


class Host(_Strict):
    """서버 — 주소·SSH 자격의 유일한 자리. 계측기/대상/동거 구분은 필드가 아니라 그 위에 무엇이 있느냐(파생)."""
    name: Optional[str] = None
    ip: str = Field(min_length=1)
    ssh: Optional[HostSsh] = Field(default=None, description='있으면 노드 procs 의 CPU/메모리를 SSH 로 관측(stop_on.target_cpu_pct 원천)')


class WorkerMedia(_Strict):
    samples: List[str] = Field(default_factory=list, description='워커가 보유한 미디어 샘플 id(§7 ⓖ) — health 보고와 대조')
    max_rtp_streams: Optional[int] = Field(default=None, ge=0)


class Worker(_Strict):
    """호스트 위의 cims-tester-worker 프로세스 — url = http://<host.ip>:<port> 파생."""
    name: str = Field(pattern=_ID_RE)
    host: str
    port: int = Field(default=7100, ge=1, le=65535)
    cpus: Optional[int] = Field(default=None, ge=1)
    media: Optional[WorkerMedia] = None


class SipListener(_Strict):
    """SIP 수신점 하나 = 대상 LocalNode 하나. `edge`: access = UE 가 등록·발신하는 곳 / peering = 피어 풀의 다음 홉(CSP `edge=peering`
    접속점은 Route 있는 피어만 받는다). `ip` 가 비면 노드 `addr` → 호스트 ip. `local_node` = cims 대상 local_nodes 의 이름 —
    시드가 그 이름을 찾고, 없으면 그 이름으로(비면 `cims-tester-<수신점 id>`) 만든다."""
    edge: ListenerEdge = 'access'
    ip: Optional[str] = Field(default=None, min_length=1, description='비면 노드 addr → 호스트 ip')
    port: int = Field(ge=1, le=65535)
    protocol: Transport = 'udp'
    local_node: Optional[str] = Field(default=None, description='cims: 대상 local_nodes 이름 — 비면 cims-tester-<id>')


class NodeSip(_Strict):
    """SIP 노드의 수신점 N 개 + 도메인. 입력이 이전 꼴(`access{udp,tcp,tls,domains}`·`peering{port,protocol,local_node}`)이면
    `normalize_sip_block` 이 수신점 항목 `udp`/`tcp`/`tls`/`peering` 으로 승계한다(store 도 읽을 때 같은 함수로 바꿔 저장)."""
    domains: List[str] = Field(default_factory=list, description='첫 항목 = 기본 홈 도메인, "ptt" 가 든 항목 = PTT 풀 도메인')
    listeners: Dict[str, SipListener] = Field(default_factory=dict)

    @model_validator(mode='before')
    @classmethod
    def _legacy(cls, v):
        return normalize_sip_block(v) if isinstance(v, dict) else v

    @field_validator('listeners')
    @classmethod
    def _ids(cls, v):
        for k in v:
            if not re.match(_ID_RE, k):
                raise ValueError(f'수신점 id 는 소문자·숫자·_ 만: {k!r}')
        return v

    def by_edge(self, edge: str) -> Dict[str, SipListener]:
        return {k: l for k, l in self.listeners.items() if l.edge == edge}


def normalize_sip_block(sip: dict) -> dict:
    """노드 `sip` 블록의 이전 꼴 → 수신점 목록. 이미 새 꼴이면 그대로. 사본을 돌려준다."""
    if not isinstance(sip, dict) or ('access' not in sip and 'peering' not in sip):
        return sip
    out = {k: v for k, v in sip.items() if k not in ('access', 'peering')}
    ls = dict(out.get('listeners') or {})
    doms = list(out.get('domains') or [])
    acc = sip.get('access')
    if isinstance(acc, dict):
        for t in ('udp', 'tcp', 'tls'):
            if acc.get(t):
                ls.setdefault(t, {'edge': 'access', 'port': acc[t], 'protocol': t})
        if not doms:
            doms = list(acc.get('domains') or [])
    pr = sip.get('peering')
    if isinstance(pr, dict) and pr.get('port'):
        row = {'edge': 'peering', 'port': pr['port'], 'protocol': pr.get('protocol') or 'udp'}
        if pr.get('local_node'):
            row['local_node'] = pr['local_node']
        ls.setdefault('peering', row)
    out['listeners'] = ls
    if doms:
        out['domains'] = doms
    return out


def normalize_topology_doc(doc: dict) -> dict:
    """토폴로지 문서의 노드 `sip` 블록을 전부 새 꼴로(사본). store 가 읽을 때·저장할 때 부른다."""
    if not isinstance(doc, dict):
        return doc
    nodes = ((doc.get('target') or {}).get('nodes') or {})
    changed = False
    new_nodes = {}
    for nid, n in nodes.items():
        if isinstance(n, dict) and isinstance(n.get('sip'), dict) and ('access' in n['sip'] or 'peering' in n['sip']):
            n = dict(n)
            n['sip'] = normalize_sip_block(n['sip'])
            changed = True
        new_nodes[nid] = n
    if not changed:
        return doc
    out = dict(doc)
    out['target'] = dict(doc['target'])
    out['target']['nodes'] = new_nodes
    return out


class NodeTas(_Strict):
    port: Optional[int] = Field(default=None, ge=1, le=65535)


class NodeMedia(_Strict):
    rtp_range: Optional[List[int]] = Field(default=None, min_length=2, max_length=2, description='있어야 미디어 leg 지표를 이 노드에 귀속')
    control: Optional[int] = Field(default=None, ge=1, le=65535)


class NodeApi(_Strict):
    port: int = Field(ge=1, le=65535)
    tls: bool = True


class NodeOam(_Strict):
    """CIMS 전용 — 통계·알람 관측 + 컬렉션 시드. url = https://<host.ip>:<port>."""
    port: int = Field(default=4419, ge=1, le=65535)
    tls: bool = True
    token_env: Optional[str] = Field(default=None, description='토큰을 담은 환경변수 이름(기본 TESTER_OAM_TOKEN)')
    csp_deployment_id: Optional[int] = Field(default=None, ge=1, description='대상 CSP 의 배포 id — 비면 배포 목록에서 패키지 csp 를 찾는다')
    observe: List[ObserveSource] = Field(default_factory=list)


class NodeDb(_Strict):
    port: int = Field(default=3306, ge=1, le=65535)
    name: str = 'cims'
    user_env: Optional[str] = None
    password_env: Optional[str] = None


class TargetNode(_Strict):
    """역할(role)별 노드 — 설정 블록이 역할마다 다르다. procs = 호스트 SSH 관측이 볼 프로세스 이름."""
    role: NodeRole
    host: str
    addr: Optional[str] = Field(default=None, min_length=1,
                                description='노드 주소 — 비면 호스트 ip. A/S 이중화의 VIP 나 다중 IP 호스트의 서비스 주소')
    fn: Optional[str] = Field(default=None, description='표시용 — CSP · P-CSCF · IBCF · MRF …')
    label: Optional[str] = None
    procs: List[str] = Field(default_factory=list)
    sip: Optional[NodeSip] = None
    tas: Optional[NodeTas] = None
    media: Optional[NodeMedia] = None
    api: Optional[NodeApi] = None
    oam: Optional[NodeOam] = None
    db: Optional[NodeDb] = None

    @model_validator(mode='after')
    def _block_by_role(self):
        blocks = {'sip': 'sip', 'tas': 'tas', 'media': 'media', 'subscriber': 'api', 'oam': 'oam', 'db': 'db'}
        for role, fld in blocks.items():
            if role != self.role and getattr(self, fld) is not None:
                raise ValueError(f'role={self.role} 노드에 {fld} 블록은 두지 않는다')
        if self.role == 'oam' and self.oam is None:
            self.oam = NodeOam()
        if self.role == 'db' and self.db is None:
            self.db = NodeDb()
        return self


class Target(_Strict):
    name: str
    kind: TargetKind = Field(default='cims', description='cims = oam 노드로 컬렉션 시드·target_build · ims/pbx = 시드 없음')
    nodes: Dict[str, TargetNode] = Field(default_factory=dict)

    @field_validator('nodes')
    @classmethod
    def _node_ids(cls, v):
        for k in v:
            if not re.match(_ID_RE, k):
                raise ValueError(f'노드 id 는 소문자·숫자·_ 만: {k!r}')
        return v


class DbSource(_Strict):
    db: str = Field(description='신원 원천 노드 id — role=db 또는 api 있는 subscriber 노드')
    table: Literal['volte_subscriptions', 'voip_subscriptions', 'ptt_subscriptions']
    offset: int = Field(default=0, ge=0)
    count: int = Field(ge=1)


class CredsSource(_Strict):
    creds: str = Field(description='creds JSONL 경로(cspsim -creds 승계) — scenarios/ 상대 또는 절대')
    count: Optional[int] = Field(default=None, ge=1)


class _PoolBase(_Strict):
    worker: str = Field(description='이 풀이 놓인 워커(풀 하나 = 워커 하나)')
    group: Optional[str] = Field(default=None, pattern=_ID_RE,
                                 description='논리 풀 이름 — 워커 여럿에 나눌 때 워커마다 풀 + 같은 group. 시나리오 roles.X.pool 이 참조')


class UePool(_PoolBase):
    kind: Literal['ue']
    access: str = Field(description='접속점 노드 id — edge=access 수신점이 있는 SIP 노드')
    listener: Optional[str] = Field(default=None, description='그 노드의 수신점 id — 비면 transport 와 같은 protocol 의 첫 access 수신점')
    source: Union[DbSource, CredsSource]
    transport: Transport = Field(default='udp', description='listener 를 주면 그 protocol 로 맞춘다(둘 다 주고 다르면 오류)')
    srtp: SrtpMode = 'off'
    register_expires: int = Field(default=3600, ge=60)
    prack: bool = Field(default=False, description='RFC 3262 100rel — 발신 INVITE 에 Supported/Require: 100rel, RSeq 1xx 에 PRACK (mgcf early media 시험)')
    dtmf: bool = Field(default=True, description='RFC 4733 telephone-event 를 오퍼/echo — dtmf 단계의 전제')


class PeerBind(_Strict):
    """피어 수신점 — ip 가 비면 워커 호스트 주소(다중 IP 호스트면 여기서 고른다)."""
    ip: Optional[str] = Field(default=None, min_length=1)
    port: int = Field(ge=1, le=65535)
    protocol: Transport = 'udp'


class PeerIdentities(_Strict):
    e164_range: Optional[List[str]] = Field(default=None, min_length=2, max_length=2)
    did_range: Optional[List[str]] = Field(default=None, min_length=2, max_length=2)
    ext_len: Optional[int] = Field(default=None, ge=2, le=8)
    count: Optional[int] = Field(default=None, ge=1, description='범위 앞에서부터 쓸 신원 수 — 생략=범위 전부')

    @model_validator(mode='after')
    def _one_range(self):
        if not (self.e164_range or self.did_range):
            raise ValueError('identities 에 e164_range 또는 did_range 하나는 필요하다')
        return self


class PeerRegister(_Strict):
    """pbx 트렁크 REGISTER(SIPconnect 2.0 §8 등록 모드) — 계정 하나가 DID 범위를 대표. 피어가 가리킨 수신점(같은 주소의 같은
    transport access 수신점)으로 Digest 등록."""
    user: str
    ha1_env: Optional[str] = Field(default=None, description='H(A1) 을 담은 환경변수 — 비밀은 YAML 에 두지 않는다')
    password_env: Optional[str] = Field(default=None, description='평문 비밀번호 환경변수 — ha1_env 가 없을 때')
    realm: Optional[str] = Field(default=None, description='Digest realm·To/From host — 비면 접속점 기본 도메인')
    expires: int = Field(default=3600, ge=60)

    @model_validator(mode='after')
    def _secret(self):
        if not (self.ha1_env or self.password_env):
            raise ValueError('register 에 ha1_env 또는 password_env 하나는 필요하다')
        return self


class PeerSeed(_Strict):
    """대상 CSP 컬렉션 시드 — 이 피어를 CSP 가 알게 하는 remote_node/route/rule/routing_policy 를 run 전에 넣고 끝에 복원한다
    (§3.2). 같은 `route_set` 을 가진 피어 풀은 한 RouteSet 의 멤버(priority/weight) — failover·round_robin 시험."""
    enabled: bool = True
    route_set: Optional[str] = Field(default=None, description='RouteSet 이름 — 생략=풀 이름')
    distribution: Literal['failover', 'round_robin', 'weighted', 'hash_by_caller'] = 'failover'
    priority: int = Field(default=100, ge=0)
    weight: int = Field(default=1, ge=1)
    acl: Optional[Literal['allow', 'deny']] = Field(default=None,
                                                    description='이 피어 소스 IP 에 대한 ACL — deny 면 403 기대')


class PeerPool(_PoolBase):
    kind: Literal['peer']
    peering: str = Field(description='다음 홉 노드 id — SIP 노드(수신점 하나 이상)')
    listener: Optional[str] = Field(default=None,
                                    description='그 노드의 수신점 id — 비면 첫 edge=peering 수신점, 없으면 bind.protocol 과 같은 첫 수신점. '
                                                'access 수신점도 된다(CSP 는 Route 로 피어를 신뢰한다)')
    profile: PeerProfile
    bind: PeerBind
    domain: str
    identities: PeerIdentities
    trunk_register: Optional[PeerRegister] = Field(default=None, alias='register',
                                                    description='pbx 트렁크 REGISTER — YAML 키는 register')
    codecs: Optional[List[str]] = Field(
        default=None,
        description='오퍼 코덱(우선순위 순). 생략=프로파일 기본 — ibcf/mgcf: AMR-WB,AMR,PCMU,PCMA · pbx: PCMA,PCMU (§3.2)')
    answer: Literal['normal', 'silent'] = Field(default='normal', description='silent = 착신 INVITE 무응답(죽은 피어 — failover 시험)')
    prack: Optional[bool] = Field(default=None, description='RFC 3262 100rel/PRACK — 생략=프로파일 기본(ibcf/mgcf 켬, pbx 끔)')
    dtmf: bool = Field(default=True, description='RFC 4733 telephone-event 오퍼/echo')
    seed: PeerSeed = Field(default_factory=PeerSeed)

    @property
    def dial(self) -> str:
        """UE 가 이 피어 신원을 부르는 꼴 — ibcf 는 user@도메인(Request-URI host 규칙), pbx/mgcf 는 번호 그대로(DID/E.164 prefix 규칙)."""
        return 'domain' if self.profile == 'ibcf' else 'number'


class RealUePool(_PoolBase):
    kind: Literal['real-ue']
    access: str
    listener: Optional[str] = None
    source: CredsSource
    transport: Transport = 'tls'
    srtp: SrtpMode = 'optional'


Pool = Union[UePool, PeerPool, RealUePool]


class LayoutRegion(_Strict):
    x: float = 0
    y: float = 0
    w: float = 360
    h: float = 240


class LayoutItem(_Strict):
    x: float = 14
    y: float = 12


class Layout(_Strict):
    """UI 배치 상태 — 캔버스 영역(호스트) 좌표·크기, 카드(워커·노드) 좌표. 컴파일러는 읽지 않는다."""
    regions: Dict[str, LayoutRegion] = Field(default_factory=dict)
    items: Dict[str, LayoutItem] = Field(default_factory=dict)


class MediaSample(_Strict):
    """샘플 라이브러리 항목(§7 ⓖ) — 코덱 → 파일 경로 또는 synthetic."""
    model_config = ConfigDict(extra='allow')


class TopologyMedia(_Strict):
    samples: Dict[str, Dict[str, str]] = Field(default_factory=dict, description='id → {코덱: 파일|synthetic}')


# ── 워커 계약(PoolCreate.target_csp) — 컨트롤러가 토폴로지 노드 참조에서 파생한다. 워커 계약은 그대로다 ──

class TargetPeering(_Strict):
    ip: Optional[str] = None
    port: int = Field(ge=1, le=65535)
    protocol: Transport = 'udp'
    local_node: str = 'cims-tester-peering'


class TargetCsp(_Strict):
    """풀이 닿는 SIP 서버(워커 관점) — access 포트 + 도메인 + (피어 풀) 피어링 다음 홉."""
    ip: str
    udp: int = 5060
    tcp: int = 25061
    tls: int = 5061
    domain_volte: Optional[str] = None
    domain_ptt: Optional[str] = None
    peering: Optional[TargetPeering] = None


class OamRef:
    """oam 노드에서 파생한 대상 OAM 접속 정보(tester_target 이 쓴다)."""
    __slots__ = ('node', 'url', 'token_env', 'csp_deployment_id', 'observe')

    def __init__(self, node: str, url: str, token_env: Optional[str], csp_deployment_id: Optional[int], observe: List[str]):
        self.node, self.url, self.token_env, self.csp_deployment_id, self.observe = node, url, token_env, csp_deployment_id, observe


class Topology(_Strict):
    name: str
    hosts: Dict[str, Host] = Field(min_length=1)
    workers: List[Worker] = Field(default_factory=list)
    target: Target
    pools: Dict[str, Pool] = Field(min_length=1)
    media: Optional[TopologyMedia] = None
    layout: Optional[Layout] = None

    @field_validator('hosts')
    @classmethod
    def _host_ids(cls, v):
        for k in v:
            if not re.match(_ID_RE, k):
                raise ValueError(f'호스트 id 는 소문자·숫자·_ 만: {k!r}')
        return v

    @field_validator('pools')
    @classmethod
    def _pool_names(cls, v):
        for k in v:
            if not re.match(_ID_RE, k):
                raise ValueError(f'pool 이름은 소문자·숫자·_ 만 (시나리오 roles.pool 이 참조): {k!r}')
        return v

    @model_validator(mode='after')
    def _refs(self):
        names = [w.name for w in self.workers]
        if len(set(names)) != len(names):
            raise ValueError('workers 이름이 중복된다')
        for w in self.workers:
            if w.host not in self.hosts:
                raise ValueError(f'workers.{w.name}.host={w.host!r} 는 hosts 에 없다')
        seen_listener: Dict[tuple, str] = {}
        for nid, n in self.target.nodes.items():
            if n.host not in self.hosts:
                raise ValueError(f'target.nodes.{nid}.host={n.host!r} 는 hosts 에 없다')
            for lid, l in (n.sip.listeners if n.sip else {}).items():
                key = (self.listener_ip(nid, lid), l.port, l.protocol)
                if key in seen_listener:
                    raise ValueError(f'target.nodes.{nid}.sip.listeners.{lid}: 수신점 {key[0]}:{key[1]}/{key[2]} 이 {seen_listener[key]} 과 겹친다')
                seen_listener[key] = f'{nid}:{lid}'
        seen_group_worker = set()
        seen_bind = {}
        for pname, p in self.pools.items():
            if p.worker not in names:
                raise ValueError(f'pools.{pname}.worker={p.worker!r} 는 workers 에 없다')
            if p.group:
                if p.group in self.pools:
                    raise ValueError(f'pools.{pname}.group={p.group!r} 이 다른 풀 이름과 같다 — 역할 해석이 모호해진다')
                key = (p.group, p.worker)
                if key in seen_group_worker:
                    raise ValueError(f'워커 {p.worker} 에 group {p.group!r} 풀이 둘 — 워커마다 논리 풀 하나만')
                seen_group_worker.add(key)
            if p.kind in ('ue', 'real-ue'):
                node = self.target.nodes.get(p.access)
                if node is None or node.sip is None or not node.sip.by_edge('access'):
                    raise ValueError(f'pools.{pname}.access={p.access!r} 는 access 수신점(sip.listeners edge=access)이 있는 노드가 아니다')
                if p.listener is not None:
                    l = node.sip.listeners.get(p.listener)
                    if l is None or l.edge != 'access':
                        raise ValueError(f'pools.{pname}.listener={p.listener!r} 는 {p.access} 의 access 수신점이 아니다')
                    if 'transport' in p.model_fields_set and p.transport != l.protocol:
                        raise ValueError(f'pools.{pname}: transport {p.transport} 인데 수신점 {p.listener} 은 {l.protocol} 이다')
                    p.transport = l.protocol
                elif not any(l.protocol == p.transport for l in node.sip.by_edge('access').values()):
                    raise ValueError(f'pools.{pname}: transport {p.transport} 인데 {p.access} 에 {p.transport} access 수신점이 없다')
                if p.kind == 'ue' and isinstance(p.source, DbSource):
                    src = self.target.nodes.get(p.source.db)
                    if src is None or not (src.role == 'db' or (src.role == 'subscriber' and src.api is not None)):
                        raise ValueError(f'pools.{pname}.source.db={p.source.db!r} 는 db 노드 또는 api 있는 subscriber 노드가 아니다')
            else:
                node = self.target.nodes.get(p.peering)
                if node is None or node.sip is None or not node.sip.listeners:
                    raise ValueError(f'pools.{pname}.peering={p.peering!r} 는 수신점(sip.listeners)이 있는 SIP 노드가 아니다')
                if p.listener is not None and p.listener not in node.sip.listeners:
                    raise ValueError(f'pools.{pname}.listener={p.listener!r} 는 {p.peering} 의 수신점이 아니다')
                key = (self.pool_bind_ip(pname), p.bind.port, p.bind.protocol)
                if key in seen_bind:
                    raise ValueError(f'pools.{pname}: 수신점 {key[0]}:{key[1]}/{key[2]} 이 {seen_bind[key]} 과 겹친다')
                seen_bind[key] = pname
        return self

    # ── 파생 조회 — 주소 사슬 listener.ip → node.addr → host.ip ────────────────

    def worker_by_name(self, name: str) -> Optional[Worker]:
        return next((w for w in self.workers if w.name == name), None)

    def host_ip(self, hid: str) -> str:
        return self.hosts[hid].ip

    def worker_host(self, wname: str) -> str:
        w = self.worker_by_name(wname)
        return self.host_ip(w.host) if w else ''

    def worker_url(self, w: Worker) -> str:
        return f'http://{self.host_ip(w.host)}:{w.port}'

    def node_ip(self, nid: str) -> str:
        """노드 주소 — addr(VIP) 이 있으면 그것, 없으면 호스트 ip. API/OAM/DB/RTP 포트와 수신점 ip 의 기본값."""
        n = self.target.nodes[nid]
        return n.addr or self.host_ip(n.host)

    def listener_ip(self, nid: str, lid: str) -> str:
        n = self.target.nodes[nid]
        l = n.sip.listeners[lid] if n.sip else None
        return (l.ip if l and l.ip else None) or self.node_ip(nid)

    def nodes_by_role(self, role: str) -> Dict[str, TargetNode]:
        return {k: n for k, n in self.target.nodes.items() if n.role == role}

    def domains_of(self, nid: str) -> List[str]:
        n = self.target.nodes.get(nid)
        return list(n.sip.domains) if n and n.sip else []

    def default_domain(self, nid: str, ptt: bool = False) -> str:
        doms = self.domains_of(nid)
        if ptt:
            for d in doms:
                if 'ptt' in d:
                    return d
        return doms[0] if doms else ''

    def pool_bind_ip(self, pname: str) -> str:
        """피어 풀 수신점 ip = bind.ip, 비면 그 워커 호스트 주소."""
        p = self.pools[pname]
        return (p.bind.ip if p.kind == 'peer' and p.bind.ip else None) or self.worker_host(p.worker)

    def pool_node(self, pname: str) -> str:
        p = self.pools[pname]
        return p.peering if p.kind == 'peer' else p.access

    def pool_listener(self, pname: str) -> Tuple[str, str, SipListener]:
        """풀이 닿는 수신점 (노드 id, 수신점 id, 항목). UE = listener 또는 transport 와 같은 첫 access 수신점.
        피어 = listener 또는 첫 edge=peering, 없으면 bind.protocol 과 같은 첫 수신점, 그것도 없으면 첫 수신점."""
        p = self.pools[pname]
        nid = self.pool_node(pname)
        sip = self.target.nodes[nid].sip
        if p.listener is not None:
            return nid, p.listener, sip.listeners[p.listener]
        if p.kind == 'peer':
            for pool in (sip.by_edge('peering'),
                         {k: l for k, l in sip.listeners.items() if l.protocol == p.bind.protocol}, sip.listeners):
                if pool:
                    lid = next(iter(pool))
                    return nid, lid, sip.listeners[lid]
        else:
            for lid, l in sip.by_edge('access').items():
                if l.protocol == p.transport:
                    return nid, lid, l
        raise ValueError(f'pool {pname}: 닿을 수신점이 없다')

    @staticmethod
    def local_node_name(lid: str, l: SipListener) -> str:
        """cims 대상에서 이 수신점이 뜻하는 local_nodes 이름 — 항목의 local_node, 비면 cims-tester-<id>."""
        return l.local_node or f'cims-tester-{lid}'

    def target_csp_for(self, pname: str) -> TargetCsp:
        """워커 계약 PoolCreate.target_csp — 풀이 닿는 수신점에서 파생한다. ip = 그 수신점 주소, udp/tcp/tls = 같은 주소의 access
        수신점 포트(트렁크 REGISTER·다른 transport 폴백), 피어 풀은 peering = 그 수신점 자체."""
        p = self.pools[pname]
        nid, lid, l = self.pool_listener(pname)
        node = self.target.nodes[nid]
        ip = self.listener_ip(nid, lid)
        kw = {'ip': ip}
        for alid, al in node.sip.by_edge('access').items():
            if self.listener_ip(nid, alid) == ip and al.protocol not in kw:
                kw[al.protocol] = al.port
        if p.kind != 'peer' and l.protocol not in kw:
            kw[l.protocol] = l.port
        doms = self.domains_of(nid)
        if doms:
            kw['domain_volte'] = self.default_domain(nid) or None
            kw['domain_ptt'] = self.default_domain(nid, ptt=True) or None
            if kw['domain_ptt'] == kw['domain_volte']:
                kw['domain_ptt'] = None
        if p.kind == 'peer':
            kw['peering'] = TargetPeering(ip=ip, port=l.port, protocol=l.protocol, local_node=self.local_node_name(lid, l))
        return TargetCsp(**kw)

    def peer_listeners(self, pools: List[str]) -> Dict[str, Tuple[str, str, SipListener]]:
        """피어 풀 이름 → (노드, 수신점 id, 항목). 시드가 풀마다 접속점(LocalNode)을 고르는 데 쓴다."""
        return {pn: self.pool_listener(pn) for pn in pools if self.pools[pn].kind == 'peer'}

    def oam_ref(self) -> Optional[OamRef]:
        for nid, n in self.target.nodes.items():
            if n.role == 'oam':
                o = n.oam or NodeOam()
                scheme = 'https' if o.tls else 'http'
                return OamRef(nid, f'{scheme}://{self.node_ip(nid)}:{o.port}', o.token_env, o.csp_deployment_id, list(o.observe))
        return None

    def host_kind(self, hid: str) -> str:
        """호스트 성격(파생) — tester(워커만)·target(대상 노드만)·shared(동거)·empty."""
        has_w = any(w.host == hid for w in self.workers)
        has_n = any(n.host == hid for n in self.target.nodes.values())
        return 'shared' if has_w and has_n else 'tester' if has_w else 'target' if has_n else 'empty'


# ──────────────────────────────────────────────────────────────────────────
#  2. 시나리오 — 역할·단계·기대치 (기능·성능 공용)
# ──────────────────────────────────────────────────────────────────────────

StepKind = Literal[
    'register', 'deregister', 'invite', 'progress', 'answer', 'reject', 'bye',
    'hold', 'resume', 'dtmf',
    'refer', 'replaces', 'join', 'pickup', 'subscribe', 'publish',
    'group_call', 'floor_request', 'floor_release', 'sds_send', 'sds_recv',
    'media_hold', 'wait', 'expect',
]

# 워커가 실행할 수 있는 단계(§4) — 나머지는 모델에는 있지만 컴파일 시 거절한다(콘솔 팔레트는 회색).
WORKER_STEPS = frozenset((
    'register', 'deregister', 'invite', 'progress', 'answer', 'reject', 'bye',
    'hold', 'resume', 'dtmf', 'refer', 'media_hold', 'wait', 'expect',
))

# media_hold.during 에 둘 수 있는 통화 중 동작(§7 ⓓ) — 컴파일러가 평평한 단계열로 푼다
DURING_STEPS = ('dtmf', 'hold', 'resume', 'refer')

# 단계 어휘 표 — 콘솔 편집기 팔레트·속성 폼·kind 게이트의 정본(GET /scenarios/vocab).
#   group   : 팔레트 묶음 · actor: 행위자 인자 꼴(who|from|fromto|seconds|none)
#   kind    : 행위자 역할의 풀 kind 게이트 — 'peer' = 피어 풀만, 'ue' = UE 풀만, 'ue|trunk' = UE 또는 트렁크 계정 피어, None = 무관
#   metrics : 이 단계에 우선 제안하는 expect 지표
STEP_VOCAB = {
    'register':      {'group': 'reg',   'actor': 'who',     'kind': 'ue|trunk', 'metrics': ['code', 'rrd_ms'], 'desc': '역할 단말 전부 등록 (prelude)'},
    'deregister':    {'group': 'reg',   'actor': 'who',     'kind': 'ue|trunk', 'metrics': ['code'], 'desc': 'run 종료 시 등록 해제 (epilogue)'},
    'wait':          {'group': 'reg',   'actor': 'seconds', 'kind': None,       'metrics': [], 'desc': '대기 (seconds)'},
    'invite':        {'group': 'call',  'actor': 'fromto',  'kind': None,       'metrics': ['code', 'srd_ms', 'ser_pct', 'seer_pct'], 'desc': 'INVITE from → to (비동기)'},
    'progress':      {'group': 'peer',  'actor': 'who',     'kind': 'peer',     'metrics': ['early_media_pct', 'prack_pct'], 'desc': '183 early media · PRACK (피어)'},
    'answer':        {'group': 'call',  'actor': 'who',     'kind': None,       'metrics': ['code', 'srd_ms', 'ser_pct'], 'desc': '착신 대기 → after_ms 뒤 200'},
    'reject':        {'group': 'call',  'actor': 'who',     'kind': None,       'metrics': ['code', 'q850_rx_pct'], 'desc': '착신 대기 → payload 코드로 거절'},
    'bye':           {'group': 'call',  'actor': 'from',    'kind': None,       'metrics': ['sdd_ms', 'code', 'scr_pct', 'q850_rx_pct', 'dtmf_rx_pct'], 'desc': 'BYE → 최종 응답 (SDD)'},
    'media_hold':    {'group': 'media', 'actor': 'seconds', 'kind': None,       'metrics': ['rtp_loss_pct', 'jitter_ms', 'mos'], 'desc': '확립 뒤 seconds 유지, 끝에 RTP 표본 (during 로 통화 중 동작)'},
    'hold':          {'group': 'media', 'actor': 'from',    'kind': None,       'metrics': ['code'], 'desc': 're-INVITE sendonly'},
    'resume':        {'group': 'media', 'actor': 'from',    'kind': None,       'metrics': ['code'], 'desc': 're-INVITE sendrecv'},
    'dtmf':          {'group': 'media', 'actor': 'from',    'kind': None,       'metrics': ['dtmf_rx_pct'], 'desc': 'RFC 4733 숫자열 송신 (payload)'},
    'refer':         {'group': 'xfer',  'actor': 'fromto',  'kind': 'peer',     'metrics': ['code'], 'desc': 'blind REFER from(전달자) → to'},
    'replaces':      {'group': 'xfer',  'actor': 'fromto',  'kind': None,       'metrics': ['code'], 'desc': 'INVITE-Replaces (RFC 3891)'},
    'join':          {'group': 'xfer',  'actor': 'fromto',  'kind': None,       'metrics': ['code'], 'desc': 'Join 합류 (RFC 3911)'},
    'pickup':        {'group': 'xfer',  'actor': 'from',    'kind': None,       'metrics': ['code'], 'desc': '당겨받기 (피처코드)'},
    'subscribe':     {'group': 'ctl',   'actor': 'who',     'kind': None,       'metrics': ['code'], 'desc': 'SUBSCRIBE (dialog/reg)'},
    'publish':       {'group': 'ctl',   'actor': 'who',     'kind': None,       'metrics': ['code'], 'desc': 'PUBLISH'},
    'group_call':    {'group': 'ptt',   'actor': 'from',    'kind': 'ue',       'metrics': ['code', 'srd_ms'], 'desc': 'PTT 그룹콜 (group)'},
    'floor_request': {'group': 'ptt',   'actor': 'who',     'kind': 'ue',       'metrics': ['floor_grant_ms', 'floor_queue_ms'], 'desc': 'Floor Request'},
    'floor_release': {'group': 'ptt',   'actor': 'who',     'kind': 'ue',       'metrics': ['floor_taken_ms'], 'desc': 'Floor Release'},
    'sds_send':      {'group': 'ptt',   'actor': 'from',    'kind': 'ue',       'metrics': ['sds_delay_ms'], 'desc': 'MCData SDS 송신'},
    'sds_recv':      {'group': 'ptt',   'actor': 'who',     'kind': 'ue',       'metrics': ['sds_delay_ms', 'sds_disposition_pct'], 'desc': 'MCData SDS 수신 대기'},
    'expect':        {'group': 'ctl',   'actor': 'none',    'kind': None,       'metrics': ['ser_pct', 'scr_pct', 'isa_pct'], 'desc': '누계 지표 게이트'},
}
STEP_GROUPS = [
    {'id': 'reg', 'label': '등록'}, {'id': 'call', 'label': '호'}, {'id': 'media', 'label': '미디어'},
    {'id': 'peer', 'label': '피어 축'}, {'id': 'xfer', 'label': '전달·합류'}, {'id': 'ptt', 'label': 'PTT · MCData'},
    {'id': 'ctl', 'label': '이벤트·게이트'},
]

METRIC_LABELS = {
    'code': 'code — 응답 코드', 'rrd_ms': 'RRD — 등록 지연', 'srd_ms': 'SRD — 세션 요청 지연', 'sdd_ms': 'SDD — 세션 해제 지연',
    'sdt_s': 'SDT — 세션 지속', 'ser_pct': 'SER — 세션 확립률', 'seer_pct': 'SEER — 유효 확립률', 'scr_pct': 'SCR — 세션 완료율',
    'isa_pct': 'ISA — 시도 실패율', 'rtp_loss_pct': 'RTP 손실률', 'jitter_ms': 'RTP 지터', 'mos': 'MOS',
    'floor_grant_ms': 'Floor grant 지연', 'floor_taken_ms': 'Floor taken 지연', 'floor_queue_ms': 'Floor 대기',
    'sds_delay_ms': 'SDS 지연', 'sds_disposition_pct': 'SDS disposition 률', 'dtmf_rx_pct': 'DTMF 수신률',
    'q850_rx_pct': 'Q.850 Reason 수신률', 'early_media_pct': '183 early media 률', 'prack_pct': 'PRACK 률',
}

# Reason: Q.850 cause (ITU-T Q.850) — 편집기 목록
Q850_CAUSES = {
    16: '정상 종료', 17: '통화 중', 18: '무응답', 19: '응답 없음', 21: '거절', 27: '목적지 고장', 28: '번호 형식',
    31: '정상·미지정', 34: '회선 없음', 38: '망 고장', 41: '일시 고장', 42: '폭주', 47: '자원 없음', 63: '서비스 불가',
    102: '타이머 만료', 127: '불특정',
}
AUDIO_CODECS = ('amr-wb', 'amr', 'pcmu', 'pcma', 'g722')
VIDEO_CODECS = ('h264', 'none')

# expect 키 = RFC 6076 / RFC 3550 / TS 24.380 지표 이름(§5). 여기 없는 이름은 거절.
METRIC_NAMES = (
    'code',
    'rrd_ms', 'srd_ms', 'sdd_ms', 'sdt_s',
    'ser_pct', 'seer_pct', 'scr_pct', 'isa_pct',
    'rtp_loss_pct', 'jitter_ms', 'mos',
    'floor_grant_ms', 'floor_taken_ms', 'floor_queue_ms',
    'sds_delay_ms', 'sds_disposition_pct',
    # 피어 pbx/mgcf 축(D) — 비율은 발생기 관측(송신 대비 수신)
    'dtmf_rx_pct', 'q850_rx_pct', 'early_media_pct', 'prack_pct',
)

# 비율 지표의 분자/분모 카운터 — 요약·판정이 같은 정의를 쓴다(§5)
RATIO_METRICS = {
    'ser_pct': ('sessions', 'attempts'),
    'scr_pct': ('completed', 'sessions'),
    'dtmf_rx_pct': ('dtmf_rx', 'dtmf_tx'),          # 수신 이벤트 수 / 송신 숫자 수
    'q850_rx_pct': ('q850_rx', 'q850_tx'),          # Reason Q.850 수신 / 송신 (B2BUA 투과 여부)
    'early_media_pct': ('early_media', 'progress_tx'),   # 발신자에 도달한 183+SDP / 피어가 낸 183
    'prack_pct': ('prack_rx', 'progress_tx'),       # 피어 UAS 가 받은 PRACK / 낸 신뢰 183
}


class Percentiles(_Strict):
    p50: Optional[float] = None
    p95: Optional[float] = None
    p99: Optional[float] = None
    max: Optional[float] = None
    min: Optional[float] = None

    @model_validator(mode='after')
    def _any(self):
        if all(getattr(self, f) is None for f in ('p50', 'p95', 'p99', 'max', 'min')):
            raise ValueError('기대치에 p50/p95/p99/max/min 중 하나는 필요하다')
        return self


Expectation = Union[int, float, Percentiles]


class Media(_Strict):
    audio: Optional[str] = Field(default='amr-wb', description='amr-wb | amr | pcmu | pcma | g722')
    video: Optional[str] = Field(default=None, description='h264 | none')


class During(_Strict):
    """media_hold 유지 구간 안 시각 지정 동작(§7 ⓓ) — at_s = 확립 뒤 경과 초. 컴파일러가 `hold at_s → 동작 → hold 나머지` 로 푼다."""
    at_s: float = Field(ge=0)
    step: Literal['dtmf', 'hold', 'resume', 'refer']
    who: Optional[List[str]] = None
    from_: Optional[str] = Field(default=None, alias='from')
    to: Optional[str] = None
    payload: Optional[str] = None
    expect: Dict[str, Expectation] = Field(default_factory=dict)

    @model_validator(mode='after')
    def _actor(self):
        if not (self.from_ or self.who):
            raise ValueError(f'during {self.step} 은 from 또는 who 가 필요하다')
        if self.step == 'dtmf' and (not self.payload or any(c not in '0123456789*#ABCDabcd' for c in self.payload)):
            raise ValueError('during dtmf 는 payload 에 숫자열(0-9 * # A-D)이 필요하다')
        if self.step == 'refer' and not (self.from_ and self.to):
            raise ValueError('during refer 는 from 과 to 가 필요하다')
        return self


class Step(_Strict):
    step: StepKind
    who: Optional[List[str]] = None
    from_: Optional[str] = Field(default=None, alias='from')
    to: Optional[str] = None
    after_ms: Optional[int] = Field(default=None, ge=0)
    seconds: Optional[Union[int, str]] = Field(default=None, description='정수 또는 ${ht} 같은 바인딩')
    media: Optional[Media] = None
    group: Optional[str] = None
    payload: Optional[str] = Field(default=None, description='dtmf: 숫자열(0-9*#A-D) · reject: 응답 코드')
    cause: Optional[int] = Field(default=None, ge=1, le=127,
                                 description='bye/reject 의 Reason: Q.850;cause= (RFC 3326 — MGCF 종료 사유, 16=정상 34=회선 없음)')
    during: Optional[List[During]] = Field(default=None, description='media_hold 만 — 유지 구간 안 통화 중 동작(at_s 순)')
    expect: Dict[str, Expectation] = Field(default_factory=dict)

    @field_validator('expect')
    @classmethod
    def _metric_names(cls, v):
        bad = [k for k in v if k not in METRIC_NAMES]
        if bad:
            raise ValueError(f'알 수 없는 지표 이름 {bad} — 허용: {list(METRIC_NAMES)}')
        return v

    @model_validator(mode='after')
    def _actor(self):
        if self.step in ('invite', 'refer', 'bye', 'sds_send', 'hold', 'resume', 'dtmf') and not (self.from_ or self.who):
            raise ValueError(f'{self.step} 단계는 from 또는 who 가 필요하다')
        if self.step in ('register', 'deregister', 'answer', 'reject', 'progress', 'subscribe', 'publish',
                         'floor_request', 'floor_release', 'sds_recv') and not self.who:
            raise ValueError(f'{self.step} 단계는 who 가 필요하다')
        if self.step in ('media_hold', 'wait') and self.seconds is None:
            raise ValueError(f'{self.step} 단계는 seconds 가 필요하다')
        if self.step == 'dtmf':
            if not self.payload or any(c not in '0123456789*#ABCDabcd' for c in self.payload):
                raise ValueError('dtmf 단계는 payload 에 숫자열(0-9 * # A-D)이 필요하다')
        if self.step == 'refer' and not (self.from_ and self.to):
            raise ValueError('refer 단계는 from(전달자)과 to(전달 대상 역할)가 필요하다')
        if self.cause is not None and self.step not in ('bye', 'reject'):
            raise ValueError('cause 는 bye/reject 단계에만 둔다')
        if self.during:
            if self.step != 'media_hold':
                raise ValueError('during 은 media_hold 단계에만 둔다')
            if isinstance(self.seconds, int):
                for d in self.during:
                    if d.at_s > self.seconds:
                        raise ValueError(f'during at_s={d.at_s} 가 seconds={self.seconds} 를 넘는다')
        return self


class Role(_Strict):
    pool: str
    disjoint_from: Optional[str] = None
    count: Optional[int] = Field(default=None, ge=1, description='역할이 쓰는 신원 수 — 생략=풀 전체')


EvidenceKind = Literal['recording_created', 'log_errors', 'alarm_raised', 'event_logged']


class Evidence(_Strict):
    kind: EvidenceKind
    min: Optional[int] = Field(default=None, ge=0)
    max: Optional[int] = Field(default=None, ge=0)
    code: Optional[str] = Field(default=None, description='알람/이벤트 정의 코드 (alarm_catalog)')


class Scenario(_Strict):
    id: str = Field(pattern=r'^[A-Z0-9][A-Z0-9-]{2,63}$')
    title: Optional[str] = None
    tags: List[str] = Field(default_factory=list)
    roles: Dict[str, Role] = Field(min_length=1)
    flow: List[Step] = Field(min_length=1)
    target_evidence: List[Evidence] = Field(default_factory=list)

    @model_validator(mode='after')
    def _refs(self):
        names = set(self.roles)
        for r, spec in self.roles.items():
            if spec.disjoint_from and spec.disjoint_from not in names:
                raise ValueError(f'roles.{r}.disjoint_from={spec.disjoint_from!r} 는 정의된 역할이 아니다')
        for i, s in enumerate(self.flow):
            for ref in [*(s.who or []), s.from_, s.to]:
                if ref and ref not in names:
                    raise ValueError(f'flow[{i}] ({s.step}) 가 정의되지 않은 역할 {ref!r} 을 참조한다')
            for d in (s.during or []):
                for ref in [*(d.who or []), d.from_, d.to]:
                    if ref and ref not in names:
                        raise ValueError(f'flow[{i}].during ({d.step}) 가 정의되지 않은 역할 {ref!r} 을 참조한다')
        return self


# ──────────────────────────────────────────────────────────────────────────
#  3. 부하 프로파일 — ETSI TS 186 008
# ──────────────────────────────────────────────────────────────────────────

class StopOn(_Strict):
    target_cpu_pct: Optional[float] = Field(default=None, gt=0, le=100)
    csp_5xx_pct: Optional[float] = Field(default=None, ge=0, le=100)
    ser_pct_min: Optional[float] = Field(default=None, ge=0, le=100)


class LoadProfile(_Strict):
    name: Optional[str] = None
    model: Literal['constant', 'step', 'ramp', 'soak', 'burst']
    unit: Literal['saps'] = Field(default='saps', description='시나리오 시도/초 — cps 를 시나리오 단위로 일반화')
    start: Optional[float] = Field(default=None, gt=0)
    step: Optional[float] = Field(default=None, gt=0)
    hold_s: Optional[int] = Field(default=None, ge=1)
    max: Optional[float] = Field(default=None, gt=0)
    rate: Optional[float] = Field(default=None, gt=0, description='constant/soak 의 고정 시도율')
    duration_s: Optional[int] = Field(default=None, ge=1)
    ramp_s: Optional[int] = Field(default=None, ge=1)
    burst_size: Optional[int] = Field(default=None, ge=1)
    burst_interval_s: Optional[int] = Field(default=None, ge=1)
    ht: Optional[int] = Field(default=None, ge=0, description='${ht} 바인딩 — 세션 유지(초)')
    ihs_threshold_pct: float = Field(default=0.1, ge=0, le=100,
                                     description='부적절 처리 시나리오 비율 임계 — 초과 시 DOC 확정·정지')
    stop_on: StopOn = Field(default_factory=StopOn)

    @model_validator(mode='after')
    def _by_model(self):
        need = {
            'constant': ('rate', 'duration_s'),
            'soak': ('rate', 'duration_s'),
            'step': ('start', 'step', 'hold_s', 'max'),
            'ramp': ('start', 'max', 'ramp_s', 'hold_s'),
            'burst': ('burst_size', 'burst_interval_s', 'duration_s'),
        }[self.model]
        missing = [f for f in need if getattr(self, f) is None]
        if missing:
            raise ValueError(f'{self.model} 프로파일에 {missing} 가 필요하다')
        if self.model in ('step', 'ramp') and self.max is not None and self.start is not None \
                and self.max < self.start:
            raise ValueError('max 는 start 이상이어야 한다')
        return self


# ──────────────────────────────────────────────────────────────────────────
#  4. 컨트롤러 → 워커 (HTTP/JSON) — 워커는 YAML 을 모르고 컴파일된 단계만 받는다(§6.1)
# ──────────────────────────────────────────────────────────────────────────

class Identity(_Strict):
    user: str
    domain: str
    auth_id: Optional[str] = Field(default=None, description="IMPI 사용자부 — '@' 없으면 워커가 domain 을 붙인다(cspsim -creds authId 규약). 비면 user")
    ha1: Optional[str] = None
    password: Optional[str] = None
    display: Optional[str] = None
    auth_scheme: Literal['digest', 'aka'] = 'digest'
    aka_k: Optional[str] = None
    aka_opc: Optional[str] = None


class TrunkRegister(_Strict):
    """워커에 내려가는 트렁크 REGISTER 계정 — 컨트롤러가 환경변수(ha1_env/password_env)를 풀어 값으로 채운다."""
    user: str
    realm: Optional[str] = None
    ha1: Optional[str] = None
    password: Optional[str] = None
    expires: int = 3600


class WorkerPeerBind(_Strict):
    ip: str
    port: int = Field(ge=1, le=65535)
    protocol: Transport = 'udp'


class WorkerPeer(_Strict):
    """워커에 내려가는 피어 엔진 사양 — 토폴로지 PeerPool 에서 워커·노드 참조를 떼고 bind.ip(워커 호스트 주소)를 채운 것."""
    profile: PeerProfile
    bind: WorkerPeerBind
    domain: str
    identities: PeerIdentities
    codecs: Optional[List[str]] = None
    answer: Literal['normal', 'silent'] = 'normal'
    prack: Optional[bool] = None
    dtmf: bool = True


class PoolCreate(_Strict):
    """POST /pools — 풀 생성·신원 적재. 멱등(pool 이름 기준)."""
    pool: str
    kind: Literal['ue', 'peer', 'real-ue']
    identities: List[Identity] = Field(default_factory=list)
    transport: Transport = 'udp'
    srtp: SrtpMode = 'off'
    prack: bool = Field(default=False, description='kind=ue — 100rel/PRACK')
    dtmf: bool = Field(default=True, description='kind=ue — telephone-event 오퍼/echo')
    target_csp: TargetCsp = Field(description='풀이 닿는 SIP 서버 — 컨트롤러가 토폴로지 노드 참조에서 파생')
    peer: Optional[WorkerPeer] = Field(default=None, description='kind=peer 일 때 프로파일·bind·신원 범위')
    trunk_register: Optional[TrunkRegister] = Field(default=None, description='kind=peer(pbx) 트렁크 REGISTER 계정 — 비밀 해석 완료본')


class CompiledStep(_Strict):
    """시나리오 단계 + 바인딩 해석 결과 — 워커가 실행하는 단위."""
    idx: int = Field(ge=0)
    step: StepKind
    who: List[str] = Field(default_factory=list, description='역할 이름 — 워커는 roles 매핑으로 풀을 찾는다')
    from_: Optional[str] = Field(default=None, alias='from')
    to: Optional[str] = None
    after_ms: int = 0
    seconds: Optional[int] = None
    media: Optional[Media] = None
    group: Optional[str] = None
    payload: Optional[str] = None
    cause: Optional[int] = None
    expect: Dict[str, Expectation] = Field(default_factory=dict)


class RunStart(_Strict):
    """POST /runs — 컴파일된 시나리오 + 이 워커의 배분."""
    run_id: str
    scenario_id: str
    roles: Dict[str, str] = Field(description='역할 → 풀 이름')
    role_slices: Dict[str, List[int]] = Field(
        default_factory=dict, description='역할 → 이 워커가 맡는 신원 인덱스 [begin, end) — 워커 분산')
    steps: List[CompiledStep] = Field(min_length=1)
    rate_saps: float = Field(ge=0, description='이 워커 몫의 시도율 (컨트롤러가 워커 수로 나눔)')
    max_instances: Optional[int] = Field(default=None, ge=1,
                                         description='이 워커가 발생시킬 인스턴스 상한 — 단발(기능) 실행. 다 끝나면 워커가 run 을 스스로 닫는다')
    stream: str = Field(description='관측 스트림 목적지 host:port (TCP JSONL)')


class RunRate(_Strict):
    """POST /runs/{id}/rate"""
    rate_saps: float = Field(ge=0)


class RunStop(_Strict):
    """POST /runs/{id}/stop"""
    drain_s: int = Field(default=5, ge=0, description='진행 중 세션을 기다리는 시간 — 0 은 즉시 BYE')


class WorkerPoolState(_Strict):
    pool: str
    kind: Literal['ue', 'peer', 'real-ue']
    endpoints: int
    registered: int = 0


class WorkerHealth(_Strict):
    """GET /health 응답 — 용량 선언 + 시계 확인(§6.1)."""
    worker: str
    version: str
    max_endpoints: int = Field(ge=0)
    max_saps: float = Field(ge=0)
    cpu_pct: float = Field(ge=0)
    active_endpoints: int = Field(ge=0)
    active_run: Optional[str] = None
    clock_unix_ms: int = Field(description='워커 시각 — 컨트롤러가 오차를 계산, > 50 ms 면 경고')
    pools: List[WorkerPoolState] = Field(default_factory=list)


# ──────────────────────────────────────────────────────────────────────────
#  5. 워커 → 컨트롤러 (TCP JSONL 한 줄 = 한 레코드)
# ──────────────────────────────────────────────────────────────────────────

class Histogram(_Strict):
    """지연 지표 1초 요약 — 버킷은 로그 스케일 상한(ms) 문자열 키."""
    count: int = Field(ge=0)
    sum: float = Field(ge=0)
    min: Optional[float] = None
    max: Optional[float] = None
    buckets: Dict[str, int] = Field(default_factory=dict)


class StreamHello(_Strict):
    kind: Literal['hello']
    worker: str
    version: str
    t: float


class StreamAgg(_Strict):
    kind: Literal['agg']
    t: float = Field(description='버킷 시작 unix 초')
    bucket_s: int = Field(default=1, ge=1)
    run_id: str
    worker: str
    counters: Dict[str, int] = Field(default_factory=dict,
                                     description='attempts·sessions·legs·codes.<n>·rtp_rx·rtp_lost … (sip_statistics 3계층 어휘)')
    gauges: Dict[str, float] = Field(default_factory=dict, description='concurrent_sessions·registered·cpu_pct …')
    timers: Dict[str, Histogram] = Field(default_factory=dict, description='rrd_ms·srd_ms·sdd_ms·jitter_ms·floor_grant_ms …')


class StreamEvent(_Strict):
    kind: Literal['event']
    t: float
    run_id: str
    worker: str
    call_id: Optional[str] = None
    role: Optional[str] = None
    identity: Optional[str] = None
    step: Optional[StepKind] = None
    code: Optional[int] = None
    metric: Optional[str] = None
    observed: Optional[float] = None
    detail: Optional[str] = None


class StreamLog(_Strict):
    kind: Literal['log']
    t: float
    worker: str
    level: Literal['debug', 'info', 'warn', 'error']
    msg: str


StreamRecord = Union[StreamHello, StreamAgg, StreamEvent, StreamLog]


# ──────────────────────────────────────────────────────────────────────────
#  6. run 레코드 — runs/<id>/run.json (색인은 file_store 에 요약만)
# ──────────────────────────────────────────────────────────────────────────

Verdict = Literal['running', 'pass', 'fail', 'aborted', 'error']


class RunRecord(_Strict):
    id: str
    scenario_id: str
    topology: str
    profile: Optional[str] = None
    started_at: str
    ended_at: Optional[str] = None
    verdict: Verdict = 'running'
    workers: List[str] = Field(default_factory=list)
    summary: Dict[str, Union[int, float, str, None]] = Field(default_factory=dict)
    target_build: Optional[str] = Field(default=None, description='대상 git sha / 패키지 manifest 해시 — 회귀 비교 축')
    label: Optional[str] = Field(default=None, max_length=120)
    stop_reason: Optional[str] = None


# ──────────────────────────────────────────────────────────────────────────
#  7. run 요청 — POST /api/v1/tester/runs (콘솔·CLI → 컨트롤러)
# ──────────────────────────────────────────────────────────────────────────

class RunRequest(_Strict):
    scenario_id: str
    topology_id: Optional[int] = Field(default=None, description='저장된 토폴로지 id — topology(name) 와 둘 중 하나')
    topology: Optional[str] = Field(default=None, description='토폴로지 name')
    profile: Optional[str] = Field(default=None, description='부하 프로파일 name — 없으면 단발(기능) 실행')
    bindings: Dict[str, Union[int, float, str]] = Field(default_factory=dict,
                                                       description='${ht} 같은 시나리오 바인딩 — profile.ht 보다 우선')
    instances: Optional[int] = Field(default=None, ge=1, description='단발 실행 인스턴스 수 (기본 1)')
    rate_saps: Optional[float] = Field(default=None, gt=0, description='단발 실행의 발생율 (기본 instances 를 1 초 안에)')
    label: Optional[str] = Field(default=None, max_length=120)

    @model_validator(mode='after')
    def _topo(self):
        if self.topology_id is None and not self.topology:
            raise ValueError('topology_id 또는 topology(name) 가 필요하다')
        return self


# 스키마 이름 → 모델 (bin/gen-schemas · GET /api/v1/tester/schema/<name>)
SCHEMAS = {
    'run_request': RunRequest,
    'topology': Topology,
    'scenario': Scenario,
    'profile': LoadProfile,
    'worker_pool_create': PoolCreate,
    'worker_run_start': RunStart,
    'worker_run_rate': RunRate,
    'worker_run_stop': RunStop,
    'worker_health': WorkerHealth,
    'worker_stream_agg': StreamAgg,
    'worker_stream_event': StreamEvent,
    'worker_stream_log': StreamLog,
    'worker_stream_hello': StreamHello,
    'run_record': RunRecord,
}


def schema_json(name: str) -> dict:
    return SCHEMAS[name].model_json_schema(by_alias=True)


def validate(kind: str, doc: dict):
    """kind ∈ SCHEMAS. 반환 (model | None, errors: list[str])."""
    model = SCHEMAS.get(kind)
    if model is None:
        return None, [f'알 수 없는 종류 {kind!r} — 허용: {sorted(SCHEMAS)}']
    try:
        return model.model_validate(doc), []
    except Exception as e:   # pydantic.ValidationError
        errs = []
        for err in getattr(e, 'errors', lambda: [])():
            loc = '.'.join(str(p) for p in err.get('loc', ()))
            errs.append(f"{loc or '<root>'}: {err.get('msg')}")
        return None, errs or [str(e)]
