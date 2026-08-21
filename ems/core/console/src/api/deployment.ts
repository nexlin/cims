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
// VITE_CSC_DIRECT=1 환경변수로 dev 모드에서만 4421 직접 전송 전환 가능 (인증서 신뢰 필요).
// CORS + 자체서명 인증서 이중신뢰 이슈를 피하려면 상대 경로 유지 권장.
function buildUploadUrl(path: string): string {
  const env = (import.meta as unknown as { env: Record<string, string> }).env || {}
  if (env.VITE_CSC_DIRECT === '1' && env.PROD !== 'true') {
    const loc = window.location
    const port = env.VITE_CSC_PORT || '4421'
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
  name: string
  ip: string
  mask: number
  mgmt?: boolean                                // CSC ↔ agent 통신 NIC — 변경 차단 (자기 단절 방지)
  managed?: boolean                             // cims-priv ip-add 로 부여 (label '<iface>:cims') — UI 에서 삭제 허용 대상
  hint?: string
  role?: 'mgmt' | 'service' | 'internal' | ''   // Phase 4d2 — IP 별 용도(망 분류). mgmt 자동, 나머지 admin 명시.
}

// HaServicesPage 용 — 운영자 설정 (iface, ip) pair 단위. 한 iface 에 여러 row 가능.
export interface ServiceIpRow {
  iface: string
  ip: string
  mask: number
  slot: string                                  // 용도 (자유 입력 / 패키지 slot)
  status?: 'up' | 'down' | 'unknown'
}

// specific route — agent heartbeat 보고 + 운영자 desired 동기.
// kernel_auto / is_default 는 UI 에서 readonly 표시. managed 만 [삭제] 허용.
export interface AgentRoute {
  dst: string                                   // CIDR (e.g. 192.168.100.0/24) 또는 'default'
  via: string                                   // gateway IPv4 (default route 가 아니면 비어있을 수도)
  dev: string                                   // iface name
  managed?: boolean                             // cims-priv route-add 로 부여 — [삭제] 허용
  is_default?: boolean                          // dst='default' 또는 '0.0.0.0/0' — readonly
  kernel_auto?: boolean                         // protocol=kernel (subnet 자동) — readonly
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
  agent_versions?: string[]      // 설치된 버전(롤백 대상; current 제외 mtime 최신순)
  last_heartbeat: string | null
  last_metric: string | null
  enrolled_at: string | null
  approved_at: string | null
  note: string | null
  create_time: string | null
  has_pending_enrollment: boolean
  enrollment_token_expires_at: string | null
  ha_group: AgentHaGroupRef | null
  interfaces: NetIface[] | null
  service_ip_rows: ServiceIpRow[] | null
  routes: AgentRoute[] | null
  mounts?: AgentMount[] | null
  // **실제** 마운트 목록 (cims-managed 아닌 기존 마운트 포함) — 공유 store 마운트 지점
  // 선택·검증용. mount guard 는 /proc/mounts 와 정확히 일치하는 경로만 통과시킨다.
  mount_targets?: Array<{ target: string; fstype: string; source: string }> | null
  /** 이 agent 가 실제로 보고하는 OAM 주소 (heartbeat 보고값). VIP 와 다르면 절체 후 단절된다. */
  oam_url?: string | null
  /** HA 판정 요약 {svc: {...}} — latched=true 면 그 노드는 승격 불가(운영자 해제 필요). */
  ha_state?: Record<string, {
    role?: string; state?: string; eligible?: boolean
    reasons?: string[]; latched?: boolean
  }> | null
  net_tuning?: AgentNetTuning | null
}

// 서버별 네트워크 튜닝 desired-state. sysctl=/etc/sysctl.d 영속, rps=sysfs+부팅 재적용.
export interface AgentNetTuning {
  sysctl: Record<string, number>          // net.core.* allowlist
  rps: Array<{ iface: string; cpus: string }>   // cpus = 16진 비트마스크 ("ff"), "0"=off
}

// cims-managed 마운트 — fstab 영속(재부팅 시 OS 자동 마운트). agent heartbeat 보고(mounted 상태 포함).
export interface AgentMount {
  source: string                 // 예: 121.161.164.105:/home/cbm/NAS/cims
  target: string                 // 예: /mnt/cims
  fstype: string                 // nfs | nfs4 | cifs | ext4 | ...
  options?: string               // 예: defaults,_netdev,nofail
  mounted?: boolean              // 현재 마운트 여부 (heartbeat 보고)
}

export interface AgentCreateResult extends Agent {
  enrollment_token: string
  enrollment_token_expires_at: string
  enrollment_token_ttl_sec?: number
  install_command: string
}

export interface NetIfaceMetric {
  name: string
  rx_bytes: number; tx_bytes: number
  rx_errors: number; tx_errors: number
  rx_rate?: number; tx_rate?: number     // Bps (직전 sample delta)
}

export interface MountMetric {
  mount: string
  device?: string
  total: number; used: number
  pct: number
}

export interface AgentMetric {
  ts: string
  cpu_pct: number | null
  mem_pct: number | null
  disk_pct: number | null
  load_avg: string | null
  processes: Array<{ name: string; pid: number; cmdline?: string }>
  per_iface?: NetIfaceMetric[]
  mounts?: MountMetric[]
}

export interface AgentHealthCheck {
  ts: string
  agent_version: string
  hostname: string
  verdict: 'healthy' | 'partial' | 'broken'
  issues: string[]
  ha?: {
    keepalived_installed: boolean
    keepalived_active: boolean
    vips: Array<{ iface: string; ip: string; mask: number }>
    journal_tail?: string[]
    error?: string
  }
  modules?: Array<{
    name: string
    running: boolean
    pid?: number
    cpu_pct?: number | null
    mem_mb?: number | null
    uptime_sec?: number
    error?: string
  }>
  metrics?: {
    mem_pct?: number | null
    disk_pct?: number | null
    load_avg?: string | null
    per_iface?: Array<{ name: string; rx_bytes: number; tx_bytes: number;
                       rx_rate?: number; tx_rate?: number;
                       rx_errors: number; tx_errors: number }>
  }
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
  // 필드 레벨 scope 오버라이드 — 섹션 안에 공통값·노드별 값이 섞인 경우
  // (예: csp media_server.LocalIp). 유효 scope = field.scope ?? section.scope.
  scope?: ConfigScope
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
  // v3: HA 그룹 멤버 간 정합 분류. "service" (공통) / "system" (멤버별). default "service".
  scope?: ConfigScope
}

export interface CollectionSchema {
  primary_key?: string[]
  id_field?: string
  id_type?: 'uuid' | 'int' | 'string'
  unique_keys?: string[][]
  fields: ConfigTemplateField[]
}

/**
 * scope — config 항목이 HA 그룹 멤버 간에 어떻게 분배되는지 표시.
 *
 *  "service" — 그룹 공통이어야 하는 값 (타이머·정책·공유 DB·목적지 VIP·포트 번호 등).
 *              [HA 공통 설정] 탭에 배치되고, 그룹 [설정 비교]의 명시적 [동기화]가
 *              복사하는 대상. 불일치 = 드리프트 경고.
 *
 *  "system"  — 서버(노드)별 고유값 (bind IP·자기 광고 주소·SystemId 등).
 *              [서버 개별 설정] 탭에 배치되고, 동기화로 절대 복사되지 않는다.
 *
 *  undefined — 기본값 "service" (보수적 — 공통 가정. 명시 권장).
 *
 * 저장(PUT config/collection)은 항상 단일 서버 대상 — scope 는 저장 시 전파 여부가
 * 아니라 "탭 배치 + 동기화 복사 마스크 + 드리프트 판정" 을 결정한다. 섹션/컬렉션
 * 단위가 기본이며, 필드에 scope 를 주면 섹션 값을 오버라이드한다 (effectiveScope).
 */
export type ConfigScope = 'system' | 'service'

// 필드 유효 scope — field.scope ?? section.scope, 기본 service.
// 백엔드 handlers.agents._effective_scope 와 동일 규칙이어야 한다.
export function effectiveScope(f: ConfigTemplateField, sectionScope?: ConfigScope): ConfigScope {
  return f.scope ?? sectionScope ?? 'service'
}

export interface ConfigTemplateCollection {
  key: string
  title: string
  description?: string
  restart?: boolean
  reload_hint?: string
  schema: CollectionSchema
  storage?: { kind: string; file?: string }
  scope?: ConfigScope
}

export interface ConfigTemplatePreset {
  // 사용자에게 노출되는 키 (kebab-case 권장: "single-node", "ha-active-standby")
  name: string
  // UI 표시명 (한국어 가능)
  label: string
  description?: string
  // 적용할 키→값 매핑. ConfigTemplateField.key 와 동일 형식.
  values: Record<string, string | number | boolean | null>
  scope?: ConfigScope
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
  // 생성 응답에만 실린다 — 성공했지만 전제 미충족으로 이중화되지 않는 경우의 사유.
  warning?: string
  warning_code?: string
  id: number
  agent_id: number
  agent_name: string | null
  package_id: number
  package_name: string | null
  package_version: string | null
  process_name: string | null
  service_functions: string[]     // machine names
  status: 'pending' | 'deploying' | 'running' | 'stopped' | 'failed' | 'removed'
  // 실측 프로세스 상태 — agent metric(live_modules) 대조. status(의도)와 어긋나면
  // 콘솔이 배지로 노출 (예: running 인데 down = 프로세스 죽음). null = 판정 불가.
  live_state?: 'up' | 'down' | null
  install_path: string | null
  prev_install_path?: string | null
  prev_package_version?: string | null
  install_history?: Array<{ version: string | null; install_path: string; at: string; job_id?: number }>
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
  process_name?: string
  service_functions?: string[]
  install_path?: string
  note?: string
}

// dep 이 소속된 HA 그룹이 이 패키지를 호스팅할 때만 채워짐 (standalone = null).
// 콘솔이 그룹 컨텍스트 표시(공통/개별 탭 안내·설정 비교 링크·버전 가드)에 사용.
export interface DeploymentConfigHa {
  group_id: number
  group_name: string
  mode: string
  members: { deployment_id: number; agent_id: number; agent_name: string | null;
             package_version: string | null }[]
}

export interface DeploymentConfigView {
  config: Record<string, unknown>
  config_applied_at: string | null
  template: ConfigTemplate | null
  meta: PackageMeta | null
  ha?: DeploymentConfigHa | null
}

export type JobType =
  | 'install' | 'upgrade' | 'uninstall'
  | 'start' | 'stop' | 'restart'
  | 'update_config' | 'collect_log' | 'health_check'

export interface AgentJob {
  id: number
  agent_id: number
  job_type: string
  params?: Record<string, unknown>
  status: 'queued' | 'running' | 'completed' | 'failed' | string
  result_code: number | null
  result_stdout: string | null
  result_stderr: string | null
  dispatched_at: string | null
  completed_at: string | null
  create_time: string
  update_time: string
}

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
  regenerateToken: (id: number) =>
    api.post<AgentCreateResult>(`/agents/${id}/regenerate-token`, {}),
  getInstallCommand: (id: number) =>
    api.get<{ install_command: string; enrollment_token_expires_at: string }>(`/agents/${id}/install-command`),
  // DEV 전용 — /release/package 빌드 산출물 (build/dist/packages/*.tar.gz) 일괄 file_store 등록.
  // 상용 환경 (Server.DevMode=false) 에서는 403.
  registerPackagesFromDist: () =>
    api.post<{
      count: number
      registered: Array<{ name: string; version: string; id: number }>
      errors: Array<{ file: string; error: string }>
      source_dir: string
    }>(`/packages/register-from-dist`, {}),
  upgradeAgent:  (id: number) => api.post<{ ok: boolean; job_id: number }>(`/agents/${id}/upgrade`, {}),
  // body 생략 시 직전 버전으로 롤백; version 지정 시 해당 버전(agent_versions 중)으로.
  rollbackAgent: (id: number, version?: string) =>
    api.post<{ ok: boolean; job_id: number; target_version: string | null }>(
      `/agents/${id}/rollback`, version ? { version } : {}),
  restartAgent:  (id: number) => api.post<{ ok: boolean; job_id: number }>(`/agents/${id}/restart`, {}),
  healthCheck:   (id: number, scope: 'ha'|'modules'|'all' = 'all') =>
    api.post<AgentHealthCheck>(`/agents/${id}/health-check`, { scope }),
  // Phase 4d2 — IP 별 NIC role 명시 (admin override).
  // body: {"<ip>": "<role>"} — role 은 mgmt/service/internal/'' (clear).
  // mgmt 는 agent 가 oam.json Mgmt.Cidr + detect_mgmt_ip 로 자동 도출 (admin 명시도 가능).
  getInterfaceRoles: (id: number) =>
    api.get<{
      agent_id: number
      overrides: Record<string, string>
      interfaces: Array<{ ip: string; name: string; role?: string; mgmt?: boolean }>
    }>(`/agents/${id}/interface-roles`),
  putInterfaceRoles: (id: number, body: Record<string, string>) =>
    api.put<{ ok: boolean; interface_role_overrides: Record<string, string> }>(`/agents/${id}/interface-roles`, body),
  applyIpConfig: (id: number,
                  ops?: { service_ip_rows?: Array<{ op: 'add'|'del'; iface: string; ip: string; mask: number; slot?: string }>;
                          routes?:          Array<{ op: 'add'|'del'; dst: string; via: string; dev: string }> }) =>
    api.post<{ agent_id: number; rows: number; routes: number;
               ok: boolean; rc: number;
               stdout: string; stderr: string }>(`/agents/${id}/apply-ip-config`, ops ?? {}),
  applyMounts:   (id: number,
                  mounts: Array<{ op: 'add'|'del'; fstype?: string; source?: string; target: string; options?: string }>) =>
    api.post<{ agent_id: number; mounts: number; ok: boolean; rc: number;
               stdout: string; stderr: string }>(`/agents/${id}/apply-mounts`, { mounts }),
  applyNetTuning: (id: number, tuning: { sysctl: Record<string, number>; rps: Array<{ iface: string; cpus: string }> }) =>
    api.post<{ agent_id: number; job_id: number; status: string; sysctl: number; rps: number }>(
      `/agents/${id}/apply-net-tuning`, tuning),
  getAgentJob:   (agentId: number, jobId: number) =>
    api.get<AgentJob>(`/agents/${agentId}/jobs/${jobId}`),
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
  // 전 agent 의 OAM 접속 주소 재지정 (이중화 전환: 노드 IP → VIP).
  // 각 agent 가 새 주소로 /health 도달 확인 후에만 적용한다 — VIP 가 없으면 아무 것도 안 바뀐다.
  retargetOamUrl: (url: string) =>
    api.post<{ url: string; jobs: { agent_id: number; job_id: number }[] }>(
      '/agents/oam-url', { url }),
  // 한 agent 만 재지정 — 이 주소는 그 노드 agent 의 설정이므로 서버 단위 편집이 기본이다.
  retargetAgentOamUrl: (agentId: number, url: string) =>
    api.post<{ url: string; jobs: { agent_id: number; job_id: number }[] }>(
      `/agents/${agentId}/oam-url`, { url }),
  // force: 안전 가드(관리평면 업그레이드 순서·단일 writer 동시 기동) 우회 — 운영자 확인 후에만.
  queueJob: (id: number, job_type: JobType, extra?: Record<string, unknown>, force?: boolean) =>
    api.post<{ job_id: number; status: string }>(`/deployments/${id}/job`,
      { job_type, extra, ...(force ? { force: true } : {}) }),
  // 모듈 버전 전환 + 설치·재기동을 **서버가 한 번에** 수행 (rollback 과 대칭).
  // package_id 생략 시 같은 모듈의 최근 업로드 패키지. 전환은 가드 통과 후에만 일어나므로
  // 콘솔이 실패를 되돌릴 필요가 없다 (되돌려도 컬렉션 스키마는 복구되지 않는다).
  upgradeDeployment: (id: number, package_id?: number, force?: boolean) =>
    api.post<{ ok: boolean; job_id: number; from_version: string | null
               to_version: string | null; package_id: number }>(
      `/deployments/${id}/upgrade`,
      { ...(package_id ? { package_id } : {}), ...(force ? { force: true } : {}) }),
  rollbackDeployment: (id: number, target?: { install_path?: string; version?: string }) =>
    api.post<{ ok: boolean; job_ids: number[]; restart_job_id: number;
               install_path: string; version: string | null }>(
      `/deployments/${id}/rollback`, target || {}),

  // deployment config (템플릿 기반) — 저장은 항상 이 서버에만 (전파 없음).
  //   구 백엔드(sync_keys 이전)와의 혼재 배포 대비 propagate_to_ha_peers=false 를
  //   항상 명시 — 옛 백엔드의 레거시 통짜 전파 경로를 차단.
  getDeploymentConfig: (id: number) =>
    api.get<DeploymentConfigView>(`/deployments/${id}/config`),
  // overlay 는 config_template 이 선언한 키만 저장한다(스키마가 계약) — 템플릿 밖 키는
  // 저장되지 않고 `pruned_keys` 로 돌아온다(조용히 버리지 않음).
  putDeploymentConfig: (id: number, values: Record<string, unknown>, queue_update = true) =>
    api.put<{ ok: boolean; job_id: number | null;
              members: Array<{ deployment_id: number; agent_id: number; job_id: number }>
              pruned_keys?: string[] }>(
      `/deployments/${id}/config`,
      { config: values, queue_update, propagate_to_ha_peers: false }),
  // 그룹 설정 동기화 — 명시적 방향성 복사 (source=id → targets). 같은 패키지·
  // 같은 버전만 허용(409 version_mismatch), keys 는 유효 scope=service 만 적용.
  syncDeploymentConfig: (id: number, body: {
    targets: number[]; keys?: string[]; collections?: string[]; queue_update?: boolean
  }) =>
    api.post<{
      ok: boolean; source_deployment_id: number; ha_group_id: number
      applied_keys: string[]; removed_keys: string[]; skipped_keys: string[]
      members: Array<{ deployment_id: number; agent_id: number; job_id: number }>
      collections: Array<{ name: string; ok: boolean; skipped?: string; count?: number
                           peers?: Array<{ deployment_id: number; agent_id: number
                                           status: number; ok: boolean; error?: unknown }> }>
      sync_id: number | null
    }>(`/deployments/${id}/sync`, body),

  // deployment collections (jsonl-on-target via agent sync REST).
  // PUT 은 이 서버에만 저장 — 그룹 정합은 syncDeploymentConfig(collections)로.
  // GET 은 멤버 hash 비교(drift_detected) 포함 — 비교 뷰/드리프트 배너용.
  getDeploymentCollection: (id: number, name: string) =>
    api.get<{
      records: Record<string, unknown>[];
      schema:  CollectionSchema;
      peers?:  Array<{ deployment_id: number; agent_id: number; status: number;
                       ok: boolean; count: number | null; hash: string;
                       error?: unknown }>;
      drift_detected?: boolean;
      ha_group_id?:   number | null;
      ha_group_mode?: string | null;
      scope?:         ConfigScope;
    }>(
      `/deployments/${id}/collection/${name}`
    ),
  putDeploymentCollection: (id: number, name: string,
                            records: Record<string, unknown>[], signal = true) =>
    api.put<{
      ok: boolean; count: number; signaled: number[];
      peers?: Array<{ deployment_id: number; agent_id: number; status: number;
                      ok: boolean; count: number | null; signaled: number[];
                      error?: unknown }>;
      scope?:      ConfigScope;
      propagated?: boolean;
    }>(
      `/deployments/${id}/collection/${name}`,
      { records, signal, propagate_to_ha_peers: false }
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
