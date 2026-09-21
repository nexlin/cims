// CMP 시간대별 차트의 **계열 묶음** (`map.series-bar.series = 'cmp-groups'`).
//
//   서비스축(VoLTE/PTT)은 SIP 전용이라 CMP 는 나눌 축이 없어 계열이 `전체` 하나였다.
//   그러면 24시간 중 19시간이 같은 값(하트비트)이라 차트가 평평해 아무것도 못 읽는다.
//   교차표에 쓰는 묶음 사전을 계열 축으로 재사용한다.
//
//   **PTT 를 셋으로 쪼개지 않는 이유**: 2주 실측(2026-09-08~21)에서 `참여/세션` 이 8번 중
//   7번 정확히 4.0 이었다 — 비율이 고정이면 세 선은 같은 모양의 복사본이다. 따로 움직이는
//   축은 일반통화 ↔ PTT ↔ 배경 셋뿐이다.
//
//   실행: node tests/frontend/cmp_series.test.mjs <번들 경로>
//   번들: npx esbuild ems/core/console/src/widgets/shapes/dataSourceSpec.ts --bundle \
//           --format=esm --platform=node --outfile=<번들 경로>
const bundle = process.argv[2]
if (!bundle) { console.error('usage: node cmp_series.test.mjs <dataSourceSpec 번들 경로>'); process.exit(2) }
const { buildDataSource } = await import(bundle)

let pass = 0, fail = 0
const chk = (name, cond, extra = '') => {
  if (cond) { pass++; console.log('  ok   ' + name) }
  else { fail++; console.log('  FAIL ' + name + (extra ? ' — ' + extra : '')) }
}

const cmpSpec = {
  id: 'cims.msg.cmp', label: 'CMP 메시지', endpoint: '/stats/messages/cmp',
  shapes: ['series-bar'],
  map: { 'series-bar': { from: 'buckets', label: ['label', 'hour'], unit: '건',
                         series: 'cmp-groups', cells: ['in', 'out'] } },
}
const ds = buildDataSource(cmpSpec)

// 실측 한 버킷 (2026-09-16 13시 .46) — API total 82 와 맞아야 한다.
const raw13 = { buckets: [{ label: '2026-09-16 13',
  in: { HEARTBEAT: 13, PTT_GROUP_ADD: 2, PTT_JOIN: 8, FLOOR_TALKERS: 16,
        PTT_LEAVE: 8, PTT_GROUP_REMOVE: 2 },
  out: { HEARTBEAT: 13, PTT_GROUP_ADD: 2, PTT_JOIN: 8, PTT_LEAVE: 8, PTT_GROUP_REMOVE: 2 } }] }
const d13 = ds.toSeriesBar(raw13)
const v13 = d13.buckets[0].values

console.log('[1] 계열은 셋 — 일반통화 미디어 · PTT · 생존 확인')
chk('개수', d13.series.length === 3, String(d13.series.length))
chk('키', d13.series.map(s => s.key).join(' ') === 'media ptt alive',
    d13.series.map(s => s.key).join(' '))
chk('이름', d13.series.map(s => s.label).join(' ') === '일반통화 미디어 PTT 생존 확인',
    d13.series.map(s => s.label).join(' '))

console.log('[2] 하트비트는 값이 아니라 배경 — chart-muted')
chk('생존 확인 색', d13.series[2].color === 'var(--chart-muted)', d13.series[2].color)
chk('나머지는 순서 색', d13.series[0].color === 'var(--chart-1)' &&
                        d13.series[1].color === 'var(--chart-2)')

console.log('[3] 값 = 묶음 안 메서드의 in+out 합 (2026-09-16 13시 실측)')
chk('PTT = 세션+참여+발언 56', v13.ptt === 56, String(v13.ptt))
chk('생존 확인 26', v13.alive === 26, String(v13.alive))
chk('일반통화 0', v13.media === 0, String(v13.media))
chk('세 계열 합 = API total 82', v13.media + v13.ptt + v13.alive === 82,
    String(v13.media + v13.ptt + v13.alive))

console.log('[4] 일반통화 묶음에 청취 leg(RELAY_TAP_*) 도 든다')
const dTap = ds.toSeriesBar({ buckets: [{ label: 'x',
  in: { RELAY_ADD: 1, RELAY_TAP_ADD: 2, RELAY_NAT_LATCHED: 3 }, out: { RELAY_REMOVE: 4 } }] })
chk('1+2+3+4 = 10', dTap.buckets[0].values.media === 10,
    String(dTap.buckets[0].values.media))

console.log('[5] 사전에 없는 메서드는 어느 계열에도 안 든다')
const dUnk = ds.toSeriesBar({ buckets: [{ label: 'x', in: { NOPE_CMD: 9, HEARTBEAT: 1 }, out: {} }] })
chk('NOPE_CMD 는 빠지고 하트비트만', dUnk.buckets[0].values.alive === 1 &&
    dUnk.buckets[0].values.media === 0 && dUnk.buckets[0].values.ptt === 0)

console.log('[6] in/out 이 없거나 비어도 0 — 터지지 않는다')
const dNone = ds.toSeriesBar({ buckets: [{ label: 'x' }, { label: 'y', in: {}, out: null }] })
chk('필드 없음 → 0', dNone.buckets[0].values.ptt === 0 && dNone.buckets[0].values.alive === 0)
chk('빈 map·null → 0', dNone.buckets[1].values.ptt === 0 && dNone.buckets[1].values.alive === 0)

console.log('[7] SIP 식 계열 배열은 그대로 동작한다 (회귀)')
const sipDs = buildDataSource({
  id: 'cims.msg.sip', label: 'SIP', endpoint: '/stats/messages/sip', shapes: ['series-bar'],
  map: { 'series-bar': { from: 'buckets', label: ['label'], unit: '건', series: [
    { key: 'volte', label: 'VoLTE', value: 'volte' },
    { key: 'ptt', label: 'PTT', value: 'ptt' },
    { key: 'unknown', label: '미분류', value: 'unknown', color: 'chart-muted' }] } },
})
const dSip = sipDs.toSeriesBar({ buckets: [{ label: 'a', volte: 7, ptt: 3, unknown: 1 }] })
chk('계열 셋·값 그대로', dSip.series.length === 3 &&
    dSip.buckets[0].values.volte === 7 && dSip.buckets[0].values.ptt === 3)
chk('미분류 색 유지', dSip.series[2].color === 'var(--chart-muted)', dSip.series[2].color)

console.log('[8] 메서드 비중(분포) 도 같은 묶음 색을 쓴다')
const distDs = buildDataSource({
  id: 'cims.msg.cmp', label: 'CMP', endpoint: '/stats/messages/cmp', shapes: ['distribution'],
  map: { distribution: { fromObject: 'method_counts', totalPath: 'total', series: 'cmp-groups' } },
})
const dist = distDs.toDistribution({ total: 82, method_counts:
  { HEARTBEAT: 26, PTT_JOIN: 16, FLOOR_TALKERS: 16, RELAY_ADD: 6, NOPE_CMD: 3 } })
chk('계열·색이 시계열과 같다',
    JSON.stringify(dist.series) === JSON.stringify(d13.series),
    JSON.stringify(dist.series))
const byLabel = Object.fromEntries(dist.items.map(it => [it.label, it.parts]))
chk('RELAY_ADD → media 한 덩이', JSON.stringify(byLabel.RELAY_ADD) === '{"media":6}',
    JSON.stringify(byLabel.RELAY_ADD))
chk('PTT_JOIN·FLOOR_TALKERS → ptt', JSON.stringify(byLabel.PTT_JOIN) === '{"ptt":16}' &&
    JSON.stringify(byLabel.FLOOR_TALKERS) === '{"ptt":16}')
chk('HEARTBEAT → alive', JSON.stringify(byLabel.HEARTBEAT) === '{"alive":26}')
chk('사전에 없는 메서드는 색을 안 찍는다(빈 막대 = 사전 갱신 신호)',
    byLabel.NOPE_CMD === undefined, JSON.stringify(byLabel.NOPE_CMD))

console.log('[9] SIP 분포는 종전대로 partsObject 를 쓴다 (회귀)')
const sipDist = buildDataSource({
  id: 'cims.msg.sip', label: 'SIP', endpoint: '/stats/messages/sip', shapes: ['distribution'],
  map: { distribution: { fromObject: 'method_counts', totalPath: 'total',
                         partsObject: 'method_service', series: [
    { key: 'volte', label: 'VoLTE', value: 'volte' },
    { key: 'ptt', label: 'PTT', value: 'ptt' }] } },
}).toDistribution({ total: 10, method_counts: { INVITE: 10 },
                    method_service: { INVITE: { volte: 7, ptt: 3 } } })
chk('계열 둘', sipDist.series.length === 2)
chk('parts 가 서비스축 그대로',
    JSON.stringify(sipDist.items[0].parts) === '{"volte":7,"ptt":3}',
    JSON.stringify(sipDist.items[0].parts))


console.log('[10] 한 소스가 선언한 shape 어댑터가 **전부** 만들어진다')
//   실제 기술자는 shape 5개를 한 소스에 declare 한다. cmp-groups 분기에서 일찍 빠져나가면
//   뒤쪽 shape(matrix·distribution·table)의 어댑터가 안 생기고, 화면은 그 위젯을
//   "데이터 없음" 으로 그린다 — 2026-09-21 배포에서 실제로 그랬다(0.2.155 → 0.2.156 수정).
const fullDs = buildDataSource({
  id: 'cims.msg.cmp', label: 'CMP 메시지', endpoint: '/stats/messages/cmp',
  shapes: ['time-bar', 'series-bar', 'distribution', 'table', 'matrix'],
  map: {
    'time-bar': { from: 'buckets', label: ['label'], value: 'count' },
    'series-bar': { from: 'buckets', label: ['label'], unit: '건',
                    series: 'cmp-groups', cells: ['in', 'out'] },
    distribution: { fromObject: 'method_counts', totalPath: 'total', series: 'cmp-groups' },
    table: { fromObject: 'method_counts', columns: ['메서드', '건수'] },
    matrix: { from: 'buckets', label: ['bucket', 'label'], cells: ['in', 'out'],
              unit: '건', order: 'message' },
  },
})
for (const [name, fn] of [['toTimeBar', fullDs.toTimeBar], ['toSeriesBar', fullDs.toSeriesBar],
                          ['toDistribution', fullDs.toDistribution], ['toTable', fullDs.toTable],
                          ['toMatrix', fullDs.toMatrix]]) {
  chk(`${name} 존재`, typeof fn === 'function', String(typeof fn))
}
const sample = { total: 82, method_counts: { HEARTBEAT: 26, PTT_JOIN: 16 },
                 buckets: [{ bucket: '2026-09-16', label: '2026-09-16', count: 82,
                             in: { HEARTBEAT: 13, PTT_JOIN: 8 }, out: { HEARTBEAT: 13, PTT_JOIN: 8 } }] }
chk('matrix 가 열·행을 낸다', fullDs.toMatrix(sample).columns.length === 2 &&
    fullDs.toMatrix(sample).rows.length === 1)
chk('distribution 이 항목을 낸다', fullDs.toDistribution(sample).items.length === 2)


console.log(`\n총 ${pass + fail} / PASS ${pass} / FAIL ${fail}`)
process.exit(fail ? 1 : 0)
