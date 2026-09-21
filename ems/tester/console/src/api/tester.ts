// 계측기 API — base 게이트웨이 경유 /api/v1/tester/* (oam-cims-tester 가 서빙).
// 계약은 handlers/tester.py 의 TESTER_API_DOCS(개발자 모드 [API] 배지)와 같다.
import { api, authHeaders } from '@core/api/client'

export interface TesterHealth {
  module: string
  version: string
  data_dir: string
  scenarios: number
  profiles: number
  topologies: number
  runs: number
  active_runs: string[]
  workers?: number
  stream_subscribers: number
  worker_stream?: { ip: string; port: number; connections: number } | null
  phase: string
}

export interface ScenarioRow {
  id: string
  title: string | null
  tags: string[]
  steps: number
  source: 'bundled' | 'user'
  path: string
  errors: string[]
}

export interface ScenarioDetail {
  id: string
  doc: ScenarioDoc
  yaml: string | null
  source: 'bundled' | 'user' | null
  errors: string[]
  valid: boolean
}

export interface ScenarioStep {
  step: string
  who?: string[]
  from?: string
  to?: string
  after_ms?: number
  seconds?: number | string
  media?: { audio?: string; video?: string; rtp?: 'auto' | 'none' | 'explicit' }
  payload?: string
  sample?: string
  loop?: boolean
  cause?: number
  /** sds_send — delivery disposition 요청(수신 단말이 SDS NOTIFICATION 을 되보낸다) */
  disposition?: boolean
  /** sds_send — control(기본, SIP MESSAGE) · media(MSRP media plane — INVITE m=message → cmdp, 그룹 SDS 만) */
  plane?: 'control' | 'media'
  expect?: Record<string, unknown>
}

export interface ScenarioDoc {
  id: string
  title?: string
  tags?: string[]
  roles?: Record<string, { pool: string; disjoint_from?: string; count?: number; multi?: boolean; member?: boolean }>
  flow?: ScenarioStep[]
  target_evidence?: { kind: string; min?: number; max?: number; code?: string; proc?: string }[]
  /** 시험 픽스처 — 키 = 이름, 값 = kind(phone_group|role|subscriber|access_service) 별 선언. run 직전 대상 CSC 관리 API 로 적용, 끝나면 복원 */
  fixtures?: Record<string, Record<string, unknown> & { kind: string }>
}

export interface ProfileRow {
  name: string
  model: string | null
  source: 'bundled' | 'user'
  path: string
  errors: string[]
}

export interface ProfileDetail {
  name: string
  doc: Record<string, unknown>
  yaml: string | null
  source: 'bundled' | 'user' | null
  errors: string[]
  valid: boolean
}

export interface TopologyRow {
  id: number
  name: string
  created_at: string
  updated_at: string
  doc: TopologyDoc
}

// 토폴로지 v2 — 호스트 › 워커·대상 노드 › 풀 (test_instrument.md §4). 주소는 hosts 에만, 나머지는 파생.
export type NodeRole = 'sip' | 'tas' | 'media' | 'subscriber' | 'oam' | 'db'
export type Transport = 'udp' | 'tcp' | 'tls'

export interface TopoHost { name?: string; ip: string; ssh?: { user: string; key_env: string; port?: number } }
export interface TopoWorker { name: string; host: string; port?: number; cpus?: number; media?: { samples?: string[]; max_rtp_streams?: number } }
export type ListenerEdge = 'access' | 'peering'
/** SIP 수신점 하나 = 대상 LocalNode 하나. ip 비면 노드 addr → 호스트 ip. local_node = cims 대상 local_nodes 이름(비면 cims-tester-<id>) */
export interface SipListener { edge?: ListenerEdge; ip?: string; port: number; protocol?: Transport; local_node?: string }
export interface TopoNode {
  role: NodeRole; host: string; addr?: string; fn?: string; label?: string; procs?: string[]
  sip?: { domains?: string[]; listeners?: Record<string, SipListener> }
  tas?: { port?: number }
  media?: { rtp_range?: [number, number]; control?: number }
  api?: { port: number; tls?: boolean }
  oam?: { port?: number; tls?: boolean; token_env?: string; csp_deployment_id?: number; observe?: string[] }
  db?: { port?: number; name?: string; user_env?: string; password_env?: string }
}
export interface TopoPoolBase { worker: string; group?: string }
export interface UePoolDoc extends TopoPoolBase {
  kind: 'ue'; access: string; listener?: string
  source: { creds: string; offset?: number; count?: number } | { db: string; table: string; offset?: number; count: number; ptt_group?: string }
  /** 접속환경 클래스 — ptt 면 MCPTT 단말(PTT 도메인·GMS/CMS 구독·그룹 affiliation·floor). 생략 = table 이 ptt_subscriptions 면 ptt, 그 외 volte */
  service?: 'volte' | 'voip' | 'ptt'
  transport?: Transport; srtp?: 'off' | 'optional' | 'required'; register_expires?: number; prack?: boolean
  /** DTMF 방식 — rfc4733(telephone-event, 기본) · inband(G.711 톤) · off. 옛 문서의 boolean(true=rfc4733, false=off)도 서버가 읽는다 */
  dtmf?: DtmfMode | boolean
  /** transport=tls — 서버 인증서 검증(워커 Tls.CaFile) · 클라이언트 인증서 제시(워커 Tls.ClientCertFile, 접속점 상호인증) */
  tls_verify?: boolean; tls_client_cert?: boolean
  /** NAT 뒤 단말 — 워커 호스트 netns 안에서 스택을 띄운다(scripts/nat-netns.sh create <netns>, 워커 CAP_SYS_ADMIN) */
  nat?: { netns: string; local_ip: string }
  /** 미디어 전담 워커 — 이 풀의 RTP 를 그 워커(에이전트 /media/*)에서 굴린다(자기 워커와 다른 이름, PTT 불가) */
  media_worker?: string
  /** MCData media plane 능력 — Contact icsi-ref 에 mcdata.sds → 대용량 SDS 를 MSRP 로 받는 배포 대상(끄면 FILEURL 폴백) */
  msrp?: boolean
  /** MCData FD(fd_send/fd_recv)가 쓰는 CSC — role=subscriber 노드 id(api). 생략 = 대상의 유일한 subscriber 노드 */
  subscriber?: string
}
export type DtmfMode = 'rfc4733' | 'inband' | 'off'
export interface PeerPoolDoc extends TopoPoolBase {
  kind: 'peer'; peering: string; listener?: string; profile: 'ibcf' | 'pbx' | 'mgcf'; bind: { ip?: string; port: number; protocol?: Transport }; domain: string
  identities: { e164_range?: [string, string]; did_range?: [string, string]; ext_len?: number; count?: number }
  register?: { user: string; ha1_env?: string; password_env?: string; realm?: string; expires?: number }
  codecs?: string[]; answer?: 'normal' | 'silent' | 'reject' | 'delay'
  /** 오류 주입 — answer 매개변수(code/q850/delay_ms) + 와이어 유실(drop_invite: 새 INVITE 첫 N 벌 · drop_pct: 임의 메시지 %, UDP 만) */
  fault?: { code?: number; q850?: number; delay_ms?: number; drop_invite?: number; drop_pct?: number }
  prack?: boolean; dtmf?: DtmfMode | boolean
  /** ibcf — 발신 INVITE 에 토큰화 Via(THIG 흔적)를 얹고 응답 보존을 관측(thig_pct) */
  thig?: boolean
  /** TLS 상호인증 — tls_client_auth: 수신점(bind tls)이 클라이언트 인증서를 요구 · tls_verify: 발신 연결의 서버 검증 · tls_client_cert: 클라이언트 인증서 제시 */
  tls_client_auth?: boolean; tls_verify?: boolean; tls_client_cert?: boolean
  seed?: { enabled?: boolean; route_set?: string; distribution?: string; priority?: number; weight?: number; acl?: 'allow' | 'deny' }
}
/** 실단말 풀(test_instrument.md §3.3) — 신원마다 워커가 cimsue-cli(libcimsue/pjsua2 실스택) 프로세스를 띄운다. 단계는 vocab.real_ue_steps 만, 미디어 평면은 실스택 것 */
export interface RealUePoolDoc extends TopoPoolBase {
  kind: 'real-ue'; access: string; listener?: string
  source: { creds: string; offset?: number; count?: number } | { db: string; table: string; offset?: number; count: number; ptt_group?: string }
  service?: 'volte' | 'voip' | 'ptt'
  transport?: Transport; srtp?: 'off' | 'optional' | 'required'
  /** 서버 TLS 인증서 검증(워커 RealUe.TlsCaFile 앵커) — 기본 끔(개발 스택 자체 서명) */
  tls_verify?: boolean
}
export type PoolDoc = UePoolDoc | PeerPoolDoc | RealUePoolDoc
export interface TopoLayout { regions: Record<string, { x: number; y: number; w: number; h: number }>; items: Record<string, { x: number; y: number }> }

export interface TopologyDoc {
  name: string
  hosts: Record<string, TopoHost>
  workers: TopoWorker[]
  target: { name: string; kind?: 'cims' | 'ims' | 'pbx'; nodes: Record<string, TopoNode> }
  pools: Record<string, PoolDoc>
  media?: { samples?: Record<string, Record<string, string>> }
  layout?: TopoLayout
}

export interface CheckItem { name: string; ok: boolean; detail: string; ms: number; info?: boolean; target?: { kind: string; id: string } }

export interface WorkerRow {
  name: string
  url: string
  host?: string
  cpus: number | null
  media?: { samples?: string[]; max_rtp_streams?: number } | null
  up: boolean
  error: string | null
  topology_id: number
  health: {
    version?: string; max_endpoints?: number; max_saps?: number; cpu_pct?: number; active_endpoints?: number
    active_run?: string | null; clock_skew_ms?: number
    media?: { rtp_streams?: number; max_rtp_streams?: number; sample_dir?: string; files?: string[] }
    pools?: { pool: string; kind: string; endpoints: number; registered?: number }[]
  } | null
}

export type Verdict = 'running' | 'pass' | 'fail' | 'aborted' | 'error'
export type RunState = 'starting' | 'provisioning' | 'running' | 'stopping' | 'stopped' | string

export type Summary = Record<string, number | string | Record<string, number> | null | undefined>

export interface HistSummary {
  count: number
  mean: number | null
  min: number | null
  max: number | null
  p50: number | null
  p95: number | null
  p99: number | null
}

export interface PlanRoleRow {
  pool: string; kind: string; profile?: string | null; disjoint_from?: string | null; count?: number | null
  workers: Record<string, [string, number, number]>; total: number
}
export interface CompiledStep {
  idx: number; step: string; src?: number; who?: string[]; from?: string; to?: string; after_ms?: number; seconds?: number
  media?: { audio?: string; video?: string }; payload?: string; cause?: number; expect?: Record<string, unknown>
}
export interface RunPlan {
  roles: Record<string, PlanRoleRow>; identities?: Record<string, number>; phases?: { prelude: number[]; body: number[]; epilogue: number[] }
  max_instances: number | null; rate_total: number; peer_pools?: string[]; pinned?: string | null; steps?: CompiledStep[]
}

export interface RunLive {
  id: string
  state: RunState
  verdict: Verdict
  rate_saps: number
  scenario_id: string
  topology: string
  profile: string | null
  started_at: string
  workers: string[]
  counters: Record<string, number>
  timers: Record<string, HistSummary>
  gauges: Record<string, number>
  events: number
  doc_rate: number | null
  notes: string[]
  step_log: StepLogRow[]
  hold?: boolean
  held_s?: number
  label?: string | null
  target_build?: string | null
  stop_reason?: string | null
  plan?: RunPlan | null
  profile_doc?: Record<string, unknown> | null
}

export interface StepLogRow { t: number; rate: number; event?: string; attempts?: number; ihs_pct?: number }

export interface ExpectResult {
  step: number
  kind: string
  metric: string
  expect: unknown
  observed: unknown
  ok: boolean
}

export interface RunRow {
  id: string
  scenario_id: string
  topology: string
  profile?: string | null
  started_at: string
  ended_at?: string | null
  verdict: Verdict
  workers?: string[]
  summary?: Summary
  target_build?: string | null
  label?: string | null
  stop_reason?: string | null
  live?: RunLive
}

// run.json — 색인 + 본체 (GET /runs/<id>/report .run)
export interface RunDoc extends RunRow {
  label?: string | null
  bindings?: Record<string, unknown>
  plan?: RunPlan | null
  profile_doc?: Record<string, unknown> | null
  held_s?: number
  counters?: Record<string, number>
  timers?: Record<string, HistSummary>
  events?: number
  expect_results?: ExpectResult[]
  evidence_results?: EvidenceResult[]
  step_log?: StepLogRow[]
  stop_reason?: string | null
  doc_rate?: number | null
  notes?: string[]
}

export interface RunEvent {
  kind: 'event'
  t: number
  run_id: string
  worker: string
  call_id?: string | null
  role?: string | null
  identity?: string | null
  step?: string | null
  code?: number | null
  metric?: string | null
  observed?: number | null
  detail?: string | null
}

export interface RunSeries {
  id: string
  t: number[]
  counters: Record<string, number[]>
  gauges: Record<string, (number | null)[]>
  timers: Record<string, { p95: (number | null)[]; count: number[] }>
}

export interface CompareMetric {
  metric: string
  direction: 'up' | 'down'
  base: number | null
  values: (number | null)[]
  delta: (number | null)[]
  regression: (boolean | null)[]
}

export interface CompareResult {
  baseline: string
  runs: (Pick<RunDoc, 'id' | 'scenario_id' | 'topology' | 'profile' | 'started_at' | 'ended_at' | 'verdict' | 'target_build' | 'label' | 'summary' | 'timers' | 'expect_results' | 'stop_reason'> & { missing: boolean })[]
  metrics: CompareMetric[]
  same_scenario: boolean
  regressions: number
}

export interface PlanRequest {
  scenario_id?: string
  doc?: unknown
  yaml?: string
  topology_id?: number
  topology?: string
  profile?: string
  bindings?: Record<string, number | string>
  instances?: number
  rate_saps?: number
  probe?: boolean
}

export interface PlanWorkerRow {
  name: string; host?: string; ip?: string; url?: string | null; cpus?: number | null; up: boolean; error?: string | null; in_run: boolean
  capacity?: { max_endpoints?: number | null; max_saps?: number | null; active_endpoints?: number | null; active_run?: string | null; clock_skew_ms?: number | null; rtp?: number | null }
  share?: number; rate_saps?: number; max_instances?: number | null
  pools?: { pool: string; kind: string; identities: number; transport?: string; target: string }[]
  endpoints_needed?: number; roles?: Record<string, [string, number, number]>
}
export interface ProcedureRow { idx: number; phase: 'prelude' | 'body' | 'epilogue'; step: string; actors: string[]; summary: string; expect: Record<string, unknown>; desc?: string | null }
export interface PlanResult {
  ok: boolean; errors: string[]; warnings: string[]; notes: string[]
  scenario_id?: string; topology?: string; topology_id?: number; profile?: string | null; target?: { name: string; kind: string }
  roles?: Record<string, PlanRoleRow>; workers?: PlanWorkerRow[]; steps?: CompiledStep[]
  phases?: { prelude: number[]; body: number[]; epilogue: number[] }; procedure?: ProcedureRow[]
  bindings?: Record<string, unknown>; rate_total?: number; max_instances?: number | null; identities?: Record<string, number>
  peer_pools?: string[]; pinned?: string | null
  samples?: Record<string, Record<string, string>>; media?: { modes: string[]; uses_rtp: boolean }
  seed?: { collection: string; count: number; names: string[]; note?: string }[]
  /** 픽스처 적용 계획(컴파일이 역할을 계획의 첫 신원으로 풀어 둔 것) */
  fixtures?: ({ key: string; kind: string } & Record<string, unknown>)[]
  env?: { env: string; for: string }[]
  little?: { sdt_s: number; peak_rate: number; concurrent: number; rows: { rate: number; need: number; ok: boolean; short: string[] }[]; first_short_rate: number | null; recommend_max: number | null }
  estimate?: { duration_s: number; sdt_s: number; peak_rate: number; concurrent: number; model: string }
  active_runs?: string[]
}

export interface StepVocab { group: string; actor: 'who' | 'from' | 'fromto' | 'seconds' | 'none'; kind: string | null; metrics: string[]; desc: string; supported: boolean; real?: boolean }
export interface ScenarioVocab {
  steps: Record<string, StepVocab>
  groups: { id: string; label: string }[]
  worker_steps: string[]
  /** 실단말(real-ue) 역할이 행위자가 될 수 있는 단계 */
  real_ue_steps?: string[]
  during_steps: string[]
  metrics: Record<string, string>
  pct_metrics: string[]
  ratio_metrics: Record<string, [string, string]>
  thresholds: string[]
  q850: Record<string, string>
  audio: string[]; video: string[]; rtp_modes?: string[]; sample_codecs?: string[]
  evidence_kinds: string[]; profile_models: string[]
  /** check.payload 종류 — conference_roster_visible|hidden · conference_warning_138 · dialog_consistent */
  check_kinds?: string[]
  pool_kinds: string[]; peer_profiles: string[]; transports: string[]; srtp: string[]; node_roles: string[]; target_kinds: string[]
  phases: Record<string, string>
}

export interface HistResult { id: string; timer: string; count: number; mean?: number | null; min?: number | null; max?: number | null; p50?: number | null; p95?: number | null; p99?: number | null; buckets: { ub: number | null; count: number }[] }
export interface EvidenceResult { kind: string; code?: string | null; min?: number | null; max?: number | null; observed: number | null; ok: boolean | null; why?: string | null }
export interface TargetSeries { id: string; agents: Record<string, { t: number[]; cpu_pct: (number | null)[]; mem_pct: (number | null)[] }>; procs?: Record<string, { t: number[]; cpu_pct: (number | null)[]; rss_mb: (number | null)[]; fds?: (number | null)[] }> }
export interface DiscoveredWorker { name: string; agent_id?: number; hostname?: string | null; ip?: string | null; port: number; ips?: string[]; cpus?: number | null; version?: string | null; live_state?: string | null; deployment_id?: number }
export interface SipDumpRow { call_id: string; bytes: number; messages: number }
export interface CallDump { id: string; call_id: string; events: RunEvent[]; dump: string | null; note?: string | null }
export interface TargetAlerts { id: string; alerts: Record<string, unknown>[]; window?: [string, string]; oam?: string; note?: string }

export interface RunFilters { scenario?: string; build?: string; verdict?: string; since?: string; profile?: string; label?: string; topology?: string; load?: boolean; limit?: number }

export interface RunRequest {
  scenario_id: string
  topology_id?: number
  topology?: string
  profile?: string
  bindings?: Record<string, number | string>
  instances?: number
  rate_saps?: number
  label?: string
}

const enc = encodeURIComponent

export const testerApi = {
  health: () => api.get<TesterHealth>('/tester/health'),
  validate: (kind: 'scenario' | 'profile' | 'topology' | 'run_request', body: { yaml?: string; doc?: unknown }) =>
    api.post<{ ok: boolean; errors: string[]; doc?: unknown }>('/tester/validate', { kind, ...body }),

  scenarios: () => api.get<{ scenarios: ScenarioRow[] }>('/tester/scenarios'),
  scenario: (id: string) => api.get<ScenarioDetail>(`/tester/scenarios/${enc(id)}`),
  saveScenario: (id: string, yaml: string) => api.put<ScenarioRow>(`/tester/scenarios/${enc(id)}`, { yaml }),
  deleteScenario: (id: string) => api.delete<{ deleted: boolean }>(`/tester/scenarios/${enc(id)}`),

  profiles: () => api.get<{ profiles: ProfileRow[] }>('/tester/profiles'),
  profile: (name: string) => api.get<ProfileDetail>(`/tester/profiles/${enc(name)}`),
  saveProfile: (name: string, yaml: string) => api.put<ProfileRow>(`/tester/profiles/${enc(name)}`, { yaml }),
  deleteProfile: (name: string) => api.delete<{ deleted: boolean }>(`/tester/profiles/${enc(name)}`),

  topologies: () => api.get<{ topologies: TopologyRow[] }>('/tester/topologies'),
  topology: (id: number) => api.get<TopologyRow>(`/tester/topologies/${id}`),
  createTopology: (doc: TopologyDoc) => api.post<TopologyRow>('/tester/topologies', doc),
  saveTopology: (id: number, doc: TopologyDoc) => api.put<TopologyRow>(`/tester/topologies/${id}`, doc),
  deleteTopology: (id: number) => api.delete<{ deleted: boolean }>(`/tester/topologies/${id}`),
  checkTopology: (id: number) => api.post<{ id: number; ok: boolean; items: CheckItem[] }>(`/tester/topologies/${id}/check`, {}),
  workers: (topologyId?: number) => api.get<{ workers: WorkerRow[] }>(`/tester/workers${topologyId ? `?topology=${topologyId}` : ''}`),

  runs: (limit = 200, f: RunFilters = {}) => {
    const q = new URLSearchParams({ limit: String(f.limit ?? limit) })
    for (const k of ['scenario', 'build', 'verdict', 'since', 'profile', 'label', 'topology'] as const) if (f[k]) q.set(k, String(f[k]))
    if (f.load) q.set('load', '1')
    return api.get<{ runs: RunRow[] }>(`/tester/runs?${q.toString()}`)
  },
  vocab: () => api.get<ScenarioVocab>('/tester/scenarios/vocab'),
  compileCheck: (body: PlanRequest) => api.post<PlanResult>('/tester/scenarios/compile-check', body),
  plan: (body: PlanRequest) => api.post<PlanResult>('/tester/runs/plan', body),
  holdRun: (id: string, hold: boolean) => api.post<{ id: string; hold: boolean }>(`/tester/runs/${enc(id)}/hold`, { hold }),
  hist: (id: string, timer: string) => api.get<HistResult>(`/tester/runs/${enc(id)}/hist?timer=${enc(timer)}`),
  discoveredWorkers: () => api.get<{ items: DiscoveredWorker[]; note?: string | null }>('/tester/workers/discovered'),
  targetSeries: (id: string) => api.get<TargetSeries>(`/tester/runs/${enc(id)}/target-series`),
  sipDumps: (id: string) => api.get<{ id: string; dumps: SipDumpRow[] }>(`/tester/runs/${enc(id)}/sip`),
  callDump: (id: string, callId: string) => api.get<CallDump>(`/tester/runs/${enc(id)}/sip/${enc(callId)}`),
  targetAlerts: (id: string) => api.get<TargetAlerts>(`/tester/runs/${enc(id)}/target-alerts`),
  compareText: async (ids: string[], format: 'md' | 'csv') => {
    // 텍스트 응답 — JSON 클라이언트를 거치지 않고 같은 자격으로 직접 받는다
    const res = await fetch(`/api/v1/tester/runs/compare?ids=${ids.map(enc).join(',')}&format=${format}`, { headers: authHeaders() })
    if (!res.ok) throw new Error(`HTTP ${res.status}`)
    return res.text()
  },
  run: (id: string) => api.get<RunRow>(`/tester/runs/${enc(id)}`),
  startRun: (body: RunRequest) => api.post<{ id: string; state: string }>('/tester/runs', body),
  stopRun: (id: string) => api.post<{ id: string; state: string }>(`/tester/runs/${enc(id)}/stop`, {}),
  rateRun: (id: string, rate_saps: number) => api.post<{ id: string; rate_saps: number }>(`/tester/runs/${enc(id)}/rate`, { rate_saps }),
  report: (id: string) => api.get<{ run: RunDoc | RunLive; markdown: string | null; final: boolean }>(`/tester/runs/${enc(id)}/report`),
  events: (id: string, limit = 300) => api.get<{ events: RunEvent[] }>(`/tester/runs/${enc(id)}/events?limit=${limit}`),
  series: (id: string) => api.get<RunSeries>(`/tester/runs/${enc(id)}/series`),
  deleteRun: (id: string) => api.delete<{ deleted: boolean }>(`/tester/runs/${enc(id)}`),
  compare: (ids: string[]) => api.get<CompareResult>(`/tester/runs/compare?ids=${ids.map(enc).join(',')}`),
}

// ── 라이브 스트림 — fetch + ReadableStream (widgets/useAlarms.ts 와 같은 방식) ─────────────
// EventSource 는 Authorization 헤더를 못 붙이므로 쓰지 않는다. 게이트웨이는 Accept: text/event-stream
// 요청을 총 타임아웃 없이 업스트림 SSE 로 청크 통과시킨다.
export type StreamName = 'hello' | 'runs' | 'workers' | 'agg' | 'events'

export interface TesterStreamFrame {
  stream: StreamName
  record: Record<string, unknown>
}

export interface AggFrame {
  run_id: string
  t: number
  worker: string
  counters: Record<string, number>
  gauges: Record<string, number>
  timers: Record<string, { count: number; sum: number; max: number | null }>
}

export interface RunStateFrame {
  run_id: string
  hold?: boolean
  step_rate?: number
  plan?: RunPlan
  state?: RunState
  verdict?: Verdict
  rate_saps?: number
  scenario_id?: string
  profile?: string | null
  summary?: Summary
  kind?: 'log' | 'hello'
  worker?: string
  msg?: string
  workers?: string[]
}

export function openTesterStream(
  onFrame: (f: TesterStreamFrame) => void,
  onState: (s: 'connecting' | 'open' | 'closed', detail?: string) => void,
  runId?: string,
): () => void {
  const ctrl = new AbortController()
  let stopped = false
  const url = runId ? `/api/v1/tester/runs/${enc(runId)}/stream` : '/api/v1/tester/events'
  ;(async () => {
    while (!stopped) {
      const token = (() => { try { return localStorage.getItem('cims_token') } catch { return null } })()
      if (!token) { onState('closed', '로그인 필요'); return }
      onState('connecting')
      try {
        const res = await fetch(url, {
          headers: { Authorization: `Bearer ${token}`, Accept: 'text/event-stream' },
          signal: ctrl.signal,
        })
        if (!res.ok || !res.body) { onState('closed', `HTTP ${res.status}`); }
        else {
          onState('open')
          const reader = res.body.getReader()
          const dec = new TextDecoder()
          let buf = ''
          for (;;) {
            const { value, done } = await reader.read()
            if (done) break
            buf += dec.decode(value, { stream: true })
            let idx
            while ((idx = buf.indexOf('\n\n')) >= 0) {
              const chunk = buf.slice(0, idx); buf = buf.slice(idx + 2)
              for (const line of chunk.split('\n')) {
                if (!line.startsWith('data:')) continue
                try { onFrame(JSON.parse(line.slice(5).trim()) as TesterStreamFrame) } catch { /* 무시 */ }
              }
            }
          }
          onState('closed', '서버 종료')
        }
      } catch (e) {
        if (stopped) return
        onState('closed', String(e))
      }
      // 재연결 backoff
      await new Promise(r => setTimeout(r, 3000))
    }
  })()
  return () => { stopped = true; ctrl.abort(); onState('closed', '해제') }
}
