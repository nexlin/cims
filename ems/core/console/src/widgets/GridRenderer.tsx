// 레이아웃 렌더러 — PageLayout 의 위젯 배치를 그린다. 두 배치 모드를 하위호환으로 지원:
//  · 2D grid (placement 에 x/y): 12칸×N행 셀 그리드에 grid-column/grid-row 로 절대 배치.
//  · legacy flow (x/y 없음): 12-col flow(합>12 wrap) — 기존 저장본/미편집 seed 가 그대로 렌더.
// 위젯은 자체 chrome(패널/헤더)을 렌더 — 렌더러는 배치(span·좌표)만 담당. 예외는 배치 단위 표시
// 이름(placement.title — 옛 저장본에만 남는 값)뿐. 위젯이 모르는 값이라 렌더러가 캡션으로 얹는다.
// registry 에 없는 위젯 id → fallback 카드(graceful, 레이아웃 안 깨짐).

import type { CSSProperties } from 'react'
import WidgetApiBadge from '../components/WidgetApiBadge'
import { getWidget } from './registry'
import type { PageLayout } from './types'
import { compact, isGridLayout, gridBox, GRID_COLS, GRID_ROWS, GRID_GAP } from './gridLayout'
import { isPlacementVisible, usePageParams } from './pageParams'
import type { WidgetPlacement } from './types'

// 조건부 표시(placement.visibleWhen) 평가 — 탭처럼 한 화면을 갈아끼우는 배치를 걸러낸다.
// 숨긴 뒤 남은 배치는 compact 로 위로 당겨 빈 자리를 없앤다(grid 배치일 때).
function useVisibleWidgets(widgets: WidgetPlacement[]): WidgetPlacement[] {
  const params = usePageParams()
  const shown = widgets.filter(p => isPlacementVisible(p, params))
  if (shown.length === widgets.length) return widgets
  return isGridLayout(shown) ? compact(shown) : shown
}

// legacy 높이 해석: 0/없음 = 자동, 1~100 = 화면 세로 비율(vh), >100 = 레거시 픽셀(하위호환).
export function widgetHeightCss(h?: number): string | undefined {
  if (!h) return undefined
  return h > 100 ? `${h}px` : `${h}vh`
}

function UnknownWidget({ id }: { id: string }) {
  return (
    <div className="panel" style={{ padding: 12, color: 'var(--destructive)', fontSize: 12 }}>
      알 수 없는 위젯: <code>{id}</code> (서비스 pack 미설치 또는 제거됨)
    </div>
  )
}

export function GridRenderer({ layout }: { layout: PageLayout }) {
  const widgets = useVisibleWidgets(layout.widgets)
  const shown = widgets === layout.widgets ? layout : { ...layout, widgets }
  return isGridLayout(shown.widgets)
    ? <GridCanvas layout={shown} />
    : <FlowGrid layout={shown} />
}

// ── 2D grid 렌더 ─────────────────────────────────────────────────────────
// 캔버스는 **고정 예산**(48열 × GRID_ROWS행)을 콘텐츠 영역에 꽉 채운다 — 행 높이가 vh 절대값이
// 아니라 `1fr` 이라 어떤 창 크기에서도 배치가 정확히 한 화면이고, 비율도 그대로다.
// 창이 설계 해상도(1920×1080)보다 작으면 캔버스 min-size(index.css) 때문에 스크롤이 생긴다 —
// 재배열하지 않고 그대로 보여주는 게 관제 화면의 규율이다(console_platform §3.0).
// DOM 은 (y,x) 정렬로 렌더 — 명시 배치라 순서 무관이지만 탭 이동 순서가 시각 순서와 맞도록.
function GridCanvas({ layout }: { layout: PageLayout }) {
  const gap = layout.gap ?? GRID_GAP
  const items = layout.widgets
    .map((p, i) => ({ p, i, b: gridBox(p) }))
    .sort((a, b) => (a.b.y - b.b.y) || (a.b.x - b.b.x))
  // 트랙 gap 은 0 — 칸 사이 간격은 카드 margin(--card-gap)으로 처리해 칸수와 간격을 분리(48칸에서도 안전).
  return (
    <div className="grid-canvas"
         style={{ display: 'grid', gridTemplateColumns: `repeat(${GRID_COLS}, 1fr)`,
                  gridTemplateRows: `repeat(${GRID_ROWS}, 1fr)`, gap: 0, alignItems: 'stretch',
                  ['--card-gap']: `${gap}px` } as CSSProperties}>
      {items.map(({ p, i, b }) => {
        const def = getWidget(p.widgetId)
        const Comp = def?.component
        const style: CSSProperties = {
          gridColumn: `${b.x + 1} / span ${b.w}`,
          gridRow: `${b.y + 1} / span ${b.h}`,
          minWidth: 0,
        }
        return (
          <div key={`${p.widgetId}-${i}`} className="widget-fixed widget-api-host" style={style}>
            <WidgetApiBadge ids={def?.apis} sourceIds={def?.apiSources?.(p.config)} title={def?.title} overlay />
            {p.title && <div className="widget-caption">{p.title}</div>}
            {Comp ? <Comp config={p.config} /> : <UnknownWidget id={p.widgetId} />}
          </div>
        )
      })}
    </div>
  )
}

// ── legacy flow 렌더 (현행 유지) ─────────────────────────────────────────
function FlowGrid({ layout }: { layout: PageLayout }) {
  return (
    <div style={{ display: 'grid', gridTemplateColumns: 'repeat(12, 1fr)', gap: 12, alignItems: 'start' }}>
      {layout.widgets.map((p, i) => {
        const def = getWidget(p.widgetId)
        const span = Math.min(Math.max(p.w ?? def?.defaultSize?.w ?? 12, 1), 12)
        const Comp = def?.component
        // h 지정 시 높이 '고정'(height) + 래퍼를 flex 로 만들어 내부 .panel(flex:1)이 채우게 함.
        // .widget-fixed > .panel 이 내부 스크롤 부여.
        const fixed = !!p.h
        const hStyle = fixed ? { height: widgetHeightCss(p.h), display: 'flex' as const } : {}
        return (
          <div key={`${p.widgetId}-${i}`}
               className={`${fixed ? 'widget-fixed ' : ''}widget-api-host`}
               style={{ gridColumn: `span ${span}`, minWidth: 0, ...hStyle }}>
            <WidgetApiBadge ids={def?.apis} sourceIds={def?.apiSources?.(p.config)} title={def?.title} overlay />
            {p.title && <div className="widget-caption">{p.title}</div>}
            {Comp ? <Comp config={p.config} /> : <UnknownWidget id={p.widgetId} />}
          </div>
        )
      })}
    </div>
  )
}
