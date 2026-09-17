// run 보고서 — 판정 요약(왜 그 판정인가) · 시간축(소형 차트 6장 + 대상 알람 마커) · RFC 6076 표 · 지연 분포(행 펼침 히스토그램) ·
// 절차/예상/확인 표(여유 막대) · 단계 로그(IHS 막대·DOC) · 실패 이벤트(코드 칩 → SIP 드로어) · 대상 증거 · 참고 · Markdown.
// 화면과 인쇄(PDF) 공용 — 루트 `.tester-report`, 호출 페이지의 @media print 가 이것만 남긴다(test_instrument.md §7 결과 보고서).
import React, { useEffect, useMemo, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { ChevronDown, ChevronRight, Copy, GitCompareArrows, RotateCw, FileCode2 } from 'lucide-react'
import { Badge } from '@core/components/ui/badge'
import { Button } from '@core/components/ui/button'
import { DataTable, Th, Td, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { useToast } from '@core/components/Toast'
import type { RunDoc, ScenarioDoc, ExpectResult, RunSeries, RunEvent, HistResult, TargetAlerts, RunRow, SipDumpRow, TargetSeries } from '@tester/api/tester'
import { testerApi } from '@tester/api/tester'
import LiveCharts from '@tester/components/LiveCharts'
import SipDrawer from '@tester/components/SipDrawer'
import MiniChart from '@tester/components/MiniChart'
import { fmtNum, fmtPct, fmtTime, fmtUnix, fmtDuration, SUMMARY_ROWS, TIMER_ROWS, STEP_LABEL, summaryValue, VERDICT_LABEL } from '@tester/lib/fmt'
import { thresholdsFromScenario, expectText, type ProfileLike } from '@tester/lib/metrics'

function stepText(s: NonNullable<ScenarioDoc['flow']>[number]): string {
  const who = s.who?.length ? s.who.join(', ') : s.from ? `${s.from} → ${s.to ?? ''}` : ''
  const extra: string[] = []
  if (s.after_ms) extra.push(`${s.after_ms} ms 뒤`)
  if (s.seconds != null) extra.push(`${s.seconds} s`)
  if (s.media) extra.push([s.media.audio, s.media.video].filter(Boolean).join('+'))
  if (s.payload) extra.push(`payload ${s.payload}`)
  if (s.cause) extra.push(`Q.850 cause ${s.cause}`)
  return `${STEP_LABEL[s.step] ?? s.step}${who ? ` (${who})` : ''}${extra.length ? ` — ${extra.join(', ')}` : ''}`
}

export function parseCodes(codes: unknown): [string, number][] {
  if (typeof codes !== 'string' || !codes) return []
  return codes.split(',').map(x => x.split(':')).filter(p => p.length === 2).map(([k, v]) => [k, Number(v)] as [string, number])
}

/** 기대/관측의 여유 비율 — 지연·손실은 관측/기대(작을수록 좋다), 비율은 기대/관측. 80 % 넘으면 주의색. */
function margin(r: ExpectResult): number | null {
  const want = typeof r.expect === 'number' ? r.expect : (r.expect && typeof r.expect === 'object') ? (Object.values(r.expect as Record<string, unknown>).find(v => typeof v === 'number') as number | undefined) : undefined
  const got = typeof r.observed === 'number' ? r.observed : (r.observed && typeof r.observed === 'object') ? (Object.values(r.observed as Record<string, unknown>).find(v => typeof v === 'number') as number | undefined) : undefined
  if (want == null || got == null || want === 0) return null
  const up = r.metric.endsWith('_pct') && r.metric !== 'rtp_loss_pct'
  return up ? want / Math.max(got, 1e-9) : got / want
}

// §12 알려진 CSP 과제 — 실패 기대치와 연결
const KNOWN: { when: (f: ExpectResult, run: RunDoc) => boolean; text: string }[] = [
  { when: f => f.metric === 'q850_rx_pct', text: 'Reason: Q.850 은 B2BUA 가 상대 leg 에 복사하지 않는다(§12 — RFC 3326 §2, TS 24.229 §5.4.3.2)' },
  { when: (f, run) => f.kind === 'register' && /trunk|pbx/i.test(run.scenario_id) && !f.ok, text: '트렁크 계정 REGISTER 수신 미구현 — 403(§12)' },
  { when: (f, run) => f.metric === 'code' && /603/.test(String(f.observed)) && /5\d\d/.test(JSON.stringify(f.expect)) && /trunk/i.test(run.scenario_id), text: '트렁크 5xx 가 발신자에 603 으로 매핑된다(§12)' },
  { when: (f, run) => /FAILOVER/.test(run.scenario_id) && !f.ok, text: 'RouteSet 헬스체크 부재 — 무응답 피어에서 Timer B 까지 대기(§12)' },
]

function whySentence(run: RunDoc, isLoad: boolean, p: ProfileLike | null): React.ReactNode {
  const er = run.expect_results ?? []
  const fails = er.filter(x => !x.ok)
  const s = run.summary ?? {}
  if (run.verdict === 'pass') return <>기대치 <b>{er.length - fails.length}/{er.length}</b> 통과{isLoad ? <> · DOC <b>{run.doc_rate != null ? `${fmtNum(run.doc_rate, 1)} SApS` : '—'}</b> (IHS 임계 {p?.ihs_threshold_pct ?? '—'} % 직전 단계)</> : null}</>
  if (run.verdict === 'fail') {
    const known = [...new Set(fails.flatMap(f => KNOWN.filter(k => { try { return k.when(f, run) } catch { return false } }).map(k => k.text)))]
    return <>
      기대치 <b>{fails.length}건 실패</b>{(s.failed as number) > 0 ? <> · 실패 인스턴스 <b>{fmtNum(s.failed, 0)}</b></> : null}
      {fails.length > 0 && <> — {fails.map((f, i) => <span key={i}>{i ? ' · ' : ''}#{f.step + 1} {STEP_LABEL[f.kind] ?? f.kind} <span className="font-mono">{f.metric}</span> 기대 {expectText(f.expect)} / 관측 <b>{expectText(f.observed)}</b></span>)}</>}
      {run.stop_reason && <><br />중단 사유: <b>{run.stop_reason}</b></>}
      {known.map((k, i) => <span key={i}><br />알려진 CSP 과제 — {k}</span>)}
    </>
  }
  if (run.verdict === 'aborted') return <><b>{run.stop_reason ?? '운영자 중단'}</b> — 판정 없이 종료. 그때까지 시도 {fmtNum(s.attempts, 0)}, 세션 {fmtNum(s.sessions, 0)}</>
  return <><b>{run.stop_reason ?? (run.notes ?? [])[0] ?? '오류'}</b> — 시도 전에 실패. 워커 health·토폴로지 연결 검사를 먼저 보십시오</>
}

const SECTIONS = [['verdict', '판정'], ['timeline', '시간축'], ['metrics', '지표'], ['procedure', '절차'], ['events', '실패'], ['steplog', '단계 로그'], ['evidence', '대상 증거'], ['markdown', 'Markdown']] as const

export default function RunReport({ run, scenario, series, events, markdown, print, runs, onRerun }: {
  run: RunDoc
  scenario?: ScenarioDoc | null
  series?: RunSeries | null
  events?: RunEvent[]
  markdown?: string | null
  /** 인쇄 표지용 — true 면 상단에 발행 일시·표지 정보 */
  print?: boolean
  /** 색인(이전 빌드 비교 후보 찾기) */
  runs?: RunRow[]
  onRerun?: (run: RunDoc) => void
}) {
  const nav = useNavigate()
  const { show } = useToast()
  const s = run.summary ?? {}
  const timers = run.timers ?? {}
  const er = run.expect_results ?? []
  const p = (run.profile_doc ?? null) as ProfileLike | null
  const isLoad = !!run.profile
  const byStep = useMemo(() => {
    const m = new Map<number, ExpectResult[]>()
    er.forEach(r => { const a = m.get(r.step) ?? []; a.push(r); m.set(r.step, a) })
    return m
  }, [er])
  const flow = scenario?.flow ?? []
  const codes = parseCodes(s.codes)
  const stepLog = (run.step_log ?? []).filter(r => r.event === 'step' || r.event === 'start')
  const th = useMemo(() => thresholdsFromScenario(scenario), [scenario])
  const [alerts, setAlerts] = useState<TargetAlerts | null>(null)
  const [hist, setHist] = useState<Record<string, HistResult | null>>({})
  const [open, setOpen] = useState<Record<string, boolean>>({})
  const [sipCall, setSipCall] = useState<string | null>(null)
  const [codeFilter, setCodeFilter] = useState<string | null>(null)
  const [hover, setHover] = useState<number | null>(null)

  const [dumps, setDumps] = useState<SipDumpRow[]>([])
  const [tgt, setTgt] = useState<TargetSeries | null>(null)
  const [tgtHover, setTgtHover] = useState<number | null>(null)
  useEffect(() => { setTgt(null); testerApi.targetSeries(run.id).then(setTgt).catch(() => setTgt(null)) }, [run.id])
  useEffect(() => { setDumps([]); testerApi.sipDumps(run.id).then(r => setDumps(r.dumps ?? [])).catch(() => setDumps([])) }, [run.id])
  useEffect(() => { setAlerts(null); testerApi.targetAlerts(run.id).then(setAlerts).catch(() => setAlerts(null)) }, [run.id])
  useEffect(() => { setHist({}); setOpen({}) }, [run.id])
  const toggleHist = async (k: string) => {
    const next = !open[k]
    setOpen(o => ({ ...o, [k]: next }))
    if (next && hist[k] === undefined) {
      try { setHist(h => ({ ...h, [k]: null })); const r = await testerApi.hist(run.id, k); setHist(h => ({ ...h, [k]: r })) } catch { setHist(h => ({ ...h, [k]: null })) }
    }
  }

  const bands = stepLog.map((r, i) => ({ from: r.t, to: i + 1 < stepLog.length ? stepLog[i + 1].t : (series?.t.length ? series.t[series.t.length - 1] + 1 : r.t + 1), label: `${fmtNum(r.rate, 1)} SApS` }))
  const markers = useMemo(() => (alerts?.alerts ?? []).map(a => {
    const ts = String(a.ts ?? a.time ?? '')
    const sev = String(a.severity ?? a.level ?? '').toLowerCase()
    return { t: new Date(ts).getTime() / 1000, label: `${ts.slice(11, 19)} ${String(a.type ?? a.code ?? a.kind ?? '')} ${String(a.message ?? a.detail ?? '')}`.trim(), tone: (/crit|major|error/.test(sev) ? 'danger' : /minor|warn/.test(sev) ? 'warning' : 'info') as 'danger' | 'warning' | 'info' }
  }).filter(m => isFinite(m.t)), [alerts])

  const prevBuild = useMemo(() => (runs ?? []).find(r => r.id !== run.id && r.scenario_id === run.scenario_id && (r.profile ?? null) === (run.profile ?? null)
    && r.verdict !== 'running' && r.target_build && r.target_build !== run.target_build && (r.started_at ?? '') < (run.started_at ?? '')), [runs, run])
  const evShown = codeFilter ? (events ?? []).filter(e => String(e.code ?? e.metric ?? e.step ?? '?') === codeFilter) : (events ?? [])
  const codeCounts = useMemo(() => { const m = new Map<string, number>(); for (const e of events ?? []) { const k = String(e.code ?? e.metric ?? e.step ?? '?'); m.set(k, (m.get(k) ?? 0) + 1) } return [...m.entries()].sort((a, b) => b[1] - a[1]) }, [events])
  const vTone = run.verdict === 'pass' ? 'border-success' : run.verdict === 'fail' || run.verdict === 'error' ? 'border-destructive' : run.verdict === 'aborted' ? 'border-warning' : 'border-info'
  const evidence = scenario?.target_evidence ?? []
  const tgtPeak = typeof s.target_cpu_peak_pct === 'number' ? s.target_cpu_peak_pct : null
  const seedRestored = (run.notes ?? []).some(n => n.includes('csp seed restored'))
  const jump = (id: string) => document.getElementById(`rr-${id}`)?.scrollIntoView({ behavior: 'smooth', block: 'start' })

  return (
    <div className="tester-report flex flex-col gap-4">
      {print && (
        <div className="hidden border-b border-border pb-2 print:block">
          <div className="text-xl font-bold">계측기 시험 보고서</div>
          <div className="text-sm text-muted-foreground">발행 {new Date().toLocaleString('ko-KR')} · oam-cims-tester</div>
        </div>
      )}

      {/* 판정 요약 */}
      <section id="rr-verdict" className={`flex flex-col gap-2 rounded-md border-l-4 bg-card px-4 py-3 ${vTone}`}>
        <div className="flex flex-wrap items-center gap-2">
          <span className={`text-2xl font-bold ${run.verdict === 'pass' ? 'text-success' : run.verdict === 'aborted' ? 'text-warning' : run.verdict === 'running' ? 'text-info' : 'text-destructive'}`}>{VERDICT_LABEL[run.verdict] ?? run.verdict}</span>
          <span className="text-sm text-muted-foreground">기대치 {er.filter(x => x.ok).length}/{er.length}</span>
          <span className="text-md font-semibold">{run.scenario_id}</span>
          {scenario?.title && <span className="text-sm text-muted-foreground">{scenario.title}</span>}
          {run.label && <Badge variant="brandSoft">{run.label}</Badge>}
          <div className="no-print ml-auto flex flex-wrap items-center gap-1.5">
            <Button variant="outline" size="sm" disabled={!prevBuild} title={prevBuild ? `${prevBuild.id} (${prevBuild.target_build})` : '같은 시나리오·프로파일의 직전 빌드 run 없음'}
                    onClick={() => prevBuild && nav(`/test/compare?ids=${encodeURIComponent(prevBuild.id)},${encodeURIComponent(run.id)}`)}><GitCompareArrows size={13} /> 이전 빌드와 비교</Button>
            {onRerun && <Button variant="outline" size="sm" onClick={() => onRerun(run)}><RotateCw size={13} /> 같은 조건 재실행</Button>}
            <Button variant="outline" size="sm" onClick={() => nav(`/test/scenarios?id=${encodeURIComponent(run.scenario_id)}`)}><FileCode2 size={13} /> 시나리오</Button>
          </div>
        </div>
        <div className="text-sm leading-relaxed">{whySentence(run, isLoad, p)}</div>
        <div className="flex flex-wrap gap-1.5 text-xs">
          <Badge variant="neutralSoft" className="font-mono">{run.id}</Badge>
          <Badge variant="neutralSoft">{run.topology} · {run.profile ?? '단발'}</Badge>
          {run.target_build && <Badge variant="neutralSoft">{run.target_build}</Badge>}
          <Badge variant="neutralSoft" className="font-mono">{fmtTime(run.started_at, true)} · {fmtDuration(run.started_at, run.ended_at)}{(run.held_s ?? 0) > 0 ? ` (고정 ${run.held_s}s)` : ''}</Badge>
          <Badge variant="neutralSoft">워커 {(run.workers ?? []).join(', ') || '—'}</Badge>
          {run.plan && <Badge variant="neutralSoft" className="font-mono">{Object.entries(run.plan.roles).map(([r, x]) => `${r}→${typeof x === 'string' ? x : x.pool}`).join(' ')}</Badge>}
          {run.bindings && Object.keys(run.bindings).length > 0 && <Badge variant="neutralSoft" className="font-mono">{Object.entries(run.bindings).map(([k, v]) => `${k}=${String(v)}`).join(' ')}</Badge>}
        </div>
      </section>

      <nav className="no-print sticky top-0 z-[5] flex flex-wrap gap-1 rounded-md border border-border bg-card/95 px-2 py-1 text-xs backdrop-blur">
        {SECTIONS.map(([id, label]) => <button key={id} onClick={() => jump(id)} className="rounded-sm px-2 py-0.5 text-muted-foreground hover:bg-accent hover:text-foreground">{label}</button>)}
      </nav>

      {series && series.t.length >= 2 && (
        <section id="rr-timeline" className="flex flex-col gap-1.5 break-inside-avoid">
          <h3 className="text-sm font-semibold text-muted-foreground">시간축 (1초 집계){markers.length ? <span className="font-normal"> · 대상 알람·이벤트 {markers.length}건 겹침</span> : alerts?.note ? <span className="font-normal"> · 알람 레인: {alerts.note}</span> : ''}</h3>
          <LiveCharts data={{ t: series.t, counters: series.counters, gauges: series.gauges, timers: series.timers }} thresholds={th} bands={bands} markers={markers} stopOn={p?.stop_on} height={96} hover={hover} onHover={setHover} />
          {markers.length > 0 && (
            <div className="flex flex-wrap gap-1 text-[11px]">{markers.slice(0, 20).map((m, i) => <Badge key={i} variant={m.tone === 'danger' ? 'dangerSoft' : m.tone === 'warning' ? 'warningSoft' : 'infoSoft'}>{m.label.slice(0, 60)}</Badge>)}</div>
          )}
        </section>
      )}

      <div id="rr-metrics" className="grid gap-4 lg:grid-cols-2">
        <section className="flex flex-col gap-1.5 break-inside-avoid">
          <h3 className="text-sm font-semibold text-muted-foreground">RFC 6076 지표</h3>
          <DataTable>
            <thead><tr><Th>지표</Th><Th align="right">값</Th></tr></thead>
            <tbody>
              {SUMMARY_ROWS.filter(([k]) => s[k] !== undefined && s[k] !== null).map(([k, label, kind]) => (
                <tr key={k}><Td>{label}</Td><Td align="right" mono>{summaryValue(s, k, kind)}</Td></tr>
              ))}
            </tbody>
          </DataTable>
          {codes.length > 0 && (
            <>
              <h3 className="mt-2 text-sm font-semibold text-muted-foreground">응답 코드 분해</h3>
              <div className="flex flex-wrap gap-1.5">
                {codes.map(([code, n]) => (
                  <Badge key={code} variant={code.startsWith('2') || code.startsWith('1') ? 'successSoft' : code.startsWith('5') ? 'dangerSoft' : 'warningSoft'}>{code} × {n}</Badge>
                ))}
              </div>
            </>
          )}
        </section>
        <section className="flex flex-col gap-1.5 break-inside-avoid">
          <h3 className="text-sm font-semibold text-muted-foreground">지연 분포 <span className="font-normal">— 행을 펼치면 버킷 히스토그램(로그 상한 1·2·5…)</span></h3>
          <DataTable>
            <thead><tr><Th width={20} /><Th>지연</Th><Th align="right">n</Th><Th align="right">p50</Th><Th align="right">p95</Th><Th align="right">p99</Th><Th align="right">max</Th></tr></thead>
            <tbody>
              {TIMER_ROWS.filter(([k]) => timers[k]).map(([k, label]) => {
                const h = timers[k]
                const thv = (th as Record<string, number | undefined>)[k]
                return (
                  <React.Fragment key={k}>
                    <tr className="cursor-pointer hover:bg-accent" onClick={() => toggleHist(k)}>
                      <Td>{open[k] ? <ChevronDown size={13} /> : <ChevronRight size={13} />}</Td>
                      <Td>{label}{thv != null && <span className="ml-1 text-xs text-muted-foreground">기대 ≤ {thv}</span>}</Td>
                      <Td align="right" mono>{h.count}</Td><Td align="right" mono>{fmtNum(h.p50)}</Td>
                      <Td align="right" mono className={thv != null && h.p95 != null && h.p95 > thv ? 'text-destructive font-semibold' : ''}>{fmtNum(h.p95)}</Td>
                      <Td align="right" mono>{fmtNum(h.p99)}</Td><Td align="right" mono>{fmtNum(h.max)}</Td>
                    </tr>
                    {open[k] && (
                      <tr><Td colSpan={7} className="bg-muted"><Histogram h={hist[k]} threshold={thv} /></Td></tr>
                    )}
                  </React.Fragment>
                )
              })}
              {TIMER_ROWS.every(([k]) => !timers[k]) && <tr><Td /><Td className="text-muted-foreground">관측 없음</Td><Td /><Td /><Td /><Td /><Td /></tr>}
            </tbody>
          </DataTable>
        </section>
      </div>

      <section id="rr-procedure" className="flex flex-col gap-1.5">
        <h3 className="text-sm font-semibold text-muted-foreground">절차 · 예상 결과 · 확인 결과 <span className="font-normal">— 여유 막대 = 관측/임계(80 % 넘으면 주의)</span></h3>
        <DataTable>
          <thead><tr><Th width={40}>#</Th><Th>절차(단계)</Th><Th>예상 결과(expect)</Th><Th>확인 결과(관측)</Th><Th width={120}>여유</Th><Th width={70}>판정</Th></tr></thead>
          <tbody>
            {(flow.length ? flow.map((st, i) => ({ i, st })) : Array.from(byStep.keys()).sort((a, b) => a - b).map(i => ({ i, st: null as null | NonNullable<ScenarioDoc['flow']>[number] }))).map(({ i, st }) => {
              const rs = byStep.get(i) ?? []
              const ok = rs.length === 0 ? null : rs.every(r => r.ok)
              const ms = rs.map(margin).filter((x): x is number => x != null)
              const worst = ms.length ? Math.max(...ms) : null
              return (
                <tr key={i} className={ok === false ? 'bg-dangersoft/40' : ''}>
                  <Td mono>{i + 1}</Td>
                  <Td>{st ? stepText(st) : (STEP_LABEL[rs[0]?.kind] ?? rs[0]?.kind)}</Td>
                  <Td mono className="text-xs">{rs.length ? rs.map(r => `${r.metric}: ${expectText(r.expect)}`).join(' · ') : (st?.expect && Object.keys(st.expect).length ? Object.entries(st.expect).map(([m, e]) => `${m}: ${expectText(e)}`).join(' · ') : '—')}</Td>
                  <Td mono className="text-xs">{rs.length ? rs.map(r => `${r.metric}: ${expectText(r.observed)}`).join(' · ') : '—'}</Td>
                  <Td>{worst != null ? (
                    <div className="flex items-center gap-1.5"><div className="h-1.5 w-16 overflow-hidden rounded-sm bg-neutral-soft"><div className={`h-full ${worst > 1 ? 'bg-destructive' : worst > 0.8 ? 'bg-warning' : 'bg-success'}`} style={{ width: `${Math.min(100, worst * 100)}%` }} /></div><span className="font-mono text-xs">{fmtNum(worst * 100, 0)} %</span></div>
                  ) : <span className="text-muted-foreground">—</span>}</Td>
                  <Td>{ok == null ? <Badge variant="neutralSoft">기대치 없음</Badge> : ok ? <Badge variant="successSoft">PASS</Badge> : <Badge variant="dangerSoft">FAIL</Badge>}</Td>
                </tr>
              )
            })}
          </tbody>
        </DataTable>
      </section>

      <section id="rr-events" className="no-print flex flex-col gap-1.5">
        <h3 className="flex flex-wrap items-center gap-1.5 text-sm font-semibold text-muted-foreground">실패 이벤트 ({(events ?? []).length})
          {codeCounts.map(([k, n]) => (
            <button key={k} onClick={() => setCodeFilter(codeFilter === k ? null : k)}
                    className={`h-5 rounded-sm border px-1.5 font-mono text-[11px] ${codeFilter === k ? 'border-primary bg-primary text-primary-foreground' : 'border-border hover:bg-accent'}`}>{k} × {n}</button>
          ))}
          <span className="font-normal">— 행을 누르면 SIP 사다리</span>
        </h3>
        {dumps.length > 0 && (
          <div className="flex flex-wrap items-center gap-1.5 text-xs">
            <span className="text-muted-foreground">SIP 덤프 ({dumps.length}) — 워커가 올린 호, 누르면 사다리</span>
            {dumps.slice(0, 40).map(d => (
              <button key={d.call_id} onClick={() => setSipCall(d.call_id)} title={`${d.messages} 메시지 · ${d.bytes} B`}
                      className="h-5 max-w-[220px] truncate rounded-sm border border-border px-1.5 font-mono text-[11px] hover:bg-accent">{d.call_id}</button>
            ))}
            {dumps.length > 40 && <span className="text-muted-foreground">… +{dumps.length - 40}</span>}
          </div>
        )}
        {evShown.length === 0 ? <EmptyState title="실패 이벤트 없음" /> : (
          <DataTable>
            <thead><tr><Th width={90}>시각</Th><Th>워커</Th><Th>역할 / 신원</Th><Th>단계</Th><Th>코드</Th><Th>Call-ID</Th><Th>상세</Th></tr></thead>
            <tbody>
              {evShown.slice(0, 300).map((e, i) => (
                <tr key={`${e.t}-${i}`} className={e.call_id ? 'cursor-pointer hover:bg-accent' : ''} onClick={() => e.call_id && setSipCall(e.call_id)}>
                  <Td mono>{fmtUnix(e.t)}</Td><Td>{e.worker}</Td>
                  <Td mono className="text-xs">{orDash(e.role)}{e.identity ? ` ${e.identity}` : ''}</Td>
                  <Td>{orDash(e.step)}</Td><Td mono>{e.code != null ? e.code : e.metric ? `${e.metric}=${fmtNum(e.observed)}` : '—'}</Td>
                  <Td mono className="text-xs">{orDash(e.call_id)}</Td>
                  <Td className="break-all text-xs">{orDash(e.detail)}</Td>
                </tr>
              ))}
            </tbody>
          </DataTable>
        )}
      </section>

      {stepLog.length > 1 && (
        <section id="rr-steplog" className="flex flex-col gap-1.5 break-inside-avoid">
          <h3 className="text-sm font-semibold text-muted-foreground">단계 로그 <span className="font-normal">— step 프로파일 · DOC {run.doc_rate != null ? `${fmtNum(run.doc_rate, 1)} SApS` : '미확정'} · IHS 임계 {p?.ihs_threshold_pct ?? '—'} %</span></h3>
          <DataTable>
            <thead><tr><Th>시각</Th><Th align="right">율(SApS)</Th><Th align="right">시도</Th><Th>IHS</Th><Th width={90} /></tr></thead>
            <tbody>
              {stepLog.map((r, i) => {
                const lim = p?.ihs_threshold_pct ?? null
                const over = lim != null && r.ihs_pct != null && r.ihs_pct > lim
                const w = lim ? Math.min(100, (100 * (r.ihs_pct ?? 0)) / (lim * 2)) : 0
                const isDoc = run.doc_rate != null && Math.abs(run.doc_rate - r.rate) < 1e-6
                return (
                  <tr key={i}><Td mono>{fmtUnix(r.t)}</Td><Td align="right" mono>{fmtNum(r.rate, 1)}</Td><Td align="right" mono>{fmtNum(r.attempts, 0)}</Td>
                    <Td><div className="flex items-center gap-1.5"><div className="relative h-1.5 w-24 overflow-hidden rounded-sm bg-neutral-soft"><div className={`h-full ${over ? 'bg-destructive' : 'bg-success'}`} style={{ width: `${w}%` }} />{lim != null && <span className="absolute inset-y-0 left-1/2 w-px bg-foreground/60" title={`임계 ${lim} %`} />}</div><span className="font-mono text-xs">{r.ihs_pct == null ? '—' : fmtPct(r.ihs_pct)}</span></div></Td>
                    <Td>{isDoc && <Badge variant="brandSoft">DOC</Badge>}{over && <Badge variant="dangerSoft">임계 초과 → 중단</Badge>}</Td></tr>
                )
              })}
            </tbody>
          </DataTable>
        </section>
      )}

      <section id="rr-evidence" className="flex flex-col gap-1.5 break-inside-avoid">
        <h3 className="text-sm font-semibold text-muted-foreground">대상 증거 <span className="font-normal">— target_evidence(2차 판정 — 대상 OAM 의 녹취·알람·이벤트를 run 창으로 센다){tgtPeak != null ? ` · 대상 호스트 CPU 피크 ${fmtNum(tgtPeak, 1)} %` : ''}</span></h3>
        {tgt && Object.keys(tgt.agents).length > 0 && (
          <div className="grid gap-2 sm:grid-cols-2">
            {Object.entries(tgt.agents).map(([name, a], i) => (
              <MiniChart key={name} t={a.t} label={`대상 호스트 ${name} — CPU / 메모리`} unit="%" max={100} hover={tgtHover} onHover={setTgtHover}
                         threshold={p?.stop_on?.target_cpu_pct ?? null} thresholdLabel="stop_on"
                         series={[{ key: 'cpu', values: a.cpu_pct, color: `var(--chart-${(i % 5) + 1})`, label: 'cpu' }, { key: 'mem', values: a.mem_pct, color: 'var(--chart-4)', label: 'mem', dashed: true }]} />
            ))}
          </div>
        )}
        {evidence.length === 0 && !run.plan?.peer_pools?.length ? <div className="text-xs text-muted-foreground">시나리오에 target_evidence 없음</div> : (
          <div className="grid gap-2 sm:grid-cols-2 xl:grid-cols-4">
            {evidence.map((e, i) => {
              const r = (run.evidence_results ?? [])[i]
              return (
                <div key={i} className="rounded-md border border-border p-2.5 text-xs">
                  <div className="flex items-center gap-1.5"><span className="font-mono font-semibold">{e.kind}</span>
                    <Badge variant={!r || r.ok == null ? 'neutralSoft' : r.ok ? 'successSoft' : 'dangerSoft'} className="ml-auto">{!r ? '판정 없음' : r.ok == null ? '판정 불가' : r.ok ? 'OK' : 'FAIL'}</Badge></div>
                  <div className="mt-1 text-muted-foreground">기대 {e.min != null ? `≥ ${e.min}` : ''}{e.max != null ? ` ≤ ${e.max}` : ''}{e.code ? ` ${e.code}` : ''}{r && r.observed != null ? <> · 관측 <b className="text-foreground">{r.observed}</b></> : null}</div>
                  {r?.why && <div className="mt-0.5 break-words text-muted-foreground">{r.why}</div>}
                </div>
              )
            })}
            {(run.plan?.peer_pools?.length ?? 0) > 0 && (
              <div className="rounded-md border border-border p-2.5 text-xs">
                <div className="flex items-center gap-1.5"><span className="font-mono font-semibold">seed_restored</span><Badge variant={seedRestored ? 'successSoft' : 'warningSoft'} className="ml-auto">{seedRestored ? 'OK' : '미확인'}</Badge></div>
                <div className="mt-1 text-muted-foreground">피어 풀 {run.plan!.peer_pools!.join(', ')} 시드가 종료 시 원본으로 복원됐는가(run 참고 메모)</div>
              </div>
            )}
          </div>
        )}
      </section>

      {(run.notes?.length ?? 0) > 0 && (
        <section className="flex flex-col gap-1">
          <h3 className="text-sm font-semibold text-muted-foreground">참고</h3>
          <ul className="list-disc pl-5 text-sm">{run.notes!.map((n, i) => <li key={i}>{n}</li>)}</ul>
        </section>
      )}

      {markdown && (
        <section id="rr-markdown" className="no-print flex flex-col gap-1.5">
          <h3 className="flex items-center gap-2 text-sm font-semibold text-muted-foreground">Markdown <span className="font-normal">— CLI `cims-tester report` 와 같은 본문</span>
            <Button variant="ghost" size="sm" onClick={async () => { try { await navigator.clipboard.writeText(markdown); show('Markdown 복사', 'ok') } catch { show('클립보드 접근 실패', 'err') } }}><Copy size={12} /> 복사</Button></h3>
          <pre className="max-h-[320px] overflow-auto whitespace-pre-wrap rounded-md border border-border bg-muted p-3 font-mono text-xs">{markdown}</pre>
        </section>
      )}
      {sipCall && <SipDrawer runId={run.id} callId={sipCall} onClose={() => setSipCall(null)} />}
    </div>
  )
}

function Histogram({ h, threshold }: { h: HistResult | null | undefined; threshold?: number }) {
  if (h === undefined || h === null) return <div className="p-2 text-xs text-muted-foreground">{h === null ? '분포 없음(metrics.sqlite 에 표본이 없다)' : '불러오는 중…'}</div>
  const total = h.buckets.reduce((a, b) => a + b.count, 0) || 1
  const maxC = Math.max(...h.buckets.map(b => b.count), 1)
  return (
    <div className="flex flex-col gap-1 p-2 text-xs">
      <div className="flex items-end gap-0.5" style={{ height: 72 }}>
        {h.buckets.map((b, i) => {
          const mark = [['p50', h.p50], ['p95', h.p95], ['p99', h.p99]].filter(([, v]) => v != null && b.ub != null && (v as number) <= (b.ub as number) && (i === 0 || (v as number) > (h.buckets[i - 1].ub ?? -1))).map(([k]) => k as string)
          const over = threshold != null && b.ub != null && b.ub > threshold
          return (
            <div key={i} className="group relative flex flex-1 flex-col items-center justify-end" title={`≤ ${b.ub ?? '∞'}: ${b.count} (${fmtNum((100 * b.count) / total, 1)} %)`}>
              <div className={`w-full rounded-t-sm ${over ? 'bg-destructive/70' : 'bg-primary/70'}`} style={{ height: `${Math.max(2, (100 * b.count) / maxC)}%` }} />
              {mark.length > 0 && <span className="absolute -top-3 text-[9px] font-semibold text-foreground">{mark.join('/')}</span>}
            </div>
          )
        })}
      </div>
      <div className="flex gap-0.5 font-mono text-[9px] text-muted-foreground">{h.buckets.map((b, i) => <span key={i} className="flex-1 truncate text-center">{b.ub == null ? '∞' : b.ub >= 1000 ? `${b.ub / 1000}k` : b.ub}</span>)}</div>
      <div className="text-muted-foreground">n {h.count} · p50 {fmtNum(h.p50)} · p95 {fmtNum(h.p95)} · p99 {fmtNum(h.p99)} · max {fmtNum(h.max)}{threshold != null ? ` · 기대 ≤ ${threshold}` : ''}</div>
    </div>
  )
}
