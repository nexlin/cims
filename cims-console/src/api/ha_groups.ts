import { api } from './client'

export type HaMode = 'active_standby' | 'all_active'
export type HaRole = 'master' | 'backup'

export interface HaMember {
  agent_id: number
  agent_name?: string
  priority: number
  role: HaRole
}

export interface HaGroup {
  id: number
  name: string
  mode: HaMode
  vip: string
  vrid: number
  vip_mask: number
  auth_pass: string
  note?: string
  create_time?: string
  update_time?: string
  members: HaMember[]
}

export interface HaGroupInput {
  name: string
  mode: HaMode
  vip: string
  vip_mask?: number
  auth_pass: string
  note?: string
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
}
