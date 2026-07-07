import { api } from './client'

// L5 (2026-05-19): csp_runtime.py 시스템 1 폐기 마이그레이션
//
// 옛 cspRuntimeApi 는 listener/trunk/route/access/service 5 도메인 CRUD 를
// 다 노출했지만, 진짜 SoT 는 csc 의 deployment.collection 임. UI 는 그 중
// listServices() 만 사용 중이라 read-only view 로 축약했고, 나머지 CRUD 는
// agents.py 의 handle_sip_services + deployments collection API 가 대체.

export type ServiceKind = 'volte' | 'ptt' | 'ibcf' | 'system' | 'console'
export type InboundPolicy = 'any' | 'restricted'

export interface SipService {
  id: number
  name: string
  kind: ServiceKind
  domain: string
  auth_realm: string | null
  inbound_policy: InboundPolicy
  priority: number
  enabled: boolean
  listeners: string[]            // access_services.allowed_local_node_refs
  note: string | null
  etag: string
  create_time: string | null
  update_time: string | null
}

export const cspRuntimeApi = {
  listServices: () =>
    api.get<{ items: SipService[] }>('/csp/services').then(r => r.items),
}
