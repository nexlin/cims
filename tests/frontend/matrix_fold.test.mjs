// 교차표 **값 없는 구간 접기** 규칙.
//
//   시간축을 구간 전체로 채우면(sip_statistics.md §2.1c) 1분 단위 하루 조회가 900행이 되고
//   그중 대부분이 0 이다. 값 있는 몇 줄이 파묻히지 않게 긴 빈 구간을 한 줄로 접는다.
//
//   실행: node tests/frontend/matrix_fold.test.mjs <번들 경로>
//   번들: npx esbuild ems/core/console/src/widgets/shapes/matrixFold.ts --bundle \
//           --format=esm --platform=node --outfile=<번들 경로>
const bundle = process.argv[2]
if (!bundle) { console.error('usage: node matrix_fold.test.mjs <matrixFold 번들 경로>'); process.exit(2) }
const { foldBlankRuns, EMPTY_RUN_MIN, blankRange } = await import(bundle)

let pass = 0, fail = 0
const chk = (name, cond, extra = '') => {
  if (cond) { pass++; console.log('  ok   ' + name) }
  else { fail++; console.log('  FAIL ' + name + (extra ? ' — ' + extra : '')) }
}
const COLS = [{ key: 'a' }, { key: 'b' }, { key: 'r' }]
const row = (label, a = 0, b = 0, r = null) => ({ label, cells: { a, b, r }, total: 0 })
const blanks = (n, from = 0) => Array.from({ length: n }, (_, i) => row(`b${from + i}`))

console.log('[1] 긴 빈 구간은 접는다')
let g = foldBlankRuns([row('x', 1), ...blanks(12), row('y', 2)], COLS)
chk('묶음 3개', g.length === 3, String(g.length))
chk('가운데만 접기 대상', !g[0].fold && g[1].fold && !g[2].fold,
    JSON.stringify(g.map(x => x.fold)))
chk('접은 묶음이 12행', g[1].rows.length === 12, String(g[1].rows.length))
chk('값 있는 행은 그대로', g[0].rows.length === 1 && g[2].rows.length === 1)

console.log('[2] 짧은 빈 구간은 접지 않는다 — 접은 줄이 더 길어진다')
g = foldBlankRuns([row('x', 1), ...blanks(EMPTY_RUN_MIN - 1), row('y', 2)], COLS)
chk(`빈 ${EMPTY_RUN_MIN - 1}행은 접지 않음`, g.every(x => !x.fold), JSON.stringify(g.map(x => x.fold)))
g = foldBlankRuns([row('x', 1), ...blanks(EMPTY_RUN_MIN), row('y', 2)], COLS)
chk(`빈 ${EMPTY_RUN_MIN}행부터 접음 (경계)`, g[1].fold === true)

console.log('[3] "빈 행" = 모든 열이 0 또는 값 없음')
chk('0 만 있는 행은 빈 행', foldBlankRuns(blanks(10), COLS)[0].fold === true)
g = foldBlankRuns([...blanks(5), row('n', 0, 0, null), ...blanks(5)], COLS)
chk('null 은 빈 값이라 구간이 안 끊긴다', g.length === 1 && g[0].fold === true,
    JSON.stringify(g.map(x => x.rows.length)))
g = foldBlankRuns([...blanks(5), row('v', 0, 1), ...blanks(5)], COLS)
chk('값이 하나라도 있으면 구간이 끊긴다', g.length === 3 && !g[1].fold,
    JSON.stringify(g.map(x => [x.rows.length, x.fold])))

console.log('[4] 전부 비었거나 전부 값이 있는 경우')
chk('전부 빈 900행 → 한 줄로 접힌다',
    (() => { const q = foldBlankRuns(blanks(900), COLS); return q.length === 1 && q[0].fold })())
chk('전부 값 있으면 접을 것이 없다',
    foldBlankRuns([row('a', 1), row('b', 2)], COLS).every(x => !x.fold))
chk('행이 없으면 묶음도 없다', foldBlankRuns([], COLS).length === 0)

console.log('[5] 순서·행 보존 — 접어도 자료가 사라지지 않는다')
const src = [row('x', 1), ...blanks(11), row('y', 2), ...blanks(3), row('z', 3)]
g = foldBlankRuns(src, COLS)
const flat = g.flatMap(x => x.rows.map(r => r.label))
chk('모든 행이 순서대로 남는다', flat.join(',') === src.map(r => r.label).join(','))

console.log('[6] 접힌 줄의 범위 표기 — 단위마다 라벨 모양이 다르다')
const rr = labels => blankRange(labels.map(l => row(l)))
chk('1분·같은 날 → 날짜를 뗀다',
    rr(['2026-09-16 13:24', '2026-09-16 13:33']) === '13:24 ~ 13:33',
    rr(['2026-09-16 13:24', '2026-09-16 13:33']))
chk('시간 단위도 같은 규칙',
    rr(['2026-09-16 01:00', '2026-09-16 11:00']) === '01:00 ~ 11:00',
    rr(['2026-09-16 01:00', '2026-09-16 11:00']))
chk('자정을 넘으면 날짜를 남긴다',
    rr(['2026-09-16 23:50', '2026-09-17 00:20']) === '2026-09-16 23:50 ~ 2026-09-17 00:20',
    rr(['2026-09-16 23:50', '2026-09-17 00:20']))
chk('일 단위 — 날짜 라벨은 그대로',
    rr(['2026-09-11', '2026-09-14']) === '2026-09-11 ~ 2026-09-14',
    rr(['2026-09-11', '2026-09-14']))
chk('월 단위', rr(['2026-03', '2026-07']) === '2026-03 ~ 2026-07', rr(['2026-03', '2026-07']))
chk('연 단위', rr(['2024', '2026']) === '2024 ~ 2026', rr(['2024', '2026']))
chk('한 행뿐이면 한 번만 적는다', rr(['2026-09-11']) === '2026-09-11', rr(['2026-09-11']))

console.log(`\n합계: ${pass} pass / ${fail} fail`)
process.exit(fail ? 1 : 0)
