// CIMS 출력 섹션의 기본 레이아웃 seed — 각 출력 route 가 EditableLayout 으로 합성.
// layout_id 는 route 경로에서 파생(예: '/stats/volte' → 'stats.volte'). 저장본 없으면 이 seed.
// 운영자는 [✎ 편집]으로 어느 페이지에든 위젯을 추가/제거/배치할 수 있다.
//
// 좌표계 = 2D 그리드 셀(gridLayout): x/w 는 48칸 기준 열, y/h 는 행(1행 = 화면 세로 2%).
// 통계 페이지는 **페이지 통짜 위젯을 쓰지 않는다** — 조회 조건(core.page-filter) 한 줄 + shape 위젯
// (지표/추이/분포/표) 조합이다. shape 위젯은 소스만 다르므로 신규 컴포넌트가 없고, 소스는 Service
// Descriptor `data_sources[]` 등록분(console_platform §4)을 config.source 로 가리킨다.
import type { PageLayout } from '@core/widgets/types'

const one = (id: string, title: string, widgetId: string,
             config?: Record<string, unknown>): PageLayout => ({
  id, title, widgets: [{ widgetId, config }],
})

// 서비스 통계(VoLTE/PTT) = 지표 카드 N개 + 추이 + 분포. **지표는 1개 = 카드 1개**,
// 소스는 메뉴가 이미 대상별로 나뉘어 있으므로 고정(선택 UI 없음. item = kpi 계약의 0-based 인덱스).
const svcStatsLayout = (id: string, title: string, source: string, stats: number,
                        trend: string, dist: string): PageLayout => {
  const w = Math.floor(48 / stats)
  return {
    id, title, seedVersion: 5,
    widgets: [
      { widgetId: 'core.page-filter', x: 0, y: 0, w: 48, h: 4 },
      ...Array.from({ length: stats }, (_, i) => ({
        widgetId: 'shape.stat', x: i * w, y: 4, w, h: 7, config: { source, item: i },
      })),
      { widgetId: 'shape.time-bar',     x: 0,  y: 11, w: 26, h: 20, config: { source, title: trend } },
      { widgetId: 'shape.distribution', x: 26, y: 11, w: 22, h: 20, config: { source, title: dist } },
    ],
  }
}

// 메시지 통계 = 인터페이스(SIP/CMP/CSC/HTTPS)를 한 화면에서 갈아 보는 구성. 대상 선택은 위젯이 아니라
// **화면 공통 조건**(core.source-picker → `src`)으로 두어 차트·표가 함께 움직인다.
// 후보는 **인터페이스**만 — 전 인터페이스 합계(cims.msg-summary)는 표 계약이 없어 같은 축에 못 둔다.
const MSG_IFACES = ['cims.msg.sip', 'cims.msg.cmp', 'cims.msg.csc', 'cims.msg.https']
const MSG_SOURCE = MSG_IFACES[0]
export const STATS_MESSAGES_LAYOUT: PageLayout = {
  id: 'stats.messages', title: '메시지 통계', seedVersion: 2,
  widgets: [
    { widgetId: 'core.page-filter',   x: 0,  y: 0, w: 48, h: 4 },
    // 인터페이스 선택은 화면 공통 조건(`src`) — 차트와 표가 같은 대상을 함께 본다.
    // 후보를 열거해 이 화면과 무관한 소스(VoLTE/PTT 서비스 KPI)가 섞이지 않게 한다.
    { widgetId: 'core.source-picker', x: 0,  y: 4, w: 48, h: 3,
      config: { sources: MSG_IFACES.join(',') } },
    { widgetId: 'shape.time-bar',     x: 0,  y: 7, w: 29, h: 22,
      config: { source: MSG_SOURCE, title: '시간대별 메시지' } },
    { widgetId: 'shape.table',        x: 29, y: 7, w: 19, h: 22,
      config: { source: MSG_SOURCE, title: '메서드별 건수' } },
  ],
}

// 누수 회수(sweeper) — 다른 통계 화면과 같은 구성: 조회 조건 + 지표 카드 낱개 + 목록.
// 집계 단위(시간/일/월)는 쓰지 않는 일자 조회라 단위 버튼은 감춘다.
export const STATS_LEAK_LAYOUT: PageLayout = {
  id: 'stats.leak-reclaims', title: '누수 회수(sweeper)', seedVersion: 2,
  widgets: [
    { widgetId: 'core.page-filter', x: 0,  y: 0,  w: 48, h: 4, config: { showGran: false } },
    { widgetId: 'cims.leak.total',   x: 0,  y: 4,  w: 12, h: 7 },
    { widgetId: 'cims.leak.orphan',  x: 12, y: 4,  w: 12, h: 7 },
    { widgetId: 'cims.leak.hold',    x: 24, y: 4,  w: 12, h: 7 },
    { widgetId: 'cims.leak.by-node', x: 36, y: 4,  w: 12, h: 7 },
    { widgetId: 'cims.leak.list',    x: 0,  y: 11, w: 48, h: 32 },
  ],
}

// 서비스 섹션 — 서비스 현황은 섹션별 개별 위젯을 합성(운영자가 ✎ 편집으로 재배치 가능).
// 탭 통합 위젯(cims.svc-detail)은 등록만 남기고 seed 에서 빼 개별 위젯으로 되돌렸다 — 한 화면에서
// 무엇이 보이는지를 배치가 그대로 드러내야 편집/재배치가 의미를 갖는다.
export const SERVICE_STATUS_LAYOUT: PageLayout = {
  id: 'service.status', title: '서비스 현황', seedVersion: 4,
  widgets: [
    // 요약은 **대상별로 묶는다** — VoLTE 는 VoLTE 끼리, PTT 는 PTT 끼리. 통화 중/호출 중/평균 통화/
    // 등록·RTP 풀은 한 대상의 상태를 함께 봐야 읽히므로 지표별로 쪼개지 않는다.
    { widgetId: 'cims.svc-volte-kpi', x: 0,  y: 0,  w: 24, h: 12 },
    { widgetId: 'cims.svc-ptt-kpi',   x: 24, y: 0,  w: 24, h: 12 },
    { widgetId: 'cims.svc-trend',     x: 0,  y: 12, w: 24, h: 18 },
    { widgetId: 'cims.svc-anomaly',   x: 24, y: 12, w: 24, h: 18 },
    // 호·그룹·이벤트·부서·조회는 한 영역에서 탭으로 오가며 보는 상세라 한 위젯이다.
    { widgetId: 'cims.svc-detail',    x: 0,  y: 30, w: 48, h: 30 },
  ],
}
export const SERVICE_HISTORY_VOLTE_LAYOUT = one('service.history-volte', 'VoLTE 이력', 'cims.volte-history')
export const SERVICE_HISTORY_PTT_LAYOUT   = one('service.history-ptt', 'PTT 이력', 'cims.ptt-history')

// 통계 섹션 — 소스는 descriptor 등록 id (service_descriptors_seed/cims.json 의 data_sources[]).
// 지표 수 = 소스가 선언한 kpi 항목 수 (descriptor cims.json 의 map.kpi.items)
//   volte: 호 시도·호 성공률·평균 통화시간·성공 (4) / ptt: 그룹콜 수·평균 세션 시간 (2)
export const STATS_VOLTE_LAYOUT =
  svcStatsLayout('stats.volte', 'VoLTE 통계', 'cims.svc.volte', 4, '호 시도 추이', '종료 사유 분포')
export const STATS_PTT_LAYOUT =
  svcStatsLayout('stats.ptt', 'PTT 통계', 'cims.svc.ptt', 2, '그룹콜 수 추이', '그룹별 사용 빈도')
