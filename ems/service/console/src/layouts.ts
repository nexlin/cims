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
             config?: Record<string, unknown>, seedVersion?: number): PageLayout => ({
  id, title, widgets: [{ widgetId, config }], ...(seedVersion ? { seedVersion } : {}),
})

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

// 서비스 통계(VoLTE/PTT) — `core.page-filter` + 지표 카드 낱개 + 추이 + 분포.
// 메시지는 여기에 두지 않는다: 메시지를 서비스별로 가른 화면은 어느 메뉴에 있든 내용이 같아
// (VoLTE 메뉴의 메시지가 아니라 "전체 메시지를 서비스별로 가른 것") 같은 페이지를 두 군데
// 걸어두는 셈이었다. 서비스축 메시지는 `메시지 통계` 메뉴 하나로 뺐다.
const svcStats = (id: string, title: string, source: string, stats: number,
                  trend: string, dist: string): PageLayout => {
  const w = Math.floor(48 / stats)
  return {
    id, title, seedVersion: 8,
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
export const STATS_VOLTE_LAYOUT =
  svcStats('stats.volte', 'VoLTE 통계', 'cims.svc.volte', 4, '호 시도 추이', '종료 사유 분포')
export const STATS_PTT_LAYOUT =
  svcStats('stats.ptt', 'PTT 통계', 'cims.svc.ptt', 2, '그룹콜 수 추이', '그룹별 사용 빈도')

// 인터페이스 통계 — 옛 SIP/CMP/CSC/HTTPS 4개 메뉴를 한 화면으로 합친다. 넷은 보는 값이 같고
// 대상만 달랐다(메뉴가 아니라 **조회 조건**). 대상 전환은 core.source-picker(`src`)가 소유하고
// 같은 페이지의 shape 위젯들이 함께 따라간다 — 차트는 SIP, 표는 CMP 인 상태가 생기지 않는다.
//
// 서비스축(VoLTE/PTT)은 **메뉴가 아니라 계열**이다. `core.series-select` 가 파라미터 `series` 를
// 소유하고, 시계열과 메서드 비중이 **함께** 그 선택을 따른다 — 한 화면의 두 그림이 다른 대상을
// 보지 않게. '전체 메시지' 타일은 계열이 아니라 전부 선택 버튼이다(계열이 겹치지 않으므로
// 전부 켠 막대가 곧 전체다).
export const STATS_IFACE_LAYOUT: PageLayout = {
  id: 'stats.interfaces', title: '인터페이스 통계', seedVersion: 9,
  widgets: [
    { widgetId: 'core.page-filter', x: 0, y: 0, w: 48, h: 4 },
    { widgetId: 'core.source-picker', x: 0, y: 4, w: 48, h: 3,
      config: { shape: 'time-bar', sources: 'cims.msg.sip, cims.msg.cmp, cims.msg.csc, cims.msg.https' } },
    { widgetId: 'core.series-select', x: 0, y: 7, w: 48, h: 8,
      config: { source: 'cims.msg.sip', allLabel: '전체 메시지' } },
    { widgetId: 'shape.series-bar', x: 0, y: 15, w: 30, h: 24,
      config: { source: 'cims.msg.sip', title: '시간대별 메시지 수' } },
    { widgetId: 'shape.distribution', x: 30, y: 15, w: 18, h: 24,
      config: { source: 'cims.msg.sip', title: '메서드 비중' } },
    { widgetId: 'shape.table', x: 0, y: 39, w: 48, h: 20,
      config: { source: 'cims.msg.sip', title: '메서드별 카운트' } },
  ],
}
