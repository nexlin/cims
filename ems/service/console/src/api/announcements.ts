// 서비스 안내음성 라이브러리 API — base OAM /api/v1/announcements (announcements.md §7.3).
//   계측기 샘플 라이브러리(@tester/api/tester samples)와 형식만 같고 별개 자원이다.
import { api, authHeaders } from '@core/api/client'

const enc = encodeURIComponent

export type AnnKind = 'tone' | 'announcement' | 'music'
export type AnnCodec = 'pcmu' | 'pcma' | 'g722' | 'amr-wb'
export type NodePresence = 'ok' | 'partial' | 'missing' | 'unreachable'

export interface AnnRow {
  id: string                 // sys:<name> | op:<name> | sub:<가입 번호 숫자열>
  name: string
  source: 'bundled' | 'operator' | 'subscriber'
  kind: AnnKind
  description: string
  duration_ms: number
  loop: boolean
  files: Partial<Record<AnnCodec, string>>
  sha256?: Partial<Record<AnnCodec, string>>
  level_dbov?: number | null
  has_master: boolean
  registered_at?: string
  registered_by?: string
}
export interface AnnNode {
  deployment_id: number
  node: string
  agent_id?: number
  install_path?: string
  status?: string
  presence: NodePresence
  missing?: string[]
  have?: number
  expected?: number
  error?: string
}
export interface AnnListResult { media: AnnRow[]; converter: boolean; store_dir: string; bundled_catalog?: string | null; nodes?: AnnNode[] }
export interface AnnRegisterOpts { id: string; kind: AnnKind; description?: string; loop?: boolean; normalize?: number | null; replace?: boolean; scope?: 'op' | 'sub' }
export type DeployResult = Record<string, { pushed: string[]; errors: string[]; signaled?: unknown; error?: string }>

/** 마스터 청취 — 인증 헤더가 필요하므로 <audio src> 대신 fetch → Blob URL */
export const annMasterPath = (id: string) => `/api/v1/announcements/${enc(id)}/master.wav`

export const announcementsApi = {
  list: (nodes = false) => api.get<AnnListResult>(`/announcements${nodes ? '?nodes=1' : ''}`),
  nodes: () => api.get<{ nodes: AnnNode[] }>('/announcements/nodes'),
  get: (id: string) => api.get<AnnRow>(`/announcements/${enc(id)}`),
  remove: (id: string, undeploy = true) => api.delete<{ deleted: boolean }>(`/announcements/${enc(id)}${undeploy ? '?undeploy=1' : ''}`),
  deploy: (ids?: string[]) => api.post<{ result: DeployResult }>('/announcements/deploy', { ids }),
  /** WAV 등록 — 본문은 파일 바이트(application/octet-stream), 메타는 query. OAM 이 변환기(cims-sample-conv)로 4 코덱을 만든다 */
  register: async (file: Blob, o: AnnRegisterOpts): Promise<AnnRow> => {
    const q = new URLSearchParams({ id: o.id, kind: o.kind, description: o.description ?? '' })
    if (o.loop) q.set('loop', '1')
    if (o.normalize != null) q.set('normalize', String(o.normalize))
    if (o.replace) q.set('replace', '1')
    if (o.scope) q.set('scope', o.scope)
    const res = await fetch(`/api/v1/announcements?${q.toString()}`, { method: 'POST', headers: { 'Content-Type': 'application/octet-stream', ...authHeaders() }, body: file })
    const data = await res.json().catch(() => ({}))
    if (!res.ok) throw new Error((data as { detail?: string; error?: string }).detail || (data as { error?: string }).error || `HTTP ${res.status}`)
    return data as AnnRow
  },
}
