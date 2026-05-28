// ServersPage 와 HaServicesPage 가 공유하는 type. HaServicesPage 가 SoT,
// 통합 후 ServersPage 의 ServerInspector "네트워크" 탭도 같은 type 사용.

import type { NetIface as ApiNetIface, ServiceIpRow as ApiServiceIpRow } from '../../api/deployment'
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
