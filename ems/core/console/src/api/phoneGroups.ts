import { api } from './client'

// 전화 그룹 (phone group) — docs/design/features/dispatch_center.md §3.1·§8.2, docs/api/admin_api.md §6.7
//   전화 그룹 = 픽업 그룹 + (선택) 대표번호. 유선 전화의 일반 기능이며 관제 권한과 무관하다(권한은 역할 —
//   api/roles.ts). id(pg-xxxxxxxx, 전환 전 dg-… 유지) 가 곧 가입자 pickup_group 값이라 당겨받기·BLF·
//   대표번호 병렬 호출이 한 축을 공유한다. 멤버십이 SoT — 가입자 편집의 pickup_group 은 여기서 파생된다
//   (직접 편집 409 derived_from_phone_group). 가입자당 그룹 하나.

export type AlertMode = 'parallel' | 'sequential'
export type BusyMembers = 'skip' | 'alert'

export interface PhoneGroupMember {
  user_id: string       // 가입자(유선 회선) id (MSISDN) — 가입자당 그룹 하나. PTT 회선은 넣지 않는다(포크 대상이 된다)
  alert_order: number   // sequential 호출·MaxForkTargets 절삭 순서
}

export interface PhoneGroup {
  id: string                       // 불변 키 pg-xxxxxxxx (= *_subscriptions.pickup_group)
  name: string                     // 표시 이름 (키에 쓰지 않는다)
  pilot_id: string | null          // 대표번호 (AoR user part). null=대표번호 없음(순수 당겨받기 그룹)
  service_ref: string | null       // 대표번호 접속서비스 name (유선 VoIP) — 도메인·SRTP 정책·피처코드 근거
  alert_mode: AlertMode            // TS 24.239 Flexible Alerting — parallel / sequential
  no_answer_sec: number            // 전원 무응답 판정 초 (CSP Setup.Sip.Dispatch.ForkRingTimeoutSec 로 clamp)
  busy_members: BusyMembers        // 통화 중 그룹원 호출 여부
  overflow_target: string | null   // 무응답 넘김 대상(대표번호/가입 번호). null=480
  org_id: number | null            // 소속 조직 (콘솔 필터)
  members: PhoneGroupMember[]
  created_at?: string | null
}

export type PhoneGroupInput = Partial<Omit<PhoneGroup, 'members'>> & { members?: PhoneGroupMember[] }

// 목록 응답 — phone_groups 테이블 미적용 DB 는 groups=[] + schema='not_migrated'
export interface PhoneGroupList { groups: PhoneGroup[]; schema?: 'not_migrated' }

// 오류 토큰 (admin_api.md §6.7) — ApiError.data.error 로 분기한다
export const PHONE_GROUP_ERRORS: Record<string, string> = {
  pilot_conflict:       '대표번호가 가입 번호 또는 다른 대표번호와 겹칩니다',
  group_exists:         '같은 id 의 전화 그룹이 이미 있습니다',
  forbidden:            '권한이 없습니다 (directory_write)',
  schema_not_migrated:  'DB 에 phone_groups 테이블이 없습니다 — sql/migrate_phone_groups_roles.sql 적용 후 사용할 수 있습니다',
}

const base = '/phone-groups'
const enc = encodeURIComponent

export const phoneGroupsApi = {
  list:   (orgId?: number | null) =>
    api.get<PhoneGroupList>(orgId != null ? `${base}?org_id=${orgId}` : base),
  get:    (id: string)                          => api.get<PhoneGroup>(`${base}/${enc(id)}`),
  create: (data: PhoneGroupInput)               => api.post<{ id: string }>(base, data),
  update: (id: string, data: PhoneGroupInput)   => api.put<{ id: string }>(`${base}/${enc(id)}`, data),
  delete: (id: string)                          => api.delete<{ id: string }>(`${base}/${enc(id)}`),

  listMembers: (id: string) =>
    api.get<{ group_id: string; members: PhoneGroupMember[] } | PhoneGroupMember[]>(`${base}/${enc(id)}/members`)
      .then(r => Array.isArray(r) ? r : r.members),
  // 추가·이동 (다른 그룹 소속 가입자는 이동 — 응답 moved_from)
  addMember:   (id: string, m: { user_id: string; alert_order?: number }) =>
    api.post<{ group_id: string; user_id: string; moved_from: string | null }>(`${base}/${enc(id)}/members`, m),
  removeMember: (id: string, userId: string) =>
    api.delete<{ group_id: string; user_id: string }>(`${base}/${enc(id)}/members/${enc(userId)}`),
}
