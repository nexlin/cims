// 기본 페이지 레이아웃 seed — 위젯 배치 서술. 좌표계는 **2D 그리드 셀**(gridLayout):
// x=0-based 열(0~47, GRID_COLS=48), y=0-based 행(0~47, GRID_ROWS=48), w=열 span, h=행 span.
//
// **세로 48행이 곧 화면 한 장**이다 — 관제 화면이라 1920×1080 에서 스크롤 없이 다 보여야 하므로
// 어떤 seed 도 y+h 가 48 을 넘지 않는다(넘으면 편집기가 조작을 거절하고 렌더도 잘린다).
// 세로가 모자라면 세로로 쌓지 말고 **가로를 쓴다**(48칸을 나눠 2~3열로).
//
// 저장본(`/console/layouts/<id>`)이 있으면 그것이 이 seed 를 덮는다. seedVersion 은 seed 개편 세대 —
// 올리면 옛 세대 기준으로 저장된 배치에 "기본 배치가 갱신됨" 안내가 뜬다(EditableLayout).

import type { PageLayout } from './types'
import { GRID_ROWS } from './gridLayout'

// 활성 알람 — 심각도 타일 5장(각각 위젯) + 목록. 타일 클릭은 페이지 파라미터 `sev` 로 목록에 걸린다.
// 타일은 48칸을 10·10·10·9·9 로 나눠 한 줄에 넣는다.
export const ALERTS_ACTIVE_LAYOUT: PageLayout = {
  id: 'alerts.active', title: '활성 알람', seedVersion: 3,
  widgets: [
    { widgetId: 'core.alarm-severity.critical',      x: 0,  y: 0, w: 10, h: 5 },
    { widgetId: 'core.alarm-severity.major',         x: 10, y: 0, w: 10, h: 5 },
    { widgetId: 'core.alarm-severity.minor',         x: 20, y: 0, w: 10, h: 5 },
    { widgetId: 'core.alarm-severity.warning',       x: 30, y: 0, w: 9,  h: 5 },
    { widgetId: 'core.alarm-severity.indeterminate', x: 39, y: 0, w: 9,  h: 5 },
    { widgetId: 'core.alarm-list',                   x: 0,  y: 5, w: 48, h: 43 },
  ],
}

// 알람 카탈로그 — 코드 사전(조회) + 평가 규칙(감지 설정)
export const ALERTS_CATALOG_LAYOUT: PageLayout = {
  id: 'alerts.catalog', title: '알람 카탈로그', seedVersion: 2,
  widgets: [
    { widgetId: 'core.alarm-catalog', x: 0, y: 0,  w: 48, h: 28 },
    { widgetId: 'core.alarm-rules',   x: 0, y: 28, w: 48, h: 20 },
  ],
}

// 알람·이벤트 이력 — 전환 탭 · 기간 선택 · 이력 표가 한 위젯이다. 셋은 함께 조작하는 한 벌이라
// 떼어 놓으면 말이 되지 않는다(고른 탭과 기간이 곧 표의 의미). 조건은 그대로 페이지 파라미터
// (`atab`/`days`)여서, 컨트롤 위젯을 따로 얹는 배치도 여전히 성립한다.
export const ALERTS_HISTORY_LAYOUT: PageLayout = {
  id: 'alerts.history', title: '알람·이벤트 이력', seedVersion: 8,
  widgets: [
    { widgetId: 'core.alarm-event-history', x: 0, y: 0, w: 48, h: 48 },
  ],
}

// 감사 이력 — 기간 선택 + 감사(kind=audit) 표. 합법감청(E-AUD-016) 열람 화면 (manager 이상, 라우트 requiredRole).
export const ALERTS_AUDIT_LAYOUT: PageLayout = {
  id: 'alerts.audit', title: '감사 이력', seedVersion: 2,
  widgets: [
    { widgetId: 'core.days-filter',   x: 0, y: 0, w: 48, h: 4 },
    { widgetId: 'core.audit-history', x: 0, y: 4, w: 48, h: 44 },
  ],
}

// 유형별 분석 — 이력 화면과 같은 구성: 전환 탭 · 기간 선택 · 블록들이 한 위젯이다. 블록들은 같은
// 기간 창을 여러 각도(요약/분포/추이/코드별/유형별)에서 보는 한 벌이라 함께 둔다.
export const ALERTS_ANALYSIS_LAYOUT: PageLayout = {
  id: 'alerts.analysis', title: '유형별 분석', seedVersion: 8,
  widgets: [
    { widgetId: 'core.alarm-event-analysis', x: 0, y: 0, w: 48, h: 48 },
  ],
}

// 서비스 정의 — 화면 전체가 카드 하나. 서비스 선택(드롭다운+추가) · 이름/JSON/삭제 · 모듈 ·
// 알람 규칙 · 데이터 소스는 모두 고른 서비스(`svc` 파라미터)에 종속이라, 선택을 떼거나 컬렉션
// 하나만 떼어 놓으면 무엇에 대한 목록인지 알 수 없다. 카드 안 구성은 SERVICE_DEF_CARD_ROWS 가 정본.
export const SERVICE_DEFS_LAYOUT: PageLayout = {
  id: 'deploy.service-defs', title: '서비스 정의', seedVersion: 3,
  widgets: [
    { widgetId: 'core.service-defs', x: 0, y: 0, w: 48, h: GRID_ROWS },
  ],
}

// 내 대시보드 구성 — 화면 전체가 카드 하나(상태·프로파일·위젯 목록). 셋은 같은 편집 초안을
// 다루는 한 벌이라 떼어 놓으면 "무엇을 저장하는지"가 흩어진다.
export const MY_LAYOUT_LAYOUT: PageLayout = {
  id: 'dashboard.my-layout', title: '내 대시보드 구성', seedVersion: 1,
  widgets: [
    { widgetId: 'core.my-layout', x: 0, y: 0, w: 48, h: GRID_ROWS },
  ],
}

export const DASHBOARD_LAYOUT: PageLayout = {
  id: 'dashboard',
  title: '대시보드',
  // 5세대: **화면 한 장(48행)에 맞춤**. 예전엔 대부분이 전폭(48칸) 스택이라 172vh 로 자라 페이지가
  //        스크롤됐다 — 관제 대시보드는 스크롤이 곧 "알람이 화면 밖으로 나감"이라 가로를 쓰도록
  //        2열로 접었다. 위젯 구성은 그대로 유지한다(하나라도 빠지면 한눈에 볼 것이 줄어든다).
  seedVersion: 5,
  widgets: [
    // ① 알람·이벤트 — 최상단(가장 강한 자리). 알람이 좌측 우선.
    { widgetId: 'cims.active-alarms',      x: 0,  y: 0,  w: 28, h: 15 },
    { widgetId: 'cims.recent-events',      x: 28, y: 0,  w: 20, h: 15 },
    // ② 현황 카드 — 지표 1개 = 위젯 1개(cims.stat.*). 7장이 한 줄에 들어가게 48칸을 7·7·7·7·7·7·6 로 나눈다.
    { widgetId: 'cims.stat.subscribers',   x: 0,  y: 15, w: 7,  h: 6 },
    { widgetId: 'cims.stat.volte-numbers', x: 7,  y: 15, w: 7,  h: 6 },
    { widgetId: 'cims.stat.ptt-numbers',   x: 14, y: 15, w: 7,  h: 6 },
    { widgetId: 'cims.stat.active-calls',  x: 21, y: 15, w: 7,  h: 6 },
    { widgetId: 'cims.stat.ptt-groups',    x: 28, y: 15, w: 7,  h: 6 },
    { widgetId: 'cims.stat.rtp-voip',      x: 35, y: 15, w: 7,  h: 6 },
    { widgetId: 'cims.stat.rtp-ptt',       x: 42, y: 15, w: 6,  h: 6 },
    // ③ 시스템 — 형상(½) + 리소스(½)
    { widgetId: 'core.system-topology',    x: 0,  y: 21, w: 24, h: 14 },
    { widgetId: 'core.system-resource',    x: 24, y: 21, w: 24, h: 14 },
    // ④ 진행 중 + 추이 — 활성 VoIP · 활성 PTT · 호 시도 추이 3열
    { widgetId: 'cims.active-voip',        x: 0,  y: 35, w: 16, h: 13 },
    { widgetId: 'cims.active-ptt',         x: 16, y: 35, w: 16, h: 13 },
    { widgetId: 'shape.time-bar',          x: 32, y: 35, w: 16, h: 13, config: { source: 'cims.svc.volte' } },
  ],
  // 과감히 제거: cims.health-dots(→형상 노드/모듈 상태로 흡수), cims.alert-banner(→active-alarms),
  //            core.system-cards(→system-topology 로 대체), cims.csp-roles(단순 역할 플래그).
}
