// 시나리오 캔버스 모델 — 역할→풀 해석(컨트롤러 compile 규칙의 편집 시점 재현), 구간(prelude/body/epilogue), 세션(다이얼로그) 추적,
// 누적 시각, 검증(vocab 기반 — 컨트롤러 Scenario 모델과 같은 규칙 + kind 게이트 + in-dialog + during), YAML 직렬화(동봉 파일 꼴, 머리 주석 보존).
// YAML → 문서는 컨트롤러 POST /validate 가 돌려준 doc 을 쓴다(파서는 서버 하나).
import type { ScenarioDoc, ScenarioStep, TopologyDoc, PoolDoc, ScenarioVocab } from '@tester/api/tester'

export interface During { at_s: number; step: 'dtmf' | 'hold' | 'resume' | 'refer' | 'media_send' | 'media_stop'; who?: string[]; from?: string; to?: string; payload?: string; sample?: string; loop?: boolean; expect?: Record<string, unknown> }
export interface Step extends ScenarioStep { during?: During[]; group?: string }
export interface Doc extends Omit<ScenarioDoc, 'flow' | 'roles' | 'tags' | 'target_evidence'> {
  roles: NonNullable<ScenarioDoc['roles']>; flow: Step[]; tags: string[]; target_evidence: NonNullable<ScenarioDoc['target_evidence']>; comment?: string
}
export type Sel = { kind: 'scenario' } | { kind: 'role'; id: string } | { kind: 'step'; idx: number } | { kind: 'sub'; idx: number; k: number }
export type Lv = 'error' | 'warning' | 'info'
export interface Issue { lv: Lv; who: string; msg: string; ref: Sel | null }

export const deep = <T,>(o: T): T => JSON.parse(JSON.stringify(o))
export const INDIALOG = new Set(['dtmf', 'hold', 'resume', 'refer'])
/** 송출 제어(미디어 평면) — 그 호에서 SDP 가 오간 뒤(progress/answer 뒤)에만, rtp: none 호에는 못 둔다. 행위자는 who(여럿) */
export const MEDIA_CTL = new Set(['media_send', 'media_stop'])
/** media_hold 의 during 에 둘 수 있는 것 = 통화 중 동작 + 송출 제어 */
export const DURING_OK = new Set([...INDIALOG, ...MEDIA_CTL])
/** 기대치 임계의 부등호 — min 은 하한(≥), 나머지(p50/p95/p99/max)는 상한(≤). 단일 값(code·비율)은 = */
export const thrOp = (q: string) => (q === 'min' ? '≥' : '≤')
export const roles = (sc: Doc) => Object.keys(sc.roles ?? {})

export interface Resolved { pools: Record<string, PoolDoc>; kind: 'ue' | 'peer' | 'real-ue'; logical: string; byName: boolean }
export function resolvePool(sc: Doc, topo: TopologyDoc | null, role: string): Resolved | null {
  const r = sc.roles?.[role]; if (!r || !topo) return null
  const name = r.pool
  if (topo.pools?.[name]) return { pools: { [name]: topo.pools[name] }, kind: topo.pools[name].kind, logical: name, byName: true }
  const g = Object.entries(topo.pools ?? {}).filter(([, p]) => p.group === name)
  if (g.length) return { pools: Object.fromEntries(g), kind: g[0][1].kind, logical: name, byName: false }
  return null
}
export function roleKindTag(sc: Doc, topo: TopologyDoc | null, role: string): string {
  const r = resolvePool(sc, topo, role); if (!r) return '?'
  const p = Object.values(r.pools)[0]; return p.kind === 'peer' ? p.profile : p.kind
}

export function phases(sc: Doc): { pre: number; epi: number } {
  const f = sc.flow ?? []; let pre = 0
  while (pre < f.length && (f[pre].step === 'register' || f[pre].step === 'wait')) pre++
  let epi = f.length
  while (epi > pre && f[epi - 1].step === 'deregister') epi--
  return { pre, epi }
}
export const phaseOf = (sc: Doc, i: number): 'prelude' | 'body' | 'epilogue' => { const { pre, epi } = phases(sc); return i < pre ? 'prelude' : i >= epi ? 'epilogue' : 'body' }

export interface Session { a: string; b: string; start: number; est: number | null; end: number; prog: number | null; via?: 'refer' }
/** 다이얼로그 추적 — invite 로 열리고 answer/progress 로 확립, bye/reject 로 닫힌다. refer 는 닫으면서 새 세션을 연다. */
export function sessions(sc: Doc): Session[] {
  const out: Session[] = []; const open: Session[] = []
  ;(sc.flow ?? []).forEach((s, i) => {
    if (s.step === 'invite' && s.from && s.to) { open.push({ a: s.from, b: s.to, start: i, est: null, end: -1, prog: null }); return }
    if ((s.step === 'answer' || s.step === 'progress') && s.who) { const o = open.find(x => x.est == null && s.who!.includes(x.b)) ?? open.find(x => x.est == null); if (!o) return; if (s.step === 'answer') o.est = i; else if (o.prog == null) o.prog = i; return }
    if (s.step === 'refer' && s.from && s.to) { const o = open.find(x => x.est != null && (x.a === s.from || x.b === s.from)); if (o) { o.end = i; open.splice(open.indexOf(o), 1); out.push(o); open.push({ a: o.a === s.from ? o.b : o.a, b: s.to, start: i, est: null, end: -1, prog: null, via: 'refer' }) } return }
    if (s.step === 'bye' || s.step === 'reject') { const who = s.from ?? (s.who ?? [])[0]; const o = open.find(x => x.a === who || x.b === who) ?? open[0]; if (o) { o.end = i; open.splice(open.indexOf(o), 1); out.push(o) } }
  })
  for (const o of open) { o.end = (sc.flow ?? []).length; out.push(o) }
  return out
}
/** 행 i 가 속한 호 — SDP 가 오갔는가(183 progress 또는 200 answer 뒤) + 그 호의 invite.media.rtp */
export function mediaCtx(sc: Doc, i: number): { sdp: boolean; rtp: 'auto' | 'none' | 'explicit' } | null {
  const o = sessions(sc).find(x => i > x.start && i <= x.end); if (!o) return null
  const first = Math.min(o.prog ?? Infinity, o.est ?? Infinity)
  return { sdp: i > first, rtp: sc.flow[o.start]?.media?.rtp ?? 'auto' }
}
export const inSession = (sc: Doc, i: number) => sessions(sc).some(o => o.est != null && i > o.est && (i < o.end || (i === o.end && sc.flow[i]?.step === 'refer')))

export function secondsOf(s: Step, bind: Record<string, number>): number {
  if (s.seconds == null || s.seconds === '') return 0
  if (typeof s.seconds === 'string') { const m = s.seconds.match(/^\$\{(\w+)\}$/); return m ? (bind[m[1]] ?? 0) : (+s.seconds || 0) }
  return +s.seconds || 0
}
/** 누적 시각(body 기준, s) — after_ms + seconds 합. 행 왼쪽 t+ 와 Little SDT 가 같은 값 */
export function timeline(sc: Doc, bind: Record<string, number>): (number | null)[] {
  const { pre } = phases(sc); let t = 0
  return (sc.flow ?? []).map((s, i) => { if (i < pre) return null; const at = t; t += (s.after_ms ?? 0) / 1000 + secondsOf(s, bind); return at })
}
export function bindVars(sc: Doc): string[] { const out = new Set<string>(); for (const s of sc.flow ?? []) if (typeof s.seconds === 'string') { const m = s.seconds.match(/^\$\{(\w+)\}$/); if (m) out.add(m[1]) } return [...out] }

export function validate(sc: Doc, topo: TopologyDoc | null, vocab: ScenarioVocab | null, bind: Record<string, number>): Issue[] {
  const out: Issue[] = []
  const E = (who: string, msg: string, ref: Sel | null) => out.push({ lv: 'error', who, msg, ref })
  const W = (who: string, msg: string, ref: Sel | null) => out.push({ lv: 'warning', who, msg, ref })
  const I = (who: string, msg: string, ref: Sel | null) => out.push({ lv: 'info', who, msg, ref })
  if (!/^[A-Z0-9][A-Z0-9-]{2,63}$/.test(sc.id ?? '')) E('id', 'id 는 대문자·숫자·하이픈 3~64 자', { kind: 'scenario' })
  if (!roles(sc).length) E('roles', '역할이 하나 이상 필요하다', { kind: 'scenario' })
  if (!(sc.flow ?? []).length) E('flow', '단계가 하나 이상 필요하다', { kind: 'scenario' })
  const names = new Set(roles(sc))
  for (const [n, r] of Object.entries(sc.roles ?? {})) {
    if (!r.pool) E(`roles.${n}`, 'pool 이 비었다', { kind: 'role', id: n })
    if (r.disjoint_from && !names.has(r.disjoint_from)) E(`roles.${n}`, `disjoint_from=${r.disjoint_from} 는 정의된 역할이 아니다`, { kind: 'role', id: n })
    else if (r.disjoint_from && sc.roles![r.disjoint_from].pool !== r.pool) W(`roles.${n}`, `disjoint_from 상대(${r.disjoint_from})가 다른 풀 — 창 분리는 같은 풀에서만 뜻이 있다`, { kind: 'role', id: n })
    if (topo && !resolvePool(sc, topo, n)) E(`roles.${n}`, `기준 토폴로지 ${topo.name} 에 pool '${r.pool}' 이 없다 (이름 또는 group)`, { kind: 'role', id: n })
  }
  const { pre, epi } = phases(sc)
  const bound = new Set(Object.keys(bind))
  ;(sc.flow ?? []).forEach((s, i) => {
    const who = `flow[${i}] ${s.step}`; const ref: Sel = { kind: 'step', idx: i }
    const D = vocab?.steps[s.step]
    if (vocab && !D) { E(who, `알 수 없는 단계 ${s.step}`, ref); return }
    for (const rr of [...(s.who ?? []), s.from, s.to]) if (rr && !names.has(rr)) E(who, `정의되지 않은 역할 '${rr}' 참조`, ref)
    if (D) {
      if (D.actor === 'who' && !(s.who && s.who.length)) E(who, 'who 가 필요하다', ref)
      if ((D.actor === 'from' || D.actor === 'fromto') && !(s.from || (s.who && s.who.length))) E(who, 'from 또는 who 가 필요하다', ref)
      if (D.actor === 'seconds' && (s.seconds === undefined || s.seconds === null || s.seconds === '')) E(who, 'seconds 가 필요하다', ref)
      if (!D.supported) W(who, '워커 미지원 단계 — 컴파일 시 거절된다(워커 재빌드 필요)', ref)
    }
    if (s.step === 'refer' && !(s.from && s.to)) E(who, 'refer 는 from(전달자)과 to(전달 대상)가 필요하다', ref)
    if (s.step === 'invite' && !s.to) E(who, 'invite 는 to 가 필요하다', ref)
    if (s.step === 'dtmf' && !(s.payload && /^[0-9*#A-Da-d]+$/.test(s.payload))) E(who, 'dtmf 는 payload 숫자열(0-9 * # A-D)이 필요하다', ref)
    if (s.cause != null && !['bye', 'reject'].includes(s.step)) E(who, 'cause 는 bye/reject 에만 둔다', ref)
    for (const k of Object.keys(s.expect ?? {})) if (vocab && !vocab.metrics[k]) E(who, `알 수 없는 지표 '${k}'`, ref)
    if (typeof s.seconds === 'string') { const m = s.seconds.match(/^\$\{(\w+)\}$/); if (m && !bound.has(m[1])) W(who, `바인딩 \${${m[1]}} 값이 없다 — profile.ht 또는 요청 bindings`, ref) }
    if ((s.step === 'register' || s.step === 'deregister') && i >= pre && i < epi) E(who, `${s.step} 는 body 안에 둘 수 없다 — 앞쪽(prelude) 또는 끝(epilogue)으로`, ref)
    // kind 게이트
    const actors = [...(s.who ?? []), s.from].filter((x): x is string => !!x)
    for (const a of actors) {
      const rp = resolvePool(sc, topo, a); if (!rp) continue
      if (D?.kind === 'peer' && rp.kind !== 'peer') E(who, `${s.step} 의 행위자 '${a}' 는 피어 풀이어야 한다 (${rp.logical} = ${rp.kind})`, ref)
      if (D?.kind === 'ue' && rp.kind === 'peer') E(who, `${s.step} 의 행위자 '${a}' 는 UE 풀이어야 한다`, ref)
      if (D?.kind === 'ue|trunk' && rp.kind === 'peer' && !(Object.values(rp.pools)[0] as { register?: unknown }).register) E(who, `역할 '${a}' 의 피어 풀에는 register(트렁크 계정)가 없다 — 고정 IP 피어는 등록하지 않는다`, ref)
    }
    if (s.cause != null) { const a = s.from ?? (s.who ?? [])[0]; const rp = a ? resolvePool(sc, topo, a) : null; if (rp && rp.kind !== 'peer') E(who, 'cause(Reason Q.850) 는 피어 역할만 보낸다', ref) }
    if (s.step === 'invite' && s.to) { const rp = resolvePool(sc, topo, s.to); const p0 = rp && Object.values(rp.pools)[0]; if (p0 && (p0 as { answer?: string }).answer === 'silent') I(who, `to '${s.to}' 는 answer=silent 풀 — 무응답 상대(failover 시험)`, ref) }
    if (INDIALOG.has(s.step) && !inSession(sc, i)) E(who, `${s.step} 는 확립된 세션 안에서만 — answer/progress 뒤·bye 앞에 두거나 media_hold 의 during 으로`, ref)
    const ctl = [...(MEDIA_CTL.has(s.step) ? [s as Step | During] : []), ...(s.during ?? []).filter(d => MEDIA_CTL.has(d.step))]
    if (ctl.length) {
      const mc = mediaCtx(sc, i)
      if (!mc || !mc.sdp) E(who, '송출 제어(media_send/media_stop)는 invite 뒤 SDP 가 오간 다음(progress/answer 뒤)에만 둔다', ref)
      else if (mc.rtp === 'none') E(who, '그 호의 invite.media.rtp 가 none(시그널링 전용) — 송출 제어를 둘 수 없다', ref)
      for (const c of ctl) if (c.sample && topo && !topo.media?.samples?.[c.sample]) E(who, `샘플 '${c.sample}' 가 기준 토폴로지 ${topo.name} 의 media.samples 에 없다`, ref)
    }
    if ((s.sample != null || s.loop != null) && s.step !== 'media_send') E(who, 'sample/loop 은 media_send 에만 둔다', ref)
    if (s.step === 'invite' && s.media?.rtp === 'explicit' && !(sc.flow ?? []).some(x => x.step === 'media_send' || (x.during ?? []).some(d => d.step === 'media_send'))) W(who, 'media.rtp: explicit 인데 media_send 가 없다 — 아무도 송출하지 않는다', ref)
    if (s.step === 'invite' && s.media?.rtp === 'none') I(who, '시그널링 전용 — RTP 를 송수신하지 않는다(RTP 기대치는 표본이 없다)', ref)
    if (s.during) {
      if (s.step !== 'media_hold') E(who, 'during 은 media_hold 에만 둔다', ref)
      const len = secondsOf(s, bind)
      s.during.forEach((d, k) => {
        const w2 = `${who}.during[${k}] ${d.step}`; const r2: Sel = { kind: 'sub', idx: i, k }
        if (!DURING_OK.has(d.step)) E(w2, `during 에는 통화 중 동작(dtmf/hold/resume/refer)과 송출 제어(media_send/media_stop)만`, r2)
        if (d.at_s == null || d.at_s < 0 || (len && d.at_s > len)) E(w2, `at_s 는 0 ~ seconds(${len}) 안`, r2)
        if (!(d.from || (d.who && d.who.length))) E(w2, MEDIA_CTL.has(d.step) ? 'who 가 필요하다' : 'from 이 필요하다', r2)
        if (d.step === 'refer' && !(d.from && d.to)) E(w2, 'refer 는 from·to 가 필요하다', r2)
        if (d.step === 'dtmf' && !(d.payload && /^[0-9*#A-Da-d]+$/.test(d.payload))) E(w2, 'dtmf payload 숫자열 필요', r2)
        for (const rr of [...(d.who ?? []), d.from, d.to]) if (rr && !names.has(rr)) E(w2, `정의되지 않은 역할 '${rr}'`, r2)
        const a = d.from ?? (d.who ?? [])[0]; const rp = a ? resolvePool(sc, topo, a) : null
        if (d.step === 'refer' && rp && rp.kind !== 'peer') E(w2, `refer 의 행위자 '${a}' 는 피어 풀이어야 한다`, r2)
      })
    }
  })
  const referenced = new Set((sc.flow ?? []).flatMap(s => [...(s.who ?? []), s.from, s.to]).filter(Boolean))
  for (const n of roles(sc)) if (!referenced.has(n)) I(`roles.${n}`, '흐름에서 참조되지 않는 역할 — 풀만 열린다(failover 상대 등)', { kind: 'role', id: n })
  return out
}

// ── 편집 조작 ─────────────────────────────────────────────────────────────
export function newStep(kind: string, sc: Doc, topo: TopologyDoc | null, vocab: ScenarioVocab | null): Step {
  const R = roles(sc); const isPeer = (n: string) => resolvePool(sc, topo, n)?.kind === 'peer'
  const peer = R.find(isPeer), ue = R.find(n => !isPeer(n)) ?? R[0]
  const D = vocab?.steps[kind]
  const s: Step = { step: kind }
  if (D?.actor === 'who') s.who = [D.kind === 'peer' ? (peer ?? R[0]) : (ue ?? R[0])].filter(Boolean)
  else if (D?.actor === 'from') s.from = D.kind === 'peer' ? (peer ?? R[0]) : (ue ?? R[0])
  else if (D?.actor === 'fromto') { s.from = D.kind === 'peer' ? (peer ?? R[0]) : (ue ?? R[0]); s.to = R.find(x => x !== s.from) ?? R[0] }
  else if (D?.actor === 'seconds') s.seconds = kind === 'media_hold' ? '${ht}' : 5
  if (kind === 'invite') s.media = { audio: 'amr-wb' }
  if (kind === 'answer' || kind === 'progress') s.after_ms = 500
  if (kind === 'dtmf') s.payload = '1234#'
  if (kind === 'reject') { s.payload = '486'; s.after_ms = 300 }
  if (kind === 'register') s.expect = { code: 200 }
  return s
}
// ── during ↔ 행 변환 — 통화 중 동작(dtmf/hold/resume/refer)과 송출 제어(media_send/media_stop)는 독립 행으로도, media_hold 의 during 으로도 둘 수 있다 ──
/** 행 i(in-dialog 단계)를 flow[holdIdx](media_hold) 의 during 으로. 반환 = 새 during 의 (holdIdx, k) — holdIdx 는 행 제거로 밀릴 수 있다 */
export function stepToDuring(sc: Doc, i: number, holdIdx: number, at_s: number): { idx: number; k: number } | null {
  const s = sc.flow[i], h = sc.flow[holdIdx]
  if (!s || !h || h.step !== 'media_hold' || !DURING_OK.has(s.step) || i === holdIdx) return null
  const d: During = { at_s, step: s.step as During['step'] }
  if (s.from) d.from = s.from; else if (s.who?.length) d.who = [...s.who]
  if (s.to) d.to = s.to; if (s.payload) d.payload = s.payload; if (s.expect && Object.keys(s.expect).length) d.expect = s.expect
  if (s.sample) d.sample = s.sample; if (s.loop != null) d.loop = s.loop
  h.during = [...(h.during ?? []), d]
  sc.flow.splice(i, 1)
  const idx = i < holdIdx ? holdIdx - 1 : holdIdx
  return { idx, k: sc.flow[idx].during!.length - 1 }
}
/** flow[holdIdx].during[k] 를 독립 행으로 — at(행 사이 위치, 삭제 전 인덱스 기준)에 끼운다. 반환 = 새 행 인덱스 */
export function duringToStep(sc: Doc, holdIdx: number, k: number, at: number): number {
  const h = sc.flow[holdIdx]; const d = h?.during?.[k]; if (!d) return -1
  const s: Step = { step: d.step }
  if (d.from) s.from = d.from; else if (d.who?.length) s.who = [...d.who]
  if (d.to) s.to = d.to; if (d.payload) s.payload = d.payload; if (d.expect) s.expect = d.expect
  if (d.sample) s.sample = d.sample; if (d.loop != null) s.loop = d.loop
  h.during!.splice(k, 1); if (!h.during!.length) delete h.during
  sc.flow.splice(Math.max(0, Math.min(sc.flow.length, at)), 0, s)
  return Math.max(0, Math.min(sc.flow.length - 1, at))
}
/** 행 i 가 붙을 수 있는 가장 가까운 media_hold — 위쪽 먼저, 없으면 아래쪽. 없으면 -1 */
export function nearestHold(sc: Doc, i: number): number {
  for (let j = i - 1; j >= 0; j--) if (sc.flow[j].step === 'media_hold') return j
  for (let j = i + 1; j < sc.flow.length; j++) if (sc.flow[j].step === 'media_hold') return j
  return -1
}

export function renameRole(sc: Doc, o: string, n: string): boolean {
  if (!n || n === o || sc.roles[n]) return false
  const R: Doc['roles'] = {}; for (const [k, v] of Object.entries(sc.roles)) R[k === o ? n : k] = v
  for (const v of Object.values(R)) if (v.disjoint_from === o) v.disjoint_from = n
  sc.roles = R
  const fix = (x: { who?: string[]; from?: string; to?: string }) => { if (x.who) x.who = x.who.map(y => y === o ? n : y); if (x.from === o) x.from = n; if (x.to === o) x.to = n }
  for (const s of sc.flow) { fix(s); for (const d of s.during ?? []) fix(d) }
  return true
}
export function removeRole(sc: Doc, n: string) {
  delete sc.roles[n]
  for (const v of Object.values(sc.roles)) if (v.disjoint_from === n) delete v.disjoint_from
  const fix = (x: { who?: string[]; from?: string; to?: string }) => { if (x.who) x.who = x.who.filter(y => y !== n); if (x.from === n) delete x.from; if (x.to === n) delete x.to }
  for (const s of sc.flow) { fix(s); for (const d of s.during ?? []) fix(d) }
}
export function reorderRoles(sc: Doc, from: string, to: string) {
  const R = roles(sc); const i = R.indexOf(from), j = R.indexOf(to); if (i < 0 || j < 0 || i === j) return
  R.splice(i, 1); R.splice(j, 0, from); const O: Doc['roles'] = {}; for (const k of R) O[k] = sc.roles[k]; sc.roles = O
}
export function addRole(sc: Doc, topo: TopologyDoc | null, kind: 'ue' | 'peer'): string {
  const cand = Object.entries(topo?.pools ?? {}).find(([, p]) => (kind === 'peer') === (p.kind === 'peer'))
  const logical = cand ? (cand[1].group ?? cand[0]) : (kind === 'peer' ? 'peer_x' : 'ue_x')
  const base = kind === 'peer' ? 'peer' : 'ue'; let n = base, k = 2; while (sc.roles[n]) n = base + k++
  sc.roles[n] = { pool: logical }; return n
}

// ── YAML 직렬화 (동봉 파일과 같은 flow-style 한 줄 = 단계 하나) ──────────────
const yv = (v: unknown): string => typeof v === 'string' ? (/^[A-Za-z_][\w.-]*$/.test(v) && !/^(true|false|null)$/.test(v) && !/^\d/.test(v) ? v : JSON.stringify(v))
  : Array.isArray(v) ? `[${v.map(yv).join(', ')}]` : (v && typeof v === 'object') ? `{ ${Object.entries(v as Record<string, unknown>).map(([k, x]) => `${k}: ${yv(x)}`).join(', ')} }` : String(v)
export function toYaml(sc: Doc): string {
  const L: string[] = []
  if (sc.comment) for (const c of sc.comment.split('\n')) L.push(`# ${c}`)
  L.push(`id: ${sc.id}`)
  if (sc.title) L.push(`title: ${yv(sc.title)}`)
  L.push(`tags: [${(sc.tags ?? []).join(', ')}]`)
  L.push('roles:'); for (const [n, r] of Object.entries(sc.roles ?? {})) L.push(`  ${n}: ${yv(r)}`)
  L.push('flow:'); const w = Math.max(...(sc.flow ?? []).map(s => s.step.length), 6)
  for (const s of sc.flow ?? []) {
    const { step, ...rest } = s as Step & Record<string, unknown>
    const r2: Record<string, unknown> = { ...rest }
    if (s.during) r2.during = [...s.during].sort((a, b) => (a.at_s ?? 0) - (b.at_s ?? 0)).map(d => ({ at_s: d.at_s, ...Object.fromEntries(Object.entries(d).filter(([k]) => k !== 'at_s')) }))
    const body = Object.entries(r2).filter(([, v]) => v != null && !(Array.isArray(v) && !v.length) && !(typeof v === 'object' && !Array.isArray(v) && !Object.keys(v as object).length)).map(([k, v]) => `${k}: ${yv(v)}`).join(', ')
    L.push(`  - { step: ${(step + ',').padEnd(w + 1)} ${body} }`)
  }
  if (sc.target_evidence && sc.target_evidence.length) { L.push('target_evidence:'); for (const e of sc.target_evidence) L.push(`  - ${yv(e)}`) }
  return L.join('\n') + '\n'
}
/** YAML 원문의 머리 주석(첫 키 앞 # 줄) */
export function headComment(yaml: string | null | undefined): string {
  if (!yaml) return ''
  const out: string[] = []
  for (const ln of yaml.split('\n')) { if (!ln.trim()) continue; if (/^\s*#/.test(ln)) { out.push(ln.replace(/^\s*#\s?/, '')); continue } break }
  return out.join('\n')
}
export function fromApiDoc(doc: ScenarioDoc | Record<string, unknown> | null | undefined, yaml?: string | null): Doc {
  const d = deep((doc ?? {}) as Doc)
  d.roles = d.roles ?? {}; d.flow = d.flow ?? []; d.tags = d.tags ?? []; d.target_evidence = d.target_evidence ?? []
  d.comment = headComment(yaml)
  return d
}
