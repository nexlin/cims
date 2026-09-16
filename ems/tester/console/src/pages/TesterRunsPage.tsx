// 시험 > 실행 — 계측기 컨트롤러 상태 + run 색인 + 라이브 스트림(게이트웨이 SSE 통과 확인).
// A 단계: run 시작은 501(B 단계) — 버튼은 상태를 그대로 보여 준다.
import { useCallback, useEffect, useRef, useState } from 'react'
import { RefreshCw } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { DataTable, Th, Td, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { StatusDot, type StatusTone } from '@core/components/custom/status-dot'
import { useToast } from '@core/components/Toast'
import { testerApi, openTesterStream, type TesterHealth, type RunRow, type TesterStreamFrame } from '@tester/api/tester'

type StreamState = 'connecting' | 'open' | 'closed'

const VERDICT_TONE: Record<RunRow['verdict'], 'infoSoft' | 'successSoft' | 'dangerSoft' | 'warningSoft' | 'neutralSoft'> = {
  running: 'infoSoft', pass: 'successSoft', fail: 'dangerSoft', aborted: 'warningSoft', error: 'dangerSoft',
}

export default function TesterRunsPage() {
  const { show } = useToast()
  const [health, setHealth] = useState<TesterHealth | null>(null)
  const [healthErr, setHealthErr] = useState<string | null>(null)
  const [runs, setRuns] = useState<RunRow[]>([])
  const [loading, setLoading] = useState(true)
  const [stream, setStream] = useState<StreamState>('connecting')
  const [streamDetail, setStreamDetail] = useState<string>('')
  const [frames, setFrames] = useState<TesterStreamFrame[]>([])
  const framesRef = useRef<TesterStreamFrame[]>([])

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
        framesRef.current = [f, ...framesRef.current].slice(0, 50)
        setFrames(framesRef.current)
        if (f.stream === 'runs') load()
      },
      (s, d) => { setStream(s); setStreamDetail(d ?? '') },
    )
    return close
  }, [load])

  const streamTone: StatusTone = stream === 'open' ? 'success' : stream === 'connecting' ? 'info' : 'danger'
  const streamLabel = stream === 'open' ? '라이브 연결' : stream === 'connecting' ? '연결 중' : `끊김${streamDetail ? ` (${streamDetail})` : ''}`

  return (
    <div className="flex h-full flex-col">
      <div className="toolbar flex flex-wrap items-center gap-2.5 border-b border-border bg-muted px-4 py-3">
        <span className="whitespace-nowrap text-md font-semibold text-foreground">실행</span>
        <StatusDot tone={streamTone} label={streamLabel} />
        {health && (
          <span className="text-sm text-muted-foreground">
            oam-cims-tester {health.version} · 단계 {health.phase} · 시나리오 {health.scenarios} · 프로파일 {health.profiles} · 토폴로지 {health.topologies} · 워커 {health.workers}
          </span>
        )}
        {healthErr && <span className="text-sm text-destructive">{healthErr}</span>}
        <div className="ml-auto flex items-center gap-2">
          <Button variant="outline" size="sm" onClick={load} disabled={loading}><RefreshCw size={13} /> 새로고침</Button>
          <Button size="sm" disabled={!health}
                  onClick={async () => {
                    try { await testerApi.startRun({ scenario_id: 'VOLTE-CALL-BASIC' }); show('run 시작', 'ok'); load() }
                    catch (e) { show(String(e), 'err') }
                  }}>
            run 시작
          </Button>
        </div>
      </div>

      <div className="page-scroll flex-1 min-h-0 overflow-auto p-4 flex flex-col gap-4">
        <section className="flex flex-col gap-2">
          <h3 className="text-sm font-semibold text-muted-foreground">run 색인</h3>
          {runs.length === 0 ? (
            <EmptyState title="실행 이력이 없습니다"
                        description="run 실행은 워커(cims-tester-worker)와 오케스트레이터가 들어오는 B 단계에서 열립니다. 지금은 계약·시나리오·토폴로지만 다룹니다." />
          ) : (
            <DataTable>
              <thead>
                <tr>
                  <Th>run</Th><Th>시나리오</Th><Th>토폴로지</Th><Th>프로파일</Th><Th>시작</Th><Th>종료</Th><Th>판정</Th>
                </tr>
              </thead>
              <tbody>
                {runs.map(r => (
                  <tr key={r.id}>
                    <Td mono>{r.id}</Td>
                    <Td mono>{r.scenario_id}</Td>
                    <Td>{orDash(r.topology)}</Td>
                    <Td>{orDash(r.profile)}</Td>
                    <Td mono>{r.started_at}</Td>
                    <Td mono>{orDash(r.ended_at)}</Td>
                    <Td><Badge variant={VERDICT_TONE[r.verdict]}>{r.verdict}</Badge></Td>
                  </tr>
                ))}
              </tbody>
            </DataTable>
          )}
        </section>

        <section className="flex flex-col gap-2">
          <h3 className="text-sm font-semibold text-muted-foreground">라이브 스트림 (최근 50)</h3>
          {frames.length === 0 ? (
            <EmptyState title="수신한 프레임이 없습니다"
                        description="연결 직후 hello 프레임이 와야 합니다. 오지 않으면 게이트웨이 SSE 통과(text/event-stream 청크 passthrough)를 확인하십시오." />
          ) : (
            <DataTable>
              <thead><tr><Th width={120}>stream</Th><Th>record</Th></tr></thead>
              <tbody>
                {frames.map((f, i) => (
                  <tr key={i}>
                    <Td><Badge variant={f.stream === 'hello' ? 'infoSoft' : 'neutralSoft'}>{f.stream}</Badge></Td>
                    <Td mono className="whitespace-pre-wrap break-all">{JSON.stringify(f.record)}</Td>
                  </tr>
                ))}
              </tbody>
            </DataTable>
          )}
        </section>
      </div>
    </div>
  )
}
