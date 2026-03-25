import { api } from './client'

export interface CallParticipant {
  msisdn: string
  role: 'caller' | 'callee' | 'member'
  join_time: string | null
  leave_time: string | null   // null = 현재 연결 중
}

export interface CallLog {
  id: number
  call_id: string
  call_type: 'voip' | 'ptt'
  group_id: string | null
  initiator: string
  callee: string
  state: 'ringing' | 'active' | 'ended'
  invite_time: string
  answer_time: string | null
  end_time: string | null
  duration: number | null     // 초
  sip_status: number | null
  end_reason: string | null
  end_reason_ko: string
  participants: CallParticipant[]
}

export interface CallLogsResponse {
  total: number
  limit: number
  offset: number
  logs: CallLog[]
}

export interface CallLogsQuery {
  state?: string
  caller?: string
  callee?: string
  msisdn?: string
  group_id?: string
  call_type?: string
  from_dt?: string
  to_dt?: string
  limit?: number
  offset?: number
}

function buildQs(q: CallLogsQuery): string {
  const p = new URLSearchParams()
  if (q.state)     p.set('state', q.state)
  if (q.caller)    p.set('caller', q.caller)
  if (q.callee)    p.set('callee', q.callee)
  if (q.msisdn)    p.set('msisdn', q.msisdn)
  if (q.group_id)  p.set('group_id', q.group_id)
  if (q.call_type) p.set('call_type', q.call_type)
  if (q.from_dt)   p.set('from_dt', q.from_dt)
  if (q.to_dt)     p.set('to_dt', q.to_dt)
  if (q.limit)     p.set('limit', String(q.limit))
  if (q.offset)    p.set('offset', String(q.offset))
  const s = p.toString()
  return s ? '?' + s : ''
}

export const callsApi = {
  list:   (q: CallLogsQuery = {}) =>
    api.get<CallLogsResponse>(`/call/logs${buildQs(q)}`),

  active: () =>
    api.get<CallLogsResponse>('/call/logs/active'),
}
