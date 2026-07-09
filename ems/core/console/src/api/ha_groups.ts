import { api } from './client'

export type HaMode = 'active_standby' | 'all_active'
export type HaRole = 'master' | 'backup'

export interface HaMember {
  agent_id: number
  agent_name?: string
  priority: number
  role: HaRole
  // 실측 VIP 보유 (R4, AS 만) — heartbeat interfaces[] 관측.
  // true=보유(ACTIVE) / false=미보유 / null=판정 불가(heartbeat stale·VIP 미정의)
  vip_observed?: boolean | null
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

// AS 절체 조건 — keepalived vrrp_instance / vrrp_script 가 사용. AS 만 의미.
// default = 현재 hardcoded 동작과 동일 (호환성).
export interface FailoverOptions {
  advert_int: number                            // VRRP 주기 (sec, 0.5~5, default 1)
  health: {
    interval: number                            // default 2 (sec)
    fall: number                                // default 2 (회)
    rise: number                                // default 2 (회)
    timeout: number                             // default 3 (sec)
    port?: number                               // 수동 오버라이드 — 미지정 시 배포 실효설정/descriptor 유도
    proto?: 'tcp' | 'udp'
  }
  track_interface: boolean                      // service NIC link down 즉시 감지 (default false)
  tracked_modules: string[]                     // pgrep 검사 모듈 (default [] = port only)
  // 모듈별 절체 모드 — cold(기본): standby 정지 + MASTER 승격 시 기동 / hot: 양쪽 상시 기동.
  module_modes?: Record<string, 'cold' | 'hot'>
  preempt: 'preempt' | 'nopreempt'              // default nopreempt
  preempt_delay: number                         // preempt 모드만 적용 (sec, default 0)
}

export const FAILOVER_DEFAULTS: FailoverOptions = {
  advert_int: 1,
  health: { interval: 2, fall: 2, rise: 2, timeout: 3 },
  track_interface: false,
  tracked_modules: [],
  module_modes: {},
  preempt: 'nopreempt',
  preempt_delay: 0,
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
  failover_options?: FailoverOptions
  // 실측 ACTIVE (R4, AS 만) — 비-stale 멤버 중 정확히 1명이 VIP 보유일 때만 확정
  active_agent_id?: number | null
  // 패키지별 자동 동기화 스위치 — 부재 시 기본 ON (auto_sync[pkg] ?? true)
  auto_sync?: Record<string, boolean>
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
  failover_options?: FailoverOptions
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

  // ── 그룹×패키지 공통 설정 (R4) ──
  // 스위치 ON: target 없이 — 전 멤버 적용. OFF: target_deployment_id 필수(멤버 선택 편집).
  putGroupPkgConfig: (id: number, pkg: string, body: {
    values: Record<string, unknown>; target_deployment_id?: number; queue_update?: boolean
  }) =>
    api.put<{ ok: boolean; applied_keys: string[]; sync_on: boolean
              members: Array<{ deployment_id: number; agent_id: number; job_id: number }>
              sync_id: number | null }>(
      `/ha-groups/${id}/packages/${encodeURIComponent(pkg)}/config`, body),
  // 자동 동기화 스위치 — ON 전환 시 즉시 정합 1회 (결과 reconcile 에 요약)
  putGroupAutoSync: (id: number, pkg: string, enabled: boolean) =>
    api.put<{ ok: boolean; package: string; enabled: boolean
              reconcile: { status: string; reason: string | null
                           active_agent_id: number | null
                           synced_keys: string[]; removed_keys: string[]
                           deferred: Array<{ deployment_id: number; package_version: string | null }>
                           sync_id: number | null } | null }>(
      `/ha-groups/${id}/packages/${encodeURIComponent(pkg)}/auto-sync`, { enabled }),
}
