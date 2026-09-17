// 시험 > 실행 — 워커·대상 띠 + 진행 중 run 의 라이브 패널(SSE) + run 색인(필터·날짜 그룹·재실행·비교 바). run 시작은 RunStartDialog
// (계획 미리보기), 상세는 [결과] 로 (test_instrument.md §7 실행 라이브).
import { useCallback, useEffect, useMemo, useState } from 'react'
import { RefreshCw, Play } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { EmptyState } from '@core/components/custom/empty-state'
import { StatusDot, type StatusTone } from '@core/components/custom/status-dot'
import { testerApi, openTesterStream, type TesterHealth, type RunRow, type RunStateFrame, type RunFilters, type TopologyRow } from '@tester/api/tester'
import RunLivePanel from '@tester/components/RunLivePanel'
import RunStartDialog, { type RunStartInitial } from '@tester/components/RunStartDialog'
import RunIndex, { type RerunSeed } from '@tester/components/RunIndex'
import WorkerFleet from '@tester/components/WorkerFleet'

type StreamState = 'connecting' | 'open' | 'closed'

export default function TesterRunsPage() {
  const [health, setHealth] = useState<TesterHealth | null>(null)
  const [healthErr, setHealthErr] = useState<string | null>(null)
  const [runs, setRuns] = useState<RunRow[]>([])
  const [indexRuns, setIndexRuns] = useState<RunRow[]>([])
  const [filters, setFilters] = useState<RunFilters>({})
  const [topologies, setTopologies] = useState<TopologyRow[]>([])
  const [loading, setLoading] = useState(true)
  const [stream, setStream] = useState<StreamState>('connecting')
  const [streamDetail, setStreamDetail] = useState('')
  const [startOpen, setStartOpen] = useState<null | RunStartInitial | 'new'>(null)
  const [picked, setPicked] = useState<string[]>([])

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [h, r, t] = await Promise.all([testerApi.health(), testerApi.runs(50), testerApi.topologies()])
      setHealth(h); setHealthErr(null); setRuns(r.runs); setTopologies(t.topologies)
    } catch (e) {
      setHealth(null); setHealthErr(String(e))
    } finally { setLoading(false) }
  }, [])
  useEffect(() => { load() }, [load])

  const reloadIndex = useCallback((f: RunFilters) => {
    setFilters(f)
    testerApi.runs(300, f).then(r => setIndexRuns(r.runs)).catch(() => {})
  }, [])
  const refreshAll = useCallback(() => { load(); testerApi.runs(300, filters).then(r => setIndexRuns(r.runs)).catch(() => {}) }, [load, filters])

  useEffect(() => {
    const close = openTesterStream(
      f => {
        if (f.stream !== 'runs') return
        const r = f.record as unknown as RunStateFrame
        if (r.kind) return
        setRuns(prev => {
          const i = prev.findIndex(x => x.id === r.run_id)
          if (i < 0) return prev
          const next = prev.slice()
          next[i] = { ...next[i], verdict: r.verdict ?? next[i].verdict, summary: r.summary ?? next[i].summary }
          return next
        })
        if (r.state === 'stopped' || r.state === 'starting' || r.state === 'running') refreshAll()
      },
      (s, d) => { setStream(s); setStreamDetail(d ?? '') },
    )
    return close
  }, [refreshAll])

  const active = useMemo(() => runs.filter(r => r.verdict === 'running'), [runs])
  const activeTopology = useMemo(() => {
    const name = active[0]?.topology ?? runs[0]?.topology
    return topologies.find(t => t.name === name || t.doc?.name === name) ?? topologies[0] ?? null
  }, [active, runs, topologies])
  const streamTone: StatusTone = stream === 'open' ? 'success' : stream === 'connecting' ? 'info' : 'danger'
  const streamLabel = stream === 'open' ? '라이브 연결' : stream === 'connecting' ? '연결 중' : `끊김${streamDetail ? ` (${streamDetail})` : ''}`
  const onRerun = (seed: RerunSeed) => setStartOpen({ scenario_id: seed.scenario_id, topology: seed.topology, profile: seed.profile, bindings: seed.bindings, label: seed.label })

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
          <Button variant="outline" size="sm" onClick={refreshAll} disabled={loading}><RefreshCw size={13} /> 새로고침</Button>
          <Button variant="default" size="sm" disabled={!health || active.length > 0} onClick={() => setStartOpen('new')}
                  title={active.length > 0 ? '진행 중 run 이 있습니다 — 동시에 하나만' : undefined}>
            <Play size={13} /> run 시작
          </Button>
        </div>
      </div>

      <div className="page-scroll flex-1 min-h-0 overflow-auto p-4 flex flex-col gap-4">
        <WorkerFleet topology={activeTopology} targetBuild={active[0]?.live?.target_build ?? runs.find(r => r.target_build)?.target_build ?? null} activeRunId={active[0]?.id ?? null} />
        {active.map(r => (
          <section key={r.id} className="rounded-md border border-border bg-card p-3">
            <RunLivePanel run={r} onEnded={refreshAll} />
          </section>
        ))}
        {active.length === 0 && (
          <EmptyState title="진행 중인 run 이 없습니다"
                      description="[run 시작] 으로 시나리오·토폴로지·부하 프로파일을 고르면 계획 미리보기(역할→풀→워커·용량·시드·Little 검산)를 보고 실행합니다. 진행 중에는 여기서 단계 사다리·종료 조건·지표별 차트·절차 진행·실패 이벤트를 1초 단위로 봅니다."
                      action={<Button variant="default" size="sm" disabled={!health} onClick={() => setStartOpen('new')}><Play size={13} /> run 시작</Button>} />
        )}
        <RunIndex runs={indexRuns} reload={reloadIndex} onRerun={onRerun} picked={picked} setPicked={setPicked} />
      </div>

      {startOpen !== null && (
        <RunStartDialog onClose={() => setStartOpen(null)} initial={startOpen === 'new' ? undefined : startOpen}
                        lastTopologyId={activeTopology?.id ?? null}
                        onStarted={() => { setStartOpen(null); refreshAll() }} />
      )}
    </div>
  )
}
