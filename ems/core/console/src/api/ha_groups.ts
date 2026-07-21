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

// AS 절체 조건 (그룹/시스템 스코프) — keepalived vrrp_instance / vrrp_script 가 사용.
// 모듈별 값(프로세스 감시·절체 모드)은 ModuleSpec(패키지 설정)으로 이관됨.
export interface RestartLimit {
  max_fails: number                             // 연속 재기동 실패 임계 (default 3)
  window_sec: number                            // 카운트 윈도우 (default 300)
}

export interface FailoverOptions {
  advert_int: number                            // VRRP 주기 (sec, 0.5~5, default 1)
  health: {
    interval: number                            // default 2 (sec)
    fall: number                                // default 2 (회)
    rise: number                                // default 2 (회)
    timeout: number                             // default 3 (sec)
    grace_sec?: number                          // 승격 후 헬스 유예 (default 30)
    port?: number                               // 수동 오버라이드 — 미지정 시 배포 실효설정/descriptor 유도
    proto?: 'tcp' | 'udp'
  }
  track_interface: boolean                      // service NIC link down 즉시 감지 (default false)
  // 재기동 임계 (로컬 복구 소진 후 절체) — watchdog N회 실패 시 cims-health 가 FAULT.
  restart_limit?: RestartLimit
  preempt: 'preempt' | 'nopreempt'              // default nopreempt
  preempt_delay: number                         // preempt 모드만 적용 (sec, default 0)
}

export const FAILOVER_DEFAULTS: FailoverOptions = {
  advert_int: 1,
  health: { interval: 2, fall: 2, rise: 2, timeout: 3, grace_sec: 30 },
  track_interface: false,
  restart_limit: { max_fails: 3, window_sec: 300 },
  preempt: 'nopreempt',
  preempt_delay: 0,
}

// 모듈 운영 명세 (그룹×모듈 스코프) — agent 가 modules/<mod>/service.json 으로 받아
// watchdog·절체 게이팅에 사용. 앱 config.json 과 물리 분리. ha_service_model.md §3.
export type SafetyClass = 'stateless' | 'read_only' | 'shared_writer' | 'unknown'

export interface ModuleSpec {
  supervision: { watchdog: boolean }            // 프로세스 감시 on/off (default on)
  ha: {
    failover_mode: 'cold' | 'hot'               // cold: standby 정지+승격 시 기동 / hot: 양쪽 상시
    failover_relevant: boolean                  // 이 모듈 실패가 절체 사유인가 (default true)
  }
  health?: { port?: number; proto?: 'tcp' | 'udp'; config_key?: string; profile?: string }
  // 안전 등급 — shared_writer/unknown 은 자동 래치 해제 금지(수동). ha_service_model.md §14.
  safety?: { class: SafetyClass; latch_clear_mode?: 'auto' | 'manual' }
}

export const MODULE_SPEC_DEFAULT: ModuleSpec = {
  supervision: { watchdog: true },
  ha: { failover_mode: 'cold', failover_relevant: true },
  safety: { class: 'unknown', latch_clear_mode: 'manual' },
}

// 진행 중/최근 계획 절체 operation (그룹 응답의 failover_op)
export interface FailoverOp {
  id: number
  state: string                                 // RELEASING / WAIT_VIP_MOVE / VERIFYING / COMMITTED / ROLLED_BACK / FAILED …
  source_agent_id: number
  target_agent_id: number
  note?: string | null
  error?: string | null
  updated_at?: string
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
  // 서비스 의도 (선언적 무장) — {module: 'running'|'stopped'}. 무장 = running 모듈 존재.
  service_intent?: Record<string, 'running' | 'stopped'>
  // 모듈 운영 명세 (그룹×모듈) — {module: ModuleSpec}. 부재 모듈은 default.
  module_specs?: Record<string, ModuleSpec>
  // 진행 중/최근 계획 절체 operation (AS 만, 없으면 부재)
  failover_op?: FailoverOp
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
  service_intent?: Record<string, 'running' | 'stopped'>
  module_specs?: Record<string, ModuleSpec>
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

  // 그룹 일괄 제어 — 서비스 시작/중지/재시작 (의도 전환 + 순서 보장 job).
  control: (id: number, action: 'start' | 'stop' | 'restart') =>
    api.post<{ action: string; group_id: number; modules: string[]; jobs: number }>(
      `/ha-groups/${id}/control`, { action }),

  // 수동 계획 절체 (스위치오버) — AS 전용. operation 생성 후 sweep 이 구동(진행상태는
  // group.failover_op 로 폴링). 현 Active → Standby.
  failover: (id: number) =>
    api.post<{ group_id: number; operation_id: number; from_agent_id: number
               to_agent_id: number; state: string }>(
      `/ha-groups/${id}/failover`, {}),

  // 노드 유지보수(EXCLUDE_NODE) 토글 — AS 전용. 지정 멤버를 승격 대상에서 제외(on)/
  // 복귀(off). on → 그 노드 모듈 정지 + 절체 대상 제외, off → role 기반 자동 재합류.
  maintenance: (id: number, agentId: number, on: boolean) =>
    api.post<{ group_id: number; agent_id: number; service: string; maintenance: boolean }>(
      `/ha-groups/${id}/maintenance`, { agent_id: agentId, on }),

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
