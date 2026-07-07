// 레이아웃 렌더러 — PageLayout 의 위젯 배치를 12-col grid 로 그린다.
// 위젯은 자체 chrome(패널/헤더)을 렌더 — 렌더러는 배치(span)만 담당.
// registry 에 없는 위젯 id → fallback 카드(graceful, 레이아웃 안 깨짐).

import { getWidget } from './registry'
import type { PageLayout } from './types'

// 위젯 높이 해석: 0/없음 = 자동, 1~100 = 화면 세로 비율(vh), >100 = 레거시 픽셀(하위호환).
// 픽셀 절대값 대신 화면 세로 기준 비율로 지정 — 해상도에 따라 비례.
export function widgetHeightCss(h?: number): string | undefined {
  if (!h) return undefined
  return h > 100 ? `${h}px` : `${h}vh`
}

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
        // h(px) 지정 시 높이 '고정'(maxHeight 가 아니라 height) + 래퍼를 flex 로 만들어
        // 내부 .panel(flex:1)이 그 높이를 채우게 함. .widget-fixed > .panel 이 내부 스크롤 부여.
        const fixed = !!p.h
        const hStyle = fixed ? { height: widgetHeightCss(p.h), display: 'flex' as const } : {}
        return (
          <div key={`${p.widgetId}-${i}`} className={fixed ? 'widget-fixed' : undefined}
               style={{ gridColumn: `span ${span}`, minWidth: 0, ...hStyle }}>
            {Comp ? <Comp config={p.config} /> : <UnknownWidget id={p.widgetId} />}
          </div>
        )
      })}
    </div>
  )
}
