// 시험 > 결과 — 왼쪽 run 레일(날짜 그룹·판정 점·검색·칩·↑↓) + RunReport(판정 요약·시간축·지표·절차·실패·단계 로그·대상 증거·Markdown)
// + 인쇄(PDF)·삭제·재실행. ?id=<run> 으로 열린다. 진행 중 run 이면 라이브 패널로 (test_instrument.md §7 결과 보고서).
import { useCallback, useEffect, useMemo, useState } from 'react'
import { useSearchParams } from 'react-router-dom'
import { Printer, Trash2, RefreshCw, Activity, ChevronUp, ChevronDown } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Input } from '@core/components/ui/input'
import { EmptyState } from '@core/components/custom/empty-state'
import { useToast } from '@core/components/Toast'
import { useConfirm } from '@core/components/custom/confirm'
import { useAuth } from '@core/contexts/AuthContext'
import { hasRole } from '@core/utils/permissions'
import { testerApi, type RunRow, type RunDoc, type RunEvent, type RunSeries, type ScenarioDoc, type Verdict } from '@tester/api/tester'
import RunReport from '@tester/components/RunReport'
import RunLivePanel from '@tester/components/RunLivePanel'
import RunStartDialog, { type RunStartInitial } from '@tester/components/RunStartDialog'
import { fmtTime, fmtDuration, verdictDot, VERDICT_TONE, VERDICT_LABEL } from '@tester/lib/fmt'
import { StatusDot } from '@core/components/custom/status-dot'

const PRINT_CSS = `
@media print {
  @page { margin: 8mm 12mm; size: A4; }
  html, body { background: #fff !important; color: #111 !important; margin: 0 !important; padding: 0 !important; }
  .app-layout, .app-layout--collapsed, .app-content, .app-content-body, .tester-results-page { display: block !important; margin: 0 !important; padding: 0 !important; max-width: none !important; width: 100% !important; height: auto !important; overflow: visible !important; }
  .sidebar, .sidebar--collapsed, .app-header, .sub-tabs, .tester-results-page .toolbar, .tester-results-page .no-print, .tester-results-page .run-rail { display: none !important; }
  .tester-results-page .page-scroll { overflow: visible !important; height: auto !important; padding: 0 !important; }
  .tester-report > section { break-inside: avoid; }
  .tester-report table { page-break-inside: auto; } .tester-report tr { page-break-inside: avoid; }
  .tester-report * { -webkit-print-color-adjust: exact !important; print-color-adjust: exact !important; }
}`

type Chip = '' | 'fail' | 'pass' | 'load'

export default function TesterResultsPage() {
  const [params, setParams] = useSearchParams()
  const { show } = useToast()
  const confirm = useConfirm()
  const { user } = useAuth()
  const canManage = hasRole(user, 'manager')
  const id = params.get('id') ?? ''
  const [runs, setRuns] = useState<RunRow[]>([])
  const [doc, setDoc] = useState<RunDoc | null>(null)
  const [final, setFinal] = useState(true)
  const [markdown, setMarkdown] = useState<string | null>(null)
  const [events, setEvents] = useState<RunEvent[]>([])
  const [series, setSeries] = useState<RunSeries | null>(null)
  const [scenario, setScenario] = useState<ScenarioDoc | null>(null)
  const [err, setErr] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)
  const [q, setQ] = useState('')
  const [chip, setChip] = useState<Chip>('')
  const [rerun, setRerun] = useState<RunStartInitial | null>(null)

  const loadRuns = useCallback(() => testerApi.runs(300).then(r => setRuns(r.runs)).catch(() => {}), [])
  useEffect(() => { loadRuns() }, [loadRuns])

  const load = useCallback(async () => {
    if (!id) { setDoc(null); return }
    setLoading(true); setErr(null)
    try {
      const rep = await testerApi.report(id)
      setFinal(rep.final); setMarkdown(rep.markdown)
      const d = rep.run as RunDoc
      setDoc(d)
      const [ev, se] = await Promise.all([testerApi.events(id, 500), testerApi.series(id)])
      setEvents(ev.events.slice().reverse()); setSeries(se)
      try { const sc = await testerApi.scenario(d.scenario_id); setScenario(sc.doc) } catch { setScenario(null) }
    } catch (e) { setErr(String(e)); setDoc(null) } finally { setLoading(false) }
  }, [id])
  useEffect(() => { load() }, [load])

  const liveRow = useMemo(() => runs.find(r => r.id === id && r.verdict === 'running'), [runs, id])
  const railRuns = useMemo(() => {
    const qq = q.trim().toLowerCase()
    return runs.filter(r => (!qq || r.id.toLowerCase().includes(qq) || r.scenario_id.toLowerCase().includes(qq) || (r.label ?? '').toLowerCase().includes(qq))
      && (chip === '' || (chip === 'load' ? !!r.profile : r.verdict === chip)))
  }, [runs, q, chip])
  const groups = useMemo(() => {
    const m = new Map<string, RunRow[]>()
    for (const r of railRuns) { const d = (r.started_at ?? '').slice(0, 10); const a = m.get(d) ?? []; a.push(r); m.set(d, a) }
    return [...m.entries()]
  }, [railRuns])
  const idx = railRuns.findIndex(r => r.id === id)
  const go = (i: number) => { const r = railRuns[i]; if (r) setParams({ id: r.id }) }

  const onDelete = async () => {
    if (!doc) return
    if (!await confirm({ title: 'run 삭제', body: `${doc.id} 의 색인·지표·이벤트·SIP 덤프를 지웁니다. 되돌릴 수 없습니다.`, confirmLabel: '삭제', tone: 'danger' })) return
    try { await testerApi.deleteRun(doc.id); show('삭제', 'ok'); setParams({}); setRuns(r => r.filter(x => x.id !== doc.id)) } catch (e) { show(String(e), 'err') }
  }

  return (
    <div className="tester-results-page flex h-full flex-col">
      <style>{PRINT_CSS}</style>
      <div className="toolbar flex flex-wrap items-center gap-2.5 border-b border-border bg-muted px-4 py-3">
        <span className="whitespace-nowrap text-md font-semibold text-foreground">결과</span>
        {doc && <span className="font-mono text-sm text-muted-foreground">{doc.id}</span>}
        {doc && <Badge variant={VERDICT_TONE[doc.verdict] ?? 'neutralSoft'}>{VERDICT_LABEL[doc.verdict] ?? doc.verdict}</Badge>}
        {!final && doc && <Badge variant="infoSoft">진행 중 — 최종 보고서 아님</Badge>}
        {err && <span className="text-sm text-destructive">{err}</span>}
        <div className="ml-auto flex items-center gap-2">
          <Button variant="outline" size="sm" onClick={() => go(idx - 1)} disabled={idx <= 0} title="이전 run"><ChevronUp size={13} /></Button>
          <Button variant="outline" size="sm" onClick={() => go(idx + 1)} disabled={idx < 0 || idx >= railRuns.length - 1} title="다음 run"><ChevronDown size={13} /></Button>
          <Button variant="outline" size="sm" onClick={() => { load(); loadRuns() }} disabled={!id || loading}><RefreshCw size={13} /> 새로고침</Button>
          <Button variant="outline" size="sm" onClick={() => window.print()} disabled={!doc || !final} title="보고서를 PDF 로 인쇄"><Printer size={13} /> 인쇄</Button>
          {canManage && <Button variant="destructive" size="sm" onClick={onDelete} disabled={!doc || !final}><Trash2 size={13} /> 삭제</Button>}
        </div>
      </div>

      <div className="flex min-h-0 flex-1">
        <aside className="run-rail flex w-[280px] shrink-0 flex-col border-r border-border bg-card">
          <div className="flex flex-col gap-1.5 border-b border-border p-2">
            <Input value={q} onChange={e => setQ(e.target.value)} placeholder="run·시나리오·라벨 검색" className="h-[28px] text-sm" />
            <div className="flex gap-1">
              {([['', '전체'], ['fail', 'FAIL'], ['pass', 'PASS'], ['load', '부하']] as [Chip, string][]).map(([c, l]) => (
                <button key={c} onClick={() => setChip(c)} className={`h-6 rounded-sm border px-2 text-xs ${chip === c ? 'border-primary bg-primary text-primary-foreground' : 'border-border text-muted-foreground hover:bg-accent'}`}>{l}</button>
              ))}
            </div>
          </div>
          <div className="min-h-0 flex-1 overflow-auto">
            {groups.length === 0 && <div className="p-3 text-xs text-muted-foreground">run 없음</div>}
            {groups.map(([day, rows]) => (
              <div key={day}>
                <div className="sticky top-0 bg-muted px-3 py-1 text-[11px] font-semibold text-muted-foreground">{day} · {rows.length}</div>
                {rows.map(r => (
                  <button key={r.id} onClick={() => setParams({ id: r.id })}
                          className={`flex w-full flex-col gap-0.5 border-b border-border px-3 py-1.5 text-left text-xs hover:bg-accent ${r.id === id ? 'bg-accent' : ''}`}>
                    <div className="flex items-center gap-1.5"><StatusDot tone={verdictDot(r.verdict as Verdict)} /><span className="truncate font-mono font-medium">{r.scenario_id}</span><span className="ml-auto font-mono text-muted-foreground">{fmtTime(r.started_at)}</span></div>
                    <div className="flex items-center gap-1.5 text-muted-foreground"><span className="truncate">{r.label ?? r.id}</span><span className="ml-auto whitespace-nowrap">{r.profile ? '부하' : '단발'} · {fmtDuration(r.started_at, r.ended_at)}</span></div>
                  </button>
                ))}
              </div>
            ))}
          </div>
        </aside>

        <div className="page-scroll min-h-0 min-w-0 flex-1 overflow-auto p-4 flex flex-col gap-4">
          {!id && <EmptyState title="run 을 선택하십시오" description="왼쪽 레일에서 고르거나 [실행] 색인에서 행을 누릅니다." />}
          {id && liveRow && (
            <section className="rounded-md border border-border bg-card p-3">
              <div className="mb-2 flex items-center gap-2 text-sm text-muted-foreground"><Activity size={13} /> 진행 중 — 라이브</div>
              <RunLivePanel run={liveRow} onEnded={() => { load(); loadRuns() }} />
            </section>
          )}
          {id && doc && final && (
            <RunReport run={doc} scenario={scenario} series={series} events={events} markdown={markdown} print runs={runs}
                       onRerun={r => setRerun({ scenario_id: r.scenario_id, topology: r.topology, profile: r.profile ?? null, bindings: r.bindings, label: r.label ?? null, instances: r.plan?.max_instances ?? undefined })} />
          )}
        </div>
      </div>
      {rerun && <RunStartDialog initial={rerun} onClose={() => setRerun(null)} onStarted={() => { setRerun(null); loadRuns() }} />}
    </div>
  )
}
