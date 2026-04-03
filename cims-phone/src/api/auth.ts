import { api } from './client'

export interface Subscription {
  id:            string   // MSISDN (E.164)
  auth_id:       string   // SIP IMPI (Digest auth ID)
  passwd:        string   // SIP Digest password
  dnd:           boolean
  forward_id:    string
  register_time?: string | null
  logout_time?:   string | null
}

export interface CimsUser {
  id:                 number
  name:               string
  login_id:           string
  role:               string
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
    api.get<CimsUser>('/auth/me'),
}
