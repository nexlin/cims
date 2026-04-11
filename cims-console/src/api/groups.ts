import { api } from './client'

export interface Member {
  user_id: string
  priority: number
}

export interface Group {
  id: string
  name: string
  members: Member[]
  priority?: number
  encryption?: boolean
  emergency_call?: boolean
  video_enabled?: boolean
  org_code?: string
  session_start?: string | null
  session_end?: string | null
}

export type GroupInput = Omit<Group, 'members'> & { members?: Member[] }

export const groupsApi = {
  list:   ()                                  => api.get<{ groups: Group[] }>('/ptt/groups').then(r => r.groups),
  get:    (id: string)                        => api.get<Group>(`/ptt/groups/${encodeURIComponent(id)}`),
  create: (data: GroupInput)                  => api.post<{ id: string }>('/ptt/groups', data),
  update: (id: string, data: Partial<GroupInput>) =>
    api.put<{ id: string }>(`/ptt/groups/${encodeURIComponent(id)}`, data),
  delete: (id: string)                        => api.delete<{ id: string }>(`/ptt/groups/${encodeURIComponent(id)}`),

  listMembers: (groupId: string)              =>
    api.get<{ group_id: string; members: Member[] }>(`/ptt/groups/${encodeURIComponent(groupId)}/members`)
      .then(r => r.members),
  addMember:   (groupId: string, m: Member)  =>
    api.post<{ group_id: string; user_id: string }>(`/ptt/groups/${encodeURIComponent(groupId)}/members`, m),
  removeMember: (groupId: string, userId: string) =>
    api.delete<{ group_id: string; user_id: string }>(
      `/ptt/groups/${encodeURIComponent(groupId)}/members/${encodeURIComponent(userId)}`
    ),
}
