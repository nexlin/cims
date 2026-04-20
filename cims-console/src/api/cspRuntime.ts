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

  listRoutes:  () => api.get<{ items: RouteRule[] }>('/csp/routes').then(r => r.items),
  getRoute:    (id: number) => api.get<RouteRule>(`/csp/routes/${id}`),
  createRoute: (body: RouteRuleInput) => api.post<RouteRule>('/csp/routes', body),
  updateRoute: (id: number, body: RouteRuleInput) => api.put<RouteRule>(`/csp/routes/${id}`, body),
  deleteRoute: (id: number) => api.delete<null>(`/csp/routes/${id}`),
  dryrunRoute: (sample: RouteDryRunSample) =>
    api.post<RouteDryRunResult>('/csp/routes/dryrun', { sample }),
}

// ── Routing rule types ─────────────────────────────────────

export type MatchField =
  | 'req_uri_user' | 'req_uri_host' | 'from_uri' | 'to_uri'
  | 'method' | 'source_ip' | 'source_trunk' | string /* header:X */
export type MatchOp = 'equals' | 'not_equals' | 'prefix' | 'suffix' | 'contains' | 'regex' | 'cidr'
export type TransformAction =
  | 'set_req_uri_user' | 'set_req_uri_host' | 'set_from_host'
  | 'add_header' | 'remove_header' | 'replace_header'
  | 'strip_prefix' | 'add_prefix'
  | 'set_transport' | 'set_privacy' | 'anonymize_from'
export type TargetMode = 'trunk' | 'priority_list' | 'round_robin' | 'weighted' | 'reject'
export type FailAction = 'reject' | 'fallback' | 'next_rule'

export interface MatchCond {
  field: MatchField
  op: MatchOp
  value: string
  invert?: boolean
  seq?: number
}

export interface TransformStep {
  action: TransformAction
  target?: string | null
  value?: string | null
  seq?: number
}

export interface RouteRule {
  id: number
  name: string
  enabled: boolean
  priority: number
  description: string | null
  match: MatchCond[]
  transform: TransformStep[]
  target: { mode: TargetMode; trunk_id: number | null; json: unknown | null }
  fail: {
    action: FailAction
    code: number
    reason: string
    fallback: number | null
    timeout_ms: number
    retry_count: number
  }
  hit_count: number
  last_hit_time: string | null
  etag: string
  create_time: string | null
  update_time: string | null
}

export interface RouteRuleInput {
  name: string
  enabled?: boolean
  priority?: number
  description?: string
  match?: MatchCond[]
  transform?: TransformStep[]
  target?: { mode?: TargetMode; trunk_id?: number | null; json?: unknown }
  fail?: Partial<RouteRule['fail']>
}

export interface RouteDryRunSample {
  method?: string
  req_uri_user?: string
  req_uri_host?: string
  from_uri?: string
  to_uri?: string
  source_ip?: string
  source_trunk?: string
  headers?: Record<string, string>
}

export interface RouteDryRunResult {
  matched: boolean
  rule_id?: number
  rule_name?: string
  apply?: TransformStep[]
  target?: { mode: TargetMode; trunk_id: number | null }
}
