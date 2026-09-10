import { api } from './client'
import { BUILTIN_ROLE_IDS } from './roles'

// 콘솔 로그인 계정 (OAM). DB users(가입자 person)와 분리된 file_store 도메인.
// role = roles.id (mcptt_authorization.md §2.1) — 내장 admin/manager/operator/monitor 또는 커스텀 role-….
//   후보는 GET /api/v1/roles 에서 고른다(api/roles.ts). telephony 전용 'user' 는 콘솔 계정이 될 수 없다.
export type ConsoleRole = string
// 역할 API(csc)가 없을 때의 폴백 후보 — 마이그레이션이 항상 시드하는 내장 4행
export const CONSOLE_ROLES: ConsoleRole[] = [...BUILTIN_ROLE_IDS]

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
