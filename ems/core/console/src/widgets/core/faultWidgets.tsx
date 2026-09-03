// 장애(Fault) 화면 위젯 — 활성 알람 · 알람 카탈로그 · 이력 · 유형별 분석을 배치 단위로 등록한다.
//
// 분해 단위는 console_platform.md §3.1 기준:
//   · 심각도 타일은 **1장 = 위젯 1개**. 선언 표(SEVERITY_ORDER) + 팩토리로 만들고, 목록과는
//     페이지 파라미터 `sev` 로 이어진다(타일 클릭 = 목록 필터).
//   · 코드 사전(조회)과 평가 규칙(감지 설정)은 성격도 API 도 달라 각각 위젯.
//   · 이력·유형별 분석은 **화면 전체가 위젯 하나**. 전환 탭·기간·표(블록)는 함께 조작하는 한 벌이라
//     떼어 놓으면 말이 되지 않는다. 조건은 그대로 페이지 파라미터라 컨트롤 위젯을 따로 놓는 배치도
//     성립한다(그 경우 위젯이 자기 안의 컨트롤을 접는다 — §3.2).
import { AlarmSeverityTile, ActiveAlarmList } from '../../pages/ActiveAlarmsPage'
import { AlarmCatalogTable, AlarmRulesTable } from '../../pages/AlarmCatalogPage'
import {
  AlarmHistoryFilter, AlarmsSection, EventHistoryFilter, EventsSection,
} from '../../pages/AlertsPage'
import { AuditEventsSection } from '../../pages/AuditEventsPage'
import {
  AlarmTotalsBlock, AlarmSeverityDistBlock, AlarmDailyBlock, AlarmByCodeBlock, AlarmByTypeBlock,
  EventTotalsBlock, EventDailyBlock, EventByTypeBlock, EventBySourceBlock,
} from '../../pages/AlarmAnalysisPage'
import { makeCardWidget } from '../CardLayout'
import { GRID_ROWS } from '../gridLayout'
import type { WidgetPlacement } from '../types'
import { AlarmEventTabs, PeriodDaysControl } from '../../components/ListControls'
import { SEVERITY_LABEL, SEVERITY_ORDER } from '../../utils/alarmLabels'
import { usePageControl } from '../pageParams'
import type { WidgetDef } from '../types'

// 심각도 타일 — 심각도별 컴포넌트를 두지 않고 SEVERITY_ORDER 표에서 팩토리로 생성한다.
export const SEVERITY_TILE_WIDGETS: WidgetDef[] = SEVERITY_ORDER.map(sev => ({
  id: `core.alarm-severity.${sev}`,
  title: `활성 알람 — ${SEVERITY_LABEL[sev]}`,
  category: 'metric',
  component: () => <AlarmSeverityTile sev={sev} />,
  apis: ['alerts.list'],
  defaultSize: { w: 2, h: 5 },     // 12-칸 기준 폭 2(≈⅕) × 5행(=10vh) — 타일 1장 크기
}))

export const alarmListWidget: WidgetDef = {
  id: 'core.alarm-list',
  title: '활성 알람 목록',
  category: 'event',
  component: ActiveAlarmList,
  apis: ['alerts.list'],
  defaultSize: { w: 12, h: 24 },
}

// 컨트롤 위젯 — 조회 조건을 **따로 떼어 놓고 싶을 때**의 배치 수단. 마운트하는 동안 그 파라미터의
// 소유를 선언하므로(usePageControl), 같은 레이아웃의 이력·분석 위젯은 자기 안의 같은 컨트롤을 접는다.
// 카드(panel)로 감싸지 않는다 — 기존 화면의 탭/툴바 모습 그대로.
function AlarmEventTabsControl() {
  usePageControl('atab')
  return <AlarmEventTabs />
}

function PeriodDaysFilterControl() {
  usePageControl('days')
  return <PeriodDaysControl />
}

export const periodDaysWidget: WidgetDef = {
  id: 'core.days-filter', title: '기간 선택 (일수)', category: 'control',
  component: PeriodDaysFilterControl, defaultSize: { w: 12, h: 4 },
}

export const alarmEventTabsWidget: WidgetDef = {
  id: 'core.alarm-event-tabs', title: '알람/이벤트 전환 (탭)', category: 'control',
  component: AlarmEventTabsControl, defaultSize: { w: 12, h: 3 },
}

// ── 이력·분석 블록 — 카드 안 배치의 부품 ─────────────────────────────────
// 카드로 합쳤다고 부품을 지우지 않는다: 카드 **안**도 48×48 셀 배치라(§3.0.1) 블록을 id 로 찾아
// 그리고, 운영자가 카드 안에서 재배치·추가·제거할 수 있어야 한다.
const block = (id: string, title: string, component: WidgetDef['component'],
               apis: string[], w: number, h: number): WidgetDef =>
  ({ id, title, category: 'event', component, apis, defaultSize: { w, h } })

export const alarmHistoryWidget = block(
  'core.alarm-history', '알람 이력 (표)', AlarmsSection, ['alerts.list'], 12, 40)
export const eventHistoryWidget = block(
  'core.event-history', '이벤트 이력 (표)', EventsSection, ['events.list'], 12, 40)
// 조회 조건 — 기간 선택 옆줄에 놓는다. 표는 아래 공간을 전부 쓴다.
export const alarmHistoryFilterWidget: WidgetDef = {
  id: 'core.alarm-history.filter', title: '알람 이력 — 조회 조건 (기간·필터)', category: 'control',
  component: AlarmHistoryFilter, apis: ['alerts.list'], defaultSize: { w: 12, h: 4 },
}
export const eventHistoryFilterWidget: WidgetDef = {
  id: 'core.event-history.filter', title: '이벤트 이력 — 조회 조건 (기간·필터)', category: 'control',
  component: EventHistoryFilter, apis: ['events.list'], defaultSize: { w: 12, h: 4 },
}

// 유형별 분석 블록 — 알람은 집계 API(alerts.summary), 이벤트는 목록 API(events.list)를 공유 로더로.
const analysis = (id: string, title: string, component: WidgetDef['component'],
                  w: number, h: number): WidgetDef =>
  ({ id, title, category: 'stats', component, defaultSize: { w, h },
     apis: [id.startsWith('core.alarm-') ? 'alerts.summary' : 'events.list'] })

export const ANALYSIS_BLOCK_WIDGETS: WidgetDef[] = [
  analysis('core.alarm-analysis.totals', '알람 분석 — 요약 타일', AlarmTotalsBlock, 12, 6),
  analysis('core.alarm-analysis.severity', '알람 분석 — 심각도 분포', AlarmSeverityDistBlock, 6, 9),
  analysis('core.alarm-analysis.daily', '알람 분석 — 일별 발생량', AlarmDailyBlock, 6, 9),
  analysis('core.alarm-analysis.by-code', '알람 분석 — 코드별 표', AlarmByCodeBlock, 7, 24),
  analysis('core.alarm-analysis.by-type', '알람 분석 — 유형(클래스)별 표', AlarmByTypeBlock, 5, 24),
  analysis('core.event-analysis.totals', '이벤트 분석 — 요약 타일', EventTotalsBlock, 12, 6),
  analysis('core.event-analysis.daily', '이벤트 분석 — 일별 통지량', EventDailyBlock, 12, 8),
  analysis('core.event-analysis.by-type', '이벤트 분석 — 유형별 표', EventByTypeBlock, 7, 22),
  analysis('core.event-analysis.by-source', '이벤트 분석 — 소스별 표', EventBySourceBlock, 5, 22),
]

// ── 카드 안 기본 배치 ─────────────────────────────────────────────────────
// 알람/이벤트 전환은 배치의 `visibleWhen`(§3.5) — 두 탭의 블록이 **같은 자리**를 갈아끼운다.
const alarmTab = { param: 'atab', equals: 'alarms' }
const eventTab = { param: 'atab', equals: 'events' }

// 알람/이벤트 전환 탭은 **예전처럼 맨 위 한 줄**. 그 아래는 기간+필터가 **한 블록 한 줄**이고
// (따로 두면 조회 조건이 두 덩어리로 갈려 보인다), 표가 나머지를 전부 차지한다.
const HISTORY_CARD_LAYOUT: WidgetPlacement[] = [
  { widgetId: 'core.alarm-event-tabs',     x: 0, y: 0, w: 48, h: 4 },
  { widgetId: 'core.alarm-history.filter', x: 0, y: 4, w: 48, h: 4, visibleWhen: alarmTab },
  { widgetId: 'core.event-history.filter', x: 0, y: 4, w: 48, h: 4, visibleWhen: eventTab },
  { widgetId: 'core.alarm-history',        x: 0, y: 8, w: 48, h: 40, visibleWhen: alarmTab },
  { widgetId: 'core.event-history',        x: 0, y: 8, w: 48, h: 40, visibleWhen: eventTab },
]

const ANALYSIS_CARD_LAYOUT: WidgetPlacement[] = [
  { widgetId: 'core.alarm-event-tabs', x: 0, y: 0, w: 48, h: 4 },
  { widgetId: 'core.days-filter',      x: 0, y: 4, w: 48, h: 4 },
  { widgetId: 'core.alarm-analysis.totals',    x: 0,  y: 8,  w: 48, h: 7,  visibleWhen: alarmTab },
  { widgetId: 'core.alarm-analysis.severity',  x: 0,  y: 15, w: 24, h: 9,  visibleWhen: alarmTab },
  { widgetId: 'core.alarm-analysis.daily',     x: 24, y: 15, w: 24, h: 9,  visibleWhen: alarmTab },
  { widgetId: 'core.alarm-analysis.by-code',   x: 0,  y: 24, w: 28, h: 24, visibleWhen: alarmTab },
  { widgetId: 'core.alarm-analysis.by-type',   x: 28, y: 24, w: 20, h: 24, visibleWhen: alarmTab },
  { widgetId: 'core.event-analysis.totals',    x: 0,  y: 8,  w: 48, h: 7,  visibleWhen: eventTab },
  { widgetId: 'core.event-analysis.daily',     x: 0,  y: 15, w: 48, h: 9,  visibleWhen: eventTab },
  { widgetId: 'core.event-analysis.by-type',   x: 0,  y: 24, w: 28, h: 24, visibleWhen: eventTab },
  { widgetId: 'core.event-analysis.by-source', x: 28, y: 24, w: 20, h: 24, visibleWhen: eventTab },
]

export const alarmEventHistoryWidget: WidgetDef = makeCardWidget({
  id: 'core.alarm-event-history', title: '알람·이벤트 이력', category: 'event',
  defaultSize: { w: 12, h: GRID_ROWS }, layout: HISTORY_CARD_LAYOUT,
})

export const alarmEventAnalysisWidget: WidgetDef = makeCardWidget({
  id: 'core.alarm-event-analysis', title: '유형별 분석 (알람·이벤트)', category: 'stats',
  defaultSize: { w: 12, h: GRID_ROWS }, layout: ANALYSIS_CARD_LAYOUT,
})

// 감사 이력 — kind=audit(E-AUD-*) 전용. 합법감청 params 를 열로 펼친다. 라우트가 manager 이상으로 게이트.
export const auditHistoryWidget: WidgetDef = {
  id: 'core.audit-history', title: '감사 이력 (합법감청 등)', category: 'event',
  component: AuditEventsSection, apis: ['events.list'], defaultSize: { w: 12, h: 38 },
}

export const alarmCatalogWidget: WidgetDef = {
  id: 'core.alarm-catalog',
  title: '알람 코드 사전',
  category: 'event',
  component: AlarmCatalogTable,
  apis: ['alerts.catalog'],
  defaultSize: { w: 12, h: 26 },
}

export const alarmRulesWidget: WidgetDef = {
  id: 'core.alarm-rules',
  title: '알람 평가 규칙 (임계·주기)',
  category: 'event',
  component: AlarmRulesTable,
  // 평가 규칙(`/alerts/rules`)은 내부 설정이라 API 문서 선언 대상이 아니다 → 배지 없음이 정상.
  defaultSize: { w: 12, h: 20 },
}

export const FAULT_WIDGETS: WidgetDef[] = [
  ...SEVERITY_TILE_WIDGETS, alarmListWidget,
  alarmEventTabsWidget, periodDaysWidget,
  alarmEventHistoryWidget, alarmEventAnalysisWidget,
  alarmHistoryFilterWidget, eventHistoryFilterWidget,
  alarmHistoryWidget, eventHistoryWidget, ...ANALYSIS_BLOCK_WIDGETS,
  auditHistoryWidget, alarmCatalogWidget, alarmRulesWidget,
]
