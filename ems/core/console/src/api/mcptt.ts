import { api } from './client'

/** MCPTT 시스템 서비스 설정 (TS 24.484 service-config) — **시스템 전역 1건**.
 *
 *  사용자별 인가(긴급콜·경보·애드혹 개시)는 가입자 화면의 user-profile 이고, 단말은 두 축을
 *  AND 로 게이트한다 — 여기서 끈 기능은 프로파일이 허용해도 열리지 않는다.
 *  단말 소비 지점: docs/design/features/android_ue_client.md §7 "CMS 문서 소비". */
export interface McpttServiceConfig {
  allow_private_call: boolean          // allow-private-call — 1:1 통화 발신
  allow_emergency_call: boolean        // allow-emergency-call — 긴급통화
  allow_alert: boolean                 // allow-alert — 긴급경보
  allow_transmit_request: boolean      // on-network allow-transmit-request — 발언권 요청
  allow_create_delete_group: boolean   // allow-create-delete-group — 사용자 그룹 생성/삭제
  max_affiliations_n2: number          // N2 — 동시 제휴(편성) 채널 상한
  num_levels_group_hierarchy: number
  num_levels_user_hierarchy: number
  update_time?: string | null
  exists?: boolean                     // false = DB 행 부재(코드 기본값 응답)
}

export const mcpttApi = {
  getServiceConfig: () => api.get<McpttServiceConfig>('/mcptt/service-config'),
  updateServiceConfig: (data: Partial<McpttServiceConfig>) =>
    api.put<McpttServiceConfig>('/mcptt/service-config', data),
}
