import { api } from './client'

export interface Member {
  user_id: string
  priority: number
  role?: 'chair' | 'participant'
  mcptt_id?: string | null
  required?: boolean           // <on-network-required> 필수 멤버 — 개시자 응답 전에 이 멤버의 응답을 기다린다(TNG1)
}

export interface Group {
  id: string                  // = mcptt_group_id (식별자)
  db_id?: number              // surrogate ptt_groups.id
  name: string
  members: Member[]
  priority?: number
  encryption?: boolean
  emergency_call?: boolean         // allow-MCPTT-emergency-call — condition(긴급·임박) 공통 게이트
  emergency_alert?: boolean        // allow-MCPTT-emergency-alert
  allow_conference_state?: boolean // on-network-allow-conference-state — 멤버의 conference 이벤트(RFC 4575) 구독 허용
  allow_sds?: boolean              // mcdata-allow-short-data-service (그룹 SDS 메시징, TS 24.481)
  allow_fd?: boolean               // mcdata-allow-file-distribution (그룹 파일전송)
  max_sds_size?: number            // mcdata-on-network-max-data-size-for-SDS (octets, 0=무제한)
  max_auto_recv?: number           // mcdata-on-network-max-data-size-auto-recv (octets)
  video_enabled?: boolean
  org_code?: string
  session_start?: string | null
  session_end?: string | null
  // floor 동시 발언 정책 (CSP → CMP PTT_GROUP_ADD, 계약 §B.1)
  floor_policy?: 'single' | 'dual' | 'multi'   // single=한 명, dual=2명, multi=max_talkers 명
  max_talkers?: number                         // multi 일 때만 유효 (2~8, CMP 슬롯 상한)
  // 3GPP MCPTT
  group_type?: 'prearranged' | 'chat'          // on-network-invite-members (일제 통화는 그룹 종류가 아니라 호 속성)
  hang_timer_sec?: number                      // on-network-hang-timer — 그룹 호 T4, 발언 없이 이 시간이 지나면 해제 (0=미사용)
  max_duration_sec?: number                    // on-network-maximum-duration — 그룹 호 최대 시간 TNG3 (0=무제한)
  // 확인 통화 설정 (TS 24.481 §7.2.2 s)t)u), TS 24.379 §6.3.3.3·§10.1.1.4.2)
  min_number_to_start?: number                 // on-network-minimum-number-to-start — 개시자 응답 전 멤버 응답 수 (0=기다리지 않음)
  ack_timeout_sec?: number                     // on-network-timeout-for-acknowledgement-of-required-members — TNG1
  ack_action?: 'proceed' | 'abandon'           // TNG1 만료·필수 멤버 거절 때 진행(Warning 111) / 포기(480 + Warning 112)
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
