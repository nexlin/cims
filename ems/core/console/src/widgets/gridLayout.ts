// 자유 2D 그리드 레이아웃 엔진 — 순수 함수(React/DOM 비의존, 런타임 측정치는 인자 주입)라
// 결정적이고 단독 검증 가능하다. 위젯 배치를 12칸 × N행 셀 그리드로 다루며, 이동/리사이즈 후
// 겹침을 아래로 밀고(collision) 빈 행을 위로 당긴다(compaction/중력). gridstack 의 float=false 와 동일.
//
// 좌표계: WidgetPlacement.{x,y,w,h} 는 모두 그리드 셀 단위(가로/세로 통일).
//  · x: 0-based 열(0..11), y: 0-based 행(>=0), w: 열 span(1..12), h: 행 span(>=1).
// x/y 없는 placement 는 legacy flow(GridRenderer 가 순서+w 로 렌더) — flowToGrid 로 grid 로 migrate.

import type { WidgetPlacement } from './types'

export const GRID_COLS = 48       // 가로 48칸 (칸당 ≈2.08% — 세로 2%와 동일 수준의 세밀도). 12 의 배수라 환산 깔끔.
export const COL_SCALE = GRID_COLS / 12   // legacy 12-칸 기준 폭(seed w·defaultSize.w) → 현재 칸수로 환산(×4)
export const ROW_H_VH = 2         // 행 높이 = 화면 세로의 2%(vh) → 리사이즈가 2% 단위로 스냅(세밀). 정수 행은
                                  // 셀 단위(조작감)로 두되, 한 행의 실제 크기는 vh 라 모든 해상도에서 같은 세로
                                  // 비율로 보인다. 더 세밀히(1%)/거칠게 하려면 이 값만 조정. index.css @media 도 함께.
export const GRID_GAP = 12        // 카드 간 기본 간격(px) — layout.gap 미지정 시 사용
export const DEFAULT_ROWS = Math.round(30 / ROW_H_VH)   // 자동/미지정 높이 위젯 기본 ≈ 화면 세로 30%
const LEGACY_PX_PER_ROW = 40      // legacy 픽셀 높이(h>100) → 행 변환용 명목 px (아주 드묾)

const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, v))

// 정규화된 그리드 박스 — 옵셔널 필드에 안전한 기본값 부여.
export interface GridBox { x: number; y: number; w: number; h: number }

export function gridBox(p: WidgetPlacement): GridBox {
  const w = clamp(Math.round(p.w ?? GRID_COLS), 1, GRID_COLS)
  return {
    w,
    h: Math.max(1, Math.round(p.h ?? DEFAULT_ROWS)),
    x: clamp(Math.round(Number.isFinite(p.x) ? (p.x as number) : 0), 0, GRID_COLS - w),
    y: Math.max(0, Math.round(Number.isFinite(p.y) ? (p.y as number) : 0)),
  }
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
export const clampH = (h: number) => Math.max(1, Math.round(h))
export const clampY = (y: number) => Math.max(0, Math.round(y))

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
export function compact(items: WidgetPlacement[], movedKey?: number): WidgetPlacement[] {
  const boxes = items.map(gridBox)
  const order = items.map((_, i) => i).sort((a, b) => {
    if (boxes[a].y !== boxes[b].y) return boxes[a].y - boxes[b].y
    const pa = a === movedKey ? 0 : 1
    const pb = b === movedKey ? 0 : 1
    if (pa !== pb) return pa - pb
    if (boxes[a].x !== boxes[b].x) return boxes[a].x - boxes[b].x
    return a - b
  })
  const placed: GridBox[] = []
  const result = items.slice()
  for (const i of order) {
    const b = boxes[i]
    b.y = firstFreeY(placed, b)
    placed.push(b)
    result[i] = { ...items[i], x: b.x, y: b.y, w: b.w, h: b.h }
  }
  return result
}

// 이동 커밋 — 목표 x·y(clamp) 세팅 후 compact. movedKey 로 옮긴 항목이 slot 을 차지.
export function moveItem(items: WidgetPlacement[], key: number, x: number, y: number): WidgetPlacement[] {
  const next = items.map((p, i) => i === key ? { ...p, x: clampX(x, gridBox(p).w), y: clampY(y) } : { ...p })
  return compact(next, key)
}

// 리사이즈 커밋 — x 고정한 채 w·h(clamp) 세팅 후 compact. 확장으로 생기는 겹침은 아래로 밀림.
export function resizeItem(items: WidgetPlacement[], key: number, w: number, h: number): WidgetPlacement[] {
  const next = items.map((p, i) => {
    if (i !== key) return { ...p }
    return { ...p, w: clampW(w, gridBox(p).x), h: clampH(h) }
  })
  return compact(next, key)
}

// 새 위젯을 첫 빈 슬롯(x=0, 최상단 빈 행)에 배치.
export function addToFirstFree(
  items: WidgetPlacement[], placement: WidgetPlacement, defW?: number, defH?: number,
): WidgetPlacement[] {
  const w = clamp(Math.round((placement.w ?? defW ?? 12) * COL_SCALE), 1, GRID_COLS)  // 12-칸 기준 → 현재 칸수
  const h = Math.max(1, Math.round(placement.h ?? defH ?? DEFAULT_ROWS))
  const next = [...items.map(p => ({ ...p })), { ...placement, x: 0, y: Number.MAX_SAFE_INTEGER, w, h }]
  return compact(next)   // 큰 y → 정렬상 마지막 → firstFreeY 가 첫 빈 x=0 슬롯에 착지
}

// 제거 후 compaction 으로 빈칸 닫힘.
export function removeAt(items: WidgetPlacement[], index: number): WidgetPlacement[] {
  return compact(items.filter((_, i) => i !== index))
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
export function flowToGrid(
  widgets: WidgetPlacement[],
  getDefaultW: (widgetId: string) => number | undefined,
  vhToRows: (vh: number) => number,
  defaultRows = DEFAULT_ROWS,
): WidgetPlacement[] {
  let col = 0
  let rowTop = 0
  let rowMaxH = 0
  const out: WidgetPlacement[] = []
  for (const p of widgets) {
    const w = clamp(Math.round((p.w ?? getDefaultW(p.widgetId) ?? 12) * COL_SCALE), 1, GRID_COLS)  // 12-칸 기준 → 현재 칸수
    const h = heightToRows(p.h, defaultRows, vhToRows)
    if (col + w > GRID_COLS) { col = 0; rowTop += rowMaxH; rowMaxH = 0 }
    out.push({ ...p, x: col, y: rowTop, w, h })
    col += w
    rowMaxH = Math.max(rowMaxH, h)
  }
  return out
}
