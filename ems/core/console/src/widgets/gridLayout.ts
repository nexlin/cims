// 자유 2D 그리드 레이아웃 엔진 — 순수 함수(React/DOM 비의존, 런타임 측정치는 인자 주입)라
// 결정적이고 단독 검증 가능하다. 이동/리사이즈 후 겹침을 아래로 밀고(collision) 빈 행을 위로
// 당긴다(compaction/중력).
//
// **캔버스는 고정 예산이다** — 관제 화면이라 1920×1080 에서 스크롤 없이 한눈에 들어와야 하므로
// 세로도 무한이 아니라 GRID_ROWS 행이 전부다(console_platform.md §3.0). 그래서 위젯을 키우면
// 캔버스가 자라는 게 아니라 **다른 위젯 자리를 뺏는다**:
//   · `locked` 배치는 위치·크기가 고정 — 밀리지도, 줄어들지도 않는다(운영자가 지킬 영역을 지정).
//   · 예산이 모자라면 잠기지 않은 위젯을 **아래에서부터** 최소 높이까지 줄여 자리를 만든다.
//   · 그래도 모자라면 조작을 **거절**한다(원래 배치를 그대로 돌려준다) — 조용히 넘치지 않는다.
//
// 좌표계: WidgetPlacement.{x,y,w,h} 는 모두 그리드 셀 단위(가로/세로 통일).
//  · x: 0-based 열(0..GRID_COLS-1), y: 0-based 행(0..GRID_ROWS-1), w/h: span.
// x/y 없는 placement 는 legacy flow(GridRenderer 가 순서+w 로 렌더) — flowToGrid 로 grid 로 migrate.

import type { WidgetPlacement } from './types'

export const GRID_COLS = 48       // 가로 48칸. 12 의 배수라 legacy 12-칸 환산이 깔끔.
export const GRID_ROWS = 48       // 세로 48행 = **화면 하나**. 1920×1080 기준 셀 ≈ 34.3 × 20.2px.
                                  // 이 값이 곧 세로 예산 — 어떤 배치도 이 행을 넘을 수 없다.
export const COL_SCALE = GRID_COLS / 12   // legacy 12-칸 기준 폭(seed w·defaultSize.w) → 현재 칸수로 환산(×4)
export const GRID_GAP = 12        // 카드 간 기본 간격(px) — layout.gap 미지정 시 사용
export const MIN_ROWS = 4         // 위젯 최소 높이(행) — 자리를 뺏을 때 여기까지만 줄인다.
                                  // 더 큰 하한이 필요한 위젯은 WidgetDef.minSize.h 로 선언한다.
export const DEFAULT_ROWS = Math.round(GRID_ROWS * 0.3)   // 자동/미지정 높이 위젯 기본 ≈ 화면 세로 30%
export const NOMINAL_ROW_VH = 100 / GRID_ROWS   // legacy vh 높이 → 행 환산용 명목값(캔버스 ≈ 화면 90%)
const LEGACY_PX_PER_ROW = 40      // legacy 픽셀 높이(h>100) → 행 변환용 명목 px (아주 드묾)

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, v))

// 위젯별 최소 행 수 해석자 — registry 를 모르는 순수 모듈이라 주입받는다(미지정 = MIN_ROWS).
export type MinRowsFn = (p: WidgetPlacement) => number
const defaultMinRows: MinRowsFn = () => MIN_ROWS
const isLocked = (p: WidgetPlacement) => p.locked === true

// 정규화된 그리드 박스 — 옵셔널 필드에 안전한 기본값 부여.
export interface GridBox { x: number; y: number; w: number; h: number }

// y 는 **위쪽만** 잠근다(>=0). `y+h <= GRID_ROWS` 로 되감으면 예산 초과가 조용히 감춰지고
// (위로 끌어올려져 다른 위젯과 겹친다) 초과 판정 자체가 불가능해진다 — 예산은 commit/fitToBudget
// 이 줄여서 맞추고, gridBox 는 있는 그대로를 잰다.
export function gridBox(p: WidgetPlacement): GridBox {
  const w = clamp(Math.round(p.w ?? GRID_COLS), 1, GRID_COLS)
  const h = clamp(Math.round(p.h ?? DEFAULT_ROWS), 1, GRID_ROWS)
  return {
    w, h,
    x: clamp(Math.round(Number.isFinite(p.x) ? (p.x as number) : 0), 0, GRID_COLS - w),
    y: Math.max(0, Math.round(Number.isFinite(p.y) ? (p.y as number) : 0)),
  }
}

// 배치 전체가 차지하는 마지막 행 — 예산(GRID_ROWS) 초과 판정용.
export function usedRows(items: WidgetPlacement[]): number {
  return items.reduce((m, p) => { const b = gridBox(p); return Math.max(m, b.y + b.h) }, 0)
}

// ── 배치 모드 판별 (x·y 존재 = grid, 없음 = legacy flow) ──────────────────
export function isGridPlacement(p: WidgetPlacement): boolean {
  return Number.isFinite(p.x) && Number.isFinite(p.y)
}
export function isGridLayout(widgets: WidgetPlacement[]): boolean {
  return widgets.length > 0 && widgets.every(isGridPlacement)
}

// ── clamp 헬퍼 (편집기 포인터 델타 커밋용) ────────────────────────────────
export const clampX = (x: number, w: number) => clamp(Math.round(x), 0, GRID_COLS - clamp(Math.round(w), 1, GRID_COLS))
export const clampW = (w: number, x: number) => clamp(Math.round(w), 1, GRID_COLS - Math.max(0, Math.round(x)))
export const clampH = (h: number, y = 0) => clamp(Math.round(h), 1, GRID_ROWS - clamp(Math.round(y), 0, GRID_ROWS - 1))
export const clampY = (y: number, h = 1) => clamp(Math.round(y), 0, GRID_ROWS - clamp(Math.round(h), 1, GRID_ROWS))

// 두 박스가 겹치는가 (경계 접촉은 겹침 아님).
export function overlap(a: GridBox, b: GridBox): boolean {
  return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y
}

// 중력: it(x·w·h 고정)가 placed 와 안 겹치는 최소 y. 겹치면 최하단 장애물 바닥으로 점프 후 재검사.
// y 는 매 반복 엄격 증가(장애물 바닥 > 현재 y) → 최대 placed.length 회 내 종료. 방어 카운터 포함.
function firstFreeY(placed: GridBox[], it: GridBox): number {
  let y = 0
  for (let guard = 0; guard <= placed.length + 1; guard++) {
    const cand: GridBox = { x: it.x, y, w: it.w, h: it.h }
    const hit = placed.filter(p => overlap(cand, p))
    if (hit.length === 0) return y
    y = Math.max(...hit.map(p => p.y + p.h))
  }
  return y
}

// 핵심 primitive — 위로 당기고(빈 행 제거) 겹침을 아래로 민다. 이동/리사이즈/추가/제거가 모두 통과.
// 정렬 순서 (y↑, movedKey 우선, x↑, 원본 index↑) = 전순서라 결정적. 1-pass 종료. idempotent.
// movedKey: 이번에 사용자가 옮긴/키운 항목 — 동일 y 에서 우선 배치돼 자기 slot 을 차지(나머지는 밀림).
// **잠긴 배치는 먼저 자기 자리에 못박는다** — 나머지가 그 주위로 채워진다(밀 수 없는 장애물).
export function compact(items: WidgetPlacement[], movedKey?: number): WidgetPlacement[] {
  const boxes = items.map(gridBox)
  const free: number[] = []
  const placed: GridBox[] = []
  const result = items.slice()
  items.forEach((p, i) => {
    if (isLocked(p) && i !== movedKey) {
      placed.push(boxes[i])
      result[i] = { ...p, x: boxes[i].x, y: boxes[i].y, w: boxes[i].w, h: boxes[i].h }
    } else {
      free.push(i)
    }
  })
  const order = free.sort((a, b) => {
    if (boxes[a].y !== boxes[b].y) return boxes[a].y - boxes[b].y
    const pa = a === movedKey ? 0 : 1
    const pb = b === movedKey ? 0 : 1
    if (pa !== pb) return pa - pb
    if (boxes[a].x !== boxes[b].x) return boxes[a].x - boxes[b].x
    return a - b
  })
  for (const i of order) {
    const b = boxes[i]
    b.y = firstFreeY(placed, b)
    placed.push(b)
    result[i] = { ...items[i], x: b.x, y: b.y, w: b.w, h: b.h }
  }
  return result
}

// ── 세로 예산(GRID_ROWS) 맞추기 ─────────────────────────────────────────────
// compact 결과가 예산을 넘으면, 넘친 만큼 **잠기지 않은** 위젯을 줄여 자리를 만든다.
// 줄일 대상은 "가장 아래까지 내려간 위젯과 같은 열 띠(column strip)" 안에서 고른다 — 엉뚱한 곳의
// 위젯이 갑자기 작아지지 않게. 같은 띠 안에서는 **아래쪽부터**(방금 조작한 위젯은 제외).
function deepestStrip(items: WidgetPlacement[]): { lo: number; hi: number } | null {
  let best: GridBox | null = null
  for (const p of items) {
    const b = gridBox(p)
    if (!best || b.y + b.h > best.y + best.h) best = b
  }
  return best ? { lo: best.x, hi: best.x + best.w } : null
}

function fitBudget(
  items: WidgetPlacement[], movedKey: number | undefined, minRows: MinRowsFn,
  allowShrink = true,
): { items: WidgetPlacement[]; ok: boolean } {
  let cur = compact(items, movedKey)
  // **이동은 남의 크기를 바꾸지 않는다** — 자리를 옮겼을 뿐인데 옆 카드가 작아지면 조작과 결과가
  // 어긋난다. 예산을 넘으면 줄이지 말고 거절한다(호출부가 원래 배치를 돌려준다).
  if (!allowShrink) return { items: cur, ok: usedRows(cur) <= GRID_ROWS }
  // 최대 반복 = 줄일 수 있는 총 행 수 상한. 매 반복 최소 1행이 줄어드므로 반드시 종료한다.
  for (let guard = 0; guard <= GRID_ROWS * items.length + 1; guard++) {
    if (usedRows(cur) <= GRID_ROWS) return { items: cur, ok: true }
    const strip = deepestStrip(cur)
    if (!strip) return { items: cur, ok: false }
    const cand = cur
      .map((p, i) => ({ p, i, b: gridBox(p) }))
      .filter(({ p, i, b }) =>
        !isLocked(p) && i !== movedKey && b.h > minRows(p) && b.x < strip.hi && b.x + b.w > strip.lo)
      .sort((a, b) => (b.b.y + b.b.h) - (a.b.y + a.b.h))[0]
    if (!cand) return { items: cur, ok: false }
    cur = compact(cur.map((p, i) => i === cand.i ? { ...p, h: cand.b.h - 1 } : p), movedKey)
  }
  return { items: cur, ok: usedRows(cur) <= GRID_ROWS }
}

// 저장본 정규화 — 예산(GRID_ROWS)을 넘는 옛 배치를 로드 시점에 줄여 맞춘다.
// 세로 무한이던 시절의 저장본(대시보드 86행 등)이 그대로 렌더되면 캔버스 밖으로 흘러나가므로,
// 잠기지 않은 위젯을 줄여 한 화면에 담는다. 줄일 여지가 없으면 그대로 둔다(렌더가 잘릴 뿐).
export function fitToBudget(
  items: WidgetPlacement[], minRows: MinRowsFn = defaultMinRows,
): WidgetPlacement[] {
  if (items.length === 0 || usedRows(items) <= GRID_ROWS) return items
  return fitBudget(items, undefined, minRows).items
}

// 예산 안에 들어가면 새 배치를, 못 들어가면 **원래 배치를 그대로** 돌려준다(조작 거절).
// allowShrink=false 면 남을 줄여서까지 우겨넣지 않는다(이동·교환).
function commit(
  prev: WidgetPlacement[], next: WidgetPlacement[], movedKey: number | undefined, minRows: MinRowsFn,
  allowShrink = true,
): WidgetPlacement[] {
  const r = fitBudget(next, movedKey, minRows, allowShrink)
  return r.ok ? r.items : prev
}

// 두 박스가 겹치는 넓이(셀 수) — 드롭 지점이 어느 카드 위인지 고르는 데 쓴다.
function overlapArea(a: GridBox, b: GridBox): number {
  const w = Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x)
  const h = Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y)
  return w > 0 && h > 0 ? w * h : 0
}

// **자리 교환** — 다른 카드 위에 떨어뜨리면 둘이 자리를 맞바꾼다(크기는 서로 그대로).
// 캔버스가 꽉 찬 관제 화면에서는 "빈 자리로 옮기기"가 대부분 불가능하다. 그렇다고 남을 줄여
// 우겨넣으면 옮겼을 뿐인데 옆 카드가 작아진다(운영자가 지적한 그 현상). 그래서 교환을 먼저 본다.
// 폭이 서로 달라 캔버스를 넘칠 땐 안쪽으로 밀어 넣되 **크기는 건드리지 않는다**.
function trySwap(
  items: WidgetPlacement[], key: number, target: GridBox, minRows: MinRowsFn,
): WidgetPlacement[] | null {
  const hit = items
    .map((p, i) => ({ p, i, b: gridBox(p), area: 0 }))
    .filter(o => o.i !== key && !isLocked(o.p))
    .map(o => ({ ...o, area: overlapArea(target, o.b) }))
    .filter(o => o.area > 0)
    .sort((a, b) => b.area - a.area)[0]
  if (!hit) return null
  const cur = gridBox(items[key])
  const next = items.map((p, i) => {
    if (i === key) return { ...p, x: clampX(hit.b.x, cur.w), y: clampY(hit.b.y, cur.h) }
    if (i === hit.i) return { ...p, x: clampX(cur.x, hit.b.w), y: clampY(cur.y, hit.b.h) }
    return { ...p }
  })
  const r = fitBudget(next, key, minRows, false)
  return r.ok ? r.items : null
}

// 이동 커밋 — **크기는 아무도 바뀌지 않는다**. 다른 카드 위에 놓으면 자리 교환, 빈 자리면 그냥 이동,
// 둘 다 안 되면 이동 자체를 거절한다(옆 카드를 줄여서 우겨넣지 않는다).
export function moveItem(
  items: WidgetPlacement[], key: number, x: number, y: number, minRows: MinRowsFn = defaultMinRows,
): WidgetPlacement[] {
  const b = gridBox(items[key])
  const target: GridBox = { x: clampX(x, b.w), y: clampY(y, b.h), w: b.w, h: b.h }
  const next = items.map((p, i) => i === key ? { ...p, x: target.x, y: target.y } : { ...p })
  // 빈 자리로의 이동이 성립하면 그것이 우선(교환은 "남의 자리에 놓았을 때"의 규칙).
  const moved = commit(items, next, key, minRows, false)
  if (moved !== items) return moved
  return trySwap(items, key, target, minRows) ?? items
}

// 리사이즈 커밋 — x 고정한 채 w·h(clamp) 세팅 후 예산 맞춤.
export function resizeItem(
  items: WidgetPlacement[], key: number, w: number, h: number, minRows: MinRowsFn = defaultMinRows,
): WidgetPlacement[] {
  const next = items.map((p, i) => {
    if (i !== key) return { ...p }
    const b = gridBox(p)
    return { ...p, w: clampW(w, b.x), h: clampH(h, b.y) }
  })
  return commit(items, next, key, minRows)
}

// 임의 방향 리사이즈/이동 커밋 — 목표 박스(x·y·w·h)를 통째로 clamp 후 세팅하고 예산 맞춤.
// 위·왼쪽 모서리 리사이즈처럼 x/y 와 w/h 가 함께 바뀌는 경우에 사용(8방향 핸들).
export function applyBox(
  items: WidgetPlacement[], key: number, box: GridBox, minRows: MinRowsFn = defaultMinRows,
): WidgetPlacement[] {
  const w = clamp(Math.round(box.w), 1, GRID_COLS)
  const x = clamp(Math.round(box.x), 0, GRID_COLS - w)
  const h = clamp(Math.round(box.h), 1, GRID_ROWS)
  const y = clamp(Math.round(box.y), 0, GRID_ROWS - h)
  const cur = gridBox(items[key])
  // 크기가 그대로면 사실상 이동이다 — 이동은 남을 줄이지 않는다.
  const sizeChanged = w !== cur.w || h !== cur.h
  const next = items.map((p, i) => i === key ? { ...p, x, y, w, h } : { ...p })
  return commit(items, next, key, minRows, sizeChanged)
}

// 새 위젯을 첫 빈 슬롯(x=0, 최상단 빈 행)에 배치. 예산이 없으면 추가하지 않는다(원본 반환).
export function addToFirstFree(
  items: WidgetPlacement[], placement: WidgetPlacement, defW?: number, defH?: number,
  minRows: MinRowsFn = defaultMinRows,
): WidgetPlacement[] {
  const w = clamp(Math.round((placement.w ?? defW ?? 12) * COL_SCALE), 1, GRID_COLS)  // 12-칸 기준 → 현재 칸수
  const h = clamp(Math.round(placement.h ?? defH ?? DEFAULT_ROWS), 1, GRID_ROWS)
  const next = [...items.map(p => ({ ...p })), { ...placement, x: 0, y: Number.MAX_SAFE_INTEGER, w, h }]
  // 새로 넣는 카드가 movedKey — 자리를 못 만들면 추가 자체가 거절된다.
  return commit(items, next, next.length - 1, minRows)
}

// 제거 후 compaction 으로 빈칸 닫힘.
export function removeAt(items: WidgetPlacement[], index: number): WidgetPlacement[] {
  return compact(items.filter((_, i) => i !== index))
}

// 잠금 토글 — 기하는 그대로, `locked` 플래그만 뒤집는다(false 는 저장하지 않는다).
export function setLockedAt(items: WidgetPlacement[], index: number, locked: boolean): WidgetPlacement[] {
  return items.map((p, i) => {
    if (i !== index) return { ...p }
    const next: WidgetPlacement = { ...p, locked }
    if (!locked) delete next.locked
    return next
  })
}

// 인스턴스 설정 patch(얕은 merge) — undefined 값은 키 제거. 배치 무변경이라 compaction 없음.
export function setConfigAt(
  items: WidgetPlacement[], index: number, patch: Record<string, unknown>,
): WidgetPlacement[] {
  return items.map((p, i) => {
    if (i !== index) return { ...p }
    const config: Record<string, unknown> = { ...(p.config ?? {}) }
    for (const [k, v] of Object.entries(patch)) {
      if (v === undefined) delete config[k]
      else config[k] = v
    }
    const next: WidgetPlacement = { ...p, config }
    if (Object.keys(config).length === 0) delete next.config
    return next
  })
}

// legacy 높이(vh|px|auto) → 행 span 변환.
export function heightToRows(h: number | undefined, defaultRows: number, vhToRows: (vh: number) => number): number {
  if (!h) return defaultRows                          // auto → 기본 span
  if (h > 100) return Math.max(1, Math.round(h / LEGACY_PX_PER_ROW))  // legacy px → 행
  return Math.max(1, vhToRows(h))                     // vh → 행
}

// legacy flow → grid migrate. 현재 flow 순서+폭으로 x/y 배정(합>12 wrap = 현행 렌더 규칙 재현),
// vh 높이는 vhToRows 로 행 변환. 끝에 compact 안 함 — shelf-packing 으로 현재 외형 그대로 재현
// (첫 편집 조작이 compact 를 돌려 자연 정착). getDefaultW/vhToRows 는 주입(순수성 유지).
//
// **높이 미지정(자동) 배치**는 grid 에 대응 개념이 없다. 상수를 박으면 편집 카드가 실제 화면과
// 전혀 다른 크기로 잡히므로(통짜 페이지가 전부 30% 로 통일되던 원인), 지금 화면에 그려진 높이를
// measureRows 로 받아 그 값으로 시작한다. 측정을 못 하면 그때만 기본값.
export function flowToGrid(
  widgets: WidgetPlacement[],
  getDefaultW: (widgetId: string) => number | undefined,
  vhToRows: (vh: number) => number,
  defaultRows = DEFAULT_ROWS,
  measureRows?: (index: number) => number | undefined,
): WidgetPlacement[] {
  let col = 0
  let rowTop = 0
  let rowMaxH = 0
  const out: WidgetPlacement[] = []
  widgets.forEach((p, i) => {
    const w = clamp(Math.round((p.w ?? getDefaultW(p.widgetId) ?? 12) * COL_SCALE), 1, GRID_COLS)  // 12-칸 기준 → 현재 칸수
    // h 가 있으면 그 값이 정본(기존 동작 그대로). 없을 때만 실측 → 기본값 순으로 채운다.
    const measured = p.h ? undefined : measureRows?.(i)
    const h = p.h ? heightToRows(p.h, defaultRows, vhToRows)
                  : Math.max(1, measured ?? defaultRows)
    if (col + w > GRID_COLS) { col = 0; rowTop += rowMaxH; rowMaxH = 0 }
    out.push({ ...p, x: col, y: rowTop, w, h })
    col += w
    rowMaxH = Math.max(rowMaxH, h)
  })
  return out
}
