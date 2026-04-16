import { api } from './client'

export interface FlowMessage {
  ts: string      // "HH:MM:SS.usec"
  node?: string   // "csp" | "cmp" | "csc"
  from: string    // "csp" | "cwrtc" | "cmp" | "ue"
  to: string
  proto: string   // "SIP" | "JSON" | "WS"
  label: string   // 요약 (예: "REGISTER", "200 OK", "add")
  detail?: string // 메서드 부가 정보 (UI: method(detail))
  mid?: string    // 메시지 식별 ID (동일 메시지 cross-node 상관)
  sesid?: string  // 세션 식별 (PTT: 그룹ID, VoLTE: 세션ID)
  subid?: string  // 하위 식별 (PTT: 세션seq, VoLTE: Call-ID)
  body?: string   // 원문 (lazy loading — 선택 시 별도 조회)
  seq?: number    // interface jsonl line number (for body lookup)
  iface?: string  // interface name: "sip" | "cmp" | "csc" (for body lookup)
}

export interface FlowResponse {
  call_id: string
  date: string
  nodes?: Record<string, FlowMessage[]>
  messages?: FlowMessage[]  // 레거시 호환
}

export interface FlowListResponse {
  date: string
  call_ids: string[]
}

export interface FlowBodyResponse {
  body: string
}

export const flowApi = {
  list(date?: string): Promise<FlowListResponse> {
    const q = date ? `?date=${date}` : ''
    return api.get(`/flow/list${q}`)
  },
  get(callId: string, date?: string): Promise<FlowResponse> {
    const q = date ? `?date=${date}` : ''
    return api.get(`/flow/${encodeURIComponent(callId)}${q}`)
  },
  /** 메시지 body 조회 (interface jsonl seq 기반, fallback: ts+dir) */
  getBody(date: string, hour: string | undefined, seq?: number, ts?: string, dir?: string, proto?: string, iface?: string): Promise<FlowBodyResponse> {
    const params = new URLSearchParams({ date })
    if (hour) params.set('hour', hour)
    if (seq && seq > 0) {
      params.set('seq', String(seq))
      if (iface) params.set('iface', iface)
    } else {
      if (ts) params.set('ts', ts)
      if (dir) params.set('dir', dir)
      if (proto) params.set('proto', proto)
    }
    return api.get(`/flow/body?${params.toString()}`)
  },
}
