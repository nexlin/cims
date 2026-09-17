// 시험 > 비교 — 두 모드. **run 비교**: run 카드 열(색 견본·첫 카드 = 기준·순서 변경·제거·[+ run 추가]는 같은 시나리오·프로파일 후보 먼저) +
// 지표 매트릭스(값·Δ·중심 0 Δ 막대·회귀 붉게/개선 초록·허용 오차) + 시간축 겹침(행 클릭 → run 시작 t+0 정렬, 기준 굵게, 기대치 점선, 기준 run
// 단계 띠) + 응답 코드 분해 + 기대치 판정 diff(PASS→FAIL 회귀 / FAIL→PASS 복구). **추세**: 시나리오×프로파일×토폴로지의 모든 run 을 시간순
// 점으로(색 = 대상 빌드, ● PASS / ◆ FAIL, 기대치 점선, 기준 = 첫 PASS ± 허용 회색 띠, 벗어나면 '회귀') 4 장 + run 목록.
// 판정은 컨트롤러 GET /runs/compare(비율 0.5 pt / 그 외 5 %) — CLI 와 같은 결과. 내보내기 = compare?format=md|csv (test_instrument.md §7).
import { useCallback, useEffect, useMemo, useState } from 'react'
import { useSearchParams } from 'react-router-dom'
import { GitCompareArrows, X, Plus, ArrowUp, ArrowDown, ChevronLeft, ChevronRight, Download } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Checkbox } from '@core/components/ui/checkbox'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { DataTable, Th, Td, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { useToast } from '@core/components/Toast'
import { testerApi, type RunRow, type CompareResult, type RunSeries, type ScenarioDoc, type ExpectResult } from '@tester/api/tester'
import { fmtTime, fmtNum, SUMMARY_ROWS, VERDICT_TONE, VERDICT_LABEL, STEP_LABEL } from '@tester/lib/fmt'
import { thresholdsFromScenario, expectText } from '@tester/lib/metrics'
import { parseCodes } from '@tester/components/RunReport'

const LABEL: Record<string, string> = Object.fromEntries(SUMMARY_ROWS.map(([k, l]) => [k, l]))
Object.assign(LABEL, { rrd_ms_p95: 'RRD p95 ms', srd_ms_p95: 'SRD p95 ms', sdd_ms_p95: 'SDD p95 ms', jitter_ms_p95: '지터 p95 ms' })
const COLORS = ['var(--chart-1)', 'var(--chart-2)', 'var(--chart-3)', 'var(--chart-4)', 'var(--chart-5)', 'var(--chart-6)', 'var(--chart-7)', 'var(--chart-8)']
const NONE = '__none__'
const ALL = '__all__'

/** 매트릭스 지표 → 시계열 열 */
function seriesOf(s: RunSeries, metric: string): (number | null)[] {
  const t = s.t
  if (metric === 'ser_pct') return t.map((_, i) => { const a = s.counters.attempts?.[i] ?? 0; return a ? (100 * (s.counters.sessions?.[i] ?? 0)) / a : null })
  if (metric === 'scr_pct') return t.map((_, i) => { const a = s.counters.sessions?.[i] ?? 0; return a ? (100 * (s.counters.completed?.[i] ?? 0)) / a : null })
  if (metric === 'rtp_loss_pct') return t.map((_, i) => { const tot = (s.counters.rtp_rx?.[i] ?? 0) + (s.counters.rtp_lost?.[i] ?? 0); return tot ? (100 * (s.counters.rtp_lost?.[i] ?? 0)) / tot : null })
  if (metric === 'doc_saps') return (s.counters.attempts ?? []).map(v => v ?? null)
  const m = metric.match(/^(\w+_ms)_p95$/)
  if (m) return (s.timers[m[1]]?.p95 ?? []).map(v => v ?? null)
  return t.map(() => null)
}

function OverlayChart({ runs, series, metric, threshold, bands, colors }: {
  runs: string[]; series: Record<string, RunSeries | undefined>; metric: string; threshold?: number; bands: { from: number; to: number; label: string }[]; colors: string[]
}) {
  const W = 700, H = 200, pl = 8, pr = 8, pt = 8, pb = 18
  const lines = runs.map((id, i) => { const s = series[id]; if (!s || s.t.length < 2) return null; const t0 = s.t[0]; return { id, i, x: s.t.map(t => t - t0), y: seriesOf(s, metric) } }).filter((x): x is NonNullable<typeof x> => !!x)
  const span = Math.max(1, ...lines.map(l => l.x[l.x.length - 1]))
  const max = Math.max(1e-9, (threshold ?? 0) * 1.15, ...lines.flatMap(l => l.y.filter((v): v is number => v != null)))
  const X = (v: number) => pl + (v / span) * (W - pl - pr)
  const Y = (v: number) => pt + (H - pt - pb) * (1 - Math.min(v, max) / max)
  if (!lines.length) return <div className="flex h-[200px] items-center justify-center text-xs text-muted-foreground">시계열 없음(단발 run 이 짧거나 metrics 삭제됨)</div>
  return (
    <svg viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" className="block w-full rounded-md border border-border bg-card" style={{ height: H }}>
      {bands.map((b, i) => <rect key={i} x={X(Math.max(0, b.from))} y={pt} width={Math.max(0, X(Math.min(span, b.to)) - X(Math.max(0, b.from)))} height={H - pt - pb} fill="var(--chart-muted)" opacity={i % 2 ? 0.16 : 0.07} />)}
      <line x1={pl} y1={H - pb} x2={W - pr} y2={H - pb} stroke="var(--border)" />
      {threshold != null && threshold <= max && <line x1={pl} x2={W - pr} y1={Y(threshold)} y2={Y(threshold)} stroke="var(--destructive)" strokeDasharray="4 3" vectorEffect="non-scaling-stroke" />}
      {lines.map(l => {
        const pts = l.x.map((x, k) => l.y[k] == null ? null : `${X(x).toFixed(1)},${Y(l.y[k] as number).toFixed(1)}`).filter(Boolean).join(' ')
        return <polyline key={l.id} points={pts} fill="none" stroke={colors[l.i]} strokeWidth={l.i === 0 ? 2.4 : 1.4} vectorEffect="non-scaling-stroke" strokeLinejoin="round" opacity={l.i === 0 ? 1 : 0.85} />
      })}
      {[0, 0.25, 0.5, 0.75, 1].map(f => <text key={f} x={X(span * f)} y={H - 5} fontSize={10} fill="var(--muted-foreground)" textAnchor={f === 0 ? 'start' : f === 1 ? 'end' : 'middle'}>t+{Math.round(span * f)}s</text>)}
      <text x={pl + 2} y={pt + 9} fontSize={10} fill="var(--muted-foreground)">max {max >= 10 ? max.toFixed(0) : max.toFixed(2)}</text>
    </svg>
  )
}

function TrendChart({ rows, metric, label, threshold, dir }: { rows: RunRow[]; metric: string; label: string; threshold?: number; dir: 'up' | 'down' }) {
  const W = 420, H = 150, pl = 8, pr = 8, pt = 8, pb = 18
  const pts = rows.map(r => ({ r, t: new Date(r.started_at).getTime() / 1000, v: (r.summary?.[metric] as number | null | undefined) ?? null })).filter(p => p.v != null && isFinite(p.t)).sort((a, b) => a.t - b.t)
  const builds = [...new Set(rows.map(r => r.target_build ?? '?'))]
  if (pts.length === 0) return <div className="rounded-md border border-border bg-card p-2 text-xs"><div className="font-semibold">{label}</div><div className="flex h-[150px] items-center justify-center text-muted-foreground">값 없음</div></div>
  const t0 = pts[0].t, t1 = pts[pts.length - 1].t, span = Math.max(1, t1 - t0)
  const base = pts.find(p => p.r.verdict === 'pass')
  const tol = base ? (metric.endsWith('_pct') ? 0.5 : Math.abs(base.v!) * 0.05) : 0
  const max = Math.max(1e-9, (threshold ?? 0) * 1.15, ...pts.map(p => p.v as number), base ? base.v! + tol : 0)
  const X = (t: number) => pl + ((t - t0) / span) * (W - pl - pr)
  const Y = (v: number) => pt + (H - pt - pb) * (1 - Math.min(v, max) / max)
  const isReg = (v: number) => base ? (dir === 'up' ? v < base.v! - tol : v > base.v! + tol) : false
  return (
    <div className="rounded-md border border-border bg-card p-2 text-xs">
      <div className="flex items-center gap-2"><span className="font-semibold">{label}</span>{threshold != null && <span className="text-destructive">기대 {threshold}</span>}<span className="ml-auto text-muted-foreground">{pts.length} run</span></div>
      <svg viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" className="block w-full" style={{ height: H }}>
        {base && <rect x={pl} y={Y(base.v! + tol)} width={W - pl - pr} height={Math.max(1, Y(base.v! - tol) - Y(base.v! + tol))} fill="var(--foreground)" opacity={0.06} />}
        {base && <line x1={pl} x2={W - pr} y1={Y(base.v!)} y2={Y(base.v!)} stroke="var(--muted-foreground)" strokeDasharray="2 3" />}
        {threshold != null && threshold <= max && <line x1={pl} x2={W - pr} y1={Y(threshold)} y2={Y(threshold)} stroke="var(--destructive)" strokeDasharray="4 3" />}
        <line x1={pl} y1={H - pb} x2={W - pr} y2={H - pb} stroke="var(--border)" />
        <polyline points={pts.map(p => `${X(p.t).toFixed(1)},${Y(p.v as number).toFixed(1)}`).join(' ')} fill="none" stroke="var(--border)" strokeWidth={1} />
        {pts.map((p, i) => {
          const c = COLORS[builds.indexOf(p.r.target_build ?? '?') % COLORS.length]
          const x = X(p.t), y = Y(p.v as number), reg = isReg(p.v as number)
          return (
            <g key={i}>
              <title>{`${p.r.id} · ${p.r.target_build ?? ''} · ${VERDICT_LABEL[p.r.verdict]} · ${fmtNum(p.v)}${reg ? ' · 회귀' : ''}`}</title>
              {p.r.verdict === 'pass' ? <circle cx={x} cy={y} r={4} fill={c} /> : <rect x={x - 4} y={y - 4} width={8} height={8} fill={c} transform={`rotate(45 ${x} ${y})`} />}
              {reg && <text x={x} y={y - 7} fontSize={9} fill="var(--destructive)" textAnchor="middle">회귀</text>}
            </g>
          )
        })}
        <text x={pl} y={H - 5} fontSize={10} fill="var(--muted-foreground)">{fmtTime(pts[0].r.started_at, true).slice(0, 10)}</text>
        <text x={W - pr} y={H - 5} fontSize={10} fill="var(--muted-foreground)" textAnchor="end">{fmtTime(pts[pts.length - 1].r.started_at, true).slice(0, 10)}</text>
      </svg>
      <div className="flex flex-wrap gap-2 text-[10px] text-muted-foreground">{builds.map((b, i) => <span key={b} className="inline-flex items-center gap-1"><span className="inline-block h-2 w-2 rounded-sm" style={{ background: COLORS[i % COLORS.length] }} />{b}</span>)}</div>
    </div>
  )
}

export default function TesterComparePage() {
  const [params, setParams] = useSearchParams()
  const { show } = useToast()
  const ids = useMemo(() => (params.get('ids') ?? '').split(',').filter(Boolean), [params])
  const mode = params.get('mode') === 'trend' ? 'trend' : 'runs'
  const [runs, setRuns] = useState<RunRow[]>([])
  const [result, setResult] = useState<CompareResult | null>(null)
  const [err, setErr] = useState<string | null>(null)
  const [picker, setPicker] = useState(false)
  const [pickAll, setPickAll] = useState(false)
  const [metric, setMetric] = useState<string>('srd_ms_p95')
  const [series, setSeries] = useState<Record<string, RunSeries | undefined>>({})
  const [scenario, setScenario] = useState<ScenarioDoc | null>(null)
  const [drag, setDrag] = useState<string | null>(null)
  // 추세
  const [tScen, setTScen] = useState(ALL)
  const [tProf, setTProf] = useState(ALL)
  const [tTopo, setTTopo] = useState(ALL)
  const [tPick, setTPick] = useState<string[]>([])

  useEffect(() => { testerApi.runs(500).then(r => setRuns(r.runs.filter(x => x.verdict !== 'running'))).catch(() => {}) }, [])

  const load = useCallback(async () => {
    if (ids.length < 2) { setResult(null); return }
    try { setResult(await testerApi.compare(ids)); setErr(null) } catch (e) { setErr(String(e)); setResult(null) }
  }, [ids])
  useEffect(() => { load() }, [load])
  useEffect(() => {
    ids.forEach(id => { if (!series[id]) testerApi.series(id).then(s => setSeries(p => ({ ...p, [id]: s }))).catch(() => {}) })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ids])
  useEffect(() => {
    const sid = result?.runs[0]?.scenario_id
    if (sid) testerApi.scenario(sid).then(s => setScenario(s.doc)).catch(() => setScenario(null))
  }, [result?.runs])

  const setIds = (next: string[]) => setParams(p => { const q = new URLSearchParams(p); if (next.length) q.set('ids', next.join(',')); else q.delete('ids'); return q })
  const setMode = (m: 'runs' | 'trend') => setParams(p => { const q = new URLSearchParams(p); if (m === 'trend') q.set('mode', 'trend'); else q.delete('mode'); return q })
  const add = (id: string) => { if (id && !ids.includes(id)) setIds([...ids, id]); setPicker(false) }
  const remove = (id: string) => setIds(ids.filter(x => x !== id))
  const makeBase = (id: string) => setIds([id, ...ids.filter(x => x !== id)])
  const move = (id: string, dir: -1 | 1) => { const i = ids.indexOf(id); const j = i + dir; if (i < 0 || j < 0 || j >= ids.length) return; const n = ids.slice(); [n[i], n[j]] = [n[j], n[i]]; setIds(n) }
  const onDrop = (target: string) => { if (!drag || drag === target) return; const n = ids.filter(x => x !== drag); n.splice(n.indexOf(target), 0, drag); setIds(n); setDrag(null) }

  const base = result?.runs[0]
  const baseRow = runs.find(r => r.id === base?.id)
  const candidates = useMemo(() => {
    const rest = runs.filter(r => !ids.includes(r.id))
    if (!baseRow) return rest.map(r => ({ r, dim: false }))
    const same = (r: RunRow) => r.scenario_id === baseRow.scenario_id && (r.profile ?? null) === (baseRow.profile ?? null)
    return [...rest.filter(same).map(r => ({ r, dim: false })), ...(pickAll ? rest.filter(r => !same(r)).map(r => ({ r, dim: true })) : [])]
  }, [runs, ids, baseRow, pickAll])
  const th = useMemo(() => thresholdsFromScenario(scenario), [scenario])
  const thOf = (m: string): number | undefined => {
    if (m === 'ser_pct') return th.ser_pct
    if (m === 'rtp_loss_pct') return th.rtp_loss_pct
    const x = m.match(/^(\w+_ms)_p95$/); return x ? (th as Record<string, number | undefined>)[x[1]] : undefined
  }
  const baseBands = useMemo(() => {
    const sl = (base as { step_log?: { t: number; rate: number; event?: string }[] } | undefined)?.step_log ?? []
    const steps = sl.filter(r => r.event === 'step' || r.event === 'start')
    const s = base ? series[base.id] : undefined
    const t0 = s?.t[0] ?? (steps[0]?.t ?? 0)
    return steps.map((r, i) => ({ from: r.t - t0, to: (i + 1 < steps.length ? steps[i + 1].t : (s?.t[s.t.length - 1] ?? r.t) + 1) - t0, label: `${fmtNum(r.rate, 1)} SApS` }))
  }, [base, series])
  const profilesDiffer = !!result && new Set(result.runs.filter(r => !r.missing).map(r => r.profile ?? '')).size > 1

  // 기대치 diff
  const expectDiff = useMemo(() => {
    if (!result) return []
    const keys = new Map<string, { step: number; kind: string; metric: string; expect: unknown }>()
    for (const r of result.runs) for (const e of r.expect_results ?? []) { const k = `${e.step}|${e.metric}`; if (!keys.has(k)) keys.set(k, { step: e.step, kind: e.kind, metric: e.metric, expect: e.expect }) }
    return [...keys.entries()].sort((a, b) => a[1].step - b[1].step).map(([k, meta]) => {
      const cells = result.runs.map(r => (r.expect_results ?? []).find((e: ExpectResult) => `${e.step}|${e.metric}` === k) ?? null)
      const b = cells[0]?.ok, last = cells[cells.length - 1]?.ok
      const change = b === true && last === false ? 'reg' : b === false && last === true ? 'fix' : null
      return { key: k, ...meta, cells, change }
    })
  }, [result])

  const exportText = async (fmt: 'md' | 'csv') => {
    try { const txt = await testerApi.compareText(ids, fmt); await navigator.clipboard.writeText(txt); show(`${fmt.toUpperCase()} 복사 (${txt.length} 자)`, 'ok') } catch (e) { show(String(e), 'err') }
  }

  // 추세 데이터
  const trendOpts = useMemo(() => ({
    scen: [...new Set(runs.map(r => r.scenario_id))].sort(), prof: [...new Set(runs.map(r => r.profile ?? ''))].sort(), topo: [...new Set(runs.map(r => r.topology))].sort(),
  }), [runs])
  const trendRows = useMemo(() => runs.filter(r => (tScen === ALL || r.scenario_id === tScen) && (tProf === ALL || (r.profile ?? '') === (tProf === NONE ? '' : tProf)) && (tTopo === ALL || r.topology === tTopo)), [runs, tScen, tProf, tTopo])
  const [trendSc, setTrendSc] = useState<ScenarioDoc | null>(null)
  useEffect(() => { if (tScen !== ALL) testerApi.scenario(tScen).then(s => setTrendSc(s.doc)).catch(() => setTrendSc(null)); else setTrendSc(null) }, [tScen])
  const tth = useMemo(() => thresholdsFromScenario(trendSc), [trendSc])

  return (
    <div className="flex h-full flex-col">
      <div className="toolbar flex flex-wrap items-center gap-2.5 border-b border-border bg-muted px-4 py-3">
        <span className="whitespace-nowrap text-md font-semibold text-foreground">비교</span>
        <div className="inline-flex overflow-hidden rounded-md border border-border">
          {(['runs', 'trend'] as const).map(m => <button key={m} onClick={() => setMode(m)} className={`h-7 px-3 text-sm ${mode === m ? 'bg-primary text-primary-foreground' : 'bg-card text-muted-foreground hover:bg-accent'}`}>{m === 'runs' ? 'run 비교' : '추세'}</button>)}
        </div>
        <span className="text-sm text-muted-foreground">{mode === 'runs' ? '첫 run 이 기준 — 지표별 Δ · 회귀(나빠짐) · 허용 오차 = 비율 0.5 pt / 그 외 5 %' : '시나리오 × 프로파일 × 토폴로지의 모든 run — 기준 = 첫 PASS ± 허용 오차'}</span>
        {mode === 'runs' && result && <Badge variant={result.regressions > 0 ? 'dangerSoft' : 'successSoft'}>{result.regressions > 0 ? `회귀 ${result.regressions}건` : '회귀 없음'}</Badge>}
        {mode === 'runs' && result && !result.same_scenario && <Badge variant="warningSoft">시나리오가 서로 다름 — 지표 비교의 뜻이 약하다</Badge>}
        {mode === 'runs' && profilesDiffer && <Badge variant="warningSoft">프로파일이 다름 — 시간축 겹침의 단계 띠는 기준 run 것</Badge>}
        {err && <span className="text-sm text-destructive">{err}</span>}
        <div className="ml-auto flex items-center gap-2">
          {mode === 'runs' && <>
            <Button variant="outline" size="sm" onClick={() => exportText('md')} disabled={ids.length < 2}><Download size={13} /> Markdown</Button>
            <Button variant="outline" size="sm" onClick={() => exportText('csv')} disabled={ids.length < 2}><Download size={13} /> CSV</Button>
            <Button variant="outline" size="sm" onClick={() => setIds([])} disabled={ids.length === 0}><X size={13} /> 비우기</Button>
          </>}
        </div>
      </div>

      {mode === 'runs' && (
        <div className="page-scroll flex-1 min-h-0 overflow-auto p-4 flex flex-col gap-4">
          <section className="relative flex flex-col gap-1.5">
            <h3 className="text-sm font-semibold text-muted-foreground">대상 run <span className="font-normal">— 카드를 끌어(또는 ‹ ›) 순서 변경 · 첫 카드 = 기준</span></h3>
            <div className="grid gap-2 sm:grid-cols-2 xl:grid-cols-4">
              {(result?.runs ?? ids.map(id => ({ id, missing: !runs.find(r => r.id === id) } as CompareResult['runs'][number]))).map((r, i) => {
                const row = runs.find(x => x.id === r.id)
                return (
                  <div key={r.id ?? i} draggable onDragStart={() => setDrag(r.id!)} onDragOver={e => e.preventDefault()} onDrop={() => onDrop(r.id!)}
                       className={`relative flex cursor-grab flex-col gap-0.5 rounded-md border p-2.5 text-xs ${i === 0 ? 'border-primary bg-brandsoft/40' : 'border-border bg-muted'}`}>
                    <div className="flex items-center gap-1.5"><span className="inline-block h-2.5 w-2.5 rounded-sm" style={{ background: COLORS[i % COLORS.length] }} /><span className="font-mono font-semibold">{r.id}</span>
                      {i === 0 ? <Badge variant="brandSoft" className="ml-auto">기준</Badge> : r.verdict ? <Badge variant={VERDICT_TONE[r.verdict] ?? 'neutralSoft'} className="ml-auto">{VERDICT_LABEL[r.verdict] ?? r.verdict}</Badge> : <Badge variant="dangerSoft" className="ml-auto">없음</Badge>}</div>
                    <div className="truncate text-muted-foreground">{r.missing ? '색인에 없음' : `${r.scenario_id} · ${r.profile ?? '단발'}${r.label ? ` · ${r.label}` : ''}`}</div>
                    <div className="truncate font-mono text-muted-foreground">{r.target_build ?? row?.target_build ?? '—'} · {fmtTime(r.started_at ?? row?.started_at, true)}</div>
                    <div className="mt-1 flex items-center gap-0.5">
                      {i !== 0 && <Button variant="ghost" size="sm" className="h-6 px-1.5 text-xs" onClick={() => makeBase(r.id!)}>기준으로</Button>}
                      <Button variant="ghost" size="iconSm" className="h-6" onClick={() => move(r.id!, -1)} disabled={i === 0}><ChevronLeft size={12} /></Button>
                      <Button variant="ghost" size="iconSm" className="h-6" onClick={() => move(r.id!, 1)} disabled={i === ids.length - 1}><ChevronRight size={12} /></Button>
                      <Button variant="ghost" size="iconSm" className="ml-auto h-6" onClick={() => remove(r.id!)} aria-label="제거"><X size={12} /></Button>
                    </div>
                  </div>
                )
              })}
              <button onClick={() => setPicker(p => !p)} className="flex min-h-[84px] items-center justify-center gap-1 rounded-md border border-dashed border-border text-xs text-muted-foreground hover:border-primary hover:text-primary"><Plus size={13} /> run 추가</button>
            </div>
            {picker && (
              <div className="absolute right-0 top-8 z-20 max-h-[360px] w-[min(600px,90vw)] overflow-auto rounded-md border border-border bg-card shadow-lg">
                <div className="sticky top-0 flex items-center gap-2 border-b border-border bg-card px-3 py-2 text-xs text-muted-foreground">{baseRow ? '같은 시나리오·프로파일 run 을 먼저' : '기준 run 이 없어 전부'}<label className="ml-auto inline-flex items-center gap-1"><Checkbox checked={pickAll} onCheckedChange={v => setPickAll(v === true)} /> 전부 보기</label><Button variant="ghost" size="iconSm" onClick={() => setPicker(false)}><X size={12} /></Button></div>
                {candidates.length === 0 && <div className="p-3 text-xs text-muted-foreground">후보 없음</div>}
                {candidates.map(({ r, dim }) => (
                  <button key={r.id} onClick={() => add(r.id)} className={`grid w-full grid-cols-[auto_1fr_auto_auto] items-center gap-2 border-b border-border px-3 py-1.5 text-left text-xs hover:bg-accent ${dim ? 'opacity-50' : ''}`}>
                    <Badge variant={VERDICT_TONE[r.verdict] ?? 'neutralSoft'}>{VERDICT_LABEL[r.verdict] ?? r.verdict}</Badge>
                    <span className="truncate"><span className="font-mono">{r.id}</span> · {r.scenario_id} · {r.profile ?? '단발'}{r.label ? ` · ${r.label}` : ''}</span>
                    <span className="font-mono text-muted-foreground">{r.target_build ?? '—'}</span><span className="font-mono text-muted-foreground">{fmtTime(r.started_at, true)}</span>
                  </button>
                ))}
              </div>
            )}
          </section>

          {ids.length < 2 && (
            <EmptyState title={ids.length === 0 ? '비교할 run 을 고르십시오' : 'run 을 하나 더 추가하십시오'}
                        description="[실행] 색인에서 체크해 [비교] 를 누르거나, 위 [+ run 추가] 로 하나씩. 같은 시나리오를 다른 대상 빌드로 돌린 run 을 겹치는 것이 본래 용도입니다." />
          )}

          {result && (
            <>
              <div className="grid gap-4 xl:grid-cols-[minmax(0,3fr)_minmax(0,2fr)]">
                <section className="flex min-w-0 flex-col gap-1.5">
                  <h3 className="text-sm font-semibold text-muted-foreground"><GitCompareArrows size={13} className="mr-1 inline" /> 지표 매트릭스 <span className="font-normal">— 행을 누르면 오른쪽에 시간축 겹침</span></h3>
                  {result.metrics.length === 0 ? <EmptyState title="비교 가능한 지표가 없습니다" /> : (
                    <DataTable>
                      <thead>
                        <tr>
                          <Th>지표</Th><Th width={40}>방향</Th>
                          {result.runs.map((r, i) => <Th key={r.id ?? i} align="right"><span className="mr-1 inline-block h-2 w-2 rounded-sm align-middle" style={{ background: COLORS[i % COLORS.length] }} />{i === 0 ? '기준 ' : ''}<span className="font-mono">{r.target_build ?? r.id}</span></Th>)}
                          <Th width={70}>허용</Th>
                        </tr>
                      </thead>
                      <tbody>
                        {result.metrics.map(m => {
                          const maxd = Math.max(...m.delta.map(d => Math.abs(d ?? 0)), 1e-9)
                          const isPct = m.metric.endsWith('_pct')
                          return (
                            <tr key={m.metric} className={`cursor-pointer hover:bg-accent ${metric === m.metric ? 'shadow-[inset_3px_0_0_var(--primary)]' : ''}`} onClick={() => setMetric(m.metric)}>
                              <Td>{LABEL[m.metric] ?? m.metric}</Td>
                              <Td>{m.direction === 'up' ? <ArrowUp size={13} className="text-muted-foreground" aria-label="클수록 좋음" /> : <ArrowDown size={13} className="text-muted-foreground" aria-label="작을수록 좋음" />}</Td>
                              {m.values.map((v, i) => {
                                const d = m.delta[i]
                                const reg = !!m.regression[i]
                                const tol = isPct ? 0.5 : Math.abs(m.base ?? 0) * 0.05
                                const imp = i > 0 && d != null && !reg && Math.abs(d) > tol
                                const w = d == null ? 0 : (Math.abs(d) / maxd) * 50
                                return (
                                  <Td key={i} align="right" mono className={reg ? 'bg-dangersoft/40 font-semibold text-destructive' : imp ? 'text-success' : ''}>
                                    {fmtNum(v)}
                                    {i > 0 && d != null && (
                                      <span className="block text-[11px]">
                                        <span className={reg ? 'text-destructive' : imp ? 'text-success' : 'text-muted-foreground'}>{d > 0 ? '+' : ''}{fmtNum(d)}{isPct ? ' pt' : ''}</span>
                                        <span className="relative ml-1.5 inline-block h-1.5 w-[60px] rounded-sm bg-neutral-soft align-middle">
                                          <span className="absolute inset-y-[-2px] left-1/2 w-px bg-foreground/50" />
                                          <span className={`absolute inset-y-0 rounded-sm ${reg ? 'bg-destructive' : imp ? 'bg-success' : 'bg-muted-foreground'}`} style={{ left: `${d > 0 ? 50 : 50 - w}%`, width: `${w}%` }} />
                                        </span>
                                      </span>
                                    )}
                                  </Td>
                                )
                              })}
                              <Td className="text-xs text-muted-foreground">{isPct ? '±0.5 pt' : '±5 %'}</Td>
                            </tr>
                          )
                        })}
                      </tbody>
                    </DataTable>
                  )}
                </section>
                <section className="flex min-w-0 flex-col gap-1.5">
                  <h3 className="text-sm font-semibold text-muted-foreground">시간축 겹침 — {LABEL[metric] ?? metric} <span className="font-normal">· run 시작 t+0 정렬 · 기준 굵게</span></h3>
                  <OverlayChart runs={ids} series={series} metric={metric} threshold={thOf(metric)} bands={baseBands} colors={COLORS} />
                  <div className="flex flex-wrap gap-2 text-[11px] text-muted-foreground">{result.runs.map((r, i) => <span key={r.id ?? i} className="inline-flex items-center gap-1"><span className="inline-block h-[3px] w-4 rounded" style={{ background: COLORS[i % COLORS.length] }} />{r.target_build ?? r.id}</span>)}</div>
                  <h3 className="mt-2 text-sm font-semibold text-muted-foreground">응답 코드 분해</h3>
                  <div className="flex flex-col gap-1 text-xs">
                    {result.runs.map((r, i) => (
                      <div key={r.id ?? i} className="flex flex-wrap items-center gap-1"><span className="inline-block h-2 w-2 rounded-sm" style={{ background: COLORS[i % COLORS.length] }} /><span className="font-mono text-muted-foreground">{r.target_build ?? r.id}</span>
                        {parseCodes(r.summary?.codes).map(([c, n]) => <Badge key={c} variant={c.startsWith('2') || c.startsWith('1') ? 'successSoft' : c.startsWith('5') ? 'dangerSoft' : 'warningSoft'}>{c} × {n}</Badge>)}
                        {parseCodes(r.summary?.codes).length === 0 && <span className="text-muted-foreground">—</span>}</div>
                    ))}
                  </div>
                </section>
              </div>

              <section className="flex flex-col gap-1.5">
                <h3 className="text-sm font-semibold text-muted-foreground">기대치 판정 diff <span className="font-normal">— 기준 PASS → 마지막 FAIL 이 회귀, 반대는 복구</span></h3>
                <DataTable>
                  <thead><tr><Th width={40}>#</Th><Th>단계 · 지표</Th><Th>기대</Th>{result.runs.map((r, i) => <Th key={r.id ?? i} align="center">{r.target_build ?? r.id}</Th>)}<Th width={80}>변화</Th></tr></thead>
                  <tbody>
                    {expectDiff.map(row => (
                      <tr key={row.key} className={row.change === 'reg' ? 'bg-dangersoft/40' : ''}>
                        <Td mono>{row.step + 1}</Td>
                        <Td>{STEP_LABEL[row.kind] ?? row.kind} <span className="font-mono text-xs">{row.metric}</span></Td>
                        <Td mono className="text-xs">{expectText(row.expect)}</Td>
                        {row.cells.map((c, i) => <Td key={i} align="center">{c == null ? <span className="text-muted-foreground">—</span> : <Badge variant={c.ok ? 'successSoft' : 'dangerSoft'} title={expectText(c.observed)}>{c.ok ? 'PASS' : 'FAIL'}</Badge>}</Td>)}
                        <Td>{row.change === 'reg' ? <Badge variant="dangerSoft">회귀</Badge> : row.change === 'fix' ? <Badge variant="successSoft">복구</Badge> : <span className="text-muted-foreground">—</span>}</Td>
                      </tr>
                    ))}
                    {expectDiff.length === 0 && <tr><Td colSpan={4 + result.runs.length} className="text-muted-foreground">기대치 판정 없음</Td></tr>}
                  </tbody>
                </DataTable>
                {result.runs.some(r => r.stop_reason) && <div className="text-xs text-muted-foreground">중단 사유: {result.runs.filter(r => r.stop_reason).map(r => `${r.id} — ${r.stop_reason}`).join(' · ')}</div>}
              </section>
            </>
          )}
        </div>
      )}

      {mode === 'trend' && (
        <div className="page-scroll flex-1 min-h-0 overflow-auto p-4 flex flex-col gap-4">
          <div className="flex flex-wrap items-center gap-2 text-sm">
            <span className="text-muted-foreground">시나리오</span>
            <Select value={tScen} onValueChange={setTScen}><SelectTrigger className="h-[28px] w-[220px] text-sm"><SelectValue /></SelectTrigger><SelectContent><SelectItem value={ALL}>전체</SelectItem>{trendOpts.scen.map(s => <SelectItem key={s} value={s}>{s}</SelectItem>)}</SelectContent></Select>
            <span className="text-muted-foreground">프로파일</span>
            <Select value={tProf} onValueChange={setTProf}><SelectTrigger className="h-[28px] w-[170px] text-sm"><SelectValue /></SelectTrigger><SelectContent><SelectItem value={ALL}>전체</SelectItem>{trendOpts.prof.map(p => <SelectItem key={p || NONE} value={p || NONE}>{p || '단발'}</SelectItem>)}</SelectContent></Select>
            <span className="text-muted-foreground">토폴로지</span>
            <Select value={tTopo} onValueChange={setTTopo}><SelectTrigger className="h-[28px] w-[160px] text-sm"><SelectValue /></SelectTrigger><SelectContent><SelectItem value={ALL}>전체</SelectItem>{trendOpts.topo.map(t => <SelectItem key={t} value={t}>{t}</SelectItem>)}</SelectContent></Select>
            <span className="ml-auto text-xs text-muted-foreground">x = run 시각 · 색 = 대상 빌드 · ● PASS / ◆ FAIL·중단 · 붉은 점선 = 기대치 · 회색 띠 = 기준(첫 PASS) ± 허용</span>
          </div>
          <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
            <TrendChart rows={trendRows} metric="srd_ms_p95" label="SRD p95 ms" threshold={tth.srd_ms} dir="down" />
            <TrendChart rows={trendRows} metric="ser_pct" label="SER %" threshold={tth.ser_pct} dir="up" />
            <TrendChart rows={trendRows} metric="rtp_loss_pct" label="RTP 손실 %" threshold={tth.rtp_loss_pct} dir="down" />
            <TrendChart rows={trendRows} metric="doc_saps" label="DOC SApS" dir="up" />
          </div>
          <section className="flex flex-col gap-1.5">
            <div className="flex items-center gap-2"><h3 className="text-sm font-semibold text-muted-foreground">run 목록 ({trendRows.length}) <span className="font-normal">— 체크 → run 비교로</span></h3>
              {tPick.length >= 2 && <Button variant="default" size="sm" onClick={() => { setIds(tPick); setMode('runs') }}><GitCompareArrows size={13} /> 선택 {tPick.length}건 비교</Button>}</div>
            <DataTable>
              <thead><tr><Th width={32} /><Th>판정</Th><Th>시각</Th><Th>run</Th><Th>대상 빌드</Th><Th>프로파일</Th><Th align="right">SRD p95</Th><Th align="right">SER</Th><Th align="right">손실</Th><Th align="right">DOC</Th></tr></thead>
              <tbody>
                {trendRows.map(r => (
                  <tr key={r.id}>
                    <Td><Checkbox checked={tPick.includes(r.id)} onCheckedChange={() => setTPick(p => p.includes(r.id) ? p.filter(x => x !== r.id) : [...p, r.id])} /></Td>
                    <Td><Badge variant={VERDICT_TONE[r.verdict] ?? 'neutralSoft'}>{VERDICT_LABEL[r.verdict] ?? r.verdict}</Badge></Td>
                    <Td mono>{fmtTime(r.started_at, true)}</Td><Td mono>{r.id}{r.label ? <span className="ml-1 text-muted-foreground">{r.label}</span> : null}</Td>
                    <Td className="text-xs">{orDash(r.target_build)}</Td><Td className="text-xs">{r.profile ?? '단발'}</Td>
                    <Td align="right" mono>{fmtNum(r.summary?.srd_ms_p95, 0)}</Td><Td align="right" mono>{fmtNum(r.summary?.ser_pct)}</Td>
                    <Td align="right" mono>{fmtNum(r.summary?.rtp_loss_pct)}</Td><Td align="right" mono>{fmtNum(r.summary?.doc_saps, 1)}</Td>
                  </tr>
                ))}
                {trendRows.length === 0 && <tr><Td colSpan={10} className="text-muted-foreground">run 없음</Td></tr>}
              </tbody>
            </DataTable>
          </section>
        </div>
      )}
    </div>
  )
}
