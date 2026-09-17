// 실행(라이브) 패널 — 머리(판정·상태·id·시나리오·라벨·토폴로지/프로파일/대상 빌드·라이브 점) + 진행 막대(경과/예상/종료 예정) +
// 왼쪽 단계 사다리·종료 조건 게이지 + KPI 타일(기대치 모서리) + 지표별 소형 차트 6장 + 절차 진행 표 + 실패 이벤트(코드 칩 필터 → SIP 드로어)
// + 워커 로그 + 조작(율 적용 · 단계 고정 · 즉시 중단) — test_instrument.md §7 실행 라이브 ②.
//
// 데이터 흐름: /runs/<id>/stream 의 agg 프레임(워커별 1초 버킷)을 초 단위로 합쳐 시계열을 쌓고, runs 프레임으로 상태·율·hold 를,
// events 프레임으로 실패 표를 갱신한다. 열 때 /runs/<id>/series 로 그동안의 시계열을 먼저 받아 이어 붙인다.
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { OctagonX, Gauge, Pause, Play } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Input } from '@core/components/ui/input'
import { DataTable, Th, Td, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { StatusDot, type StatusTone } from '@core/components/custom/status-dot'
import { useToast } from '@core/components/Toast'
import { useConfirm } from '@core/components/custom/confirm'
import LiveCharts, { type ColumnSeries } from '@tester/components/LiveCharts'
import LiveSidebar from '@tester/components/LiveSidebar'
import SipDrawer from '@tester/components/SipDrawer'
import { testerApi, openTesterStream, type RunRow, type RunLive, type RunEvent, type AggFrame, type RunStateFrame, type StepLogRow, type ScenarioDoc } from '@tester/api/tester'
import { fmtNum, fmtPct, fmtUnix, fmtDuration, fmtTime, STATE_LABEL, VERDICT_TONE, VERDICT_LABEL, STEP_LABEL } from '@tester/lib/fmt'
import { thresholdsFromScenario, funnelRows, estimateDuration, sdtSeconds, ratiosFrom, type ProfileLike, type Judge } from '@tester/lib/metrics'

interface Bucket { c: Record<string, number>; g: Record<string, number>; tm: Record<string, { count: number; sum: number; max: number | null }> }
export interface LiveSeries { t: number[]; buckets: Map<number, Bucket> }

function mergeAgg(store: LiveSeries, f: AggFrame) {
  const t = Math.floor(f.t)
  let b = store.buckets.get(t)
  if (!b) { b = { c: {}, g: {}, tm: {} }; store.buckets.set(t, b); store.t.push(t); store.t.sort((a, z) => a - z) }
  for (const [k, v] of Object.entries(f.counters || {})) b.c[k] = (b.c[k] || 0) + Number(v)
  for (const [k, v] of Object.entries(f.gauges || {})) b.g[k] = k === 'cpu_pct' ? Math.max(b.g[k] || 0, Number(v)) : (b.g[k] || 0) + Number(v)
  for (const [k, h] of Object.entries(f.timers || {})) {
    const cur = b.tm[k] || { count: 0, sum: 0, max: null }
    cur.count += h.count || 0; cur.sum += h.sum || 0
    cur.max = h.max == null ? cur.max : Math.max(cur.max ?? -Infinity, h.max)
    b.tm[k] = cur
  }
  if (store.t.length > 7200) { const drop = store.t.splice(0, store.t.length - 7200); drop.forEach(x => store.buckets.delete(x)) }
}

export function KpiTile({ label, value, sub, tone, expect, judge }: {
  label: string; value: string; sub?: string; tone?: 'danger' | 'warning'
  /** 기대치 모서리 — 시나리오 expect 에서 유도 */
  expect?: string; judge?: Judge
}) {
  const cls = judge === 'bad' ? 'border-destructive' : judge === 'ok' ? 'border-success' : 'border-border'
  return (
    <div className={`relative min-w-[120px] flex-1 rounded-md border bg-card px-3 py-2 ${cls}`}>
      {expect && (
        <span className={`absolute right-1.5 top-1 text-[10px] font-mono ${judge === 'bad' ? 'text-destructive' : judge === 'ok' ? 'text-success' : 'text-muted-foreground'}`}>{expect}</span>
      )}
      <div className="text-xs text-muted-foreground">{label}</div>
      <div className={`text-lg font-bold ${tone === 'danger' || judge === 'bad' ? 'text-destructive' : tone === 'warning' ? 'text-warning' : ''}`}>{value}</div>
      {sub && <div className="text-xs text-muted-foreground">{sub}</div>}
    </div>
  )
}

export default function RunLivePanel({ run, onEnded, scenario }: { run: RunRow; onEnded?: () => void; scenario?: ScenarioDoc | null }) {
  const { show } = useToast()
  const confirm = useConfirm()
  const [live, setLive] = useState<RunLive | null>(run.live ?? null)
  const [state, setState] = useState<RunStateFrame>({ run_id: run.id, state: run.live?.state ?? 'starting', verdict: run.verdict, rate_saps: run.live?.rate_saps, hold: run.live?.hold })
  const [events, setEvents] = useState<RunEvent[]>([])
  const [logs, setLogs] = useState<string[]>([])
  const [conn, setConn] = useState<'connecting' | 'open' | 'closed'>('connecting')
  const [tick, setTick] = useState(0)
  const [now, setNow] = useState(() => Date.now() / 1000)
  const [rateInput, setRateInput] = useState('')
  const [codeFilter, setCodeFilter] = useState<string | null>(null)
  const [sipCall, setSipCall] = useState<string | null>(null)
  const [sc, setSc] = useState<ScenarioDoc | null>(scenario ?? null)
  const storeRef = useRef<LiveSeries>({ t: [], buckets: new Map() })

  useEffect(() => { if (scenario) setSc(scenario); else testerApi.scenario(run.scenario_id).then(s => setSc(s.doc)).catch(() => {}) }, [run.scenario_id, scenario])

  // 지나간 시계열 + 진행 누계 선적재
  useEffect(() => {
    let alive = true
    ;(async () => {
      try {
        const [s, r, ev] = await Promise.all([testerApi.series(run.id), testerApi.run(run.id), testerApi.events(run.id, 300)])
        if (!alive) return
        const st = storeRef.current
        s.t.forEach((t, i) => {
          const b: Bucket = { c: {}, g: {}, tm: {} }
          for (const [k, arr] of Object.entries(s.counters)) b.c[k] = arr[i] ?? 0
          for (const [k, arr] of Object.entries(s.gauges)) { const v = arr[i]; if (v != null) b.g[k] = v }
          for (const [k, tm] of Object.entries(s.timers)) { const p = tm.p95[i]; if (p != null) b.tm[k] = { count: tm.count[i] ?? 0, sum: p * (tm.count[i] ?? 0), max: p } }
          if (!st.buckets.has(t)) { st.buckets.set(t, b); st.t.push(t) }
        })
        st.t.sort((a, z) => a - z)
        if (r.live) { setLive(r.live); setState(p => ({ ...p, state: r.live!.state, rate_saps: r.live!.rate_saps, verdict: r.live!.verdict, hold: r.live!.hold })) }
        setEvents(ev.events.slice().reverse())
        setTick(x => x + 1)
      } catch { /* 비어 있으면 스트림으로 채운다 */ }
    })()
    return () => { alive = false }
  }, [run.id])

  // 이 run 만의 SSE
  useEffect(() => {
    const close = openTesterStream(f => {
      if (f.stream === 'agg') { mergeAgg(storeRef.current, f.record as unknown as AggFrame); setTick(x => x + 1) }
      else if (f.stream === 'events') { setEvents(p => [f.record as unknown as RunEvent, ...p].slice(0, 300)) }
      else if (f.stream === 'runs') {
        const r = f.record as unknown as RunStateFrame
        if (r.kind === 'log' || r.kind === 'hello') { setLogs(p => [`${r.worker ?? ''}: ${r.msg ?? ''}`, ...p].slice(0, 40)); return }
        setState(p => ({ ...p, ...r }))
        if (r.state === 'stopped') { testerApi.run(run.id).then(x => { if (x.live) setLive(x.live) }).catch(() => {}); onEnded?.() }
      }
    }, s => setConn(s), run.id)
    return close
  }, [run.id, onEnded])

  // 진행 누계는 2 초마다(SSE 프레임에는 누계 p95 가 없다 — live() 가 병합 히스토그램 p95 를 준다) · 시계 1 초
  useEffect(() => {
    if (state.state === 'stopped') return
    const id = window.setInterval(() => { testerApi.run(run.id).then(r => { if (r.live) setLive(r.live) }).catch(() => {}); setNow(Date.now() / 1000) }, 2000)
    return () => window.clearInterval(id)
  }, [run.id, state.state])

  const data = useMemo<ColumnSeries>(() => {
    const st = storeRef.current
    const t = st.t
    const col = (fn: (b: Bucket) => number | null | undefined) => t.map(x => fn(st.buckets.get(x)!))
    const p95 = (b: Bucket, k: string) => { const h = b.tm[k]; return h && h.count ? (h.max ?? h.sum / h.count) : null }
    return {
      t,
      counters: { attempts: col(b => b.c.attempts ?? 0), failed: col(b => b.c.failed ?? 0), sessions: col(b => b.c.sessions ?? 0), rtp_rx: col(b => b.c.rtp_rx ?? 0), rtp_lost: col(b => b.c.rtp_lost ?? 0) },
      gauges: { concurrent_sessions: col(b => b.g.concurrent_sessions), cpu_pct: col(b => b.g.cpu_pct), target_cpu_pct: col(b => b.g.target_cpu_pct) },
      timers: { srd_ms: { p95: col(b => p95(b, 'srd_ms')) } },
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tick])

  const stepLog: StepLogRow[] = live?.step_log ?? []
  const bands = useMemo(() => {
    const steps = stepLog.filter(r => r.event === 'step' || r.event === 'start')
    return steps.map((r, i) => ({ from: r.t, to: i + 1 < steps.length ? steps[i + 1].t : (data.t.length ? data.t[data.t.length - 1] + 1 : r.t + 1), label: `${fmtNum(r.rate, 1)} SApS` }))
  }, [stepLog, data.t])

  // 최근 60 초 창 5xx · 현 단계 창 IHS
  const windows = useMemo(() => {
    const st = storeRef.current
    const last = st.t.length ? st.t[st.t.length - 1] : 0
    let att = 0, five = 0
    for (const t of st.t) { if (t < last - 60) continue; const b = st.buckets.get(t)!; att += b.c.attempts ?? 0; for (const [k, v] of Object.entries(b.c)) if (k.startsWith('codes.5')) five += v }
    const curStep = [...stepLog].reverse().find(r => r.event === 'step' || r.event === 'start')
    let a2 = 0, bad = 0
    for (const t of st.t) { if (curStep && t < curStep.t) continue; const b = st.buckets.get(t)!; a2 += b.c.attempts ?? 0; bad += (b.c.failed ?? 0) + (b.c.skipped ?? 0) }
    return { five: att >= 10 ? (100 * five) / att : null, ihs: a2 ? (100 * bad) / a2 : null }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tick, stepLog])

  const onStop = useCallback(async () => {
    if (!await confirm({ title: 'run 중단', body: `${run.id} 를 중단합니다. 진행 중 세션은 drain 뒤 정리되고 판정은 '중단' 이 됩니다.`, confirmLabel: '중단', tone: 'danger' })) return
    try { await testerApi.stopRun(run.id); show('중단 요청', 'ok') } catch (e) { show(String(e), 'err') }
  }, [run.id, confirm, show])
  const onRate = useCallback(async () => {
    const v = parseFloat(rateInput)
    if (!isFinite(v) || v < 0) { show('율은 0 이상 숫자', 'err'); return }
    try { await testerApi.rateRun(run.id, v); show(`율 ${v} SApS 적용`, 'ok'); setRateInput('') } catch (e) { show(String(e), 'err') }
  }, [run.id, rateInput, show])
  const onHold = useCallback(async () => {
    const next = !state.hold
    try { await testerApi.holdRun(run.id, next); setState(p => ({ ...p, hold: next })); show(next ? '단계 고정 — 프로파일 시계 정지' : '단계 재개', 'ok') } catch (e) { show(String(e), 'err') }
  }, [run.id, state.hold, show])

  const c = live?.counters ?? {}
  const tm = live?.timers ?? {}
  const g = live?.gauges ?? {}
  const ratios = ratiosFrom(c)
  const th = useMemo(() => thresholdsFromScenario(sc), [sc])
  const profile = (live?.profile_doc ?? null) as ProfileLike | null
  const attempts = c.attempts ?? 0
  const running = state.state !== 'stopped'
  const connTone: StatusTone = conn === 'open' ? 'success' : conn === 'connecting' ? 'info' : 'danger'
  const sdt = sdtSeconds(live?.plan?.steps, live?.plan?.phases)
  const est = estimateDuration(profile, live?.plan?.rate_total ?? live?.rate_saps ?? 0, live?.plan?.max_instances, sdt)
  const started = new Date(run.started_at).getTime() / 1000
  const elapsed = Math.max(0, now - started)
  const progress = est ? Math.min(100, (100 * (elapsed - (live?.held_s ?? 0))) / est) : null
  const eta = est ? new Date((started + est + (live?.held_s ?? 0)) * 1000).toISOString() : null
  const funnel = useMemo(() => funnelRows(sc, live?.plan, c, tm, g), [sc, live?.plan, c, tm, g])
  const codeCounts = useMemo(() => { const m = new Map<string, number>(); for (const e of events) { const k = e.code != null ? String(e.code) : (e.metric ?? e.step ?? '?'); m.set(k, (m.get(k) ?? 0) + 1) } return [...m.entries()].sort((a, b) => b[1] - a[1]) }, [events])
  const shownEvents = codeFilter ? events.filter(e => (e.code != null ? String(e.code) : (e.metric ?? e.step ?? '?')) === codeFilter) : events
  const judgeTimer = (k: keyof typeof th, h?: { p95?: number | null }): Judge => th[k] == null || h?.p95 == null ? 'none' : (h.p95 as number) <= (th[k] as number) ? 'ok' : 'bad'

  return (
    <div className="flex flex-col gap-3">
      <div className="flex flex-wrap items-center gap-2.5">
        <Badge variant={VERDICT_TONE[(state.verdict ?? run.verdict) as keyof typeof VERDICT_TONE] ?? 'neutralSoft'}>{VERDICT_LABEL[(state.verdict ?? run.verdict) as keyof typeof VERDICT_LABEL] ?? state.verdict}</Badge>
        <span className="text-sm font-medium">{STATE_LABEL[state.state ?? ''] ?? state.state}{state.hold ? ' · 단계 고정' : ''}</span>
        <span className="font-mono text-sm text-muted-foreground">{run.id}</span>
        <span className="text-sm font-semibold">{run.scenario_id}</span>
        {sc?.title && <span className="text-sm text-muted-foreground">{sc.title}</span>}
        {(live?.label ?? run.label) && <Badge variant="brandSoft">{live?.label ?? run.label}</Badge>}
        <span className="text-sm text-muted-foreground">{run.topology} · {run.profile ?? '단발'}{live?.target_build ? ` · 대상 ${live.target_build}` : ''}</span>
        <StatusDot tone={connTone} label={conn === 'open' ? '라이브' : conn === 'connecting' ? '연결 중' : '끊김'} />
        {running && (
          <div className="ml-auto flex flex-wrap items-center gap-2">
            <span className="text-sm text-muted-foreground">현재 율 <span className="font-mono text-foreground">{fmtNum(state.rate_saps ?? live?.rate_saps, 2)}</span> SApS</span>
            <Input value={rateInput} onChange={e => setRateInput(e.target.value)} placeholder="새 율" className="h-[26px] w-[90px] text-sm" inputMode="decimal" />
            <Button variant="outline" size="sm" onClick={onRate} disabled={!rateInput}><Gauge size={13} /> 율 적용</Button>
            {profile && <Button variant={state.hold ? 'default' : 'outline'} size="sm" onClick={onHold} title="현재 단계에 머무름 — 프로파일 진행을 멈추고 율은 유지">{state.hold ? <Play size={13} /> : <Pause size={13} />} {state.hold ? '재개' : '단계 고정'}</Button>}
            <Button variant="destructive" size="sm" onClick={onStop}><OctagonX size={13} /> 즉시 중단</Button>
          </div>
        )}
      </div>

      <div className="flex flex-wrap items-center gap-3 text-xs text-muted-foreground">
        <span>경과 <span className="font-mono text-foreground">{fmtDuration(run.started_at, running ? null : (live as unknown as { ended_at?: string })?.ended_at)}</span>{(live?.held_s ?? 0) > 0 ? ` (고정 ${fmtNum(live!.held_s, 0)} s)` : ''}</span>
        <div className="h-1.5 min-w-[160px] flex-1 overflow-hidden rounded-sm bg-neutral-soft"><div className="h-full bg-primary" style={{ width: `${progress ?? (running ? 0 : 100)}%` }} /></div>
        <span>예상 <span className="font-mono text-foreground">{est ? fmtDuration(new Date(0).toISOString(), new Date(est * 1000).toISOString()) : '—'}</span>
          {profile?.model === 'step' && est ? ` (${Math.floor(((profile.max ?? 0) - (profile.start ?? 0)) / (profile.step || 1)) + 1} 단계 × ${profile.hold_s} s)` : ''}
          {eta && running ? <> · 종료 예정 <span className="font-mono text-foreground">{fmtTime(eta)}</span></> : null}</span>
      </div>

      <div className="flex flex-col gap-3 lg:flex-row">
        <LiveSidebar live={live} window5xx={windows.five} windowIhs={windows.ihs} now={now} />
        <div className="flex min-w-0 flex-1 flex-col gap-3">
          <div className="flex flex-wrap gap-2">
            <KpiTile label="호 시도" value={fmtNum(attempts, 0)} sub={`세션 ${fmtNum(c.sessions ?? 0, 0)} · 완료 ${fmtNum(c.completed ?? 0, 0)}`} />
            <KpiTile label="SER" value={fmtPct(ratios.ser_pct)} expect={th.ser_pct != null ? `≥ ${th.ser_pct} %` : undefined}
                     judge={th.ser_pct != null && ratios.ser_pct != null ? (ratios.ser_pct >= th.ser_pct ? 'ok' : 'bad') : 'none'} tone={ratios.ser_pct != null && ratios.ser_pct < 99 ? 'warning' : undefined} />
            <KpiTile label="실패 / 건너뜀" value={`${fmtNum(c.failed ?? 0, 0)} / ${fmtNum(c.skipped ?? 0, 0)}`} tone={(c.failed ?? 0) > 0 ? 'danger' : undefined} />
            <KpiTile label="RRD p95" value={`${fmtNum(tm.rrd_ms?.p95, 0)} ms`} sub={`등록 ${fmtNum(c.registered_ok ?? 0, 0)}/${fmtNum((c.registered_ok ?? 0) + (c.registered_fail ?? 0), 0)}`}
                     expect={th.rrd_ms != null ? `≤ ${th.rrd_ms}` : undefined} judge={judgeTimer('rrd_ms', tm.rrd_ms)} />
            <KpiTile label="SRD p95" value={`${fmtNum(tm.srd_ms?.p95, 0)} ms`} sub={`SDD p95 ${fmtNum(tm.sdd_ms?.p95, 0)} ms`}
                     expect={th.srd_ms != null ? `≤ ${th.srd_ms}` : undefined} judge={judgeTimer('srd_ms', tm.srd_ms)} />
            <KpiTile label="RTP 손실" value={fmtPct(ratios.rtp_loss_pct)} sub={`지터 p95 ${fmtNum(tm.jitter_ms?.p95, 1)} ms`}
                     expect={th.rtp_loss_pct != null ? `≤ ${th.rtp_loss_pct} %` : undefined}
                     judge={th.rtp_loss_pct != null && ratios.rtp_loss_pct != null ? (ratios.rtp_loss_pct <= th.rtp_loss_pct ? 'ok' : 'bad') : 'none'} />
            <KpiTile label="동시 세션" value={fmtNum(g.concurrent_sessions ?? 0, 0)} sub={`워커 CPU ${fmtNum(g.cpu_pct, 0)} %`} />
            {live?.doc_rate != null && <KpiTile label="DOC" value={`${fmtNum(live.doc_rate, 1)} SApS`} sub="IHS 임계 직전 단계" />}
          </div>

          <LiveCharts data={data} thresholds={th} bands={bands} stopOn={profile?.stop_on} />

          {(live?.notes?.length ?? 0) > 0 && (
            <ul className="list-disc pl-5 text-xs text-muted-foreground">{live!.notes.map((n, i) => <li key={i}>{n}</li>)}</ul>
          )}

          <div className="grid gap-3 xl:grid-cols-2">
            <section className="flex min-w-0 flex-col gap-1.5">
              <h3 className="text-sm font-semibold text-muted-foreground">절차 진행 <span className="font-normal">— 인스턴스가 어느 단계에 있는가 · 기대치는 누계 판정</span></h3>
              {funnel.length === 0 ? <EmptyState title="시나리오 flow 없음" /> : (
                <DataTable>
                  <thead><tr><Th width={32}>#</Th><Th>단계</Th><Th align="right">진입</Th><Th align="right">완료</Th><Th align="right">진행</Th><Th align="right">실패</Th><Th>기대치</Th></tr></thead>
                  <tbody>
                    {funnel.map(r => (
                      <tr key={r.idx} className={r.phase !== 'body' ? 'text-muted-foreground' : ''}>
                        <Td mono>{r.idx + 1}</Td>
                        <Td><span className="font-medium">{STEP_LABEL[r.step] ?? r.step}</span> <span className="text-xs text-muted-foreground">{r.actors}</span>{r.phase !== 'body' && <Badge variant="neutralSoft" className="ml-1">{r.phase}</Badge>}</Td>
                        <Td align="right" mono>{fmtNum(r.entered, 0)}</Td><Td align="right" mono>{fmtNum(r.done, 0)}</Td>
                        <Td align="right" mono>{fmtNum(r.active, 0)}</Td>
                        <Td align="right" mono className={(r.failed ?? 0) > 0 ? 'text-destructive' : ''}>{fmtNum(r.failed, 0)}</Td>
                        <Td>
                          <div className="flex flex-wrap gap-1">
                            {r.expects.map(x => <Badge key={x.metric} variant={x.judge === 'ok' ? 'successSoft' : x.judge === 'bad' ? 'dangerSoft' : 'neutralSoft'} title={x.text}>{x.metric}</Badge>)}
                          </div>
                        </Td>
                      </tr>
                    ))}
                  </tbody>
                </DataTable>
              )}
            </section>
            <section className="flex min-w-0 flex-col gap-1.5">
              <h3 className="flex flex-wrap items-center gap-1.5 text-sm font-semibold text-muted-foreground">실패 이벤트 ({events.length})
                {codeCounts.map(([k, n]) => (
                  <button key={k} onClick={() => setCodeFilter(codeFilter === k ? null : k)}
                          className={`h-5 rounded-sm border px-1.5 font-mono text-[11px] ${codeFilter === k ? 'border-primary bg-primary text-primary-foreground' : 'border-border hover:bg-accent'}`}>{k} × {n}</button>
                ))}
              </h3>
              {shownEvents.length === 0 ? <EmptyState title="실패 이벤트 없음" /> : (
                <DataTable>
                  <thead><tr><Th width={80}>시각</Th><Th>역할 / 신원</Th><Th>단계</Th><Th>코드</Th><Th>상세</Th></tr></thead>
                  <tbody>
                    {shownEvents.slice(0, 100).map((e, i) => (
                      <tr key={`${e.t}-${i}`} className={e.call_id ? 'cursor-pointer hover:bg-accent' : ''} onClick={() => e.call_id && setSipCall(e.call_id)} title={e.call_id ? `SIP 사다리 — ${e.call_id}` : undefined}>
                        <Td mono>{fmtUnix(e.t)}</Td>
                        <Td mono className="text-xs">{orDash(e.role)}{e.identity ? ` ${e.identity}` : ''}</Td>
                        <Td>{orDash(e.step)}</Td>
                        <Td mono>{e.code != null ? e.code : e.metric ? `${e.metric}=${fmtNum(e.observed)}` : '—'}</Td>
                        <Td className="break-all text-xs">{orDash(e.detail)}</Td>
                      </tr>
                    ))}
                  </tbody>
                </DataTable>
              )}
            </section>
          </div>
          {logs.length > 0 && (
            <details className="text-xs text-muted-foreground"><summary className="cursor-pointer">워커 로그 ({logs.length})</summary>
              <pre className="mt-1 whitespace-pre-wrap font-mono">{logs.join('\n')}</pre></details>
          )}
        </div>
      </div>
      {sipCall && <SipDrawer runId={run.id} callId={sipCall} onClose={() => setSipCall(null)} />}
    </div>
  )
}
