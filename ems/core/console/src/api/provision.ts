// 자동 배포 API — base OAM 내장 (/api/v1/provision).
//   경로의 'provision' 은 내부 이름이다. 화면 이름은 '자동 배포'.
// 설계 정본: docs/design/features/auto_deployment.md
import { api } from './client'

export interface ProvIssue {
  level: 'error' | 'warning'
  path: string                    // 'blueprint:systems[0].members[1].server' 형태
  message: string
}

export interface BlueprintSummary {
  id: number
  name: string
  description?: string
}

export interface InventorySummary {
  id: number
  name: string
  server_count: number
}

export interface InventoryServerView {
  name: string
  host: string
  ssh: { user?: string; port?: number; password?: string | null }
  sudo: { method?: string; password?: string | null }
  install_dir?: string
  svc_user?: string
  agent_preinstalled?: boolean
  // 'preinstalled' = agent 기설치 노드 → SSH 하지 않는다. 인증은 비밀번호만 지원.
  auth_mode: 'preinstalled' | 'password' | 'none'
}

export interface InventoryView {
  version: number
  servers: InventoryServerView[]
}

export interface ValidateResult {
  ok: boolean
  issues: ProvIssue[]
  blueprint: Record<string, unknown> | null
  inventory: InventoryView | null
}

export interface PreflightRow {
  server: string
  host: string
  auth_mode: string
  ok: boolean
  os?: string
  login_user?: string
  sudo_ok?: boolean
  error?: string
  error_code?: string
}

export interface PlanStep {
  target: string
  action?: string
  [k: string]: unknown
}

export interface PlanPhase {
  key: string
  title: string
  serial?: boolean
  error?: string
  steps: PlanStep[]
}

export type StepStatus = 'pending' | 'running' | 'done' | 'skipped' | 'failed' | 'aborted'

export interface RunStep {
  target: string
  status: StepStatus
  detail?: string
  error?: string
  error_code?: string
  elapsed_sec?: number
}

export interface RunPhase {
  key: string
  title: string
  status: string
  steps: RunStep[]
  error?: string
}

export interface Run {
  id: number
  status: 'pending' | 'running' | 'succeeded' | 'failed' | 'aborted'
    | 'rolled_back' | 'rollback_partial'
  blueprint: string
  actor?: string
  created_at?: string
  started_at?: string
  finished_at?: string
  error?: string
  live?: boolean
  phases: RunPhase[]
  log?: string[]
  created?: Array<{ kind: string; id: number; label: string }>
  rollback?: { undone: string[]; failed: string[] }
}

export interface RunSummary {
  id: number
  status: Run['status']
  blueprint: string
  created_at?: string
  progress: { total: number; done: number; failed: number }
}

const B = '/provision'

export const provisionApi = {
  // ── blueprints ──
  listBlueprints: () =>
    api.get<{ blueprints: BlueprintSummary[] }>(`${B}/blueprints`),
  uploadBlueprint: (raw: string) =>
    api.post<{ id: number; name: string; issues: ProvIssue[] }>(`${B}/blueprints`, { raw }),
  getBlueprint: (id: number) =>
    api.get<{ id: number; name: string; doc: Record<string, unknown>; raw: string }>(
      `${B}/blueprints/${id}`),
  // raw 우선 — 원문을 직접 고치면 주석이 유지된다. doc 은 구성 뷰 저장용(주석 소실).
  saveBlueprint: (id: number, body: { raw?: string; doc?: Record<string, unknown> }) =>
    api.put<{ id: number; issues: ProvIssue[] }>(`${B}/blueprints/${id}`, body),
  deleteBlueprint: (id: number) =>
    api.delete<{ deleted: boolean }>(`${B}/blueprints/${id}`),

  // ── inventories ──
  listInventories: () =>
    api.get<{ inventories: InventorySummary[] }>(`${B}/inventories`),
  uploadInventory: (raw: string, name?: string) =>
    api.post<{ id: number; name: string; inventory: InventoryView; issues: ProvIssue[] }>(
      `${B}/inventories`, { raw, name }),
  getInventory: (id: number) =>
    api.get<{ id: number; name: string; inventory: InventoryView }>(`${B}/inventories/${id}`),
  // 비밀 필드를 비워 보내면 저장값을 유지한다 (마스킹 왕복 덮어쓰기 방지).
  saveInventory: (id: number, body: { raw?: string; doc?: InventoryView }) =>
    api.put<{ id: number; inventory: InventoryView; issues: ProvIssue[] }>(
      `${B}/inventories/${id}`, body),
  deleteInventory: (id: number) =>
    api.delete<{ deleted: boolean }>(`${B}/inventories/${id}`),
  preflight: (id: number) =>
    api.post<{ ok: boolean; results: PreflightRow[] }>(`${B}/inventories/${id}/preflight`, {}),

  // ── validate / runs ──
  validate: (blueprint_id: number, inventory_id: number) =>
    api.post<ValidateResult>(`${B}/blueprints/validate`, { blueprint_id, inventory_id }),
  dryRun: (blueprint_id: number, inventory_id: number) =>
    api.post<{ dry_run: true; blueprint: string; phases: PlanPhase[] }>(
      `${B}/runs?dry_run=true`, { blueprint_id, inventory_id }),
  startRun: (blueprint_id: number, inventory_id: number, on_error: 'stop' | 'continue' = 'stop') =>
    api.post<{ run_id: number; status: string }>(`${B}/runs`,
      { blueprint_id, inventory_id, on_error }),
  listRuns: () => api.get<{ runs: RunSummary[] }>(`${B}/runs`),
  getRun: (id: number) => api.get<Run>(`${B}/runs/${id}`),
  resumeRun: (id: number) => api.post<{ run_id: number }>(`${B}/runs/${id}/resume`, {}),
  abortRun: (id: number) => api.post<{ run_id: number }>(`${B}/runs/${id}/abort`, {}),
  rollbackRun: (id: number) =>
    api.post<{ run_id: number; status: string; undone: string[]; failed: string[] }>(
      `${B}/runs/${id}/rollback`, {}),
}

// blueprint/raw 는 텍스트라 api.get(JSON) 을 못 쓴다 — 다운로드 링크로 연다.
export function blueprintRawUrl(id: number): string {
  return `/api/v1${B}/blueprints/${id}/raw`
}
