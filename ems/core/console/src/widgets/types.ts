// 위젯(컴포넌트 합성) 모델 — OAM 플랫폼화 5-3 (L2).
//
// page = "고정 화면" 이 아니라 "위젯을 배치하는 레이아웃". 서비스 pack 은 page 가 아니라
// 위젯(WidgetDef)을 등록하고, layout(PageLayout)이 어떤 위젯을 어디에 배치할지 서술한다.
// 향후 menu/layout 을 file_store 에 저장 + 관리자 편집 UI 로 추가/삭제/배치 (5-3 step2).

import type { ComponentType } from 'react'

export interface WidgetProps {
  // 위젯 인스턴스 설정 (예: StatsMessages 의 iface). layout placement 의 config 가 주입됨.
  config?: Record<string, unknown>
  // 위젯이 자기 인스턴스 설정을 영속시키는 경로 (예: ShapeWidget 의 소스 선택). **편집 모드에서만**
  // 주입된다 — 뷰 모드의 조작은 일시적(로컬 state), 편집 모드의 조작은 draft→저장으로 남는다.
  // patch 는 기존 config 에 얕게 merge 된다(undefined 값 = 키 제거).
  onConfigChange?: (patch: Record<string, unknown>) => void
}

// 위젯 인스턴스 설정 항목 선언 — 편집기의 [⚙] 패널이 이 선언만 보고 폼을 그린다.
// select 의 options 는 함수도 허용한다 (shape 위젯의 소스처럼 목록이 런타임 카탈로그에서 오는 경우).
export interface WidgetConfigOption { value: string; label: string }
export interface WidgetConfigField {
  key: string
  label: string
  type: 'select' | 'text' | 'number' | 'bool'
  // 함수형 옵션은 현재 인스턴스 config 를 받는다 — 앞 필드 선택에 따라 목록이 달라지는 의존 필드
  // (예: 소스를 고르면 그 소스의 지표 목록이 나온다)를 위해.
  options?: WidgetConfigOption[] | ((config?: Record<string, unknown>) => WidgetConfigOption[])
  placeholder?: string
}

export interface WidgetDef {
  id: string                      // 'core.system-cards' / 'cims.active-voip' / 'page:/deploy/servers'
  title: string                   // 편집 UI/메뉴 표시용 (렌더러는 강제 안 함 — 위젯이 자체 chrome)
  category?: 'infra' | 'service' | 'stats' | 'metric' | 'event' | 'view' | 'control' | 'page'
  serviceId?: string              // undefined=코어, 'cims'=CIMS 서비스 pack
  component: ComponentType<WidgetProps>
  defaultSize?: { w: number; h?: number }  // 기본 폭(12-칸 기준 1~12, 미지정=12; grid 은 ×COL_SCALE 환산) + grid 기본 행 span(h).
  // 이 위젯이 쓸모를 유지하는 최소 크기(그리드 셀). 캔버스가 고정 예산이라 위젯을 키우면 다른
  // 위젯이 줄어드는데, 그 하한이 여기다(미지정 = gridLayout.MIN_ROWS). 표·차트처럼 몇 행부터
  // 읽을 수 있는지가 분명한 위젯만 선언한다.
  minSize?: { w?: number; h?: number }
  adminOnly?: boolean
  // 이 위젯 인스턴스가 갖는 설정 항목 — 편집기 [⚙] 패널이 그린다. 배치(placement.config)에 저장돼
  // 같은 위젯을 다른 설정으로 여러 벌 놓을 수 있다(예: 소스가 다른 시계열 차트 2개).
  configFields?: WidgetConfigField[]
  // 이 위젯이 페이지 파라미터(기간/단위 등)를 소비하는가 — `core.page-filter` 가 같은 페이지에
  // 있으면 자체 컨트롤을 접고 버스 값을 따른다(pageParams.tsx). 선언은 편집 UI 표기용.
  usesPageParams?: boolean
  // 이 위젯이 호출하는 API 의 id 목록 (백엔드 `*_API_DOCS` 의 id). 개발자 모드에서 위젯에 [API]
  // 배지로 노출된다. **id 만** 선언한다 — 경로/파라미터/응답 등 내용은 백엔드가 소유(api_docs.md).
  apis?: string[]
  // 카드 안 기본 배치 — 이 위젯이 "여러 블록을 담은 카드"면 그 안의 배치를 여기 선언한다
  // (widgets/CardLayout.tsx). 좌표계는 바깥과 같은 48×48 셀이라 편집기가 그대로 재사용된다.
  // 운영자가 카드 안을 편집하면 `placement.config.layout` 에 저장돼 그것이 우선한다.
  cardLayout?: WidgetPlacement[]
  // 배치 설정에 따라 호출 API 가 갈리는 위젯(소스 선택형 shape)은 API id 를 정적으로 못 적는다.
  // 대신 이 배치가 쓰는 **데이터 소스 id** 를 돌려주면, 배지가 카탈로그에서 그 소스의 endpoint 를
  // 찾아 API 문서의 path 와 대조해 id 를 얻는다. 둘 다 이미 존재하는 정보라 새 중복 선언이 아니다.
  // `apis` 와 합집합으로 표시된다.
  apiSources?: (config: Record<string, unknown> | undefined) => string[]
}

// 레이아웃에 배치된 위젯 1개. 두 배치 모드가 하위호환으로 공존한다:
//  · legacy flow: x/y 없음 → GridRenderer 가 순서+w 로 flow(합>12 wrap). h 는 vh(1~100)|px(>100).
//  · 2D grid:     x/y 있음 → 절대 셀 좌표 배치. w=열 span, h=행 span(모두 그리드 셀 단위로 통일).
//                 세로는 **고정 예산**(gridLayout.GRID_ROWS)이라 y+h 가 그 값을 넘을 수 없다.
// 편집 진입 시 legacy 를 grid 로 1회 migrate(gridLayout.flowToGrid). 판별은 gridLayout.isGridPlacement.
export interface WidgetPlacement {
  widgetId: string
  x?: number                      // grid: 0-based 열(0..GRID_COLS-1). 없으면 legacy flow 배치.
  y?: number                      // grid: 0-based 행(>=0).  없으면 legacy flow 배치.
  w?: number                      // grid: 열 span(1..GRID_COLS). legacy: 12-칸 기준. 미지정 시 defaultSize.w.
  h?: number                      // grid: 행 span(>=1 정수). legacy: 높이 vh(1~100)|px(>100), 미지정=자동.
  config?: Record<string, unknown>
  // 조건부 표시 — 페이지 파라미터가 이 값일 때만 보인다. 한 화면을 탭처럼 갈아끼울 때 쓴다
  // (예: 유형별 분석의 [알람]/[이벤트]). 숨겨진 배치는 렌더에서 빠지고 남은 것이 위로 당겨진다.
  // **편집 모드에서는 조건과 무관하게 모두 보인다** — 숨은 위젯도 배치해야 하므로.
  visibleWhen?: { param: string; equals: string }
  // 잠금 — 이 배치의 위치·크기를 고정한다. 다른 위젯을 키울 때 밀리지도 줄어들지도 않는다.
  // 캔버스가 고정 예산이라 "어디를 지킬지"를 운영자가 지정하는 장치다(console_platform §3.0).
  locked?: boolean
  // 이 배치의 표시 이름 — **옛 저장본 호환 전용**. 편집 UI 에서는 더 이상 붙일 수 없다:
  // 관제 화면에서 위젯 이름을 바꾸면 그게 원래 무엇인지 가려져 위험하다(identifier_model —
  // 동작은 불변 id 로, 표시는 정의된 이름으로). 남아 있는 값은 뷰에서 캡션으로 계속 그린다.
  title?: string
}

// page = 위젯 배치 서술. (step2 에서 file_store 영속 + 편집)
export interface PageLayout {
  id: string                      // 'dashboard'
  title?: string
  gap?: number                    // 카드 간 간격(px). 미지정 시 기본 간격(gridLayout.GRID_GAP).
  widgets: WidgetPlacement[]
  // seed 세대. seed 를 개편하면(위젯 분해 등) 올린다 — 저장본의 값이 더 낮으면 EditableLayout 이
  // "기본 레이아웃이 갱신됨" 배너를 띄운다. 저장본이 seed 를 영구히 고정하는 문제의 해소 경로.
  // 저장본에는 저장 시점의 seed 값이 그대로 실려 나간다(PUT 이 필드를 통째 보존).
  seedVersion?: number
}
