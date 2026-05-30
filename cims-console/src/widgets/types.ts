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
  id: string                      // 'core.system-cards' / 'cims.active-voip'
  title: string                   // 편집 UI/메뉴 표시용 (렌더러는 강제 안 함 — 위젯이 자체 chrome)
  category?: 'infra' | 'service' | 'stats' | 'event' | 'view'
  serviceId?: string              // undefined=코어, 'cims'=CIMS 서비스 pack
  component: ComponentType<WidgetProps>
  defaultSize?: { w: number }     // 12-col grid 기준 기본 폭 (1~12). 미지정 시 12.
  adminOnly?: boolean
}

// 레이아웃에 배치된 위젯 1개.
export interface WidgetPlacement {
  widgetId: string
  w?: number                      // 12-col span. 미지정 시 WidgetDef.defaultSize.w → 12.
  h?: number                      // 높이(px). 지정 시 그 높이로 고정 + 내부 스크롤. 미지정=자동(내용 높이).
  config?: Record<string, unknown>
}

// page = 위젯 배치 서술. (step2 에서 file_store 영속 + 편집)
export interface PageLayout {
  id: string                      // 'dashboard'
  title?: string
  widgets: WidgetPlacement[]
}
