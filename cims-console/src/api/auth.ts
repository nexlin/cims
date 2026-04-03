import { api } from './client'
import type { Subscription } from './users'

export interface AuthUser {
  id: number
  name: string
  login_id: string
  role: 'admin' | 'user'
  call_subscriptions: Subscription[]
  ptt_subscriptions: Subscription[]
}

interface AuthResponse {
  token: string
  user: AuthUser
}

export const authApi = {
  login:          (login_id: string, password: string) =>
    api.post<AuthResponse>('/auth/login', { login_id, password }),
  register:       (name: string, login_id: string, password: string) =>
    api.post<AuthResponse>('/auth/register', { name, login_id, password }),
  me:             () => api.get<AuthUser>('/auth/me'),
  changePassword: (old_password: string, new_password: string) =>
    api.put<{ ok: boolean }>('/auth/password', { old_password, new_password }),
}
