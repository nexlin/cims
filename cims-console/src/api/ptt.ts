import { api } from './client'
import type { FlowMessage } from './flow'

export interface PttSession {
  dir: string
  session_id?: string
  start_time: string
  end_time: string | null
  state: string
  initiator: string
  member_count?: number
}

export interface PttEvent {
  ts: string
  type: string
  member?: string
  duration?: number
  [key: string]: unknown
}

export interface PttSessionsResponse {
  group_id: string
  sessions: PttSession[]
}

export interface PttSessionEventsResponse {
  session: Record<string, unknown>
  group_snapshot: Record<string, unknown>
  events: PttEvent[]
  participants: Array<{ msisdn: string; role: string; join_time: string | null; leave_time: string | null }>
}

export interface PttFlowResponse {
  call_id: string
  date: string
  messages: FlowMessage[]
}

export const pttApi = {
  sessions(groupId: string, date?: string): Promise<PttSessionsResponse> {
    const p = new URLSearchParams()
    p.set('group_id', groupId)
    if (date) p.set('date', date)
    return api.get(`/ptt/history?${p.toString()}`)
  },

  events(groupId: string, sessionDir: string, date?: string): Promise<PttSessionEventsResponse> {
    const q = date ? `?date=${date}` : ''
    return api.get(`/ptt/history/${encodeURIComponent(groupId)}/${encodeURIComponent(sessionDir)}${q}`)
  },

  flow(groupId: string, sessionDir: string, date?: string): Promise<PttFlowResponse> {
    const q = date ? `?date=${date}` : ''
    return api.get(`/ptt/history/${encodeURIComponent(groupId)}/${encodeURIComponent(sessionDir)}/flow${q}`)
  },
}
