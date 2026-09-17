// 못 본 구간 알림(경고 띠)의 통로 — sourceNotice.
//
//   실행: node tests/frontend/source_notice.test.mjs <번들 경로>
//   번들: npx esbuild ems/core/console/src/widgets/shapes/sourceNotice.ts --bundle \
//           --format=esm --platform=node --outfile=<번들 경로>
//
// 지키는 것:
//   ① 서버가 `warning` 을 안 주면 띠는 없다 — 정상일 때 화면이 시끄러워지지 않는다
//   ② 빠진 날짜를 문구에 함께 적는다 — "며칠 빠졌다" 만으로는 조치할 수 없다
//   ③ 조회 조건이 키의 일부다 — 구간을 바꾸면 옛 경고가 저절로 빠진다(지우는 시점 관리 없음)
//   ④ 같은 조건을 보는 블록이 여럿이어도 문구는 하나다(중복 제거)
const bundle = process.argv[2]
if (!bundle) { console.error('usage: node source_notice.test.mjs <sourceNotice 번들 경로>'); process.exit(2) }
const { noticeOf, noticeScope, publishNotice, noticesFor } = await import(bundle)

let pass = 0, fail = 0
const chk = (name, cond, extra = '') => {
  if (cond) { pass++; console.log('  ok   ' + name) }
  else { fail++; console.log('  FAIL ' + name + (extra ? ' — ' + extra : '')) }
}

// 훅(useNotices)은 구독만 얹은 껍데기고 판정은 noticesFor 가 한다 — 시험은 그쪽을 본다.
const readNotices = scope => noticesFor(scope)

console.log('[1] 문구 만들기')
chk('warning 이 없으면 null', noticeOf({ coverage: { missing_days: ['2026-09-01'] } }) === null)
chk('warning 만 있으면 그대로', noticeOf({ warning: '8일이 빠졌습니다' }) === '8일이 빠졌습니다')
const withDays = noticeOf({ warning: '3일이 빠졌습니다',
                            coverage: { missing_days: ['2026-09-01', '2026-09-02', '2026-09-18'] } })
chk('빠진 날짜를 월-일로 붙인다', withDays === '3일이 빠졌습니다 (09-01 · 09-02 · 09-18)', withDays)
const many = noticeOf({ warning: 'x', coverage: { missing_days: Array.from({ length: 20 },
  (_, i) => `2026-09-${String(i + 1).padStart(2, '0')}`) } })
chk('너무 길면 뒤를 접는다', / 외 8일\)$/.test(many), many)
chk('null 응답에도 안 터진다', noticeOf(null) === null)

console.log('[2] 조회 조건별로 갈린다')
const A = noticeScope('2026-09-01 00:00', '2026-09-18 23:59', '1d')
const B = noticeScope('2026-09-17 00:00', '2026-09-17 23:59', '1h')
publishNotice(`${A}|cims.svc.volte`, '8일이 빠졌습니다')
chk('그 조건에서는 보인다', readNotices(A).length === 1)
chk('다른 조건에서는 안 보인다', readNotices(B).length === 0)

console.log('[3] 중복·해제')
publishNotice(`${A}|cims.svc.ptt`, '8일이 빠졌습니다')
chk('같은 문구는 한 번만', readNotices(A).length === 1)
publishNotice(`${A}|cims.svc.ptt`, '다른 경고')
chk('다른 문구는 함께 보인다', readNotices(A).length === 2)
publishNotice(`${A}|cims.svc.volte`, null)
publishNotice(`${A}|cims.svc.ptt`, null)
chk('경고가 사라지면 띠도 사라진다', readNotices(A).length === 0)

console.log('[4] 무한정 쌓이지 않는다')
for (let i = 0; i < 100; i++) publishNotice(`s${i}|src`, `w${i}`)
let total = 0
for (let i = 0; i < 100; i++) total += readNotices(`s${i}`).length
chk('보관 상한이 있다', total > 0 && total <= 40, String(total))

console.log(`\n합계: ${pass} pass / ${fail} fail`)
process.exit(fail ? 1 : 0)
