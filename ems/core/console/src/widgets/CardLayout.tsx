// 카드 하나 안에 위젯 여러 개를 블록으로 배치한다.
//
// 화면 전체가 한 벌로만 의미를 갖는 경우(성능 통계, 서비스 정의) 위젯을 낱개로 흩어 놓으면
// 하나만 빠져도 화면이 성립하지 않는다. 그래서 **배치 단위는 카드 하나**로 두고, 카드 안의 구성은
// 별도 좌표계가 아니라 **바깥과 똑같은 48×48 셀 그리드**(gridLayout)로 서술한다.
//
// 좌표계를 통일한 덕에:
//   · 렌더는 바깥 캔버스와 같은 규칙(칸을 채우고 넘치면 블록 안에서 스크롤),
//   · 편집도 **같은 GridEditor** 를 카드에 겨누기만 하면 된다(이동/리사이즈/잠금/예산 전부 재사용),
//   · 블록은 레지스트리의 **같은 위젯**을 id 로 꺼내 쓰므로 낱개로 배치했을 때와 동작이 같다.
//
// 기본 배치는 `WidgetDef.cardLayout` 선언이고, 운영자가 카드 안을 편집하면
// `placement.config.layout` 에 저장돼 그것이 우선한다(seed ↔ 저장본 관계가 한 단계 아래에서 반복).

import { getWidget } from './registry'
import { GRID_COLS, GRID_ROWS, compact, gridBox, isGridLayout } from './gridLayout'
import { isPlacementVisible, usePageParams } from './pageParams'
import type { WidgetDef, WidgetPlacement, WidgetProps } from './types'

// 배치 1개 — 레지스트리에 없으면(서비스 pack 미설치) 자리만 남기고 알린다.
function Block({ widgetId, config }: WidgetPlacement) {
  const Comp = getWidget(widgetId)?.component
  if (!Comp) {
    return (
      <div className="panel" style={{ padding: 12, color: 'var(--danger)', fontSize: 12 }}>
        알 수 없는 블록: <code>{widgetId}</code>
      </div>
    )
  }
  return <Comp config={config} />
}

// 이 카드가 지금 쓸 배치 — 운영자 저장본(config.layout)이 있으면 그것, 없으면 선언된 기본.
export function resolveCardLayout(
  fallback: WidgetPlacement[], config?: Record<string, unknown>,
): WidgetPlacement[] {
  const saved = config?.layout
  return Array.isArray(saved) && saved.length > 0 ? saved as WidgetPlacement[] : fallback
}

export function CardLayout({ layout }: { layout: WidgetPlacement[] }) {
  // 탭 — 배치의 조건부 표시(§3.5)는 카드 **안**에서도 같은 규칙이다. 숨은 블록은 렌더에서 빠지고
  // 남은 것이 위로 당겨진다(compact). 편집 모드에서는 전부 보인다(GridEditor 가 따로 그린다).
  const params = usePageParams()
  const shown = layout.filter(p => isPlacementVisible(p, params))
  const view = shown.length === layout.length ? layout
    : isGridLayout(shown) ? compact(shown) : shown
  return (
    <div className="card-canvas"
         style={{ gridTemplateColumns: `repeat(${GRID_COLS}, 1fr)`,
                  gridTemplateRows: `repeat(${GRID_ROWS}, 1fr)` }}>
      {view.map((p, i) => {
        const b = gridBox(p)
        return (
          <div key={`${p.widgetId}-${i}`} className="card-block"
               style={{ gridColumn: `${b.x + 1} / span ${b.w}`, gridRow: `${b.y + 1} / span ${b.h}` }}>
            <Block {...p} />
          </div>
        )
      })}
    </div>
  )
}

// 카드가 쓰는 데이터 소스 id 목록 — WidgetDef.apiSources 로 넘겨 [API] 배지가 살아 있게 한다.
// (블록을 낱개로 배치했을 때 각 위젯이 내던 선언을 카드가 대신 낸다.)
export function cardSources(layout: WidgetPlacement[]): string[] {
  const out = new Set<string>()
  for (const p of layout) {
    const s = p.config?.source
    if (typeof s === 'string' && s) out.add(s)
  }
  return [...out]
}

// 카드 위젯 정의 팩토리 — 기본 배치를 `cardLayout` 으로 선언하고, 렌더는 저장본 우선으로 고른다.
export function makeCardWidget(
  base: Omit<WidgetDef, 'component' | 'cardLayout' | 'apiSources'> & { layout: WidgetPlacement[] },
): WidgetDef {
  const { layout, ...def } = base
  return {
    ...def,
    cardLayout: layout,
    apiSources: config => cardSources(resolveCardLayout(layout, config)),
    component: (p: WidgetProps) => <CardLayout layout={resolveCardLayout(layout, p.config)} />,
  }
}
