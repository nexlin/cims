import { api } from './client'

export interface CallAuth {
  auth_id: string
  passwd?: string
  dnd: boolean
  forward_id: string
  register_time?: string | null
  logout_time?: string | null
}

export interface UserSummary {
  id: string
  name: string
  org_id: string
  details?: string | null
  has_call: boolean
  has_ptt: boolean
  reject_id: string[]
  create_time?: string | null
  update_time?: string | null
}

export interface UserDetail extends UserSummary {
  call_auth: CallAuth | null
  ptt_auth: CallAuth | null
}

export type UserInput = {
  id: string
  name: string
  org_id: string
  details?: string
  call_auth?: Partial<CallAuth> | null
  ptt_auth?: Partial<CallAuth> | null
  reject_id?: string[]
}

export const usersApi = {
  list:       ()                                     => api.get<{ users: UserSummary[] }>('/users').then(r => r.users),
  get:        (id: string)                           => api.get<UserDetail>(`/users/${encodeURIComponent(id)}`),
  create:     (data: UserInput)                      => api.post<{ id: string }>('/users', data),
  update:     (id: string, data: Partial<UserInput>) => api.put<{ id: string }>(`/users/${encodeURIComponent(id)}`, data),
  delete:     (id: string)                           => api.delete<{ id: string }>(`/users/${encodeURIComponent(id)}`),
  upsertCall: (id: string, auth: Partial<CallAuth>)  => api.post<{ id: string }>(`/users/${encodeURIComponent(id)}/call`, auth),
  deleteCall: (id: string)                           => api.delete<{ id: string }>(`/users/${encodeURIComponent(id)}/call`),
  upsertPtt:  (id: string, auth: Partial<CallAuth>)  => api.post<{ id: string }>(`/users/${encodeURIComponent(id)}/ptt`, auth),
  deletePtt:  (id: string)                           => api.delete<{ id: string }>(`/users/${encodeURIComponent(id)}/ptt`),
}
