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
