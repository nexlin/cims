// 그리드 엔진 단위 테스트 — **세로 예산 + 잠금(lock)** 동작 (console_platform.md §3.0).
//   캔버스가 화면 한 장(GRID_ROWS 행)이라 위젯을 키우면 다른 위젯 자리를 뺏는다. 그 규칙이
//   조용히 깨지면(겹침·캔버스 초과) 관제 화면이 스크롤되거나 카드가 잘리므로 여기서 고정한다.
//
//   실행: node tests/frontend/grid_budget.test.mjs <번들 경로>
//   번들: npx esbuild ems/core/console/src/widgets/gridLayout.ts --bundle --format=esm \
//           --platform=node --outfile=<번들 경로>
//   (검증 파이프라인은 S1-UNIT-GRID-BUDGET 가 번들→실행을 한 번에 한다)
const bundle = process.argv[2]
if (!bundle) { console.error('usage: node grid_budget.test.mjs <gridLayout 번들 경로>'); process.exit(2) }
const { applyBox, moveItem, addToFirstFree, setLockedAt, gridBox, usedRows, fitToBudget,
        GRID_ROWS, MIN_ROWS } = await import(bundle)
let pass = 0, fail = 0
const chk = (name, cond, extra = '') => {
  if (cond) { pass++; console.log('  ok   ' + name) }
  else { fail++; console.log('  FAIL ' + name + (extra ? ' — ' + extra : '')) }
}
const box = (items, i) => { const b = gridBox(items[i]); return `${b.x},${b.y} ${b.w}x${b.h}` }

// 3개 위젯 — A(위), B(중), C(아래). 전부 전폭. 합 = 48행(예산 꽉).
const base = [
  { widgetId: 'A', x: 0, y: 0,  w: 48, h: 16 },
  { widgetId: 'B', x: 0, y: 16, w: 48, h: 16 },
  { widgetId: 'C', x: 0, y: 32, w: 48, h: 16 },
]
console.log('[1] 예산 꽉 찬 상태')
chk('합계 = 48행', usedRows(base) === 48, String(usedRows(base)))

console.log('[2] A 잠금 후 B 를 8행 키우면 → C 가 8행 줄어야 한다')
let s = setLockedAt(base, 0, true)
s = applyBox(s, 1, { x: 0, y: 16, w: 48, h: 24 })
chk('A 그대로 (0,0 48x16)', box(s, 0) === '0,0 48x16', box(s, 0))
chk('B 커짐 (0,16 48x24)', box(s, 1) === '0,16 48x24', box(s, 1))
chk('C 줄어듦 (0,40 48x8)', box(s, 2) === '0,40 48x8', box(s, 2))
chk('예산 안 (<=48)', usedRows(s) <= GRID_ROWS, String(usedRows(s)))

console.log('[3] 잠긴 위젯은 자리를 뺏기지 않는다 — A·C 둘 다 잠그면 B 확대 거절')
s = setLockedAt(setLockedAt(base, 0, true), 2, true)
const before = s.map((_, i) => box(s, i)).join(' / ')
const after = applyBox(s, 1, { x: 0, y: 16, w: 48, h: 24 })
chk('배치 불변(조작 거절)', after.map((_, i) => box(after, i)).join(' / ') === before,
    after.map((_, i) => box(after, i)).join(' / '))

console.log('[4] 최소 높이(MIN_ROWS) 이하로는 안 줄어든다')
s = applyBox(base, 0, { x: 0, y: 0, w: 48, h: 40 })
const heights = s.map((_, i) => gridBox(s[i]).h)
chk(`모든 h >= ${MIN_ROWS}`, heights.every(h => h >= MIN_ROWS), JSON.stringify(heights))
chk('예산 안', usedRows(s) <= GRID_ROWS, String(usedRows(s)))

console.log('[5] 잠긴 위젯은 밀리지도 않는다 — C 를 A 자리로 이동 시도')
s = setLockedAt(base, 0, true)
s = moveItem(s, 2, 0, 0)
chk('A 여전히 y=0', gridBox(s[0]).y === 0, box(s, 0))
chk('예산 안', usedRows(s) <= GRID_ROWS, String(usedRows(s)))

console.log('[6] 자리 없으면 위젯 추가 거절')
const full = [{ widgetId: 'X', x: 0, y: 0, w: 48, h: 48, locked: true }]
chk('추가 안 됨', addToFirstFree(full, { widgetId: 'Y' }, 12, 14).length === 1)
chk('잠금 없으면 자리 만들어 추가', addToFirstFree(
  [{ widgetId: 'X', x: 0, y: 0, w: 48, h: 48 }], { widgetId: 'Y' }, 12, 14).length === 2)

console.log('[7] 다른 열 띠는 건드리지 않는다 — 좌/우 2열, 좌측만 확대')
const twoCol = [
  { widgetId: 'L1', x: 0,  y: 0,  w: 24, h: 24 },
  { widgetId: 'L2', x: 0,  y: 24, w: 24, h: 24 },
  { widgetId: 'R1', x: 24, y: 0,  w: 24, h: 24 },
  { widgetId: 'R2', x: 24, y: 24, w: 24, h: 24 },
]
s = applyBox(twoCol, 0, { x: 0, y: 0, w: 24, h: 32 })
chk('L2 가 줄어듦', gridBox(s[1]).h === 16, box(s, 1))
chk('R1 그대로', box(s, 2) === '24,0 24x24', box(s, 2))
chk('R2 그대로', box(s, 3) === '24,24 24x24', box(s, 3))

console.log('[8] 예산을 넘는 옛 저장본은 로드 시 줄여 맞춘다(fitToBudget)')
const legacy = [
  { widgetId: 'A', x: 0, y: 0,  w: 48, h: 26 },
  { widgetId: 'B', x: 0, y: 26, w: 48, h: 26 },
  { widgetId: 'C', x: 0, y: 52, w: 48, h: 26 },   // 합 78행 — 세로 무한 시절 저장본
]
chk('원본은 초과', usedRows(legacy) === 78, String(usedRows(legacy)))
const fitted = fitToBudget(legacy)
chk('예산 안으로', usedRows(fitted) <= GRID_ROWS, String(usedRows(fitted)))
chk('겹침 없음', (() => {
  const bs = fitted.map((_, i) => gridBox(fitted[i]))
  for (let i = 0; i < bs.length; i++) for (let j = i + 1; j < bs.length; j++) {
    const a = bs[i], b = bs[j]
    if (a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y) return false
  }
  return true
})())
chk('예산 안이면 그대로(참조 동일)', fitToBudget(base) === base)

console.log('[9] 이동/교환은 남의 크기를 바꾸지 않는다 (줄여서 우겨넣지 않음)')
{
  // 예산이 꽉 찬 3단에서 C 를 맨 위로 = **순서 교환**. 총량이 같으니 성립해야 하고, 이때
  // 어느 카드도 작아지면 안 된다(예전엔 fitBudget 이 끼어들어 상대를 줄였다).
  const sizesOf = (ws) => ws.map((_, i) => gridBox(ws[i]).h).join(',')
  const swapped = moveItem(base, 2, 0, 0)
  chk('교환 성립 (C 가 맨 위로)', gridBox(swapped[2]).y === 0, box(swapped, 2))
  chk('교환해도 높이 전부 불변', sizesOf(swapped) === sizesOf(base), sizesOf(swapped))
  chk('예산 유지', usedRows(swapped) === 48, String(usedRows(swapped)))
  // 자리가 정말 없으면(잠긴 카드가 막고 있음) 줄이지 말고 거절한다.
  const blocked = setLockedAt(setLockedAt(base, 0, true), 1, true)
  const before = blocked.map((_, i) => box(blocked, i)).join(' / ')
  const rejected = moveItem(blocked, 2, 0, 0)
  chk('잠금에 막히면 이동 거절(크기 불변)',
      rejected.map((_, i) => box(rejected, i)).join(' / ') === before,
      rejected.map((_, i) => box(rejected, i)).join(' / '))
  // 여유가 있으면 이동은 되고, 그때도 크기는 아무도 안 바뀐다.
  const roomy = [
    { widgetId: 'A', x: 0, y: 0,  w: 24, h: 10 },
    { widgetId: 'B', x: 24, y: 0, w: 24, h: 10 },
    { widgetId: 'C', x: 0, y: 10, w: 24, h: 10 },
  ]
  const m2 = moveItem(roomy, 2, 24, 10)
  chk('이동 후에도 모든 높이 불변', sizesOf(m2) === sizesOf(roomy), sizesOf(m2))
  chk('실제로 옮겨짐', gridBox(m2[2]).x === 24, box(m2, 2))
  // 리사이즈는 여전히 남을 줄여서 자리를 만든다(설계대로).
  const grown = applyBox(base, 1, { x: 0, y: 16, w: 48, h: 24 })
  chk('리사이즈는 이웃 축소 허용', gridBox(grown[2]).h < 16, box(grown, 2))
}

console.log('[10] 꽉 찬 캔버스에서 다른 카드 위에 놓으면 **자리 교환**(크기 불변)')
{
  // 위/아래 2단 + 아래 좌우 2개. 아래 좌(26폭)와 우(22폭)를 맞바꾼다.
  const full = [
    { widgetId: 'TOP', x: 0,  y: 0,  w: 48, h: 12 },
    { widgetId: 'L',   x: 0,  y: 12, w: 26, h: 36 },
    { widgetId: 'R',   x: 26, y: 12, w: 22, h: 36 },
  ]
  chk('예산 꽉', usedRows(full) === 48, String(usedRows(full)))
  const sizesOf = (ws) => ws.map((_, i) => gridBox(ws[i]).w + 'x' + gridBox(ws[i]).h).join(' ')
  const sw = moveItem(full, 2, 0, 12)           // R 을 L 자리로
  chk('교환됨 (R 이 왼쪽으로)', gridBox(sw[2]).x === 0, box(sw, 2))
  chk('상대(L)는 오른쪽으로', gridBox(sw[1]).x > 0, box(sw, 1))
  chk('크기 전부 불변', sizesOf(sw) === sizesOf(full), sizesOf(sw))
  chk('예산 유지', usedRows(sw) === 48, String(usedRows(sw)))
  chk('겹침 없음', (() => {
    const bs = sw.map((_, i) => gridBox(sw[i]))
    for (let i = 0; i < bs.length; i++) for (let j = i + 1; j < bs.length; j++) {
      const a = bs[i], b = bs[j]
      if (a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y) return false
    }
    return true
  })())
  // 잠긴 카드와는 교환하지 않는다.
  const lockedR = setLockedAt(full, 1, true)
  const before = lockedR.map((_, i) => box(lockedR, i)).join(' / ')
  const no = moveItem(lockedR, 2, 0, 12)
  chk('잠긴 카드와는 교환 안 함', no.map((_, i) => box(no, i)).join(' / ') === before)
}

console.log(`\n${pass} pass / ${fail} fail`)
process.exit(fail ? 1 : 0)
