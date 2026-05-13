import { api } from './client'

export type HaMode = 'active_standby' | 'all_active'
export type HaRole = 'master' | 'backup'

export interface HaMember {
  agent_id: number
  agent_name?: string
  priority: number
  role: HaRole
}

// HaServicesPage 의 VIP slot binding — group 단위. 멤버별 iface 자동 매핑 + 수동 override
export interface VipBinding {
  bid: number
  slot: string                                  // 용도 (SIP / Admin / ...)
  ip: string
  mask?: number
  status?: 'up' | 'down' | 'unknown'
  memberIfaces?: { [serverId: number]: string } // 멤버 agent_id → iface name
}

export interface HaGroup {
  id: number
  name: string
  mode: HaMode
  vip: string | null                              // legacy 단일 VIP (Phase 2 부터 nullable, vip_bindings 권장)
  vrid: number
  vip_mask: number
  auth_pass: string
  note?: string
  vip_bindings?: VipBinding[]
  create_time?: string
  update_time?: string
  members: HaMember[]
}

export interface HaGroupInput {
  name: string
  mode: HaMode
  vip?: string                                    // optional — vip_bindings 가 권장
  vip_mask?: number
  auth_pass: string
  note?: string
  vip_bindings?: VipBinding[]
  members?: { agent_id: number; role?: HaRole; priority?: number }[]
}

export const haGroupsApi = {
  list:   ()                                => api.get<{ groups: HaGroup[] }>('/ha-groups').then(r => r.groups),
  get:    (id: number)                      => api.get<HaGroup>(`/ha-groups/${id}`),
  create: (data: HaGroupInput)              => api.post<{ id: number; vrid: number }>('/ha-groups', data),
  update: (id: number, data: Partial<HaGroupInput>) =>
    api.put<{ id: number }>(`/ha-groups/${id}`, data),
  delete: (id: number)                      => api.delete<{ id: number }>(`/ha-groups/${id}`),

  listMembers:  (id: number)                =>
    api.get<{ members: HaMember[] }>(`/ha-groups/${id}/members`).then(r => r.members),
  addMember:    (id: number, m: { agent_id: number; role?: HaRole; priority?: number }) =>
    api.post<{ group_id: number; agent_id: number }>(`/ha-groups/${id}/members`, m),
  removeMember: (id: number, agentId: number) =>
    api.delete<{ group_id: number; agent_id: number }>(`/ha-groups/${id}/members/${agentId}`),

  // VipPanel "[적용]" 진입점 — 데이터 변경 없이 멤버들에게 update_ha job 강제 큐잉
  apply: (id: number) =>
    api.post<{ group_id: number; jobs_queued: number }>(`/ha-groups/${id}/apply`, {}),
}
