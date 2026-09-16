// 실행(라이브) — SApS·동시 세션·SER·SRD p95·RTP 손실·대상 CPU 를 한 시간축에(SSE 1초), 단계 진행 띠,
// 실패 이벤트 표, 즉시 중단·율 조정 (test_instrument.md §7 실행 화면).
//
// 데이터 흐름: /runs/<id>/stream 의 agg 프레임(워커별 1초 버킷)을 초 단위로 합쳐 시계열을 쌓고, runs 프레임으로
// 상태·율·verdict 를, events 프레임으로 실패 표를 갱신한다. 열 때 /runs/<id>/series 로 그동안의 시계열을 먼저
// 받아 이어 붙인다(중간에 화면을 열어도 처음부터 보인다).
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { OctagonX, Gauge } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Input } from '@core/components/ui/input'
import { DataTable, Th, Td, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { StatusDot, type StatusTone } from '@core/components/custom/status-dot'
import { useToast } from '@core/components/Toast'
import { useConfirm } from '@core/components/custom/confirm'
import LineChart, { type Series } from '@tester/components/LineChart'
import { testerApi, openTesterStream, type RunRow, type RunLive, type RunEvent, type AggFrame, type RunStateFrame, type StepLogRow } from '@tester/api/tester'
import { fmtNum, fmtPct, fmtUnix, fmtDuration, STATE_LABEL, VERDICT_TONE, VERDICT_LABEL } from '@tester/lib/fmt'

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

export function KpiTile({ label, value, sub, tone }: { label: string; value: string; sub?: string; tone?: 'danger' | 'warning' }) {
  return (
    <div className="min-w-[120px] flex-1 rounded-md border border-border bg-card px-3 py-2">
      <div className="text-xs text-muted-foreground">{label}</div>
      <div className={`text-lg font-bold ${tone === 'danger' ? 'text-destructive' : tone === 'warning' ? 'text-warning' : ''}`}>{value}</div>
      {sub && <div className="text-xs text-muted-foreground">{sub}</div>}
    </div>
  )
}

export default function RunLivePanel({ run, onEnded }: { run: RunRow; onEnded?: () => void }) {
  const { show } = useToast()
  const confirm = useConfirm()
  const [live, setLive] = useState<RunLive | null>(run.live ?? null)
  const [state, setState] = useState<RunStateFrame>({ run_id: run.id, state: run.live?.state ?? 'starting', verdict: run.verdict, rate_saps: run.live?.rate_saps })
  const [events, setEvents] = useState<RunEvent[]>([])
  const [logs, setLogs] = useState<string[]>([])
  const [conn, setConn] = useState<'connecting' | 'open' | 'closed'>('connecting')
  const [tick, setTick] = useState(0)
  const [rateInput, setRateInput] = useState('')
  const storeRef = useRef<LiveSeries>({ t: [], buckets: new Map() })
  const seededRef = useRef(false)

  // 지나간 시계열 + 진행 누계 선적재
  useEffect(() => {
    let alive = true
    ;(async () => {
      try {
        const [s, r, ev] = await Promise.all([testerApi.series(run.id), testerApi.run(run.id), testerApi.events(run.id, 200)])
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
        seededRef.current = true
        if (r.live) { setLive(r.live); setState(p => ({ ...p, state: r.live!.state, rate_saps: r.live!.rate_saps, verdict: r.live!.verdict })) }
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
        if (r.kind === 'log' || r.kind === 'hello') { setLogs(p => [`${r.worker ?? ''}: ${r.msg ?? ''}`, ...p].slice(0, 30)); return }
        setState(p => ({ ...p, ...r }))
        if (r.state === 'stopped') { testerApi.run(run.id).then(x => { if (x.live) setLive(x.live) }).catch(() => {}); onEnded?.() }
      }
    }, s => setConn(s), run.id)
    return close
  }, [run.id, onEnded])

  // 진행 누계는 2 초마다 한 번 (SSE 프레임에는 누계 p95 가 없다 — live() 가 병합 히스토그램 p95 를 준다)
  useEffect(() => {
    if (state.state === 'stopped') return
    const id = window.setInterval(() => testerApi.run(run.id).then(r => { if (r.live) setLive(r.live) }).catch(() => {}), 2000)
    return () => window.clearInterval(id)
  }, [run.id, state.state])

  const series = useMemo(() => {
    const st = storeRef.current
    const t = st.t
    const get = (fn: (b: Bucket) => number | null) => t.map(x => fn(st.buckets.get(x)!))
    const ratio = (b: Bucket) => { const a = b.c.attempts || 0; return a ? (100 * (b.c.sessions || 0)) / a : null }
    const p95 = (b: Bucket, k: string) => { const h = b.tm[k]; return h && h.count ? (h.max ?? h.sum / h.count) : null }
    const loss = (b: Bucket) => { const tot = (b.c.rtp_rx || 0) + (b.c.rtp_lost || 0); return tot ? (100 * (b.c.rtp_lost || 0)) / tot : null }
    const out: Series[] = [
      { key: 'saps', label: 'SApS(시도/초)', values: get(b => b.c.attempts ?? 0), color: 'var(--chart-1)' },
      { key: 'conc', label: '동시 세션', values: get(b => b.g.concurrent_sessions ?? null), color: 'var(--chart-2)' },
      { key: 'ser', label: 'SER', values: get(ratio), color: 'var(--chart-3)', unit: '%', max: 100 },
      { key: 'srd', label: 'SRD p95', values: get(b => p95(b, 'srd_ms')), color: 'var(--chart-4)', unit: 'ms' },
      { key: 'loss', label: 'RTP 손실', values: get(loss), color: 'var(--chart-5)', unit: '%', max: 5 },
      { key: 'cpu', label: '워커 CPU', values: get(b => b.g.cpu_pct ?? null), color: 'var(--chart-6)', unit: '%', max: 100 },
    ]
    return { t, out }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tick])

  const bands = useMemo(() => {
    const sl: StepLogRow[] = live?.step_log ?? []
    const steps = sl.filter(r => r.event === 'step' || r.event === 'start')
    return steps.map((r, i) => ({ from: r.t, to: i + 1 < steps.length ? steps[i + 1].t : (series.t.length ? series.t[series.t.length - 1] + 1 : r.t + 1), label: `${fmtNum(r.rate, 1)} SApS` }))
  }, [live?.step_log, series.t])

  const onStop = useCallback(async () => {
    if (!await confirm({ title: 'run 중단', body: `${run.id} 를 중단합니다. 진행 중 세션은 drain 뒤 정리되고 판정은 '중단' 이 됩니다.`, confirmLabel: '중단', tone: 'danger' })) return
    try { await testerApi.stopRun(run.id); show('중단 요청', 'ok') } catch (e) { show(String(e), 'err') }
  }, [run.id, confirm, show])

  const onRate = useCallback(async () => {
    const v = parseFloat(rateInput)
    if (!isFinite(v) || v < 0) { show('율은 0 이상 숫자', 'err'); return }
    try { await testerApi.rateRun(run.id, v); show(`율 ${v} SApS 적용`, 'ok'); setRateInput('') } catch (e) { show(String(e), 'err') }
  }, [run.id, rateInput, show])

  const c = live?.counters ?? {}
  const tm = live?.timers ?? {}
  const attempts = c.attempts ?? 0
  const ser = attempts ? (100 * (c.sessions ?? 0)) / attempts : null
  const lossTot = (c.rtp_rx ?? 0) + (c.rtp_lost ?? 0)
  const loss = lossTot ? (100 * (c.rtp_lost ?? 0)) / lossTot : null
  const running = state.state !== 'stopped'
  const connTone: StatusTone = conn === 'open' ? 'success' : conn === 'connecting' ? 'info' : 'danger'

  return (
    <div className="flex flex-col gap-3">
      <div className="flex flex-wrap items-center gap-2.5">
        <Badge variant={VERDICT_TONE[(state.verdict ?? run.verdict) as keyof typeof VERDICT_TONE] ?? 'neutralSoft'}>{VERDICT_LABEL[(state.verdict ?? run.verdict) as keyof typeof VERDICT_LABEL] ?? state.verdict}</Badge>
        <span className="text-sm font-medium">{STATE_LABEL[state.state ?? ''] ?? state.state}</span>
        <span className="font-mono text-sm text-muted-foreground">{run.id}</span>
        <span className="text-sm text-muted-foreground">{run.scenario_id} · {run.topology} · {run.profile ?? '단발'} · {fmtDuration(run.started_at, running ? null : (live as unknown as { ended_at?: string })?.ended_at)}</span>
        <StatusDot tone={connTone} label={conn === 'open' ? '라이브' : conn === 'connecting' ? '연결 중' : '끊김'} />
        {running && (
          <div className="ml-auto flex items-center gap-2">
            <span className="text-sm text-muted-foreground">현재 율 <span className="font-mono text-foreground">{fmtNum(state.rate_saps ?? live?.rate_saps, 2)}</span> SApS</span>
            <Input value={rateInput} onChange={e => setRateInput(e.target.value)} placeholder="새 율" className="h-[26px] w-[90px] text-sm" inputMode="decimal" />
            <Button variant="outline" size="sm" onClick={onRate} disabled={!rateInput}><Gauge size={13} /> 율 적용</Button>
            <Button variant="destructive" size="sm" onClick={onStop}><OctagonX size={13} /> 즉시 중단</Button>
          </div>
        )}
      </div>

      <div className="flex flex-wrap gap-2">
        <KpiTile label="호 시도" value={fmtNum(attempts, 0)} sub={`세션 ${fmtNum(c.sessions ?? 0, 0)} · 완료 ${fmtNum(c.completed ?? 0, 0)}`} />
        <KpiTile label="SER" value={fmtPct(ser)} tone={ser != null && ser < 99 ? 'warning' : undefined} />
        <KpiTile label="실패 / 건너뜀" value={`${fmtNum(c.failed ?? 0, 0)} / ${fmtNum(c.skipped ?? 0, 0)}`} tone={(c.failed ?? 0) > 0 ? 'danger' : undefined} />
        <KpiTile label="RRD p95" value={`${fmtNum(tm.rrd_ms?.p95, 0)} ms`} sub={`등록 ${fmtNum(c.registered_ok ?? 0, 0)}/${fmtNum((c.registered_ok ?? 0) + (c.registered_fail ?? 0), 0)}`} />
        <KpiTile label="SRD p95" value={`${fmtNum(tm.srd_ms?.p95, 0)} ms`} sub={`SDD p95 ${fmtNum(tm.sdd_ms?.p95, 0)} ms`} />
        <KpiTile label="RTP 손실" value={fmtPct(loss)} sub={`지터 p95 ${fmtNum(tm.jitter_ms?.p95, 1)} ms`} tone={loss != null && loss > 0.5 ? 'warning' : undefined} />
        <KpiTile label="동시 세션" value={fmtNum(live?.gauges?.concurrent_sessions ?? 0, 0)} sub={`워커 CPU ${fmtNum(live?.gauges?.cpu_pct, 0)} %`} />
        {live?.doc_rate != null && <KpiTile label="DOC" value={`${fmtNum(live.doc_rate, 1)} SApS`} sub="IHS 임계 직전 단계" />}
      </div>

      <LineChart t={series.t} series={series.out} bands={bands} height={200} />

      {(live?.notes?.length ?? 0) > 0 && (
        <ul className="list-disc pl-5 text-sm text-muted-foreground">{live!.notes.map((n, i) => <li key={i}>{n}</li>)}</ul>
      )}

      <section className="flex flex-col gap-1.5">
        <h3 className="text-sm font-semibold text-muted-foreground">실패 이벤트 (최근 {events.length})</h3>
        {events.length === 0 ? <EmptyState title="실패 이벤트 없음" /> : (
          <DataTable>
            <thead><tr><Th width={90}>시각</Th><Th>워커</Th><Th>역할 / 신원</Th><Th>단계</Th><Th>코드</Th><Th>지표</Th><Th>상세</Th></tr></thead>
            <tbody>
              {events.map((e, i) => (
                <tr key={`${e.t}-${i}`}>
                  <Td mono>{fmtUnix(e.t)}</Td>
                  <Td>{e.worker}</Td>
                  <Td mono>{orDash(e.role)}{e.identity ? ` ${e.identity}` : ''}</Td>
                  <Td>{orDash(e.step)}</Td>
                  <Td mono>{orDash(e.code)}</Td>
                  <Td mono>{e.metric ? `${e.metric}=${fmtNum(e.observed)}` : orDash(null)}</Td>
                  <Td className="break-all">{orDash(e.detail)}</Td>
                </tr>
              ))}
            </tbody>
          </DataTable>
        )}
      </section>
      {logs.length > 0 && (
        <details className="text-xs text-muted-foreground"><summary className="cursor-pointer">워커 로그 ({logs.length})</summary>
          <pre className="mt-1 whitespace-pre-wrap font-mono">{logs.join('\n')}</pre></details>
      )}
    </div>
  )
}
