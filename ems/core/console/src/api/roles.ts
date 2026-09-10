import { api } from './client'

// 역할 (role) = 능력 + 범위 — docs/design/features/mcptt_authorization.md §2.2·§3, docs/api/admin_api.md §6.8
//   내장 프리셋 4행(admin/manager/operator/monitor, builtin=1, 읽기 전용)과 관제 프리셋(감독/관리/전체)으로
//   만드는 커스텀 역할이 같은 엔티티다. 사람은 역할 하나에 배정된다 — 콘솔 계정은 OAM console_accounts.role
//   (= roles.id, PUT /console-accounts/{login_id}), 가입자(person)는 role_assignments(user, users.id).
//   전부 authz_manage(콘솔 manager 이상) — 관제 앱에는 이 API 가 없다.

export type DirectoryScope = 'none' | 'own' | 'all'            // own = org_id 조직과 그 하위
export type PttGroupManage = 'none' | 'own' | 'scope' | 'all'  // own=본인 소유, scope=directory_write 범위
export type MonitorCall = 'none' | 'own' | 'listed' | 'all'    // 통화 감청·세션 관측·통화 이력/녹취 범위 (전화 그룹 단위)
export type PttListen = 'none' | 'listed' | 'all'              // PTT 청취·conference 구독·PTT 이력/녹취 범위 (PTT 그룹 단위)
export type ListenVisibility = 'hidden' | 'visible'            // PTT 청취 멤버 로스터 노출
export type HistoryRead = 'none' | 'scope' | 'all'             // scope = monitor_call/ptt_listen 범위

// 내장 프리셋 id — 마이그레이션이 항상 시드한다. 콘솔 계정의 role 기본 후보(역할 API 가 없을 때 폴백).
export const BUILTIN_ROLE_IDS = ['admin', 'manager', 'operator', 'monitor'] as const
export type BuiltinRoleId = typeof BUILTIN_ROLE_IDS[number]

// 관제 프리셋 — 역할 생성 시 초깃값 (mcptt_authorization.md §3 표). 저장은 개별 필드다.
export type RolePreset = 'supervisor' | 'admin' | 'full'
export const ROLE_PRESET_LABELS: Record<RolePreset, string> = { supervisor: '감독', admin: '관리', full: '전체' }

export interface RoleDef {
  id: string                       // 불변 키 — 내장 admin|manager|operator|monitor, 커스텀 role-xxxxxxxx
  name: string
  builtin: boolean                 // 내장 프리셋(읽기 전용)
  authz_manage: boolean            // 역할·배정·범위 관리 — 내장 admin/manager 만 (커스텀 400 not_delegable)
  audit_read: boolean              // E-AUD 감사 이벤트 열람 — 수행(monitor_call/ptt_listen)과 분리
  directory_write: DirectoryScope  // 조직·구성원·번호·전화 그룹 쓰기
  directory_read: DirectoryScope
  ptt_group_manage: PttGroupManage
  monitor_call: MonitorCall
  ptt_listen: PttListen
  listen_visibility: ListenVisibility
  history_read: HistoryRead
  alarm_ack: boolean
  mcptt_control: boolean
  org_id: number | null            // own 범위의 루트
  // 목록 응답 동봉 (admin_api.md §6.8 "대상·배정 수 포함")
  monitor_targets?: string[]       // monitor_call=listed 대상 전화 그룹 id
  ptt_targets?: string[]           // ptt_listen=listed 대상 PTT 그룹 (mcptt_group_id)
  assignment_count?: number
  created_at?: string | null
}

// 생성/갱신 본문 — 커스텀에 켤 수 없는 4 능력(authz_manage/audit_read/alarm_ack/mcptt_control)은 보내지 않는다
export type RoleInput = Partial<Pick<RoleDef,
  'id' | 'name' | 'directory_write' | 'directory_read' | 'ptt_group_manage' | 'monitor_call' | 'ptt_listen' |
  'listen_visibility' | 'history_read' | 'org_id'>> & { preset?: RolePreset }

export type PrincipalType = 'console' | 'user'   // console=OAM 콘솔 계정(login_id), user=가입자 person(users.id)
export interface RoleAssignment {
  principal_type: PrincipalType
  principal_id: string
  name?: string
}

// 목록 응답 — roles 테이블 미적용 DB 는 roles=[] + schema='not_migrated'
export interface RoleList { roles: RoleDef[]; schema?: 'not_migrated' }

// 오류 토큰 (admin_api.md §6.8) — ApiError.data.error 로 분기한다
export const ROLE_ERRORS: Record<string, string> = {
  not_delegable:        'authz_manage·audit_read·alarm_ack·mcptt_control 은 커스텀 역할에 켤 수 없습니다',
  builtin:              '내장 역할은 읽기 전용입니다',
  assigned:             '배정이 남아 있는 역할은 삭제할 수 없습니다 — 배정을 먼저 해제하세요',
  forbidden:            '권한이 없습니다 (authz_manage — manager 이상)',
  schema_not_migrated:  'DB 에 roles 테이블이 없습니다 — sql/migrate_phone_groups_roles.sql 적용 후 사용할 수 있습니다',
}

const base = '/roles'
const enc = encodeURIComponent

export const rolesApi = {
  list: () =>
    api.get<RoleList | RoleDef[]>(base).then(r => Array.isArray(r) ? { roles: r } as RoleList : r),
  get:    (id: string)                    => api.get<RoleDef>(`${base}/${enc(id)}`),
  create: (data: RoleInput)               => api.post<{ id: string }>(base, data),
  update: (id: string, data: RoleInput)   => api.put<{ id: string }>(`${base}/${enc(id)}`, data),
  delete: (id: string)                    => api.delete<{ id: string }>(`${base}/${enc(id)}`),

  setMonitorTargets: (id: string, phoneGroupIds: string[]) =>
    api.put<{ id: string; phone_group_ids: string[] }>(`${base}/${enc(id)}/monitor-targets`, { phone_group_ids: phoneGroupIds }),
  setPttTargets: (id: string, pttGroupIds: string[]) =>
    api.put<{ id: string; ptt_group_ids: string[] }>(`${base}/${enc(id)}/ptt-targets`, { ptt_group_ids: pttGroupIds }),

  listAssignments: (id: string) =>
    api.get<RoleAssignment[] | { assignments: RoleAssignment[] }>(`${base}/${enc(id)}/assignments`)
      .then(r => Array.isArray(r) ? r : r.assignments),
  // 가입자(person) 배정 — 사람당 역할 하나(다른 역할에서 이동, 응답 moved_from).
  //   콘솔 계정 배정은 OAM PUT /console-accounts/{login_id} role (api/consoleAccounts.ts).
  assignUser: (id: string, personId: number | string) =>
    api.put<{ role_id: string; principal_type: 'user'; principal_id: string; moved_from: string | null }>(
      `${base}/${enc(id)}/assignments`, { principal_type: 'user', principal_id: String(personId) }),
  unassign: (id: string, principalType: PrincipalType, principalId: string) =>
    api.delete<{ role_id: string }>(`${base}/${enc(id)}/assignments/${principalType}/${enc(principalId)}`),
}
