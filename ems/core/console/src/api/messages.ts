import { api } from './client'

/** MCData 그룹 메시지 보관 레코드 — CSP MCDATA-AS 의 messages.jsonl (oam-svc /messages 스캔). */
export interface GroupMessage {
  ts: string
  group: string
  from: string
  msg_type: 'sds' | 'fd' | 'text'
  conv_id: string
  msg_id: string
  text: string
  size: number
  disposition_req: number
  fanout: number
  // msg_type === 'fd'
  file_name?: string
  file_url?: string
  file_size?: number
  file_type?: string
}

export interface GroupMessagesResponse {
  date: string
  group_id: string
  total: number
  items: GroupMessage[]
  groups: string[]   // 보관 데이터가 있는 그룹 목록
}

export interface GroupMessagesQuery {
  date?: string       // YYYY-MM-DD (기본 오늘)
  group_id?: string
  hour?: string
  q?: string          // 본문/발신자/파일명 검색
  limit?: number
  offset?: number
}

function buildQs(q: GroupMessagesQuery): string {
  const p = new URLSearchParams()
  Object.entries(q).forEach(([k, v]) => {
    if (v !== undefined && v !== null && v !== '') p.set(k, String(v))
  })
  const s = p.toString()
  return s ? `?${s}` : ''
}

export const messagesApi = {
  list: (q: GroupMessagesQuery = {}) => api.get<GroupMessagesResponse>('/messages' + buildQs(q)),
}
