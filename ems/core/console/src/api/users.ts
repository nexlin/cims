import { api } from './client'

export interface Subscription {
  id: string          // MSISDN of this line
  auth_id?: string    // legacy — P8 에서 제거됨(백엔드 미반환). imsi 로 대체.
  passwd?: string
  dnd: boolean
  forward_id: string
  service_ref?: string | null   // 소속 서비스(access_services.name, 예: volte/mcptt) — 도메인 결정
  service_id?: number | null    // (구) 숫자 service_id 호환
  imsi?: string | null          // SIM IMSI — 인증 username 의 user 파트. 번호 add 시 필수.
  register_time?: string | null
  logout_time?: string | null
  mcptt_profile?: McpttProfile | null   // PTT 번호에만 (상세 응답 동봉, 미설정=null → 기본값)
}

// 사용자 MCPTT 프로파일 (ptt_user_profile — TS 24.484). SOS 대상 결정 + 개시 인가.
export interface McpttProfile {
  allow_emergency_call: boolean     // 긴급 그룹콜 개시 인가
  allow_emergency_alert: boolean    // 긴급경보 개시 인가
  allow_adhoc_call: boolean         // ad hoc 개시 인가 (시스템 정책과 AND)
  emergency_group_mode: 'DedicatedGroup' | 'UseCurrentlySelectedGroup'  // SOS 대상 결정
  emergency_group_id: string | null // 전용 긴급그룹 (DedicatedGroup 모드의 콜·경보 대상)
}

// 가입자(person). login_id/passwd = 단말(IdMS) 로그인 자격 — MCPTT ID 와 별개.
//   (콘솔 admin 계정은 별도 console_accounts. passwd 는 목록 응답에 미포함, 편집 입력만.)
export interface UserSummary {
  id: number          // person ID (auto-increment)
  name: string
  title?: string | null      // 직함 (예: 팀장) — 그룹문서 cims:user-title 확장으로 단말에 전달
  login_id?: string | null   // 단말/IdMS 로그인 ID (예: test001)
  org_id: string
  email?: string
  details?: string | null
  reject_id: string[]
  call_subscriptions: Subscription[]
  ptt_subscriptions: Subscription[]
  create_time?: string | null
  update_time?: string | null
}

// UserDetail is same shape as UserSummary (list API now includes subscriptions)
export type UserDetail = UserSummary

export type UserInput = {
  name: string; org_id: string; title?: string; email?: string; details?: string; reject_id?: string[]
  login_id?: string; passwd?: string   // 단말 IdMS 로그인 자격 (passwd 는 변경 시에만 전송)
}

const enc = (s: string) => encodeURIComponent(s)

export const usersApi = {
  list:   ()                                                => api.get<{users: UserSummary[]}>('/users').then(r => r.users),
  get:    (id: number)                                      => api.get<UserDetail>(`/users/${id}`),
  create: (data: UserInput)                                 => api.post<{id:number}>('/users', data),
  update: (id: number, data: Partial<UserInput>)            => api.put<{id:number}>(`/users/${id}`, data),
  delete:      (id: number)                                  => api.delete<{id:number}>(`/users/${id}`),
  batchDelete: (ids: number[])                               => api.delete<{deleted:number, errors:Array<{id:number,error:string}>}>('/users/batch', {ids}),

  importExcel: (base64: string)                              => api.post<{total:number, created_users:number, created_voip:number, created_ptt:number, errors:Array<{row:number,sheet:string,error:string}>}>('/users/import', {file_base64: base64}),
  templateUrl: '/api/v1/users/import/template',

  addSub:     (pid: number, svc: 'call'|'ptt', sub: Partial<Subscription>)              => api.post<{id:string}>(`/users/${pid}/${svc}`, sub),
  updateSub:  (pid: number, svc: 'call'|'ptt', msisdn: string, data: Partial<Subscription>) => api.put<{id:string}>(`/users/${pid}/${svc}/${enc(msisdn)}`, data),
  deleteSub:  (pid: number, svc: 'call'|'ptt', msisdn: string)                          => api.delete<{id:string}>(`/users/${pid}/${svc}/${enc(msisdn)}`),

  getPttProfile:    (pid: number, msisdn: string)                        => api.get<McpttProfile & {id:string, exists:boolean}>(`/users/${pid}/ptt/${enc(msisdn)}/profile`),
  updatePttProfile: (pid: number, msisdn: string, data: McpttProfile)    => api.put<McpttProfile & {id:string}>(`/users/${pid}/ptt/${enc(msisdn)}/profile`, data),
}
