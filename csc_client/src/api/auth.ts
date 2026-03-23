import { api } from './client'
import type { Subscription } from './users'

export interface AuthUser {
  id: number
  name: string
  email: string
  role: 'admin' | 'user'
  call_subscriptions: Subscription[]
  ptt_subscriptions: Subscription[]
}

interface AuthResponse {
  token: string
  user: AuthUser
}

export const authApi = {
  login:          (email: string, password: string) =>
    api.post<AuthResponse>('/auth/login', { email, password }),
  register:       (name: string, email: string, password: string) =>
    api.post<AuthResponse>('/auth/register', { name, email, password }),
  me:             () => api.get<AuthUser>('/auth/me'),
  changePassword: (old_password: string, new_password: string) =>
    api.put<{ ok: boolean }>('/auth/password', { old_password, new_password }),
}
