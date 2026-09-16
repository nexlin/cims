// 계측기 API — base 게이트웨이 경유 /api/v1/tester/* (oam-cims-tester 가 서빙).
// 계약은 handlers/tester.py 의 TESTER_API_DOCS(개발자 모드 [API] 배지)와 같다.
import { api } from '@core/api/client'

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
  media?: { audio?: string; video?: string }
  payload?: string
  cause?: number
  expect?: Record<string, unknown>
}

export interface ScenarioDoc {
  id: string
  title?: string
  tags?: string[]
  roles?: Record<string, { pool: string; disjoint_from?: string; count?: number }>
  flow?: ScenarioStep[]
  target_evidence?: { kind: string; min?: number; max?: number; code?: string }[]
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

export interface TopologyDoc {
  name: string
  target: {
    name: string
    csp: { ip: string; udp?: number; tcp?: number; tls?: number; domain_volte?: string; domain_ptt?: string;
           peering?: { ip?: string; port: number; protocol?: string; local_node?: string } }
    csc?: { host: string; port?: number; tls?: boolean }
    oam?: { url: string; token_env?: string; csp_deployment_id?: number }
    observe?: string[]
  }
  workers?: { name: string; url: string; cpus?: number }[]
  pools: Record<string, { kind: 'ue' | 'peer' | 'real-ue'; profile?: string; [k: string]: unknown }>
}

export interface CheckItem { name: string; ok: boolean; detail: string; ms: number; info?: boolean }

export interface WorkerRow {
  name: string
  url: string
  cpus: number | null
  up: boolean
  error: string | null
  topology_id: number
  health: {
    version?: string; max_endpoints?: number; max_saps?: number; cpu_pct?: number; active_endpoints?: number
    active_run?: string | null; clock_skew_ms?: number
    pools?: { pool: string; kind: string; endpoints: number; registered?: number }[]
  } | null
}

export type Verdict = 'running' | 'pass' | 'fail' | 'aborted' | 'error'
export type RunState = 'starting' | 'provisioning' | 'running' | 'stopping' | 'stopped' | string

export type Summary = Record<string, number | string | null | undefined>

export interface HistSummary {
  count: number
  mean: number | null
  min: number | null
  max: number | null
  p50: number | null
  p95: number | null
  p99: number | null
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
  live?: RunLive
}

// run.json — 색인 + 본체 (GET /runs/<id>/report .run)
export interface RunDoc extends RunRow {
  label?: string | null
  bindings?: Record<string, unknown>
  plan?: { roles: Record<string, string>; identities: number; max_instances: number | null; rate_total: number } | null
  profile_doc?: Record<string, unknown> | null
  counters?: Record<string, number>
  timers?: Record<string, HistSummary>
  events?: number
  expect_results?: ExpectResult[]
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
    api.post<{ ok: boolean; errors: string[] }>('/tester/validate', { kind, ...body }),

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

  runs: (limit = 200) => api.get<{ runs: RunRow[] }>(`/tester/runs?limit=${limit}`),
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
