import { api } from './client'

export interface Recording {
  id: number
  call_id: string
  call_type: 'voip' | 'ptt'
  group_id: string | null
  caller: string
  callee: string | null
  start_time: string
  end_time: string | null
  duration: number
  has_video: boolean
  file_size: number
  status: 'raw' | 'transcoding' | 'ready' | 'failed'
  segment_count: number
  total_speech_ms: number
  segments?: RecordingSegment[]
}

export interface RecordingSegment {
  seq: number
  speaker_id: string
  start_time: string
  end_time: string | null
  duration_ms: number
  has_video: boolean
  file_size: number
  status: 'raw' | 'transcoding' | 'ready' | 'failed'
}

export interface RecordingsResponse {
  total: number
  limit: number
  offset: number
  recordings: Recording[]
}

export interface RecordingsQuery {
  call_type?: string
  caller?: string
  group_id?: string
  from_dt?: string
  to_dt?: string
  limit?: number
  offset?: number
}

function buildQs(q: RecordingsQuery): string {
  const p = new URLSearchParams()
  if (q.call_type) p.set('call_type', q.call_type)
  if (q.caller)    p.set('caller', q.caller)
  if (q.group_id)  p.set('group_id', q.group_id)
  if (q.from_dt)   p.set('from_dt', q.from_dt)
  if (q.to_dt)     p.set('to_dt', q.to_dt)
  if (q.limit)     p.set('limit', String(q.limit))
  if (q.offset)    p.set('offset', String(q.offset))
  const s = p.toString()
  return s ? '?' + s : ''
}

export const recordingsApi = {
  list: (q: RecordingsQuery = {}) =>
    api.get<RecordingsResponse>(`/recordings${buildQs(q)}`),

  get: (id: number) =>
    api.get<Recording>(`/recordings/${id}`),

  delete: (id: number) =>
    api.delete<{ id: number }>(`/recordings/${id}`),

  audioUrl: (id: number) => `/api/v1/recordings/${id}/audio`,

  videoUrl: (id: number, side: 'a' | 'b' = 'a') =>
    `/api/v1/recordings/${id}/video?side=${side}`,

  segmentAudioUrl: (id: number, seq: number) =>
    `/api/v1/recordings/${id}/segments/${seq}/audio`,

  segments: (id: number) =>
    api.get<{ recording_id: number; segments: RecordingSegment[] }>(`/recordings/${id}/segments`),
}
