"""계측기 계약 모델 — 시나리오 3층(토폴로지·시나리오·부하 프로파일) + 컨트롤러↔워커 메시지.

test_instrument.md §4·§6 의 정본. 여기 pydantic 모델이 SoT 이고 `schema/*.json` 은
`bin/gen-schemas` 가 이 모델에서 생성한 파생물이다(단위시험이 동기화를 검사한다).
워커(C++)는 JSON 스키마를 읽어 같은 계약을 검증한다.

원칙: 모든 모델은 `extra='forbid'` — 오타 난 키가 조용히 무시되면 "기대치가 안 걸린 시험"이
PASS 로 보인다. 지표 이름은 RFC 6076 어휘(`rrd_ms`·`srd_ms`·`sdd_ms`·`ser_pct`·`scr_pct`)를
그대로 쓴다(§5).
"""
from __future__ import annotations

from typing import Dict, List, Literal, Optional, Union

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator


class _Strict(BaseModel):
    model_config = ConfigDict(extra='forbid', populate_by_name=True)


# ──────────────────────────────────────────────────────────────────────────
#  1. 토폴로지 — 대상(SUT)·워커·풀
# ──────────────────────────────────────────────────────────────────────────

Transport = Literal['udp', 'tcp', 'tls']
SrtpMode = Literal['off', 'optional', 'required']
PeerProfile = Literal['ibcf', 'pbx', 'mgcf']
ObserveSource = Literal['oam_stats', 'oam_alarms', 'agent_heartbeat', 'ssh_proc']


class TargetPeering(_Strict):
    """대상 CSP 의 피어링 접속점(edge=peering LocalNode) — 피어 풀 발신의 다음 홉이자 시드 route 의 local_node_ref.
    `local_node` 가 대상에 이미 있으면 그 레코드를 쓰고, 없으면 그 이름으로 LocalNode 를 시드한다(run 끝에 복원)."""
    ip: Optional[str] = Field(default=None, description='비면 csp.ip')
    port: int = Field(ge=1, le=65535)
    protocol: Transport = 'udp'
    local_node: str = Field(default='cims-tester-peering', description='대상 local_nodes 의 name')


class TargetCsp(_Strict):
    ip: str
    udp: int = 5060
    tcp: int = 25061
    tls: int = 5061
    domain_volte: Optional[str] = None
    domain_ptt: Optional[str] = None
    peering: Optional[TargetPeering] = Field(default=None, description='피어 풀이 쓰는 CSP 피어링 접속점 — 없으면 access UDP 접속점')


class TargetCsc(_Strict):
    host: str
    port: int = 4430
    tls: bool = True


class TargetOam(_Strict):
    """대상 관측·컬렉션 시드 전용 — 동거 형태여도 스토어를 직접 읽지 않고 이 API 로 본다(I5)."""
    url: str
    token_env: Optional[str] = Field(default=None, description='토큰을 담은 환경변수 이름')
    csp_deployment_id: Optional[int] = Field(default=None, ge=1,
                                             description='대상 CSP 의 배포 id — 비면 배포 목록에서 패키지 csp 를 찾는다')


class Target(_Strict):
    name: str
    csp: TargetCsp
    csc: Optional[TargetCsc] = None
    oam: Optional[TargetOam] = None
    observe: List[ObserveSource] = Field(default_factory=list)

    @model_validator(mode='after')
    def _observe_needs_source(self):
        if any(o.startswith('oam_') for o in self.observe) and self.oam is None:
            raise ValueError('observe 에 oam_* 가 있으면 target.oam 이 필요하다')
        return self


class Worker(_Strict):
    name: str
    url: str = Field(description='cims-tester-worker 제어 URL (http://ip:7100)')
    cpus: Optional[int] = Field(default=None, ge=1)


class DbSource(_Strict):
    db: Literal['target'] = Field(description="대상 DB — 토폴로지 target 의 CSC 가 알려주는 접속(계측기가 자격을 직접 갖지 않음)")
    table: Literal['volte_subscriptions', 'voip_subscriptions', 'ptt_subscriptions']
    offset: int = Field(default=0, ge=0)
    count: int = Field(ge=1)


class CredsSource(_Strict):
    creds: str = Field(description='creds JSONL 경로(cspsim -creds 승계) — scenarios/ 상대 또는 절대')
    count: Optional[int] = Field(default=None, ge=1)


class UePool(_Strict):
    kind: Literal['ue']
    source: Union[DbSource, CredsSource]
    transport: Transport = 'udp'
    srtp: SrtpMode = 'off'
    register_expires: int = Field(default=3600, ge=60)


class PeerBind(_Strict):
    ip: str
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
    user: str
    ha1_env: str = Field(description='H(A1) 을 담은 환경변수 — 비밀은 YAML 에 두지 않는다')


class PeerSeed(_Strict):
    """대상 CSP 컬렉션 시드 — 이 피어를 CSP 가 알게 하는 remote_node/route/rule/routing_policy 를 run 전에 넣고 끝에 복원한다
    (§3.2). 같은 `route_set` 을 가진 피어 풀은 한 RouteSet 의 멤버(priority/weight) — failover·round_robin 시험."""
    enabled: bool = True
    route_set: Optional[str] = Field(default=None, description='RouteSet 이름 — 생략=풀 이름')
    distribution: Literal['failover', 'round_robin', 'weighted', 'hash_by_caller'] = 'failover'
    priority: int = Field(default=100, ge=0)
    weight: int = Field(default=1, ge=1)
    acl: Optional[Literal['allow', 'deny']] = Field(default=None,
                                                    description='이 피어 소스 IP 에 대한 ACL(global) — deny 면 403 기대')


class PeerPool(_Strict):
    kind: Literal['peer']
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
    seed: PeerSeed = Field(default_factory=PeerSeed)


class RealUePool(_Strict):
    kind: Literal['real-ue']
    source: CredsSource
    transport: Transport = 'tls'
    srtp: SrtpMode = 'optional'


Pool = Union[UePool, PeerPool, RealUePool]


class Topology(_Strict):
    name: str
    target: Target
    workers: List[Worker] = Field(default_factory=list,
                                  description='수동 항목 — 배포 목록에서 자동 발견한 워커에 보태거나 덮어쓴다(§6.1)')
    pools: Dict[str, Pool] = Field(min_length=1)

    @field_validator('pools')
    @classmethod
    def _pool_names(cls, v):
        for k in v:
            if not k.replace('_', '').isalnum():
                raise ValueError(f'pool 이름은 영숫자·_ 만: {k!r}')
        return v


# ──────────────────────────────────────────────────────────────────────────
#  2. 시나리오 — 역할·단계·기대치 (기능·성능 공용)
# ──────────────────────────────────────────────────────────────────────────

StepKind = Literal[
    'register', 'deregister', 'invite', 'answer', 'reject', 'bye',
    'refer', 'replaces', 'join', 'pickup', 'subscribe', 'publish',
    'group_call', 'floor_request', 'floor_release', 'sds_send', 'sds_recv',
    'media_hold', 'wait', 'expect',
]

# expect 키 = RFC 6076 / RFC 3550 / TS 24.380 지표 이름(§5). 여기 없는 이름은 거절.
METRIC_NAMES = (
    'code',
    'rrd_ms', 'srd_ms', 'sdd_ms', 'sdt_s',
    'ser_pct', 'seer_pct', 'scr_pct', 'isa_pct',
    'rtp_loss_pct', 'jitter_ms', 'mos',
    'floor_grant_ms', 'floor_taken_ms', 'floor_queue_ms',
    'sds_delay_ms', 'sds_disposition_pct',
)


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


class Step(_Strict):
    step: StepKind
    who: Optional[List[str]] = None
    from_: Optional[str] = Field(default=None, alias='from')
    to: Optional[str] = None
    after_ms: Optional[int] = Field(default=None, ge=0)
    seconds: Optional[Union[int, str]] = Field(default=None, description='정수 또는 ${ht} 같은 바인딩')
    media: Optional[Media] = None
    group: Optional[str] = None
    payload: Optional[str] = None
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
        if self.step in ('invite', 'refer', 'bye', 'sds_send') and not (self.from_ or self.who):
            raise ValueError(f'{self.step} 단계는 from 또는 who 가 필요하다')
        if self.step in ('register', 'deregister', 'answer', 'reject', 'subscribe', 'publish',
                         'floor_request', 'floor_release', 'sds_recv') and not self.who:
            raise ValueError(f'{self.step} 단계는 who 가 필요하다')
        if self.step in ('media_hold', 'wait') and self.seconds is None:
            raise ValueError(f'{self.step} 단계는 seconds 가 필요하다')
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


class PoolCreate(_Strict):
    """POST /pools — 풀 생성·신원 적재. 멱등(pool 이름 기준)."""
    pool: str
    kind: Literal['ue', 'peer', 'real-ue']
    identities: List[Identity] = Field(default_factory=list)
    transport: Transport = 'udp'
    srtp: SrtpMode = 'off'
    target_csp: TargetCsp
    peer: Optional[PeerPool] = Field(default=None, description='kind=peer 일 때 프로파일·bind·신원 범위')


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
