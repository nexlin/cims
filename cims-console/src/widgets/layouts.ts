// 기본 페이지 레이아웃 seed — 위젯 배치 서술. w = 12-col span (전체=12, ½=6, ⅓=4, ¼=3).
// 같은 단(행)에 합이 12 이하인 위젯들이 나란히 배치되고, 넘으면 다음 단으로 wrap.
// 5-3 step2 file_store 영속 + 관리자 편집(EditableLayout)으로 덮어쓸 수 있는 기본값.

import type { PageLayout } from './types'

export const DASHBOARD_LAYOUT: PageLayout = {
  id: 'dashboard',
  title: '대시보드',
  widgets: [
    { widgetId: 'cims.active-alarms' },                          // 1단: 활성 알람 (표준 알람 스트림) ★ 최상단
    { widgetId: 'cims.health-dots' },                            // 2단: CSP/CMP/DB 상태 (전체폭 3카드)
    { widgetId: 'cims.kpi' },                                    // 3단: 4 KPI (가입자/통화/PTT/RTP)
    { widgetId: 'core.system-cards' },                           // 4단: 시스템/HA 카드 (전체폭)
    { widgetId: 'cims.active-voip', w: 6 },                      // 5단: 활성 VoIP(½) + PTT(½)
    { widgetId: 'cims.active-ptt', w: 6 },
    { widgetId: 'shape.time-bar', config: { source: 'cims.svc.volte' } },  // 6단: 주요 트래픽 추이(VoLTE)
  ],
  // 과감히 제거: cims.alert-banner(→active-alarms 대체), cims.csp-roles(단순 역할 플래그).
}
