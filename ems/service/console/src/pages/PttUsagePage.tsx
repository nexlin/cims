// 서비스 > MCPTT 이용 정보 — 기간 집계(mcptt_management_views.md §5). 조회 전용.
//   ① 기간(오늘 · 최근 7일 · 최근 30일 · 직접 지정)  ② 요약 타일(그룹 세션 · 발언 · 총 발언 시간 · 긴급·임박 · 영상 송출 · 평균 개시 시간)
//   ③ 시간대(하루)·일별(여러 날) 발언 추이 + 그룹별 표 + 사용자별 발언 상위  ④ Excel 내려받기(같은 숫자).
//   원천 = oam-svc GET /stats/service/ptt-usage — 1분 롤업 피라미드. 발언 축은 talk_measured 로 미측정 구간을 알린다(0 과 모름을 가른다).
import { useCallback, useEffect, useMemo, useState } from 'react'
import { Download, RefreshCw, Table2, BarChart3 } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { ToggleGroup, ToggleGroupItem } from '@core/components/ui/toggle-group'
import { DataTable, Th, Td, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { useToast } from '@core/components/Toast'
import { Alert, AlertDescription } from '@core/components/ui/alert'
import { authHeaders } from '@core/api/client'
import { statsApi, pttUsagePath, type PttUsageResponse } from '@core/api/stats'

type Range = 'today' | '7d' | '30d' | 'custom'
const RANGES: Array<{ id: Range; label: string }> = [
  { id: 'today', label: '오늘' }, { id: '7d', label: '최근 7일' }, { id: '30d', label: '최근 30일' }, { id: 'custom', label: '직접 지정' },
]
const ymd = (d: Date) => `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`
function rangeOf(r: Range, from: string, to: string): [string, string] {
  const now = new Date()
  if (r === 'today') return [ymd(now), ymd(now)]
  if (r === 'custom') return [from, to || from]
  const back = new Date(now); back.setDate(now.getDate() - (r === '7d' ? 6 : 29))
  return [ymd(back), ymd(now)]
}
/** 초 → "1시간 2분" · "3분 4초" · "5초" */
function fmtDur(sec: number): string {
  if (!sec) return '0초'
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60
  return h ? `${h}시간 ${m}분` : m ? `${m}분 ${s}초` : `${s}초`
}

export default function PttUsagePage() {
  const toast = useToast()
  const [range, setRange] = useState<Range>('today')
  const [from, setFrom] = useState(ymd(new Date()))
  const [to, setTo] = useState(ymd(new Date()))
  const [data, setData] = useState<PttUsageResponse | null>(null)
  const [err, setErr] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)
  const [asTable, setAsTable] = useState(false)
  const [f, t] = rangeOf(range, from, to)

  const load = useCallback(async () => {
    setLoading(true)
    try { setData(await statsApi.pttUsage(f, t)); setErr(null) }
    catch (e: unknown) { setErr(String(e)) } finally { setLoading(false) }
  }, [f, t])
  useEffect(() => { void load() }, [load])

  async function download() {
    try {
      const res = await fetch(`/api/v1${pttUsagePath(f, t, undefined, 'xlsx')}`, { headers: authHeaders() })
      if (!res.ok) throw new Error(`HTTP ${res.status}`)
      const url = URL.createObjectURL(await res.blob())
      const a = document.createElement('a')
      a.href = url; a.download = `mcptt_usage_${f}_${t}.xlsx`; a.click()
      setTimeout(() => URL.revokeObjectURL(url), 1000)
    } catch (e: unknown) { toast.show(`내려받기 실패 — ${String(e)}`, 'err') }
  }

  const s = data?.summary
  const partial = !!s && s.talk_measured < s.talk_coverage_sessions
  const tiles: Array<{ label: string; value: string; talk?: boolean }> = s ? [
    { label: '그룹 세션', value: `${s.sessions}` },
    { label: '발언', value: `${s.turns}`, talk: true },
    { label: '총 발언 시간', value: fmtDur(s.talk_sum_sec), talk: true },
    { label: '긴급·임박 세션', value: `${s.emergency}`, talk: true },
    { label: '영상 송출 세션', value: `${s.video}`, talk: true },
    { label: '평균 개시 시간', value: s.pdd_avg_ms == null ? '—' : `${s.pdd_avg_ms} ms` },
  ] : []

  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="toolbar flex flex-wrap items-center gap-2 border-b border-border px-3 py-2">
        <ToggleGroup type="single" value={range} onValueChange={v => { if (v) setRange(v as Range) }}
          className="shrink-0 justify-start rounded-md bg-muted p-[3px]">
          {RANGES.map(r => <ToggleGroupItem key={r.id} value={r.id}>{r.label}</ToggleGroupItem>)}
        </ToggleGroup>
        {range === 'custom' && (
          <span className="flex items-center gap-1.5">
            <Input type="date" className="h-8 w-40 font-mono" value={from} max={to} onChange={e => setFrom(e.target.value)} />
            <span className="text-muted-foreground">~</span>
            <Input type="date" className="h-8 w-40 font-mono" value={to} min={from} onChange={e => setTo(e.target.value)} />
          </span>
        )}
        <span className="font-mono text-xs text-muted-foreground">{f === t ? f : `${f} ~ ${t}`}</span>
        <Button variant="outline" size="sm" onClick={() => void load()} disabled={loading}><RefreshCw size={14} className={loading ? 'animate-spin' : ''} /> 새로고침</Button>
        <Button size="sm" className="ml-auto" onClick={() => void download()} disabled={!data}><Download size={14} /> Excel 내려받기</Button>
      </div>
      <div className="scroll-fill flex min-h-0 flex-1 flex-col gap-4 overflow-auto p-4">
        {err && <div className="text-sm text-destructive">조회 실패 — {err}</div>}
        {(data?.warning || partial) && (
          <Alert variant="warning">
            <AlertDescription>
              {data?.warning && <div>{data.warning}</div>}
              {partial && <div>발언 축 미측정 — 세션 {s!.talk_coverage_sessions}건 중 {s!.talk_measured}건만 발언을 쟀습니다. 발언·발언 시간·긴급·영상은 잰 세션만의 값입니다(그 구간을 재집계하면 채워진다 — POST /api/v1/stats/calls/rebuild).</div>}
            </AlertDescription>
          </Alert>
        )}
        {!data && !err && <EmptyState title="불러오는 중…" />}
        {data && s && (
          <>
            <div className="grid grid-cols-6 gap-3">
              {tiles.map(tl => (
                <div key={tl.label} className="rounded-md border border-border bg-card px-3 py-2.5">
                  <div className="text-sm text-muted-foreground">{tl.label}</div>
                  <div className="text-2xl font-bold">{tl.value}</div>
                  {tl.talk && partial && <div className="text-xs text-muted-foreground">측정 세션 {s.talk_measured}건 기준</div>}
                </div>
              ))}
            </div>
            <section className="flex flex-col gap-2 rounded-md border border-border bg-card p-3">
              <div className="flex items-center gap-2">
                <span className="text-md font-semibold">{data.unit === '1h' ? '시간대별' : '일별'} 발언 수</span>
                <span className="text-xs text-muted-foreground">막대에 올리면 세션·발언 시간</span>
                <Button variant="ghost" size="sm" className="ml-auto" onClick={() => setAsTable(v => !v)}>
                  {asTable ? <><BarChart3 size={14} /> 차트로 보기</> : <><Table2 size={14} /> 표로 보기</>}
                </Button>
              </div>
              {asTable ? <TrendTable data={data} /> : <TrendBars data={data} />}
            </section>
            <div className="grid grid-cols-2 gap-4">
              <section className="flex min-w-0 flex-col gap-2 rounded-md border border-border bg-card p-3">
                <span className="text-md font-semibold">그룹별 이용</span>
                {data.by_group.length === 0 ? <div className="text-sm text-muted-foreground">—</div> : (
                  <DataTable>
                    <thead><tr><Th>그룹</Th><Th align="right">세션</Th><Th align="right">발언</Th><Th align="right">발언 시간</Th><Th align="right">긴급·임박</Th><Th align="right">영상</Th></tr></thead>
                    <tbody>{data.by_group.map(g => (
                      <tr key={g.id}>
                        <Td><div>{orDash(g.name)}</div><div className="font-mono text-xs text-muted-foreground">{g.id}</div></Td>
                        <Td align="right">{g.sessions}</Td><Td align="right">{g.turns}</Td><Td align="right">{fmtDur(g.talk_sum_sec)}</Td>
                        <Td align="right">{g.emergency}</Td><Td align="right">{g.video}</Td>
                      </tr>))}</tbody>
                  </DataTable>
                )}
              </section>
              <section className="flex min-w-0 flex-col gap-2 rounded-md border border-border bg-card p-3">
                <span className="text-md font-semibold">사용자별 발언 상위 <span className="text-xs font-normal text-muted-foreground">(발언 시간순, 최대 50명)</span></span>
                {data.by_user.length === 0 ? <div className="text-sm text-muted-foreground">—</div> : (
                  <DataTable>
                    <thead><tr><Th>사용자</Th><Th align="right">참여 세션</Th><Th align="right">발언</Th><Th align="right">발언 시간</Th><Th align="right">긴급·임박</Th></tr></thead>
                    <tbody>{data.by_user.map(u => (
                      <tr key={u.id}>
                        <Td><div>{orDash(u.name)}</div><div className="font-mono text-xs text-muted-foreground">{u.id}</div></Td>
                        <Td align="right">{u.sessions}</Td><Td align="right">{u.turns}</Td><Td align="right">{fmtDur(u.talk_sum_sec)}</Td><Td align="right">{u.emergency}</Td>
                      </tr>))}</tbody>
                  </DataTable>
                )}
              </section>
            </div>
          </>
        )}
      </div>
    </div>
  )
}

/** 단일 계열 막대 — 발언 수. 자료 없는 구간은 막대 대신 바닥의 점선 표시(0 과 모름을 가른다). */
function TrendBars({ data }: { data: PttUsageResponse }) {
  const [hover, setHover] = useState<number | null>(null)
  const pts = data.trend
  const H = 180, PAD_L = 36, PAD_B = 22, PAD_T = 8
  const max = useMemo(() => Math.max(1, ...pts.map(p => p.turns)), [pts])
  const ticks = useMemo(() => { const step = Math.max(1, Math.ceil(max / 4)); return [0, step, step * 2, step * 3, step * 4].filter(v => v <= Math.max(max, step)) }, [max])
  const top = ticks[ticks.length - 1] || 1
  if (pts.length === 0) return <div className="text-sm text-muted-foreground">—</div>
  const W = 1000, slot = (W - PAD_L) / pts.length, bw = Math.max(2, slot - 2)
  const y = (v: number) => PAD_T + (H - PAD_T - PAD_B) * (1 - v / top)
  const labelEvery = Math.max(1, Math.ceil(pts.length / 12))
  const lab = (b: string) => (data.unit === '1h' ? b.slice(11, 13) + '시' : b.slice(5))
  const hp = hover != null ? pts[hover] : null
  return (
    <div className="relative">
      <svg viewBox={`0 0 ${W} ${H}`} className="block h-[180px] w-full" preserveAspectRatio="none" role="img"
        aria-label={`${data.unit === '1h' ? '시간대별' : '일별'} 발언 수`} onMouseLeave={() => setHover(null)}>
        {ticks.map(v => (
          <g key={v}>
            <line x1={PAD_L} x2={W} y1={y(v)} y2={y(v)} stroke="var(--border)" strokeWidth={1} vectorEffect="non-scaling-stroke" />
            <text x={PAD_L - 6} y={y(v) + 4} textAnchor="end" fontSize={11} fill="var(--muted-foreground)">{v}</text>
          </g>
        ))}
        {pts.map((p, i) => {
          const x = PAD_L + i * slot + 1
          const h = H - PAD_B - y(p.turns)
          return (
            <g key={p.bucket} onMouseEnter={() => setHover(i)}>
              <rect x={PAD_L + i * slot} y={PAD_T} width={slot} height={H - PAD_T - PAD_B} fill="transparent" />
              {p.missing
                ? <line x1={x} x2={x + bw} y1={H - PAD_B - 1} y2={H - PAD_B - 1} stroke="var(--muted-foreground)" strokeDasharray="3 3" vectorEffect="non-scaling-stroke" />
                : p.turns > 0 && (h > 4 && bw > 8
                    ? <path d={`M${x},${H - PAD_B} v${-(h - 4)} q0,-4 4,-4 h${bw - 8} q4,0 4,4 v${h - 4} z`}
                        fill="var(--chart-1)" opacity={hover == null || hover === i ? 1 : 0.55} />
                    : <rect x={x} y={H - PAD_B - h} width={bw} height={h} fill="var(--chart-1)" opacity={hover == null || hover === i ? 1 : 0.55} />)}
              {i % labelEvery === 0 && <text x={x + bw / 2} y={H - 6} textAnchor="middle" fontSize={11} fill="var(--muted-foreground)">{lab(p.bucket)}</text>}
            </g>
          )
        })}
      </svg>
      {hp && (
        <div className="pointer-events-none absolute top-0 rounded-md border border-border bg-card px-2.5 py-1.5 text-xs shadow-md"
          style={{ left: `${Math.min(85, ((hover! + 0.5) / pts.length) * 100)}%` }}>
          <div className="font-mono font-semibold">{hp.bucket}</div>
          {hp.missing ? <div className="text-muted-foreground">자료 없음</div> : <>
            <div>발언 <b>{hp.turns}</b></div><div>세션 {hp.sessions}</div><div>발언 시간 {fmtDur(hp.talk_sum_sec)}</div>
          </>}
        </div>
      )}
    </div>
  )
}

function TrendTable({ data }: { data: PttUsageResponse }) {
  return (
    <DataTable>
      <thead><tr><Th>구간</Th><Th align="right">세션</Th><Th align="right">발언</Th><Th align="right">발언 시간</Th></tr></thead>
      <tbody>{data.trend.map(p => (
        <tr key={p.bucket}>
          <Td mono>{p.bucket}</Td>
          {p.missing ? <Td align="right" className="text-muted-foreground" colSpan={3}>자료 없음</Td> : <>
            <Td align="right">{p.sessions}</Td><Td align="right">{p.turns}</Td><Td align="right">{fmtDur(p.talk_sum_sec)}</Td>
          </>}
        </tr>))}</tbody>
    </DataTable>
  )
}
