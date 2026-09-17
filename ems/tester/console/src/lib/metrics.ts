// 계측기 팩 지표 헬퍼 — 시나리오 기대치 → 차트 임계·KPI 모서리, 누계 판정(라이브 근사), 절차 진행 표, 예상 소요·Little 검산.
// 컨트롤러가 최종 판정(expect_results)을 내리기 전, 진행 중 화면이 같은 규칙으로 '지금 어디쯤인가'를 보여 주기 위한 것이다.
import type { ScenarioDoc, ScenarioStep, HistSummary, RunPlan, CompiledStep } from '@tester/api/tester'

export interface Thresholds {
  rrd_ms?: number; srd_ms?: number; sdd_ms?: number; jitter_ms?: number
  rtp_loss_pct?: number; ser_pct?: number; scr_pct?: number
  dtmf_rx_pct?: number; q850_rx_pct?: number; early_media_pct?: number; prack_pct?: number
}

function pct(exp: unknown): number | undefined {
  if (typeof exp === 'number') return exp
  if (exp && typeof exp === 'object') {
    const o = exp as Record<string, unknown>
    for (const k of ['p95', 'p99', 'max', 'p50', 'min']) if (typeof o[k] === 'number') return o[k] as number
  }
  return undefined
}

/** 시나리오 flow 의 expect 에서 차트 임계(붉은 점선)·KPI 기대치를 뽑는다 — 지연은 p95(없으면 p99/max), 손실은 max, 비율은 최소값. */
export function thresholdsFromScenario(sc: ScenarioDoc | null | undefined): Thresholds {
  const out: Thresholds = {}
  for (const st of sc?.flow ?? []) {
    for (const [m, e] of Object.entries(st.expect ?? {})) {
      const v = pct(e)
      if (v == null) continue
      if (m in out) continue
      ;(out as Record<string, number>)[m] = v
    }
  }
  return out
}

export type Judge = 'ok' | 'bad' | 'none'

/** 누계 판정(라이브 근사) — 컨트롤러 _summary 와 같은 방향: 지연은 분위수 ≤ 기대, 손실은 ≤ max, 비율은 ≥ 기대. 표본이 없으면 none. */
export function judgeMetric(metric: string, exp: unknown, counters: Record<string, number>, timers: Record<string, HistSummary>, ratios: Record<string, number | null>): Judge {
  if (metric === 'code') return 'none'
  if (metric.endsWith('_pct')) {
    const v = ratios[metric]
    if (v == null) return 'none'
    const want = pct(exp)
    if (want == null) return 'none'
    return LOWER_BETTER.has(metric) ? (v <= want ? 'ok' : 'bad') : (v >= want ? 'ok' : 'bad')
  }
  const h = timers[metric]
  if (!h || !h.count) return 'none'
  if (typeof exp === 'number') return (h.max ?? Infinity) <= exp ? 'ok' : 'bad'
  const o = (exp ?? {}) as Record<string, unknown>
  let ok = true
  for (const q of ['p50', 'p95', 'p99', 'max'] as const) {
    if (typeof o[q] !== 'number') continue
    const got = h[q]
    if (got == null || got > (o[q] as number)) ok = false
  }
  if (typeof o.min === 'number' && (h.min == null || h.min < o.min)) ok = false
  void counters
  return ok ? 'ok' : 'bad'
}

/** 낮을수록 좋은 비율(기대치 = 상한) — 컨트롤러 LOWER_BETTER_RATIOS + 호별 손실 */
export const LOWER_BETTER = new Set(['rtp_loss_pct', 'isa_pct'])
/** 비율 지표 값 — 컨트롤러 RATIO_METRICS 와 같은 분자/분모. */
export function ratiosFrom(c: Record<string, number>): Record<string, number | null> {
  const r = (n: number, d: number) => (d ? (100 * n) / d : null)
  const lossTot = (c.rtp_rx ?? 0) + (c.rtp_lost ?? 0)
  return {
    ser_pct: r(c.sessions ?? 0, c.attempts ?? 0),
    scr_pct: r(c.completed ?? 0, c.sessions ?? 0),
    seer_pct: r(c.seer_ok ?? 0, c.invite_tx ?? 0),
    isa_pct: r(c.isa_fail ?? 0, c.invite_tx ?? 0),
    join_tap_pct: r(c.join_ssrc2 ?? 0, c.join_ok ?? 0),
    video_pct: r(c.video_ok ?? 0, c.video_offered ?? 0),
    fork_alert_pct: r(c.fork_rx ?? 0, c.fork_expected ?? 0),
    listen_pct: r(c.listen_ok ?? 0, c.listen_tx ?? 0),
    early_rtp_pct: r(c.early_rtp_ok ?? 0, c.progress_tx ?? 0),
    floor_grant_pct: r(c.floor_granted ?? 0, c.floor_request_tx ?? 0),
    rtp_loss_pct: lossTot ? (100 * (c.rtp_lost ?? 0)) / lossTot : null,
    dtmf_rx_pct: r(c.dtmf_rx ?? 0, c.dtmf_tx ?? 0),
    q850_rx_pct: r(c.q850_rx ?? 0, c.q850_tx ?? 0),
    early_media_pct: r(c.early_media ?? 0, c.progress_tx ?? 0),
    prack_pct: r(c.prack_rx ?? 0, c.progress_tx ?? 0),
  }
}

export interface FunnelRow {
  idx: number; step: string; phase: 'prelude' | 'body' | 'epilogue'; actors: string
  entered: number | null; done: number | null; active: number | null; failed: number | null
  expects: { metric: string; judge: Judge; text: string }[]
}

function stepActors(s: ScenarioStep): string {
  if (s.from) return `${s.from}${s.to ? ` → ${s.to}` : ''}`
  return (s.who ?? []).join(', ')
}

/** 절차 진행 — 시나리오 단계마다 인스턴스가 어디에 있는지(워커 누계 카운터로 근사) + 누계 기대치 칩. */
export function funnelRows(sc: ScenarioDoc | null | undefined, plan: RunPlan | null | undefined, c: Record<string, number>,
                           timers: Record<string, HistSummary>, gauges: Record<string, number>): FunnelRow[] {
  const flow = sc?.flow ?? []
  const ratios = ratiosFrom(c)
  const ph = (i: number): FunnelRow['phase'] => plan?.phases ? (plan.phases.prelude.includes(i) ? 'prelude' : plan.phases.epilogue.includes(i) ? 'epilogue' : 'body') : 'body'
  return flow.map((s, i) => {
    let entered: number | null = null, done: number | null = null, active: number | null = null, failed: number | null = null
    switch (s.step) {
      case 'register': entered = (c.registered_ok ?? 0) + (c.registered_fail ?? 0); done = c.registered_ok ?? 0; failed = c.registered_fail ?? 0; break
      case 'invite': entered = c.attempts ?? 0; done = c.sessions ?? 0; failed = c.failed ?? 0; break
      case 'progress': entered = c.attempts ?? 0; done = c.progress_tx ?? 0; break
      case 'answer': entered = c.attempts ?? 0; done = c.sessions ?? 0; break
      case 'reject': entered = c.attempts ?? 0; done = Object.entries(c).filter(([k]) => k.startsWith('codes.') && !k.startsWith('codes.1') && !k.startsWith('codes.2')).reduce((a, [, v]) => a + v, 0); break
      case 'media_hold': entered = c.sessions ?? 0; done = c.completed ?? 0; active = gauges.concurrent_sessions ?? null; break
      case 'hold': case 'resume': entered = c.sessions ?? 0; done = c.reinvite_ok ?? 0; failed = c.reinvite_fail ?? 0; break
      case 'dtmf': entered = c.dtmf_tx ?? 0; done = c.dtmf_rx ?? 0; break
      case 'refer': entered = c.refer_tx ?? 0; done = Object.entries(c).filter(([k]) => k.startsWith('refer_codes.2')).reduce((a, [, v]) => a + v, 0); break
      case 'pickup': case 'replaces': case 'join': entered = c[`${s.step}_tx`] ?? 0; done = c[`${s.step}_ok`] ?? 0; break
      case 'subscribe': case 'publish': entered = c[`${s.step}_tx`] ?? 0; done = Object.entries(c).filter(([k]) => k.startsWith(`${s.step}_codes.2`)).reduce((a, [, v]) => a + v, 0); break
      case 'bye': entered = c.sessions ?? 0; done = c.completed ?? 0; break
      case 'deregister': done = null; break
      default: break
    }
    const expects = Object.entries(s.expect ?? {}).map(([metric, e]) => ({
      metric, judge: judgeMetric(metric, e, c, timers, ratios), text: expectText(e),
    }))
    return { idx: i, step: s.step, phase: ph(i), actors: stepActors(s), entered, done, active, failed, expects }
  })
}

export function expectText(exp: unknown): string {
  if (exp == null) return '—'
  if (typeof exp !== 'object') return String(exp)
  return Object.entries(exp as Record<string, unknown>).map(([k, v]) => `${k} ${typeof v === 'number' ? (Number.isInteger(v) ? v : v.toFixed(2)) : String(v)}`).join(' ')
}

/** 세션 지속 추정(SDT) — 컨트롤러 tester_plan._sdt_seconds 와 같은 규칙(body 단계 after_ms + seconds + 1 s). */
export function sdtSeconds(steps: CompiledStep[] | undefined, phases: RunPlan['phases'] | undefined): number {
  if (!steps) return 0
  const body = new Set(phases?.body ?? [])
  let sdt = 1
  for (const s of steps) {
    if (phases && !body.has(s.src ?? s.idx)) continue
    sdt += (s.after_ms ?? 0) / 1000 + (s.seconds ?? 0)
  }
  return Math.round(sdt * 10) / 10
}

export interface ProfileLike { model?: string; start?: number; step?: number; hold_s?: number; max?: number; rate?: number; duration_s?: number; ramp_s?: number; burst_size?: number; burst_interval_s?: number; ht?: number; ihs_threshold_pct?: number; stop_on?: { target_cpu_pct?: number; csp_5xx_pct?: number; ser_pct_min?: number } }

/** 예상 소요(초) — 컨트롤러 estimate_duration 과 같은 규칙. */
export function estimateDuration(p: ProfileLike | null | undefined, rateTotal: number, maxInstances: number | null | undefined, sdt: number): number | null {
  if (!p || !p.model) {
    if (!maxInstances) return null
    return Math.ceil(maxInstances / Math.max(0.1, rateTotal)) + sdt + 5
  }
  if (p.model === 'constant' || p.model === 'soak' || p.model === 'burst') return (p.duration_s ?? 0) + sdt + 5
  if (p.model === 'step') { const n = Math.floor(((p.max ?? 0) - (p.start ?? 0)) / (p.step || 1)) + 1; return n * (p.hold_s ?? 0) + sdt + 5 }
  if (p.model === 'ramp') return (p.ramp_s ?? 0) + (p.hold_s ?? 0) + sdt + 5
  return null
}

/** step 프로파일의 율 사다리 */
export function stepRates(p: ProfileLike | null | undefined): number[] {
  if (!p || p.model !== 'step' || p.start == null || p.max == null) return []
  const out: number[] = []
  let r = p.start
  const st = p.step || 1
  while (r < p.max && out.length < 200) { out.push(Math.round(r * 100) / 100); r += st }
  out.push(p.max)
  return out
}

export function minRoleIdentities(plan: RunPlan | null | undefined): number | null {
  if (!plan?.roles) return null
  const totals = Object.values(plan.roles).map(r => r.total)
  return totals.length ? Math.min(...totals) : null
}

export const CHART_DEFS = [
  { id: 'rate', label: '시도율 · 실패/초', unit: '/s' },
  { id: 'conc', label: '동시 세션', unit: '' },
  { id: 'ser', label: 'SER', unit: '%' },
  { id: 'srd', label: 'SRD p95', unit: 'ms' },
  { id: 'loss', label: 'RTP 손실', unit: '%' },
  { id: 'cpu', label: '워커 · 대상 CPU', unit: '%' },
] as const
