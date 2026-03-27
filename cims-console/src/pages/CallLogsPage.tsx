import { useState, useEffect, useCallback, useRef } from 'react'
import { callsApi, type CallLog, type CallParticipant } from '../api/calls'
import Modal from '../components/Modal'
import FlowPage from './FlowPage'
import { useToast } from '../components/Toast'

// ── helpers ───────────────────────────────────────────────────
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
  const map: Record<string, string> = {
    ringing: 'badge--blue',
    active:  'badge--green',
    ended:   'badge--gray',
  }
  const label: Record<string, string> = {
    ringing: '호출 중', active: '통화 중', ended: '종료',
  }
  return <span className={`badge ${map[state] ?? 'badge--gray'}`}>{label[state] ?? state}</span>
}

function TypeChip({ type }: { type: CallLog['call_type'] }) {
  return (
    <span className={`badge ${type === 'ptt' ? 'badge--green' : 'badge--blue'}`}>
      {type === 'ptt' ? 'PTT' : 'VoIP'}
    </span>
  )
}

// ── types ─────────────────────────────────────────────────────
type SubTab = 'live' | 'history'

// ── main component ────────────────────────────────────────────
export default function CallLogsPage() {
  const { show } = useToast()
  const [subTab, setSubTab] = useState<SubTab>('live')

  // live
  const [liveLogs, setLiveLogs]     = useState<CallLog[]>([])
  const [liveLoading, setLiveLoading] = useState(true)
  const liveTimerRef = useRef<ReturnType<typeof setInterval> | null>(null)

  // history
  const [histLogs, setHistLogs]     = useState<CallLog[]>([])
  const [histTotal, setHistTotal]   = useState(0)
  const [histLoading, setHistLoading] = useState(false)
  const [histPage, setHistPage]     = useState(0)
  const PAGE_SIZE = 50

  // filters
  const [fMsisdn,    setFMsisdn]    = useState('')
  const [fGroupId,   setFGroupId]   = useState('')
  const [fType,      setFType]      = useState('')
  const [fFromDt,    setFFromDt]    = useState('')
  const [fToDt,      setFToDt]      = useState('')

  // detail modal
  const [detail, setDetail] = useState<CallLog | null>(null)

  // flow modal
  const [flowTarget, setFlowTarget] = useState<{ callId: string; date: string } | null>(null)

  function openFlow(log: CallLog, e: React.MouseEvent) {
    e.stopPropagation()
    const date = log.invite_time ? log.invite_time.substring(0, 10).replace(/-/g, '') : undefined
    setFlowTarget({ callId: log.call_id, date: date ?? '' })
  }

  // ── load live ────────────────────────────────────────────────
  const loadLive = useCallback(async () => {
    try {
      const r = await callsApi.active()
      setLiveLogs(r.logs)
    } catch (e: unknown) {
      // silently handle 503 (tables not created yet) on live tab
      const msg = String(e)
      if (msg.includes('503')) {
        setLiveLogs([])
      } else {
        show(msg, 'err')
      }
    } finally {
      setLiveLoading(false)
    }
  }, [show])

  useEffect(() => {
    if (subTab !== 'live') return
    setLiveLoading(true)
    loadLive()
    liveTimerRef.current = setInterval(loadLive, 5000)
    return () => { if (liveTimerRef.current) clearInterval(liveTimerRef.current) }
  }, [subTab, loadLive])

  // ── load history ─────────────────────────────────────────────
  const loadHistory = useCallback(async (page: number) => {
    setHistLoading(true)
    try {
      const r = await callsApi.list({
        state: 'ended',
        msisdn:   fMsisdn   || undefined,
        group_id: fGroupId  || undefined,
        call_type: fType    || undefined,
        from_dt:  fFromDt   || undefined,
        to_dt:    fToDt     || undefined,
        limit:    PAGE_SIZE,
        offset:   page * PAGE_SIZE,
      })
      setHistLogs(r.logs)
      setHistTotal(r.total)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setHistLoading(false)
    }
  }, [show, fMsisdn, fGroupId, fType, fFromDt, fToDt])

  useEffect(() => {
    if (subTab !== 'history') return
    setHistPage(0)
    loadHistory(0)
  }, [subTab, loadHistory])

  function handleSearch() {
    setHistPage(0)
    loadHistory(0)
  }

  function handlePageChange(p: number) {
    setHistPage(p)
    loadHistory(p)
  }

  // ── render helpers ───────────────────────────────────────────
  function renderParticipants(parts: CallParticipant[]) {
    if (parts.length === 0) return <span className="ts">—</span>
    return (
      <div style={{ display: 'flex', flexWrap: 'wrap', gap: 4 }}>
        {parts.map(p => (
          <span key={p.msisdn} style={{ display: 'inline-flex', flexDirection: 'column', alignItems: 'flex-start' }}>
            <span className={`sub-chip ${p.leave_time ? 'sub-chip--call' : 'sub-chip--ptt'}`}
                  style={{ borderRadius: 4, padding: '2px 6px', fontSize: 11 }}>
              {p.msisdn}
              {p.role !== 'caller' && p.role !== 'callee' && (
                <span style={{ opacity: 0.6, marginLeft: 2 }}>({p.role})</span>
              )}
              {!p.leave_time && <span style={{ marginLeft: 4, fontWeight: 700 }}>●</span>}
            </span>
          </span>
        ))}
      </div>
    )
  }

  function renderLogRow(log: CallLog, showParticipants = false) {
    return (
      <tr key={log.id} style={{ cursor: 'pointer' }} onClick={() => setDetail(log)}>
        <td><TypeChip type={log.call_type} /></td>
        <td>
          <div>{log.initiator}</div>
          {log.call_type === 'ptt' && log.group_id && (
            <div className="ts">그룹: {log.group_id}</div>
          )}
          {log.call_type === 'voip' && (
            <div className="ts">→ {log.callee}</div>
          )}
        </td>
        <td><StateChip state={log.state} /></td>
        <td className="ts">{fmtTime(log.invite_time)}</td>
        <td className="ts">{fmtDuration(log.duration)}</td>
        {showParticipants && <td>{renderParticipants(log.participants)}</td>}
        {!showParticipants && <td className="ts">{log.end_reason_ko || (log.sip_status ? String(log.sip_status) : '—')}</td>}
        <td>
          <button
            className="btn btn--sm btn--outline"
            title="메시지 플로우"
            onClick={(e) => openFlow(log, e)}
            style={{ fontSize: 11, padding: '2px 6px' }}
          >
            플로우
          </button>
        </td>
      </tr>
    )
  }

  // ── total pages ──────────────────────────────────────────────
  const totalPages = Math.ceil(histTotal / PAGE_SIZE)

  return (
    <div className="page">
      {/* sub-tab bar */}
      <div className="toolbar" style={{ paddingBottom: 0 }}>
        <div className="tab-bar" style={{ margin: 0, border: 'none', gap: 8 }}>
          <button
            className={`tab-btn${subTab === 'live' ? ' tab-btn--active' : ''}`}
            onClick={() => setSubTab('live')}
          >
            🔴 실시간
          </button>
          <button
            className={`tab-btn${subTab === 'history' ? ' tab-btn--active' : ''}`}
            onClick={() => setSubTab('history')}
          >
            📋 이력
          </button>
        </div>
        {subTab === 'live' && (
          <span className="ts" style={{ marginLeft: 'auto', alignSelf: 'center' }}>
            5초 자동 갱신
          </span>
        )}
      </div>

      {/* ── LIVE tab ── */}
      {subTab === 'live' && (
        <>
          {liveLoading ? (
            <div className="empty">로딩 중…</div>
          ) : liveLogs.length === 0 ? (
            <div className="empty">현재 진행 중인 통화가 없습니다.</div>
          ) : (
            <div className="table-wrap">
              <table className="data-table">
                <thead>
                  <tr>
                    <th style={{ width: 60 }}>유형</th>
                    <th>발신 / 대상</th>
                    <th style={{ width: 90 }}>상태</th>
                    <th>시작 시각</th>
                    <th>경과 시간</th>
                    <th>참여자</th>
                    <th style={{ width: 60 }}></th>
                  </tr>
                </thead>
                <tbody>
                  {liveLogs.map(log => renderLogRow(log, true))}
                </tbody>
              </table>
            </div>
          )}
        </>
      )}

      {/* ── HISTORY tab ── */}
      {subTab === 'history' && (
        <>
          {/* filter bar */}
          <div className="toolbar">
            <input
              className="search-input"
              placeholder="번호 검색 (발신/수신/참여자)"
              value={fMsisdn}
              onChange={e => setFMsisdn(e.target.value)}
              onKeyDown={e => e.key === 'Enter' && handleSearch()}
              style={{ maxWidth: 200 }}
            />
            <input
              className="search-input"
              placeholder="그룹 ID"
              value={fGroupId}
              onChange={e => setFGroupId(e.target.value)}
              style={{ maxWidth: 120 }}
            />
            <select
              className="form-input"
              value={fType}
              onChange={e => setFType(e.target.value)}
              style={{ width: 90 }}
            >
              <option value="">전체유형</option>
              <option value="voip">VoIP</option>
              <option value="ptt">PTT</option>
            </select>
            <input
              type="date"
              className="form-input"
              value={fFromDt}
              onChange={e => setFFromDt(e.target.value)}
              style={{ width: 140 }}
            />
            <span className="ts">~</span>
            <input
              type="date"
              className="form-input"
              value={fToDt}
              onChange={e => setFToDt(e.target.value)}
              style={{ width: 140 }}
            />
            <button className="btn btn--primary" onClick={handleSearch}>검색</button>
          </div>

          {histLoading ? (
            <div className="empty">로딩 중…</div>
          ) : histLogs.length === 0 ? (
            <div className="empty">이력이 없습니다.</div>
          ) : (
            <>
              <div className="table-wrap">
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
                    {histLogs.map(log => renderLogRow(log, false))}
                  </tbody>
                </table>
              </div>

              {/* pagination */}
              {totalPages > 1 && (
                <div className="toolbar" style={{ justifyContent: 'center', gap: 8 }}>
                  <button className="btn btn--sm btn--outline"
                    disabled={histPage === 0} onClick={() => handlePageChange(histPage - 1)}>← 이전</button>
                  <span className="ts">{histPage + 1} / {totalPages} (총 {histTotal}건)</span>
                  <button className="btn btn--sm btn--outline"
                    disabled={histPage >= totalPages - 1} onClick={() => handlePageChange(histPage + 1)}>다음 →</button>
                </div>
              )}
            </>
          )}
        </>
      )}

      {/* ── flow modal ── */}
      {flowTarget && (
        <FlowPage
          callId={flowTarget.callId}
          date={flowTarget.date || undefined}
          onClose={() => setFlowTarget(null)}
        />
      )}

      {/* ── detail modal ── */}
      {detail && (
        <Modal
          title={`통화 상세 — ${detail.call_type.toUpperCase()} ${detail.initiator}`}
          onClose={() => setDetail(null)}
        >
          <dl className="detail-list">
            <dt>유형</dt>     <dd><TypeChip type={detail.call_type} /></dd>
            <dt>상태</dt>     <dd><StateChip state={detail.state} /></dd>
            <dt>발신</dt>     <dd>{detail.initiator}</dd>
            <dt>수신</dt>     <dd>{detail.callee}</dd>
            {detail.group_id && <><dt>그룹</dt><dd>{detail.group_id}</dd></>}
            <dt>호출 시각</dt> <dd>{fmtTime(detail.invite_time)}</dd>
            <dt>연결 시각</dt> <dd>{fmtTime(detail.answer_time)}</dd>
            <dt>종료 시각</dt> <dd>{fmtTime(detail.end_time)}</dd>
            <dt>통화 시간</dt> <dd>{fmtDuration(detail.duration)}</dd>
            <dt>SIP 상태</dt> <dd>{detail.sip_status ?? '—'}</dd>
            <dt>종료 사유</dt> <dd>{detail.end_reason_ko || detail.end_reason || '—'}</dd>
          </dl>

          {detail.participants.length > 0 && (
            <>
              <div className="form-section-title" style={{ marginTop: 16 }}>참여자</div>
              <table className="data-table">
                <thead>
                  <tr>
                    <th>MSISDN</th>
                    <th>역할</th>
                    <th>연결 시각</th>
                    <th>이탈 시각</th>
                    <th>참여 시간</th>
                  </tr>
                </thead>
                <tbody>
                  {detail.participants.map(p => {
                    const joined = p.join_time ? new Date(p.join_time).getTime() : null
                    const left   = p.leave_time ? new Date(p.leave_time).getTime() : Date.now()
                    const sec    = joined ? Math.floor((left - joined) / 1000) : null
                    return (
                      <tr key={p.msisdn}>
                        <td>
                          {p.msisdn}
                          {!p.leave_time && <span className="badge badge--green" style={{ marginLeft: 6 }}>연결 중</span>}
                        </td>
                        <td className="ts">{
                          p.role === 'caller' ? '발신' :
                          p.role === 'callee' ? '수신' : '멤버'
                        }</td>
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

          <div className="modal-footer">
            <button className="btn btn--ghost" onClick={() => setDetail(null)}>닫기</button>
          </div>
        </Modal>
      )}
    </div>
  )
}
