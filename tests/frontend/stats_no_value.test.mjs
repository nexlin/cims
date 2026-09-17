// 통계 매핑 단위 시험 — **집계 불가(null) 를 0 으로 접지 않는다** + 실패 원인 툴팁.
//
//   서버는 분모가 원천에 없는 비율을 `null` 로 내린다(sip_statistics.md §2.1 `rate_gap`).
//   `Number(null) === 0` 이고 0 은 유한하므로 매핑에서 한 번만 방심하면 **0% 로 보인다** —
//   멀쩡한 서비스가 성공률 0% 로 나오는 거짓 경보다(실측 2026-09-16). 그 회귀를 여기서 막는다.
//
//   실행: node tests/frontend/stats_no_value.test.mjs <번들 경로>
//   번들: npx esbuild ems/core/console/src/widgets/shapes/dataSourceSpec.ts --bundle \
//           --format=esm --platform=node --outfile=<번들 경로>
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'

const bundle = process.argv[2]
if (!bundle) { console.error('usage: node stats_no_value.test.mjs <dataSourceSpec 번들 경로>'); process.exit(2) }
const { buildDataSource, NO_VALUE } = await import(bundle)

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..', '..')
const SEED = join(REPO, 'ems/core/oam/src/services/service_descriptors_seed/cims.json')

let pass = 0, fail = 0
const chk = (name, cond, extra = '') => {
  if (cond) { pass++; console.log('  ok   ' + name) }
  else { fail++; console.log('  FAIL ' + name + (extra ? ' — ' + extra : '')) }
}

// 디스크립터는 **정본 seed 를 그대로** 읽는다 — 시험이 화면과 같은 선언을 본다.
const seed = JSON.parse(readFileSync(SEED, 'utf-8'))
const spec = seed.data_sources.find(s => s.id === 'cims.svc.ptt')
chk('cims.svc.ptt 디스크립터가 있다', !!spec)
const ds = buildDataSource(spec)

// 서버 응답 — 장부 이전 버킷이 섞인 구간(실측 형태). 비율은 전부 null 이다.
const GAP = {
  totals: {
    ptt: {
      attempts: 2, sessions: 7, talked: 7, completed: 1,
      success_rate: null, completion_rate: null, talk_rate: null, ner: null,
      talk_rate_sessions: 100.0, join_rate: 100.0, avg_duration_sec: 23.0,
      // 버킷들의 합 — 서버는 totals 와 buckets 를 같은 원천에서 낸다
      reasons: { error: 5, denied: 4, no_answer: 1 }, statuses: { 488: 3, 403: 3, 480: 1 },
      causes: { codec_mismatch: 1, not_member: 2, srtp_failed: 1, private_callee_offline: 1 },
      rate_gap: ['ptt'],
    },
  },
  buckets: [
    { bucket: '2026-09-09', ptt: { attempts: 0, sessions: 4, talked: 4, completed: 0,
        success_rate: null, completion_rate: null, talk_rate_sessions: 100.0, join_rate: 100.0,
        legs_invited: 8, legs_joined: 8, reasons: {}, causes: {} } },
    { bucket: '2026-09-15', ptt: { attempts: 2, sessions: 1, talked: 1, completed: 1,
        success_rate: 50.0, completion_rate: 100.0, talk_rate_sessions: 100.0, join_rate: 100.0,
        legs_invited: 4, legs_joined: 4, reasons: { error: 1, denied: 2, no_answer: 1 },
        causes: { codec_mismatch: 1, not_member: 2, private_callee_offline: 1 },
        statuses: { 488: 1, 403: 2, 480: 1 } } },
    // 옛 자료 — 원인 기록 전(csp 0.2.126 이전). 응답코드만 있다.
    { bucket: '2026-09-13', ptt: { attempts: 3, sessions: 0, talked: 0, completed: 0,
        success_rate: 0, completion_rate: 0, talk_rate_sessions: 0, join_rate: 0,
        legs_invited: 0, legs_joined: 0, reasons: { error: 2, denied: 1 },
        causes: {}, statuses: { 488: 2, 403: 1 } } },
    // 원인도 코드도 없는 줄 — 응답코드가 0 인 실패 경로(세션 시간창·leg 확립 실패)
    { bucket: '2026-09-14', ptt: { attempts: 2, sessions: 0, talked: 0, completed: 0,
        success_rate: 0, completion_rate: 0, talk_rate_sessions: 0, join_rate: 0,
        legs_invited: 0, legs_joined: 0, reasons: { error: 1, denied: 1 },
        causes: {}, statuses: {} } },
    { bucket: '2026-09-16', ptt: { attempts: 1, sessions: 0, talked: 0, completed: 0,
        success_rate: 0.0, completion_rate: 0.0, talk_rate_sessions: 0.0, join_rate: 0.0,
        legs_invited: 0, legs_joined: 0, reasons: { error: 1 },
        causes: { srtp_failed: 1 } } },
    { bucket: '2026-09-11' },      // 그 날 PTT 통화가 없었다 — ptt 축 자체가 없다
    // 호는 있었는데 **하나도 안 붙은** 날 — 성공률 0 은 강조 대상이다(빈칸이 아니다)
    { bucket: '2026-09-12', ptt: { attempts: 3, sessions: 0, talked: 0, completed: 0,
        success_rate: 0, completion_rate: null, talk_rate_sessions: null, join_rate: null,
        legs_invited: 0, legs_joined: 0, reasons: { denied: 3 },
        causes: { not_member: 3 }, statuses: { 403: 3 }, end_reasons: {} } },
  ],
}

console.log('[1] 지표 타일 — null 은 0% 가 아니라 빈 자리')
const kpi = ds.toKpi(GAP)
const byLabel = Object.fromEntries(kpi.items.map(i => [i.label, i.value]))
chk("성공률 = '—'", byLabel['성공률'] === NO_VALUE, String(byLabel['성공률']))
chk("완료율 = '—'", byLabel['완료율'] === NO_VALUE, String(byLabel['완료율']))
chk('호 시도 = 2 (개수는 사실이므로 그대로)', byLabel['호 시도'] === 2, String(byLabel['호 시도']))
chk('세션 = 7', byLabel['세션'] === 7, String(byLabel['세션']))
chk('소통률 = 100 (분모가 성립이라 영향 없음)', byLabel['소통률'] === 100, String(byLabel['소통률']))
chk('참여율 = 100', byLabel['참여율'] === 100, String(byLabel['참여율']))

console.log('[2] 구간별 상세 표 — null 칸은 null 로 남는다 (0 아님)')
const mx = ds.toMatrix(GAP)
const row = l => mx.rows.find(r => r.label === l)
const col = k => mx.columns.find(c => c.key === k)
chk('9/9 성공률 칸 = null', row('2026-09-09').cells.success === null,
    JSON.stringify(row('2026-09-09').cells.success))
chk('9/9 완료율 칸 = null', row('2026-09-09').cells.comp === null)
chk('9/9 성립 칸 = 4 (개수)', row('2026-09-09').cells.sessions === 4)
chk('9/15 성공률 칸 = 50', row('2026-09-15').cells.success === 50)
chk('합계 성공률 = null', col('success').total === null, JSON.stringify(col('success').total))
chk('합계 시도 = 2', col('attempts').total === 2)

console.log('[2b] 자료가 없는 버킷 — 건수는 0 (빈 자리가 아니다)')
const empty = row('2026-09-11')
chk('시도 = 0', empty.cells.attempts === 0, JSON.stringify(empty.cells.attempts))
chk('세션 = 0', empty.cells.sessions === 0, JSON.stringify(empty.cells.sessions))
chk('초대 leg = 0', empty.cells.invited === 0, JSON.stringify(empty.cells.invited))
chk('거부 = 0', empty.cells.r_denied === 0, JSON.stringify(empty.cells.r_denied))
chk('null 은 그대로 null 이다 (9/9 성공률)', row('2026-09-09').cells.success === null)
chk('비율 칸은 빈 자리 (통화가 없던 날의 0% 는 거짓 경보)',
    empty.cells.success === null && empty.cells.comp === null && empty.cells.talk === null,
    JSON.stringify([empty.cells.success, empty.cells.comp, empty.cells.talk]))

console.log('[2c] 호 없음(—) 과 전부 실패(0) 를 가른다')
const allfail = row('2026-09-12')
chk('성공률 = 0 (null 아님)', allfail.cells.success === 0, JSON.stringify(allfail.cells.success))
chk('시도 = 3', allfail.cells.attempts === 3)
chk('완료율 = null (붙은 세션이 없다)', allfail.cells.comp === null)
chk('비율 열은 0 을 칠한다', col('success').paintZero === true && col('comp').paintZero === true,
    JSON.stringify([col('success').paintZero, col('comp').paintZero]))
chk('건수 열은 칠하지 않는다', col('attempts').paintZero !== true,
    JSON.stringify(col('attempts').paintZero))
chk('호 없는 날의 성공률은 여전히 null', empty.cells.success === null)

console.log('[3] 실패 원인 — 값이 있는 칸은 끝까지 설명한다')
const d15 = row('2026-09-15').details ?? {}
chk('오류 칸 상세 = 코덱', /서비스 코덱 없음\(488\) 1건/.test(d15.r_error ?? ''), d15.r_error)
chk('거부 칸 상세 = 비멤버', /비멤버\(403\) 2건/.test(d15.r_denied ?? ''), d15.r_denied)
chk('원인 축을 열끼리 섞지 않는다', !/비멤버/.test(d15.r_error ?? ''), d15.r_error)
chk('9/9 는 원인이 없어 상세도 없다', !(row('2026-09-09').details ?? {}).r_error)
// 합계 행은 여러 버킷의 원인을 **함께** 담고, 원인으로 못 채운 만큼은 코드로 메운다
chk('합계 행 = 원인(코덱1+SRTP1) + 코드로 메운 나머지',
    /서비스 코덱 없음\(488\) 1건/.test(col('r_error').detail ?? '') &&
    /SRTP 협상 실패\(488\) 1건/.test(col('r_error').detail ?? '') &&
    /488 코덱 불일치 또는 SRTP 협상 실패 3건/.test(col('r_error').detail ?? ''),
    col('r_error').detail)
chk('0 인 칸에는 상세를 달지 않는다',
    (row('2026-09-16').details ?? {}).r_denied === undefined,
    JSON.stringify(row('2026-09-16').details))

console.log('[4] 원인 기록이 없는 옛 자료 — 응답코드로 대신 낸다')
const d13 = row('2026-09-13').details ?? {}
chk('오류 칸 → 488 2건', /488 코덱 불일치 또는 SRTP 협상 실패 2건/.test(d13.r_error ?? ''), d13.r_error)
chk('거부 칸 → 403 1건', /403 비멤버 또는 권한·조건 거부 1건/.test(d13.r_denied ?? ''), d13.r_denied)
chk('열끼리 코드를 섞지 않는다 (오류 칸에 403 없음)', !/403/.test(d13.r_error ?? ''), d13.r_error)

console.log('[4b] 무응답 열 — 상대 단말 사정은 거부·오류와 다른 칸이다')
chk('무응답 칸 = 1건', row('2026-09-15').cells.r_noanswer === 1,
    JSON.stringify(row('2026-09-15').cells.r_noanswer))
chk('무응답 툴팁 = 상대 단말',
    /상대 단말 미등록\/꺼짐\(480\) 1건/.test((row('2026-09-15').details ?? {}).r_noanswer ?? ''),
    (row('2026-09-15').details ?? {}).r_noanswer)
chk('거부 칸에 상대 사정이 섞이지 않는다',
    !/상대 단말/.test((row('2026-09-15').details ?? {}).r_denied ?? ''),
    (row('2026-09-15').details ?? {}).r_denied)
chk('오류 칸에도 섞이지 않는다',
    !/상대 단말/.test((row('2026-09-15').details ?? {}).r_error ?? ''),
    (row('2026-09-15').details ?? {}).r_error)

console.log('[5] 원인도 코드도 없으면 — 미기록으로 남기고 합을 맞춘다')
const d14 = row('2026-09-14').details ?? {}
chk('오류 칸 → 미기록 1건', /원인 미기록\(응답코드 없음\) 1건/.test(d14.r_error ?? ''), d14.r_error)
chk('거부 칸 → 미기록 1건', /원인 미기록\(응답코드 없음\) 1건/.test(d14.r_denied ?? ''), d14.r_denied)

console.log('[6] 칸의 숫자와 상세의 합은 항상 같다')
const sumOf = t => [...(t ?? '').matchAll(/ (\d+)건/g)].reduce((a, m) => a + Number(m[1]), 0)
for (const [lbl, key] of [['오류', 'r_error'], ['거부', 'r_denied'], ['무응답', 'r_noanswer']]) {
  for (const r of mx.rows) {
    const v = r.cells[key] ?? 0
    const got = sumOf((r.details ?? {})[key])
    if (v > 0) chk(`${r.label} ${lbl} 칸 ${v}건 = 상세 합 ${got}건`, v === got,
                   JSON.stringify((r.details ?? {})[key]))
  }
  const tv = col(key).total ?? 0
  chk(`합계 ${lbl} ${tv}건 = 상세 합 ${sumOf(col(key).detail)}건`, tv === sumOf(col(key).detail),
      col(key).detail)
}

// ── VoLTE — 사유 열이 종료 사유를 다 덮고, 응답코드가 제 열로만 간다 ───────────────
//
//   VoLTE 는 원인 축(`causes`)이 없어 응답코드가 툴팁의 유일한 근거다. 코드를 열마다 나눠
//   선언하므로(거절 403·404·603 / 통화중 486·600 / 무응답 408·480 / 오류 488·5xx),
//   한 코드가 두 열에 걸리면 같은 호가 두 번 세어진다. 여기서 그 회귀를 막는다.
console.log('\n[7] VoLTE — 사유 열과 응답코드 툴팁')
const vspec = seed.data_sources.find(s => s.id === 'cims.svc.volte')
chk('cims.svc.volte 디스크립터가 있다', !!vspec)
const vds = buildDataSource(vspec)

// 한 버킷 — 시도 20 = 성립 12 + 거절 4 + 통화중 2 + 무응답 1 + 오류 1.
//   `statuses` 는 그 실패들의 응답코드 합이다(200 은 담기지 않는다).
const VCALL = {
  attempts: 20, sessions: 12, talked: 12, completed: 11,
  reasons: { normal: 11, incomplete: 1, rejected: 4, busy: 2, no_answer: 1, error: 1 },
  statuses: { 603: 2, 404: 1, 403: 1, 486: 2, 480: 1, 503: 1 },
}
const VBUCKET = {
  totals: { volte: VCALL },
  buckets: [{ bucket: '2026-09-16 14:00', volte: VCALL }],
}
const vmx = vds.toMatrix(VBUCKET)
const vrow = vmx.rows[0]
const vcol = k => vmx.columns.find(c => c.key === k)

for (const [lbl, key, want] of [['거절', 'r_rejected', 4], ['통화중', 'r_busy', 2],
                                ['무응답', 'r_noanswer', 1], ['오류', 'r_error', 1],
                                ['비정상종료', 'r_incomplete', 1]]) {
  chk(`${lbl} 열이 있고 값이 ${want}`, (vrow.cells[key] ?? null) === want,
      String(vrow.cells[key]))
}

const vd = vrow.details ?? {}
chk('거절 툴팁 = 603 2건 · 404 1건 · 403 1건',
    /603 거절\(DND·착신거부\) 2건/.test(vd.r_rejected ?? '')
    && /404 없는 번호 1건/.test(vd.r_rejected ?? '')
    && /403 금지 1건/.test(vd.r_rejected ?? ''), vd.r_rejected)
chk('오류 툴팁에 503 만 온다(486·480 은 다른 열)',
    /503 서비스 불가 1건/.test(vd.r_error ?? '') && !/486|480/.test(vd.r_error ?? ''),
    vd.r_error)
chk('무응답 툴팁 = 480 1건', /480 일시 불가 1건/.test(vd.r_noanswer ?? ''), vd.r_noanswer)

console.log('[8] VoLTE — 칸의 숫자와 상세의 합이 같다')
for (const [lbl, key] of [['거절', 'r_rejected'], ['통화중', 'r_busy'],
                          ['무응답', 'r_noanswer'], ['오류', 'r_error']]) {
  const v = vrow.cells[key] ?? 0
  const got = sumOf(vd[key])
  chk(`${lbl} 칸 ${v}건 = 상세 합 ${got}건`, v === got, JSON.stringify(vd[key]))
  const tv = vcol(key).total ?? 0
  chk(`합계 ${lbl} ${tv}건 = 상세 합 ${sumOf(vcol(key).detail)}건`,
      tv === sumOf(vcol(key).detail), vcol(key).detail)
}

// ── [9] 자료가 없는 날 vs 호가 0 건인 날 ───────────────────────────────────
//   표는 빈 칸을 0 으로 그린다(§2.1c). 그 규약은 "읽었고 호가 없었다" 일 때만 참이라,
//   서버가 `missing` 을 세운 행(집계도 원본도 없는 날)은 통째로 빈칸이어야 한다.
//   실측(2026-09-17): 9/1~9/3 행이 9/17(자료 있고 0건) 행과 화면에서 똑같이 0 이었다.
console.log('[9] 자료 없는 날은 0 이 아니라 빈칸')
const MISS = {
  totals: { volte: { attempts: 0, sessions: 0, talked: 0, completed: 0,
                     success_rate: null, ner: null, talk_rate: null, completion_rate: null,
                     reasons: {}, statuses: {} } },
  buckets: [
    { bucket: '2026-09-02', missing: true },                 // 자료 자체가 없는 날
    { bucket: '2026-09-17', all: { attempts: 0 } },          // 자료는 있고 VoLTE 만 0 건
  ],
}
const mds = buildDataSource(seed.data_sources.find(s => s.id === 'cims.svc.volte'))
const mmx = mds.toMatrix(MISS)
const mrow = lbl => mmx.rows.find(r => r.label === lbl)
chk('자료 없는 날(9/02) 시도 칸이 빈칸', mrow('2026-09-02').cells.attempts === null,
    JSON.stringify(mrow('2026-09-02').cells.attempts))
chk('자료 없는 날은 사유 칸도 빈칸', mrow('2026-09-02').cells.r_rejected === null)
chk('0 건인 날(9/17) 시도 칸은 0', mrow('2026-09-17').cells.attempts === 0,
    JSON.stringify(mrow('2026-09-17').cells.attempts))
chk('0 건인 날의 비율은 빈칸', mrow('2026-09-17').cells.success === null)

//   합계 칸은 서버가 읽은 구간이면 0 으로 채워 보낸다(ensure_svc) — 없는 것은 곧 모르는 것.
const NOREAD = { totals: {}, buckets: [{ bucket: '2026-09-02', missing: true }] }
const nmx = mds.toMatrix(NOREAD)
const ncol = k => nmx.columns.find(c => c.key === k)
chk('못 읽은 구간의 합계 시도는 빈칸', ncol('attempts').total === null,
    JSON.stringify(ncol('attempts').total))
const rmx = mds.toMatrix(MISS)
chk('읽은 구간의 합계 시도는 0', rmx.columns.find(c => c.key === 'attempts').total === 0)

console.log(`\n합계: ${pass} pass / ${fail} fail`)
process.exit(fail ? 1 : 0)
