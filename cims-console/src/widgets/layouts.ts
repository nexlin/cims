// 기본 페이지 레이아웃 seed — 위젯 배치 서술. w = 12-col span (전체=12, ½=6, ⅓=4, ¼=3).
// 같은 단(행)에 합이 12 이하인 위젯들이 나란히 배치되고, 넘으면 다음 단으로 wrap.
// 5-3 step2 file_store 영속 + 관리자 편집(EditableLayout)으로 덮어쓸 수 있는 기본값.

import type { PageLayout } from './types'

export const DASHBOARD_LAYOUT: PageLayout = {
  id: 'dashboard',
  title: '대시보드',
  widgets: [
    { widgetId: 'cims.active-alarms', h: 260 },                  // 1단: 활성 알람 (표준 알람 스트림) ★ 최상단
    { widgetId: 'core.system-topology', w: 6, h: 440 },          // 2단: 시스템 형상(½) + 리소스(½) 나란히
    { widgetId: 'core.system-resource', w: 6, h: 440 },
    { widgetId: 'cims.kpi' },                                    // 3단: 4 KPI (가입자/통화/PTT/RTP)
    { widgetId: 'cims.active-voip', w: 6, h: 300 },              // 4단: 활성 VoIP(½) + PTT(½)
    { widgetId: 'cims.active-ptt', w: 6, h: 300 },
    { widgetId: 'shape.time-bar', config: { source: 'cims.svc.volte' } },  // 5단: 주요 트래픽 추이(VoLTE)
  ],
  // 과감히 제거: cims.health-dots(→형상 노드/모듈 상태로 흡수), cims.alert-banner(→active-alarms),
  //            core.system-cards(→system-topology 로 대체), cims.csp-roles(단순 역할 플래그).
}
