import { api } from './client'

export interface Recording {
  id: string                  // 세션 디렉터리 상대경로
  call_type: 'volte' | 'ptt' | 'voip_video'
  group_id: string | null
  caller: string
  callee: string | null
  start_time: string
  end_time: string | null
  duration: number
  has_video: boolean
  status: 'recording' | 'raw' | 'transcoding' | 'ready' | 'failed'
  segment_count: number
  total_speech_ms: number
  segments?: RecordingSegment[]
}

export interface RecordingSegment {
  seq: number
  speaker_id: string
  caller?: string
  callee?: string
  start_time: string
  end_time: string | null
  duration_ms: number
  has_video: boolean
  file_size: number
  status: 'recording' | 'raw' | 'transcoding' | 'ready' | 'failed'
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

// 경로 내 특수문자(+, 공백 등)만 인코딩, 슬래시는 유지
function encPath(id: string): string {
  return id.split('/').map(encodeURIComponent).join('/')
}

export const recordingsApi = {
  list: (q: RecordingsQuery = {}) =>
    api.get<RecordingsResponse>(`/recordings${buildQs(q)}`),

  get: (id: string) =>
    api.get<Recording>(`/recordings/${encPath(id)}`),

  delete: (id: string) =>
    api.delete<{ id: string }>(`/recordings/${encPath(id)}`),

  audioUrl: (id: string) =>
    `/api/v1/recordings/${encPath(id)}/audio`,

  videoUrl: (id: string, side: 'a' | 'b' = 'a') =>
    `/api/v1/recordings/${encPath(id)}/video?side=${side}`,

  segmentAudioUrl: (id: string, seq: number) =>
    `/api/v1/recordings/${encPath(id)}/segments/${seq}/audio`,

  segmentVideoUrl: (id: string, seq: number) =>
    `/api/v1/recordings/${encPath(id)}/segments/${seq}/video`,

  segments: (id: string) =>
    api.get<{ id: string; segments: RecordingSegment[] }>(`/recordings/${encPath(id)}/segments`),
}
