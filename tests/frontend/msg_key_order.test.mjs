// 메시지 교차표 **열 배열 순서** 규칙 (`map.matrix.order = 'message'`).
//
//   서버는 응답을 CSeq 로 원 요청에 귀속시켜 `메서드/코드` 로 내준다
//   (handlers/stats.py `_parse_msg_method`). 그런데 열을 합계 내림차순으로 늘어놓으면
//   `INVITE/401`(3.5만)이 `INVITE`(3천)보다 앞에 와서 **한 트랜잭션이 표 여기저기로
//   흩어진다.** 메서드로 묶고 그 안에서 요청 → 응답(코드 오름차순)으로 둔다.
//
//   뽑는 기준(limit)은 **여전히 합계 내림차순**이다 — 이 규칙은 뽑힌 열의 순서만 정한다.
//
//   실행: node tests/frontend/msg_key_order.test.mjs <번들 경로>
//   번들: npx esbuild ems/core/console/src/widgets/shapes/dataSourceSpec.ts --bundle \
//           --format=esm --platform=node --outfile=<번들 경로>
const bundle = process.argv[2]
if (!bundle) { console.error('usage: node msg_key_order.test.mjs <dataSourceSpec 번들 경로>'); process.exit(2) }
const { parseMsgKey, msgGroupOf, compareMsgKeys } = await import(bundle)

let pass = 0, fail = 0
const chk = (name, cond, extra = '') => {
  if (cond) { pass++; console.log('  ok   ' + name) }
  else { fail++; console.log('  FAIL ' + name + (extra ? ' — ' + extra : '')) }
}
const sorted = keys => [...keys].sort(compareMsgKeys).join(' ')

console.log('[1] 키를 요청/응답/귀속실패로 가른다')
chk('요청', JSON.stringify(parseMsgKey('INVITE')) ===
    JSON.stringify({ method: 'INVITE', code: '', orphan: false }))
chk('응답', JSON.stringify(parseMsgKey('INVITE/401')) ===
    JSON.stringify({ method: 'INVITE', code: '401', orphan: false }))
chk('귀속 실패(생코드)', JSON.stringify(parseMsgKey('488')) ===
    JSON.stringify({ method: '', code: '488', orphan: true }))

console.log('[2] 요청이 자기 응답들보다 앞')
chk('INVITE → INVITE/100 → INVITE/401',
    sorted(['INVITE/401', 'INVITE/100', 'INVITE']) === 'INVITE INVITE/100 INVITE/401',
    sorted(['INVITE/401', 'INVITE/100', 'INVITE']))

console.log('[3] 응답코드는 숫자 오름차순 (문자열 정렬이면 100 < 401 < 488 이 깨진다)')
chk('100 < 401 < 488 < 1xx 아님',
    sorted(['INVITE/488', 'INVITE/100', 'INVITE/401']) === 'INVITE/100 INVITE/401 INVITE/488')
chk('두 자리/세 자리 섞여도 수로 비교',
    sorted(['X/99', 'X/100']) === 'X/99 X/100', sorted(['X/99', 'X/100']))

console.log('[4] 메서드끼리는 가나다순, 귀속 실패는 맨 뒤')
chk('ACK BYE INVITE 순 + 생코드 후미',
    sorted(['488', 'INVITE', 'BYE', 'ACK', '100']) === 'ACK BYE INVITE 100 488',
    sorted(['488', 'INVITE', 'BYE', 'ACK', '100']))

console.log('[5] 묶음 이름 — 경계선이 메서드 단위로 그어진다')
chk('요청/응답이 같은 묶음',
    msgGroupOf('INVITE') === 'INVITE' && msgGroupOf('INVITE/401') === 'INVITE')
chk('귀속 실패는 별도 묶음', msgGroupOf('488') === '(메서드 미상)')

console.log('[6] 실측 키 한 벌 (2026-09-13 .46)')
const real = ['INVITE/401', 'INVITE', 'INVITE/100', 'INVITE/488', '488', 'OPTIONS', 'ACK', '100', 'OPTIONS/200']
chk('트랜잭션이 흩어지지 않는다',
    sorted(real) === 'ACK INVITE INVITE/100 INVITE/401 INVITE/488 OPTIONS OPTIONS/200 100 488',
    sorted(real))

console.log(`\n총 ${pass + fail} / PASS ${pass} / FAIL ${fail}`)
process.exit(fail ? 1 : 0)
