// 위젯(컴포넌트 합성) 모델 — OAM 플랫폼화 5-3 (L2).
//
// page = "고정 화면" 이 아니라 "위젯을 배치하는 레이아웃". 서비스 pack 은 page 가 아니라
// 위젯(WidgetDef)을 등록하고, layout(PageLayout)이 어떤 위젯을 어디에 배치할지 서술한다.
// 향후 menu/layout 을 file_store 에 저장 + 관리자 편집 UI 로 추가/삭제/배치 (5-3 step2).

import type { ComponentType } from 'react'

export interface WidgetProps {
  // 위젯 인스턴스 설정 (예: StatsMessages 의 iface). layout placement 의 config 가 주입됨.
  config?: Record<string, unknown>
}

export interface WidgetDef {
  id: string                      // 'core.system-cards' / 'cims.active-voip' / 'page:/deploy/servers'
  title: string                   // 편집 UI/메뉴 표시용 (렌더러는 강제 안 함 — 위젯이 자체 chrome)
  category?: 'infra' | 'service' | 'stats' | 'event' | 'view' | 'page'
  serviceId?: string              // undefined=코어, 'cims'=CIMS 서비스 pack
  component: ComponentType<WidgetProps>
  defaultSize?: { w: number; h?: number }  // 기본 폭(12-칸 기준 1~12, 미지정=12; grid 은 ×COL_SCALE 환산) + grid 기본 행 span(h).
  adminOnly?: boolean
}

// 레이아웃에 배치된 위젯 1개. 두 배치 모드가 하위호환으로 공존한다:
//  · legacy flow: x/y 없음 → GridRenderer 가 순서+w 로 flow(합>12 wrap). h 는 vh(1~100)|px(>100).
//  · 2D grid:     x/y 있음 → 절대 셀 좌표 배치. w=열 span, h=행 span(모두 그리드 셀 단위로 통일).
// 편집 진입 시 legacy 를 grid 로 1회 migrate(gridLayout.flowToGrid). 판별은 gridLayout.isGridPlacement.
export interface WidgetPlacement {
  widgetId: string
  x?: number                      // grid: 0-based 열(0..GRID_COLS-1). 없으면 legacy flow 배치.
  y?: number                      // grid: 0-based 행(>=0).  없으면 legacy flow 배치.
  w?: number                      // grid: 열 span(1..GRID_COLS). legacy: 12-칸 기준. 미지정 시 defaultSize.w.
  h?: number                      // grid: 행 span(>=1 정수). legacy: 높이 vh(1~100)|px(>100), 미지정=자동.
  config?: Record<string, unknown>
}

// page = 위젯 배치 서술. (step2 에서 file_store 영속 + 편집)
export interface PageLayout {
  id: string                      // 'dashboard'
  title?: string
  gap?: number                    // 카드 간 간격(px). 미지정 시 기본 간격(gridLayout.GRID_GAP).
  widgets: WidgetPlacement[]
}
