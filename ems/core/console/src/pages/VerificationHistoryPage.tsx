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
  PASS: '#16a34a',
  FAIL: '#dc2626',
  UNKNOWN: '#6b7280',
}

const STATUS_COLOR: Record<VerifyStatus, string> = {
  PASS:    '#16a34a',
  FAIL:    '#dc2626',
  SKIP:    '#9ca3af',
  BLOCKED: '#eab308',
  RUNNING: '#2563eb',
  PENDING: '#6b7280',
  UNKNOWN: '#6b7280',
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
    <tr onClick={onClick} style={{ cursor: 'pointer' }}>
      <td style={{ ...td, fontFamily: 'monospace', fontSize: 11 }} title={`run_id=${run.id}`}>
        {fmtRunIdShort(run.id)}
      </td>
      <td style={td}>{fmtDate(run.started_at)}</td>
      <td style={td}>{scopeLabel(run.scope)}</td>
      <td style={{ ...td, color: VERDICT_COLOR[run.verdict] || 'var(--text)', fontWeight: 600 }}>
        {run.verdict}
      </td>
      <td style={{ ...td, fontSize: 12, color: 'var(--text-muted)' }}>
        {(t.pass ?? 0)} / {(t.fail ?? 0)} / {(t.skip ?? 0)}
        {t.blocked ? ` / ${t.blocked}` : ''}
        <span style={{ color: 'var(--text-muted)', marginLeft: 6 }}>(P/F/S)</span>
      </td>
      <td style={td}>{fmtDuration(run.elapsed_ms)}</td>
      <td style={{ ...td, fontFamily: 'monospace', fontSize: 11, color: 'var(--text-muted)' }}>
        {run.git_branch && <span>{run.git_branch}@</span>}
        <span>{run.git_sha || '-'}</span>
      </td>
      <td style={{ ...td, fontFamily: 'monospace', fontSize: 11, color: 'var(--text-muted)' }}>
        {run.pkg_manifest_hash ? run.pkg_manifest_hash.slice(0, 10) + '…' : '-'}
      </td>
      <td style={{ ...td, fontSize: 11, color: 'var(--text-muted)' }}>{run.trigger}</td>
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
    <div className="verify-history-modal-backdrop" style={modalBackdrop} onClick={onClose}>
      <div className="verify-history-modal" style={modal} onClick={e => e.stopPropagation()}>
        <header style={modalHeader}>
          <div>
            <div style={{ fontSize: 18, fontWeight: 700 }} title={`run_id=${run.id}`}>
              회차 {fmtRunIdShort(run.id)}
            </div>
            <div style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 4 }}>
              {fmtDate(run.started_at)} ~ {fmtDate(run.finished_at)} ({fmtDuration(run.elapsed_ms)})
            </div>
          </div>
          <div>
            <button style={btnSecondary} onClick={() => window.print()} title="이 회차를 PDF 보고서로 인쇄">
              📄 PDF 인쇄
            </button>
            <button style={{ ...btnDanger, marginLeft: 8 }} onClick={() => {
              if (confirm(`회차 ${fmtRunIdShort(run.id)} 를 삭제할까요? 이 작업은 되돌릴 수 없습니다.`)) {
                onDelete(run.id)
              }
            }}>삭제</button>
            <button style={{ ...btnPrimary, marginLeft: 8 }} onClick={onClose}>닫기</button>
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

        <div style={{ padding: '12px 20px', display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12 }}>
          <Field label="Scope" value={scopeLabel(run.scope)} />
          <Field label="Verdict" value={
            <span style={{ color: VERDICT_COLOR[run.verdict], fontWeight: 700 }}>{run.verdict}</span>
          } />
          <Field label="Trigger" value={run.trigger} />
          <Field label="Host" value={run.host || '-'} />
          <Field label="Git" value={`${run.git_branch}@${run.git_sha}`} />
          <Field label="Pkg manifest hash" value={
            <span style={{ fontFamily: 'monospace', fontSize: 11 }}>
              {run.pkg_manifest_hash || '-'}
            </span>
          } />
          <Field label="Report path" value={
            <span style={{ fontFamily: 'monospace', fontSize: 11, wordBreak: 'break-all' }}>
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
        <div style={{ padding: '0 20px 12px' }}>
          <div style={totalsBox}>
            <Total label="총" value={run.totals?.total ?? '-'} />
            <Total label="PASS" value={run.totals?.pass ?? 0} color="#16a34a" />
            <Total label="FAIL" value={run.totals?.fail ?? 0} color="#dc2626" />
            <Total label="SKIP" value={run.totals?.skip ?? 0} color="#9ca3af" />
            {(run.totals?.blocked ?? 0) > 0 && (
              <Total label="BLOCKED" value={run.totals?.blocked ?? 0} color="#eab308" />
            )}
          </div>
        </div>

        {/* 항목별 표 */}
        <div style={{ padding: '0 20px 20px', overflow: 'auto' }}>
          <h3 style={{ fontSize: 14, fontWeight: 600, margin: '8px 0 6px' }}>항목별 결과</h3>
          <table style={tableStyle}>
            <thead>
              <tr>
                <th style={th}>#</th>
                <th style={th}>ID</th>
                <th style={th}>Stage</th>
                <th style={th}>이름</th>
                <th style={th}>상태</th>
                <th style={th}>소요</th>
              </tr>
            </thead>
            <tbody>
              {grouped.parents.map(p => (
                <>
                  <tr key={p.id}>
                    <td style={td}>{p.idx}</td>
                    <td style={{ ...td, fontFamily: 'monospace', fontSize: 11, fontWeight: p.is_group ? 700 : 400 }}>
                      {p.is_group ? '▼ ' : ''}{p.id}
                    </td>
                    <td style={td}>S{p.stage}</td>
                    <td style={td}>{p.name}</td>
                    <td style={{ ...td, color: STATUS_COLOR[p.status] || '#6b7280', fontWeight: 600 }}>
                      {p.status}
                    </td>
                    <td style={td}>{fmtDuration(p.elapsed_ms)}</td>
                  </tr>
                  {(grouped.childrenByParent[p.id] || []).map(c => (
                    <tr key={c.id} style={{ background: 'var(--bg-soft)' }}>
                      <td style={td}>{c.idx}</td>
                      <td style={{ ...td, paddingLeft: 32, fontFamily: 'monospace', fontSize: 11, color: 'var(--text-muted)' }}>
                        └ {c.id}
                      </td>
                      <td style={td}>S{c.stage}</td>
                      <td style={td}>{c.name}</td>
                      <td style={{ ...td, color: STATUS_COLOR[c.status] || '#6b7280' }}>
                        {c.status}
                      </td>
                      <td style={td}>{fmtDuration(c.elapsed_ms)}</td>
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
      <div style={{ fontSize: 11, color: 'var(--text-muted)', textTransform: 'uppercase' }}>{label}</div>
      <div style={{ fontSize: 13, color: '#111827', marginTop: 2 }}>{value}</div>
    </div>
  )
}

function Total({ label, value, color = 'var(--text)' }: { label: string; value: number | string; color?: string }) {
  return (
    <div style={totalCell}>
      <div style={{ fontSize: 11, color: 'var(--text-muted)' }}>{label}</div>
      <div style={{ fontSize: 22, fontWeight: 700, color }}>{value}</div>
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
  const card = useMemo<React.CSSProperties>(() => ({
    background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6,
    padding: 12, fontSize: 12,
  }), [])
  return (
    <div style={{ marginBottom: 16 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 8 }}>
        <span style={{ fontSize: 14, fontWeight: 600 }}>통계 (최근 {days}일)</span>
        <select
          value={days}
          onChange={e => setDays(Number(e.target.value))}
          style={selectStyle}
        >
          {[7, 14, 30, 60, 90].map(d => (
            <option key={d} value={d}>{d}일</option>
          ))}
        </select>
        {err && <span style={{ color: '#dc2626', fontSize: 11 }}>⚠ {err}</span>}
      </div>
      {stats === null ? (
        <div style={{ ...card, color: 'var(--text-muted)' }}>로딩 중…</div>
      ) : stats.overall.runs === 0 ? (
        <div style={{ ...card, color: 'var(--text-muted)' }}>해당 기간 회차 없음</div>
      ) : (
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: 12 }}>
          {/* overall */}
          <div style={card}>
            <div style={{ fontWeight: 600, marginBottom: 6 }}>종합</div>
            <KpiGrid items={[
              { label: '전체 회차', value: `${stats.overall.runs}회` },
              { label: '성공률', value: `${stats.overall.success_rate}%`,
                color: stats.overall.success_rate >= 80 ? '#16a34a'
                       : stats.overall.success_rate >= 50 ? 'var(--warning)' : '#dc2626' },
              { label: 'PASS', value: `${stats.overall.pass}회`, color: '#16a34a' },
              { label: 'FAIL', value: `${stats.overall.fail}회`, color: '#dc2626' },
              { label: '평균 소요', value: fmtMsShort(stats.overall.avg_elapsed_ms) },
              { label: 'p95 소요', value: fmtMsShort(stats.overall.p95_elapsed_ms) },
            ]} />
          </div>
          {/* by scope */}
          <div style={card}>
            <div style={{ fontWeight: 600, marginBottom: 6 }}>scope 별 성공률</div>
            <ScopeTable rows={stats.by_scope} />
          </div>
          {/* timeline sparkline */}
          <div style={card}>
            <div style={{ fontWeight: 600, marginBottom: 6 }}>회차 추세 ({stats.timeline.length}건)</div>
            <Sparkline timeline={stats.timeline} />
          </div>
        </div>
      )}
    </div>
  )
}

function KpiGrid({ items }: { items: { label: string; value: string; color?: string }[] }) {
  return (
    <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 4 }}>
      {items.map(it => (
        <div key={it.label} style={{ display: 'flex', justifyContent: 'space-between', padding: '2px 0' }}>
          <span style={{ color: 'var(--text-muted)' }}>{it.label}</span>
          <span style={{ fontWeight: 600, color: it.color || 'var(--text)' }}>{it.value}</span>
        </div>
      ))}
    </div>
  )
}

function ScopeTable({ rows }: { rows: RunsStatsResponse['by_scope'] }) {
  if (rows.length === 0) return <div style={{ color: 'var(--text-muted)' }}>없음</div>
  return (
    <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11 }}>
      <thead>
        <tr style={{ borderBottom: '1px solid var(--border)' }}>
          <th style={{ textAlign: 'left',  padding: '4px 6px' }}>scope</th>
          <th style={{ textAlign: 'right', padding: '4px 6px' }}>회차</th>
          <th style={{ textAlign: 'right', padding: '4px 6px' }}>성공률</th>
          <th style={{ textAlign: 'right', padding: '4px 6px' }}>평균</th>
        </tr>
      </thead>
      <tbody>
        {rows.map(r => (
          <tr key={r.scope} style={{ borderBottom: '1px dashed var(--border)' }}>
            <td style={{ padding: '3px 6px' }}>{r.scope}</td>
            <td style={{ padding: '3px 6px', textAlign: 'right' }}>{r.runs}</td>
            <td style={{
              padding: '3px 6px', textAlign: 'right', fontWeight: 600,
              color: r.success_rate >= 80 ? '#16a34a'
                     : r.success_rate >= 50 ? 'var(--warning)' : '#dc2626',
            }}>{r.success_rate}%</td>
            <td style={{ padding: '3px 6px', textAlign: 'right' }}>{fmtMsShort(r.avg_elapsed_ms)}</td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}

/** 회차별 verdict + elapsed 시계열 — inline SVG sparkline (라이브러리 의존 X). */
function Sparkline({ timeline }: { timeline: RunsStatsResponse['timeline'] }) {
  if (timeline.length === 0) return <div style={{ color: 'var(--text-muted)' }}>데이터 없음</div>
  const W = 380
  const H = 70
  const PAD_X = 4
  const PAD_Y = 6
  const maxElapsed = Math.max(1, ...timeline.map(t => t.elapsed_ms))
  const stepX = (W - PAD_X * 2) / Math.max(1, timeline.length - 1)
  return (
    <svg width={W} height={H + 18} style={{ display: 'block' }}>
      {/* 좌표축: 하단선 */}
      <line x1={PAD_X} y1={H - PAD_Y} x2={W - PAD_X} y2={H - PAD_Y}
            stroke="#e5e7eb" strokeWidth={1} />
      {timeline.map((t, i) => {
        const x = PAD_X + stepX * i
        const y = H - PAD_Y - ((t.elapsed_ms / maxElapsed) * (H - PAD_Y * 2))
        const color = VERDICT_COLOR[t.verdict] || '#6b7280'
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
      <text x={PAD_X} y={H + 12} fontSize={9} fill="#6b7280">
        {timeline[0]?.started_at?.slice(0, 10) ?? ''}
      </text>
      <text x={W - PAD_X} y={H + 12} fontSize={9} fill="#6b7280" textAnchor="end">
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
      alert('회차 조회 실패: ' + (e instanceof Error ? e.message : String(e)))
    }
  }, [])

  const handleDelete = useCallback(async (id: number) => {
    try {
      await verifyApi.deleteRun(id)
      setDetail(null)
      load()
    } catch (e: unknown) {
      alert('삭제 실패: ' + (e instanceof Error ? e.message : String(e)))
    }
  }, [load])

  const totalPages = Math.max(1, Math.ceil(total / limit))
  const curPage = Math.floor(offset / limit) + 1

  return (
    <div className="verify-history-page" style={{ padding: 20 }}>
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
      <header style={{ marginBottom: 16, display: 'flex', alignItems: 'baseline', gap: 16 }}>
        <h1 style={{ fontSize: 22, fontWeight: 700 }}>검증 이력</h1>
        <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>
          총 {total} 회차
        </span>
        <button onClick={() => { load(); loadStats() }} style={{ ...btnSecondary, marginLeft: 'auto' }}>↻ 새로고침</button>
      </header>

      {/* 통계 패널 */}
      <StatsPanel stats={stats} days={statsDays} setDays={setStatsDays} err={statsErr} />

      {/* 필터 */}
      <div style={{ display: 'flex', gap: 12, marginBottom: 12, alignItems: 'center', flexWrap: 'wrap' }}>
        <label style={filterLabel}>
          Stage:
          <select value={stage} onChange={e => { setOffset(0); setStage(e.target.value === '' ? '' : Number(e.target.value)) }} style={selectStyle}>
            <option value="">전체</option>
            {[1, 2, 3, 4, 5, 6].map(n => (
              <option key={n} value={n}>S{n} {STAGE_LABEL[n]}</option>
            ))}
          </select>
        </label>
        <label style={filterLabel}>
          Verdict:
          <select value={verdict} onChange={e => { setOffset(0); setVerdict(e.target.value) }} style={selectStyle}>
            <option value="">전체</option>
            <option value="PASS">PASS</option>
            <option value="FAIL">FAIL</option>
            <option value="UNKNOWN">UNKNOWN</option>
          </select>
        </label>
        {error && (
          <span style={{ color: '#dc2626', fontSize: 12, marginLeft: 12 }}>{error}</span>
        )}
      </div>

      {/* list 표 */}
      <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6, overflow: 'auto' }}>
        <table style={tableStyle}>
          <thead>
            <tr>
              <th style={th}>#</th>
              <th style={th}>시작 시각</th>
              <th style={th}>Scope</th>
              <th style={th}>Verdict</th>
              <th style={th}>P/F/S</th>
              <th style={th}>소요</th>
              <th style={th}>Git</th>
              <th style={th}>Pkg hash</th>
              <th style={th}>Trigger</th>
            </tr>
          </thead>
          <tbody>
            {loading ? (
              <tr><td colSpan={9} style={{ ...td, textAlign: 'center', padding: 20, color: 'var(--text-muted)' }}>로딩 중…</td></tr>
            ) : runs.length === 0 ? (
              <tr><td colSpan={9} style={{ ...td, textAlign: 'center', padding: 20, color: 'var(--text-muted)' }}>회차 없음</td></tr>
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
        <div style={{ marginTop: 12, display: 'flex', alignItems: 'center', gap: 8, justifyContent: 'center' }}>
          <button style={btnSecondary} disabled={offset === 0}
                  onClick={() => setOffset(Math.max(0, offset - limit))}>← 이전</button>
          <span style={{ fontSize: 13, color: 'var(--text)' }}>
            {curPage} / {totalPages}
          </span>
          <button style={btnSecondary} disabled={offset + limit >= total}
                  onClick={() => setOffset(offset + limit)}>다음 →</button>
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
const tableStyle: React.CSSProperties = {
  width: '100%', borderCollapse: 'collapse', fontSize: 13,
}
const th: React.CSSProperties = {
  padding: '8px 12px', borderBottom: '1px solid var(--border)',
  textAlign: 'left', background: 'var(--bg-soft)',
  fontSize: 11, color: 'var(--text-muted)', textTransform: 'uppercase',
  position: 'sticky', top: 0, zIndex: 1,
}
const td: React.CSSProperties = {
  padding: '8px 12px', borderBottom: '1px solid var(--border)',
}
const filterLabel: React.CSSProperties = {
  display: 'inline-flex', alignItems: 'center', gap: 6,
  fontSize: 13, color: 'var(--text)',
}
const selectStyle: React.CSSProperties = {
  padding: '4px 8px', border: '1px solid var(--border)', borderRadius: 4,
  background: 'var(--surface)', fontSize: 13,
}
const btnPrimary: React.CSSProperties = {
  padding: '6px 14px', border: 'none', borderRadius: 4,
  background: '#2563eb', color: '#fff', fontSize: 13, cursor: 'pointer',
}
const btnSecondary: React.CSSProperties = {
  padding: '6px 14px', border: '1px solid var(--border)', borderRadius: 4,
  background: 'var(--surface)', color: 'var(--text)', fontSize: 13, cursor: 'pointer',
}
const btnDanger: React.CSSProperties = {
  padding: '6px 14px', border: 'none', borderRadius: 4,
  background: '#dc2626', color: '#fff', fontSize: 13, cursor: 'pointer',
}
const modalBackdrop: React.CSSProperties = {
  position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.4)',
  display: 'flex', justifyContent: 'center', alignItems: 'center',
  zIndex: 1000,
}
const modal: React.CSSProperties = {
  background: 'var(--surface)', borderRadius: 8, width: '90vw', maxWidth: 1100,
  maxHeight: '90vh', overflow: 'auto',
  display: 'flex', flexDirection: 'column',
}
const modalHeader: React.CSSProperties = {
  padding: '16px 20px', borderBottom: '1px solid var(--border)',
  display: 'flex', justifyContent: 'space-between', alignItems: 'center',
}
const totalsBox: React.CSSProperties = {
  display: 'flex', gap: 16, padding: 12,
  background: 'var(--bg-soft)', borderRadius: 6, border: '1px solid var(--border)',
}
const totalCell: React.CSSProperties = {
  textAlign: 'center', minWidth: 80,
}
