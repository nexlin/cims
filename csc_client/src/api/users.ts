import { api } from './client'

export interface User {
  id: string
  auth_id: string
  passwd?: string
  org_id: string
  dnd: boolean
  forward_id: string
  reject_id: string[]
  create_time?: string
  update_time?: string
  register_time?: string
  logout_time?: string
}

export type UserInput = Omit<User, 'create_time' | 'update_time' | 'register_time' | 'logout_time'>

export const usersApi = {
  list:   ()                            => api.get<{ users: User[] }>('/users').then(r => r.users),
  get:    (id: string)                  => api.get<User>(`/users/${encodeURIComponent(id)}`),
  create: (data: UserInput)             => api.post<{ id: string }>('/users', data),
  update: (id: string, data: Partial<UserInput>) =>
    api.put<{ id: string }>(`/users/${encodeURIComponent(id)}`, data),
  delete: (id: string)                  => api.delete<{ id: string }>(`/users/${encodeURIComponent(id)}`),
}
