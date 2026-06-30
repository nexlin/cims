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
}

// 가입자(person) — 콘솔 로그인 계정(login_id/password/role)은 별도(console_accounts).
export interface UserSummary {
  id: number          // person ID (auto-increment)
  name: string
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
  name: string; org_id: string; email?: string; details?: string; reject_id?: string[]
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
}
