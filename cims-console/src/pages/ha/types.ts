// ServersPage 와 HaServicesPage 가 공유하는 type. HaServicesPage 가 SoT,
// 통합 후 ServersPage 의 ServerInspector "네트워크" 탭도 같은 type 사용.

import type { NetIface as ApiNetIface, ServiceIpRow as ApiServiceIpRow, AgentRoute } from '../../api/deployment'
import type { VipBinding as ApiVipBinding } from '../../api/ha_groups'

export type Mode = 'active_standby' | 'all_active' | 'standalone'
export type Role = 'master' | 'backup' | null
export type ServerStatus = 'pending' | 'online' | 'offline'

/** IP slot — 패키지가 요구하는 IP 필드. scope='vip' 그룹 단위 / 'service' 서버 단위. */
export interface IpSlot {
  scope: 'service' | 'vip'
  name: string
  port?: number
  proto?: 'tcp' | 'udp'
}

export type NetIface = ApiNetIface
export type BindingStatus = 'up' | 'down' | 'unknown' | 'applying' | 'fail' | 'idle'
export type ServiceIpRow = ApiServiceIpRow & { status?: BindingStatus }
// dirty 플래그 — 사용자가 ip/mask 편집 중인 row 는 NIC 매칭과 무관하게 status 'unknown'
export type VipBinding = ApiVipBinding & { dirty?: boolean }

/** 서버 = Agent. mgmt IP / 인터페이스 / 운영자 설정 service IP·route 보유. */
export interface ServerRow {
  id: number                    // = Agent.id (음수 = pending placeholder)
  name: string
  role: Role
  ip: string | null             // = Agent.ip_address (mgmt)
  status: ServerStatus
  agent_version: string | null
  token: string                 // enrollment_token (one-time install command 용)
  expiresAt: string | null      // enrollment_token_expires_at — UI 만료 표시 + 재발행 차단
  interfaces: NetIface[]
  serviceIpRows: ServiceIpRow[]
  routes: AgentRoute[]
}

/** 서비스 = HA 그룹 (active_standby|all_active) 또는 standalone agent (id=-agent.id). */
export interface ServiceRow {
  id: number                    // HaGroup.id (양수) 또는 -agent.id (standalone)
  name: string
  mode: Mode
  vrid: number | null
  vip: string                   // primary VIP (legacy field, vipBindings 와 별도)
  vipMask: number
  authPass: string
  servers: ServerRow[]
  packageIds: number[]          // derived from deployments
  vipBindings: VipBinding[]
}
