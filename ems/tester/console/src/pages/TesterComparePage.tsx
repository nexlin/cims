// 시험 > 비교 — run 두 개 이상을 겹쳐 회귀 판정(첫 run 이 기준). 같은 시나리오·다른 대상 빌드(target_build)가 본래 용도.
// 판정은 컨트롤러 GET /runs/compare (비율 지표 0.5 pt / 그 외 5 % 허용) — CLI 와 같은 결과.
import { useCallback, useEffect, useMemo, useState } from 'react'
import { useSearchParams } from 'react-router-dom'
import { GitCompareArrows, X, Plus, ArrowUp, ArrowDown } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { DataTable, Th, Td, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { testerApi, type RunRow, type CompareResult } from '@tester/api/tester'
import { fmtTime, fmtNum, SUMMARY_ROWS, VERDICT_TONE, VERDICT_LABEL } from '@tester/lib/fmt'

const LABEL: Record<string, string> = Object.fromEntries(SUMMARY_ROWS.map(([k, l]) => [k, l]))
Object.assign(LABEL, { rrd_ms_p95: 'RRD p95 ms', srd_ms_p95: 'SRD p95 ms', sdd_ms_p95: 'SDD p95 ms', jitter_ms_p95: '지터 p95 ms' })

export default function TesterComparePage() {
  const [params, setParams] = useSearchParams()
  const ids = useMemo(() => (params.get('ids') ?? '').split(',').filter(Boolean), [params])
  const [runs, setRuns] = useState<RunRow[]>([])
  const [result, setResult] = useState<CompareResult | null>(null)
  const [err, setErr] = useState<string | null>(null)
  const [adding, setAdding] = useState('')

  useEffect(() => { testerApi.runs().then(r => setRuns(r.runs.filter(x => x.verdict !== 'running'))).catch(() => {}) }, [])

  const load = useCallback(async () => {
    if (ids.length < 2) { setResult(null); return }
    try { setResult(await testerApi.compare(ids)); setErr(null) } catch (e) { setErr(String(e)); setResult(null) }
  }, [ids])
  useEffect(() => { load() }, [load])

  const setIds = (next: string[]) => setParams(next.length ? { ids: next.join(',') } : {})
  const add = (id: string) => { if (id && !ids.includes(id)) setIds([...ids, id]); setAdding('') }
  const remove = (id: string) => setIds(ids.filter(x => x !== id))
  const makeBase = (id: string) => setIds([id, ...ids.filter(x => x !== id)])

  const candidates = runs.filter(r => !ids.includes(r.id))

  return (
    <div className="flex h-full flex-col">
      <div className="toolbar flex flex-wrap items-center gap-2.5 border-b border-border bg-muted px-4 py-3">
        <span className="whitespace-nowrap text-md font-semibold text-foreground">비교</span>
        <span className="text-sm text-muted-foreground">첫 run 이 기준 — 지표별 차이와 회귀(나빠짐)를 표시</span>
        {result && (
          <Badge variant={result.regressions > 0 ? 'dangerSoft' : 'successSoft'}>
            {result.regressions > 0 ? `회귀 ${result.regressions}건` : '회귀 없음'}
          </Badge>
        )}
        {result && !result.same_scenario && <Badge variant="warningSoft">시나리오가 서로 다름 — 지표 비교의 뜻이 약하다</Badge>}
        {err && <span className="text-sm text-destructive">{err}</span>}
        <div className="ml-auto flex items-center gap-2">
          <Select value={adding} onValueChange={add}>
            <SelectTrigger className="w-[380px]"><SelectValue placeholder="run 추가…" /></SelectTrigger>
            <SelectContent>
              {candidates.map(r => (
                <SelectItem key={r.id} value={r.id}><span className="font-mono">{r.id}</span> · {r.scenario_id} · {fmtTime(r.started_at, true)} · {r.target_build ?? ''}</SelectItem>
              ))}
            </SelectContent>
          </Select>
          <Button variant="outline" size="sm" onClick={() => setIds([])} disabled={ids.length === 0}><X size={13} /> 비우기</Button>
        </div>
      </div>

      <div className="page-scroll flex-1 min-h-0 overflow-auto p-4 flex flex-col gap-4">
        {ids.length < 2 && (
          <EmptyState title={ids.length === 0 ? '비교할 run 을 고르십시오' : 'run 을 하나 더 추가하십시오'}
                      description="[실행] 색인에서 체크해 [비교] 를 누르거나, 위에서 하나씩 추가합니다. 같은 시나리오를 다른 대상 빌드로 돌린 run 을 겹치는 것이 본래 용도입니다."
                      action={<span className="inline-flex items-center gap-1 text-sm text-muted-foreground"><Plus size={13} /> 위 [run 추가…]</span>} />
        )}

        {result && (
          <>
            <section className="flex flex-col gap-1.5">
              <h3 className="text-sm font-semibold text-muted-foreground">대상 run</h3>
              <DataTable>
                <thead><tr><Th width={60}>기준</Th><Th>run</Th><Th>라벨</Th><Th>시나리오</Th><Th>토폴로지 / 프로파일</Th><Th>시작</Th><Th>대상 빌드</Th><Th>판정</Th><Th width={80} /></tr></thead>
                <tbody>
                  {result.runs.map((r, i) => (
                    <tr key={r.id ?? i}>
                      <Td>{i === 0 ? <Badge variant="brandSoft">기준</Badge> : <Button variant="ghost" size="sm" onClick={() => makeBase(r.id!)}>기준으로</Button>}</Td>
                      <Td mono>{r.id}</Td>
                      <Td>{orDash(r.label)}</Td>
                      <Td mono>{r.missing ? <span className="text-destructive">없음</span> : r.scenario_id}</Td>
                      <Td>{r.missing ? '—' : `${r.topology} / ${r.profile ?? '단발'}`}</Td>
                      <Td mono>{fmtTime(r.started_at, true)}</Td>
                      <Td className="text-xs">{orDash(r.target_build)}</Td>
                      <Td>{r.verdict ? <Badge variant={VERDICT_TONE[r.verdict] ?? 'neutralSoft'}>{VERDICT_LABEL[r.verdict] ?? r.verdict}</Badge> : '—'}</Td>
                      <Td><Button variant="ghost" size="iconSm" onClick={() => remove(r.id!)} aria-label="제거"><X size={13} /></Button></Td>
                    </tr>
                  ))}
                </tbody>
              </DataTable>
            </section>

            <section className="flex flex-col gap-1.5">
              <h3 className="text-sm font-semibold text-muted-foreground"><GitCompareArrows size={13} className="mr-1 inline" /> 지표 비교 <span className="font-normal">— Δ 는 기준 대비, 붉은 표시가 회귀</span></h3>
              {result.metrics.length === 0 ? <EmptyState title="비교 가능한 지표가 없습니다" /> : (
                <DataTable>
                  <thead>
                    <tr>
                      <Th>지표</Th><Th width={40}>방향</Th>
                      {result.runs.map((r, i) => <Th key={r.id ?? i} align="right">{i === 0 ? '기준 ' : ''}<span className="font-mono">{r.id}</span></Th>)}
                    </tr>
                  </thead>
                  <tbody>
                    {result.metrics.map(m => (
                      <tr key={m.metric}>
                        <Td>{LABEL[m.metric] ?? m.metric}</Td>
                        <Td>{m.direction === 'up' ? <ArrowUp size={13} className="text-muted-foreground" aria-label="클수록 좋음" /> : <ArrowDown size={13} className="text-muted-foreground" aria-label="작을수록 좋음" />}</Td>
                        {m.values.map((v, i) => (
                          <Td key={i} align="right" mono className={m.regression[i] ? 'text-destructive font-semibold' : ''}>
                            {fmtNum(v)}
                            {i > 0 && m.delta[i] != null && (
                              <span className={`ml-1 text-xs ${m.regression[i] ? 'text-destructive' : 'text-muted-foreground'}`}>
                                ({m.delta[i]! > 0 ? '+' : ''}{fmtNum(m.delta[i])})
                              </span>
                            )}
                          </Td>
                        ))}
                      </tr>
                    ))}
                  </tbody>
                </DataTable>
              )}
            </section>

            <section className="flex flex-col gap-1.5">
              <h3 className="text-sm font-semibold text-muted-foreground">기대치 판정</h3>
              <DataTable>
                <thead><tr><Th>run</Th><Th align="right">PASS</Th><Th align="right">FAIL</Th><Th>실패 항목</Th><Th>중단 사유</Th></tr></thead>
                <tbody>
                  {result.runs.map((r, i) => {
                    const er = r.expect_results ?? []
                    const fails = er.filter(x => !x.ok)
                    return (
                      <tr key={r.id ?? i}>
                        <Td mono>{r.id}</Td>
                        <Td align="right" mono>{er.length - fails.length}</Td>
                        <Td align="right" mono className={fails.length ? 'text-destructive' : ''}>{fails.length}</Td>
                        <Td className="text-xs">{fails.map(f => `#${f.step + 1} ${f.kind} ${f.metric}`).join(', ') || '—'}</Td>
                        <Td>{orDash(r.stop_reason)}</Td>
                      </tr>
                    )
                  })}
                </tbody>
              </DataTable>
            </section>
          </>
        )}
      </div>
    </div>
  )
}
