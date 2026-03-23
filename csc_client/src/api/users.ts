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
  id: string          // person ID
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
  id: string; name: string; org_id: string; details?: string; reject_id?: string[]
}

const enc = (s: string) => encodeURIComponent(s)

export const usersApi = {
  list:   ()                                                => api.get<{users: UserSummary[]}>('/users').then(r => r.users),
  get:    (id: string)                                      => api.get<UserDetail>(`/users/${enc(id)}`),
  create: (data: UserInput)                                 => api.post<{id:string}>('/users', data),
  update: (id: string, data: Partial<UserInput>)            => api.put<{id:string}>(`/users/${enc(id)}`, data),
  delete: (id: string)                                      => api.delete<{id:string}>(`/users/${enc(id)}`),

  listSubs:   (pid: string, svc: 'call'|'ptt')                                          => api.get<{subscriptions: Subscription[]}>(`/users/${enc(pid)}/${svc}`).then(r => r.subscriptions),
  addSub:     (pid: string, svc: 'call'|'ptt', sub: Partial<Subscription>)              => api.post<{id:string}>(`/users/${enc(pid)}/${svc}`, sub),
  updateSub:  (pid: string, svc: 'call'|'ptt', msisdn: string, data: Partial<Subscription>) => api.put<{id:string}>(`/users/${enc(pid)}/${svc}/${enc(msisdn)}`, data),
  deleteSub:  (pid: string, svc: 'call'|'ptt', msisdn: string)                          => api.delete<{id:string}>(`/users/${enc(pid)}/${svc}/${enc(msisdn)}`),
}
