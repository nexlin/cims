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
  service_kind: string | null
  status: 'pending' | 'deploying' | 'running' | 'stopped' | 'failed' | 'removed'
  install_path: string | null
  deployed_at: string | null
  last_job_id: number | null
  note: string | null
  create_time: string | null
}

export interface DeploymentCreateInput {
  agent_id: number
  package_id: number
  instance_id?: number | null
  service_kind?: string
  install_path?: string
  note?: string
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
  updateAgent:   (id: number, body: { name?: string; note?: string }) =>
    api.put<Agent>(`/agents/${id}`, body),
  deleteAgent:   (id: number) => api.delete<null>(`/agents/${id}`),
  approveAgent:  (id: number) => api.post<{ ok: boolean }>(`/agents/${id}/approve`, {}),
  revokeAgent:   (id: number) => api.post<{ ok: boolean }>(`/agents/${id}/revoke`, {}),
  agentMetrics:  (id: number) => api.get<{ items: AgentMetric[] }>(`/agents/${id}/metrics`),

  // packages
  listPackages:  () => api.get<{ items: SipPackage[] }>('/packages').then(r => r.items),
  getPackage:    (id: number) => api.get<SipPackage>(`/packages/${id}`),
  uploadPackage: (body: PackageCreateInput) => api.post<SipPackage>('/packages', body),
  uploadPackageFile: (file: File, force: boolean,
                      onProgress?: (p: UploadProgress) => void): UploadHandle<SipPackage> =>
    uploadMultipart<SipPackage>('/packages', file, force, onProgress),
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
}
