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
  _node?: string  // 백엔드 응답의 노드 그룹 키(csp/cmp/csc) — 노드별 필터/뱃지용(프론트 부여)
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
    return api.getCached(`/flow/list${q}`, 4000)
  },
  get(callId: string, date?: string, callType?: string, hour?: string): Promise<FlowResponse> {
    const params = new URLSearchParams()
    if (date) params.set('date', date)
    if (callType) params.set('call_type', callType)
    // hour: 선택 호의 invite_time 에서 도출 — .d 디렉터리 탐색을 해당 시간으로 좁혀 빠르게.
    //   (서버는 추가로 call.json 시간창으로 메시지 읽기를 5분 버킷까지 좁힌다.)
    if (hour) params.set('hour', hour)
    const q = params.toString() ? `?${params.toString()}` : ''
    // 호별 메시지 흐름은 종료된 호면 불변 → 재오픈/리렌더 시 재요청 회피(10s 캐시 + in-flight 중복제거).
    return api.getCached(`/flow/${encodeURIComponent(callId)}${q}`, 10000)
  },
  /** 메시지 body 조회 (interface jsonl seq 기반, fallback: ts+dir)
   *  node: 'csp' | 'cmp' | 'csc' — 여러 노드가 같은 iface에 msg 파일을 쓸 때 정확한 파일 선택에 사용
   */
  getBody(date: string, hour: string | undefined, seq?: number, ts?: string, dir?: string, proto?: string, iface?: string, node?: string): Promise<FlowBodyResponse> {
    const params = new URLSearchParams({ date })
    if (hour) params.set('hour', hour)
    if (seq && seq > 0) {
      params.set('seq', String(seq))
      if (iface) params.set('iface', iface)
      if (node) params.set('node', node)
      // 5분 버킷(open-per-write) 파일에서 seq 는 버킷별로 리셋되므로, 메시지 ts(HH:MM:SS)를 함께 전달해
      // 서버가 정확한 5분 파일을 선택하게 한다. (구 단일 시간당 파일은 ts 무시 → 호환)
      if (ts) params.set('ts', ts)
    } else {
      if (ts) params.set('ts', ts)
      if (dir) params.set('dir', dir)
      if (proto) params.set('proto', proto)
    }
    return api.get(`/flow/body?${params.toString()}`)
  },
}
