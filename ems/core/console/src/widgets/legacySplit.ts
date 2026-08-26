// 폐지된 묶음 위젯 → 분해된 개별 위젯으로 펼치기.
//
// 서로 다른 축의 독립 지표를 한 카드에 담았던 위젯(구 `cims.kpi` = 가입자·번호·활성 호·그룹·RTP)은
// 지표별 위젯으로 분해했다. 이미 저장된 레이아웃이 그 옛 id 를 참조하면 "알 수 없는 위젯" 이 되므로,
// 레이아웃을 읽는 시점에 부품들로 펼쳐 준다(저장본은 건드리지 않고 표시만 — 다음 저장 때 굳는다).
//
// **분해 대상이 아닌 묶음도 있다** — 같은 축의 분포(심각도 CRIT/MAJOR/…)나 같은 대상의 지표 묶음
// (VoLTE 요약, VoLTE 통계 지표)은 하나만 떼면 의미를 잃어 그대로 둔다(console_platform.md §3.1).
// 새 폐지 건은 CORE_SPLITS(코어 위젯) 또는 ServiceManifest.splits(서비스 pack 위젯)에 한 줄 추가한다.

import type { WidgetPlacement } from './types'
import { compact, gridBox, isGridLayout, isGridPlacement } from './gridLayout'
import { catalogSources } from './shapes/sourceRegistry'

export interface SplitPart { widgetId: string; config?: Record<string, unknown> }
// 부모 배치의 config 를 받아 부품 목록을 만든다 (소스 같은 인스턴스 설정을 부품에 물려주기 위함).
export type SplitFn = (config?: Record<string, unknown>) => SplitPart[]

const PART_MIN_COLS = 6      // 부품 1개의 최소 폭(48칸 기준) — 이보다 좁아지면 줄을 나눈다
const PART_MIN_ROWS = 3

// 구 `core.analysis-filter`(탭 + 기간을 한 카드에) → 이력 화면과 같은 컨트롤 2개로.
export const CORE_SPLITS: Record<string, SplitFn> = {
  // 구 `shape.kpi`(지표 여러 개가 한 카드) → 지표 1개짜리 `shape.stat` 여러 개.
  // 항목 수는 소스 선언(kpiItems)에서 얻고, 카탈로그가 아직이면 4개로 펼친다(남는 카드는 "지표 없음").
  'shape.kpi': (config) => {
    const source = typeof config?.source === 'string' ? config.source : undefined
    const n = catalogSources().find(s => s.id === source)?.kpiItems?.length ?? 4
    return Array.from({ length: n }, (_, i) => ({
      widgetId: 'shape.stat',
      config: { ...(source ? { source } : {}), item: i },
    }))
  },
  'core.analysis-filter': () => [
    { widgetId: 'core.alarm-event-tabs' },
    { widgetId: 'core.days-filter' },
  ],
}

// 배치 1개를 부품 N개로 — 원래 박스 안에서 나눠 담는다(칸이 좁으면 여러 줄).
function expandOne(p: WidgetPlacement, parts: SplitPart[]): WidgetPlacement[] {
  const n = parts.length
  if (n === 0) return [p]
  if (!isGridPlacement(p)) {
    // legacy flow — x/y 가 없으므로 순서+폭만 준다(12칸 기준).
    const w = Math.max(2, Math.floor(12 / Math.min(n, 6)))
    return parts.map(part => ({ ...p, widgetId: part.widgetId, config: part.config, w }))
  }
  const box = gridBox(p)
  const cols = Math.max(1, Math.min(n, Math.floor(box.w / PART_MIN_COLS)))
  const rows = Math.ceil(n / cols)
  const cw = Math.floor(box.w / cols)
  const rh = Math.max(PART_MIN_ROWS, Math.floor(box.h / rows))
  return parts.map((part, i) => ({
    widgetId: part.widgetId,
    config: part.config,
    x: box.x + (i % cols) * cw,
    y: box.y + Math.floor(i / cols) * rh,
    w: cw,
    h: rh,
  }))
}

// 레이아웃 전체 전개. 펼친 게 있으면 grid 레이아웃은 compact 로 겹침을 정리한다
// (부품이 여러 줄이 되면 아래 위젯과 겹칠 수 있으므로).
export function expandSplits(
  widgets: WidgetPlacement[], lookup: (widgetId: string) => SplitFn | undefined,
): WidgetPlacement[] {
  let changed = false
  const out: WidgetPlacement[] = []
  for (const p of widgets) {
    const fn = lookup(p.widgetId)
    if (!fn) { out.push(p); continue }
    const parts = fn(p.config)
    if (parts.length === 0) { out.push(p); continue }
    changed = true
    out.push(...expandOne(p, parts))
  }
  if (!changed) return widgets
  return isGridLayout(out) ? compact(out) : out
}
