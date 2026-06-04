import { useState, useEffect, useCallback, useMemo, type CSSProperties } from 'react'
import { callsApi, type CallLog } from '../../../api/calls'
import { recordingsApi, type RecordingSegment } from '../../../api/recordings'
import FlowPage from '../../../pages/FlowPage'
import SegmentPlayer from '../../../components/SegmentPlayer'
import { useInlineAudio, type InlineAudio } from '../../../components/useInlineAudio'
import { useToast } from '../../../components/Toast'

function fmtDur(s: number | null) { if (!s || s <= 0) return '—'; const m = Math.floor(s / 60); return m > 0 ? `${m}분 ${s % 60}초` : `${s}초` }
function fmtMs(ms: number | null | undefined) {
  if (!ms || ms <= 0) return '—'
  const total = Math.round(ms / 1000); const m = Math.floor(total / 60); const s = total % 60
  return m > 0 ? `${m}분 ${s}초` : `${s}초`
}
// ISO → HH:MM:SS
function fmtClock(iso: string | null | undefined) {
  if (!iso) return '—'
  const s = iso.replace('T', ' '); const i = s.indexOf(' ')
  return i >= 0 ? s.substring(i + 1, i + 9) : s.substring(0, 8)
}
function tms(iso: string | null | undefined): number { const n = Date.parse(iso || ''); return Number.isFinite(n) ? n : 0 }
// 파일기반 로그는 id 가 null 일 수 있음 → 고유키
const callKey = (l: CallLog) => l.call_id || l.dir_name || String(l.id)

// 발신/착신 2색
const CALLER_C = '#2563eb', CALLEE_C = '#16a34a'
const partyColor = (l: CallLog, msisdn: string) => (msisdn === l.callee ? CALLEE_C : CALLER_C)

function callState(s: string) {
  return s === 'ended' ? { label: '종료', cls: 'badge--gray' }
    : s === 'active' ? { label: '통화중', cls: 'badge--green' }
    : s === 'ringing' ? { label: '호출중', cls: 'badge--blue' }
    : { label: s || '—', cls: 'badge--gray' }
}

interface CallDetailState { segments: RecordingSegment[]; loading: boolean; loaded: boolean }

export default function VolteHistoryPage() {
  const { show } = useToast()
  const audio = useInlineAudio(useCallback((m: string) => show(m, 'err'), [show]))

  const [logs, setLogs] = useState<CallLog[]>([])
  const [total, setTotal] = useState(0)
  const [loading, setLoading] = useState(false)
  const [page, setPage] = useState(0)
  const PS = 50
  const [fMsisdn, setFM] = useState('')
  const [fDate, setFD] = useState('')
  const [autoRefresh, setAR] = useState(false)

  const [openHours, setOpenHours] = useState<Set<string>>(new Set())
  // 파일기반 call 로그는 id 가 null 일 수 있어 고유키로 call_id(폴백 dir_name) 사용
  const [expandedCall, setExpandedCall] = useState<string | null>(null)
  const [callDetail, setCallDetail] = useState<Map<string, CallDetailState>>(new Map())

  const [flow, setFlow] = useState<{ callId: string; date: string; callType?: 'volte' | 'ptt' } | null>(null)
  const [recPlayer, setRecPlayer] = useState<{ id: string; segments: RecordingSegment[]; callType: 'volte' | 'ptt' | 'volte_video'; caller: string; callee: string } | null>(null)

  const load = useCallback(async (p: number) => {
    setLoading(true)
    try {
      const r = await callsApi.list({ call_type: 'volte', msisdn: fMsisdn || undefined, date: fDate || undefined, limit: PS, offset: p * PS })
      setLogs(r.logs); setTotal(r.total)
    } catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [show, fMsisdn, fDate])

  useEffect(() => { setPage(0); load(0) }, [load])
  useEffect(() => { if (!autoRefresh) return; const iv = setInterval(() => load(page), 10000); return () => clearInterval(iv) }, [autoRefresh, load, page])

  // ── 시간대(시) 그룹핑: key = 'YYYY-MM-DDTHH' ──
  const hourGroups = useMemo(() => {
    const m = new Map<string, CallLog[]>()
    for (const l of logs) {
      const key = (l.invite_time || '').substring(0, 13) || '기타'
      const arr = m.get(key); if (arr) arr.push(l); else m.set(key, [l])
    }
    return [...m.entries()].sort((a, b) => b[0].localeCompare(a[0]))   // 최신 시간대 먼저
  }, [logs])

  // 로드/페이지 변경 시 최신 시간대 자동 펼침
  useEffect(() => {
    if (hourGroups.length > 0) setOpenHours(new Set([hourGroups[0][0]]))
    setExpandedCall(null)
  }, [hourGroups])

  const toggleHour = (k: string) => setOpenHours(prev => {
    const n = new Set(prev); if (n.has(k)) n.delete(k); else n.add(k); return n
  })

  const loadCallDetail = useCallback(async (dirName: string) => {
    if (callDetail.get(dirName)?.loaded) return
    setCallDetail(prev => { const m = new Map(prev); m.set(dirName, { segments: [], loading: true, loaded: false }); return m })
    try {
      const rec = await recordingsApi.get(dirName)
      setCallDetail(prev => { const m = new Map(prev); m.set(dirName, { segments: rec.segments || [], loading: false, loaded: true }); return m })
    } catch {
      setCallDetail(prev => { const m = new Map(prev); m.set(dirName, { segments: [], loading: false, loaded: true }); return m })
    }
  }, [callDetail])

  const toggleCall = (l: CallLog) => {
    audio.stop()
    const key = callKey(l)
    setExpandedCall(prev => {
      if (prev === key) return null
      if (l.dir_name) loadCallDetail(l.dir_name)
      return key
    })
  }

  const openRecording = async (l: CallLog) => {
    if (!l.dir_name) { show('녹취 디렉터리 정보 없음', 'err'); return }
    try {
      const rec = await recordingsApi.get(l.dir_name)
      if (rec.segments && rec.segments.length > 0) {
        setRecPlayer({ id: l.dir_name, segments: rec.segments, callType: rec.call_type as 'volte' | 'volte_video', caller: l.initiator, callee: l.callee })
      } else { show('세그먼트 없음', 'err') }
    } catch (e: unknown) { show(String(e), 'err') }
  }

  const openFlow = (l: CallLog) => setFlow({ callId: l.call_id, date: l.invite_time?.substring(0, 10) || '', callType: 'volte' })

  const totalPages = Math.ceil(total / PS)
  const thS: CSSProperties = { padding: '7px 10px', fontWeight: 600, color: 'var(--text-muted)', textAlign: 'left', whiteSpace: 'nowrap' }

  return (
    <div>
      <div className="toolbar">
        <input className="search-input" placeholder="발신/착신 번호" value={fMsisdn} onChange={e => setFM(e.target.value)} onKeyDown={e => e.key === 'Enter' && (setPage(0), load(0))} style={{ maxWidth: 180 }} />
        <input type="date" className="form-input" value={fDate} onChange={e => { setFD(e.target.value); setPage(0) }} style={{ width: 140 }} />
        <button className="btn btn--primary btn--sm" onClick={() => { setPage(0); load(0) }}>검색</button>
        <label style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 4, fontSize: 12, color: 'var(--text-muted)', cursor: 'pointer' }}>
          <input type="checkbox" checked={autoRefresh} onChange={e => setAR(e.target.checked)} />자동갱신
        </label>
      </div>

      {loading ? <div className="empty">로딩 중...</div>
        : hourGroups.length === 0 ? <div className="empty">이력 없음</div>
          : (
            <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
              {hourGroups.map(([hourKey, calls]) => {
                const open = openHours.has(hourKey)
                const label = hourKey.length >= 13 ? `${hourKey.slice(5, 7)}/${hourKey.slice(8, 10)} ${hourKey.slice(11, 13)}시` : hourKey
                const recCnt = calls.filter(c => c.has_recording).length
                return (
                  <div key={hourKey} style={{ border: '1px solid var(--border)', borderRadius: 8, overflow: 'hidden' }}>
                    {/* ── 시간대 그룹 헤더 ── */}
                    <div onClick={() => toggleHour(hourKey)}
                      style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '8px 12px', cursor: 'pointer', background: 'var(--surface-alt, #f7f9fc)', fontWeight: 600 }}>
                      <span style={{ color: 'var(--text-muted)' }}>{open ? '▾' : '▸'}</span>
                      <span>{label}</span>
                      <span className="badge badge--gray" style={{ fontSize: 11 }}>{calls.length}건</span>
                      {recCnt > 0 && <span className="ts" style={{ color: 'var(--text-muted)' }}>녹취 {recCnt}</span>}
                    </div>

                    {/* ── 시간대 내 호 목록 ── */}
                    {open && (
                      <table className="data-table" style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
                        <thead>
                          <tr style={{ borderTop: '1px solid var(--border)' }}>
                            <th style={{ ...thS, width: 24 }}></th>
                            <th style={thS}>유형</th>
                            <th style={thS}>발신 → 착신</th>
                            <th style={{ ...thS, textAlign: 'center' }}>상태</th>
                            <th style={thS}>시작</th>
                            <th style={{ ...thS, textAlign: 'right' }}>통화시간</th>
                            <th style={thS}>종료사유</th>
                            <th style={{ ...thS, textAlign: 'right' }}>작업</th>
                          </tr>
                        </thead>
                        <tbody>
                          {calls.map(l => {
                            const isOpen = expandedCall === callKey(l)
                            const st = callState(l.state)
                            const dur = l.answer_time && l.end_time
                              ? Math.max(0, Math.floor((tms(l.end_time) - tms(l.answer_time)) / 1000))
                              : l.duration
                            const detail = l.dir_name ? callDetail.get(l.dir_name) : undefined
                            return (
                              <CallRow
                                key={callKey(l)} l={l} isOpen={isOpen} st={st} dur={dur} detail={detail} audio={audio}
                                onToggle={() => toggleCall(l)} onFlow={() => openFlow(l)} onPlayAll={() => openRecording(l)}
                              />
                            )
                          })}
                        </tbody>
                      </table>
                    )}
                  </div>
                )
              })}
            </div>
          )}

      {totalPages > 1 && <div className="toolbar" style={{ justifyContent: 'center', gap: 8, marginTop: 8 }}>
        <button className="btn btn--sm btn--outline" disabled={page === 0} onClick={() => { setPage(page - 1); load(page - 1) }}>← 이전</button>
        <span className="ts">{page + 1}/{totalPages} (총 {total}건)</span>
        <button className="btn btn--sm btn--outline" disabled={page >= totalPages - 1} onClick={() => { setPage(page + 1); load(page + 1) }}>다음 →</button>
      </div>}

      {audio.node}

      {recPlayer && (
        <div className="modal-overlay" onClick={() => setRecPlayer(null)}>
          <div className="modal-box" style={{ maxWidth: 1360, width: '95vw' }} onClick={e => e.stopPropagation()}>
            <SegmentPlayer segments={recPlayer.segments} recordingId={recPlayer.id} callType={recPlayer.callType} caller={recPlayer.caller} callee={recPlayer.callee} onClose={() => setRecPlayer(null)} />
          </div>
        </div>
      )}

      {flow && <FlowPage callId={flow.callId} date={flow.date} callType={flow.callType} onClose={() => setFlow(null)} />}
    </div>
  )
}

// ════════════════════════════════════════════════════════════════
// 호 단위 accordion 행 (헤더 + 펼침 타임라인)
// ════════════════════════════════════════════════════════════════
const tdS: CSSProperties = { padding: '6px 10px', whiteSpace: 'nowrap' }

function CallRow({ l, isOpen, st, dur, detail, audio, onToggle, onFlow, onPlayAll }: {
  l: CallLog
  isOpen: boolean
  st: { label: string; cls: string }
  dur: number | null
  detail: CallDetailState | undefined
  audio: InlineAudio
  onToggle: () => void
  onFlow: () => void
  onPlayAll: () => void
}) {
  return (
    <>
      <tr onClick={onToggle} style={{ cursor: 'pointer', borderTop: '1px solid var(--border)', background: isOpen ? 'var(--hover, #eef5ff)' : 'transparent' }}>
        <td style={{ ...tdS, textAlign: 'center', color: 'var(--text-muted)' }}>{isOpen ? '▾' : '▸'}</td>
        <td style={tdS}><span className={`badge ${l.call_type === 'volte_video' ? 'badge--blue' : 'badge--gray'}`} style={{ fontSize: 10 }}>{l.call_type === 'volte_video' ? '영상' : '음성'}</span></td>
        <td style={tdS}>
          <span style={{ fontWeight: 600, color: CALLER_C }}>{l.initiator}</span>
          <span style={{ color: 'var(--text-muted)' }}> → </span>
          <span style={{ fontWeight: 600, color: CALLEE_C }}>{l.callee || '—'}</span>
        </td>
        <td style={{ ...tdS, textAlign: 'center' }}><span className={`badge ${st.cls}`}>{st.label}</span></td>
        <td style={tdS} className="ts">{fmtClock(l.invite_time)}</td>
        <td style={{ ...tdS, textAlign: 'right' }} className="ts">{fmtDur(dur)}</td>
        <td style={tdS} className="ts">{l.end_reason_ko || l.end_reason || '—'}</td>
        <td style={{ ...tdS, textAlign: 'right' }} onClick={e => e.stopPropagation()}>
          <button className="btn btn--sm btn--outline" style={{ marginRight: 4 }} onClick={onFlow}>플로우</button>
          {l.has_recording && <button className="btn btn--sm btn--outline" onClick={onPlayAll}>&#9654; 전체</button>}
        </td>
      </tr>
      {isOpen && (
        <tr>
          <td colSpan={8} style={{ padding: 0, background: 'var(--surface-alt, #fafbfd)', borderTop: '1px solid var(--border)' }}>
            <div style={{ padding: '12px 16px' }}>
              <CallTimeline l={l} detail={detail} audio={audio} />
            </div>
          </td>
        </tr>
      )}
    </>
  )
}

// 호 단위 통합 타임라인: 발신→호출/응답→종료 시그널링 + 발화 세그먼트(인라인 재생)
type CTItem =
  | { t: number; kind: 'sig'; iso: string; label: string; color: string; sub?: string }
  | { t: number; kind: 'seg'; seg: RecordingSegment }

function CallTimeline({ l, detail, audio }: { l: CallLog; detail: CallDetailState | undefined; audio: InlineAudio }) {
  const recId = l.dir_name || null

  const items = useMemo<CTItem[]>(() => {
    const out: CTItem[] = []
    if (l.invite_time) out.push({ t: tms(l.invite_time), kind: 'sig', iso: l.invite_time, label: '발신 (INVITE)', color: CALLER_C, sub: `${l.initiator} → ${l.callee || '—'}` })
    if (l.answer_time) out.push({ t: tms(l.answer_time), kind: 'sig', iso: l.answer_time, label: '응답 (200 OK)', color: '#16a34a' })
    for (const seg of (detail?.segments || [])) out.push({ t: tms(seg.start_time), kind: 'seg', seg })
    if (l.end_time) {
      const reason = l.end_reason_ko || l.end_reason || ''
      out.push({ t: tms(l.end_time), kind: 'sig', iso: l.end_time, label: '종료 (BYE)', color: '#9333ea', sub: [reason, l.sip_status ? `SIP ${l.sip_status}` : ''].filter(Boolean).join(' · ') })
    }
    out.sort((a, b) => a.t - b.t)
    return out
  }, [l, detail?.segments])

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
      {/* 요약 줄 */}
      <div style={{ display: 'flex', gap: 16, flexWrap: 'wrap', fontSize: 12, color: 'var(--text-muted)' }}>
        <span>발신 <b style={{ color: CALLER_C }}>{l.initiator}</b></span>
        <span>착신 <b style={{ color: CALLEE_C }}>{l.callee || '—'}</b></span>
        <span>시작 {fmtClock(l.invite_time)}</span>
        {l.answer_time && <span>응답 {fmtClock(l.answer_time)}</span>}
        {l.end_time && <span>종료 {fmtClock(l.end_time)}</span>}
      </div>

      {/* 통합 타임라인 */}
      {detail?.loading ? (
        <div className="empty" style={{ padding: 10 }}>상세 로딩 중...</div>
      ) : (
        <div style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface, #fff)', maxHeight: 360, overflowY: 'auto' }}>
          {items.map((it, i) => {
            const border = i > 0 ? '1px solid var(--border)' : undefined
            if (it.kind === 'sig') {
              return (
                <div key={`g${i}`} style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '5px 10px', fontSize: 12, borderTop: border, borderLeft: '4px solid transparent' }}>
                  <span className="ts" style={{ minWidth: 70, color: 'var(--text-muted)' }}>{fmtClock(it.iso)}</span>
                  <span style={{ minWidth: 30, textAlign: 'center', color: it.color }}>◆</span>
                  <span style={{ color: it.color, fontWeight: 600 }}>{it.label}</span>
                  {it.sub && <span className="ts" style={{ color: 'var(--text-muted)' }}>{it.sub}</span>}
                </div>
              )
            }
            const seg = it.seg
            const color = partyColor(l, seg.speaker_id)
            const isPlaying = audio.playing?.recId === recId && audio.playing?.seq === seg.seq
            const isPrep = audio.preparing?.recId === recId && audio.preparing?.seq === seg.seq
            const playable = seg.status !== 'recording' && !seg.has_video
            return (
              <div key={`s${seg.seq}`} style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '6px 10px', fontSize: 12, borderTop: border, borderLeft: `4px solid ${color}`, background: isPlaying ? 'var(--hover, #eef5ff)' : undefined }}>
                <span className="ts" style={{ minWidth: 70, color: 'var(--text-muted)' }}>{fmtClock(seg.start_time)}</span>
                <button
                  className={`btn btn--sm ${isPlaying ? 'btn--primary' : 'btn--outline'}`}
                  disabled={!recId || !playable}
                  style={{ minWidth: 30, padding: '2px 6px' }}
                  onClick={() => recId && playable && audio.play(recId, seg.seq)}
                  title={seg.has_video ? '영상은 전체재생으로' : playable ? '재생/정지' : '재생 불가'}
                >
                  {isPrep ? '…' : isPlaying ? '❚❚' : '▶'}
                </button>
                <span style={{ fontWeight: 600, color }}>{seg.speaker_id}</span>
                <span style={{ color: 'var(--text-muted)' }}>발화</span>
                {seg.has_video && <span className="badge badge--blue" style={{ fontSize: 9 }}>영상</span>}
                {seg.status === 'recording' && <span className="badge badge--blue" style={{ fontSize: 9 }}>녹취중</span>}
                <span className="ts" style={{ marginLeft: 'auto', color: 'var(--text-muted)' }}>{fmtMs(seg.duration_ms)}</span>
              </div>
            )
          })}
        </div>
      )}

      {/* 참여자 */}
      {l.participants?.length > 0 && (
        <div style={{ display: 'flex', gap: 12, flexWrap: 'wrap', fontSize: 11, color: 'var(--text-muted)' }}>
          {l.participants.map(p => (
            <span key={p.msisdn} style={{ display: 'inline-flex', alignItems: 'center', gap: 4 }}>
              <span style={{ width: 9, height: 9, borderRadius: 2, background: p.role === 'callee' ? CALLEE_C : CALLER_C }} />
              {p.msisdn} ({p.role === 'caller' ? '발신' : p.role === 'callee' ? '수신' : '멤버'})
              <span className="ts">{fmtClock(p.join_time)}{p.leave_time ? `~${fmtClock(p.leave_time)}` : ''}</span>
            </span>
          ))}
        </div>
      )}
    </div>
  )
}
