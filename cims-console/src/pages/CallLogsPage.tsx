import { useState, useEffect, useCallback } from 'react'
import { callsApi, type CallLog } from '../api/calls'
import Modal from '../components/Modal'
import FlowPage from './FlowPage'
import { useToast } from '../components/Toast'

function fmtDuration(sec: number | null): string {
  if (sec == null || sec <= 0) return '—'
  const m = Math.floor(sec / 60)
  const s = sec % 60
  return m > 0 ? `${m}분 ${s}초` : `${s}초`
}

function fmtTime(iso: string | null): string {
  if (!iso) return '—'
  return iso.replace('T', ' ').substring(0, 19)
}

function StateChip({ state }: { state: CallLog['state'] }) {
  const map: Record<string, string> = { ringing: 'badge--blue', active: 'badge--green', ended: 'badge--gray' }
  const label: Record<string, string> = { ringing: '호출 중', active: '통화 중', ended: '종료' }
  return <span className={`badge ${map[state] ?? 'badge--gray'}`}>{label[state] ?? state}</span>
}

function TypeChip({ type }: { type: CallLog['call_type'] }) {
  return <span className={`badge ${type === 'ptt' ? 'badge--green' : 'badge--blue'}`}>{type === 'ptt' ? 'PTT' : 'VoIP'}</span>
}

export default function CallLogsPage() {
  const { show } = useToast()

  const [logs, setLogs] = useState<CallLog[]>([])
  const [total, setTotal] = useState(0)
  const [loading, setLoading] = useState(false)
  const [page, setPage] = useState(0)
  const PAGE_SIZE = 50

  const [fMsisdn, setFMsisdn] = useState('')
  const [fGroupId, setFGroupId] = useState('')
  const [fType, setFType] = useState('')
  const [fFromDt, setFFromDt] = useState('')
  const [fToDt, setFToDt] = useState('')

  const [fEndReason, setFEndReason] = useState('')
  const [autoRefresh, setAutoRefresh] = useState(false)

  const [detail, setDetail] = useState<CallLog | null>(null)
  const [flowTarget, setFlowTarget] = useState<{ callId: string; date: string } | null>(null)

  function openFlow(log: CallLog, e: React.MouseEvent) {
    e.stopPropagation()
    const date = log.invite_time ? log.invite_time.substring(0, 10) : undefined
    setFlowTarget({ callId: log.call_id, date: date ?? '' })
  }

  const load = useCallback(async (p: number) => {
    setLoading(true)
    try {
      const r = await callsApi.list({
        msisdn: fMsisdn || undefined,
        group_id: fGroupId || undefined,
        call_type: fType || undefined,
        date: fFromDt || undefined,
        limit: PAGE_SIZE,
        offset: p * PAGE_SIZE,
      })
      setLogs(r.logs)
      setTotal(r.total)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [show, fMsisdn, fGroupId, fType, fFromDt, fToDt])

  useEffect(() => { setPage(0); load(0) }, [load])

  useEffect(() => {
    if (!autoRefresh) return
    const iv = setInterval(() => load(page), 10000)
    return () => clearInterval(iv)
  }, [autoRefresh, load, page])

  function handleSearch() { setPage(0); load(0) }
  function handlePageChange(p: number) { setPage(p); load(p) }

  const totalPages = Math.ceil(total / PAGE_SIZE)

  return (
    <div className="page">
      {/* filter bar */}
      <div className="toolbar">
        <input className="search-input" placeholder="번호 검색 (발신/수신/참여자)"
          value={fMsisdn} onChange={e => setFMsisdn(e.target.value)}
          onKeyDown={e => e.key === 'Enter' && handleSearch()} style={{ maxWidth: 200 }} />
        <input className="search-input" placeholder="그룹 ID"
          value={fGroupId} onChange={e => setFGroupId(e.target.value)} style={{ maxWidth: 120 }} />
        <select className="form-input" value={fType} onChange={e => setFType(e.target.value)} style={{ width: 90 }}>
          <option value="">전체유형</option>
          <option value="voip">VoIP</option>
          <option value="ptt">PTT</option>
        </select>
        <input type="date" className="form-input" value={fFromDt} onChange={e => setFFromDt(e.target.value)} style={{ width: 140 }} />
        <span className="ts">~</span>
        <input type="date" className="form-input" value={fToDt} onChange={e => setFToDt(e.target.value)} style={{ width: 140 }} />
        <select className="form-input" value={fEndReason} onChange={e => setFEndReason(e.target.value)} style={{ width: 110 }}>
          <option value="">종료사유</option>
          <option value="normal">정상종료</option>
          <option value="no_answer">무응답</option>
          <option value="busy">통화중</option>
          <option value="rejected">거절</option>
          <option value="error">오류</option>
        </select>
        <button className="btn btn--primary" onClick={handleSearch}>검색</button>
        <label style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 4, fontSize: 12, color: 'var(--text-muted)', cursor: 'pointer' }}>
          <input type="checkbox" checked={autoRefresh} onChange={e => setAutoRefresh(e.target.checked)} />
          자동갱신
        </label>
        <span style={{ marginLeft: 'auto', color: 'var(--text-muted)', fontSize: 13 }}>총 {total}건</span>
      </div>

      {loading ? (
        <div className="empty">로딩 중...</div>
      ) : logs.length === 0 ? (
        <div className="empty">이력이 없습니다.</div>
      ) : (
        <>
          <div className="panel">
            <table className="data-table">
              <thead>
                <tr>
                  <th style={{ width: 60 }}>유형</th>
                  <th>발신 / 대상</th>
                  <th style={{ width: 70 }}>상태</th>
                  <th>시작 시각</th>
                  <th>통화 시간</th>
                  <th>종료 사유</th>
                  <th style={{ width: 60 }}></th>
                </tr>
              </thead>
              <tbody>
                {logs.map(log => (
                  <tr key={log.id} style={{ cursor: 'pointer' }} onClick={() => setDetail(log)}>
                    <td><TypeChip type={log.call_type} /></td>
                    <td>
                      <div>{log.initiator}</div>
                      {log.call_type === 'ptt' && log.group_id && <div className="ts">그룹: {log.group_id}</div>}
                      {log.call_type === 'voip' && <div className="ts">→ {log.callee}</div>}
                    </td>
                    <td><StateChip state={log.state} /></td>
                    <td className="ts">{fmtTime(log.invite_time)}</td>
                    <td className="ts">{fmtDuration(log.duration)}</td>
                    <td className="ts">{log.end_reason_ko || (log.sip_status ? String(log.sip_status) : '—')}</td>
                    <td>
                      <button className="btn btn--sm btn--outline" title="메시지 플로우"
                        onClick={e => openFlow(log, e)} style={{ fontSize: 11, padding: '2px 6px' }}>
                        플로우
                      </button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>

          {totalPages > 1 && (
            <div className="toolbar" style={{ justifyContent: 'center', gap: 8 }}>
              <button className="btn btn--sm btn--outline" disabled={page === 0} onClick={() => handlePageChange(page - 1)}>← 이전</button>
              <span className="ts">{page + 1} / {totalPages} (총 {total}건)</span>
              <button className="btn btn--sm btn--outline" disabled={page >= totalPages - 1} onClick={() => handlePageChange(page + 1)}>다음 →</button>
            </div>
          )}
        </>
      )}

      {flowTarget && (
        <FlowPage callId={flowTarget.callId} date={flowTarget.date || undefined} onClose={() => setFlowTarget(null)} />
      )}

      {detail && (
        <Modal title={`통화 상세 — ${detail.call_type.toUpperCase()} ${detail.initiator}`} onClose={() => setDetail(null)}>
          <dl className="detail-list">
            <dt>유형</dt><dd><TypeChip type={detail.call_type} /></dd>
            <dt>상태</dt><dd><StateChip state={detail.state} /></dd>
            <dt>발신</dt><dd>{detail.initiator}</dd>
            <dt>수신</dt><dd>{detail.callee}</dd>
            {detail.group_id && <><dt>그룹</dt><dd>{detail.group_id}</dd></>}
            <dt>호출 시각</dt><dd>{fmtTime(detail.invite_time)}</dd>
            <dt>연결 시각</dt><dd>{fmtTime(detail.answer_time)}</dd>
            <dt>종료 시각</dt><dd>{fmtTime(detail.end_time)}</dd>
            <dt>통화 시간</dt><dd>{fmtDuration(detail.duration)}</dd>
            <dt>SIP 상태</dt><dd>{detail.sip_status ?? '—'}</dd>
            <dt>종료 사유</dt><dd>{detail.end_reason_ko || detail.end_reason || '—'}</dd>
          </dl>

          {detail.participants.length > 0 && (
            <>
              <div className="form-section-title" style={{ marginTop: 16 }}>참여자</div>
              <table className="data-table">
                <thead><tr><th>MSISDN</th><th>역할</th><th>연결</th><th>이탈</th><th>참여 시간</th></tr></thead>
                <tbody>
                  {detail.participants.map(p => {
                    const joined = p.join_time ? new Date(p.join_time).getTime() : null
                    const left = p.leave_time ? new Date(p.leave_time).getTime() : Date.now()
                    const sec = joined ? Math.floor((left - joined) / 1000) : null
                    return (
                      <tr key={p.msisdn}>
                        <td>{p.msisdn} {!p.leave_time && <span className="badge badge--green" style={{ marginLeft: 6 }}>연결 중</span>}</td>
                        <td className="ts">{p.role === 'caller' ? '발신' : p.role === 'callee' ? '수신' : '멤버'}</td>
                        <td className="ts">{fmtTime(p.join_time)}</td>
                        <td className="ts">{fmtTime(p.leave_time)}</td>
                        <td className="ts">{fmtDuration(sec)}</td>
                      </tr>
                    )
                  })}
                </tbody>
              </table>
            </>
          )}
          <div className="modal-footer" style={{ justifyContent: 'space-between' }}>
            <div style={{ display: 'flex', gap: 8 }}>
              <button className="btn btn--sm btn--outline" onClick={() => { if (detail) openFlow(detail, null as unknown as React.MouseEvent); }}>
                플로우 보기
              </button>
              <button className="btn btn--sm btn--outline" style={{ color: 'var(--danger)' }}
                onClick={() => window.open(`/api/v1/recordings?call_type=${detail.call_type}&caller=${encodeURIComponent(detail.initiator)}&limit=1`, '_blank')}>
                녹취 조회
              </button>
            </div>
            <button className="btn btn--ghost" onClick={() => setDetail(null)}>닫기</button>
          </div>
        </Modal>
      )}
    </div>
  )
}
