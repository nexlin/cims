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
  sessions: number
  talked: number
  completed: number
  /** = sessions (옛 소비자 호환) */
  success: number
  success_rate: number
  talk_rate: number
  completion_rate: number
}

/**
 * VoLTE 호 KPI — 시도(attempt) 기준 3지표.
 *   성공률 sessions/attempts · 소통률 talked/attempts · 완료율 completed/sessions
 * 비율은 표시용 파생값이고 근거는 항상 건수다 (sip_statistics.md §2.1, §5.1).
 */
export interface VoipStats {
  total_attempts: number
  /** = total_sessions. 판정 근거는 answer_time 이다. */
  total_success: number
  success_rate: number
  avg_duration_sec: number
  end_reasons: Record<string, number>
  buckets: VoipBucket[]
  total_sessions: number
  total_talked: number
  total_completed: number
  talk_rate: number
  completion_rate: number
  duration_sum_sec: number
  pdd_sum_ms: number
  pdd_n: number
  /** 평균 PDD(호 접속 지연) — answer_time - invite_time */
  avg_pdd_ms: number
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

/** 조회 단위 — 서버(services/stats_rollup.GRANULARITIES)와 같은 목록이어야 한다. */
export type StatGranularity = '1m' | '5m' | '10m' | '1h' | '1d' | '1w' | '1M' | '1y'
export type StatService = 'all' | 'volte' | 'ptt' | 'unknown'

/**
 * 서비스 1칸의 호 지표 — 분자·분모와 비율이 함께 온다.
 * 비율만 쓰면 구간을 다시 합칠 수 없고 "3건 중 2건"과 "3만건 중 2만건"이 같아 보인다.
 */
export interface CallCell {
  attempts: number
  sessions: number
  talked: number
  completed: number
  duration_sum_sec: number
  pdd_sum_ms: number
  pdd_n: number
  legs_invited: number
  legs_joined: number
  open: number
  late_dropped: number
  reasons: Record<string, number>
  success_rate: number
  talk_rate: number
  /** 세션을 분모로 한 소통률 — PTT 용(attempts 가 0 이라 talk_rate 를 쓸 수 없다) */
  talk_rate_sessions: number
  completion_rate: number
  join_rate: number
  avg_pdd_ms: number
  avg_duration_sec: number
}

export interface CallBucket {
  /** 표시 라벨 = 버킷 시작 시각 */
  bucket: string
  /** 오프셋 포함 ISO — 라벨 파싱 없이 시각을 알 수 있게 */
  bucket_start: string
  all?: CallCell
  volte?: CallCell
  ptt?: CallCell
  unknown?: CallCell
}

export interface CallsResponse {
  from: string
  to: string
  granularity: StatGranularity
  svc: StatService
  /** rollup = 1분 집계, scan = 원본 즉석 집계(집계 없는 구간) */
  source: 'rollup' | 'scan' | 'none'
  truncated?: boolean
  totals: Partial<Record<StatService, CallCell>>
  buckets: CallBucket[]
}

export interface CallsQuery {
  from?: string
  to?: string
  date?: string
  granularity?: StatGranularity
  svc?: StatService
}

export const statsApi = {
  health: () => api.get<HealthResponse>('/stats/health'),

  /** 호 통계 — 1분 기저 집계 위. 서비스축(volte/ptt)과 분~년 단위를 모두 받는다. */
  calls: (params: CallsQuery = {}) => {
    const sp = new URLSearchParams()
    if (params.from) sp.set('from', params.from)
    if (params.to) sp.set('to', params.to)
    if (params.date) sp.set('date', params.date)
    if (params.granularity) sp.set('granularity', params.granularity)
    if (params.svc) sp.set('svc', params.svc)
    const qs = sp.toString()
    return api.get<CallsResponse>('/stats/calls' + (qs ? `?${qs}` : ''))
  },

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
