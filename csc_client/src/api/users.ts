import { api } from './client'

export interface Subscription {
  id: string          // MSISDN of this line
  auth_id: string
  passwd?: string
  dnd: boolean
  forward_id: string
  register_time?: string | null
  logout_time?: string | null
}

export interface UserSummary {
  id: number          // person ID (auto-increment)
  name: string
  org_id: string
  details?: string | null
  call_count: number
  ptt_count: number
  reject_id: string[]
  create_time?: string | null
  update_time?: string | null
}

export interface UserDetail extends Omit<UserSummary, 'call_count' | 'ptt_count'> {
  call_subscriptions: Subscription[]
  ptt_subscriptions: Subscription[]
}

export type UserInput = {
  name: string; org_id: string; details?: string; reject_id?: string[]
}

const enc = (s: string) => encodeURIComponent(s)

export const usersApi = {
  list:   ()                                                => api.get<{users: UserSummary[]}>('/users').then(r => r.users),
  get:    (id: number)                                      => api.get<UserDetail>(`/users/${id}`),
  create: (data: UserInput)                                 => api.post<{id:number}>('/users', data),
  update: (id: number, data: Partial<UserInput>)            => api.put<{id:number}>(`/users/${id}`, data),
  delete: (id: number)                                      => api.delete<{id:number}>(`/users/${id}`),

  listSubs:   (pid: number, svc: 'call'|'ptt')                                          => api.get<{subscriptions: Subscription[]}>(`/users/${pid}/${svc}`).then(r => r.subscriptions),
  addSub:     (pid: number, svc: 'call'|'ptt', sub: Partial<Subscription>)              => api.post<{id:string}>(`/users/${pid}/${svc}`, sub),
  updateSub:  (pid: number, svc: 'call'|'ptt', msisdn: string, data: Partial<Subscription>) => api.put<{id:string}>(`/users/${pid}/${svc}/${enc(msisdn)}`, data),
  deleteSub:  (pid: number, svc: 'call'|'ptt', msisdn: string)                          => api.delete<{id:string}>(`/users/${pid}/${svc}/${enc(msisdn)}`),
}
