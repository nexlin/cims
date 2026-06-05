import { api } from './client'
import type { FlowMessage } from './flow'

export interface PttSession {
  dir: string
  session_id?: string
  start_time: string
  end_time: string | null
  state: string
  initiator?: string
  member_count?: number
  segment_count?: number
  speaker_count?: number
  total_speech_ms?: number
}

export interface PttGroupSummary {
  session_count: number
  last_window: string   // YYYYMMDDHH
}

export interface PttSummaryResponse {
  summaries: Record<string, PttGroupSummary>   // key = groupKey(ptt_groups.id)
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
  nodes?: Record<string, FlowMessage[]>
  messages?: FlowMessage[]
}

export interface PttFloorEvent {
  ts: string
  op: string            // GRANT | REVOKE | REJECT | RELEASE | IDLE | TAKEN
  user: string
  ssrc?: number
  prio?: number
  preempt?: boolean
  preempted_from?: string
  preempted_by?: string
  owner?: string
  owner_prio?: number
  [key: string]: unknown
}

export interface PttFloorResponse {
  floor: PttFloorEvent[]
}

export interface PttHeatCell {
  date: string; hour: number; window: string
  segment_count: number; speaker_count: number; total_speech_ms: number
}
export interface PttHeatmapResponse { group_id: string; days: number; cells: PttHeatCell[] }

export const pttApi = {
  summary(): Promise<PttSummaryResponse> {
    return api.get(`/ptt/history?summary=1`)
  },

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

  floor(groupId: string, sessionDir: string, date?: string): Promise<PttFloorResponse> {
    const q = date ? `?date=${date}` : ''
    return api.get(`/ptt/history/${encodeURIComponent(groupId)}/${encodeURIComponent(sessionDir)}/floor${q}`)
  },

  heatmap(groupId: string, days = 7): Promise<PttHeatmapResponse> {
    return api.get(`/ptt/history/${encodeURIComponent(groupId)}/heatmap?days=${days}`)
  },
}
