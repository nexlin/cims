// 기본 페이지 레이아웃 seed — 위젯 배치 서술. 좌표계는 **2D 그리드 셀**(gridLayout):
// x=0-based 열(0~47, GRID_COLS=48), y=0-based 행, w=열 span, h=행 span(1행 = 화면 세로 2%, ROW_H_VH).
// 저장본(`/console/layouts/<id>`)이 있으면 그것이 이 seed 를 덮는다. seedVersion 은 seed 개편 세대 —
// 올리면 옛 세대 기준으로 저장된 배치에 "기본 배치가 갱신됨" 안내가 뜬다(EditableLayout).

import type { PageLayout } from './types'

// 알람/이벤트 전환 탭 — 배치의 표시 조건. 같은 자리를 갈아끼우는 위젯들이 이 조건으로 갈린다.
const alarmTab = { param: 'atab', equals: 'alarms' }
const eventTab = { param: 'atab', equals: 'events' }

// 활성 알람 — 심각도 요약(타일 묶음) + 목록. 타일 클릭은 페이지 파라미터 `sev` 로 목록에 걸린다.
export const ALERTS_ACTIVE_LAYOUT: PageLayout = {
  id: 'alerts.active', title: '활성 알람', seedVersion: 1,
  widgets: [
    { widgetId: 'core.alarm-severity', x: 0, y: 0, w: 48, h: 5 },
    { widgetId: 'core.alarm-list',     x: 0, y: 5, w: 48, h: 34 },
  ],
}

// 알람 카탈로그 — 코드 사전(조회) + 평가 규칙(감지 설정)
export const ALERTS_CATALOG_LAYOUT: PageLayout = {
  id: 'alerts.catalog', title: '알람 카탈로그', seedVersion: 1,
  widgets: [
    { widgetId: 'core.alarm-catalog', x: 0, y: 0,  w: 48, h: 26 },
    { widgetId: 'core.alarm-rules',   x: 0, y: 26, w: 48, h: 20 },
  ],
}

// 알람·이벤트 이력 — 탭(파라미터 `atab`) + 알람 이력 / 이벤트 이력. 고른 쪽만 보인다.
// 알람·이벤트 이력 = 전환 탭 / 기간 선택 / 이력 표 3부분.
// 두 이력은 **같은 자리**를 갈아끼우므로 좌표가 같다 — 편집 화면도 뷰와 같은 모습이 된다.
export const ALERTS_HISTORY_LAYOUT: PageLayout = {
  id: 'alerts.history', title: '알람·이벤트 이력', seedVersion: 2,
  widgets: [
    { widgetId: 'core.alarm-event-tabs', x: 0, y: 0, w: 48, h: 4 },
    { widgetId: 'core.days-filter',      x: 0, y: 4, w: 48, h: 4 },
    { widgetId: 'core.alarm-history',    x: 0, y: 8, w: 48, h: 37, visibleWhen: alarmTab },
    { widgetId: 'core.event-history',    x: 0, y: 8, w: 48, h: 37, visibleWhen: eventTab },
  ],
}

// 유형별 분석 — 이력 화면과 같은 구성: 전환 탭 / 기간 선택 / 블록들.
// 블록은 각각 위젯이고, 어느 쪽을 보일지는 배치의 visibleWhen 이 판정한다(탭처럼 갈아끼움).
// 숨겨진 블록은 렌더에서 빠지고 남은 것이 위로 당겨진다 — 편집 모드에선 전부 보인다.
export const ALERTS_ANALYSIS_LAYOUT: PageLayout = {
  id: 'alerts.analysis', title: '유형별 분석', seedVersion: 5,
  widgets: [
    { widgetId: 'core.alarm-event-tabs', x: 0, y: 0, w: 48, h: 4 },
    { widgetId: 'core.days-filter',      x: 0, y: 4, w: 48, h: 4 },
    // 알람 탭 / 이벤트 탭 — **같은 자리**를 갈아끼우므로 두 탭의 블록이 같은 y 대역을 쓴다.
    { widgetId: 'core.alarm-analysis.totals',    x: 0,  y: 8,  w: 48, h: 6,  visibleWhen: alarmTab },
    { widgetId: 'core.alarm-analysis.severity',  x: 0,  y: 14, w: 24, h: 9,  visibleWhen: alarmTab },
    { widgetId: 'core.alarm-analysis.daily',     x: 24, y: 14, w: 24, h: 9,  visibleWhen: alarmTab },
    { widgetId: 'core.alarm-analysis.by-code',   x: 0,  y: 23, w: 28, h: 24, visibleWhen: alarmTab },
    { widgetId: 'core.alarm-analysis.by-type',   x: 28, y: 23, w: 20, h: 24, visibleWhen: alarmTab },
    { widgetId: 'core.event-analysis.totals',    x: 0,  y: 8,  w: 48, h: 6,  visibleWhen: eventTab },
    { widgetId: 'core.event-analysis.daily',     x: 0,  y: 14, w: 48, h: 8,  visibleWhen: eventTab },
    { widgetId: 'core.event-analysis.by-type',   x: 0,  y: 22, w: 28, h: 22, visibleWhen: eventTab },
    { widgetId: 'core.event-analysis.by-source', x: 28, y: 22, w: 20, h: 22, visibleWhen: eventTab },
  ],
}

// 서비스 정의 — 서비스 선택(드롭다운+추가) + 이름/JSON/삭제 + 모듈 · 알람 규칙 · 데이터 소스.
// 세 컬렉션은 모두 선택된 서비스(`svc` 파라미터)에 종속이고, 각각 자기 항목의 추가/편집/삭제를 갖는다.
export const SERVICE_DEFS_LAYOUT: PageLayout = {
  id: 'deploy.service-defs', title: '서비스 정의', seedVersion: 1,
  widgets: [
    { widgetId: 'core.service-picker',      x: 0,  y: 0,  w: 48, h: 4 },
    { widgetId: 'core.service-def.header',  x: 0,  y: 4,  w: 48, h: 4 },
    { widgetId: 'core.service-def.modules', x: 0,  y: 8,  w: 24, h: 19 },
    { widgetId: 'core.service-def.rules',   x: 24, y: 8,  w: 24, h: 19 },
    { widgetId: 'core.service-def.sources', x: 0,  y: 27, w: 48, h: 20 },
  ],
}

export const DASHBOARD_LAYOUT: PageLayout = {
  id: 'dashboard',
  title: '대시보드',
  seedVersion: 4,                 // 4세대: 지표 묶음(구 cims.kpi)만 분해. 활성 알람·최근 이벤트처럼
                                  //        같은 축의 분포는 한 위젯으로 유지한다(console_platform §3.1).
  widgets: [
    { widgetId: 'cims.active-alarms',      x: 0,  y: 0,  w: 48, h: 13 },   // 활성 알람 (심각도 타일 + 목록) ★ 최상단
    { widgetId: 'cims.recent-events',      x: 0,  y: 13, w: 48, h: 13 },   // 최근 이벤트 (stateChange/audit)
    { widgetId: 'core.system-topology',    x: 0,  y: 26, w: 24, h: 23 },   // 시스템 형상(½) + 리소스(½)
    { widgetId: 'core.system-resource',    x: 24, y: 26, w: 24, h: 23 },
    // 현황 카드 — 지표 1개 = 위젯 1개(cims.stat.*). 7장이 한 줄에 들어가게 48칸을 7·7·7·7·7·7·6 로 나눈다.
    { widgetId: 'cims.stat.subscribers',   x: 0,  y: 49, w: 7,  h: 6 },
    { widgetId: 'cims.stat.volte-numbers', x: 7,  y: 49, w: 7,  h: 6 },
    { widgetId: 'cims.stat.ptt-numbers',   x: 14, y: 49, w: 7,  h: 6 },
    { widgetId: 'cims.stat.active-calls',  x: 21, y: 49, w: 7,  h: 6 },
    { widgetId: 'cims.stat.ptt-groups',    x: 28, y: 49, w: 7,  h: 6 },
    { widgetId: 'cims.stat.rtp-voip',      x: 35, y: 49, w: 7,  h: 6 },
    { widgetId: 'cims.stat.rtp-ptt',       x: 42, y: 49, w: 6,  h: 6 },
    { widgetId: 'cims.active-voip',        x: 0,  y: 55, w: 24, h: 16 },   // 활성 VoIP(½) + PTT(½)
    { widgetId: 'cims.active-ptt',         x: 24, y: 55, w: 24, h: 16 },
    { widgetId: 'shape.time-bar',          x: 0,  y: 71, w: 48, h: 15, config: { source: 'cims.svc.volte' } },
  ],
  // 과감히 제거: cims.health-dots(→형상 노드/모듈 상태로 흡수), cims.alert-banner(→active-alarms),
  //            core.system-cards(→system-topology 로 대체), cims.csp-roles(단순 역할 플래그).
}
