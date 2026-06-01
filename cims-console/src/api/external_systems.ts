import { api } from './client'

// 외부 시스템(외부 DB / 모니터링 / 스토리지 등) 레지스트리. OAM /api/v1/external-systems.
export type ExternalSystemType = 'db' | 'monitoring' | 'storage' | 'auth' | 'other'
export type ProbeMode = 'none' | 'tcp' | 'http' | 'icmp'
export type ProbeStatus = 'up' | 'down' | 'unknown'

export interface Endpoint { host: string; port: number; label?: string }
export interface ProbeConfig { mode: ProbeMode; host?: string; port?: number; url?: string; timeout?: number }

export interface ExternalSystem {
  id: number
  name: string
  type: ExternalSystemType
  endpoints: Endpoint[]
  description?: string
  probe?: ProbeConfig
  tags?: string[]
  enabled: boolean
  create_time?: string
  update_time?: string
}

export interface ExternalSystemInput {
  name: string
  type: ExternalSystemType
  endpoints: Endpoint[]
  description?: string
  probe?: ProbeConfig
  tags?: string[]
  enabled?: boolean
}

export interface ProbeResult { id: number; status: ProbeStatus; latency_ms?: number; checked_at?: string }

export const externalSystemsApi = {
  list:   () => api.get<{ systems: ExternalSystem[] }>('/external-systems').then(r => r.systems),
  get:    (id: number) => api.get<ExternalSystem>(`/external-systems/${id}`),
  create: (d: ExternalSystemInput) => api.post<{ id: number }>('/external-systems', d),
  update: (id: number, d: Partial<ExternalSystemInput>) => api.put<{ id: number }>(`/external-systems/${id}`, d),
  delete: (id: number) => api.delete<{ id: number; deleted: boolean }>(`/external-systems/${id}`),
  probe:  (id: number) => api.post<ProbeResult>(`/external-systems/${id}/probe`, {}),
  status: () => api.get<{ items: ProbeResult[] }>('/external-systems/status').then(r => r.items),
}
