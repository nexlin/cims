// 기본 페이지 레이아웃 seed — 위젯 배치 서술. w = 12-col span (전체=12, ½=6, ⅓=4, ¼=3).
// 같은 단(행)에 합이 12 이하인 위젯들이 나란히 배치되고, 넘으면 다음 단으로 wrap.
// 5-3 step2 file_store 영속 + 관리자 편집(EditableLayout)으로 덮어쓸 수 있는 기본값.

import type { PageLayout } from './types'

export const DASHBOARD_LAYOUT: PageLayout = {
  id: 'dashboard',
  title: '대시보드',
  widgets: [
    { widgetId: 'cims.health-dots' },                  // 1단: 전체폭 (내부 3카드)
    { widgetId: 'cims.kpi' },                           // 2단: 전체폭 (내부 4 KPI)
    { widgetId: 'cims.alert-banner' },                  // (알람 시에만)
    { widgetId: 'core.system-cards', w: 8 },            // 3단: 시스템(8) + CSP역할(4) 나란히
    { widgetId: 'cims.csp-roles', w: 4 },
    { widgetId: 'cims.active-voip', w: 6 },             // 4단: VoIP(6) + PTT(6) 나란히
    { widgetId: 'cims.active-ptt', w: 6 },
  ],
}
