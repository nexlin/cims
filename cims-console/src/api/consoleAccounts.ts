import { api } from './client'
import type { Role } from './auth'

// 콘솔 로그인 계정 (OAM). DB users(가입자 person)와 분리된 file_store 도메인.
// telephony 전용 'user' 는 콘솔 계정이 될 수 없음 → admin/manager/operator/monitor.
export type ConsoleRole = Exclude<Role, 'user'>
export const CONSOLE_ROLES: ConsoleRole[] = ['admin', 'manager', 'operator', 'monitor']

export interface ConsoleAccount {
  login_id: string
  name: string
  role: ConsoleRole
  email?: string
  create_time?: string | null
  update_time?: string | null
}

export type ConsoleAccountCreate = {
  login_id: string; name: string; role: ConsoleRole; password: string; email?: string
}
export type ConsoleAccountUpdate = { name?: string; role?: ConsoleRole; email?: string }

const enc = (s: string) => encodeURIComponent(s)

export const consoleAccountsApi = {
  list:   ()                          => api.get<{ items: ConsoleAccount[] }>('/console-accounts').then(r => r.items),
  create: (data: ConsoleAccountCreate) => api.post<ConsoleAccount>('/console-accounts', data),
  update: (loginId: string, data: ConsoleAccountUpdate) => api.put<ConsoleAccount>(`/console-accounts/${enc(loginId)}`, data),
  delete: (loginId: string)           => api.delete<{ ok: boolean }>(`/console-accounts/${enc(loginId)}`),
  setPassword: (loginId: string, newPassword: string) =>
    api.put<{ ok: boolean }>(`/console-accounts/${enc(loginId)}/password`, { new_password: newPassword }),
}
