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

// AS 절체 조건 — keepalived vrrp_instance / vrrp_script 가 사용. AS 만 의미.
// default = 현재 hardcoded 동작과 동일 (호환성).
export interface FailoverOptions {
  advert_int: number                            // VRRP 주기 (sec, 0.5~5, default 1)
  health: {
    interval: number                            // default 2 (sec)
    fall: number                                // default 2 (회)
    rise: number                                // default 2 (회)
    timeout: number                             // default 3 (sec)
  }
  track_interface: boolean                      // service NIC link down 즉시 감지 (default false)
  tracked_modules: string[]                     // pgrep 검사 모듈 (default [] = port only)
  preempt: 'preempt' | 'nopreempt'              // default nopreempt
  preempt_delay: number                         // preempt 모드만 적용 (sec, default 0)
}

export const FAILOVER_DEFAULTS: FailoverOptions = {
  advert_int: 1,
  health: { interval: 2, fall: 2, rise: 2, timeout: 3 },
  track_interface: false,
  tracked_modules: [],
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
  // R2: 패키지별 설정 동기화 체크 상태 (deployments/{id}/config PUT 의 sync_checked 로
  // 백엔드가 기록 — ha-groups PUT 로는 갱신하지 않는 콘솔 메타)
  config_sync?: Record<string, string[]>
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
}
