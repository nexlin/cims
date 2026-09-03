// CIMS 출력 섹션의 기본 레이아웃 seed — 각 출력 route 가 EditableLayout 으로 합성.
// layout_id 는 route 경로에서 파생(예: '/stats/volte' → 'stats.volte'). 저장본 없으면 이 seed.
// 운영자는 [✎ 편집]으로 어느 페이지에든 위젯을 추가/제거/배치할 수 있다.
//
// 좌표계 = 2D 그리드 셀(gridLayout): x/w 는 48칸 기준 열, y/h 는 행(1행 = 화면 세로 2%).
import type { PageLayout } from '@core/widgets/types'
import { GRID_ROWS } from '@core/widgets/gridLayout'

// 화면 전체가 카드 하나 — 캔버스(48×48)를 통째로 차지한다. 높이를 지정해야 카드 안 블록이
// 비율대로 나뉘고, 넘치는 내용은 카드 안에서 스크롤한다(console_platform §3.0).
const oneCard = (id: string, title: string, widgetId: string, seedVersion: number): PageLayout => ({
  id, title, seedVersion, widgets: [{ widgetId, x: 0, y: 0, w: 48, h: GRID_ROWS }],
})

// 서비스 섹션 — 서비스 현황은 섹션별 개별 위젯을 합성(운영자가 ✎ 편집으로 재배치 가능).
// 탭 통합 위젯(cims.svc-detail)은 등록만 남기고 seed 에서 빼 개별 위젯으로 되돌렸다 — 한 화면에서
// 무엇이 보이는지를 배치가 그대로 드러내야 편집/재배치가 의미를 갖는다.
export const SERVICE_STATUS_LAYOUT: PageLayout = {
  // 5세대: 화면 한 장(48행)에 맞춤 — 예전 60행은 페이지가 스크롤됐다.
  id: 'service.status', title: '서비스 현황', seedVersion: 5,
  widgets: [
    // 요약은 **대상별로 묶는다** — VoLTE 는 VoLTE 끼리, PTT 는 PTT 끼리. 통화 중/호출 중/평균 통화/
    // 등록·RTP 풀은 한 대상의 상태를 함께 봐야 읽히므로 지표별로 쪼개지 않는다.
    { widgetId: 'cims.svc-volte-kpi', x: 0,  y: 0,  w: 24, h: 11 },
    { widgetId: 'cims.svc-ptt-kpi',   x: 24, y: 0,  w: 24, h: 11 },
    { widgetId: 'cims.svc-trend',     x: 0,  y: 11, w: 24, h: 14 },
    { widgetId: 'cims.svc-anomaly',   x: 24, y: 11, w: 24, h: 14 },
    // 호·그룹·이벤트·부서·조회는 한 영역에서 탭으로 오가며 보는 상세라 한 위젯이다.
    { widgetId: 'cims.svc-detail',    x: 0,  y: 25, w: 48, h: 23 },
  ],
}
// 비정상 세션 이력 — 화면 전체가 카드 하나(조회 조건 · 지표 · 발신 IP · 세션 표).
export const ABNORMAL_SESSIONS_LAYOUT =
  oneCard('service.abnormal-sessions', '비정상 세션 이력', 'cims.abnormal-sessions', 1)

export const SERVICE_HISTORY_VOLTE_LAYOUT =
  oneCard('service.history-volte', 'VoLTE 이력', 'cims.volte-history', 1)
export const SERVICE_HISTORY_PTT_LAYOUT =
  oneCard('service.history-ptt', 'PTT 이력', 'cims.ptt-history', 1)

// 성능(통계) 화면은 **화면 하나 = 카드 하나**다 — 조회 조건·지표·추이·분포·표는 같은 구간을 함께
// 읽는 한 벌이라 하나만 떼면 화면이 성립하지 않는다. 카드 안의 블록 구성은 `widgets/statsScreens.tsx`
// 선언 표가 정본이고, 여기서는 그 카드를 어느 화면에 얼마만 한 크기로 놓을지만 정한다.
// 메시지는 VoLTE/PTT 화면에 두지 않는다: 메시지를 서비스별로 가른 화면은 어느 메뉴에 있든 내용이
// 같아(VoLTE 메뉴의 메시지가 아니라 "전체 메시지를 서비스별로 가른 것") 같은 페이지를 두 군데
// 걸어두는 셈이었다. 서비스축 메시지는 `인터페이스 통계` 메뉴 하나로 뺐다.
export const STATS_VOLTE_LAYOUT = oneCard(
  'stats.volte', 'VoLTE 통계', 'cims.stats.volte', 10)
export const STATS_PTT_LAYOUT = oneCard(
  'stats.ptt', 'PTT 통계', 'cims.stats.ptt', 10)
export const STATS_IFACE_LAYOUT = oneCard(
  'stats.interfaces', '인터페이스 통계', 'cims.stats.interfaces', 11)
