// 위젯 레지스트리 — 코어 위젯 + 서비스 pack 이 기여한 위젯(ServiceManifest.widgets) 병합.
// 새 위젯: 코어면 CORE_WIDGETS 에, 서비스면 해당 manifest.widgets 에 추가.

import type { WidgetDef } from './types'
import { SERVICE_MANIFESTS } from '../services/registry'
import { systemCardsWidget } from './core/SystemCardsWidget'

// 코어(서비스 무지) 위젯 — 인프라/시스템.
const CORE_WIDGETS: WidgetDef[] = [
  systemCardsWidget,
]

// 서비스 pack 기여 위젯 — serviceId 태깅.
const SERVICE_WIDGETS: WidgetDef[] = SERVICE_MANIFESTS.flatMap(
  m => (m.widgets ?? []).map(w => ({ ...w, serviceId: w.serviceId ?? m.id }))
)

const ALL_WIDGETS: WidgetDef[] = [...CORE_WIDGETS, ...SERVICE_WIDGETS]
const BY_ID = new Map(ALL_WIDGETS.map(w => [w.id, w]))

export function getWidget(id: string): WidgetDef | undefined {
  return BY_ID.get(id)
}

export function allWidgets(): WidgetDef[] {
  return ALL_WIDGETS
}

// 위젯 카테고리 — 편집 UI 그룹핑용. 표시 순서 + 한글 라벨.
export type WidgetCategory = 'infra' | 'service' | 'stats' | 'event' | 'etc'
export const WIDGET_CATEGORY_ORDER: WidgetCategory[] = ['infra', 'service', 'stats', 'event', 'etc']
export const WIDGET_CATEGORY_LABELS: Record<WidgetCategory, string> = {
  infra: '인프라/시스템', service: '서비스', stats: '통계', event: '이벤트/알람', etc: '기타',
}

// 카테고리 → 위젯 목록 (위젯이 늘어날 때 편집 드롭다운을 성격별로 묶기 위함).
// 빈 카테고리는 생략, 표시 순서는 WIDGET_CATEGORY_ORDER 를 따른다.
export function widgetsByCategory(): { category: WidgetCategory; label: string; widgets: WidgetDef[] }[] {
  return WIDGET_CATEGORY_ORDER
    .map(cat => ({
      category: cat,
      label: WIDGET_CATEGORY_LABELS[cat],
      widgets: ALL_WIDGETS.filter(w => (w.category ?? 'etc') === cat),
    }))
    .filter(g => g.widgets.length > 0)
}
