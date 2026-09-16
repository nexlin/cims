// 시험 > 실행 — 진행 중 run 의 라이브 패널(SSE) + run 색인. run 시작은 RunStartDialog, 상세는 [결과] 로.
import { useCallback, useEffect, useMemo, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { RefreshCw, Play, GitCompareArrows, FileText } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Checkbox } from '@core/components/ui/checkbox'
import { DataTable, Th, Td, TrLink, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { StatusDot, type StatusTone } from '@core/components/custom/status-dot'
import { testerApi, openTesterStream, type TesterHealth, type RunRow, type RunStateFrame } from '@tester/api/tester'
import RunLivePanel from '@tester/components/RunLivePanel'
import RunStartDialog from '@tester/components/RunStartDialog'
import { fmtTime, fmtDuration, fmtPct, fmtNum, VERDICT_TONE, VERDICT_LABEL } from '@tester/lib/fmt'

type StreamState = 'connecting' | 'open' | 'closed'

export default function TesterRunsPage() {
  const nav = useNavigate()
  const [health, setHealth] = useState<TesterHealth | null>(null)
  const [healthErr, setHealthErr] = useState<string | null>(null)
  const [runs, setRuns] = useState<RunRow[]>([])
  const [loading, setLoading] = useState(true)
  const [stream, setStream] = useState<StreamState>('connecting')
  const [streamDetail, setStreamDetail] = useState('')
  const [startOpen, setStartOpen] = useState(false)
  const [picked, setPicked] = useState<string[]>([])

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [h, r] = await Promise.all([testerApi.health(), testerApi.runs()])
      setHealth(h); setHealthErr(null); setRuns(r.runs)
    } catch (e) {
      setHealth(null); setHealthErr(String(e))
    } finally { setLoading(false) }
  }, [])
  useEffect(() => { load() }, [load])

  useEffect(() => {
    const close = openTesterStream(
      f => {
        if (f.stream !== 'runs') return
        const r = f.record as unknown as RunStateFrame
        if (r.kind) return
        // 상태 프레임 — 색인 행을 즉시 갱신하고, 시작/종료면 목록을 다시 받는다(색인 저장은 종료 시점)
        setRuns(prev => {
          const i = prev.findIndex(x => x.id === r.run_id)
          if (i < 0) return prev
          const next = prev.slice()
          next[i] = { ...next[i], verdict: r.verdict ?? next[i].verdict, summary: r.summary ?? next[i].summary }
          return next
        })
        if (r.state === 'stopped' || r.state === 'starting' || r.state === 'running') load()
      },
      (s, d) => { setStream(s); setStreamDetail(d ?? '') },
    )
    return close
  }, [load])

  const active = useMemo(() => runs.filter(r => r.verdict === 'running'), [runs])
  const lastTopologyId = useMemo(() => null, [])
  const streamTone: StatusTone = stream === 'open' ? 'success' : stream === 'connecting' ? 'info' : 'danger'
  const streamLabel = stream === 'open' ? '라이브 연결' : stream === 'connecting' ? '연결 중' : `끊김${streamDetail ? ` (${streamDetail})` : ''}`

  const togglePick = (id: string) => setPicked(p => p.includes(id) ? p.filter(x => x !== id) : [...p, id])

  return (
    <div className="flex h-full flex-col">
      <div className="toolbar flex flex-wrap items-center gap-2.5 border-b border-border bg-muted px-4 py-3">
        <span className="whitespace-nowrap text-md font-semibold text-foreground">실행</span>
        <StatusDot tone={streamTone} label={streamLabel} />
        {health && (
          <span className="text-sm text-muted-foreground">
            oam-cims-tester {health.version} · 시나리오 {health.scenarios} · 프로파일 {health.profiles} · 토폴로지 {health.topologies} · run {health.runs}
            {health.worker_stream ? ` · 관측 수신 ${health.worker_stream.ip}:${health.worker_stream.port} (연결 ${health.worker_stream.connections})` : ''}
          </span>
        )}
        {healthErr && <span className="text-sm text-destructive">{healthErr}</span>}
        <div className="ml-auto flex items-center gap-2">
          {picked.length >= 2 && (
            <Button variant="outline" size="sm" onClick={() => nav(`/test/compare?ids=${picked.map(encodeURIComponent).join(',')}`)}>
              <GitCompareArrows size={13} /> 선택 {picked.length}건 비교
            </Button>
          )}
          <Button variant="outline" size="sm" onClick={load} disabled={loading}><RefreshCw size={13} /> 새로고침</Button>
          <Button variant="default" size="sm" disabled={!health || active.length > 0} onClick={() => setStartOpen(true)}
                  title={active.length > 0 ? '진행 중 run 이 있습니다 — 동시에 하나만' : undefined}>
            <Play size={13} /> run 시작
          </Button>
        </div>
      </div>

      <div className="page-scroll flex-1 min-h-0 overflow-auto p-4 flex flex-col gap-4">
        {active.map(r => (
          <section key={r.id} className="rounded-md border border-border bg-card p-3">
            <RunLivePanel run={r} onEnded={load} />
          </section>
        ))}
        {active.length === 0 && (
          <EmptyState title="진행 중인 run 이 없습니다"
                      description="[run 시작] 으로 시나리오·토폴로지·부하 프로파일을 골라 실행합니다. 진행 중에는 여기서 SApS·동시 세션·SER·SRD p95·RTP 손실·CPU 를 1초 단위로 봅니다."
                      action={<Button variant="default" size="sm" disabled={!health} onClick={() => setStartOpen(true)}><Play size={13} /> run 시작</Button>} />
        )}

        <section className="flex flex-col gap-2">
          <h3 className="text-sm font-semibold text-muted-foreground">run 색인 <span className="font-normal">— 행을 누르면 결과, 체크 후 비교</span></h3>
          {runs.length === 0 ? (
            <EmptyState title="실행 이력이 없습니다" />
          ) : (
            <DataTable>
              <thead>
                <tr>
                  <Th width={32} />
                  <Th>run</Th><Th>라벨/시나리오</Th><Th>토폴로지</Th><Th>프로파일</Th><Th>시작</Th><Th>소요</Th>
                  <Th align="right">시도</Th><Th align="right">SER</Th><Th align="right">SRD p95</Th><Th align="right">RTP 손실</Th><Th>대상 빌드</Th><Th>판정</Th><Th width={40} />
                </tr>
              </thead>
              <tbody>
                {runs.map(r => (
                  <TrLink key={r.id} selected={picked.includes(r.id)} onClick={() => nav(`/test/results?id=${encodeURIComponent(r.id)}`)}>
                    <Td onClick={e => e.stopPropagation()}>
                      <Checkbox checked={picked.includes(r.id)} onCheckedChange={() => togglePick(r.id)} aria-label="비교 대상" />
                    </Td>
                    <Td mono>{r.id}</Td>
                    <Td><span className="font-mono">{r.scenario_id}</span>{(r as { label?: string }).label ? <span className="ml-1 text-muted-foreground">{(r as { label?: string }).label}</span> : null}</Td>
                    <Td>{orDash(r.topology)}</Td>
                    <Td>{orDash(r.profile)}</Td>
                    <Td mono>{fmtTime(r.started_at, true)}</Td>
                    <Td mono>{fmtDuration(r.started_at, r.ended_at)}</Td>
                    <Td align="right" mono>{fmtNum(r.summary?.attempts, 0)}</Td>
                    <Td align="right" mono>{fmtPct(r.summary?.ser_pct)}</Td>
                    <Td align="right" mono>{r.summary?.srd_ms_p95 != null ? `${fmtNum(r.summary.srd_ms_p95, 0)} ms` : '—'}</Td>
                    <Td align="right" mono>{fmtPct(r.summary?.rtp_loss_pct)}</Td>
                    <Td className="text-xs">{orDash(r.target_build)}</Td>
                    <Td><Badge variant={VERDICT_TONE[r.verdict] ?? 'neutralSoft'}>{VERDICT_LABEL[r.verdict] ?? r.verdict}</Badge></Td>
                    <Td><FileText size={13} className="text-muted-foreground" /></Td>
                  </TrLink>
                ))}
              </tbody>
            </DataTable>
          )}
        </section>
      </div>

      {startOpen && (
        <RunStartDialog onClose={() => setStartOpen(false)} lastTopologyId={lastTopologyId}
                        onStarted={() => { setStartOpen(false); load() }} />
      )}
    </div>
  )
}
