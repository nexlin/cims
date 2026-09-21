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
const { parseMsgKey, msgGroupOf, compareMsgKeys, cmpGroupLabelOf } = await import(bundle)

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

console.log('[7] CMP 묶음 — 뜻이 한 벌인 명령끼리 (키에 /코드 가 없어 메서드=묶음 이 되는 것을 막는다)')
chk('열고-닫는 짝이 같은 묶음',
    msgGroupOf('RELAY_ADD') === '일반통화 미디어' && msgGroupOf('RELAY_REMOVE') === '일반통화 미디어')
chk('PTT 참여', msgGroupOf('PTT_JOIN') === 'PTT 참여' && msgGroupOf('PTT_LEAVE') === 'PTT 참여')
chk('생존 확인은 따로', msgGroupOf('HEARTBEAT') === '생존 확인')
chk('아직 안 나온 명령도 자리를 갖는다',
    msgGroupOf('RELAY_TAP_ADD') === '청취 leg' && msgGroupOf('PTT_GROUP_ABORTED') === 'PTT 그룹 세션')

console.log('[8] 묶음 제목 줄 — CMP 만 이름이 있고 SIP 은 없다')
chk('CMP 는 이름을 준다', cmpGroupLabelOf('RELAY_ADD') === '일반통화 미디어')
chk('SIP 은 안 준다(제목이 열 이름을 되풀이할 뿐)',
    cmpGroupLabelOf('INVITE') === undefined && cmpGroupLabelOf('INVITE/401') === undefined)
chk('귀속 실패도 안 준다', cmpGroupLabelOf('488') === undefined)

console.log('[9] CMP 열 순서 — 가나다가 아니라 묶음 차례, HEARTBEAT 는 맨 뒤')
const cmpReal = ['FLOOR_TALKERS', 'HEARTBEAT', 'PTT_GROUP_ADD', 'PTT_GROUP_REMOVE',
                 'PTT_JOIN', 'PTT_LEAVE', 'RELAY_ADD', 'RELAY_MODIFY', 'RELAY_REMOVE']
const want = 'RELAY_ADD RELAY_MODIFY RELAY_REMOVE PTT_GROUP_ADD PTT_GROUP_REMOVE '
           + 'PTT_JOIN PTT_LEAVE FLOOR_TALKERS HEARTBEAT'
chk('실측 키 한 벌 (2026-09-16 .46 CMP)', sorted(cmpReal) === want, sorted(cmpReal))
const order = sorted(cmpReal).split(' ')
chk('ADD 바로 뒤에 REMOVE',
    order.indexOf('PTT_GROUP_REMOVE') === order.indexOf('PTT_GROUP_ADD') + 1 &&
    order.indexOf('PTT_LEAVE') === order.indexOf('PTT_JOIN') + 1)
chk('HEARTBEAT 가 맨 뒤', order[order.length - 1] === 'HEARTBEAT')

console.log('[10] SIP 순서는 그대로다 (회귀)')
chk('가나다 + 요청→응답 유지',
    sorted(['BYE', 'INVITE/200', 'ACK', 'INVITE']) === 'ACK BYE INVITE INVITE/200',
    sorted(['BYE', 'INVITE/200', 'ACK', 'INVITE']))
chk('묶음 이름은 메서드 그대로', msgGroupOf('INVITE/200') === 'INVITE')

console.log('[11] 비교 함수가 한 줄 세우기여야 한다 — 시작 순서가 달라도 결과가 같다')
const mixed = [...cmpReal, 'INVITE', 'BYE', '488']
chk('두 번 섞어 정렬해도 같은 결과',
    sorted(mixed) === sorted([...mixed].reverse()), sorted(mixed))


console.log(`\n총 ${pass + fail} / PASS ${pass} / FAIL ${fail}`)
process.exit(fail ? 1 : 0)
