// run 색인 — 필터 바(라벨/id 검색·시나리오·대상 빌드·판정 칩·부하 run 만·기간) + 날짜 그룹 + 행 액션(재실행·결과) + 체크 → 하단 고정 비교 바
// (test_instrument.md §7 실행 라이브 ③). 필터는 컨트롤러 색인 필터(GET /runs?…)로 — 검색·칩은 즉시, 서버 재조회는 300 ms 디바운스.
import { useCallback, useEffect, useMemo, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { FileText, RotateCw, GitCompareArrows, X } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Input } from '@core/components/ui/input'
import { Checkbox } from '@core/components/ui/checkbox'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { DataTable, Th, Td, TrLink, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { testerApi, type RunRow, type RunFilters, type Verdict } from '@tester/api/tester'
import { fmtTime, fmtDuration, fmtPct, fmtNum, VERDICT_TONE, VERDICT_LABEL } from '@tester/lib/fmt'

const ALL = '__all__'
const VERDICTS: (Verdict | '')[] = ['', 'pass', 'fail', 'aborted', 'error']
const RANGES: { id: string; label: string; since?: string }[] = [
  { id: '7d', label: '최근 7일', since: '7d' }, { id: '1d', label: '오늘', since: '1d' }, { id: '30d', label: '최근 30일', since: '30d' }, { id: 'all', label: '전체' },
]

export interface RerunSeed { scenario_id: string; topology: string; profile?: string | null; bindings?: Record<string, unknown>; label?: string | null }

export default function RunIndex({ runs, reload, onRerun, picked, setPicked }: {
  runs: RunRow[]
  reload: (f: RunFilters) => void
  onRerun: (seed: RerunSeed) => void
  picked: string[]
  setPicked: (ids: string[]) => void
}) {
  const nav = useNavigate()
  const [q, setQ] = useState('')
  const [scenario, setScenario] = useState(ALL)
  const [build, setBuild] = useState(ALL)
  const [verdict, setVerdict] = useState<Verdict | ''>('')
  const [loadOnly, setLoadOnly] = useState(false)
  const [range, setRange] = useState('7d')
  const [options, setOptions] = useState<{ scenarios: string[]; builds: string[] }>({ scenarios: [], builds: [] })

  // 선택지는 전체 색인에서(필터 전) 한 번
  useEffect(() => {
    testerApi.runs(500).then(r => setOptions({
      scenarios: [...new Set(r.runs.map(x => x.scenario_id))].sort(),
      builds: [...new Set(r.runs.map(x => x.target_build).filter((x): x is string => !!x))].sort(),
    })).catch(() => {})
  }, [])

  const filters = useMemo<RunFilters>(() => ({
    scenario: scenario === ALL ? undefined : scenario, build: build === ALL ? undefined : build,
    verdict: verdict || undefined, load: loadOnly || undefined, since: RANGES.find(r => r.id === range)?.since,
    label: q.trim() || undefined, limit: 300,
  }), [scenario, build, verdict, loadOnly, range, q])
  useEffect(() => { const id = window.setTimeout(() => reload(filters), 300); return () => window.clearTimeout(id) }, [filters, reload])

  const groups = useMemo(() => {
    const m = new Map<string, RunRow[]>()
    for (const r of runs) { const d = (r.started_at ?? '').slice(0, 10); const a = m.get(d) ?? []; a.push(r); m.set(d, a) }
    return [...m.entries()]
  }, [runs])
  const counts = useMemo(() => { const c: Record<string, number> = {}; for (const r of runs) c[r.verdict] = (c[r.verdict] ?? 0) + 1; return c }, [runs])

  const toggle = useCallback((id: string) => setPicked(picked.includes(id) ? picked.filter(x => x !== id) : [...picked, id]), [picked, setPicked])
  const rerun = (r: RunRow) => onRerun({ scenario_id: r.scenario_id, topology: r.topology, profile: r.profile ?? null, label: r.label ?? null })

  return (
    <section className="flex flex-col gap-2">
      <div className="flex flex-wrap items-center gap-2">
        <h3 className="text-sm font-semibold text-muted-foreground">run 색인 <span className="font-normal">— 행 = 결과 · 체크 = 비교 · <RotateCw size={11} className="inline" /> = 같은 조건으로 재실행</span></h3>
      </div>
      <div className="flex flex-wrap items-center gap-2">
        <Input value={q} onChange={e => setQ(e.target.value)} placeholder="라벨·run id·시나리오 검색" className="h-[28px] w-[200px] text-sm" />
        <Select value={scenario} onValueChange={setScenario}>
          <SelectTrigger className="h-[28px] w-[200px] text-sm"><SelectValue /></SelectTrigger>
          <SelectContent><SelectItem value={ALL}>시나리오 전체</SelectItem>{options.scenarios.map(s => <SelectItem key={s} value={s}>{s}</SelectItem>)}</SelectContent>
        </Select>
        <Select value={build} onValueChange={setBuild}>
          <SelectTrigger className="h-[28px] w-[190px] text-sm"><SelectValue /></SelectTrigger>
          <SelectContent><SelectItem value={ALL}>대상 빌드 전체</SelectItem>{options.builds.map(b => <SelectItem key={b} value={b}>{b}</SelectItem>)}</SelectContent>
        </Select>
        <div className="flex items-center gap-1">
          {VERDICTS.map(v => (
            <button key={v || 'all'} onClick={() => setVerdict(v)}
                    className={`h-6 rounded-sm border px-2 text-xs transition-colors ${verdict === v ? 'border-primary bg-primary text-primary-foreground' : 'border-border text-muted-foreground hover:bg-accent'}`}>
              {v ? VERDICT_LABEL[v] : '전체'}{v ? (counts[v] ? ` ${counts[v]}` : '') : ` ${runs.length}`}
            </button>
          ))}
        </div>
        <label className="inline-flex items-center gap-1.5 text-xs text-muted-foreground"><Checkbox checked={loadOnly} onCheckedChange={v => setLoadOnly(v === true)} /> 부하 run 만</label>
        <Select value={range} onValueChange={setRange}>
          <SelectTrigger className="h-[28px] w-[120px] text-sm"><SelectValue /></SelectTrigger>
          <SelectContent>{RANGES.map(r => <SelectItem key={r.id} value={r.id}>{r.label}</SelectItem>)}</SelectContent>
        </Select>
      </div>
      {runs.length === 0 ? <EmptyState title="조건에 맞는 run 이 없습니다" /> : (
        <DataTable>
          <thead>
            <tr>
              <Th width={32} /><Th width={70}>판정</Th><Th width={70}>시각</Th><Th>시나리오 / 라벨</Th><Th>프로파일</Th><Th>대상 빌드</Th>
              <Th align="right">시도</Th><Th align="right">SER</Th><Th align="right">SRD p95</Th><Th align="right">손실</Th><Th align="right">소요</Th><Th width={90} />
            </tr>
          </thead>
          <tbody>
            {groups.map(([day, rows]) => (
              <GroupRows key={day} day={day} rows={rows} picked={picked} toggle={toggle} rerun={rerun}
                         open={id => nav(`/test/results?id=${encodeURIComponent(id)}`)} />
            ))}
          </tbody>
        </DataTable>
      )}
      {picked.length > 0 && (
        <div className="sticky bottom-0 z-10 flex flex-wrap items-center gap-2 rounded-md border border-border bg-card px-3 py-2 shadow-md">
          <span className="text-sm">선택 {picked.length}건 <span className="text-muted-foreground">· 첫 선택 <span className="font-mono">{picked[0]}</span> 이 기준</span></span>
          <Button variant="default" size="sm" disabled={picked.length < 2} onClick={() => nav(`/test/compare?ids=${picked.map(encodeURIComponent).join(',')}`)}><GitCompareArrows size={13} /> 비교</Button>
          <Button variant="outline" size="sm" onClick={() => setPicked([])}><X size={13} /> 선택 해제</Button>
        </div>
      )}
    </section>
  )
}

function GroupRows({ day, rows, picked, toggle, rerun, open }: {
  day: string; rows: RunRow[]; picked: string[]; toggle: (id: string) => void; rerun: (r: RunRow) => void; open: (id: string) => void
}) {
  return (
    <>
      <tr><Td className="bg-muted text-xs font-semibold text-muted-foreground" colSpan={12}>{day} <span className="font-normal">· {rows.length}건</span></Td></tr>
      {rows.map(r => (
        <TrLink key={r.id} selected={picked.includes(r.id)} onClick={() => open(r.id)} className="group">
          <Td onClick={e => e.stopPropagation()}>
            <Checkbox checked={picked.includes(r.id)} onCheckedChange={() => toggle(r.id)} disabled={r.verdict === 'running'} aria-label="비교 대상" />
          </Td>
          <Td><Badge variant={VERDICT_TONE[r.verdict] ?? 'neutralSoft'}>{VERDICT_LABEL[r.verdict] ?? r.verdict}</Badge></Td>
          <Td mono>{fmtTime(r.started_at)}</Td>
          <Td>
            <span className="font-mono">{r.scenario_id}</span>
            {r.label && <span className="ml-1.5 text-muted-foreground">{r.label}</span>}
            {r.stop_reason && <Badge variant="warningSoft" className="ml-1.5" title={r.stop_reason}>사유</Badge>}
            <span className="ml-1.5 font-mono text-xs text-muted-foreground">{r.id}</span>
          </Td>
          <Td className="text-xs">{r.profile ?? <span className="text-muted-foreground">단발</span>}</Td>
          <Td className="text-xs">{orDash(r.target_build)}</Td>
          <Td align="right" mono>{fmtNum(r.summary?.attempts, 0)}</Td>
          <Td align="right" mono>{fmtPct(r.summary?.ser_pct)}</Td>
          <Td align="right" mono>{r.summary?.srd_ms_p95 != null ? `${fmtNum(r.summary.srd_ms_p95, 0)} ms` : '—'}</Td>
          <Td align="right" mono>{fmtPct(r.summary?.rtp_loss_pct)}</Td>
          <Td align="right" mono>{fmtDuration(r.started_at, r.ended_at)}</Td>
          <Td onClick={e => e.stopPropagation()}>
            <div className="flex items-center gap-0.5 opacity-60 group-hover:opacity-100">
              <Button variant="ghost" size="iconSm" title="같은 조건으로 시작 창 열기" onClick={() => rerun(r)}><RotateCw size={13} /></Button>
              <Button variant="ghost" size="iconSm" title="결과" onClick={() => open(r.id)}><FileText size={13} /></Button>
            </div>
          </Td>
        </TrLink>
      ))}
    </>
  )
}
