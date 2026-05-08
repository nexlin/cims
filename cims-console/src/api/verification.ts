import { api } from './client'

// ─────────────────────────────────────────────────────────────
// 검증 (verify) API client — backend csc/src/handlers/verification.py
// ─────────────────────────────────────────────────────────────

export type VerifyStatus = 'PASS' | 'FAIL' | 'SKIP' | 'BLOCKED' | 'RUNNING' | 'PENDING' | 'UNKNOWN'

export interface VerifyItemMeta {
  id: string
  stage: number
  category: string
  name: string
  is_group: boolean
  parent: string | null
  depends_on: string[]
  presets: string[]
  side_effects: string[]
  timeout_s: number
  description: string
}

export interface VerifyStageMeta {
  stage: number
  title: string
  description: string
  timeout_s: number
  items: VerifyItemMeta[]
}

export interface VerifyPreset {
  name: string
  items: string[]
}

export interface VerifyStagesOverview {
  stages: VerifyStageMeta[]
  presets: VerifyPreset[]
}

export interface ProgressItem {
  id: string
  name: string
  stage: number
  status: VerifyStatus
  elapsed_ms: number
  idx: number
  children: { id: string; name: string; status: VerifyStatus; elapsed_ms: number }[]
}

export interface ProgressSummary {
  pass: number
  fail: number
  skip: number
  blocked: number
}

export interface StageGate {
  first_failed: number
  blocked_stages: Record<number, number>
}

export interface ItemsProgress {
  selected: string[]
  total: number
  completed: number
  current: string | null
  items: ProgressItem[]
  summary: ProgressSummary | null
  stage_gate: StageGate | null
}

export interface JobStatus {
  job_id: string
  stage: number
  label: string
  scope: string
  selected_ids: string[]
  argv: string[]
  started_at: number
  ended_at: number | null
  elapsed: number
  done: boolean
  returncode: number | null
  verdict: 'PASS' | 'FAIL' | 'UNKNOWN' | null
  report_path: string | null
  report_ts: string
  run_id: number | null
  stdout_tail: string
  items_progress: ItemsProgress
}

export interface RunOpts {
  async?: boolean
  items?: string[]
  preset?: string
  skip_build?: boolean
  skip_pkg?: boolean
  skip_reset?: boolean
  keep_agent?: boolean
  only_children?: Record<string, string[]>
  trigger?: 'user' | 'cli' | 'ci'
}

export interface RunHistoryItem {
  id: number
  started_at: string | null
  finished_at: string | null
  elapsed_ms: number
  trigger: 'user' | 'cli' | 'ci'
  scope: string
  verdict: 'PASS' | 'FAIL' | 'UNKNOWN'
  totals: { total?: number; pass?: number; fail?: number; skip?: number; blocked?: number }
  pkg_manifest_hash: string
  git_branch: string
  git_sha: string
  host: string
  report_path: string
  job_id: string
}

export interface RunDetailItem {
  id: string
  stage: number
  parent_id: string | null
  is_group: boolean
  name: string
  status: VerifyStatus
  elapsed_ms: number
  detail: string
  idx: number
}

export interface RunDetail extends RunHistoryItem {
  selected_ids: string[]
  ens_ip: string
  note: string
  items: RunDetailItem[]
}

export interface RunsListResponse {
  total: number
  limit: number
  offset: number
  runs: RunHistoryItem[]
}

export interface RunsStatsOverall {
  runs: number
  pass: number
  fail: number
  unknown: number
  success_rate: number
  avg_elapsed_ms: number
  median_elapsed_ms: number
  p95_elapsed_ms: number
}

export interface RunsStatsByScope {
  scope: string
  runs: number
  pass: number
  fail: number
  success_rate: number
  avg_elapsed_ms: number
}

export interface RunsTimelineItem {
  id: number
  started_at: string | null
  scope: string
  verdict: 'PASS' | 'FAIL' | 'UNKNOWN'
  elapsed_ms: number
  pass: number
  fail: number
  skip: number
  blocked: number
  total: number
}

export interface RunsStatsResponse {
  window:   { days: number; since_iso: string; limit: number }
  overall:  RunsStatsOverall
  by_scope: RunsStatsByScope[]
  timeline: RunsTimelineItem[]
}

const BASE = '/verification'

export const verifyApi = {
  // 6 stage 메타 + 항목 트리 + 프리셋
  getStages: () => api.get<VerifyStagesOverview>(`${BASE}/stages`),

  // stage 단독 실행
  runStage: (stage: number, opts: RunOpts = {}) =>
    api.post<{ job_id: string; stage: number; argv: string[]; started_at: number; message: string }>(
      `${BASE}/stages/${stage}`, { async: true, ...opts }
    ),

  // 임의 실행 (multi-stage 가능)
  runArbitrary: (opts: RunOpts) =>
    api.post<{ job_id: string; stage: number; argv: string[]; started_at: number; message: string }>(
      `${BASE}/run`, { async: true, ...opts }
    ),

  // job 폴링
  getJob: (jobId: string) => api.get<JobStatus>(`${BASE}/jobs/${jobId}`),

  // 회차 이력
  listRuns: (opts: { stage?: number; verdict?: string; scope?: string; limit?: number; offset?: number } = {}) => {
    const qs = new URLSearchParams()
    if (opts.stage)   qs.set('stage', String(opts.stage))
    if (opts.verdict) qs.set('verdict', opts.verdict)
    if (opts.scope)   qs.set('scope', opts.scope)
    if (opts.limit)   qs.set('limit', String(opts.limit))
    if (opts.offset)  qs.set('offset', String(opts.offset))
    const q = qs.toString()
    return api.get<RunsListResponse>(`${BASE}/runs${q ? '?' + q : ''}`)
  },
  getRun:    (id: number) => api.get<RunDetail>(`${BASE}/runs/${id}`),
  deleteRun: (id: number) => api.delete<{ id: number; deleted: boolean }>(`${BASE}/runs/${id}`),

  // 이력 통계 (이력 페이지 chart)
  getRunsStats: (opts: { days?: number; limit?: number } = {}) => {
    const qs = new URLSearchParams()
    if (opts.days)  qs.set('days', String(opts.days))
    if (opts.limit) qs.set('limit', String(opts.limit))
    const q = qs.toString()
    return api.get<RunsStatsResponse>(`${BASE}/runs/stats${q ? '?' + q : ''}`)
  },

  // 현재 검증 환경 메타 (V2 LIVE PrintReport meta 주입용)
  getEnv: () => api.get<VerifyEnvResponse>(`${BASE}/env`),

  // 진행 중·최근 종료된 LIVE 회차 (CLI + 백엔드 통합 시야)
  getActive: () => api.get<ActiveRunsResponse>(`${BASE}/active`),
}

export interface ActiveRunSummary {
  job_id: string
  source: 'cli' | 'backend'
  scope: string
  label: string
  selected_ids: string[]
  started_at: number
  ended_at: number | null
  done: boolean
  verdict: 'PASS' | 'FAIL' | 'UNKNOWN' | 'ABORTED' | null
  returncode: number | null
  run_id: number | null
  host: string
  pid: number | null
  progress: {
    total: number
    completed: number
    pass: number
    fail: number
    skip: number
    blocked: number
    current: string | null
  }
}

export interface ActiveRunsResponse {
  runs: ActiveRunSummary[]
}

export interface VerifyEnvResponse {
  host: string
  git_branch: string
  git_sha: string
  pkg_manifest_hash: string
}
