import { api } from './client'

export interface FlowMessage {
  ts: string      // "HH:MM:SS.usec"
  from: string    // "csp" | "cwrtc" | "cmp" | "ue"
  to: string
  proto: string   // "SIP" | "JSON" | "WS"
  label: string   // 요약 (예: "REGISTER", "200 OK", "add")
  body: string    // 원문
}

export interface FlowResponse {
  call_id: string
  date: string
  messages: FlowMessage[]
}

export interface FlowListResponse {
  date: string
  call_ids: string[]
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
}
