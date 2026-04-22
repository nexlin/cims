import { api } from './client'

// v3 (2026-04-22): 로그인과 프로파일/가입자 정보 분리.
//   /auth/login              — 인증 (token + 기본 user)
//   /users/me                — 프로파일 (role 등)
//   /users/me/subscriptions  — 본인 VoIP/PTT 가입자 배열 (Phone UE 는 이 응답을 써서 SIP REGISTER)

export interface Subscription {
  id:            string   // MSISDN (E.164)
  service_ref:   string   // access_services.name
  imsi:          string
  domain:        string   // access_services.domain (CSC 가 조립하여 반환)
  auth_id:       string   // imsi@domain — Digest username (CSC 가 조립하여 반환)
  passwd:        string
  dnd:           boolean
  forward_id:    string
  register_time?: string | null
  logout_time?:   string | null
}

export interface CimsUser {
  id:        number
  name:      string
  login_id:  string
  role:      string
}

export interface MySubscriptions {
  call_subscriptions: Subscription[]
  ptt_subscriptions:  Subscription[]
}

interface AuthResponse {
  token: string
  user:  CimsUser
}

export const authApi = {
  login: (login_id: string, password: string) =>
    api.post<AuthResponse>('/auth/login', { login_id, password }),
  me: () =>
    api.get<CimsUser>('/users/me'),
  mySubscriptions: () =>
    api.get<MySubscriptions>('/users/me/subscriptions'),
}
