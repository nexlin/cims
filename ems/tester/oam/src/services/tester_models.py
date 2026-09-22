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
from typing import Annotated, Dict, List, Literal, Optional, Tuple, Union

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
# DTMF 방식 — rfc4733: telephone-event 오퍼/echo(기본) · inband: G.711 톤(telephone-event 없음 — PSTN 게이트웨이 뒤 in-band 경로, 협상 코덱이 G.711 일 때만 송신)
#   · off: 없음. YAML 의 옛 bool 은 true→rfc4733, false→off 로 읽는다
DtmfMode = Literal['rfc4733', 'inband', 'off']
SdsPlane = Literal['control', 'media']


def _dtmf_mode(v):
    if v is True:
        return 'rfc4733'
    if v is False:
        return 'off'
    return v
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
    ssh: Optional[HostSsh] = Field(default=None, description='있으면 run 동안 SSH 로 호스트 CPU/메모리·노드 procs 의 프로세스별 CPU/RSS 를 관측하고(stop_on.target_cpu_pct 원천) '
                                                            'nodes.*.logs 의 늘어난 ERROR 줄을 센다(log_errors). 비밀은 key_env 의 개인키 파일')


class WorkerMedia(_Strict):
    samples: List[str] = Field(default_factory=list,
                               description='이 워커가 보유한다고 선언한 샘플 id(topology.media.samples 의 키) — 비면 전부 보유로 본다. '
                                           '실제 파일은 health media.files 와 대조')
    max_rtp_streams: Optional[int] = Field(default=None, ge=0, description='RTP 를 쓰는 단말 동시 상한 선언(워커 Media.MaxRtpStreams 와 같은 뜻 — 계획 미리보기 용량 경고)')


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


class DialPlanSpec(_Strict):
    """다이얼 플랜(sip_service_model.md §2-10) — 국가코드·접두. SIP 노드에 두면 그 대상의 번호계획(단계 `invite.dial: national` 이 E.164 신원을
    국내형으로 바꿔 다이얼할 때의 규칙), 피어 풀에 두면 시드 Route 의 인바운드 플랜(피어가 국내형 DID 로 보낸 착신을 CSP 가 번역)."""
    country_code: str = Field(pattern=r'^[0-9]{1,3}$', description='E.164 국가코드 digits(예 82)')
    national_prefix: str = Field(default='0', pattern=r'^[0-9]{0,3}$', description='국내 트렁크 접두(비면 접두 없는 국가)')
    international_prefix: str = Field(default='00', pattern=r'^[0-9]{0,4}$', description='국제 접두')


class NodeSip(_Strict):
    """SIP 노드의 수신점 N 개 + 도메인. 입력이 이전 꼴(`access{udp,tcp,tls,domains}`·`peering{port,protocol,local_node}`)이면
    `normalize_sip_block` 이 수신점 항목 `udp`/`tcp`/`tls`/`peering` 으로 승계한다(store 도 읽을 때 같은 함수로 바꿔 저장)."""
    domains: List[str] = Field(default_factory=list, description='첫 항목 = 기본 홈 도메인, "ptt" 가 든 항목 = PTT 풀 도메인')
    listeners: Dict[str, SipListener] = Field(default_factory=dict)
    dial_plan: Optional[DialPlanSpec] = Field(default=None, description='대상의 번호계획(접속서비스 country_code 와 같은 값) — 단계 invite.dial: national|international 이 '
                                                                       '착신 역할의 E.164 신원을 그 꼴로 바꿔 다이얼한다(대상 CSP 다이얼 플랜 번역 시험). 없으면 그 단계는 컴파일 오류')

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
    logs: List[str] = Field(default_factory=list,
                            description='이 노드 모듈의 로그 파일 경로(호스트 기준, 글롭 가능 — 예 /opt/cims/csp/log/csp_*.log). hosts.*.ssh 관측이 run 동안 '
                                        '늘어난 ERROR/FATAL 줄을 세어 target_evidence log_errors 의 원천으로 쓴다')
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


ServiceKind = Literal['volte', 'voip', 'ptt']


class DbSource(_Strict):
    db: str = Field(description='신원 원천 노드 id — role=db 또는 api 있는 subscriber 노드')
    table: Literal['volte_subscriptions', 'voip_subscriptions', 'ptt_subscriptions']
    offset: int = Field(default=0, ge=0)
    count: int = Field(ge=1)
    ptt_group: Optional[str] = Field(default=None, description='ptt_subscriptions 만 — 이 MCPTT 그룹(mcptt_group_id) 멤버만 읽는다. '
                                                                  '생략 = 가입자마다 첫 그룹(mcptt_group_id·priority 순)')

    @model_validator(mode='after')
    def _ptt_group(self):
        if self.ptt_group and self.table != 'ptt_subscriptions':
            raise ValueError('source.ptt_group 은 table=ptt_subscriptions 에만 둔다')
        return self


class CredsSource(_Strict):
    creds: str = Field(description='creds JSONL 경로(cspsim -creds 승계) — scenarios/ 상대 또는 절대')
    offset: int = Field(default=0, ge=0, description='파일의 몇 번째 신원부터(0 기준) — 워커 여럿이 같은 creds 파일의 다른 구간을 나눠 쓸 때')
    count: Optional[int] = Field(default=None, ge=1)


class _PoolBase(_Strict):
    worker: str = Field(description='이 풀이 놓인 워커(풀 하나 = 워커 하나)')
    group: Optional[str] = Field(default=None, pattern=_ID_RE,
                                 description='논리 풀 이름 — 워커 여럿에 나눌 때 워커마다 풀 + 같은 group. 시나리오 roles.X.pool 이 참조')


class UeNat(_Strict):
    """NAT 뒤 단말 모사(§3.1 nat, ue_nat_traversal.md 검증) — 워커 호스트의 network namespace 안에서 UE 스택·RTP 소켓을 만든다(setns, 워커에
    CAP_SYS_ADMIN). netns·veth·MASQUERADE 는 워커 패키지 scripts/nat-netns.sh create <netns> 가 만든다. 대상은 호스트 주소로 변환된 소스만 본다."""
    netns: str = Field(pattern=r'^[A-Za-z0-9_-]{1,15}$', description='워커 호스트의 netns 이름(/var/run/netns/<netns>, Nat.NetnsDir)')
    local_ip: str = Field(min_length=7, description='netns 안 단말 주소 — SDP·Via·Contact 의 로컬 IP(스크립트 기본 <cidr>.2)')


class UePool(_PoolBase):
    kind: Literal['ue']
    access: str = Field(description='접속점 노드 id — edge=access 수신점이 있는 SIP 노드')
    listener: Optional[str] = Field(default=None, description='그 노드의 수신점 id — 비면 transport 와 같은 protocol 의 첫 access 수신점')
    source: Union[DbSource, CredsSource]
    service: Optional[ServiceKind] = Field(
        default=None, description='접속환경 클래스(sip_service_model kind) — ptt 면 MCPTT 단말(feature tag·PTT 도메인·GMS/CMS 구독·그룹 affiliation·floor). '
                                  '생략 = source.table 이 ptt_subscriptions 면 ptt, 그 외 volte')
    transport: Transport = Field(default='udp', description='listener 를 주면 그 protocol 로 맞춘다(둘 다 주고 다르면 오류)')
    srtp: SrtpMode = 'off'
    register_expires: int = Field(default=3600, ge=60)
    prack: bool = Field(default=False, description='RFC 3262 100rel — 발신 INVITE 에 Supported/Require: 100rel, RSeq 1xx 에 PRACK (mgcf early media 시험). 착신 UE 의 progress(183) 도 신뢰 1xx 로')
    dtmf: DtmfMode = Field(default='rfc4733', description='DTMF 방식 — rfc4733(telephone-event 오퍼/echo) · inband(G.711 톤 — 협상 코덱이 pcmu/pcma 일 때만) · off. 옛 bool 도 받는다')
    tls_verify: bool = Field(default=False, description='transport=tls — 접속점 서버 인증서를 워커 Tls.CaFile 로 검증(체인만, 호스트명 대조 없음). 기본 끔(개발 스택 자체 서명)')
    tls_client_cert: bool = Field(default=False, description='transport=tls — 접속점이 클라이언트 인증서를 요구할 때(상호인증) 워커 Tls.ClientCertFile 을 제시')
    nat: Optional[UeNat] = Field(default=None, description='NAT 뒤 단말 — 워커 호스트 netns 안에서 소켓을 만든다(대상은 변환된 주소만 본다). 워커 health nat 과 대조')
    media_worker: Optional[str] = Field(default=None, description='미디어 전담 워커(§4 미디어 평면 후속) — 이 풀의 RTP 소켓·송수신을 그 워커(에이전트 /media/*)에 둔다. '
                                                                     'SDP c=/m= 는 그 호스트를 가리키고 시그널링은 worker 에 남는다. 자기 워커와 다른 이름, PTT(floor) 풀은 불가')
    call_waiting: bool = Field(default=False, description='통화중대기 단말(TS 24.615) — 통화 중 두 번째 착신을 486 대신 180 으로 받아 보류한다(reject 단계로 거절, 응답은 미지원). '
                                                          '통화중대기 시나리오(VOLTE-ANN-CALL-WAITING)의 착신 풀')
    msrp: bool = Field(default=False, description='MCData media plane 능력(TS 24.282 §9.2.3) — REGISTER Contact 의 +g.3gpp.icsi-ref 에 mcdata.sds 를 더해 서버가 '
                                                  '대용량 SDS 를 MSRP(INVITE m=message)로 배포하는 대상이 된다. 끄면 FD SIGNALLING(FILEURL) MESSAGE 폴백으로 받는다. '
                                                  'sds_send plane: media 의 발신 쪽은 이 플래그와 무관')
    subscriber: Optional[str] = Field(default=None, description='MCData FD(fd_send/fd_recv) 가 쓰는 CSC — role=subscriber 노드 id(api 블록). 생략 = 대상의 유일한 '
                                                                  'subscriber 노드(둘 이상이면 지정). 신원에 IdMS 로그인(creds login/loginPw · DB users.login_id)이 있어야 한다')

    @field_validator('dtmf', mode='before')
    @classmethod
    def _dtmf(cls, v):
        return _dtmf_mode(v)


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


PeerAnswer = Literal['normal', 'silent', 'reject', 'delay']


class PeerFault(_Strict):
    """피어 오류 주입 — answer 정책의 매개변수(code/q850/delay_ms) + answer 와 독립인 **와이어 유실**(drop_invite/drop_pct — psip RecvFilter 로
    수신 메시지를 트랜잭션에 넣기 전에 버린다 → 상대의 Timer A/E 재전송이 닿는지, SRD 가 T1 만큼 늘어나는지 시험. UDP 에서만 뜻이 있다)."""
    code: int = Field(default=503, ge=300, le=699, description='answer=reject 의 최종 응답 코드(503 = 5xx failover, 486/603 = 사용자 측 거절)')
    q850: Optional[int] = Field(default=None, ge=1, le=127, description='거절에 실을 Reason: Q.850;cause= (RFC 3326 — 대상의 Reason 투과 시험)')
    delay_ms: int = Field(default=0, ge=0, le=120000, description='answer=delay — 착신 INVITE 뒤 이 시간 동안 아무 응답도 내지 않는다(100 Trying 은 스택)')
    drop_invite: int = Field(default=0, ge=0, le=5, description='새 착신 INVITE 마다 첫 N 벌을 와이어 유실처럼 버린다(1 = 상대 Timer A 500 ms 재전송이 첫 도달 — 카운터 peer_fault_drop·invite_retrans_rx, 비율 retrans_rx_pct)')
    drop_pct: int = Field(default=0, ge=0, le=90, description='모든 수신 메시지를 이 확률(%)로 버린다 — 재전송·재시도 복원력(응답 유실 → 상대의 200 재전송 등)')


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
    answer: PeerAnswer = Field(default='normal', description='착신 정책 — normal: 시나리오 단계가 응답 · silent: 무응답(죽은 피어 — Timer B failover) · '
                                                            'reject: 엔진이 즉시 fault.code 로 거절(5xx failover·Reason 투과) · delay: fault.delay_ms 동안 '
                                                            '100 Trying 뒤 침묵(응답 지연 — 대상 타이머·early media 대기 시험), 그 뒤 시나리오 단계가 응답')
    fault: Optional[PeerFault] = Field(default=None, description='answer reject/delay 의 매개변수 + 와이어 유실 drop_invite/drop_pct(오류 주입)')
    prack: Optional[bool] = Field(default=None, description='RFC 3262 100rel/PRACK — 생략=프로파일 기본(ibcf/mgcf 켬, pbx 끔)')
    dtmf: DtmfMode = Field(default='rfc4733', description='DTMF 방식 — rfc4733(telephone-event 오퍼/echo) · inband(G.711 톤 — mgcf in-band 옵션) · off. 옛 bool 도 받는다')
    thig: bool = Field(default=False, description='ibcf — 발신 INVITE 에 토큰화 Via(tokenized-by, TS 24.229 §5.10.4 THIG 흔적)를 얹고 응답의 Via 보존을 관측(thig_pct)')
    tls_client_auth: bool = Field(default=False, description='bind.protocol=tls — 수신점이 클라이언트 인증서를 요구(상호인증, 워커 Tls.CaFile 이 발급자). 대상 CSP 가 인증서를 내지 않으면 핸드셰이크 실패 = §12 과제 드러남')
    tls_verify: bool = Field(default=False, description='발신 TLS 연결(→ 대상 수신점·트렁크 REGISTER)에서 서버 인증서를 워커 Tls.CaFile 로 검증')
    tls_client_cert: bool = Field(default=False, description='발신 TLS 연결에 워커 Tls.ClientCertFile 을 클라이언트 인증서로 제시(대상 접속점이 상호인증을 요구할 때)')
    dial_plan: Optional[DialPlanSpec] = Field(default=None, description='시드 Route 의 인바운드 다이얼 플랜(sip_service_model.md §2-10) — 이 피어가 국내형 DID 로 착신을 보낼 때 '
                                                                       '대상 CSP 가 +E.164 로 번역한다(TRUNK-PBX-INBOUND-NATIONAL). 없으면 NNI 기본 = 국제형만')
    seed: PeerSeed = Field(default_factory=PeerSeed)

    @field_validator('dtmf', mode='before')
    @classmethod
    def _dtmf(cls, v):
        return _dtmf_mode(v)

    @model_validator(mode='after')
    def _fault(self):
        if self.answer == 'delay' and not (self.fault and self.fault.delay_ms > 0):
            raise ValueError('answer: delay 는 fault.delay_ms(> 0) 가 필요하다')
        if self.answer == 'reject' and self.fault is None:
            self.fault = PeerFault()
        if self.fault and (self.fault.drop_invite or self.fault.drop_pct) and self.bind.protocol != 'udp':
            raise ValueError('fault.drop_invite/drop_pct 는 bind.protocol: udp 에서만 — TCP/TLS 는 SIP 재전송이 없다(RFC 3261 §17.1.1.2)')
        if self.thig and self.profile != 'ibcf':
            raise ValueError('thig 는 profile: ibcf 에서만(토큰화 Via 는 IBCF 의 흔적)')
        if (self.tls_client_auth) and self.bind.protocol != 'tls':
            raise ValueError('tls_client_auth 는 bind.protocol: tls 에서만')
        return self

    @property
    def dial(self) -> str:
        """UE 가 이 피어 신원을 부르는 꼴 — ibcf 는 user@도메인(Request-URI host 규칙), pbx/mgcf 는 번호 그대로(DID/E.164 prefix 규칙)."""
        return 'domain' if self.profile == 'ibcf' else 'number'


class RealUePool(_PoolBase):
    """실단말 풀(§3.3) — 신원마다 워커가 `cimsue-cli … drive`(libcimsue/pjsua2 실스택) 프로세스 하나를 띄운다. 소수(워커 RealUe.MaxProcesses)로
    대량 가상 부하 아래 실단말 품질을 표본 측정한다. 지원 단계 = REAL_UE_STEPS(등록·1:1 호·hold/resume·DTMF·픽업·그룹콜·floor), 미디어는 실스택 것
    (invite.media.rtp 는 auto 만, media_send/stop 불가). AKA 신원은 받지 않는다."""
    kind: Literal['real-ue']
    access: str = Field(description='접속점 노드 id — edge=access 수신점이 있는 SIP 노드')
    listener: Optional[str] = None
    source: Union[DbSource, CredsSource]
    service: Optional[ServiceKind] = Field(default=None, description='접속환경 클래스 — ptt 면 MCPTT 단말(mcptt-id·affiliation·floor). 생략 = source.table 이 ptt_subscriptions 면 ptt, 그 외 volte')
    transport: Transport = 'tls'
    srtp: SrtpMode = 'optional'
    tls_verify: bool = Field(default=False, description='서버 TLS 인증서 검증 — 워커 RealUe.TlsCaFile 을 앵커로(없으면 검증 없이 접속). 기본은 개발 스택(자체 서명) 전제로 끔')


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


# 샘플이 파일로 가질 수 있는 코덱 — 워커 송신기(libcsim CRtpThread)가 읽는 raw 형식: amr-wb = 61 B 프레임, pcmu/pcma/g722 = 160 B(20 ms)
SAMPLE_CODECS = ('amr-wb', 'pcmu', 'pcma', 'g722')


class TopologyMedia(_Strict):
    """미디어 샘플 라이브러리(§4 미디어 평면) — id → {코덱: 워커 샘플 디렉터리(Media.SampleDir) 안 상대 경로 | 'synthetic'}.
    합의 코덱에 해당하는 항목이 없으면 그 코덱은 합성으로 나간다."""
    samples: Dict[str, Dict[str, str]] = Field(default_factory=dict, description='id → {amr-wb|pcmu|pcma|g722: 파일|synthetic}')

    @field_validator('samples')
    @classmethod
    def _samples(cls, v):
        for sid, m in v.items():
            if not re.match(r'^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$', sid):
                raise ValueError(f'media.samples 의 id {sid!r} 는 영숫자·_.- 64자 이내')
            if not m:
                raise ValueError(f'media.samples.{sid} 에 코덱 항목이 없다')
            for codec, f in m.items():
                if codec not in SAMPLE_CODECS:
                    raise ValueError(f'media.samples.{sid}.{codec} — 코덱은 {list(SAMPLE_CODECS)} 중 하나')
                if not f or f.startswith('/') or '..' in f:
                    raise ValueError(f'media.samples.{sid}.{codec}={f!r} — 워커 샘플 디렉터리 안의 상대 경로 또는 synthetic')
        return v


# ── 워커 계약(PoolCreate.target_csp) — 컨트롤러가 토폴로지 노드 참조에서 파생한다. 워커 계약은 그대로다 ──

class TargetPeering(_Strict):
    ip: Optional[str] = None
    port: int = Field(ge=1, le=65535)
    protocol: Transport = 'udp'
    local_node: str = 'cims-tester-peering'


class TargetCsp(_Strict):
    """풀이 닿는 SIP 서버(워커 관점) — access 포트 + 도메인 + (피어 풀) 피어링 다음 홉 + 번호계획(dial 변환)."""
    ip: str
    udp: int = 5060
    tcp: int = 25061
    tls: int = 5061
    domain_volte: Optional[str] = None
    domain_ptt: Optional[str] = None
    peering: Optional[TargetPeering] = None
    dial_plan: Optional[DialPlanSpec] = Field(default=None, description='노드 sip.dial_plan — 워커가 invite.dial: national|international 의 다이얼 문자열을 만든다')


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
            mw = getattr(p, 'media_worker', None)
            if mw is not None:
                if mw not in names:
                    raise ValueError(f'pools.{pname}.media_worker={mw!r} 는 workers 에 없다')
                if mw == p.worker:
                    raise ValueError(f'pools.{pname}.media_worker 는 자기 워커({p.worker})와 달라야 한다 — 같은 워커면 분리할 것이 없다')
                if self.pool_service(pname) == 'ptt':
                    raise ValueError(f'pools.{pname}: PTT 풀(service ptt)은 media_worker 를 쓸 수 없다 — floor 제어 소켓이 RTP 스레드에 있다')
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
                if isinstance(p.source, DbSource):
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

    def default_domain(self, nid: str, ptt: bool = False, service: Optional[str] = None) -> str:
        """노드 도메인 중 접속환경 클래스의 것 — 이름에 그 kind(`ptt`·`voip`)가 든 항목, 없으면 첫 항목(volte). Digest username 은
        `imsi@<서비스 domain>` 이라(sip_service_model.md §3) voip 풀이 volte 도메인으로 등록하면 403(username mismatch)."""
        doms = self.domains_of(nid)
        kind = 'ptt' if ptt else (service or '')
        if kind in ('ptt', 'voip'):
            for d in doms:
                if kind in d:
                    return d
        return doms[0] if doms else ''

    def pool_service(self, pname: str) -> str:
        """UE 풀의 접속환경 클래스 — service, 비면 source.table 이 ptt_subscriptions 일 때 ptt, 그 외 volte."""
        p = self.pools[pname]
        if p.kind not in ('ue', 'real-ue'):
            return 'volte'
        if p.service:
            return p.service
        return 'ptt' if getattr(p.source, 'table', None) == 'ptt_subscriptions' else 'volte'

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
        if node.sip.dial_plan is not None:
            kw['dial_plan'] = node.sip.dial_plan
        return TargetCsp(**kw)

    def target_csc_for(self, pname: str) -> Optional[TargetCsc]:
        """워커 계약 PoolCreate.target_csc — 풀 subscriber(없으면 대상의 유일한 role=subscriber 노드)의 api 블록. 없으면 None(fd_* 단계 컴파일 오류).
        주소 = 노드 addr(VIP) 또는 호스트 ip."""
        p = self.pools[pname]
        want = getattr(p, 'subscriber', None)
        subs = {nid: n for nid, n in self.target.nodes.items() if n.role == 'subscriber' and n.api is not None}
        if want:
            if want not in subs:
                raise ValueError(f'pool {pname}: subscriber={want!r} 는 api 블록이 있는 role=subscriber 노드가 아니다')
            nid = want
        elif len(subs) == 1:
            nid = next(iter(subs))
        else:
            return None
        n = subs[nid]
        return TargetCsc(ip=self.node_ip(nid), port=n.api.port, tls=n.api.tls)

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
    'register', 'deregister', 'invite', 'progress', 'answer', 'reject', 'no_answer', 'unreachable', 'bye',
    'hold', 'resume', 'dtmf',
    'refer', 'replaces', 'join', 'pickup', 'subscribe', 'publish',
    'group_call', 'floor_request', 'floor_release', 'sds_send', 'sds_recv', 'fd_send', 'fd_recv',
    'media_hold', 'media_send', 'media_stop', 'wait', 'expect', 'check',
]

# 워커가 실행할 수 있는 단계(§4) — 나머지는 모델에는 있지만 컴파일 시 거절한다(콘솔 팔레트는 회색).
WORKER_STEPS = frozenset((
    'register', 'deregister', 'invite', 'progress', 'answer', 'reject', 'no_answer', 'unreachable', 'bye',
    'hold', 'resume', 'dtmf', 'refer', 'media_hold', 'media_send', 'media_stop', 'wait', 'expect',
    'group_call', 'floor_request', 'floor_release',
    'pickup', 'subscribe', 'replaces', 'join', 'publish',
    'sds_send', 'sds_recv', 'fd_send', 'fd_recv', 'check',
))
# fd_recv.payload — 도착 뒤 동작: download(기본 — who 전원이 FILEURL 을 내려받아 Metadata size 와 대조, fd_download_pct) | signal(FD SIGNALLING 도착만)
FD_RECV_MODES = ('download', 'signal')
# fd_send.payload — 합성 크기(`65536`·`256k`·`2m`, 1 B ~ 64 MiB) 또는 워커 Media.SampleDir 의 파일 이름(상대 경로, `..` 불가)
_FD_SIZE = re.compile(r'^[0-9]{1,8}[kKmM]?$')
_FD_FILE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._\-/]{0,127}$')
# check.payload — 관측 정합 판정 종류(cspsim 검사의 계측기 이전 — S3-SCN-PTT-LISTEN L1b/L5 · S3-SCN-FA F7):
#   conference_roster_visible|hidden = who 의 conference NOTIFY 로스터에 to 역할 신원이 있는가/없는가(listen_visibility)
#   conference_warning_138 = who 의 conference SUBSCRIBE 거절 Warning warn-code 138(TS 24.379 §10.1.3.4.1 범위 밖)
#   dialog_consistent = who 가 받은 dialog NOTIFY 열(RFC 4235) 정합 — entity 별 dialog 하나·local/remote/direction 불변·상태 전진·terminated 1회·version 단조
CHECK_KINDS = ('conference_roster_visible', 'conference_roster_hidden', 'conference_warning_138', 'dialog_consistent')
# 실단말(real-ue) 역할이 행위자(from/who)가 될 수 있는 단계 — cimsue-cli drive 명령이 있는 것만(§3.3). 나머지는 컴파일 오류.
#   빠진 것: progress(피어) · refer(실스택이 REFER 최종 응답을 이벤트로 내지 않음) · replaces/join/subscribe(dialog 학습은 실스택 앱 몫) ·
#   publish(affiliation 은 기동 절차가 한다) · media_send/media_stop(송출은 실스택 것) · sds_*
REAL_UE_STEPS = frozenset(('register', 'deregister', 'wait', 'expect', 'invite', 'answer', 'reject', 'bye', 'media_hold',
                           'hold', 'resume', 'dtmf', 'pickup', 'group_call', 'floor_request', 'floor_release'))

# publish.payload — MCPTT affiliation 명령(TS 24.379 §9): affiliate(기본) | deaffiliate
PUBLISH_COMMANDS = ('affiliate', 'deaffiliate')
# subscribe.payload — 이벤트 패키지 토큰(RFC 6665 §7.2.1 event-type). 기본 dialog(RFC 4235)
_EVENT_TOKEN = re.compile(r'^[A-Za-z0-9][A-Za-z0-9.\-_]{0,63}$')
_BIND_REF = re.compile(r'^\$\{(\w+)\}$')

# floor_request.payload — 기대 결과(TS 24.380 Granted / Deny / Queue Position Info). any = 결과가 나오기만 하면 된다
FLOOR_OUTCOMES = ('granted', 'denied', 'queued', 'any')
# group_call 의 payload — listen = a=recvonly 청취 합류(dispatch_center.md §5.6, 비멤버 관제사). 비면 일반 멤버 개시
GROUP_CALL_MODES = ('listen',)
# invite/pickup 의 to 가 역할이 아니라 다이얼 번호일 때(대표번호·피처코드 대상) — E.164/내선/피처코드 문자
_DIAL_LITERAL = re.compile(r'^[0-9*#+]{1,32}$')


def is_dial_literal(v: Optional[str]) -> bool:
    """to 가 역할 이름이 아닌 다이얼 리터럴(번호) 또는 ${var} 바인딩인가 — 대표번호(TS 24.239 Flexible Alerting) 등 '역할 아닌 번호' 를 부를 때."""
    return bool(v) and bool(_DIAL_LITERAL.match(v) or _BIND_REF.match(v))

# media_hold.during 에 둘 수 있는 통화 중 동작(§7 ⓓ) — 컴파일러가 평평한 단계열로 푼다
DURING_STEPS = ('dtmf', 'hold', 'resume', 'refer', 'media_send', 'media_stop')

# invite.media.rtp — 그 호의 미디어 평면(§4): auto = SDP 교환 즉시 기본 원천으로 송출 · none = 시그널링 전용(SDP 는 오퍼, RTP 없음)
#   · explicit = 수신만 시작하고 송출은 media_send 가 부를 때
RTP_MODES = ('auto', 'none', 'explicit')

# 단계 어휘 표 — 콘솔 편집기 팔레트·속성 폼·kind 게이트의 정본(GET /scenarios/vocab).
#   group   : 팔레트 묶음 · actor: 행위자 인자 꼴(who|from|fromto|seconds|none)
#   kind    : 행위자 역할의 풀 kind 게이트 — 'peer' = 피어 풀만, 'ue' = UE 풀만, 'ue|trunk' = UE 또는 트렁크 계정 피어, None = 무관
#   metrics : 이 단계에 우선 제안하는 expect 지표
STEP_VOCAB = {
    'register':      {'group': 'reg',   'actor': 'who',     'kind': 'ue|trunk', 'metrics': ['code', 'rrd_ms'], 'desc': '역할 단말 전부 등록 (prelude)'},
    'deregister':    {'group': 'reg',   'actor': 'who',     'kind': 'ue|trunk', 'metrics': ['code'], 'desc': '등록 해제 — 흐름 끝(epilogue: run 종료 시) 또는 앞(prelude: register 뒤 곧바로 내려 미등록 착신 역할을 만든다 — CFNL 전환 시나리오)'},
    'wait':          {'group': 'reg',   'actor': 'seconds', 'kind': None,       'metrics': [], 'desc': '대기 (seconds)'},
    'invite':        {'group': 'call',  'actor': 'fromto',  'kind': None,       'metrics': ['code', 'srd_ms', 'ser_pct', 'seer_pct', 'video_pct', 'fork_alert_pct', 'retrans_rx_pct', 'thig_pct', 'early_media_pct', 'early_rtp_pct', 'cdiv_181_pct', 'cdiv_hi_pct'], 'desc': 'INVITE from → to (비동기). to 는 역할 또는 다이얼 번호 리터럴(대표번호 — 인스턴스의 다른 UE 역할이 포크 착신, ${var} 바인딩 가능). from 이 통화 중이면 상담 통화(두 번째 다이얼로그 — attended 전달의 전제). dial: national|international 이면 착신 역할의 E.164 를 그 꼴로 다이얼한다(대상 다이얼 플랜 번역 시험 — 노드 sip.dial_plan 필요)'},
    'progress':      {'group': 'peer',  'actor': 'who',     'kind': None,       'metrics': ['early_media_pct', 'early_rtp_pct', 'prack_pct'], 'desc': '183 Session Progress + SDP(early media) · 신뢰 1xx 면 PRACK — 피어(pbx/mgcf 링백) 또는 착신 UE(실 단말 안내음 모사; 그 뒤 answer 는 같은 answer 로 200)'},
    'answer':        {'group': 'call',  'actor': 'who',     'kind': None,       'metrics': ['code', 'srd_ms', 'ser_pct'], 'desc': '착신 대기 → after_ms 뒤 200'},
    'reject':        {'group': 'call',  'actor': 'who',     'kind': None,       'metrics': ['code', 'q850_rx_pct'], 'desc': '착신 대기 → payload 코드로 거절'},
    'no_answer':     {'group': 'call',  'actor': 'who',     'kind': None,       'metrics': [], 'desc': '착신에 응답하지 않는다(링잉만) — 망이 CANCEL 하는 것이 정상(무응답 착신전환 CFNR·대표번호 무응답 등, ringing_leg_cancelled)'},
    'unreachable':   {'group': 'call',  'actor': 'who',     'kind': None,       'metrics': [], 'desc': '도달 불가 단말 흉내(TS 24.604 CFNRc) — 이후 착신 INVITE 를 18x 없이 payload 코드(기본 480)로 즉시 거절한다. 망이 링잉 없는 480/408 을 도달 불가로 판정해 forward_not_reachable_id 로 전환(cause=503). 가상 단말만'},
    'bye':           {'group': 'call',  'actor': 'from',    'kind': None,       'metrics': ['sdd_ms', 'code', 'scr_pct', 'q850_rx_pct', 'dtmf_rx_pct'], 'desc': 'BYE → 최종 응답 (SDD)'},
    'media_hold':    {'group': 'media', 'actor': 'seconds', 'kind': None,       'metrics': ['rtp_loss_pct', 'jitter_ms', 'mos'], 'desc': '확립 뒤 seconds 유지, 끝에 RTP 표본 (during 로 통화 중 동작)'},
    'hold':          {'group': 'media', 'actor': 'from',    'kind': None,       'metrics': ['code', 'moh_rtp_pct'], 'desc': 're-INVITE sendonly — 피보류 단말의 수신 누계를 기준점으로 잡는다(보류 음악 판정 시작)'},
    'resume':        {'group': 'media', 'actor': 'from',    'kind': None,       'metrics': ['code', 'moh_rtp_pct'], 'desc': 're-INVITE sendrecv — 보류 구간의 피보류 단말 RTP 증분(≥ 5 패킷)을 moh_rtp_pct 로 판정한 뒤 재개'},
    'dtmf':          {'group': 'media', 'actor': 'from',    'kind': None,       'metrics': ['dtmf_rx_pct'], 'desc': '숫자열 송신 (payload) — 풀 dtmf 방식대로 RFC 4733 telephone-event 또는 in-band G.711 톤'},
    'media_send':    {'group': 'media', 'actor': 'who',     'kind': None,       'metrics': [], 'desc': 'RTP 송출 시작 — sample(생략 = 기본 원천)·loop·after_ms. SDP 교환 뒤에만'},
    'media_stop':    {'group': 'media', 'actor': 'who',     'kind': None,       'metrics': [], 'desc': 'RTP 송출 정지 (수신은 계속)'},
    'refer':         {'group': 'xfer',  'actor': 'fromto',  'kind': None,       'metrics': ['code'], 'desc': 'REFER 전달 from(전달자) → to (RFC 3515) — 전달자가 to 와 상담 통화 중이면 attended(Refer-To 에 Replaces), 아니면 blind'},
    'replaces':      {'group': 'xfer',  'actor': 'fromto',  'kind': 'ue',       'metrics': ['code', 'srd_ms'], 'desc': 'INVITE-Replaces (RFC 3891) — from 이 dialog 구독(subscribe)으로 배운 to 의 다이얼로그를 가져온다(BLF 클릭 픽업)'},
    'join':          {'group': 'xfer',  'actor': 'fromto',  'kind': 'ue',       'metrics': ['code', 'join_tap_pct'], 'desc': 'INVITE-Join (RFC 3911) — from 이 dialog 구독으로 배운 to 의 세션에 recvonly 청취 leg 로 합류(합법감청, SSRC 2개)'},
    'pickup':        {'group': 'xfer',  'actor': 'fromto',  'kind': 'ue',       'metrics': ['code', 'srd_ms'], 'desc': '당겨받기 — payload 피처코드를 다이얼(<code> 그룹 픽업 · to 가 있으면 <code><번호> 지정 픽업 — to 는 역할 또는 번호 리터럴(링잉 대표번호))'},
    'subscribe':     {'group': 'ctl',   'actor': 'who',     'kind': 'ue',       'metrics': ['code'], 'desc': 'SUBSCRIBE (RFC 6665) — payload 이벤트 패키지(기본 dialog), to = 감시 대상 역할(생략 = 자기 AoR). 최종 응답까지'},
    'publish':       {'group': 'ctl',   'actor': 'who',     'kind': 'ptt',      'metrics': ['code', 'affiliate_ms'], 'desc': 'PUBLISH — MCPTT affiliation 명령(TS 24.379 §9): payload affiliate|deaffiliate, group 생략 = 신원의 그룹'},
    'group_call':    {'group': 'ptt',   'actor': 'fromto',  'kind': 'ptt',      'metrics': ['code', 'srd_ms', 'group_fanout_ms', 'video_pct', 'listen_pct'], 'desc': 'PTT 그룹콜 — from 이 자기 그룹으로 INVITE, to(multi 역할) 멤버 전원 합류까지. payload listen = 그룹 밖 역할(member: false)의 a=recvonly 청취 합류(진행 중 세션에)'},
    'floor_request': {'group': 'ptt',   'actor': 'who',     'kind': 'ptt',      'metrics': ['floor_grant_ms', 'floor_taken_ms', 'floor_queue_ms', 'floor_grant_pct'], 'desc': 'Floor Request → 결과(payload: granted|denied|queued|any)'},
    'floor_release': {'group': 'ptt',   'actor': 'who',     'kind': 'ptt',      'metrics': ['floor_idle_ms'], 'desc': 'Floor Release → Idle 도달'},
    'sds_send':      {'group': 'ptt',   'actor': 'fromto',  'kind': 'ue',       'metrics': ['code', 'sds_delay_ms', 'sds_disposition_pct', 'sds_media_pct'], 'desc': 'MCData SDS 송신(TS 24.282) — payload 본문, to 역할 = 1:1 · to 없음 = 그룹 SDS(인스턴스 그룹 또는 group, 수신자는 multi 역할), disposition = delivery 회신 요청. plane: control(기본) = SIP MESSAGE(완료 = 최종 응답) · media = MSRP media plane(INVITE m=message → cmdp, 완료 = MSRP SEND 200/REPORT — 대상 CSP 는 그룹 SDS 만)'},
    'fd_send':       {'group': 'ptt',   'actor': 'fromto',  'kind': 'ue',       'metrics': ['code', 'fd_upload_ms', 'fd_delay_ms'], 'desc': 'MCData FD 파일 배포(TS 23.282 §7.4 + TS 24.282 §15.1.3) — IdMS 토큰 → POST /mcdata/fd(CSC 게이트 allow_fd·멤버십·크기) → FD SIGNALLING MESSAGE(FILEURL+Metadata). payload = 합성 크기(256k·2m) 또는 워커 샘플 파일, to 역할 = 1:1 · to 없음 = 그룹 FD(인스턴스 그룹 또는 group, 수신자는 multi 역할). 신원에 IdMS 로그인 필요'},
    'fd_recv':       {'group': 'ptt',   'actor': 'who',     'kind': 'ue',       'metrics': ['fd_delay_ms', 'fd_download_ms', 'fd_download_pct'], 'desc': 'who 전원이 앞선 fd_send 의 FD SIGNALLING 을 받을 때까지(단계 진입 전 도착도 인정) → payload download(기본)이면 각자 FILEURL 을 내려받아 크기 대조(fd_download_pct) · signal = 도착만. fd_delay_ms = MESSAGE 송신 → 도착'},
    'sds_recv':      {'group': 'ptt',   'actor': 'who',     'kind': 'ue',       'metrics': ['sds_delay_ms', 'sds_media_pct'], 'desc': 'who 전원이 앞선 sds_send 의 SDS 를 받을 때까지(단계 진입 전 도착도 인정) — sds_delay_ms = 송신 → 도착. media plane 배포(풀 msrp)·FILEURL 폴백 둘 다 도착으로 센다'},
    'expect':        {'group': 'ctl',   'actor': 'none',    'kind': None,       'metrics': ['ser_pct', 'scr_pct', 'isa_pct'], 'desc': '누계 지표 게이트'},
    'check':         {'group': 'ctl',   'actor': 'who',     'kind': 'ue',       'metrics': ['check_pct'], 'desc': '관측 정합 판정 — payload: conference_roster_visible|hidden(to = 로스터에서 찾을 역할) · conference_warning_138 · dialog_consistent(RFC 4235 NOTIFY 열). after_ms 뒤 판정, 틀리면 인스턴스 실패'},
}
for _k, _v in STEP_VOCAB.items():
    _v['real'] = _k in REAL_UE_STEPS   # 실단말(real-ue) 역할이 행위자가 될 수 있는가 — 편집기 행위자 칩 게이트
STEP_GROUPS = [
    {'id': 'reg', 'label': '등록'}, {'id': 'call', 'label': '호'}, {'id': 'media', 'label': '미디어'},
    {'id': 'peer', 'label': '피어 축'}, {'id': 'xfer', 'label': '전달·합류'}, {'id': 'ptt', 'label': 'PTT · MCData'},
    {'id': 'ctl', 'label': '이벤트·게이트'},
]

METRIC_LABELS = {
    'code': 'code — 응답 코드', 'rrd_ms': 'RRD — 등록 지연', 'srd_ms': 'SRD — 세션 요청 지연', 'sdd_ms': 'SDD — 세션 해제 지연',
    'sdt_s': 'SDT — 세션 지속', 'ser_pct': 'SER — 세션 확립률', 'seer_pct': 'SEER — 유효 확립률', 'scr_pct': 'SCR — 세션 완료율',
    'isa_pct': 'ISA — 시도 실패율', 'rtp_loss_pct': 'RTP 손실률', 'jitter_ms': 'RTP 지터', 'mos': 'MOS',
    'floor_grant_ms': 'Floor grant 지연', 'floor_taken_ms': 'Floor taken 도달', 'floor_queue_ms': 'Floor 큐 대기',
    'floor_idle_ms': 'Floor idle 도달', 'floor_grant_pct': 'Floor 허가율', 'group_fanout_ms': '그룹 fan-out 완료',
    'affiliate_ms': 'affiliation PUBLISH 지연',
    'sds_delay_ms': 'SDS 지연', 'sds_disposition_pct': 'SDS disposition 률', 'sds_media_pct': 'SDS media plane(MSRP) 도착률', 'dtmf_rx_pct': 'DTMF 수신률',
    'fd_upload_ms': 'FD 업로드(토큰 포함)', 'fd_delay_ms': 'FD SIGNALLING 지연', 'fd_download_ms': 'FD 다운로드', 'fd_download_pct': 'FD 다운로드 성공률',
    'q850_rx_pct': 'Q.850 Reason 수신률', 'early_media_pct': '183 early media 률', 'prack_pct': 'PRACK 률',
    'cdiv_181_pct': '착신전환 181 통지 도달률(발신자)', 'cdiv_hi_pct': '착신전환 History-Info 도달률(전환 대상)',
    'early_rtp_pct': 'early media RTP 도달률', 'moh_rtp_pct': '보류 음악 RTP 도달률(피보류 단말, hold 당)', 'join_tap_pct': 'Join 청취 leg SSRC 2개 도달률', 'video_pct': '영상 협상률(m=video 활성 answer)',
    'fork_alert_pct': '대표번호 포크 alert 률(그룹원 착신/기대)', 'listen_pct': 'PTT 청취 합류율(recvonly 200)',
    'retrans_rx_pct': 'INVITE 재전송 도달률(유실 주입 뒤)', 'thig_pct': 'THIG 토큰화 Via 보존률', 'check_pct': '관측 정합 판정 통과율(check)',
}

# Reason: Q.850 cause (ITU-T Q.850) — 편집기 목록
Q850_CAUSES = {
    16: '정상 종료', 17: '통화 중', 18: '무응답', 19: '응답 없음', 21: '거절', 27: '목적지 고장', 28: '번호 형식',
    31: '정상·미지정', 34: '회선 없음', 38: '망 고장', 41: '일시 고장', 42: '폭주', 47: '자원 없음', 63: '서비스 불가',
    102: '타이머 만료', 127: '불특정',
}
AUDIO_CODECS = ('amr-wb', 'amr', 'pcmu', 'pcma', 'g722')
VIDEO_CODECS = ('h264', 'none')

# 낮을수록 좋은 비율 — 기대치는 상한(스칼라 또는 max ≤). 나머지 비율은 하한(≥)
LOWER_BETTER_RATIOS = frozenset(('isa_pct',))

# expect 키 = RFC 6076 / RFC 3550 / TS 24.380 지표 이름(§5). 여기 없는 이름은 거절.
METRIC_NAMES = (
    'code',
    'rrd_ms', 'srd_ms', 'sdd_ms', 'sdt_s',
    'ser_pct', 'seer_pct', 'scr_pct', 'isa_pct',
    'rtp_loss_pct', 'jitter_ms', 'mos',
    # PTT(TS 24.380 메시지 시각) — 요청→Granted · 요청→다른 참가자의 Taken · 큐 경유 요청→Granted · 해제→Idle, 그룹 INVITE→마지막 멤버 합류
    'floor_grant_ms', 'floor_taken_ms', 'floor_queue_ms', 'floor_idle_ms', 'floor_grant_pct', 'group_fanout_ms', 'affiliate_ms',
    'sds_delay_ms', 'sds_disposition_pct', 'sds_media_pct',
    # MCData FD(TS 23.282 §7.4 HTTP 콘텐츠 서버) — 업로드(IdMS 토큰 포함) · FD SIGNALLING MESSAGE 송신→도착 · 다운로드 · 다운로드 성공(200 + 크기 일치)률
    'fd_upload_ms', 'fd_delay_ms', 'fd_download_ms', 'fd_download_pct',
    # 피어 pbx/mgcf 축(D) — 비율은 발생기 관측(송신 대비 수신)
    'dtmf_rx_pct', 'q850_rx_pct', 'early_media_pct', 'prack_pct',
    # 미디어 평면 — 183+SDP 뒤 200 전에 발신자가 실제 RTP 를 받았는가(시그널링 early_media_pct 와 별개)
    'early_rtp_pct',
    # 보류 음악(announcements.md §3.3) — hold 단계 뒤 피보류 단말이 RTP(≥ 5 패킷)를 받은 보류 / hold 송신
    'moh_rtp_pct',
    # 착신전환(TS 24.604, volte_supplementary_services §6A) — 발신자에 181 / 전환 대상 INVITE 에 History-Info(RFC 7044)
    'cdiv_181_pct', 'cdiv_hi_pct',
    # 합법감청 청취 leg(RFC 3911 Join) — 서버가 양 화자를 SSRC 2개로 분리 인도했는가
    'join_tap_pct',
    # 영상 — m=video 를 실은 발신 중 answer 에 활성 video m-line(포트>0)이 온 비율
    'video_pct',
    # 피어 오류 주입 후속(C) — 유실 주입 뒤 INVITE 재전송 도달률 · ibcf THIG 토큰화 Via 보존률
    'retrans_rx_pct', 'thig_pct',
    # check 단계(관측 정합 판정) 통과율
    'check_pct',
    # 대표번호(TS 24.239 Flexible Alerting) — to 가 번호 리터럴인 invite 에서 인스턴스의 다른 UE 역할(그룹원)에 포크 INVITE 가 도달한 비율
    'fork_alert_pct',
    # PTT 청취(dispatch_center.md §5.6) — group_call payload listen 의 recvonly INVITE 가 200 으로 확립된 비율
    'listen_pct',
    # 실단말(real-ue) 표본(§3.3) — 실스택 단말 leg 만 따로: 발신 SRD · RTP 손실/지터(pjmedia 통계) · MOS(min 이 판정)
    'real_srd_ms', 'real_rtp_loss_pct', 'real_jitter_ms', 'real_mos',
)


# 비율 지표의 분자/분모 카운터 — 요약·판정이 같은 정의를 쓴다(§5)
RATIO_METRICS = {
    'ser_pct': ('sessions', 'attempts'),
    'scr_pct': ('completed', 'sessions'),
    # RFC 6076 §4.4 SEER = (200 + 480/486/600/603) / INVITE 송신 · §4.6 ISA = (408/500/503/504 + Timer B 만료) / INVITE 송신
    #   분모 invite_tx = 워커가 낸 세션 개시 INVITE 전부(invite·상담·pickup·replaces·join·group_call). 401/407 재시도는 psip 이 흡수한다
    'seer_pct': ('seer_ok', 'invite_tx'),
    'isa_pct': ('isa_fail', 'invite_tx'),
    'dtmf_rx_pct': ('dtmf_rx', 'dtmf_tx'),          # 수신 이벤트 수 / 송신 숫자 수
    'q850_rx_pct': ('q850_rx', 'q850_tx'),          # Reason Q.850 수신 / 송신 (B2BUA 투과 여부)
    'early_media_pct': ('early_media', 'progress_tx'),   # 발신자에 도달한 183+SDP / 피어가 낸 183
    'prack_pct': ('prack_rx', 'progress_tx'),       # 피어 UAS 가 받은 PRACK / 낸 신뢰 183
    'early_rtp_pct': ('early_rtp_ok', 'progress_tx'),   # 200 전에 RTP(≥ 5 패킷)를 받은 발신자 / 피어가 낸 183
    'moh_rtp_pct': ('moh_rtp_ok', 'hold_tx'),           # 보류 중 피보류 단말에 RTP(≥ 5 패킷)가 닿은 보류 / hold re-INVITE 송신 — 서버 MOH(RELAY_PLAY) 도달
    'cdiv_181_pct': ('cdiv_181_rx', 'invite_tx'),       # 착신전환 181 Call Is Being Forwarded 를 받은 발신자 / INVITE 송신 (TS 24.604 §4.5.2.6.1 통지)
    'cdiv_hi_pct': ('cdiv_hi_rx', 'invite_tx'),         # History-Info(RFC 7044, cause=302) 를 실은 착신 INVITE / INVITE 송신 — 서버측 전환의 재타게팅 이력
    'floor_grant_pct': ('floor_granted', 'floor_request_tx'),   # Granted 수신 / Floor Request 송신
    'join_tap_pct': ('join_ssrc2', 'join_ok'),      # 표본 때 SSRC 2개를 받은 청취 leg / 확립된 Join
    'video_pct': ('video_ok', 'video_offered'),     # answer 에 활성 m=video / m=video 를 실은 INVITE(워커 Media.VideoFile 필요)
    'fork_alert_pct': ('fork_rx', 'fork_expected'),  # 번호 리터럴 다이얼 뒤 인스턴스 UE 에 도달한 포크 INVITE / 발신자를 뺀 UE 역할 수
    'listen_pct': ('listen_ok', 'listen_tx'),       # 확립된 청취 합류(200) / recvonly 청취 INVITE
    'retrans_rx_pct': ('invite_retrans_rx', 'peer_fault_drop'),   # 유실 주입 뒤 닿은 INVITE 재전송 벌 / 버린 벌 — 대상의 Timer A 재전송 복원력
    'thig_pct': ('thig_via_ok', 'thig_tx'),          # 응답에 토큰화 Via 가 보존된 발신 / THIG Via 를 얹은 ibcf 발신 INVITE
    'sds_disposition_pct': ('sds_disposition_rx', 'sds_disposition_req'),   # 발신자에 닿은 SDS NOTIFICATION(delivered) / delivery 를 요청한 SDS
    'sds_media_pct': ('sds_media_rx', 'sds_rx'),                           # MSRP(media plane)로 도착한 SDS / 도착한 SDS 전부(FILEURL 폴백 포함)
    'fd_download_pct': ('fd_dl_ok', 'fd_dl_tx'),                            # 200 + Metadata size 일치로 내려받은 FD / fd_recv 가 개시한 다운로드
    'check_pct': ('check_ok', 'check_tx'),           # check 단계 판정 통과 / 판정 수
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
    rtp: Literal['auto', 'none', 'explicit'] = Field(
        default='auto', description='미디어 평면 — auto: SDP 교환 즉시 송출 · none: 시그널링 전용 · explicit: media_send 가 부를 때만 송출')


class During(_Strict):
    """media_hold 유지 구간 안 시각 지정 동작(§7 ⓓ) — at_s = 확립 뒤 경과 초. 컴파일러가 `hold at_s → 동작 → hold 나머지` 로 푼다."""
    at_s: float = Field(ge=0)
    step: Literal['dtmf', 'hold', 'resume', 'refer', 'media_send', 'media_stop']
    who: Optional[List[str]] = None
    from_: Optional[str] = Field(default=None, alias='from')
    to: Optional[str] = None
    payload: Optional[str] = None
    sample: Optional[str] = Field(default=None, description='media_send — 샘플 id(topology.media.samples). 생략 = 풀 기본 원천')
    loop: Optional[bool] = Field(default=None, description='media_send — false 면 샘플 끝에서 송출 정지(기본 true)')
    expect: Dict[str, Expectation] = Field(default_factory=dict)

    @model_validator(mode='after')
    def _actor(self):
        if not (self.from_ or self.who):
            raise ValueError(f'during {self.step} 은 from 또는 who 가 필요하다')
        if self.step == 'dtmf' and (not self.payload or any(c not in '0123456789*#ABCDabcd' for c in self.payload)):
            raise ValueError('during dtmf 는 payload 에 숫자열(0-9 * # A-D)이 필요하다')
        if self.step == 'refer' and not (self.from_ and self.to):
            raise ValueError('during refer 는 from 과 to 가 필요하다')
        if (self.sample is not None or self.loop is not None) and self.step != 'media_send':
            raise ValueError('sample/loop 은 media_send 에만 둔다')
        return self


DialForm = Literal['e164', 'national', 'international']


class Step(_Strict):
    step: StepKind
    who: Optional[List[str]] = None
    from_: Optional[str] = Field(default=None, alias='from')
    to: Optional[str] = None
    dial: Optional[DialForm] = Field(default=None, description='invite 만 — 착신 역할의 E.164 신원을 어떤 꼴로 다이얼하는가: e164(기본, +82…) · national(국내형 0… — 대상 노드 '
                                                                'sip.dial_plan 의 접두) · international(국제 접두 00+82…). 대상 CSP 의 다이얼 플랜 번역(TS 24.229 §5.4.3.2) 시험. '
                                                                '역할 대상에만(번호 리터럴 to 에는 못 둔다)')
    after_ms: Optional[int] = Field(default=None, ge=0)
    seconds: Optional[Union[int, str]] = Field(default=None, description='정수 또는 ${ht} 같은 바인딩')
    media: Optional[Media] = None
    group: Optional[str] = Field(default=None, description='group_call — MCPTT 그룹 id 를 직접 지정(생략 = 인스턴스가 잡은 그룹, 곧 발신 멤버의 affiliation 그룹) · publish — affiliation 대상 그룹(생략 = 신원의 그룹) · sds_send/subscribe conference — 대상 그룹(생략 = 인스턴스 그룹)')
    payload: Optional[str] = Field(default=None, description='dtmf: 숫자열(0-9*#A-D) · reject: 응답 코드 · floor_request: 기대 결과 granted|denied|queued|any · '
                                                                 'pickup: 피처코드(${var} 바인딩 가능) · subscribe: 이벤트 패키지(기본 dialog) · publish: affiliate|deaffiliate · '
                                                                 'fd_send: 파일 원천(합성 크기 256k|2m 또는 워커 샘플 파일 이름) · fd_recv: download(기본)|signal')
    cause: Optional[int] = Field(default=None, ge=1, le=127,
                                 description='bye/reject 의 Reason: Q.850;cause= (RFC 3326 — MGCF 종료 사유, 16=정상 34=회선 없음)')
    during: Optional[List[During]] = Field(default=None, description='media_hold 만 — 유지 구간 안 통화 중 동작(at_s 순)')
    sample: Optional[str] = Field(default=None, description='media_send — 샘플 id(topology.media.samples). 생략 = 풀 기본 원천')
    loop: Optional[bool] = Field(default=None, description='media_send — false 면 샘플 끝에서 송출 정지(기본 true)')
    disposition: Optional[bool] = Field(default=None, description='sds_send — delivery disposition 요청(TS 24.282 §9.2.2): 수신 단말이 SDS NOTIFICATION(delivered)을 되보낸다(sds_disposition_pct)')
    plane: Optional[SdsPlane] = Field(default=None, description='sds_send — control(기본): SIP MESSAGE(C-plane) · media: MSRP media plane(TS 24.282 §9.2.3 — INVITE m=message → cmdp 종단, '
                                                                 '수신자는 풀 msrp 면 MSRP 배포·아니면 FILEURL 폴백). 대상 CSP 는 그룹 SDS 만 media plane 을 받는다(1:1 은 403)')
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
        if self.step in ('invite', 'refer', 'bye', 'sds_send', 'fd_send', 'hold', 'resume', 'dtmf') and not (self.from_ or self.who):
            raise ValueError(f'{self.step} 단계는 from 또는 who 가 필요하다')
        if self.step in ('pickup', 'replaces', 'join') and not self.from_:
            raise ValueError(f'{self.step} 단계는 from(발신 단말 역할)이 필요하다')
        if self.step in ('replaces', 'join') and not self.to:
            raise ValueError(f'{self.step} 단계는 to(대상 다이얼로그의 당사자 역할)가 필요하다 — from 이 그 역할을 dialog 구독(subscribe)해 배운다')
        if self.step == 'pickup':
            if not self.payload:
                raise ValueError('pickup 단계는 payload(당겨받기 피처코드 — 접속서비스 pickup_feature_code, ${var} 바인딩 가능)가 필요하다')
            if not (_BIND_REF.match(self.payload) or re.match(r'^[0-9*#+A-Da-d]{1,16}$', self.payload)):
                raise ValueError('pickup 의 payload 는 다이얼 가능한 피처코드(0-9 * # A-D) 또는 ${var} 바인딩')
        if self.step == 'subscribe' and self.payload is not None and not (_BIND_REF.match(self.payload) or _EVENT_TOKEN.match(self.payload)):
            raise ValueError('subscribe 의 payload 는 이벤트 패키지 토큰(RFC 6665 event-type — dialog·reg·…)')
        if self.step == 'publish' and self.payload is not None and self.payload not in PUBLISH_COMMANDS:
            raise ValueError(f'publish 의 payload(affiliation 명령)는 {list(PUBLISH_COMMANDS)} 중 하나')
        if self.step == 'check':
            if not (self.payload and (self.payload in CHECK_KINDS or _BIND_REF.match(self.payload))):
                raise ValueError(f'check 의 payload 는 {list(CHECK_KINDS)} 중 하나(또는 ${{var}} 바인딩 — 컴파일 때 검사)')
            if self.payload.startswith('conference_roster_') and not self.to:
                raise ValueError('check conference_roster_* 는 to(로스터에서 찾을 역할)가 필요하다')
        if self.step in ('register', 'deregister', 'answer', 'reject', 'no_answer', 'unreachable', 'progress', 'subscribe', 'publish',
                         'floor_request', 'floor_release', 'sds_recv', 'fd_recv', 'media_send', 'media_stop', 'check') and not self.who:
            raise ValueError(f'{self.step} 단계는 who 가 필요하다')
        if self.step in ('media_hold', 'wait') and self.seconds is None:
            raise ValueError(f'{self.step} 단계는 seconds 가 필요하다')
        if self.step == 'dtmf':
            if not self.payload or any(c not in '0123456789*#ABCDabcd' for c in self.payload):
                raise ValueError('dtmf 단계는 payload 에 숫자열(0-9 * # A-D)이 필요하다')
        if self.step == 'sds_send':
            if not self.payload:
                raise ValueError('sds_send 단계는 payload(SDS 본문)가 필요하다')
            if not self.from_:
                raise ValueError('sds_send 단계는 from(발신 단말 역할)이 필요하다 — to 역할 = 1:1, to 없음 = 그룹 SDS(그룹 세션 또는 group)')
        if self.step == 'fd_send':
            if not self.from_:
                raise ValueError('fd_send 단계는 from(발신 단말 역할)이 필요하다 — to 역할 = 1:1, to 없음 = 그룹 FD(그룹 세션 또는 group)')
            if not self.payload or not (_FD_SIZE.match(self.payload) or _FD_FILE.match(self.payload)) or '..' in self.payload:
                raise ValueError('fd_send 단계는 payload(파일 원천)가 필요하다 — 합성 크기(`65536`·`256k`·`2m`) 또는 워커 샘플 디렉터리의 파일 이름')
        if self.step == 'fd_recv' and self.payload is not None and self.payload not in FD_RECV_MODES:
            raise ValueError(f'fd_recv 의 payload 는 {list(FD_RECV_MODES)} 중 하나(생략 = download)')
        if self.disposition is not None and self.step != 'sds_send':
            raise ValueError('disposition 은 sds_send 에만 둔다')
        if self.plane is not None and self.step != 'sds_send':
            raise ValueError('plane 은 sds_send 에만 둔다')
        if self.step == 'refer' and not (self.from_ and self.to):
            raise ValueError('refer 단계는 from(전달자)과 to(전달 대상 역할)가 필요하다')
        if self.step == 'group_call' and not self.from_:
            raise ValueError('group_call 단계는 from(발신 멤버 역할)이 필요하다 — to 는 합류를 기다릴 multi 역할(선택)')
        if self.step == 'group_call' and self.payload is not None and self.payload not in GROUP_CALL_MODES:
            raise ValueError(f'group_call 의 payload 는 {list(GROUP_CALL_MODES)} 중 하나(listen = recvonly 청취 합류) 또는 생략')
        if self.step == 'invite' and not self.to:
            raise ValueError('invite 단계는 to(상대 역할 또는 다이얼 번호 리터럴)가 필요하다')
        if self.step == 'floor_request' and self.payload is not None and self.payload not in FLOOR_OUTCOMES:
            raise ValueError(f'floor_request 의 payload(기대 결과)는 {list(FLOOR_OUTCOMES)} 중 하나')
        if self.group is not None and self.step not in ('group_call', 'publish'):
            raise ValueError('group 은 group_call/publish 단계에만 둔다')
        if self.cause is not None and self.step not in ('bye', 'reject'):
            raise ValueError('cause 는 bye/reject 단계에만 둔다')
        if (self.sample is not None or self.loop is not None) and self.step != 'media_send':
            raise ValueError('sample/loop 은 media_send 단계에만 둔다')
        if self.media is not None and self.media.rtp != 'auto' and self.step not in ('invite', 'group_call'):
            raise ValueError('media.rtp 는 invite/group_call 단계에만 둔다')
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
    multi: bool = Field(default=False, description='인스턴스마다 단말 여럿 — 그룹 세션(group_call)에서 단일 역할들이 멤버를 하나씩 잡고 남은 그룹 멤버 전부')
    member: bool = Field(default=True, description='그룹 세션에서 false = 그룹 밖 신원(청취 관제사·비멤버) — 인스턴스가 잡은 그룹의 멤버가 아닌 PTT 단말을 '
                                                  '역할 풀에서 배정한다(다른 풀도 됨). 첫 group_call 의 from·to 는 될 수 없다')


# 대상 증거 종류 — recording_created/alarm_raised/event_logged = 대상 OAM API 를 run 창으로 센다 · log_errors = 호스트 SSH 관측의 로그 ERROR 증분 ·
#   rss_growth_mb/fd_growth = 호스트 SSH 관측의 프로세스별 RSS(MB)·열린 fd 수 처음↔끝 차(소크 누수 판정 — soak 프로파일과 짝, 원천 없으면 판정 불가)
EvidenceKind = Literal['recording_created', 'log_errors', 'alarm_raised', 'event_logged', 'rss_growth_mb', 'fd_growth']


class Evidence(_Strict):
    kind: EvidenceKind
    min: Optional[float] = Field(default=None, ge=0)
    max: Optional[float] = Field(default=None, ge=0)
    code: Optional[str] = Field(default=None, description='알람/이벤트 정의 코드 (alarm_catalog)')
    proc: Optional[str] = Field(default=None, description='rss_growth_mb/fd_growth — 판정할 프로세스("host/proc" 또는 프로세스 이름 = 모든 호스트). 비면 관측된 프로세스 전부 중 최댓값')

    @model_validator(mode='after')
    def _fields(self):
        if self.proc and self.kind not in ('rss_growth_mb', 'fd_growth'):
            raise ValueError('proc 은 rss_growth_mb/fd_growth 에만')
        if self.code and self.kind not in ('alarm_raised', 'event_logged'):
            raise ValueError('code 는 alarm_raised/event_logged 에만')
        if self.min is None and self.max is None:
            raise ValueError('min 또는 max 하나는 필요하다')
        return self


# 시험 픽스처 — 시나리오가 자기 전제(전화 그룹·픽업 그룹·역할·가입 서비스)를 선언하고, 컨트롤러가 run 직전에 **대상의 운영 프로비저닝 경로**
#   (CSC 관리 API — 대상 oam 노드 게이트웨이 경유: /api/v1/phone-groups·/api/v1/roles·/api/v1/users, 접속서비스는 CSP 컬렉션)로 적용·확인하고 run 뒤 되돌린다.
#   DB 직접 쓰기·CSP 내부 통지는 쓰지 않는다 — CSC 가 단일 쓰기 주체로서 CSP 에 통지한다(dispatch_center.md §8, sip_access_security.md P1).
#   멤버·배정 대상은 **역할 이름**으로 적는다 — 계획이 그 역할에 배정한 신원(단발 첫 인스턴스가 쓰는 것)에 입힌다.
FIXTURE_KINDS = ('phone_group', 'role', 'subscriber', 'access_service')
MonitorScope = Literal['none', 'own', 'listed', 'all']
ListenVisibility = Literal['hidden', 'visible']
_FIXTURE_KEY = re.compile(r'^[a-z][a-z0-9_]{0,31}$')


class FixturePhoneGroup(_Strict):
    """전화 그룹(dispatch_center.md §4·§5.1 — 픽업 그룹 + 선택 대표번호). 멤버 = 역할 이름(유선/이동 UE 풀). CSC 가 멤버의 pickup_group 을 그룹 id 로 파생한다."""
    kind: Literal['phone_group']
    members: List[str] = Field(min_length=1, description='그룹원 역할 이름 — 역할마다 계획의 첫 신원(단발 첫 인스턴스)이 멤버가 된다')
    pilot: Optional[str] = Field(default=None, description='대표번호(TS 24.239 Flexible Alerting) — 번호 리터럴 또는 ${var}. ${var} 인데 바인딩이 없으면 컨트롤러가 '
                                                            '내선형 번호(7<첫 멤버 끝 3자리>0)를 만들어 그 바인딩에 넣는다(시나리오 to: "${var}" 와 짝). 생략 = 픽업 그룹만')
    service_ref: Optional[str] = Field(default=None, description='대표번호의 서비스명(pilot 있을 때) — 생략 = 첫 멤버 신원의 현 service_ref')
    alert_mode: Literal['parallel', 'sequential'] = 'parallel'
    no_answer_sec: int = Field(default=8, ge=1, le=120)
    overflow: Optional[str] = Field(default=None, description='무응답 overflow 대상 역할 이름(선택)')

    @model_validator(mode='after')
    def _pilot_form(self):
        if self.pilot is not None and not is_dial_literal(self.pilot):
            raise ValueError('phone_group.pilot 은 번호 리터럴(0-9*#+) 또는 ${var}')
        if self.overflow and self.overflow in self.members:
            raise ValueError('phone_group.overflow 역할은 members 에 있을 수 없다')
        return self


class FixtureRole(_Strict):
    """역할(mcptt_authorization.md — 능력+범위). assign 의 역할 신원이 속한 person(users.id)에 배정한다(사람당 역할 하나 — 되돌릴 때 종전 역할로)."""
    kind: Literal['role']
    assign: List[str] = Field(min_length=1, description='배정 대상 역할 이름')
    monitor_call: MonitorScope = 'none'
    monitor_targets: List[str] = Field(default_factory=list, description='monitor_call=listed — phone_group 픽스처 키')
    ptt_listen: MonitorScope = 'none'
    ptt_targets: List[str] = Field(default_factory=list, description='ptt_listen=listed — MCPTT 그룹 id 리터럴 또는 ${group}(그룹 세션이 잡는 첫 그룹)')
    listen_visibility: str = Field(default='hidden', description='hidden|visible 또는 ${var} 바인딩(로스터 노출/은닉을 같은 시나리오로 두 번 볼 때)')
    history_read: Literal['none', 'scope', 'all'] = 'scope'

    @model_validator(mode='after')
    def _scope(self):
        if self.listen_visibility not in ('hidden', 'visible') and not _BIND_REF.match(self.listen_visibility):
            raise ValueError('role.listen_visibility 는 hidden|visible 또는 ${var}')
        if self.monitor_call == 'listed' and not self.monitor_targets:
            raise ValueError('role.monitor_call=listed 는 monitor_targets 가 필요하다')
        if self.monitor_call != 'listed' and self.monitor_targets:
            raise ValueError('role.monitor_targets 는 monitor_call=listed 에만')
        if self.ptt_listen == 'listed' and not self.ptt_targets:
            raise ValueError('role.ptt_listen=listed 는 ptt_targets 가 필요하다')
        if self.ptt_listen != 'listed' and self.ptt_targets:
            raise ValueError('role.ptt_targets 는 ptt_listen=listed 에만')
        return self


class FixtureSubscriber(_Strict):
    """가입 회선 속성 — 역할 신원의 회선 service_ref(가입 서비스 소속)·ringback_media(개인 링백 음원, announcements.md §6.3)·
    forward_to(착신전환 대상 — 역할 이름, TS 24.604 CFU, volte_supplementary_services.md §6A)를 바꾼다
    (CSC PUT /users/{person}/{kind}/{msisdn}). run 뒤 종전 값으로. 셋 중 하나는 있어야 한다."""
    kind: Literal['subscriber']
    roles: List[str] = Field(min_length=1)
    service_ref: Optional[str] = Field(default=None, min_length=1, description='접속서비스 이름 — access_service 픽스처 키 또는 대상에 이미 있는 서비스명')
    ringback_media: Optional[str] = Field(default=None, pattern=r'^(sys|op|sub):[A-Za-z0-9_.-]+$',
                                          description='피착신 가입자의 링백 음원 id(sys:|op:|sub:) — 발신자 프로파일의 ringback 이 켜져 있어야 들린다')
    forward_to: Optional[str] = Field(default=None, min_length=1,
                                      description='착신전환(CFU) 대상 역할 — 그 역할의 첫 신원 번호가 forward_id 로 들어간다. 이 회선으로 온 호는 서버가 그 역할로 전환한다(181·History-Info·전환 안내)')
    forward_busy_to: Optional[str] = Field(default=None, min_length=1, description='CFB — 이 회선이 통화중(486/600) 응답이면 그 역할로 전환(forward_busy_id, §6A.4)')
    forward_no_reply_to: Optional[str] = Field(default=None, min_length=1, description='CFNR — 이 회선이 링잉 뒤 no_reply_sec 안에 응답하지 않으면(링잉 뒤 480/408 포함) 그 역할로 전환(forward_no_reply_id)')
    no_reply_sec: Optional[int] = Field(default=None, ge=1, le=120, description='CFNR 무응답 시한(초, forward_no_reply_sec) — 없으면 대상 CSP 의 Setup.Sip.Cdiv.NoReplySec')
    forward_not_logged_in_to: Optional[str] = Field(default=None, min_length=1, description='CFNL — 이 회선이 미등록이면 그 역할로 전환(forward_not_logged_in_id)')
    forward_not_reachable_to: Optional[str] = Field(default=None, min_length=1, description='CFNRc — 이 회선이 도달 불가(Q.850 20 · 링잉 없이 480/408)면 그 역할로 전환(forward_not_reachable_id)')

    @model_validator(mode='after')
    def _any(self):
        targets = [self.forward_to, self.forward_busy_to, self.forward_no_reply_to, self.forward_not_logged_in_to, self.forward_not_reachable_to]
        if self.service_ref is None and self.ringback_media is None and all(t is None for t in targets) and self.no_reply_sec is None:
            raise ValueError('subscriber 픽스처는 service_ref·ringback_media·forward_*_to 중 하나는 있어야 한다')
        for t in targets:
            if t is not None and t in self.roles:
                raise ValueError('subscriber 픽스처 forward_*_to 는 roles 자신이 아니어야 한다(자기 전환)')
        return self


class FixtureAccessService(_Strict):
    """접속서비스 변종 — 역할 신원의 현 서비스 레코드를 복제해 이름·필드를 바꿔 대상 CSP 컬렉션 access_services 에 넣는다(태그 cims-tester, run 뒤 제거).
    subscriber 픽스처가 service_ref 로 참조한다(예: transfer_allowed=false 변종 — volte_supplementary_services.md §6.3 게이트)."""
    kind: Literal['access_service']
    from_role: str = Field(description='복제 원본 = 이 역할 첫 신원의 현 service_ref 레코드')
    set: Dict[str, Union[bool, int, str]] = Field(min_length=1, description='바꿀 필드(예: {transfer_allowed: false}). name/id/kind/domain 은 못 바꾼다')

    @model_validator(mode='after')
    def _fields(self):
        bad = {'id', 'name', 'kind', 'domain', 'tags'} & set(self.set)
        if bad:
            raise ValueError(f'access_service.set 에 {sorted(bad)} 는 둘 수 없다(복제본의 정체)')
        return self


Fixture = Annotated[Union[FixturePhoneGroup, FixtureRole, FixtureSubscriber, FixtureAccessService], Field(discriminator='kind')]


class Scenario(_Strict):
    id: str = Field(pattern=r'^[A-Z0-9][A-Z0-9-]{2,63}$')
    title: Optional[str] = None
    tags: List[str] = Field(default_factory=list)
    roles: Dict[str, Role] = Field(min_length=1)
    flow: List[Step] = Field(min_length=1)
    target_evidence: List[Evidence] = Field(default_factory=list)
    fixtures: Dict[str, Fixture] = Field(default_factory=dict, description='시험 픽스처 — 키 = 이름(소문자·숫자·_), 값 = kind 별 선언. run 직전 대상 CSC 관리 API 로 적용·확인, run 뒤 복원')

    @model_validator(mode='after')
    def _refs(self):
        names = set(self.roles)
        self._check_fixtures(names)
        for r, spec in self.roles.items():
            if spec.disjoint_from and spec.disjoint_from not in names:
                raise ValueError(f'roles.{r}.disjoint_from={spec.disjoint_from!r} 는 정의된 역할이 아니다')
        for i, s in enumerate(self.flow):
            for ref in [*(s.who or []), s.from_, s.to]:
                if ref and ref not in names:
                    if ref == s.to and s.step in ('invite', 'pickup', 'subscribe') and is_dial_literal(ref):
                        continue   # 다이얼 번호 리터럴(대표번호·${pilot}) — 역할이 아니다(subscribe 는 대표번호 dialog 감시 — F7)
                    raise ValueError(f'flow[{i}] ({s.step}) 가 정의되지 않은 역할 {ref!r} 을 참조한다'
                                     + (' (invite/pickup 의 to 는 번호 리터럴(0-9*#+) 또는 ${var} 도 된다)' if ref == s.to and s.step in ('invite', 'pickup') else ''))
            for d in (s.during or []):
                for ref in [*(d.who or []), d.from_, d.to]:
                    if ref and ref not in names:
                        raise ValueError(f'flow[{i}].during ({d.step}) 가 정의되지 않은 역할 {ref!r} 을 참조한다')
            if s.dial is not None:
                if s.step != 'invite':
                    raise ValueError(f'flow[{i}] dial 은 invite 에만 둔다')
                if not s.to or s.to not in names:
                    raise ValueError(f'flow[{i}] invite dial={s.dial} 은 착신이 역할일 때만 — 번호 리터럴 to 는 이미 다이얼 꼴이다')
        self._check_group_session(names)
        self._check_dialog_learning()
        # 송출 제어(media_send/media_stop)는 그 호에서 SDP 가 오간 뒤(183 progress 또는 200 answer)에만, rtp: none 인 호에는 못 둔다
        rtp, sdp = None, False
        for i, s in enumerate(self.flow):
            if s.step == 'invite':
                rtp, sdp = (s.media.rtp if s.media else 'auto'), False
            elif s.step == 'group_call':
                rtp, sdp = (s.media.rtp if s.media else 'auto'), True   # 완료 = 발신자 200 + 멤버 자동응답 — SDP 교환이 끝난 상태
            elif s.step in ('answer', 'progress'):
                sdp = True
            elif s.step == 'bye':
                rtp, sdp = None, False
            ctl = [s.step] if s.step in ('media_send', 'media_stop') else []
            ctl += [d.step for d in (s.during or []) if d.step in ('media_send', 'media_stop')]
            for c in ctl:
                if rtp is None or not sdp:
                    raise ValueError(f'flow[{i}] {c} 는 invite 뒤 SDP 가 오간 다음(progress/answer 뒤)에만 둔다')
                if rtp == 'none':
                    raise ValueError(f'flow[{i}] {c} — 그 호의 invite.media.rtp 가 none(시그널링 전용)이다')
        return self

    def _check_fixtures(self, names) -> None:
        """픽스처 참조 무결성 — 역할 이름·픽스처 키·${group} 은 그룹 세션에서만."""
        pg_keys = {k for k, f in self.fixtures.items() if f.kind == 'phone_group'}
        svc_keys = {k for k, f in self.fixtures.items() if f.kind == 'access_service'}
        for k, f in self.fixtures.items():
            if not _FIXTURE_KEY.match(k):
                raise ValueError(f'fixtures.{k}: 키는 소문자로 시작하는 [a-z0-9_] 32자 이내')
            refs = []
            if f.kind == 'phone_group':
                refs = [*f.members, *([f.overflow] if f.overflow else [])]
            elif f.kind == 'role':
                refs = list(f.assign)
                for t in f.monitor_targets:
                    if t not in pg_keys:
                        raise ValueError(f'fixtures.{k}.monitor_targets={t!r} 는 phone_group 픽스처 키가 아니다')
                for t in f.ptt_targets:
                    if _BIND_REF.match(t):
                        if t != '${group}':
                            raise ValueError(f'fixtures.{k}.ptt_targets 의 바인딩은 ${{group}} 만(그룹 세션의 첫 그룹)')
                        if not self.is_group_session():
                            raise ValueError(f'fixtures.{k}.ptt_targets=${{group}} 은 group_call 이 있는 시나리오에서만')
            elif f.kind == 'subscriber':
                refs = list(f.roles)
                if f.service_ref is not None and f.service_ref in self.fixtures and f.service_ref not in svc_keys:
                    raise ValueError(f'fixtures.{k}.service_ref={f.service_ref!r} 는 access_service 픽스처가 아니다')
            elif f.kind == 'access_service':
                refs = [f.from_role]
            for r in refs:
                if r not in names:
                    raise ValueError(f'fixtures.{k} 가 정의되지 않은 역할 {r!r} 을 참조한다')
        # 같은 역할이 두 전화 그룹의 멤버일 수는 없다(가입자당 그룹 하나)
        seen: Dict[str, str] = {}
        for k in sorted(pg_keys):
            for r in self.fixtures[k].members:
                if r in seen:
                    raise ValueError(f'역할 {r!r} 이 전화 그룹 픽스처 {seen[r]}·{k} 둘에 있다 — 가입자당 그룹 하나')
                seen[r] = k

    def _check_dialog_learning(self) -> None:
        """replaces/join(RFC 3891/3911)은 대상 다이얼로그를 dialog 이벤트(RFC 4235)로 배워야 한다 — 앞에 from 이 to 를 감시하는
        subscribe(payload dialog) 가 있어야 한다. 워커도 같은 전제로 NOTIFY(early|confirmed) 를 기다린다."""
        watching = set()   # (subscriber, watched)
        for i, s in enumerate(self.flow):
            if s.step == 'subscribe' and (s.payload in (None, 'dialog')):
                for w in s.who or []:
                    watching.add((w, s.to or w))
            elif s.step in ('replaces', 'join'):
                if (s.from_, s.to) not in watching:
                    raise ValueError(f'flow[{i}] {s.step}: {s.from_!r} 이 {s.to!r} 를 dialog 구독하는 subscribe 단계가 앞에 있어야 한다'
                                     f"(RFC 4235 NOTIFY 로 대상 다이얼로그를 배운다) — {{ step: subscribe, who: [{s.from_}], to: {s.to} }}")

    def is_group_session(self) -> bool:
        """인스턴스 = MCPTT 그룹 하나인 시나리오 — group_call 또는 to 없는 sds_send/fd_send(그룹 SDS·FD, 수신자 = multi 역할)."""
        return any(s.step == 'group_call' or (s.step in ('sds_send', 'fd_send') and not s.to and not s.group) for s in self.flow)

    def multi_roles(self) -> List[str]:
        return [r for r, spec in self.roles.items() if spec.multi]

    def guest_roles(self) -> List[str]:
        """그룹 세션의 그룹 밖 역할(member: false) — 청취 관제사·비멤버 거절 시험."""
        return [r for r, spec in self.roles.items() if not spec.member]

    def dial_targets(self) -> List[str]:
        """역할이 아닌 다이얼 번호 리터럴 to(invite/pickup) — 대표번호 등(중복 제거)."""
        out: List[str] = []
        for s in self.flow:
            if s.step in ('invite', 'pickup') and s.to and s.to not in self.roles and s.to not in out:
                out.append(s.to)
        return out

    def _check_group_session(self, names) -> None:
        """그룹 세션 시나리오(group_call) 규칙 — 인스턴스 = MCPTT 그룹 하나: 단일 역할은 멤버 하나씩, multi 역할(하나만)은 나머지 전부.
        그래서 역할은 모두 같은 풀이어야 하고(그룹 멤버가 한 풀에 있다), multi 역할은 여럿이 함께 할 수 있는 단계에만 선다."""
        multi = self.multi_roles()
        guests = self.guest_roles()
        if not self.is_group_session():
            if multi:
                raise ValueError(f'roles.{multi[0]}.multi 는 group_call(또는 그룹 SDS/FD sds_send·fd_send)이 있는 시나리오에만 둔다')
            if guests:
                raise ValueError(f'roles.{guests[0]}.member=false 는 group_call 이 있는 시나리오에만 둔다(그룹 밖 신원)')
            for i, s in enumerate(self.flow):
                if s.step in ('floor_request', 'floor_release'):
                    raise ValueError(f'flow[{i}] {s.step} 는 group_call 뒤에만 둔다')
            return
        if len(multi) > 1:
            raise ValueError(f'multi 역할은 하나만 둔다(그룹의 나머지 멤버) — {multi}')
        for g in guests:
            if g in multi:
                raise ValueError(f'roles.{g}: member=false 역할은 multi 가 될 수 없다(그룹 밖 신원은 인스턴스마다 하나)')
        pools = {spec.pool for r, spec in self.roles.items() if r not in guests}
        if len(pools) > 1:
            raise ValueError(f'그룹 세션 시나리오의 멤버 역할은 모두 같은 풀이어야 한다 — {sorted(pools)} (그룹 밖 역할은 member: false 로 표시)')
        in_session = False
        first_call = True
        for i, s in enumerate(self.flow):
            if s.step in ('invite', 'answer', 'reject', 'progress', 'refer', 'hold', 'resume', 'dtmf', 'pickup', 'replaces', 'join'):
                raise ValueError(f'flow[{i}] {s.step} — 그룹 세션 시나리오에는 1:1 호 단계를 섞지 않는다')
            if s.step == 'group_call':
                if s.from_ in multi:
                    raise ValueError(f'flow[{i}] group_call.from={s.from_!r} 은 단일 역할이어야 한다')
                if s.to and s.to not in multi:
                    raise ValueError(f'flow[{i}] group_call.to={s.to!r} 는 multi 역할이어야 한다(합류를 기다릴 나머지 멤버)')
                if first_call and s.from_ in guests:
                    raise ValueError(f'flow[{i}] 첫 group_call 의 from={s.from_!r} 은 그룹 멤버 역할이어야 한다(인스턴스의 그룹을 정한다) — '
                                     f'그룹 밖 역할(member: false)은 그 뒤에 listen 합류 또는 거절(expect.code 403) 로')
                if s.payload == 'listen' and s.from_ not in guests:
                    raise ValueError(f'flow[{i}] group_call payload listen 의 from={s.from_!r} 은 그룹 밖 역할(member: false)이어야 한다 — '
                                     f'청취 합류는 비멤버 관제사의 recvonly INVITE (TS 24.379 비멤버 · dispatch_center.md §5.6)')
                if s.payload == 'listen' and not in_session:
                    raise ValueError(f'flow[{i}] group_call payload listen 은 진행 중인 그룹 세션(앞선 group_call) 뒤에만 둔다')
                if s.payload == 'listen' and s.to:
                    raise ValueError(f'flow[{i}] group_call payload listen 은 to 를 두지 않는다(합류 대기는 청취자 자기 200 만)')
                first_call = False
                in_session = True
            elif s.step in ('floor_request', 'floor_release') and not in_session:
                raise ValueError(f'flow[{i}] {s.step} 는 group_call 뒤에만 둔다')
            elif s.step == 'bye':
                in_session = False

    def sample_refs(self) -> List[str]:
        """이 시나리오가 참조하는 샘플 id(순서 보존·중복 제거)."""
        out: List[str] = []
        for s in self.flow:
            for x in [s, *(s.during or [])]:
                if x.step == 'media_send' and x.sample and x.sample not in out:
                    out.append(x.sample)
        return out


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
    ptt_group: Optional[str] = Field(default=None, description='service=ptt — 이 단말이 affiliation 하는 MCPTT 그룹 id(가상 단말 하나 = 그룹 하나)')
    login: Optional[str] = Field(default=None, description='IdMS 로그인 id(users.login_id — SIP 자격과 별개, creds `login`/DB users) — MCData FD 의 토큰(POST/GET /mcdata/fd). '
                                                            '비면 fd_send/fd_recv(download) 의 행위자가 될 수 없다(컴파일 오류)')
    login_pw: Optional[str] = Field(default=None, description='IdMS 로그인 비밀번호(users.passwd, creds `loginPw`)')


class TargetCsc(_Strict):
    """풀이 닿는 CSC(subscriber 노드 api — IdMS `/idms/*` + MCData FD 콘텐츠 서버 `/mcdata/fd`, 같은 host:port). 컨트롤러가 토폴로지에서 파생."""
    ip: str
    port: int = Field(default=4430, ge=1, le=65535)
    tls: bool = True


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
    answer: PeerAnswer = 'normal'
    fault: Optional[PeerFault] = None
    prack: Optional[bool] = None
    dtmf: DtmfMode = 'rfc4733'
    thig: bool = False
    tls_client_auth: bool = False
    tls_verify: bool = False
    tls_client_cert: bool = False


class PoolCreate(_Strict):
    """POST /pools — 풀 생성·신원 적재. 멱등(pool 이름 기준)."""
    pool: str
    kind: Literal['ue', 'peer', 'real-ue']
    identities: List[Identity] = Field(default_factory=list)
    transport: Transport = 'udp'
    srtp: SrtpMode = 'off'
    service: ServiceKind = Field(default='volte', description='kind=ue — ptt 면 MCPTT 단말(기동 절차·자동응답·floor)')
    prack: bool = Field(default=False, description='kind=ue — 100rel/PRACK')
    dtmf: DtmfMode = Field(default='rfc4733', description='kind=ue — rfc4733(telephone-event 오퍼/echo) · inband(G.711 톤) · off')
    tls_verify: bool = Field(default=False, description='kind=ue|real-ue — 서버 TLS 인증서 검증(ue: 워커 Tls.CaFile · real-ue: RealUe.TlsCaFile 앵커, 없으면 검증 없이)')
    tls_client_cert: bool = Field(default=False, description='kind=ue — 워커 Tls.ClientCertFile 을 클라이언트 인증서로 제시(대상 접속점 상호인증)')
    nat: Optional[UeNat] = Field(default=None, description='kind=ue — NAT 풀: 워커가 netns 안에서 스택을 띄운다(local_ip 가 단말 주소)')
    media_agent: Optional[str] = Field(default=None, description='kind=ue — 미디어 전담 워커의 제어 URL(http://ip:port) — CRtpThread 원격 모드(RtpRemote.h)')
    msrp: bool = Field(default=False, description='kind=ue — MCData media plane 능력(Contact icsi-ref 에 mcdata.sds) — MSRP 배포 수신 대상')
    target_csp: TargetCsp = Field(description='풀이 닿는 SIP 서버 — 컨트롤러가 토폴로지 노드 참조에서 파생')
    target_csc: Optional[TargetCsc] = Field(default=None, description='kind=ue — 대상 CSC(subscriber 노드 api) — MCData FD 의 IdMS 토큰·콘텐츠 서버. '
                                                                       '토폴로지에 subscriber 노드가 있으면 파생(풀 subscriber 로 고른다), 없으면 fd_* 단계 불가')
    peer: Optional[WorkerPeer] = Field(default=None, description='kind=peer 일 때 프로파일·bind·신원 범위')
    trunk_register: Optional[TrunkRegister] = Field(default=None, description='kind=peer(pbx) 트렁크 REGISTER 계정 — 비밀 해석 완료본')


class CompiledStep(_Strict):
    """시나리오 단계 + 바인딩 해석 결과 — 워커가 실행하는 단위."""
    idx: int = Field(ge=0)
    step: StepKind
    who: List[str] = Field(default_factory=list, description='역할 이름 — 워커는 roles 매핑으로 풀을 찾는다')
    from_: Optional[str] = Field(default=None, alias='from')
    to: Optional[str] = None
    dial: Optional[DialForm] = Field(default=None, description='invite — 착신 역할 신원의 다이얼 꼴(e164|national|international, 풀 target_csp.dial_plan 으로 변환)')
    after_ms: int = 0
    seconds: Optional[int] = None
    media: Optional[Media] = None
    group: Optional[str] = None
    payload: Optional[str] = None
    cause: Optional[int] = None
    sample: Optional[str] = None
    loop: Optional[bool] = None
    disposition: Optional[bool] = None
    plane: Optional[SdsPlane] = None
    expect: Dict[str, Expectation] = Field(default_factory=dict)


class RunStart(_Strict):
    """POST /runs — 컴파일된 시나리오 + 이 워커의 배분."""
    run_id: str
    scenario_id: str
    roles: Dict[str, str] = Field(description='역할 → 풀 이름')
    role_slices: Dict[str, List[int]] = Field(
        default_factory=dict, description='역할 → 이 워커가 맡는 신원 인덱스 [begin, end) — 워커 분산')
    multi_roles: List[str] = Field(default_factory=list, description='인스턴스마다 단말 여럿인 역할 — 그룹 세션의 나머지 멤버')
    guest_roles: List[str] = Field(default_factory=list, description='그룹 세션의 그룹 밖 역할(member: false) — 잡은 그룹의 멤버가 아닌 PTT 단말을 역할 풀 free 목록에서 배정')
    steps: List[CompiledStep] = Field(min_length=1)
    samples: Dict[str, Dict[str, str]] = Field(
        default_factory=dict, description='이 시나리오가 참조하는 샘플 — id → {코덱: 워커 샘플 디렉터리 안 파일|synthetic} (topology.media.samples 발췌)')
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
    service: Optional[ServiceKind] = None
    affiliated: Optional[int] = Field(default=None, description='service=ptt — 그룹 affiliation 200 을 받은 단말 수')
    groups: Optional[int] = Field(default=None, description='service=ptt — 이 풀 신원이 속한 MCPTT 그룹 수')


class WorkerHealthMedia(_Strict):
    rtp_streams: int = Field(default=0, ge=0, description='RTP 를 쓰는 단말 수(진행 중 인스턴스의 단말, rtp: none 제외)')
    max_rtp_streams: int = Field(default=0, ge=0, description='Media.MaxRtpStreams — 0 = 제한 없음')
    sample_dir: str = ''
    files: List[str] = Field(default_factory=list, description='샘플 디렉터리의 파일 이름 — 컨트롤러가 샘플 라이브러리와 대조')


class WorkerHealthRealUe(_Strict):
    processes: int = Field(default=0, ge=0, description='살아 있는 cimsue-cli 프로세스 수')
    max: int = Field(default=0, ge=0, description='RealUe.MaxProcesses')
    cli: str = Field(default='', description='RealUe.CliPath 해석 결과')


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
    local_ip: Optional[str] = None
    media: Optional[WorkerHealthMedia] = None
    real_ue: Optional[WorkerHealthRealUe] = None


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


class SipDumpMessage(_Strict):
    t: float
    dir: Literal['tx', 'rx']
    transport: Literal['udp', 'tcp', 'tls']
    peer: str = Field(description='상대 ip:port')
    text: str = Field(description='SIP 메시지 원문 (psip 로그 버퍼 8 KB 에서 잘릴 수 있다)')


class StreamSip(_Strict):
    """워커 SIP 덤프 — 끝난 인스턴스의 Call-ID 하나(워커 `Sip.Capture`: failed = 실패한 인스턴스만 · all = 전부).
    컨트롤러가 `runs/<id>/sip/<call_id>.log` 로 적는다(§5)."""
    kind: Literal['sip']
    t: float
    run_id: str
    worker: str
    call_id: str
    instance: Optional[int] = None
    messages: List[SipDumpMessage] = Field(default_factory=list)


class StreamLog(_Strict):
    kind: Literal['log']
    t: float
    worker: str
    level: Literal['debug', 'info', 'warn', 'error']
    msg: str


StreamRecord = Union[StreamHello, StreamAgg, StreamEvent, StreamSip, StreamLog]


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
    summary: Dict[str, Union[int, float, str, Dict[str, float], None]] = Field(default_factory=dict,
                                                                          description="RFC 6076 요약 — 값은 수·문자열(코드 분해) 또는 이름별 수(프로세스별 CPU 피크·RSS 증감)")
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
    'worker_stream_sip': StreamSip,
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
