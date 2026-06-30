import { api } from './client'

export interface Member {
  user_id: string
  priority: number
  role?: 'chair' | 'participant'
  mcptt_id?: string | null
}

export interface Group {
  id: string                  // = mcptt_group_id (식별자)
  db_id?: number              // surrogate ptt_groups.id
  name: string
  members: Member[]
  priority?: number
  encryption?: boolean
  emergency_call?: boolean
  imminent_peril_call?: boolean   // allow-imminent-peril-call (TS 24.481)
  emergency_alert?: boolean        // allow-MCPTT-emergency-alert
  adhoc_enabled?: boolean          // ad hoc 그룹콜 허용 (Rel-18)
  video_enabled?: boolean
  org_code?: string
  session_start?: string | null
  session_end?: string | null
  // 3GPP MCPTT
  group_type?: 'prearranged' | 'chat' | 'broadcast'
  on_network?: boolean
  max_members?: number
  require_affiliation?: boolean
  alias?: string
  // 그룹 소유 (3GPP authorized user = 생성자 = 관리주체)
  authorized_user_id?: number | null      // 소유자 users.id
  authorized_user?: string | null         // 파생 MCPTT ID (tel:URI), 읽기전용
  authorized_user_name?: string | null    // 소유자 표시명, 읽기전용
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
