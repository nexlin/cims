import { api } from './client'

/** MCPTT 시스템 서비스 설정 (TS 24.484 §8.4 service-config) — **시스템 전역 1건**.
 *
 *  인가(1:1·긴급·경보·그룹 생성)는 이 문서에 없다 — 가입자 화면의 user-profile 과 그룹 능력이 정본이다.
 *  floor 타이머·Resource-Priority 는 CSC 배포 설정 ServiceConfig.* 이다. */
export interface McpttServiceConfig {
  max_affiliations_n2: number          // N2 — user-profile MaxAffiliationsN2 기본값
  max_calls_n6: number                 // N6 — user-profile MaxSimultaneousCallsN6 (관제가 아닌 사용자)
  max_calls_n6_dispatch: number        // N6 — 관제(역할 배정 사용자)
  num_levels_group_hierarchy: number   // common/broadcast-group/num-levels-group-hierarchy
  num_levels_user_hierarchy: number    // common/broadcast-group/num-levels-user-hierarchy
  update_time?: string | null
  exists?: boolean                     // false = DB 행 부재(코드 기본값 응답)
}

export const mcpttApi = {
  getServiceConfig: () => api.get<McpttServiceConfig>('/mcptt/service-config'),
  updateServiceConfig: (data: Partial<McpttServiceConfig>) =>
    api.put<McpttServiceConfig>('/mcptt/service-config', data),
}
