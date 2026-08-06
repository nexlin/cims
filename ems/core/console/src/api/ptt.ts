import { api } from './client'
import type { FlowMessage } from './flow'

export interface PttSession {
  // 세션키 — 'S{yyyymmddHHMMSSuuuuuu}_{n}' (세션 디렉터리 이름). 구 녹취는 'YYYYMMDDHH'.
  dir: string
  session_id?: string
  // 이 세션이 걸친 시간버킷들 — 통화가 시간을 넘기면 2개 이상 (행은 그래도 하나).
  windows?: string[]
  sesid?: string             // flow 로그와 같은 세션 식별자
  call_id?: string
  speakers?: string[]
  start_time: string
  end_time: string | null
  state: string
  initiator?: string
  member_count?: number
  segment_count?: number
  turn_count?: number        // 발언 턴 = 화자 구간 수 (동시 발언 세그먼트는 턴이 여럿)
  speaker_count?: number     // 슬롯 화자 포함
  max_concurrent?: number    // 최대 동시 발언 인원
  total_speech_ms?: number   // 발화 구간 합 (겹침 1회 계산)
  talk_ms?: number           // 발화 누적 합 (화자별 구간 합)
  // 세션 당시 floor 축 (시간버킷 session.json — 없으면 ''/0, 그룹 레벨로 폴백)
  floor_control?: string     // 'on' | 'off'(전이중) | ''
  floor_policy?: string      // single | dual | multi
  max_talkers?: number
}

// 세션 분류 — 좌측 목록의 섹션. DB 그룹이 아닌 세션(1:1/임시)도 녹취 디렉터리에서 드러난다.
export type PttSessionKind = 'group' | 'private' | 'adhoc' | 'unknown'

export interface PttGroupSummary {
  session_count: number
  last_window: string        // YYYYMMDDHH
  kind?: PttSessionKind
  mcptt_group_id?: string
  name?: string
  group_type?: string        // prearranged | chat | broadcast | private
  floor_control?: string     // 'on' | 'off'(전이중) | ''(구 세션 — 미기록)
  floor_policy?: string      // single | dual | multi
  max_talkers?: number
  video_enabled?: boolean
  member_count?: number
  peers?: string[]           // kind=private 일 때 상대 2인
}

export interface PttSummaryResponse {
  // key = 녹취 디렉터리명 — DB 그룹은 ptt_groups.id(surrogate), 그 외는 세션 식별자
  summaries: Record<string, PttGroupSummary>
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

// ── 세션 행 (평면 목록) ────────────────────────────────────────────
// 기록 단위가 세션이므로 그룹·1:1·임시가 같은 행 모양을 갖는다. 그룹 축 값(그룹명·
// mcptt_group_id)은 행에 실려 오고, 목록이 그룹을 따로 펼치지 않는다.
export interface PttSessionRow extends PttSession {
  group_key: string          // 녹취 저장 키 (= ptt/{key}) — 상세/녹취 조회에 쓴다
  kind: PttSessionKind
  group_name?: string
  mcptt_group_id?: string
  group_type?: string
  people?: string[]          // 참여자 (발언 안 한 참가자 포함)
  peers?: string[]           // 1:1·임시에서 개시자를 뺀 상대
}

export interface PttSessionsFlatResponse {
  date: string
  from: string
  to: string
  hour: string
  state: string
  sort: string
  order: string
  total: number
  live_total: number         // 필터 적용 후 진행중 수 (state 필터 이전 값)
  items: PttSessionRow[]
  hours: Record<string, number>   // 시간대별 세션 수 (hour 필터 이전) — 히트맵용
}

export interface PttSessionQuery {
  date?: string
  from?: string
  to?: string
  days?: number
  kind?: string              // group,private,adhoc (콤마)
  group_key?: string         // 콤마 다중
  person?: string
  state?: 'live' | 'ended'
  hour?: string
  q?: string
  sort?: 'start' | 'turns' | 'speech' | 'duration' | 'speakers'
  order?: 'asc' | 'desc'
  limit?: number
  offset?: number
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

// CMP 가 floor.jsonl 에 기록하는 op 8종 (TS 24.380)
export type PttFloorOp =
  | 'GRANT' | 'RELEASE' | 'IDLE' | 'REVOKE' | 'REVOKE_END'
  | 'QUEUE' | 'QUEUE_CANCEL' | 'DENY'

export interface PttFloorEvent {
  ts: string
  op: PttFloorOp | string
  user: string
  ssrc?: number
  prio?: number
  // GRANT
  slot?: number
  talkers?: number
  policy?: string
  preempt?: boolean
  preempted_from?: string
  tier?: string
  // DENY
  reason?: string
  cause?: number
  owner?: string
  owner_prio?: number
  owner_tier?: string
  initiator?: string
  // QUEUE / QUEUE_CANCEL
  pos?: number
  qsize?: number
  revoked?: string
  removed?: number
  // REVOKE / REVOKE_END / RELEASE
  grace_sec?: number
  idle_ms?: number
  preempted_by?: string
  [key: string]: unknown
}

export interface PttFloorResponse {
  floor: PttFloorEvent[]
}

export const pttApi = {
  summary(): Promise<PttSummaryResponse> {
    return api.get(`/ptt/history?summary=1`)
  },

  /** 세션 평면 목록 — /call/logs 와 같은 계약 (date/hour/q/limit/offset + hours).
   *  진행중 세션은 state='live' 로 따로 받는다(페이징 없이 전량). */
  flat(q: PttSessionQuery = {}): Promise<PttSessionsFlatResponse> {
    const p = new URLSearchParams()
    for (const [k, v] of Object.entries(q)) {
      if (v !== undefined && v !== null && v !== '') p.set(k, String(v))
    }
    return api.get(`/ptt/sessions?${p.toString()}`)
  },

  sessions(groupId: string, opts?: { date?: string; days?: number }): Promise<PttSessionsResponse> {
    const p = new URLSearchParams()
    p.set('group_id', groupId)
    if (opts?.date) p.set('date', opts.date)
    if (opts?.days) p.set('days', String(opts.days))
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
}
