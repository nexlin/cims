// 위젯 레지스트리 — 코어 위젯 + 서비스 pack 이 기여한 위젯(ServiceManifest.widgets) 병합.
// 새 위젯: 코어면 CORE_WIDGETS 에, 서비스면 해당 manifest.widgets 에 추가.

import type { WidgetDef } from './types'
import { SERVICE_MANIFESTS } from '../services/registry'
import { systemCardsWidget } from './core/SystemCardsWidget'
import { systemTopologyWidget } from './core/SystemTopologyWidget'
import { SHAPE_WIDGETS } from './shapes/ShapeWidget'

// 코어(서비스 무지) 위젯 — 인프라/시스템 + 범용 shape 위젯(소스 선택형).
const CORE_WIDGETS: WidgetDef[] = [
  systemCardsWidget,
  systemTopologyWidget,
  ...SHAPE_WIDGETS,
]

// SERVICE_MANIFESTS 는 순환 import(registry↔manifest↔EditableLayout) 상 모듈 eval 시점엔
// 부분 초기화일 수 있어 lazy 집계 — 첫 호출(렌더 시점)엔 완전 초기화 보장.
let _all: WidgetDef[] | null = null
let _byId: Map<string, WidgetDef> | null = null
function ensure() {
  if (_all && _byId) return
  const service = SERVICE_MANIFESTS.flatMap(
    m => (m.widgets ?? []).map(w => ({ ...w, serviceId: w.serviceId ?? m.id }))
  )
  _all = [...CORE_WIDGETS, ...service]
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

// 위젯 카테고리 — 편집 UI 그룹핑용. 표시 순서 + 한글 라벨.
export type WidgetCategory = 'infra' | 'service' | 'stats' | 'view' | 'event' | 'etc'
export const WIDGET_CATEGORY_ORDER: WidgetCategory[] = ['infra', 'service', 'stats', 'view', 'event', 'etc']
export const WIDGET_CATEGORY_LABELS: Record<WidgetCategory, string> = {
  infra: '인프라/시스템', service: '서비스', stats: '통계',
  view: '데이터 뷰 (소스 선택)', event: '이벤트/알람', etc: '기타',
}

// 카테고리 → 위젯 목록 (위젯이 늘어날 때 편집 드롭다운을 성격별로 묶기 위함).
// 빈 카테고리는 생략, 표시 순서는 WIDGET_CATEGORY_ORDER 를 따른다.
export function widgetsByCategory(): { category: WidgetCategory; label: string; widgets: WidgetDef[] }[] {
  const all = allWidgets()
  return WIDGET_CATEGORY_ORDER
    .map(cat => ({
      category: cat,
      label: WIDGET_CATEGORY_LABELS[cat],
      widgets: all.filter(w => (w.category ?? 'etc') === cat),
    }))
    .filter(g => g.widgets.length > 0)
}
