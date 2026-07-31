import { api } from './client'

export interface HealthResponse {
  health: { csp: string; cmp: string; db: string }
  csp: {
    registered_users: number
    active_calls: number
    db_connected: boolean
    roles: { CSCF: boolean; TAS: boolean; PTT_AS: boolean; IBCF: boolean }
    // 대시보드 KPI — DB 카운트 (가입자/번호/등록/그룹). 구버전 OAM 이면 undefined.
    subscribers_total?: number
    volte_numbers?: number
    volte_registered?: number
    ptt_numbers?: number
    ptt_registered?: number
    ptt_groups_total?: number
    timeouts?: { user_timeout: number; stale_call_timeout: number; send_options_period: number }
    trunks?: Array<{
      id: number
      name: string
      remote: string
      enabled: boolean
      alive: boolean
      last_rtt_ms: number
      last_ping: number
      last_reply: number
      fail_count: number
    }>
  }
  cmp: {
    sessions: number
    groups: number
    rtp_ports: { total: number; used: number; free: number }            // VoIP 풀
    rtp_ports_ptt?: { total: number; used: number; free: number }        // PTT(그룹통화) 풀
    session_timeout?: number
  }
  record_enable: boolean
  active_voip: Array<{ call_id: string; initiator: string; callee: string; state: string; invite_time: string }>
  active_ptt: Array<{ call_id: string; group_id: string; initiator: string; state: string; invite_time: string }>
}

export interface MessageBucket {
  hour?: number
  voip_invite: number
  ptt_invite: number
  total: number
}

export interface MessagesResponse {
  date: string
  granularity: string
  buckets: MessageBucket[]
}

export interface VoipBucket {
  hour?: number
  date?: string
  attempts: number
  success: number
  success_rate: number
}

export interface VoipStats {
  total_attempts: number
  total_success: number
  success_rate: number
  avg_duration_sec: number
  end_reasons: Record<string, number>
  buckets: VoipBucket[]
}

export interface PttBucket {
  hour?: number
  date?: string
  calls: number
}

export interface PttStats {
  total_calls: number
  avg_duration_sec: number
  by_group: Record<string, number>
  buckets: PttBucket[]
}

export interface ServiceStatsResponse {
  granularity: string
  from: string
  to: string
  volte?: VoipStats   // 백엔드 응답 키는 'volte' (svc=volte/voip 둘 다 volte 키 반환)
  ptt?: PttStats
}

export interface SubscriberVolte {
  msisdn: string
  online: boolean
  register_time: string | null
  calls: Array<{ call_id: string; peer: string; role: string; state: string; invite_time: string }>
}

export interface SubscriberPttGroup {
  call_id: string
  group_id: string
  state: string
  invite_time: string
  total_members: number
  active_members: number
  floor_holder: string | null
}

export interface SubscriberPtt {
  msisdn: string
  online: boolean
  register_time: string | null
  groups: SubscriberPttGroup[]
}

export interface Subscriber {
  person_id: number
  name: string
  org?: string
  org_path?: string
  volte: SubscriberVolte | null
  ptt: SubscriberPtt | null
}

export interface SubscribersResponse {
  total: number
  page: number
  limit: number
  status: 'active' | 'online' | 'all'
  counts: { all: number; online: number; active: number }
  subscribers: Subscriber[]
}

export interface SubscribersQuery {
  status?: 'active' | 'online' | 'all'
  q?: string
  page?: number
  limit?: number
  org?: string
}

// ── 서비스 라이브 모니터링 ──
export interface Pool { total: number; used: number; free: number }
export interface LiveAnomalyTag { type: string; detail: string }
export interface VolteCall {
  call_id: string; session_id: string; caller: string; callee: string
  state: string; video: boolean; invite_time: string | null; answered_at: string | null
  duration_sec: number; anomalies: LiveAnomalyTag[]
  media_node?: string; org?: string
}
export interface PttGroupMember { subscriber_id: string; role: string }
export interface PttGroup {
  group_id: string; session_id: string; name: string; type: string
  total_members: number; active_members: number
  // floor_holder = 대표 화자, floor_holders = 발언자 전원(dual/multi-talker 동시 발언)
  floor_holder: string | null; floor_holders?: string[]; initiator: string | null
  invite_time: string | null; duration_sec: number; floor_held_sec?: number
  floor_count?: number; last_floor?: string | null
  members?: PttGroupMember[]; anomalies: LiveAnomalyTag[]; org?: string
}
export interface PttTalker { msisdn: string; org: string; group_id: string; group_name: string }
export interface PttMember { msisdn: string; name: string; role: string; priority: number | null; active: boolean; talking: boolean }
export interface PttMembersResponse {
  group: string; total: number; page: number; limit: number
  active_count: number; floor_holder: string | null; floor_holders?: string[]; members: PttMember[]
}
export interface Anomaly { kind: string; type: string; detail: string; label: string; ref: string }
export interface MediaNode { host: string; up: boolean; volte_rtp: Pool; ptt_rtp: Pool; groups: number }
export interface ServiceLive {
  ts: string
  volte: { kpi: { active: number; ringing: number; avg_duration_sec: number; registered: number; numbers: number }; calls: VolteCall[] }
  ptt: {
    kpi: { talking: number; recent_active: number; active_groups: number; participants: number; total_groups: number; registered: number; numbers: number }
    groups: PttGroup[]; talkers: PttTalker[]
  }
  capacity: { volte_rtp: Pool; ptt_rtp: Pool; nodes: MediaNode[] }
  anomalies: Anomaly[]
}
export interface ServiceEvent { ts: string; kind: string; type: string; detail: string; ref: string }
export interface OrgStat {
  code: string; name: string; parent: string | null; depth: number
  members: number; volte_reg: number; ptt_reg: number
  active_volte: number; active_ptt: number; ptt_talking: number
}
export interface TrendPoint {
  t: number; volte_active: number; volte_calls: number
  ptt_grants: number; ptt_speakers: number; ptt_groups: number
}
export type TrendMetric = 'volte_active' | 'volte_calls' | 'ptt_grants' | 'ptt_speakers' | 'ptt_groups'
export interface ServiceTrend {
  window: string; window_min: number; bucket_sec: number
  points: TrendPoint[]; peaks: Record<TrendMetric, number>
}

export const statsApi = {
  health: () => api.get<HealthResponse>('/stats/health'),

  subscribers: (params: SubscribersQuery = {}) => {
    const sp = new URLSearchParams()
    if (params.status) sp.set('status', params.status)
    if (params.q) sp.set('q', params.q)
    if (params.page) sp.set('page', String(params.page))
    if (params.limit) sp.set('limit', String(params.limit))
    if (params.org) sp.set('org', params.org)
    const qs = sp.toString()
    return api.get<SubscribersResponse>('/stats/subscribers' + (qs ? `?${qs}` : ''))
  },

  serviceLive: () => api.get<ServiceLive>('/stats/service/live'),
  serviceTrend: (window: string = '8h') => api.get<ServiceTrend>(`/stats/service/trend?window=${window}`),
  serviceEvents: (limit = 60) => api.get<{ events: ServiceEvent[] }>(`/stats/service/events?limit=${limit}`),
  serviceOrg: () => api.get<{ orgs: OrgStat[]; db_degraded?: boolean }>('/stats/service/org'),
  pttMembers: (group: string, page = 1, limit = 50) => api.get<PttMembersResponse>(`/stats/service/ptt-members?group=${encodeURIComponent(group)}&page=${page}&limit=${limit}`),

  messages: (params: { date?: string; granularity?: string; proto?: string }) => {
    const p = new URLSearchParams()
    if (params.date) p.set('date', params.date)
    if (params.granularity) p.set('granularity', params.granularity)
    if (params.proto) p.set('proto', params.proto)
    const s = p.toString()
    return api.get<MessagesResponse>(`/stats/messages${s ? '?' + s : ''}`)
  },

  service: (svc: 'volte' | 'ptt' | 'summary', params: { granularity?: string; from?: string; to?: string; date?: string }) => {
    const p = new URLSearchParams()
    if (params.granularity) p.set('granularity', params.granularity)
    if (params.from) p.set('from', params.from)
    if (params.to) p.set('to', params.to)
    if (params.date) p.set('date', params.date)
    const s = p.toString()
    return api.get<ServiceStatsResponse>(`/stats/service/${svc}${s ? '?' + s : ''}`)
  },
}
