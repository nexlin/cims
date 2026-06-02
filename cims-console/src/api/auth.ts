import { api } from './client'
import type { Subscription } from './users'

// v3 (2026-04-22): 로그인과 프로파일/가입자 정보 분리.
//   /auth/login              → 인증 (token + 기본 user)
//   /users/me                → 프로파일 (role, org_id 등; Console admin 용)
//   /users/me/subscriptions  → 본인 VoIP/PTT 가입자 배열 (Phone UE 용)
// RBAC 역할 (계층적 5종) — docs/design/features/mcptt_authorization.md §3.
//   admin > manager > operator > monitor > user (user=OAM 로그인 불가, telephony 전용).
export type Role = 'admin' | 'manager' | 'operator' | 'monitor' | 'user'

export interface AuthUser {
  id: number
  name: string
  login_id: string
  role: Role
}

// /users/me 응답 (프로파일 상세)
export interface UserProfile extends AuthUser {
  org_id?: string | null
  create_time?: string | null
  update_time?: string | null
}

// /users/me/subscriptions 응답
export interface MySubscriptions {
  call_subscriptions: Subscription[]
  ptt_subscriptions:  Subscription[]
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
  me:             () => api.get<UserProfile>('/users/me'),
  mySubscriptions:() => api.get<MySubscriptions>('/users/me/subscriptions'),
  changePassword: (old_password: string, new_password: string) =>
    api.put<{ ok: boolean }>('/auth/password', { old_password, new_password }),
}
