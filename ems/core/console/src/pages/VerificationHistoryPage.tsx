import { Button } from '@core/components/ui/button'
import { useConfirm } from '../components/custom/confirm'
import { AlertTriangle, ArrowLeft, ArrowRight, ChevronDown, FileText, RotateCw } from 'lucide-react'
import { useState, useEffect, useCallback, useMemo } from 'react'

import {
  verifyApi,
  type RunHistoryItem, type RunDetail, type RunDetailItem,
  type VerifyStatus,
  type RunsStatsResponse,
} from '../api/verification'
import {
  VerificationPrintReport,
  type ReportStage, type ReportItem, type ItemStatus as ReportItemStatus,
} from '../components/VerificationPrintReport'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { NONE, fromSel, toSel } from '@core/components/custom/select-value'
import { useToast } from '@core/components/Toast'

const STAGE_DESC: Record<number, string> = {
  1: 'lint / format / unit test',
  2: 'preflight + cmake build',
  3: 'configure → start dev → 1콜 VoIP/PTT',
  4: 'tarball + manifest hash',
  5: 'TB-CSC → Test-agent → mgmt-server → service-server 체인',
  6: 'VoLTE/PTT 음성·영상 (배포본 대상)',
}

/** RunDetail.items → PrintReport 가 받는 ReportStage[] 로 변환. */
function runDetailToReportStages(detail: RunDetail): ReportStage[] {
  const byStage = new Map<number, ReportItem[]>()
  for (const it of detail.items) {
    const s = it.stage || 0
    if (!byStage.has(s)) byStage.set(s, [])
    byStage.get(s)!.push({
      id:        it.id,
      name:      it.name,
      desc:      it.detail || '',
      status:    (it.status as ReportItemStatus) || 'PENDING',
      elapsedMs: it.elapsed_ms || 0,
      isGroup:   it.is_group,
      parent:    it.parent_id || undefined,
    })
  }
  // 모든 stage 1~6 표시 (없는 stage 도 빈 항목)
  const stages: ReportStage[] = []
  for (let n = 1; n <= 6; n++) {
    stages.push({
      num: n, id: `S${n}`,
      title: STAGE_LABEL[n] || `Stage ${n}`,
      desc:  STAGE_DESC[n] || '',
      items: byStage.get(n) || [],
    })
  }
  return stages
}

// ─────────────────────────────────────────────────────────────
// 검증 이력 페이지 — /release/verify-history
//   list + detail (modal) + 필터 (stage/verdict/limit)
// ─────────────────────────────────────────────────────────────

const STAGE_LABEL: Record<number, string> = {
  1: '정적 검사', 2: '빌드', 3: '스모크',
  4: '패키지화', 5: '로컬 배포', 6: '통합 검증',
}

const VERDICT_COLOR: Record<string, string> = {
  PASS: 'var(--cims-success)',
  FAIL: 'var(--destructive)',
  UNKNOWN: 'var(--muted-foreground)',
}

const STATUS_COLOR: Record<VerifyStatus, string> = {
  PASS:    'var(--cims-success)',
  FAIL:    'var(--destructive)',
  SKIP:    'var(--muted-foreground)',
  BLOCKED: 'var(--cims-warning)',
  RUNNING: 'var(--cims-info)',
  PENDING: 'var(--muted-foreground)',
  UNKNOWN: 'var(--muted-foreground)',
}

function fmtDate(iso: string | null): string {
  if (!iso) return '-'
  const d = new Date(iso.replace(' ', 'T'))
  if (isNaN(d.getTime())) return iso
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
}

function fmtDuration(ms: number): string {
  if (!ms || ms < 0) return '-'
  if (ms < 1000) return `${ms}ms`
  const s = Math.floor(ms / 1000)
  if (s < 60) return `${s}s`
  const m = Math.floor(s / 60)
  return `${m}m ${s % 60}s`
}

// id = ms timestamp (e.g., 1778125658339). 가독성 위해 YYMMDD-HHMMSS 로 표시.
// id 자체는 backend API 호출용 그대로 보존.
function fmtRunIdShort(id: number): string {
  if (!id) return '-'
  const d = new Date(id)
  if (isNaN(d.getTime())) return String(id)
  const pad = (n: number) => String(n).padStart(2, '0')
  const yy = String(d.getFullYear()).slice(2)
  return `${yy}${pad(d.getMonth() + 1)}${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`
}

function scopeLabel(scope: string): string {
  if (!scope) return '-'
  const m = /^stage(\d+)$/.exec(scope)
  if (m) return `S${m[1]}: ${STAGE_LABEL[Number(m[1])] || scope}`
  if (scope.startsWith('preset:')) return `preset: ${scope.slice(7)}`
  return scope
}

// ─────────────────────────────────────────────────────────────
// list 표 행
// ─────────────────────────────────────────────────────────────
function RunListRow({ run, onClick }: { run: RunHistoryItem; onClick: () => void }) {
  const t = run.totals || {}
  return (
    <tr className="cursor-pointer" onClick={onClick}>
      <td className="border-b border-border px-3 py-2 font-mono text-xs" title={`run_id=${run.id}`}>
        {fmtRunIdShort(run.id)}
      </td>
      <td className="border-b border-border px-3 py-2">{fmtDate(run.started_at)}</td>
      <td className="border-b border-border px-3 py-2">{scopeLabel(run.scope)}</td>
      <td className="border-b border-border px-3 py-2 font-semibold" style={{ color: VERDICT_COLOR[run.verdict] || 'var(--foreground)' }}>
        {run.verdict}
      </td>
      <td className="border-b border-border px-3 py-2 text-sm text-muted-foreground">
        {(t.pass ?? 0)} / {(t.fail ?? 0)} / {(t.skip ?? 0)}
        {t.blocked ? ` / ${t.blocked}` : ''}
        <span className="text-muted-foreground ml-1.5">(P/F/S)</span>
      </td>
      <td className="border-b border-border px-3 py-2">{fmtDuration(run.elapsed_ms)}</td>
      <td className="border-b border-border px-3 py-2 font-mono text-xs text-muted-foreground">
        {run.git_branch && <span>{run.git_branch}@</span>}
        <span>{run.git_sha || '-'}</span>
      </td>
      <td className="border-b border-border px-3 py-2 font-mono text-xs text-muted-foreground">
        {run.pkg_manifest_hash ? run.pkg_manifest_hash.slice(0, 10) + '…' : '-'}
      </td>
      <td className="border-b border-border px-3 py-2 text-xs text-muted-foreground">{run.trigger}</td>
    </tr>
  )
}

// ─────────────────────────────────────────────────────────────
// detail modal
// ─────────────────────────────────────────────────────────────
function DetailModal({ run, onClose, onDelete }: {
  run: RunDetail
  onClose: () => void
  onDelete: (id: number) => void
}) {
  const confirm = useConfirm()
  // 부모/자식 트리로 그룹핑
  const grouped = useMemo(() => {
    const parents: RunDetailItem[] = []
    const childrenByParent: Record<string, RunDetailItem[]> = {}
    for (const it of run.items) {
      if (it.parent_id) {
        ;(childrenByParent[it.parent_id] ||= []).push(it)
      } else {
        parents.push(it)
      }
    }
    return { parents, childrenByParent }
  }, [run.items])

  return (
    <div className="verify-history-modal-backdrop fixed inset-0 z-[1000] flex items-center justify-center bg-black/40" onClick={onClose}>
      <div className="verify-history-modal flex max-h-[90vh] w-[90vw] max-w-[1100px] flex-col overflow-auto rounded-md bg-card" onClick={e => e.stopPropagation()}>
        <header className="flex items-center justify-between border-b border-border px-5 py-4">
          <div>
            <div className="text-xl font-bold" title={`run_id=${run.id}`}>
              회차 {fmtRunIdShort(run.id)}
            </div>
            <div className="text-sm text-muted-foreground mt-1">
              {fmtDate(run.started_at)} ~ {fmtDate(run.finished_at)} ({fmtDuration(run.elapsed_ms)})
            </div>
          </div>
          <div>
            <Button variant="outline" onClick={() => window.print()} title="이 회차를 PDF 보고서로 인쇄">
              <FileText size={13} className="inline align-[-2px]" /> PDF 인쇄
            </Button>
            <Button variant="destructive" className="ml-2" onClick={() => void (async () => {
              if (await confirm({ title: '회차 삭제', tone: 'danger', confirmLabel: '삭제',
                body: `회차 ${fmtRunIdShort(run.id)} 를 삭제할까요? 이 작업은 되돌릴 수 없습니다.` })) {
                onDelete(run.id)
              }
            })()}>삭제</Button>
            <Button variant="default" className="ml-2" onClick={onClose}>닫기</Button>
          </div>
        </header>

        {/* 인쇄 시에만 노출되는 보고서 */}
        <VerificationPrintReport
          stages={runDetailToReportStages(run)}
          resumeStage={1}
          meta={{
            issuedAt:    fmtDate(run.started_at),
            host:        run.host,
            gitBranch:   run.git_branch,
            gitSha:      run.git_sha,
            pkgManifest: run.pkg_manifest_hash || '-',
            runId:       run.id,
          }}
        />

        <div className="grid grid-cols-2 gap-3 px-5 py-3">
          <Field label="Scope" value={scopeLabel(run.scope)} />
          <Field label="Verdict" value={
            <span style={{ color: VERDICT_COLOR[run.verdict], fontWeight: 700 }}>{run.verdict}</span>
          } />
          <Field label="Trigger" value={run.trigger} />
          <Field label="Host" value={run.host || '-'} />
          <Field label="Git" value={`${run.git_branch}@${run.git_sha}`} />
          <Field label="Pkg manifest hash" value={
            <span className="font-mono text-xs">
              {run.pkg_manifest_hash || '-'}
            </span>
          } />
          <Field label="Report path" value={
            <span className="font-mono text-xs break-all">
              {run.report_path || '-'}
            </span>
          } />
          <Field label="Selected" value={
            run.selected_ids.length
              ? `${run.selected_ids.length} items: ${run.selected_ids.slice(0, 3).join(', ')}${run.selected_ids.length > 3 ? '…' : ''}`
              : '-'
          } />
        </div>

        {/* totals 박스 */}
        <div className="pt-0 px-5 pb-3">
          <div className="flex gap-4 rounded-sm border border-border bg-muted p-3">
            <Total label="총" value={run.totals?.total ?? '-'} />
            <Total label="PASS" value={run.totals?.pass ?? 0} color="var(--cims-success)" />
            <Total label="FAIL" value={run.totals?.fail ?? 0} color="var(--destructive)" />
            <Total label="SKIP" value={run.totals?.skip ?? 0} color="var(--muted-foreground)" />
            {(run.totals?.blocked ?? 0) > 0 && (
              <Total label="BLOCKED" value={run.totals?.blocked ?? 0} color="var(--cims-warning)" />
            )}
          </div>
        </div>

        {/* 항목별 표 */}
        <div className="pt-0 px-5 pb-5 overflow-auto">
          <h3 className="text-base font-semibold mt-2 mx-0 mb-1.5">항목별 결과</h3>
          <table className="w-full border-collapse text-md">
            <thead>
              <tr>
                <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">#</th>
                <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">ID</th>
                <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">Stage</th>
                <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">이름</th>
                <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">상태</th>
                <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">소요</th>
              </tr>
            </thead>
            <tbody>
              {grouped.parents.map(p => (
                <>
                  <tr key={p.id}>
                    <td className="border-b border-border px-3 py-2">{p.idx}</td>
                    <td className="border-b border-border px-3 py-2 font-mono text-xs" style={{ fontWeight: p.is_group ? 700 : 400 }}>
                      {p.is_group ? <ChevronDown size={12} className="mr-1 inline align-[-2px]" /> : null}{p.id}
                    </td>
                    <td className="border-b border-border px-3 py-2">S{p.stage}</td>
                    <td className="border-b border-border px-3 py-2">{p.name}</td>
                    <td className="border-b border-border px-3 py-2 font-semibold" style={{ color: STATUS_COLOR[p.status] || 'var(--foreground)' }}>
                      {p.status}
                    </td>
                    <td className="border-b border-border px-3 py-2">{fmtDuration(p.elapsed_ms)}</td>
                  </tr>
                  {(grouped.childrenByParent[p.id] || []).map(c => (
                    <tr className="bg-muted" key={c.id}>
                      <td className="border-b border-border px-3 py-2">{c.idx}</td>
                      <td className="border-b border-border px-3 py-2 pl-8 font-mono text-xs text-muted-foreground">
                        └ {c.id}
                      </td>
                      <td className="border-b border-border px-3 py-2">S{c.stage}</td>
                      <td className="border-b border-border px-3 py-2">{c.name}</td>
                      <td className="border-b border-border px-3 py-2" style={{ color: STATUS_COLOR[c.status] || 'var(--foreground)' }}>
                        {c.status}
                      </td>
                      <td className="border-b border-border px-3 py-2">{fmtDuration(c.elapsed_ms)}</td>
                    </tr>
                  ))}
                </>
              ))}
            </tbody>
          </table>
        </div>
      </div>
    </div>
  )
}

function Field({ label, value }: { label: string; value: React.ReactNode }) {
  return (
    <div>
      <div className="text-xs text-muted-foreground uppercase">{label}</div>
      <div className="text-md text-foreground mt-0.5">{value}</div>
    </div>
  )
}

function Total({ label, value, color = 'var(--foreground)' }: { label: string; value: number | string; color?: string }) {
  return (
    <div className="min-w-20 text-center">
      <div className="text-xs text-muted-foreground">{label}</div>
      <div className="text-2xl font-bold" style={{ color }}>{value}</div>
    </div>
  )
}

// ─────────────────────────────────────────────────────────────
// 메인 페이지
// ─────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────
// 통계 패널 — overall summary + scope 별 + 시계열 sparkline
// ─────────────────────────────────────────────────────────────
function StatsPanel({
  stats, days, setDays, err,
}: {
  stats: RunsStatsResponse | null
  days: number
  setDays: (n: number) => void
  err: string | null
}) {
  return (
    <div className="mb-4">
      <div className="flex items-center gap-3 mb-2">
        <span className="text-base font-semibold">통계 (최근 {days}일)</span>
        <Select value={String(days)} onValueChange={(v: string) => setDays(Number(v))}>
          <SelectTrigger><SelectValue /></SelectTrigger>
          <SelectContent>
            {[7, 14, 30, 60, 90].map(d => (
              <SelectItem key={d} value={String(d)}>{d}일</SelectItem>
            ))}
          </SelectContent>
        </Select>
        {err && <span className="inline-flex items-center gap-1 text-xs text-destructive">
        <AlertTriangle size={12} /> {err}</span>}
      </div>
      {stats === null ? (
        <div className="rounded-md border border-border bg-card p-3 text-muted-foreground">로딩 중…</div>
      ) : stats.overall.runs === 0 ? (
        <div className="rounded-md border border-border bg-card p-3 text-muted-foreground">해당 기간 회차 없음</div>
      ) : (
        <div className="grid grid-cols-3 gap-3">
          {/* overall */}
          <div className="rounded-md border border-border bg-card p-3">
            <div className="font-semibold mb-1.5">종합</div>
            <KpiGrid items={[
              { label: '전체 회차', value: `${stats.overall.runs}회` },
              { label: '성공률', value: `${stats.overall.success_rate}%`,
                color: stats.overall.success_rate >= 80 ? 'var(--cims-success)'
                       : stats.overall.success_rate >= 50 ? 'var(--cims-warning)' : 'var(--destructive)' },
              { label: 'PASS', value: `${stats.overall.pass}회`, color: 'var(--cims-success)' },
              { label: 'FAIL', value: `${stats.overall.fail}회`, color: 'var(--destructive)' },
              { label: '평균 소요', value: fmtMsShort(stats.overall.avg_elapsed_ms) },
              { label: 'p95 소요', value: fmtMsShort(stats.overall.p95_elapsed_ms) },
            ]} />
          </div>
          {/* by scope */}
          <div className="rounded-md border border-border bg-card p-3">
            <div className="font-semibold mb-1.5">scope 별 성공률</div>
            <ScopeTable rows={stats.by_scope} />
          </div>
          {/* timeline sparkline */}
          <div className="rounded-md border border-border bg-card p-3">
            <div className="font-semibold mb-1.5">회차 추세 ({stats.timeline.length}건)</div>
            <Sparkline timeline={stats.timeline} />
          </div>
        </div>
      )}
    </div>
  )
}

function KpiGrid({ items }: { items: { label: string; value: string; color?: string }[] }) {
  return (
    <div className="grid grid-cols-2 gap-1">
      {items.map(it => (
        <div className="flex justify-between py-0.5 px-0" key={it.label}>
          <span className="text-muted-foreground">{it.label}</span>
          <span className="font-semibold" style={{ color: it.color || 'var(--foreground)' }}>{it.value}</span>
        </div>
      ))}
    </div>
  )
}

function ScopeTable({ rows }: { rows: RunsStatsResponse['by_scope'] }) {
  if (rows.length === 0) return <div className="text-muted-foreground">없음</div>
  return (
    <table className="w-full border-collapse text-xs">
      <thead>
        <tr className="border-b border-border">
          <th className="text-left py-1 px-1.5">scope</th>
          <th className="text-right py-1 px-1.5">회차</th>
          <th className="text-right py-1 px-1.5">성공률</th>
          <th className="text-right py-1 px-1.5">평균</th>
        </tr>
      </thead>
      <tbody>
        {rows.map(r => (
          <tr key={r.scope} className="border-b border-dashed border-border">
            <td className="py-[3px] px-1.5">{r.scope}</td>
            <td className="py-[3px] px-1.5 text-right">{r.runs}</td>
            <td className={`px-1.5 py-[3px] text-right font-semibold ${
              r.success_rate >= 80 ? 'text-success' : r.success_rate >= 50 ? 'text-warning' : 'text-destructive'}`}>
              {r.success_rate}%</td>
            <td className="py-[3px] px-1.5 text-right">{fmtMsShort(r.avg_elapsed_ms)}</td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}

/** 회차별 verdict + elapsed 시계열 — inline SVG sparkline (라이브러리 의존 X). */
function Sparkline({ timeline }: { timeline: RunsStatsResponse['timeline'] }) {
  if (timeline.length === 0) return <div className="text-muted-foreground">데이터 없음</div>
  const W = 380
  const H = 70
  const PAD_X = 4
  const PAD_Y = 6
  const maxElapsed = Math.max(1, ...timeline.map(t => t.elapsed_ms))
  const stepX = (W - PAD_X * 2) / Math.max(1, timeline.length - 1)
  return (
    <svg className="block" width={W} height={H + 18}>
      {/* 좌표축: 하단선 */}
      <line x1={PAD_X} y1={H - PAD_Y} x2={W - PAD_X} y2={H - PAD_Y}
            stroke="var(--border)" strokeWidth={1} />
      {timeline.map((t, i) => {
        const x = PAD_X + stepX * i
        const y = H - PAD_Y - ((t.elapsed_ms / maxElapsed) * (H - PAD_Y * 2))
        const color = VERDICT_COLOR[t.verdict] || 'var(--muted-foreground)'
        return (
          <g key={t.id}>
            <line x1={x} y1={H - PAD_Y} x2={x} y2={y}
                  stroke={color} strokeWidth={2} opacity={0.5} />
            <circle cx={x} cy={y} r={2.5} fill={color}>
              <title>{`${fmtRunIdShort(t.id)} ${t.scope} ${t.verdict} — ${fmtMsShort(t.elapsed_ms)}`}</title>
            </circle>
          </g>
        )
      })}
      {/* 범례 */}
      <text x={PAD_X} y={H + 12} fontSize={9} fill="var(--muted-foreground)">
        {timeline[0]?.started_at?.slice(0, 10) ?? ''}
      </text>
      <text x={W - PAD_X} y={H + 12} fontSize={9} fill="var(--muted-foreground)" textAnchor="end">
        {timeline[timeline.length - 1]?.started_at?.slice(0, 10) ?? ''}
      </text>
    </svg>
  )
}

function fmtMsShort(ms: number): string {
  if (ms < 1000) return `${ms}ms`
  if (ms < 60_000) return `${(ms / 1000).toFixed(1)}s`
  const mins = Math.floor(ms / 60_000)
  const secs = Math.floor((ms % 60_000) / 1000)
  return `${mins}m${secs.toString().padStart(2, '0')}s`
}

export default function VerificationHistoryPage() {
  const { show } = useToast()
  const [runs, setRuns] = useState<RunHistoryItem[]>([])
  const [total, setTotal] = useState(0)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [stage, setStage] = useState<number | ''>('')
  const [verdict, setVerdict] = useState<string>('')
  const [limit] = useState(50)
  const [offset, setOffset] = useState(0)
  const [detail, setDetail] = useState<RunDetail | null>(null)
  const [stats, setStats] = useState<RunsStatsResponse | null>(null)
  const [statsDays, setStatsDays] = useState(30)
  const [statsErr, setStatsErr] = useState<string | null>(null)

  const loadStats = useCallback(async () => {
    setStatsErr(null)
    try {
      const s = await verifyApi.getRunsStats({ days: statsDays, limit: 200 })
      setStats(s)
    } catch (e: unknown) {
      setStatsErr(e instanceof Error ? e.message : String(e))
    }
  }, [statsDays])

  useEffect(() => { loadStats() }, [loadStats])

  const load = useCallback(async () => {
    setLoading(true)
    setError(null)
    try {
      const res = await verifyApi.listRuns({
        stage: stage === '' ? undefined : stage,
        verdict: verdict || undefined,
        limit, offset,
      })
      setRuns(res.runs)
      setTotal(res.total)
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e))
    } finally {
      setLoading(false)
    }
  }, [stage, verdict, limit, offset])

  useEffect(() => { load() }, [load])

  const openDetail = useCallback(async (id: number) => {
    try {
      const d = await verifyApi.getRun(id)
      setDetail(d)
    } catch (e: unknown) {
      show('회차 조회 실패: ' + (e instanceof Error ? e.message : String(e)), 'err')
    }
  }, [])

  const handleDelete = useCallback(async (id: number) => {
    try {
      await verifyApi.deleteRun(id)
      setDetail(null)
      load()
    } catch (e: unknown) {
      show('삭제 실패: ' + (e instanceof Error ? e.message : String(e)), 'err')
    }
  }, [load])

  const totalPages = Math.max(1, Math.ceil(total / limit))
  const curPage = Math.floor(offset / limit) + 1

  return (
    <div className="verify-history-page p-5">
      <style>{`
        @media print {
          @page { margin: 3mm 15mm 2mm 15mm; size: A4; }
          /* color 고정 — 다크로 인쇄하면 --text(밝은 회색)가 상속돼 흰 종이에 흐려진다. */
          html, body { background: #fff !important; color: #111 !important;
                       margin: 0 !important; padding: 0 !important; }
          .app-layout, .app-layout--collapsed,
          .app-content, .app-content-body,
          .verify-history-page {
            display: block !important;
            margin: 0 !important; padding: 0 !important;
            grid-template-columns: 1fr !important;
            max-width: none !important; width: 100% !important;
          }
          .sidebar, .sidebar--collapsed, .app-header, .sub-tabs { display: none !important; }
          /* 모달의 버튼/list 영역 모두 숨김 */
          .verify-history-page > * { display: none !important; }
          /* 모달 자체 배경/포지션 해제 */
          .verify-history-page .verify-history-modal-backdrop {
            position: static !important; background: none !important; display: block !important;
          }
          .verify-history-page .verify-history-modal {
            position: static !important; box-shadow: none !important;
            max-width: none !important; max-height: none !important;
            margin: 0 !important; padding: 0 !important; border-radius: 0 !important;
          }
          .verify-history-page .verify-history-modal > *:not(.v2-report) { display: none !important; }
          .v2-report {
            display: block !important; position: static !important;
            margin: 0 !important; padding: 0 !important; width: 100% !important;
          }
          .v2-report table { page-break-inside: auto; }
          .v2-report tr    { page-break-inside: avoid; }
          .v2-report * {
            -webkit-print-color-adjust: exact !important;
            print-color-adjust: exact !important;
          }
        }
      `}</style>
      <header className="mb-4 flex items-baseline gap-4">
        <h1 className="text-2xl font-bold">검증 이력</h1>
        <span className="text-sm text-muted-foreground">
          총 {total} 회차
        </span>
        <Button variant="outline" className="ml-auto" onClick={() => { load(); loadStats() }}><RotateCw size={13} /> 새로고침</Button>
      </header>

      {/* 통계 패널 */}
      <StatsPanel stats={stats} days={statsDays} setDays={setStatsDays} err={statsErr} />

      {/* 필터 */}
      <div className="flex gap-3 mb-3 items-center flex-wrap">
        <label className="inline-flex items-center gap-1.5 text-md text-foreground">
          Stage:
          <Select value={toSel(stage === '' ? '' : String(stage))}
                  onValueChange={(v: string) => { setOffset(0); setStage(fromSel(v) === '' ? '' : Number(fromSel(v))) }}>
            <SelectTrigger><SelectValue /></SelectTrigger>
            <SelectContent>
              <SelectItem value={NONE}>전체</SelectItem>
              {[1, 2, 3, 4, 5, 6].map(n => (
                <SelectItem key={n} value={String(n)}>S{n} {STAGE_LABEL[n]}</SelectItem>
              ))}
            </SelectContent>
          </Select>
        </label>
        <label className="inline-flex items-center gap-1.5 text-md text-foreground">
          Verdict:
          <Select value={toSel(verdict)} onValueChange={(v: string) => { setOffset(0); setVerdict(fromSel(v)) }}>
            <SelectTrigger><SelectValue /></SelectTrigger>
            <SelectContent>
              <SelectItem value={NONE}>전체</SelectItem>
              <SelectItem value="PASS">PASS</SelectItem>
              <SelectItem value="FAIL">FAIL</SelectItem>
              <SelectItem value="UNKNOWN">UNKNOWN</SelectItem>
            </SelectContent>
          </Select>
        </label>
        {error && (
          <span className="text-destructive text-sm ml-3">{error}</span>
        )}
      </div>

      {/* list 표 */}
      <div className="bg-card border border-border rounded-sm overflow-auto">
        <table className="w-full border-collapse text-md">
          <thead>
            <tr>
              <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">#</th>
              <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">시작 시각</th>
              <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">Scope</th>
              <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">Verdict</th>
              <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">P/F/S</th>
              <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">소요</th>
              <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">Git</th>
              <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">Pkg hash</th>
              <th className="border-b border-border bg-muted px-3 py-2 text-left text-xs text-muted-foreground">Trigger</th>
            </tr>
          </thead>
          <tbody>
            {loading ? (
              <tr><td colSpan={9} className="border-b border-border px-3 py-2 p-5 text-center text-muted-foreground">로딩 중…</td></tr>
            ) : runs.length === 0 ? (
              <tr><td colSpan={9} className="border-b border-border px-3 py-2 p-5 text-center text-muted-foreground">회차 없음</td></tr>
            ) : (
              runs.map(r => (
                <RunListRow key={r.id} run={r} onClick={() => openDetail(r.id)} />
              ))
            )}
          </tbody>
        </table>
      </div>

      {/* 페이지 네비게이션 */}
      {totalPages > 1 && (
        <div className="mt-3 flex items-center gap-2 justify-center">
          <Button variant="outline" disabled={offset === 0}
                  onClick={() => setOffset(Math.max(0, offset - limit))}><ArrowLeft size={13} /> 이전</Button>
          <span className="text-md text-foreground">
            {curPage} / {totalPages}
          </span>
          <Button variant="outline" disabled={offset + limit >= total}
                  onClick={() => setOffset(offset + limit)}>다음 <ArrowRight size={13} /></Button>
        </div>
      )}

      {detail && (
        <DetailModal run={detail} onClose={() => setDetail(null)} onDelete={handleDelete} />
      )}
    </div>
  )
}

// ─────────────────────────────────────────────────────────────
// 스타일
// ─────────────────────────────────────────────────────────────
