import { api } from './client'

export interface Member {
  user_id: string
  priority: number
  role?: 'chair' | 'participant'
  mcptt_id?: string | null
  required?: boolean           // <on-network-required> 필수 멤버 — 개시자 응답 전에 이 멤버의 응답을 기다린다(TNG1)
  implicit_affiliation?: boolean  // user profile <ImplicitAffiliations> — 등록(서비스 인가) 때 서버가 이 그룹에 자동 제휴(TS 24.379 §9.2.2.2.15)
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
  // 옛 «PTT 영상»(MCPTT 호에 m=video) — V7 전 CSC 만 응답에 싣는다. V7 CSC 는 키가 없고 받아도 무시한다(mcvideo.md §8).
  video_enabled?: boolean
  // MCVideo 서비스 (TS 24.481 §7.2.2 — 한 그룹 = 서비스 집합, TS 23.280 §3). null/없음 = MCVideo 그룹 아님.
  //   쓰기: 키 없음 = 그대로 · null = 끔 · 객체 = 켬/갱신(준 키만) — admin_api.md §6.
  mcvideo?: McVideoGroupAttrs | null
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

// 그룹 문서 MCVideo 몫 (mcvideo-* 요소 — TS 24.481 §7.2.2, CSC services/mcvideo.py GROUP_ATTR_DEFAULTS 와 같은 키)
export interface McVideoGroupAttrs {
  invite_members: boolean                 // mcvideo-on-network-invite-members — true = prearranged(제휴 멤버 초대), false = chat
  max_duration_sec: number                // mcvideo-on-network-maximum-duration — TNG3 (0~86400, 0 = 무제한)
  max_transmitters: number                // mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members (1~16)
  audio_encodings: string[]               // mcvideo-preferred-audio-encodings (rtpmap 이름)
  video_encodings: string[]               // mcvideo-preferred-video-encodings
  video_resolutions: string | null        // mcvideo-preferred-video-resolutions — null = 요소 생략
  video_frame_rate: string | null         // mcvideo-preferred-video-frame-rate — null = 요소 생략
  reception_hang_timer_sec: number        // on-network-reception-hang-timer — T5 (TS 24.581 §11.1.3, 0~3600)
  min_number_to_start: number             // mcvideo-on-network-minimum-number-to-start
  group_priority: number | null           // mcvideo-on-network-group-priority (0~255) — null = 생략(가장 낮음)
  protect_media: boolean                  // mcvideo-protect-media — E2E(GMK) 전에는 false 만
  protect_transmission_control: boolean   // mcvideo-protect-transmission-control — 같음
  allow_conference_state: boolean         // mcvideo-on-network-allow-conference-state
}

/** MCVideo 를 켤 때 쓰는 기본값 — CSC GROUP_ATTR_DEFAULTS 와 같은 값. */
export const MCVIDEO_GROUP_DEFAULTS: McVideoGroupAttrs = {
  invite_members: false, max_duration_sec: 3600, max_transmitters: 2, audio_encodings: ['AMR-WB'], video_encodings: ['H264'],
  video_resolutions: null, video_frame_rate: null, reception_hang_timer_sec: 30, min_number_to_start: 0, group_priority: null,
  protect_media: false, protect_transmission_control: false, allow_conference_state: true,
}

// 쓰기 본문 — mcvideo 는 준 키만 바꾼다(부분 객체), null = 끔
export type GroupInput = Omit<Group, 'members' | 'mcvideo'> & { members?: Member[]; mcvideo?: Partial<McVideoGroupAttrs> | null }

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
