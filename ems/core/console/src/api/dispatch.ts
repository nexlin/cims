import { api } from './client'

// 관제 그룹 (dispatch group) — docs/design/features/dispatch_center.md §3·§8.2
//   관제 그룹 = 픽업 그룹 + (선택) 대표번호 + (선택) 감청 범위. id(dg-xxxxxxxx) 가 곧 가입자
//   pickup_group 값이라 당겨받기·BLF 인가·대표번호 병렬 호출·감청 범위가 한 축을 공유한다.

export type AlertMode = 'parallel' | 'sequential'
export type BusyMembers = 'skip' | 'alert'
export type MonitorScope = 'none' | 'own' | 'listed' | 'all'
export type PttListen = 'none' | 'listed' | 'all'
export type ListenVisibility = 'hidden' | 'visible'

export interface DispatchMember {
  user_id: string       // 가입자 id (MSISDN) — 가입자당 그룹 하나
  alert_order: number   // sequential 호출·MaxForkTargets 절삭 순서
}

export interface DispatchGroup {
  id: string                       // 불변 키 dg-xxxxxxxx (= volte_subscriptions.pickup_group)
  name: string                     // 표시 이름 (키에 쓰지 않는다)
  pilot_id: string | null          // 대표번호 (AoR user part). null=대표번호 없음(순수 당겨받기 그룹)
  service_ref: string | null       // 대표번호 접속서비스 name — 도메인·SRTP 정책 근거
  alert_mode: AlertMode            // TS 24.239 Flexible Alerting (sequential 상태기는 후속)
  no_answer_sec: number            // 전원 무응답 판정 초 (CSP Setup.Sip.Dispatch.ForkRingTimeoutSec 로 clamp)
  busy_members: BusyMembers        // 통화 중 그룹원 호출 여부
  overflow_target: string | null   // 무응답 넘김 대상(대표번호/내선). null=480
  monitor_scope: MonitorScope      // 합법감청(dialog 감시·Join) 범위 — manager 만 변경
  ptt_listen: PttListen            // PTT 그룹콜 청취 범위 — manager 만 변경
  listen_visibility: ListenVisibility  // PTT 청취 멤버 로스터 노출
  org_id: number | null
  members: DispatchMember[]
  monitor_targets: string[]        // monitor_scope=listed 의 대상 그룹 id
  ptt_targets: string[]            // ptt_listen=listed 의 대상 PTT 그룹 (mcptt_group_id)
  created_at?: string | null
  updated_at?: string | null
}

export type DispatchGroupInput = Partial<Omit<DispatchGroup, 'members' | 'monitor_targets' | 'ptt_targets'>> & {
  members?: DispatchMember[]
}

// 목록 응답 — dispatch_groups 테이블 미적용 DB 는 groups=[] + schema='not_migrated'
export interface DispatchGroupList { groups: DispatchGroup[]; schema?: 'not_migrated' }

const base = '/dispatch-groups'
const enc = encodeURIComponent

export const dispatchApi = {
  list:   (orgId?: number | null) =>
    api.get<DispatchGroupList>(orgId != null ? `${base}?org_id=${orgId}` : base),
  get:    (id: string)                            => api.get<DispatchGroup>(`${base}/${enc(id)}`),
  create: (data: DispatchGroupInput)              => api.post<{ id: string }>(base, data),
  update: (id: string, data: DispatchGroupInput)  => api.put<{ id: string }>(`${base}/${enc(id)}`, data),
  delete: (id: string)                            => api.delete<{ id: string }>(`${base}/${enc(id)}`),

  listMembers: (id: string) =>
    api.get<{ group_id: string; members: DispatchMember[] }>(`${base}/${enc(id)}/members`).then(r => r.members),
  addMember:   (id: string, m: DispatchMember) =>
    api.post<{ group_id: string; user_id: string; moved_from: string | null }>(`${base}/${enc(id)}/members`, m),
  removeMember: (id: string, userId: string) =>
    api.delete<{ group_id: string; user_id: string }>(`${base}/${enc(id)}/members/${enc(userId)}`),

  setMonitorTargets: (id: string, targetGroupIds: string[]) =>
    api.put<{ group_id: string; target_group_ids: string[] }>(`${base}/${enc(id)}/monitor-targets`,
      { target_group_ids: targetGroupIds }),
  setPttTargets: (id: string, pttGroupIds: string[]) =>
    api.put<{ group_id: string; ptt_group_ids: string[] }>(`${base}/${enc(id)}/ptt-targets`,
      { ptt_group_ids: pttGroupIds }),
}
