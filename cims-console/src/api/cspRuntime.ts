import { api } from './client'

export type SipProtocol = 'UDP' | 'TCP' | 'TLS' | 'WS' | 'WSS'
export type SipService = 'volte' | 'mcptt' | 'system' | 'console'

export interface SipListener {
  id: number
  name: string
  enabled: boolean
  bind_ip: string
  bind_port: number
  protocol: SipProtocol
  domain: string
  service: SipService
  tls_cert_path: string | null
  tls_key_path: string | null
  tls_ca_path: string | null
  tls_verify_peer: boolean
  max_connections: number
  thread_count: number
  note: string | null
  etag: string
  create_time: string | null
  update_time: string | null
}

export interface SipListenerCreate {
  name: string
  bind_ip: string
  bind_port: number
  protocol: SipProtocol
  domain?: string
  service: SipService
  enabled?: boolean
  thread_count?: number
  tls_cert_path?: string | null
  tls_key_path?: string | null
  tls_ca_path?: string | null
  tls_verify_peer?: boolean
  max_connections?: number
  note?: string | null
}

export type SipListenerUpdate = Partial<SipListenerCreate>

export const cspRuntimeApi = {
  listListeners: () => api.get<{ items: SipListener[] }>('/csp/listeners').then(r => r.items),
  getListener:   (id: number) => api.get<SipListener>(`/csp/listeners/${id}`),
  createListener: (body: SipListenerCreate) => api.post<SipListener>('/csp/listeners', body),
  updateListener: (id: number, body: SipListenerUpdate) => api.put<SipListener>(`/csp/listeners/${id}`, body),
  deleteListener: (id: number) => api.delete<null>(`/csp/listeners/${id}`),
}
