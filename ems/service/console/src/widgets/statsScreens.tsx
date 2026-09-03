// 성능(통계) 화면 — **화면 하나 = 카드 하나**.
//
// 조회 조건(구간·단위) · 지표 · 추이 · 분포 · 표는 같은 구간을 함께 읽는 한 벌이라, 하나만 떼어
// 놓으면 화면이 성립하지 않는다(무엇을 언제 기준으로 본 값인지 사라진다). 그래서 배치 단위는
// 카드 하나로 두고, 카드 안의 구성은 아래 선언이 정본이다(`WidgetDef.cardLayout`).
//
// 좌표계는 바깥 캔버스와 같은 **48×48 셀**이라 운영자가 카드 안을 같은 편집기로 재배치할 수 있고,
// 그 결과는 `placement.config.layout` 에 저장돼 이 기본 선언을 덮는다.
// 블록은 코어 위젯(`core.page-filter` / `core.source-picker` / `core.series-select` / `shape.*`)을
// id 로 그대로 쓴다 — 새 컴포넌트가 없고, 소스는 Service Descriptor `data_sources[]` 등록분
// (console_platform §4)을 config.source 로 가리킨다.
import { makeCardWidget } from '@core/widgets/CardLayout'
import { GRID_COLS, GRID_ROWS } from '@core/widgets/gridLayout'
import type { WidgetDef, WidgetPlacement } from '@core/widgets/types'

// 서비스 통계(VoLTE/PTT) — 조회 조건 / 지표 카드 낱개 / 추이 · 분포.
const svcLayout = (source: string, stats: number, trend: string, dist: string): WidgetPlacement[] => {
  const w = Math.floor(GRID_COLS / stats)
  return [
    { widgetId: 'core.page-filter', x: 0, y: 0, w: GRID_COLS, h: 4 },
    ...Array.from({ length: stats }, (_, i) => ({
      widgetId: 'shape.stat', x: i * w, y: 4, w, h: 7, config: { source, item: i },
    })),
    { widgetId: 'shape.time-bar', x: 0, y: 11, w: 26, h: 37, config: { source, title: trend } },
    { widgetId: 'shape.distribution', x: 26, y: 11, w: 22, h: 37, config: { source, title: dist } },
  ]
}

// 인터페이스 통계 — 옛 SIP/CMP/CSC/HTTPS 4개 메뉴를 한 화면으로 합친 것. 넷은 보는 값이 같고
// 대상만 달랐다(메뉴가 아니라 **조회 조건**). 대상 전환은 `core.source-picker`(`src`)가 소유하고
// 카드 안의 shape 블록들이 함께 따라간다 — 차트는 SIP, 표는 CMP 인 상태가 생기지 않는다.
//
// 서비스축(VoLTE/PTT)은 메뉴가 아니라 **계열**이다. `core.series-select` 가 파라미터 `series` 를
// 소유하고, 시계열과 메서드 비중이 함께 그 선택을 따른다. '전체 메시지' 타일은 계열이 아니라 전부
// 선택 버튼이다(계열이 겹치지 않으므로 전부 켠 막대가 곧 전체다).
const IFACE_SOURCE = 'cims.msg.sip'
const ifaceLayout: WidgetPlacement[] = [
  { widgetId: 'core.page-filter', x: 0, y: 0, w: 48, h: 4 },
  { widgetId: 'core.source-picker', x: 0, y: 4, w: 48, h: 3,
    config: { shape: 'time-bar', sources: 'cims.msg.sip, cims.msg.cmp, cims.msg.csc, cims.msg.https' } },
  { widgetId: 'core.series-select', x: 0, y: 7, w: 48, h: 7,
    config: { source: IFACE_SOURCE, allLabel: '전체 메시지' } },
  { widgetId: 'shape.series-bar', x: 0, y: 14, w: 30, h: 20,
    config: { source: IFACE_SOURCE, title: '시간대별 메시지 수' } },
  { widgetId: 'shape.distribution', x: 30, y: 14, w: 18, h: 20,
    config: { source: IFACE_SOURCE, title: '메서드 비중' } },
  { widgetId: 'shape.table', x: 0, y: 34, w: 48, h: 14,
    config: { source: IFACE_SOURCE, title: '메서드별 카운트' } },
]

// 화면별 카드 안 기본 배치 — 세로 합 = GRID_ROWS = **화면 한 장**.
export const STATS_SCREEN_LAYOUTS = {
  volte: svcLayout('cims.svc.volte', 4, '호 시도 추이', '종료 사유 분포'),
  ptt: svcLayout('cims.svc.ptt', 2, '그룹콜 수 추이', '그룹별 사용 빈도'),
  interfaces: ifaceLayout,
} as const

const screen = (key: keyof typeof STATS_SCREEN_LAYOUTS, id: string, title: string): WidgetDef =>
  makeCardWidget({
    id, title, category: 'stats', usesPageParams: true,
    defaultSize: { w: 12, h: GRID_ROWS },
    layout: STATS_SCREEN_LAYOUTS[key],
  })

export const STATS_SCREEN_WIDGETS: WidgetDef[] = [
  screen('volte', 'cims.stats.volte', 'VoLTE 통계 화면'),
  screen('ptt', 'cims.stats.ptt', 'PTT 통계 화면'),
  screen('interfaces', 'cims.stats.interfaces', '인터페이스 통계 화면'),
]
