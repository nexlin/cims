// 레이아웃 렌더러 — PageLayout 의 위젯 배치를 12-col grid 로 그린다.
// 위젯은 자체 chrome(패널/헤더)을 렌더 — 렌더러는 배치(span)만 담당.
// registry 에 없는 위젯 id → fallback 카드(graceful, 레이아웃 안 깨짐).

import { getWidget } from './registry'
import type { PageLayout } from './types'

function UnknownWidget({ id }: { id: string }) {
  return (
    <div className="panel" style={{ padding: 12, color: 'var(--danger)', fontSize: 12 }}>
      알 수 없는 위젯: <code>{id}</code> (서비스 pack 미설치 또는 제거됨)
    </div>
  )
}

export function GridRenderer({ layout }: { layout: PageLayout }) {
  return (
    <div style={{ display: 'grid', gridTemplateColumns: 'repeat(12, 1fr)', gap: 12, alignItems: 'start' }}>
      {layout.widgets.map((p, i) => {
        const def = getWidget(p.widgetId)
        const span = Math.min(Math.max(p.w ?? def?.defaultSize?.w ?? 12, 1), 12)
        const Comp = def?.component
        return (
          <div key={`${p.widgetId}-${i}`} style={{ gridColumn: `span ${span}`, minWidth: 0 }}>
            {Comp ? <Comp config={p.config} /> : <UnknownWidget id={p.widgetId} />}
          </div>
        )
      })}
    </div>
  )
}
