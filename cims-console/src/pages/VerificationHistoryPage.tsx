import { useState, useEffect, useCallback, useMemo } from 'react'

import {
  verifyApi,
  type RunHistoryItem, type RunDetail, type RunDetailItem,
  type VerifyStatus,
} from '../api/verification'

// ─────────────────────────────────────────────────────────────
// 검증 이력 페이지 — /testbed/verify-history
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
      <td style={td}>{run.id}</td>
      <td style={td}>{fmtDate(run.started_at)}</td>
      <td style={td}>{scopeLabel(run.scope)}</td>
      <td style={{ ...td, color: VERDICT_COLOR[run.verdict] || '#374151', fontWeight: 600 }}>
        {run.verdict}
      </td>
      <td style={{ ...td, fontSize: 12, color: '#4b5563' }}>
        {(t.pass ?? 0)} / {(t.fail ?? 0)} / {(t.skip ?? 0)}
        {t.blocked ? ` / ${t.blocked}` : ''}
        <span style={{ color: '#9ca3af', marginLeft: 6 }}>(P/F/S)</span>
      </td>
      <td style={td}>{fmtDuration(run.elapsed_ms)}</td>
      <td style={{ ...td, fontFamily: 'monospace', fontSize: 11, color: '#6b7280' }}>
        {run.git_branch && <span>{run.git_branch}@</span>}
        <span>{run.git_sha || '-'}</span>
      </td>
      <td style={{ ...td, fontFamily: 'monospace', fontSize: 11, color: '#9ca3af' }}>
        {run.pkg_manifest_hash ? run.pkg_manifest_hash.slice(0, 10) + '…' : '-'}
      </td>
      <td style={{ ...td, fontSize: 11, color: '#6b7280' }}>{run.trigger}</td>
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
    <div style={modalBackdrop} onClick={onClose}>
      <div style={modal} onClick={e => e.stopPropagation()}>
        <header style={modalHeader}>
          <div>
            <div style={{ fontSize: 18, fontWeight: 700 }}>회차 #{run.id}</div>
            <div style={{ fontSize: 12, color: '#6b7280', marginTop: 4 }}>
              {fmtDate(run.started_at)} ~ {fmtDate(run.finished_at)} ({fmtDuration(run.elapsed_ms)})
            </div>
          </div>
          <div>
            <button style={btnDanger} onClick={() => {
              if (confirm(`회차 #${run.id} 를 삭제할까요? 이 작업은 되돌릴 수 없습니다.`)) {
                onDelete(run.id)
              }
            }}>삭제</button>
            <button style={{ ...btnPrimary, marginLeft: 8 }} onClick={onClose}>닫기</button>
          </div>
        </header>

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
                    <tr key={c.id} style={{ background: '#fafafa' }}>
                      <td style={td}>{c.idx}</td>
                      <td style={{ ...td, paddingLeft: 32, fontFamily: 'monospace', fontSize: 11, color: '#6b7280' }}>
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
      <div style={{ fontSize: 11, color: '#9ca3af', textTransform: 'uppercase' }}>{label}</div>
      <div style={{ fontSize: 13, color: '#111827', marginTop: 2 }}>{value}</div>
    </div>
  )
}

function Total({ label, value, color = '#374151' }: { label: string; value: number | string; color?: string }) {
  return (
    <div style={totalCell}>
      <div style={{ fontSize: 11, color: '#6b7280' }}>{label}</div>
      <div style={{ fontSize: 22, fontWeight: 700, color }}>{value}</div>
    </div>
  )
}

// ─────────────────────────────────────────────────────────────
// 메인 페이지
// ─────────────────────────────────────────────────────────────
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
    <div style={{ padding: 20 }}>
      <header style={{ marginBottom: 16, display: 'flex', alignItems: 'baseline', gap: 16 }}>
        <h1 style={{ fontSize: 22, fontWeight: 700 }}>검증 이력</h1>
        <span style={{ fontSize: 12, color: '#6b7280' }}>
          총 {total} 회차
        </span>
        <button onClick={load} style={{ ...btnSecondary, marginLeft: 'auto' }}>↻ 새로고침</button>
      </header>

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
      <div style={{ background: '#fff', border: '1px solid #e5e7eb', borderRadius: 6, overflow: 'auto' }}>
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
              <tr><td colSpan={9} style={{ ...td, textAlign: 'center', padding: 20, color: '#6b7280' }}>로딩 중…</td></tr>
            ) : runs.length === 0 ? (
              <tr><td colSpan={9} style={{ ...td, textAlign: 'center', padding: 20, color: '#9ca3af' }}>회차 없음</td></tr>
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
          <span style={{ fontSize: 13, color: '#374151' }}>
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
  padding: '8px 12px', borderBottom: '1px solid #e5e7eb',
  textAlign: 'left', background: '#f9fafb',
  fontSize: 11, color: '#6b7280', textTransform: 'uppercase',
  position: 'sticky', top: 0, zIndex: 1,
}
const td: React.CSSProperties = {
  padding: '8px 12px', borderBottom: '1px solid #f3f4f6',
}
const filterLabel: React.CSSProperties = {
  display: 'inline-flex', alignItems: 'center', gap: 6,
  fontSize: 13, color: '#374151',
}
const selectStyle: React.CSSProperties = {
  padding: '4px 8px', border: '1px solid #d1d5db', borderRadius: 4,
  background: '#fff', fontSize: 13,
}
const btnPrimary: React.CSSProperties = {
  padding: '6px 14px', border: 'none', borderRadius: 4,
  background: '#2563eb', color: '#fff', fontSize: 13, cursor: 'pointer',
}
const btnSecondary: React.CSSProperties = {
  padding: '6px 14px', border: '1px solid #d1d5db', borderRadius: 4,
  background: '#fff', color: '#374151', fontSize: 13, cursor: 'pointer',
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
  background: '#fff', borderRadius: 8, width: '90vw', maxWidth: 1100,
  maxHeight: '90vh', overflow: 'auto',
  display: 'flex', flexDirection: 'column',
}
const modalHeader: React.CSSProperties = {
  padding: '16px 20px', borderBottom: '1px solid #e5e7eb',
  display: 'flex', justifyContent: 'space-between', alignItems: 'center',
}
const totalsBox: React.CSSProperties = {
  display: 'flex', gap: 16, padding: 12,
  background: '#f9fafb', borderRadius: 6, border: '1px solid #e5e7eb',
}
const totalCell: React.CSSProperties = {
  textAlign: 'center', minWidth: 80,
}
