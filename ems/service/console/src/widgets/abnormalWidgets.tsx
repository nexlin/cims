// 비정상 세션 이력 화면 위젯 — 조회 조건 · 지표 · 발신 IP 상위 · 세션 표와 그것을 담은 카드.
//
// 배치 단위는 카드 하나(조건과 결과가 한 벌)지만, 카드 안은 바깥과 같은 48×48 셀이라
// 운영자가 `[⚙]` 으로 블록 자리·크기를 바꿀 수 있다(console_platform §3.0.1).
import { ABN_METRICS, AbnFilter, AbnKpi, AbnTopIps, AbnTable } from '../pages/AbnormalSessionsPage'
import { makeCardWidget } from '@core/widgets/CardLayout'
import { GRID_ROWS } from '@core/widgets/gridLayout'
import type { WidgetDef, WidgetPlacement } from '@core/widgets/types'

const API = ['security.abnormal-sessions']

export const abnFilterWidget: WidgetDef = {
  id: 'cims.abn.filter', title: '비정상 세션 — 조회 조건', category: 'control',
  component: AbnFilter, apis: API, defaultSize: { w: 12, h: 4 },
}
// 지표 — 서로 다른 축이라 **낱개 위젯**. 선언 표(ABN_METRICS) + 팩토리로 만든다.
export const ABN_KPI_WIDGETS: WidgetDef[] = ABN_METRICS.map(m => ({
  id: `cims.abn.kpi.${m.key}`, title: m.title, category: 'metric',
  component: () => <AbnKpi metric={m} />, apis: API,
  defaultSize: { w: 3, h: 7 }, minSize: { h: 4 },
}))
export const abnTopIpsWidget: WidgetDef = {
  id: 'cims.abn.top-ips', title: '비정상 세션 — 발신 IP 상위', category: 'event',
  component: AbnTopIps, apis: API, defaultSize: { w: 12, h: 7 }, minSize: { h: 5 },
}
export const abnTableWidget: WidgetDef = {
  id: 'cims.abn.table', title: '비정상 세션 — 세션 표', category: 'event',
  component: AbnTable, apis: API, defaultSize: { w: 12, h: 30 }, minSize: { h: 8 },
}

// 카드 안 기본 배치 — 세로 합 = GRID_ROWS(화면 한 장).
// 지표 4장은 48칸을 12·12·12·12 로 나눠 한 줄에 넣는다.
export const ABN_CARD_LAYOUT: WidgetPlacement[] = [
  { widgetId: 'cims.abn.filter',       x: 0,  y: 0,  w: 48, h: 4 },
  ...ABN_METRICS.map((m, i) => ({
    widgetId: `cims.abn.kpi.${m.key}`, x: i * 12, y: 4, w: 12, h: 7,
  })),
  { widgetId: 'cims.abn.top-ips',      x: 0,  y: 11, w: 48, h: 7 },
  { widgetId: 'cims.abn.table',        x: 0,  y: 18, w: 48, h: 30 },
]

export const abnCardWidget: WidgetDef = makeCardWidget({
  id: 'cims.abnormal-sessions', title: '비정상 세션 이력 화면', category: 'event',
  defaultSize: { w: 12, h: GRID_ROWS }, layout: ABN_CARD_LAYOUT,
})

export const ABNORMAL_WIDGETS: WidgetDef[] = [
  abnCardWidget, abnFilterWidget, ...ABN_KPI_WIDGETS, abnTopIpsWidget, abnTableWidget,
]
