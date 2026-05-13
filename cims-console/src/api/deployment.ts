import { api } from './client'
import { ApiError } from './client'

const BASE = '/api/v1'

export interface UploadProgress {
  pct: number
  loaded: number
  total: number
}

// 업로드 핸들 — XHR 참조 + abort 가능
export interface UploadHandle<T> {
  promise: Promise<T>
  abort: () => void
}

// 업로드 URL — 기본은 상대 경로(Vite 프록시 또는 동일 오리진).
// VITE_CSC_DIRECT=1 환경변수로 dev 모드에서만 4420 직접 전송 전환 가능 (인증서 신뢰 필요).
// CORS + 자체서명 인증서 이중신뢰 이슈를 피하려면 상대 경로 유지 권장.
function buildUploadUrl(path: string): string {
  const env = (import.meta as unknown as { env: Record<string, string> }).env || {}
  if (env.VITE_CSC_DIRECT === '1' && env.PROD !== 'true') {
    const loc = window.location
    const port = env.VITE_CSC_PORT || '4420'
    return `${loc.protocol}//${loc.hostname}:${port}${BASE}${path}`
  }
  return `${BASE}${path}`
}

// raw 바이너리 업로드 (multipart 오버헤드 제거) — Content-Type: application/octet-stream
// 파일 자체를 body 에, 메타는 query string 에
function uploadMultipart<T>(path: string, file: File, force: boolean,
                             onProgress?: (p: UploadProgress) => void): UploadHandle<T> {
  const xhr = new XMLHttpRequest()
  let aborted = false

  const promise = new Promise<T>((resolve, reject) => {
    const url = buildUploadUrl(path) + `?force=${force ? 'true' : 'false'}`
    xhr.open('POST', url)
    const token = localStorage.getItem('cims_token')
    if (token) xhr.setRequestHeader('Authorization', `Bearer ${token}`)
    xhr.setRequestHeader('Content-Type', 'application/octet-stream')
    // 파일명 힌트 (서버 측 로그용)
    xhr.setRequestHeader('X-Filename', encodeURIComponent(file.name))

    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable && onProgress) {
        onProgress({
          pct: Math.round((e.loaded / e.total) * 100),
          loaded: e.loaded, total: e.total,
        })
      }
    }
    xhr.onload = () => {
      let data: Record<string, unknown> = {}
      try { data = JSON.parse(xhr.responseText || '{}') } catch { /* ignore */ }
      if (xhr.status >= 200 && xhr.status < 300) {
        resolve(data as T)
      } else {
        if (xhr.status === 401) {
          localStorage.removeItem('cims_token')
          window.location.reload()
        }
        const msg = (typeof data.error === 'string' && data.error) || `HTTP ${xhr.status}`
        reject(new ApiError(msg, xhr.status, data))
      }
    }
    xhr.onabort = () => reject(new ApiError('aborted', 0, { error: 'aborted' }))
    xhr.onerror = () => reject(new ApiError('network error', 0, {}))
    xhr.ontimeout = () => reject(new ApiError('timeout', 0, {}))

    // File 객체를 그대로 body 로 전송 — 브라우저가 streaming 으로 처리
    xhr.send(file)
  })

  return {
    promise,
    abort: () => { if (!aborted) { aborted = true; try { xhr.abort() } catch { /* no-op */ } } },
  }
}

// ──────────────── Agent ────────────────
export interface AgentHaGroupRef {
  id: number
  name: string
  mode: 'active_standby' | 'all_active'
  role: 'master' | 'backup'
}

// HaServicesPage 용 — agent heartbeat 보고 인터페이스
export interface NetIface {
  name: string                                  // eth0 / eth1 / lo
  ip: string
  mask: number
  hint?: string
}

// HaServicesPage 용 — 운영자 설정 iface→slot 매핑
export interface ServiceIpRow {
  iface: string
  ip: string
  mask: number
  slot: string                                  // 용도 (자유 입력 / 패키지 slot)
  status?: 'up' | 'down' | 'unknown'
}

export interface Agent {
  id: number
  name: string
  status: 'pending' | 'approved' | 'online' | 'offline' | 'error' | 'revoked'
  hostname: string | null
  ip_address: string | null
  os_info: string | null
  cpu_cores: number | null
  memory_mb: number | null
  disk_gb: number | null
  agent_version: string | null
  last_heartbeat: string | null
  last_metric: string | null
  enrolled_at: string | null
  approved_at: string | null
  note: string | null
  create_time: string | null
  has_pending_enrollment: boolean
  ha_group: AgentHaGroupRef | null
  interfaces: NetIface[] | null
  service_ip_rows: ServiceIpRow[] | null
}

export interface AgentCreateResult extends Agent {
  enrollment_token: string
  install_command: string
}

export interface AgentMetric {
  ts: string
  cpu_pct: number | null
  mem_pct: number | null
  disk_pct: number | null
  load_avg: string | null
  processes: Array<{ name: string; pid: number; cmdline?: string }>
}

// ──────────────── Package ────────────────
export interface PackageMeta {
  name?: string
  version?: string
  description?: string
  build_date?: string
  git_sha?: string
  git_branch?: string
  ha_capability?: 'active_standby' | 'all_active' | 'standalone'
  service?: {
    functions?: Array<{ name: string; desc?: string }>
    processes?: string[]
  }
  [key: string]: unknown
}

export interface ConfigTemplateField {
  key: string
  label: string
  // v3 추가: string_list (tags), ref, ref_list, object_list
  type: 'string' | 'int' | 'bool' | 'enum' | 'path' | 'password'
        | 'string_list' | 'ref' | 'ref_list' | 'object_list'
  default?: unknown
  help?: string
  required?: boolean
  restart?: boolean
  advanced?: boolean
  options?: string[]        // enum
  min?: number              // int
  max?: number              // int
  reload_hint?: string
  readonly?: boolean        // collection schema 전용
  auto?: string             // collection schema 전용 (e.g. "uuid")
  // ref / ref_list 용 — 다른 collection 의 어떤 필드를 참조할지
  ref_collection?: string
  // object_list 용 — 중첩 객체의 필드 스키마
  item_schema?: { fields: ConfigTemplateField[] }
  // v2 _infra: 고급 설정 토글로만 노출
  hidden?: boolean
  // v2 _infra: section.groups[].key 와 매칭되는 sub-group 분류
  group?: string
  // deploy-time 치환값 (@VAR@ 포함). UI 는 무시.
  deploy_value?: unknown
  // HaServicesPage Phase 2 — IP slot 메타. 운영자가 어떤 IP 인지 식별.
  ip_scope?: 'service' | 'vip'   // 서버 단위 IP / 그룹 단위 VIP
  ip_slot?: string                // 'SIP' / 'Admin' / 'RTP' 등 (자유 명명, 정렬 키)
  ip_port?: number                // 참고용 포트
  ip_proto?: 'tcp' | 'udp'
}

export interface ConfigTemplateSection {
  key: string
  title: string
  description?: string
  fields: ConfigTemplateField[]
  // v2: 섹션 전체를 고급 설정 토글로만 노출 (_infra 등)
  hidden?: boolean
  // v2: 섹션 내부 sub-header 정의. field.group 으로 필드 정렬.
  groups?: { key: string; title: string; description?: string }[]
}

export interface CollectionSchema {
  primary_key?: string[]
  id_field?: string
  id_type?: 'uuid' | 'int' | 'string'
  unique_keys?: string[][]
  fields: ConfigTemplateField[]
}

export interface ConfigTemplateCollection {
  key: string
  title: string
  description?: string
  restart?: boolean
  reload_hint?: string
  schema: CollectionSchema
  storage?: { kind: string; file?: string }
}

export interface ConfigTemplatePreset {
  // 사용자에게 노출되는 키 (kebab-case 권장: "single-node", "ha-active-standby")
  name: string
  // UI 표시명 (한국어 가능)
  label: string
  description?: string
  // 적용할 키→값 매핑. ConfigTemplateField.key 와 동일 형식.
  values: Record<string, string | number | boolean | null>
}

export interface ConfigTemplate {
  version: number
  sections: ConfigTemplateSection[]
  collections?: ConfigTemplateCollection[]
  presets?: ConfigTemplatePreset[]
}

export interface SipPackage {
  id: number
  name: string
  version: string
  file_path: string
  file_size: number
  sha256: string
  description: string | null
  uploaded_by: string | null
  uploaded_at: string | null
  meta?: PackageMeta | null
  config_template?: ConfigTemplate | null
}

export interface PackageCreateInput {
  name?: string              // meta.json 없을 때만 필수
  version?: string           // meta.json 없을 때만 필수
  description?: string
  changelog?: string
  file_base64?: string
  file_path?: string
  force?: boolean            // 동일 (name, version) 덮어쓰기
}

// ──────────────── Deployment ────────────────
export interface Deployment {
  id: number
  agent_id: number
  agent_name: string | null
  package_id: number
  package_name: string | null
  package_version: string | null
  instance_id: number | null
  instance_name: string | null
  process_name: string | null
  service_functions: string[]     // machine names
  status: 'pending' | 'deploying' | 'running' | 'stopped' | 'failed' | 'removed'
  install_path: string | null
  deployed_at: string | null
  last_job_id: number | null
  note: string | null
  config: Record<string, unknown> | null
  config_applied_at: string | null
  create_time: string | null
}

export interface DeploymentCreateInput {
  agent_id: number
  package_id: number
  instance_id?: number | null
  process_name?: string
  service_functions?: string[]
  install_path?: string
  note?: string
}

export interface DeploymentConfigView {
  config: Record<string, unknown>
  config_applied_at: string | null
  template: ConfigTemplate | null
  meta: PackageMeta | null
}

export type JobType =
  | 'install' | 'upgrade' | 'uninstall'
  | 'start' | 'stop' | 'restart'
  | 'update_config' | 'collect_log' | 'health_check'

export const deploymentApi = {
  // agents
  listAgents:    () => api.get<{ items: Agent[] }>('/agents').then(r => r.items),
  getAgent:      (id: number) => api.get<Agent>(`/agents/${id}`),
  createAgent:   (name: string, note?: string) =>
    api.post<AgentCreateResult>('/agents', { name, note }),
  updateAgent:   (id: number, body: { name?: string; note?: string; service_ip_rows?: ServiceIpRow[] | null }) =>
    api.put<Agent>(`/agents/${id}`, body),
  deleteAgent:   (id: number) => api.delete<null>(`/agents/${id}`),
  approveAgent:  (id: number) => api.post<{ ok: boolean }>(`/agents/${id}/approve`, {}),
  revokeAgent:   (id: number) => api.post<{ ok: boolean }>(`/agents/${id}/revoke`, {}),
  upgradeAgent:  (id: number) => api.post<{ ok: boolean; job_id: number }>(`/agents/${id}/upgrade`, {}),
  applyIpConfig: (id: number) =>
    api.post<{ agent_id: number; job_id: number; rows: number }>(`/agents/${id}/apply-ip-config`, {}),
  agentMetrics:  (id: number) => api.get<{ items: AgentMetric[] }>(`/agents/${id}/metrics`),

  // packages
  listPackages:  () => api.get<{ items: SipPackage[] }>('/packages').then(r => r.items),
  getPackage:    (id: number) => api.get<SipPackage>(`/packages/${id}`),
  uploadPackage: (body: PackageCreateInput) => api.post<SipPackage>('/packages', body),
  uploadPackageFile: (file: File, force: boolean,
                      onProgress?: (p: UploadProgress) => void): UploadHandle<SipPackage> =>
    uploadMultipart<SipPackage>('/packages', file, force, onProgress),
  updatePackage: (id: number, body: { description?: string; config_template?: ConfigTemplate | null }) =>
    api.put<SipPackage>(`/packages/${id}`, body),
  deletePackage: (id: number) => api.delete<null>(`/packages/${id}`),

  // deployments
  listDeployments:  () => api.get<{ items: Deployment[] }>('/deployments').then(r => r.items),
  getDeployment:    (id: number) => api.get<Deployment>(`/deployments/${id}`),
  createDeployment: (body: DeploymentCreateInput) => api.post<Deployment>('/deployments', body),
  updateDeployment: (id: number, body: Partial<DeploymentCreateInput>) =>
    api.put<Deployment>(`/deployments/${id}`, body),
  deleteDeployment: (id: number) => api.delete<null>(`/deployments/${id}`),
  queueJob: (id: number, job_type: JobType, extra?: Record<string, unknown>) =>
    api.post<{ job_id: number; status: string }>(`/deployments/${id}/job`, { job_type, extra }),

  // deployment config (템플릿 기반)
  getDeploymentConfig: (id: number) =>
    api.get<DeploymentConfigView>(`/deployments/${id}/config`),
  putDeploymentConfig: (id: number, values: Record<string, unknown>, queue_update = true) =>
    api.put<{ ok: boolean; job_id: number | null }>(`/deployments/${id}/config`,
      { config: values, queue_update }),

  // deployment collections (jsonl-on-target via agent sync REST)
  getDeploymentCollection: (id: number, name: string) =>
    api.get<{ records: Record<string, unknown>[]; schema: CollectionSchema }>(
      `/deployments/${id}/collection/${name}`
    ),
  putDeploymentCollection: (id: number, name: string,
                            records: Record<string, unknown>[], signal = true) =>
    api.put<{ ok: boolean; count: number; signaled: number[] }>(
      `/deployments/${id}/collection/${name}`,
      { records, signal }
    ),

  // Phase 1 로컬 모듈 overlay 설정
  getModuleConfig: (name: string) =>
    api.get<ModuleConfigView>(`/modules/${name}/config`),
  putModuleConfig: (name: string, values: Record<string, unknown>) =>
    api.put<ModuleConfigPutResult>(`/modules/${name}/config`, { values }),

  // Phase 1 로컬 모듈 collection (jsonl) — DeploymentCollection 과 response 스키마 호환
  getModuleCollection: (name: string, collKey: string) =>
    api.get<{ records: Record<string, unknown>[]; schema: CollectionSchema }>(
      `/modules/${name}/collection/${collKey}`
    ),
  putModuleCollection: (name: string, collKey: string,
                        records: Record<string, unknown>[], signal = true) =>
    api.put<{ ok: boolean; count: number; signaled: number[] }>(
      `/modules/${name}/collection/${collKey}`,
      { records, signal }
    ),
}

// ──────────────── Module overlay (local Phase 1) ────────────────
export interface ModuleConfigView {
  module: string
  version: string
  template: ConfigTemplate
  current: Record<string, unknown>
  overlay_path: string
  owned_keys: string[]
}

export interface ModuleConfigPutResult {
  ok: boolean
  module: string
  applied: number
  removed: number
  current: Record<string, unknown>
  overlay_path: string
  restart_required: boolean
}
