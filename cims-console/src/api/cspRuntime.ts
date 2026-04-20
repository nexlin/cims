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

export interface SipTrunk {
  id: number
  name: string
  enabled: boolean
  remote_ip: string
  remote_port: number
  remote_domain: string
  protocol: 'UDP' | 'TCP' | 'TLS'
  outbound_proxy_ip: string | null
  outbound_proxy_port: number | null
  register_to_remote: boolean
  auth_user: string | null
  auth_realm: string | null
  register_expires: number
  options_ping_sec: number
  options_dead_threshold: number
  srv_lookup: boolean
  dns_fallback: boolean
  max_concurrent_calls: number
  cps_limit: number
  note: string | null
  etag: string
  create_time: string | null
  update_time: string | null
}

export interface SipTrunkCreate {
  name: string
  remote_ip: string
  remote_port?: number
  remote_domain?: string
  protocol?: 'UDP' | 'TCP' | 'TLS'
  enabled?: boolean
  outbound_proxy_ip?: string | null
  outbound_proxy_port?: number | null
  register_to_remote?: boolean
  auth_user?: string | null
  auth_password?: string | null
  auth_realm?: string | null
  register_expires?: number
  options_ping_sec?: number
  options_dead_threshold?: number
  srv_lookup?: boolean
  dns_fallback?: boolean
  max_concurrent_calls?: number
  cps_limit?: number
  note?: string | null
}

export type SipTrunkUpdate = Partial<SipTrunkCreate>

export interface SipTrunkStatus {
  id: number
  name: string
  remote: string
  enabled: boolean
  alive: boolean
  last_rtt_ms: number
  last_ping: number
  last_reply: number
  fail_count: number
}

export const cspRuntimeApi = {
  listListeners: () => api.get<{ items: SipListener[] }>('/csp/listeners').then(r => r.items),
  getListener:   (id: number) => api.get<SipListener>(`/csp/listeners/${id}`),
  createListener: (body: SipListenerCreate) => api.post<SipListener>('/csp/listeners', body),
  updateListener: (id: number, body: SipListenerUpdate) => api.put<SipListener>(`/csp/listeners/${id}`, body),
  deleteListener: (id: number) => api.delete<null>(`/csp/listeners/${id}`),

  listTrunks: () => api.get<{ items: SipTrunk[] }>('/csp/trunks').then(r => r.items),
  getTrunk:   (id: number) => api.get<SipTrunk>(`/csp/trunks/${id}`),
  createTrunk: (body: SipTrunkCreate) => api.post<SipTrunk>('/csp/trunks', body),
  updateTrunk: (id: number, body: SipTrunkUpdate) => api.put<SipTrunk>(`/csp/trunks/${id}`, body),
  deleteTrunk: (id: number) => api.delete<null>(`/csp/trunks/${id}`),
}
