// 시험 > 결과 — run 상세(RFC 6076 표·지연 분포·절차/예상/결과·단계 DOC/IHS·실패 코드·시간축) + 실패 이벤트 + 보고서 인쇄(PDF).
// ?id=<run> 으로 열린다(실행 화면의 행 클릭). 진행 중 run 이면 라이브 패널로 넘긴다.
import { useCallback, useEffect, useMemo, useState } from 'react'
import { useNavigate, useSearchParams } from 'react-router-dom'
import { Printer, Trash2, RefreshCw, Copy, GitCompareArrows, Activity } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { DataTable, Th, Td, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { useToast } from '@core/components/Toast'
import { useConfirm } from '@core/components/custom/confirm'
import { useAuth } from '@core/contexts/AuthContext'
import { hasRole } from '@core/utils/permissions'
import { testerApi, type RunRow, type RunDoc, type RunEvent, type RunSeries, type ScenarioDoc } from '@tester/api/tester'
import RunReport from '@tester/components/RunReport'
import RunLivePanel from '@tester/components/RunLivePanel'
import { fmtTime, fmtUnix, fmtNum, VERDICT_TONE, VERDICT_LABEL } from '@tester/lib/fmt'

const PRINT_CSS = `
@media print {
  @page { margin: 8mm 12mm; size: A4; }
  html, body { background: #fff !important; color: #111 !important; margin: 0 !important; padding: 0 !important; }
  .app-layout, .app-layout--collapsed, .app-content, .app-content-body, .tester-results-page { display: block !important; margin: 0 !important; padding: 0 !important; max-width: none !important; width: 100% !important; height: auto !important; overflow: visible !important; }
  .sidebar, .sidebar--collapsed, .app-header, .sub-tabs, .tester-results-page .toolbar, .tester-results-page .no-print { display: none !important; }
  .tester-results-page .page-scroll { overflow: visible !important; height: auto !important; padding: 0 !important; }
  .tester-report table { page-break-inside: auto; } .tester-report tr { page-break-inside: avoid; }
  .tester-report * { -webkit-print-color-adjust: exact !important; print-color-adjust: exact !important; }
}`

export default function TesterResultsPage() {
  const [params, setParams] = useSearchParams()
  const nav = useNavigate()
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

  useEffect(() => { testerApi.runs().then(r => setRuns(r.runs)).catch(() => {}) }, [])

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

  const onDelete = async () => {
    if (!doc) return
    if (!await confirm({ title: 'run 삭제', body: `${doc.id} 의 색인·지표·이벤트·SIP 덤프를 지웁니다. 되돌릴 수 없습니다.`, confirmLabel: '삭제', tone: 'danger' })) return
    try { await testerApi.deleteRun(doc.id); show('삭제', 'ok'); setParams({}); setRuns(r => r.filter(x => x.id !== doc.id)) } catch (e) { show(String(e), 'err') }
  }

  const copyMd = async () => {
    if (!markdown) return
    try { await navigator.clipboard.writeText(markdown); show('Markdown 복사', 'ok') } catch { show('클립보드 접근 실패', 'err') }
  }

  return (
    <div className="tester-results-page flex h-full flex-col">
      <style>{PRINT_CSS}</style>
      <div className="toolbar flex flex-wrap items-center gap-2.5 border-b border-border bg-muted px-4 py-3">
        <span className="whitespace-nowrap text-md font-semibold text-foreground">결과</span>
        <Select value={id} onValueChange={v => setParams({ id: v })}>
          <SelectTrigger className="w-[420px]"><SelectValue placeholder="run 선택" /></SelectTrigger>
          <SelectContent>
            {runs.map(r => (
              <SelectItem key={r.id} value={r.id}>
                <span className="font-mono">{r.id}</span> · {r.scenario_id} · {fmtTime(r.started_at, true)} · {VERDICT_LABEL[r.verdict] ?? r.verdict}
              </SelectItem>
            ))}
          </SelectContent>
        </Select>
        {doc && <Badge variant={VERDICT_TONE[doc.verdict] ?? 'neutralSoft'}>{VERDICT_LABEL[doc.verdict] ?? doc.verdict}</Badge>}
        {!final && doc && <Badge variant="infoSoft">진행 중 — 최종 보고서 아님</Badge>}
        {err && <span className="text-sm text-destructive">{err}</span>}
        <div className="ml-auto flex items-center gap-2">
          <Button variant="outline" size="sm" onClick={load} disabled={!id || loading}><RefreshCw size={13} /> 새로고침</Button>
          {doc && runs.length > 1 && (
            <Button variant="outline" size="sm" onClick={() => nav(`/test/compare?ids=${encodeURIComponent(doc.id)}`)}><GitCompareArrows size={13} /> 비교에 담기</Button>
          )}
          <Button variant="outline" size="sm" onClick={copyMd} disabled={!markdown}><Copy size={13} /> Markdown</Button>
          <Button variant="outline" size="sm" onClick={() => window.print()} disabled={!doc || !final} title="보고서를 PDF 로 인쇄"><Printer size={13} /> 인쇄</Button>
          {canManage && <Button variant="destructive" size="sm" onClick={onDelete} disabled={!doc || !final}><Trash2 size={13} /> 삭제</Button>}
        </div>
      </div>

      <div className="page-scroll flex-1 min-h-0 overflow-auto p-4 flex flex-col gap-4">
        {!id && <EmptyState title="run 을 선택하십시오" description="[실행] 의 색인에서 행을 누르거나 위에서 고릅니다." />}
        {id && liveRow && (
          <section className="rounded-md border border-border bg-card p-3">
            <div className="mb-2 flex items-center gap-2 text-sm text-muted-foreground"><Activity size={13} /> 진행 중 — 라이브</div>
            <RunLivePanel run={liveRow} onEnded={() => { load(); testerApi.runs().then(r => setRuns(r.runs)).catch(() => {}) }} />
          </section>
        )}
        {id && doc && final && (
          <>
            <section className="rounded-md border border-border bg-card p-4">
              <RunReport run={doc} scenario={scenario} series={series} print />
            </section>
            <section className="no-print flex flex-col gap-1.5">
              <h3 className="text-sm font-semibold text-muted-foreground">실패 이벤트 ({events.length}) <span className="font-normal">— 실패 호 SIP 덤프는 계측기 호스트 <span className="font-mono">runs/{doc.id}/sip/</span></span></h3>
              {events.length === 0 ? <EmptyState title="실패 이벤트 없음" /> : (
                <DataTable>
                  <thead><tr><Th width={90}>시각</Th><Th>워커</Th><Th>역할 / 신원</Th><Th>단계</Th><Th>코드</Th><Th>지표</Th><Th>Call-ID</Th><Th>상세</Th></tr></thead>
                  <tbody>
                    {events.map((e, i) => (
                      <tr key={`${e.t}-${i}`}>
                        <Td mono>{fmtUnix(e.t)}</Td><Td>{e.worker}</Td>
                        <Td mono>{orDash(e.role)}{e.identity ? ` ${e.identity}` : ''}</Td>
                        <Td>{orDash(e.step)}</Td><Td mono>{orDash(e.code)}</Td>
                        <Td mono>{e.metric ? `${e.metric}=${fmtNum(e.observed)}` : orDash(null)}</Td>
                        <Td mono className="text-xs">{orDash(e.call_id)}</Td>
                        <Td className="break-all">{orDash(e.detail)}</Td>
                      </tr>
                    ))}
                  </tbody>
                </DataTable>
              )}
            </section>
          </>
        )}
      </div>
    </div>
  )
}
