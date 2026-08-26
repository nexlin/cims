// 위젯 레지스트리 — 코어 위젯 + 서비스 pack 이 기여한 위젯(ServiceManifest.widgets) 병합.
// 새 위젯: 코어면 CORE_WIDGETS 에, 서비스면 해당 manifest.widgets 에 추가.

import type { WidgetDef } from './types'
import { CORE_SPLITS, type SplitFn } from './legacySplit'
import { SERVICE_MANIFESTS } from '../services/registry'
import { systemCardsWidget } from './core/SystemCardsWidget'
import { systemTopologyWidget } from './core/SystemTopologyWidget'
import { systemResourceWidget } from './core/SystemResourceWidget'
import { pageFilterWidget } from './core/PageFilterWidget'
import { sourcePickerWidget } from './core/SourcePickerWidget'
import { FAULT_WIDGETS } from './core/faultWidgets'
import { SERVICE_DEF_WIDGETS } from './core/serviceDefWidgets'
import { SHAPE_WIDGETS } from './shapes/ShapeWidget'

// 코어(서비스 무지) 위젯 — 인프라/시스템 + 범용 shape 위젯(소스 선택형).
const CORE_WIDGETS: WidgetDef[] = [
  systemCardsWidget,
  systemTopologyWidget,
  systemResourceWidget,
  pageFilterWidget,
  sourcePickerWidget,
  ...FAULT_WIDGETS,
  ...SERVICE_DEF_WIDGETS,
  ...SHAPE_WIDGETS,
]

// 런타임 등록 위젯 — App 이 라우트 컴포넌트를 'page:<path>' 위젯으로 주입(모든 메뉴 페이지를
// 위젯 합성 surface 로 편집 가능하게). registry 내 정적 import 로 잡기엔 순환이라 외부 주입.
const _extra: WidgetDef[] = []
export function registerWidgets(defs: WidgetDef[]) {
  let changed = false
  for (const d of defs) {
    if (!_extra.some(e => e.id === d.id)) { _extra.push(d); changed = true }
  }
  if (changed) { _all = null; _byId = null }   // 캐시 무효화 → 다음 ensure() 에서 재집계
}

// SERVICE_MANIFESTS 는 순환 import(registry↔manifest↔EditableLayout) 상 모듈 eval 시점엔
// 부분 초기화일 수 있어 lazy 집계 — 첫 호출(렌더 시점)엔 완전 초기화 보장.
let _all: WidgetDef[] | null = null
let _byId: Map<string, WidgetDef> | null = null
function ensure() {
  if (_all && _byId) return
  const service = SERVICE_MANIFESTS.flatMap(
    m => (m.widgets ?? []).map(w => ({ ...w, serviceId: w.serviceId ?? m.id }))
  )
  _all = [...CORE_WIDGETS, ...service, ..._extra]
  _byId = new Map(_all.map(w => [w.id, w]))
}

export function getWidget(id: string): WidgetDef | undefined {
  ensure()
  return _byId!.get(id)
}

export function allWidgets(): WidgetDef[] {
  ensure()
  return _all!
}

// 폐지된 묶음 위젯 → 부품 전개 규칙 (코어 + 서비스 pack 기여분). legacySplit.expandSplits 가 쓴다.
let _splits: Record<string, SplitFn> | null = null
export function splitFor(widgetId: string): SplitFn | undefined {
  if (!_splits) {
    _splits = { ...CORE_SPLITS }
    for (const m of SERVICE_MANIFESTS) Object.assign(_splits, m.splits ?? {})
  }
  return _splits[widgetId]
}

// 위젯 카테고리 — 편집 UI 그룹핑용. 표시 순서 + 한글 라벨.
export type WidgetCategory =
  'infra' | 'service' | 'stats' | 'metric' | 'view' | 'control' | 'event' | 'page' | 'etc'
export const WIDGET_CATEGORY_ORDER: WidgetCategory[] =
  ['metric', 'infra', 'service', 'stats', 'view', 'control', 'event', 'page', 'etc']
export const WIDGET_CATEGORY_LABELS: Record<WidgetCategory, string> = {
  metric: '현황 지표 (단일 카드)', infra: '인프라/시스템', service: '서비스', stats: '통계',
  view: '데이터 뷰 (소스 선택)', control: '컨트롤 (조회 조건)',
  event: '이벤트/알람', page: '페이지 (전체 화면)', etc: '기타',
}

// 카테고리 → 위젯 목록 (위젯이 늘어날 때 편집 드롭다운을 성격별로 묶기 위함).
// 빈 카테고리는 생략, 표시 순서는 WIDGET_CATEGORY_ORDER 를 따른다.
// query: 편집 UI 검색어 — 제목/id 부분일치로 걸러낸다(위젯 수가 많아 드롭다운 스캔이 힘들어짐).
export function widgetsByCategory(query = ''): { category: WidgetCategory; label: string; widgets: WidgetDef[] }[] {
  const needle = query.trim().toLowerCase()
  const all = needle
    ? allWidgets().filter(w => `${w.title} ${w.id}`.toLowerCase().includes(needle))
    : allWidgets()
  return WIDGET_CATEGORY_ORDER
    .map(cat => ({
      category: cat,
      label: WIDGET_CATEGORY_LABELS[cat],
      widgets: all.filter(w => (w.category ?? 'etc') === cat),
    }))
    .filter(g => g.widgets.length > 0)
}
