import { api } from './client'

export interface HealthResponse {
  health: { csp: string; cmp: string; db: string }
  csp: {
    registered_users: number
    active_calls: number
    db_connected: boolean
    roles: { CSCF: boolean; TAS: boolean; PTT_AS: boolean; IBCF: boolean }
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
    rtp_ports: { total: number; used: number; free: number }
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

export interface SubscriberVoip {
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
  voip: SubscriberVoip | null
  ptt: SubscriberPtt | null
}

export interface SubscribersResponse {
  total: number
  subscribers: Subscriber[]
}

export const statsApi = {
  health: () => api.get<HealthResponse>('/stats/health'),

  subscribers: () => api.get<SubscribersResponse>('/stats/subscribers'),

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
