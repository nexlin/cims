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

// 그룹 공통 마운트 — agent.mounts 와 같은 모양 (mounted 는 멤버별이라 여기 없음).
export interface GroupMount {
  target: string                                // 마운트 위치 (예: /mnt/cims-log)
  source: string                                // 예: 121.161.164.105:/home/cbm/NAS/log
  fstype: string                                // nfs | nfs4 | cifs | ext4 | ...
  options?: string
}

export type MountOp = {
  op: 'add' | 'del'
  target: string
  fstype?: string
  source?: string
  options?: string
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

// 공유 store — 관리평면 store 가 놓인 **공유 마운트 지점**(NAS). 그룹 스코프이며 양 노드가
// 같은 경로를 상시 마운트한다. agent 는 마운트를 조작하지 않고, 승격 전 그 경로가 실제
// 마운트이고 write 가능한지 확인만 한다. 마운트 생성·영속(fstab)은 서버별 마운트 관리 담당.
// 상세: docs/design/features/oam_ha.md §4
export interface HaSharedStore {
  mount_point: string       // 절대경로 (CimsRuntimeMount 와 동일해야 함)
}

// 그룹×패키지 공통(service) 설정 정합 판정 — 서버(evaluate_group_package)가 자동 교정과
// 동일한 규칙으로 낸 결과. 콘솔은 이걸 그리기만 한다.
//   status  in_sync=정합 / out_of_sync=드리프트 / unknown=판정 불가(reason 참조)
//   reason  active_unknown(ACTIVE 미확정)·version_mismatch(버전 혼재)·no_peers 등
//   action  copy=ACTIVE 값 복사 / reset=overlay 제거(템플릿 기본값 복귀)
//   auto_sync  false 면 교정은 멈춰 있다 (드리프트가 있어도 스스로 해소되지 않음)
export interface GroupPkgSyncDrift {
  key: string
  action: 'copy' | 'reset'
  active: unknown
  members: Array<{
    deployment_id: number; agent_id: number; agent_name: string | null
    value: unknown; present: boolean
  }>
}

// 표시용 실효값 — 렌더 결과(overlay + 템플릿 기본값 + 배포 시 주입). 화면이 overlay 만
// 보고 그리면 주입 값(JwtSecret 등)이 빈칸으로 보이고, 판정(overlay 기준)과 표시 기준이
// 달라 "값이 같은데 드리프트"가 된다. src 가 그 차이를 드러낸다.
export interface GroupPkgSyncMember {
  deployment_id: number
  agent_id: number
  agent_name: string | null
  package_version: string | null
  values: Record<string, { v: unknown; src: 'overlay' | 'injected' | 'default' }>
}

export interface GroupPkgSync {
  group_id: number
  package: string
  auto_sync: boolean
  status: 'in_sync' | 'out_of_sync' | 'unknown'
  reason: string | null
  active_agent_id: number | null
  compared_to: {
    deployment_id: number; agent_id: number
    agent_name: string | null; package_version: string | null
  } | null
  drift: GroupPkgSyncDrift[]
  deferred: Array<{ deployment_id: number; package_version: string | null }>
  members: GroupPkgSyncMember[]
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
  // 그룹 공통 마운트 선언 (전 멤버 동일 — 모듈 로그 수집용 NAS 등). 멤버별 실제 적용
  // 여부는 agent.mounts 와 대조해 판정한다(선언에 있는데 멤버에 없으면 미적용).
  mounts?: GroupMount[]
  shared_store?: HaSharedStore                    // 공유 store (미설정 = 이중화 대상 아님)
  // 선언(`requires_leader_lease`) 전제 미충족으로 **HA 편입에서 제외된** 모듈과 사유.
  // 예: {oam: 'no_shared_store'} — 공유 store 가 없으면 관리평면은 절체 대상이 아니다.
  ha_excluded?: Record<string, string>
  /** VIP 가 아닌 주소로 OAM 에 보고하는 agent — 이대로 절체하면 fleet 이 단절된다. */
  agents_not_on_vip?: Array<{ agent_id: number; name: string; oam_url: string }>
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
  // shared_store 는 입력이 아니다 — oam/oam-svc 배포설정(`CimsRuntimeMount`)에서 유도되고
  // PUT 으로 보내면 400 `shared_store_not_group_scoped` 다. 응답의 값은 읽기 전용 유도값.
  /** 그룹 공통 마운트 선언 — 멤버 적용 여부는 agent.mounts 로 대조. */
  mounts?: GroupMount[]
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
  // 관리 store 를 공유 마운트로 이관 — 그룹 shared_store 저장 + oam 배포설정 갱신 +
  // store 를 들고 있는 노드에 이관 job(정지→복사→기동)까지 한 번에. 콘솔이 잠깐 끊긴다.
  migrateSharedStore: (id: number, mount_point: string) =>
    api.post<{
      shared_store: HaSharedStore; runtime_dir: string; detail: string
      jobs: { agent_id: number; process_name: string; job_type: string; job_id: number }[]
    }>(`/ha-groups/${id}/shared-store/migrate`, { mount_point }),

  // 그룹 공통 마운트 — 선언 갱신 + 전 멤버 fan-out 적용. 오프라인/실패 멤버가 있어도
  // 선언은 갱신되고 results 로 개별 사유가 온다 (콘솔이 '미적용'으로 표시 → 재적용).
  applyMounts: (id: number, mounts: MountOp[]) =>
    api.post<{
      group_id: number; mounts: number; applied: number
      results: Array<{ agent_id: number; name: string; ok: boolean; rc?: number; error?: string }>
    }>(`/ha-groups/${id}/apply-mounts`, { mounts }),

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
  // force: agent 주소가 VIP 가 아닐 때의 사전 점검(409 agents_not_on_vip)을 우회.
  failover: (id: number, force = false) =>
    api.post<{ group_id: number; operation_id: number; from_agent_id: number
               to_agent_id: number; state: string }>(
      `/ha-groups/${id}/failover`, force ? { force: true } : {}),

  // 노드 유지보수(EXCLUDE_NODE) 토글 — AS 전용. 지정 멤버를 승격 대상에서 제외(on)/
  // 복귀(off). on → 그 노드 모듈 정지 + 절체 대상 제외, off → role 기반 자동 재합류.
  maintenance: (id: number, agentId: number, on: boolean) =>
    api.post<{ group_id: number; agent_id: number; service: string; maintenance: boolean }>(
      `/ha-groups/${id}/maintenance`, { agent_id: agentId, on }),

  // ── 그룹×패키지 공통 설정 (R4) ──
  // 정합 상태 조회 — 드리프트 판정은 **서버 소유**다. 멤버별 설정을 받아 화면에서 직접
  // 비교하지 말 것: 자동 교정 데몬과 판정이 갈라져 실제로 교정되지 않을 것을 "교정 대기"로
  // 표시하게 된다 (다른 패키지의 설정을 새 템플릿에 얹어 세던 유령 드리프트가 그 사례).
  getGroupPkgSync: (id: number, pkg: string) =>
    api.get<GroupPkgSync>(`/ha-groups/${id}/packages/${encodeURIComponent(pkg)}/sync`),
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
