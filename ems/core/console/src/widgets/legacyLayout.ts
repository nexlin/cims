// 폐지된 위젯 id → 지금 쓰는 위젯으로 갈아끼우기 (양방향).
//
// 분해 단위는 화면을 쓰면서 바뀐다(console_platform.md §3.1). 이미 저장된 레이아웃이 옛 id 를
// 참조하면 "알 수 없는 위젯" 이 되므로, 레이아웃을 읽는 시점에 갈아끼워 보여준다 — 저장본은
// 건드리지 않고 표시만 하고(다음 저장 때 굳는다), 두 방향이 있다:
//   · **분해**(splits): 묶음 1개 → 부품 N개. 부품들은 원래 상자 안에서 나눠 담는다.
//   · **합치기**(merges): 부품 N개 → 대체 위젯 1개. 부품들이 차지하던 영역(합집합 상자)에 들어앉는다.
//
// 새 폐지 건은 CORE_SPLITS / CORE_MERGES(코어 위젯) 또는 ServiceManifest.splits / .merges
// (서비스 pack 위젯)에 한 줄 추가한다.

import type { WidgetPlacement } from './types'
import { compact, gridBox, isGridLayout, isGridPlacement } from './gridLayout'
import { catalogSources } from './shapes/sourceRegistry'
import { SEVERITY_ORDER } from '../utils/alarmLabels'

export interface SplitPart { widgetId: string; config?: Record<string, unknown> }
// 부모 배치의 config 를 받아 부품 목록을 만든다 (소스 같은 인스턴스 설정을 부품에 물려주기 위함).
export type SplitFn = (config?: Record<string, unknown>) => SplitPart[]

const PART_MIN_COLS = 6      // 부품 1개의 최소 폭(48칸 기준) — 이보다 좁아지면 줄을 나눈다
const PART_MIN_ROWS = 3

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
  // 구 `core.analysis-filter`(탭 + 기간을 한 카드에) → 이력 화면과 같은 컨트롤 2개로.
  'core.analysis-filter': () => [
    { widgetId: 'core.alarm-event-tabs' },
    { widgetId: 'core.days-filter' },
  ],
  // 구 `core.alarm-severity`(심각도 타일 5장이 한 카드) → 타일 1장짜리 위젯 5개.
  'core.alarm-severity': () => SEVERITY_ORDER.map(sev => ({ widgetId: `core.alarm-severity.${sev}` })),
}

// 폐지 위젯 id → 대체 위젯 id (부품 N개 → 위젯 1개).
//
// **카드로 올린 합치기에는 쓰지 않는다.** 카드(CardLayout)는 부품을 id 로 찾아 그리므로 그 id 가
// 살아 있어야 하고, 규칙을 걸면 운영자가 일부러 낱개로 놓은 배치까지 카드로 바뀐다. 장애 이력·
// 유형별 분석·성능 통계·서비스 정의가 그 경우이며, 전이 경로는 `seedVersion` 상승(§3.4)이다.
// 여기 남길 것은 부품이 **아예 사라진** 합치기뿐이다(현재 없음).
export const CORE_MERGES: Record<string, string> = {}

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

// 두 배치를 감싸는 최소 상자 — 흩어져 있던 부품 자리에 대체 위젯 하나가 그대로 들어앉게 한다.
function unionBox(a: WidgetPlacement, b: WidgetPlacement): WidgetPlacement {
  if (!isGridPlacement(a) || !isGridPlacement(b)) return a
  const ab = gridBox(a), bb = gridBox(b)
  const x = Math.min(ab.x, bb.x)
  const y = Math.min(ab.y, bb.y)
  return { ...a, x, y,
           w: Math.max(ab.x + ab.w, bb.x + bb.w) - x,
           h: Math.max(ab.y + ab.h, bb.y + bb.h) - y }
}

// 레이아웃 전체 접기. 같은 대체 위젯으로 가는 배치는 **첫 자리에 하나만** 남기고 상자만 넓힌다.
// 탭 조건(visibleWhen)은 떨어뜨린다 — 어느 탭을 보여줄지는 합친 위젯이 안에서 판단한다.
export function collapseMerges(
  widgets: WidgetPlacement[], lookup: (widgetId: string) => string | undefined,
): WidgetPlacement[] {
  if (!widgets.some(p => lookup(p.widgetId))) return widgets
  const out: WidgetPlacement[] = []
  const at = new Map<string, number>()          // 대체 위젯 id → out 인덱스
  for (const p of widgets) {
    const target = lookup(p.widgetId)
    if (!target) { out.push(p); continue }
    const i = at.get(target)
    if (i === undefined) {
      at.set(target, out.length)
      out.push({ ...p, widgetId: target, visibleWhen: undefined })
    } else {
      out[i] = unionBox(out[i], p)
    }
  }
  return isGridLayout(out) ? compact(out) : out
}
