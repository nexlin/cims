// 내 대시보드 구성 화면 위젯 — 상태·프로파일·위젯 목록 3블록과 그것을 담은 카드.
//
// 화면 자체는 "고르고 저장하는" 한 벌(작업 트랜잭션)이라 배치 단위는 **카드 하나**다.
// 다만 카드 안은 바깥과 같은 48×48 셀이라(console_platform §3.0.1) 세 블록의 자리·크기는
// 운영자가 카드 안 편집(`[⚙]`)으로 바꿀 수 있다. 세 블록은 같은 편집 초안을 보므로 상태를
// 모듈 store(`pages/myLayoutStore.ts`)에 둔다.
import { MyLayoutHeader, MyLayoutProfile, MyLayoutWidgets } from '../../pages/MyLayoutPage'
import { makeCardWidget } from '../CardLayout'
import { GRID_ROWS } from '../gridLayout'
import type { WidgetDef, WidgetPlacement } from '../types'

const API = ['console.catalog', 'console.profiles', 'console.layouts.me']

export const myLayoutHeaderWidget: WidgetDef = {
  id: 'core.my-layout.header', title: '내 대시보드 — 상태·저장', category: 'control',
  component: MyLayoutHeader, apis: API, defaultSize: { w: 12, h: 5 },
}
export const myLayoutProfileWidget: WidgetDef = {
  id: 'core.my-layout.profile', title: '내 대시보드 — 프로파일', category: 'view',
  component: MyLayoutProfile, apis: API, defaultSize: { w: 12, h: 7 }, minSize: { h: 6 },
}
export const myLayoutWidgetsWidget: WidgetDef = {
  id: 'core.my-layout.widgets', title: '내 대시보드 — 위젯 목록', category: 'view',
  component: MyLayoutWidgets, apis: API, defaultSize: { w: 12, h: 36 }, minSize: { h: 10 },
}

// 카드 안 기본 배치 — 세로 합 = GRID_ROWS(화면 한 장).
export const MY_LAYOUT_CARD_LAYOUT: WidgetPlacement[] = [
  { widgetId: 'core.my-layout.header',  x: 0, y: 0,  w: 48, h: 5 },
  { widgetId: 'core.my-layout.profile', x: 0, y: 5,  w: 48, h: 7 },
  { widgetId: 'core.my-layout.widgets', x: 0, y: 12, w: 48, h: 36 },
]

export const myLayoutCardWidget: WidgetDef = makeCardWidget({
  id: 'core.my-layout', title: '내 대시보드 구성 화면', category: 'view',
  defaultSize: { w: 12, h: GRID_ROWS }, layout: MY_LAYOUT_CARD_LAYOUT,
})

export const MY_LAYOUT_WIDGETS: WidgetDef[] = [
  myLayoutCardWidget, myLayoutHeaderWidget, myLayoutProfileWidget, myLayoutWidgetsWidget,
]
