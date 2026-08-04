import { api } from './client'

export interface Recording {
  id: string                  // 세션 디렉터리 상대경로
  call_type: 'volte' | 'ptt' | 'volte_video'
  group_id: string | null
  caller: string
  callee: string | null
  start_time: string
  end_time: string | null
  duration: number
  has_video: boolean
  status: 'recording' | 'raw' | 'transcoding' | 'ready' | 'failed'
  segment_count: number
  turn_count?: number        // 발언 턴 = 화자 구간 수 (동시 발언은 세그먼트 1개에 턴 여럿)
  max_concurrent?: number
  total_speech_ms: number
  segments?: RecordingSegment[]
}

// 한 슬롯 트랙 안에서 한 화자가 점유한 구간 (세그먼트 시작 기준 offset)
export interface SpeakerSpan {
  id: string
  offset_ms: number
  dur_ms: number
}

// 세그먼트의 미디어 트랙 — PTT 는 동시 발언 슬롯(0..N), VoIP 는 leg(a/b).
// 단일 화자 세그먼트는 audio 트랙 1개뿐이다.
export interface SegmentTrack {
  slot: number               // PTT 슬롯 번호 (VoIP 는 -1)
  kind: 'audio' | 'video'
  side: string               // VoIP leg ('a'|'b'), PTT 는 ''
  pt: number
  codec: string
  speakers: SpeakerSpan[]
  has_video: boolean         // 이 슬롯에 영상 트랙이 함께 있는가
  status: 'recording' | 'raw' | 'transcoding' | 'ready' | 'failed'
}

export interface RecordingSegment {
  seq: number
  speaker_id: string         // 대표 화자 (슬롯 0 의 첫 화자)
  caller?: string
  callee?: string
  start_time: string
  end_time: string | null
  duration_ms: number
  has_video: boolean
  file_size: number
  status: 'recording' | 'raw' | 'transcoding' | 'ready' | 'failed'
  status_reason?: string     // status=failed 일 때 사유 (예: 녹취 음성 데이터 없음)
  tracks?: SegmentTrack[]    // 슬롯 트랙 (동시 발언·전이중 private call)
  speaker_ids?: string[]     // 이 세그먼트의 화자 (등장 순서)
  talker_count?: number      // 음성 트랙 수
  max_concurrent?: number    // 동시 발언 최대 인원
}

// 파형 피크 (0..255) — 전이중 통화 플레이어의 화자별 레인
export interface SegmentPeaks {
  seq: number
  slot: number | null
  buckets: number
  peaks: number[]
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

  // slot 미지정 = 믹스(동시 발언 화자 전원 합성 — 실제로 들린 소리).
  // slot=K = 슬롯 K 화자 단독본 (화자 식별·증거용).
  segmentAudioUrl: (id: string, seq: number, slot?: number) =>
    `/api/v1/recordings/${encPath(id)}/segments/${seq}/audio${slot != null ? `?slot=${slot}` : ''}`,

  segmentVideoUrl: (id: string, seq: number, slot?: number) =>
    `/api/v1/recordings/${encPath(id)}/segments/${seq}/video${slot != null ? `?slot=${slot}` : ''}`,

  segmentPeaksUrl: (id: string, seq: number, slot?: number) =>
    `/api/v1/recordings/${encPath(id)}/segments/${seq}/peaks${slot != null ? `?slot=${slot}` : ''}`,

  segmentPeaks: (id: string, seq: number, slot?: number) =>
    api.get<SegmentPeaks>(`/recordings/${encPath(id)}/segments/${seq}/peaks${slot != null ? `?slot=${slot}` : ''}`),

  segments: (id: string) =>
    api.get<{ id: string; segments: RecordingSegment[] }>(`/recordings/${encPath(id)}/segments`),
}
