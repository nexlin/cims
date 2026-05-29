// 기본 페이지 레이아웃 seed — 위젯 배치 서술 (모두 w=12 = 수직 스택, 기존 대시보드와 동일).
// 5-3 step2 에서 file_store 영속 + 관리자 편집으로 대체될 기본값.

import type { PageLayout } from './types'

export const DASHBOARD_LAYOUT: PageLayout = {
  id: 'dashboard',
  title: '대시보드',
  widgets: [
    { widgetId: 'cims.health-dots' },
    { widgetId: 'cims.kpi' },
    { widgetId: 'core.system-cards' },
    { widgetId: 'cims.csp-roles' },
    { widgetId: 'cims.alert-banner' },
    { widgetId: 'cims.active-voip' },
    { widgetId: 'cims.active-ptt' },
  ],
}
