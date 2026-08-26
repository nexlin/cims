// 장애(Fault) 화면 위젯 — 활성 알람과 알람 카탈로그를 배치 가능한 단위로 등록한다.
//
// 분해 단위는 console_platform.md §3.1 기준:
//   · 심각도 요약은 **타일 묶음 하나**가 최소 단위다(같은 축의 분포 — 하나만 보면 의미가 없다).
//     목록과는 페이지 파라미터 `sev` 로 이어진다(타일 클릭 = 목록 필터).
//   · 코드 사전(조회)과 평가 규칙(감지 설정)은 성격도 API 도 달라 각각 위젯.
import { AlarmSeverityTiles, ActiveAlarmList } from '../../pages/ActiveAlarmsPage'
import { AlarmCatalogTable, AlarmRulesTable } from '../../pages/AlarmCatalogPage'
import { AlarmsSection, EventsSection } from '../../pages/AlertsPage'
import { usePageParam } from '../pageParams'
import { DaysButtons } from '../../components/ListControls'
import {
  AlarmTotalsBlock, AlarmSeverityDistBlock, AlarmDailyBlock,
  AlarmByCodeBlock, AlarmByTypeBlock,
  EventTotalsBlock, EventDailyBlock, EventByTypeBlock, EventBySourceBlock,
} from '../../pages/AlarmAnalysisPage'
import type { WidgetDef } from '../types'

export const alarmSeverityWidget: WidgetDef = {
  id: 'core.alarm-severity',
  title: '심각도 요약 (활성 알람)',
  category: 'event',
  component: AlarmSeverityTiles,
  defaultSize: { w: 12, h: 5 },
}

export const alarmListWidget: WidgetDef = {
  id: 'core.alarm-list',
  title: '활성 알람 목록',
  category: 'event',
  component: ActiveAlarmList,
  defaultSize: { w: 12, h: 24 },
}

// 알람/이벤트 전환 — 기존 화면과 같은 탭 모양(카드 껍데기 없음). 파라미터 `atab` 을 소유하고,
// 각 배치의 visibleWhen 이 그 값으로 자기 표시 여부를 판정한다.
function AlarmEventTabs() {
  const [tab, setTab] = usePageParam('atab')
  const cur = tab || 'alarms'
  return (
    <div className="tab-nav">
      <button className={`tab-btn ${cur === 'alarms' ? 'tab-btn--active' : ''}`}
              onClick={() => setTab('alarms')}>알람</button>
      <button className={`tab-btn ${cur === 'events' ? 'tab-btn--active' : ''}`}
              onClick={() => setTab('events')}>이벤트</button>
    </div>
  )
}

// 기간 선택 — 페이지 파라미터 `days` 만 소유하는 범용 컨트롤. 같은 페이지의 조회 위젯이 함께 따른다.
// 카드(panel)로 감싸지 않는다 — 기존 화면의 툴바 바 모습 그대로.
function PeriodDaysControl() {
  const [days, setDays] = usePageParam('days')
  return (
    <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
      <DaysButtons days={Number(days) || 7} onChange={d => setDays(String(d))} />
      <span style={{ fontSize: 11, color: 'var(--text-muted)', marginLeft: 'auto' }}>
        이 페이지의 조회 위젯에 함께 적용
      </span>
    </div>
  )
}

export const periodDaysWidget: WidgetDef = {
  id: 'core.days-filter', title: '기간 선택 (일수)', category: 'control',
  component: PeriodDaysControl, defaultSize: { w: 12, h: 4 },
}

export const alarmEventTabsWidget: WidgetDef = {
  id: 'core.alarm-event-tabs', title: '알람/이벤트 전환 (탭)', category: 'control',
  component: AlarmEventTabs, defaultSize: { w: 12, h: 3 },
}

export const alarmHistoryWidget: WidgetDef = {
  id: 'core.alarm-history', title: '알람 이력', category: 'event',
  component: AlarmsSection, defaultSize: { w: 12, h: 34 },
}

export const eventHistoryWidget: WidgetDef = {
  id: 'core.event-history', title: '이벤트 이력', category: 'event',
  component: EventsSection, defaultSize: { w: 12, h: 34 },
}

export const alarmCatalogWidget: WidgetDef = {
  id: 'core.alarm-catalog',
  title: '알람 코드 사전',
  category: 'event',
  component: AlarmCatalogTable,
  defaultSize: { w: 12, h: 26 },
}

export const alarmRulesWidget: WidgetDef = {
  id: 'core.alarm-rules',
  title: '알람 평가 규칙 (임계·주기)',
  category: 'event',
  component: AlarmRulesTable,
  defaultSize: { w: 12, h: 20 },
}

// ── 유형별 분석 블록 — 조회 일수는 core.days-filter 가 소유하고 각 블록이 읽는다 ──
const analysis = (id: string, title: string, component: WidgetDef['component'],
                  w: number, h: number): WidgetDef =>
  ({ id, title, category: 'stats', component, defaultSize: { w, h } })

export const ANALYSIS_WIDGETS: WidgetDef[] = [
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

export const FAULT_WIDGETS: WidgetDef[] = [
  alarmSeverityWidget, alarmListWidget, alarmEventTabsWidget, periodDaysWidget,
  alarmHistoryWidget, eventHistoryWidget, alarmCatalogWidget, alarmRulesWidget,
  ...ANALYSIS_WIDGETS,
]
