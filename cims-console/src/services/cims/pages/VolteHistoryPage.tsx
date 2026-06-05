import { useState, useEffect, useCallback, useMemo, type CSSProperties } from 'react'
import { callsApi, type CallLog } from '../../../api/calls'
import { statsApi, type OrgStat } from '../../../api/stats'
import { recordingsApi, type RecordingSegment } from '../../../api/recordings'
import { flowApi, type FlowMessage } from '../../../api/flow'
import FlowPage, { SequenceDiagram } from '../../../pages/FlowPage'
import SegmentPlayer from '../../../components/SegmentPlayer'
import { useToast } from '../../../components/Toast'

function fmtDur(s: number | null) { if (!s || s <= 0) return '—'; const m = Math.floor(s / 60); return m > 0 ? `${m}분 ${s % 60}초` : `${s}초` }
function fmtClock(iso: string | null | undefined) {
  if (!iso) return '—'
  const s = iso.replace('T', ' '); const i = s.indexOf(' ')
  return i >= 0 ? s.substring(i + 1, i + 12) : s.substring(0, 12)
}
function tms(iso: string | null | undefined): number { const n = Date.parse(iso || ''); return Number.isFinite(n) ? n : 0 }
const callKey = (l: CallLog) => l.call_id || l.dir_name || String(l.id)

// flow body 조회용 헬퍼 (FlowPage 와 동일 규약)
const hourFromTs = (ts: string) => (ts || '').slice(0, 2) || undefined
const inferDir = (m: FlowMessage) => (m.from === 'csp' ? 'TX' : m.to === 'csp' ? 'RX' : '')

const CALLER_C = '#2563eb', CALLEE_C = '#16a34a'
const ACTOR_LABEL: Record<string, string> = { ue: 'UE', ue_o: 'UEᴼ', ue_t: 'UEᵀ', cwrtc: 'CWRTC', csc: 'CSC', csp: 'CSP', cmp: 'CMP' }
const actorLbl = (a: string) => ACTOR_LABEL[a] || (a ? a.toUpperCase() : '—')
const PROTO_COLOR: Record<string, string> = { SIP: '#2563eb', JSON: '#d97706', CSC: '#9333ea', RTP: '#16a34a', INT: '#0891b2', MCPTT: '#db2777' }
const protoColor = (p: string) => PROTO_COLOR[p] || 'var(--text-muted)'
const nodeOf = (m: FlowMessage) => (m.node || m.iface || '').replace(/_\d+$/, '')

function callState(s: string) {
  return s === 'ended' ? { label: '종료', cls: 'badge--gray' }
    : s === 'active' ? { label: '통화중', cls: 'badge--green' }
    : s === 'ringing' ? { label: '호출중', cls: 'badge--blue' }
    : { label: s || '—', cls: 'badge--gray' }
}

interface CallFlowState { messages: FlowMessage[]; loading: boolean; loaded: boolean }
type RecPlayer = { id: string; segments: RecordingSegment[]; callType: 'volte' | 'ptt' | 'volte_video'; caller: string; callee: string }

const PAGE_SIZES = [10, 20, 50, 100]
const todayStr = () => { const d = new Date(); return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}` }

// 시간대 히트맵 — 선택 날짜의 24시간 호 건수(색 농도+숫자), 클릭 시 해당 시간 필터
function HourHeatmap({ hours, selHour, onPick }: { hours: Record<string, number>; selHour: string | null; onPick: (h: string | null) => void }) {
  const cells = Array.from({ length: 24 }, (_, h) => ({ h: String(h).padStart(2, '0'), v: hours[String(h).padStart(2, '0')] || 0 }))
  const max = Math.max(1, ...cells.map(c => c.v))
  return (
    <div style={{ display: 'flex', gap: 2, marginBottom: 10 }}>
      {cells.map(c => {
        const ratio = c.v > 0 ? 0.18 + 0.82 * (c.v / max) : 0
        const on = selHour === c.h
        return (
          <div key={c.h} onClick={() => c.v > 0 && onPick(on ? null : c.h)}
            title={`${c.h}시 · ${c.v}건`}
            style={{
              flex: 1, cursor: c.v > 0 ? 'pointer' : 'default', textAlign: 'center',
              borderRadius: 4, padding: '3px 0',
              border: on ? '2px solid var(--primary)' : '1px solid var(--border)',
              background: c.v > 0 ? `rgba(37,99,235,${ratio.toFixed(3)})` : 'var(--surface-alt, #f7f9fc)',
              color: ratio > 0.55 ? '#fff' : 'var(--text)',
            }}>
            <div style={{ fontSize: 12, fontWeight: 600, lineHeight: 1.3, height: 16 }}>{c.v > 0 ? c.v : ' '}</div>
            <div style={{ fontSize: 9, lineHeight: 1.3, height: 12, color: ratio > 0.55 ? 'rgba(255,255,255,.8)' : 'var(--text-muted)' }}>{c.h}</div>
          </div>
        )
      })}
    </div>
  )
}

export default function VolteHistoryPage() {
  const { show } = useToast()

  const [logs, setLogs] = useState<CallLog[]>([])
  const [hours, setHours] = useState<Record<string, number>>({})
  const [total, setTotal] = useState(0)
  const [loading, setLoading] = useState(false)
  const [page, setPage] = useState(0)
  const [ps, setPs] = useState(20)
  const [fDate, setFD] = useState(todayStr())
  const [searchInput, setSearchInput] = useState('')
  const [q, setQ] = useState('')
  const [selOrg, setSelOrg] = useState('')      // 선택 부서 코드 ('' = 전체)
  const [selHour, setSelHour] = useState<string | null>(null)
  const [autoRefresh, setAR] = useState(false)

  const [orgs, setOrgs] = useState<OrgStat[]>([])

  const [expandedCall, setExpandedCall] = useState<string | null>(null)
  const [flowByCall, setFlowByCall] = useState<Map<string, CallFlowState>>(new Map())

  const [flow, setFlow] = useState<{ callId: string; date: string; callType?: 'volte' | 'ptt' } | null>(null)
  const [recPlayer, setRecPlayer] = useState<RecPlayer | null>(null)

  // 부서 트리 (한 번 로드)
  useEffect(() => {
    statsApi.serviceOrg().then(r => setOrgs(r.orgs)).catch(e => show(String(e), 'err'))
  }, [show])

  // 검색 디바운스
  useEffect(() => { const t = setTimeout(() => { setQ(searchInput.trim()); setPage(0) }, 350); return () => clearTimeout(t) }, [searchInput])

  const load = useCallback(async (p: number) => {
    setLoading(true)
    try {
      const r = await callsApi.list({
        call_type: 'volte', date: fDate || undefined,
        org: selOrg || undefined, q: q || undefined,
        hour: selHour || undefined,
        limit: ps, offset: p * ps,
      })
      setLogs(r.logs); setTotal(r.total); setHours(r.hours || {})
    } catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [show, fDate, selOrg, q, selHour, ps])

  // 필터 변경 → 1페이지부터 재조회
  useEffect(() => { setPage(0); setExpandedCall(null); load(0) }, [load])
  useEffect(() => { if (!autoRefresh) return; const iv = setInterval(() => load(page), 10000); return () => clearInterval(iv) }, [autoRefresh, load, page])

  // 날짜/부서/검색 변경 시 시간 선택 해제 (히트맵 재구성)
  useEffect(() => { setSelHour(null) }, [fDate, selOrg, q])

  // ── 호 펼침 시: 메시지 이력(flow) + 녹취(있으면) lazy 로드 ──
  const loadCallFlow = useCallback(async (l: CallLog) => {
    const key = callKey(l)
    if (flowByCall.get(key)?.loaded) return
    setFlowByCall(prev => { const m = new Map(prev); m.set(key, { messages: [], loading: true, loaded: false }); return m })
    try {
      const date = l.invite_time?.substring(0, 10) || undefined
      const r = await flowApi.get(l.call_id, date, 'volte')
      const msgs: FlowMessage[] = r.nodes ? Object.values(r.nodes).flat() : (r.messages || [])
      msgs.sort((a, b) => (a.ts || '').localeCompare(b.ts || ''))
      setFlowByCall(prev => { const m = new Map(prev); m.set(key, { messages: msgs, loading: false, loaded: true }); return m })
    } catch {
      setFlowByCall(prev => { const m = new Map(prev); m.set(key, { messages: [], loading: false, loaded: true }); return m })
    }
  }, [flowByCall])

  const toggleCall = (l: CallLog) => {
    setExpandedCall(prev => {
      if (prev === callKey(l)) return null
      loadCallFlow(l)
      return callKey(l)
    })
  }

  // 녹취 재생 — 별도 dialog(반투명) 로 표시
  const openRecording = async (l: CallLog) => {
    if (!l.dir_name) { show('녹취 디렉터리 정보 없음', 'err'); return }
    try {
      const rec = await recordingsApi.get(l.dir_name)
      if (rec.segments && rec.segments.length > 0) {
        setRecPlayer({ id: l.dir_name, segments: rec.segments, callType: (rec.call_type as RecPlayer['callType']) || 'volte', caller: l.initiator, callee: l.callee })
      } else { show('녹취 세그먼트 없음', 'err') }
    } catch (e: unknown) { show(String(e), 'err') }
  }

  const totalPages = Math.max(1, Math.ceil(total / ps))
  const dayTotal = useMemo(() => Object.values(hours).reduce((a, b) => a + b, 0), [hours])
  const selNode = orgs.find(o => o.code === selOrg)
  const thS: CSSProperties = { padding: '7px 10px', fontWeight: 600, color: 'var(--text-muted)', textAlign: 'left', whiteSpace: 'nowrap', position: 'sticky', top: 0, background: 'var(--surface, #fff)', zIndex: 1 }

  return (
    <div className="panel" style={{ padding: 10 }}>
      {/* 상단 검색/날짜/표시수 */}
      <div className="toolbar" style={{ marginBottom: 8 }}>
        <input type="date" className="form-input" value={fDate} onChange={e => setFD(e.target.value)} style={{ width: 150 }} />
        <input className="search-input" placeholder="가입자 이름/번호 검색" value={searchInput}
          onChange={e => setSearchInput(e.target.value)} style={{ maxWidth: 240 }} />
        {q && <button className="btn btn--sm btn--ghost" onClick={() => setSearchInput('')}>검색 해제</button>}
        <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>
          {selNode ? `부서: ${selNode.name}` : '전체'}{q ? `  &  검색: "${q}"` : ''}{selHour ? `  &  ${selHour}시` : ''}
        </span>
        <label style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 4, fontSize: 12, color: 'var(--text-muted)', cursor: 'pointer' }}>
          <input type="checkbox" checked={autoRefresh} onChange={e => setAR(e.target.checked)} />자동갱신
        </label>
        <label style={{ fontSize: 12, color: 'var(--text-muted)' }}>
          표시{' '}
          <select className="form-input" value={ps} onChange={e => { setPs(Number(e.target.value)); setPage(0) }}
            style={{ width: 'auto', padding: '2px 6px', display: 'inline-block' }}>
            {PAGE_SIZES.map(n => <option key={n} value={n}>{n}건</option>)}
          </select>
        </label>
      </div>

      <div style={{ display: 'flex', gap: 10, alignItems: 'flex-start' }}>
        {/* 좌: 부서 트리 */}
        <div style={{ flex: '0 0 220px', height: 'calc(100vh - 230px)', minHeight: 420, overflow: 'auto', borderRight: '1px solid var(--border)', paddingRight: 6 }}>
          <div onClick={() => setSelOrg('')}
            style={{ cursor: 'pointer', padding: '4px 6px', borderRadius: 4, fontSize: 13, fontWeight: 700,
              background: selOrg === '' ? 'rgba(80,120,255,.12)' : undefined }}>
            전체 호이력
          </div>
          {orgs.map(o => (
            <div key={o.code} onClick={() => setSelOrg(o.code)}
              style={{ cursor: 'pointer', padding: '4px 6px', paddingLeft: 6 + o.depth * 16, borderRadius: 4, fontSize: 13,
                background: selOrg === o.code ? 'rgba(80,120,255,.12)' : undefined,
                fontWeight: o.depth === 0 ? 700 : o.depth === 1 ? 600 : 400 }}>
              {o.name} <span className="ts">({o.members})</span>
            </div>
          ))}
        </div>

        {/* 우: 히트맵 + 호 목록 */}
        <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column', height: 'calc(100vh - 230px)', minHeight: 420 }}>
          {/* 시간대 히트맵 */}
          <div style={{ flexShrink: 0 }}>
            <div style={{ display: 'flex', alignItems: 'baseline', gap: 8, marginBottom: 4 }}>
              <span style={{ fontSize: 12, fontWeight: 600 }}>시간대별 호 분포</span>
              <span className="ts" style={{ color: 'var(--text-muted)' }}>{fDate} · 총 {dayTotal}건</span>
              {selHour && <button className="btn btn--sm btn--ghost" onClick={() => setSelHour(null)} style={{ marginLeft: 'auto' }}>{selHour}시 해제</button>}
            </div>
            <HourHeatmap hours={hours} selHour={selHour} onPick={h => { setSelHour(h); setPage(0) }} />
          </div>

          {/* 호 목록 — 헤더(고정)·본문(스크롤, 항목없어도 영역 유지)·테일(고정) 항상 표시 */}
          <div style={{ flex: 1, minHeight: 0, overflow: 'auto', border: '1px solid var(--border)', borderRadius: 6 }}>
            <table className="data-table" style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
              <thead>
                <tr>
                  <th style={{ ...thS, width: 24 }}></th>
                  <th style={thS}>유형</th>
                  <th style={thS}>발신 → 착신</th>
                  <th style={{ ...thS, textAlign: 'center' }}>상태</th>
                  <th style={thS}>시작시간</th>
                  <th style={thS}>응답시간</th>
                  <th style={thS}>종료시간</th>
                  <th style={{ ...thS, textAlign: 'right' }}>통화시간</th>
                  <th style={thS}>종료사유</th>
                  <th style={{ ...thS, textAlign: 'center' }}>녹취</th>
                </tr>
              </thead>
              <tbody>
                {loading
                  ? <tr><td colSpan={10} style={{ textAlign: 'center', color: 'var(--text-muted)', padding: 24 }}>로딩 중...</td></tr>
                  : logs.length === 0
                    ? <tr><td colSpan={10} style={{ textAlign: 'center', color: 'var(--text-muted)', padding: 24 }}>이력 없음</td></tr>
                    : logs.map(l => {
                      const isOpen = expandedCall === callKey(l)
                      const st = callState(l.state)
                      const dur = l.answer_time && l.end_time
                        ? Math.max(0, Math.floor((tms(l.end_time) - tms(l.answer_time)) / 1000))
                        : l.duration
                      return (
                        <CallRow
                          key={callKey(l)} l={l} isOpen={isOpen} st={st} dur={dur}
                          flow={flowByCall.get(callKey(l))}
                          onToggle={() => toggleCall(l)}
                          onOpenDiagram={() => setFlow({ callId: l.call_id, date: l.invite_time?.substring(0, 10) || '', callType: 'volte' })}
                          onOpenRec={() => openRecording(l)}
                        />
                      )
                    })}
              </tbody>
            </table>
          </div>

          {/* 페이지네이션 (항상 표시) */}
          <div className="toolbar" style={{ justifyContent: 'flex-end', gap: 8, borderTop: '1px solid var(--border)', flexShrink: 0, paddingTop: 6 }}>
            <span className="ts" style={{ color: 'var(--text-muted)' }}>총 {total.toLocaleString()}건 · {page + 1}/{totalPages}</span>
            <button className="btn btn--sm btn--ghost" disabled={page === 0} onClick={() => { setPage(page - 1); load(page - 1) }}>이전</button>
            <button className="btn btn--sm btn--ghost" disabled={page >= totalPages - 1} onClick={() => { setPage(page + 1); load(page + 1) }}>다음</button>
          </div>
        </div>
      </div>

      {recPlayer && (
        <div className="modal-overlay" onClick={() => setRecPlayer(null)}
          style={{ background: 'rgba(15,23,42,0.32)', backdropFilter: 'blur(2px)' }}>
          <div className="modal-box" style={{ maxWidth: 1100, width: '92vw', background: 'var(--surface, rgba(255,255,255,0.97))', boxShadow: '0 12px 48px rgba(0,0,0,0.3)' }} onClick={e => e.stopPropagation()}>
            <SegmentPlayer segments={recPlayer.segments} recordingId={recPlayer.id} callType={recPlayer.callType} caller={recPlayer.caller} callee={recPlayer.callee} onClose={() => setRecPlayer(null)} />
          </div>
        </div>
      )}

      {flow && <FlowPage callId={flow.callId} date={flow.date} callType={flow.callType} onClose={() => setFlow(null)} />}
    </div>
  )
}

// ════════════════════════════════════════════════════════════════
// 호 단위 accordion 행 (헤더 + 펼침: 녹취 + 다이어그램/메시지/상세)
// ════════════════════════════════════════════════════════════════
const tdS: CSSProperties = { padding: '6px 10px', whiteSpace: 'nowrap' }

function CallRow({ l, isOpen, st, dur, flow, onToggle, onOpenDiagram, onOpenRec }: {
  l: CallLog
  isOpen: boolean
  st: { label: string; cls: string }
  dur: number | null
  flow: CallFlowState | undefined
  onToggle: () => void
  onOpenDiagram: () => void
  onOpenRec: () => void
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
        <td style={tdS} className="ts">{fmtClock(l.answer_time)}</td>
        <td style={tdS} className="ts">{fmtClock(l.end_time)}</td>
        <td style={{ ...tdS, textAlign: 'right' }} className="ts">{fmtDur(dur)}</td>
        <td style={tdS} className="ts">{l.end_reason_ko || l.end_reason || '—'}</td>
        <td style={{ ...tdS, textAlign: 'center' }} onClick={e => e.stopPropagation()}>
          {l.has_recording
            ? <button className="btn btn--sm btn--outline" onClick={onOpenRec}>&#9654; 녹취</button>
            : <span style={{ color: 'var(--text-muted)' }}>—</span>}
        </td>
      </tr>
      {isOpen && (
        <tr>
          <td colSpan={10} style={{ padding: 0, background: 'var(--surface-alt, #fafbfd)', borderTop: '1px solid var(--border)' }}>
            <div style={{ padding: '10px 14px' }}>
              <CallDetailPanel l={l} flow={flow} onOpenDiagram={onOpenDiagram} />
            </div>
          </td>
        </tr>
      )}
    </>
  )
}

// 펼침 패널: 좌[다이어그램↑/메시지목록↓] 우[메시지 상세] (녹취는 행의 녹취 컬럼→별도 dialog)
function CallDetailPanel({ l, flow, onOpenDiagram }: {
  l: CallLog
  flow: CallFlowState | undefined
  onOpenDiagram: () => void
}) {
  const [selIdx, setSelIdx] = useState<number | null>(null)
  const [body, setBody] = useState<string | null>(null)
  const [bodyLoading, setBodyLoading] = useState(false)
  const msgs = useMemo(() => flow?.messages || [], [flow])
  const date = l.invite_time?.substring(0, 10) || ''

  const select = (idx: number) => {
    if (selIdx === idx) { setSelIdx(null); setBody(null); return }
    setSelIdx(idx); setBody(null)
    const m = msgs[idx]; if (!m) return
    if (m.body) { setBody(m.body); return }
    setBodyLoading(true)
    flowApi.getBody(date, hourFromTs(m.ts), m.seq, m.ts, inferDir(m), m.proto, m.iface, nodeOf(m))
      .then(r => setBody(r.body || '(빈 본문)'))
      .catch(() => setBody('(본문 조회 실패)'))
      .finally(() => setBodyLoading(false))
  }

  const dS: CSSProperties = { padding: '3px 8px', whiteSpace: 'nowrap', fontSize: 12 }
  const hS: CSSProperties = { padding: '4px 8px', fontWeight: 600, color: 'var(--text-muted)', textAlign: 'left', fontSize: 11 }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
      {/* 좌(다이어그램/메시지) 우(상세) — 녹취는 행의 녹취 컬럼 버튼으로 */}
      <div style={{ display: 'flex', gap: 12, alignItems: 'stretch', flexWrap: 'wrap' }}>
        {/* 좌 */}
        <div style={{ flex: '1 1 460px', minWidth: 320, display: 'flex', flexDirection: 'column', gap: 8 }}>
          {/* 시퀀스 다이어그램 */}
          <div style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface, #fff)' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '5px 10px', borderBottom: '1px solid var(--border)' }}>
              <span style={{ fontWeight: 600, fontSize: 12 }}>시퀀스 다이어그램</span>
              <button className="btn btn--sm btn--outline" style={{ marginLeft: 'auto', padding: '1px 8px', fontSize: 11 }} onClick={onOpenDiagram}>⛶ 최대화</button>
            </div>
            <div style={{ maxHeight: 230, overflow: 'auto', padding: 6 }}>
              {flow?.loading ? <div className="empty" style={{ padding: 8 }}>로딩 중...</div>
                : msgs.length > 0 ? <SequenceDiagram messages={msgs} selectedIdx={selIdx} onSelect={select} />
                  : <div className="ts" style={{ color: 'var(--text-muted)', padding: 6 }}>메시지 없음</div>}
            </div>
          </div>
          {/* 메시지 이력 */}
          <div style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface, #fff)' }}>
            <div style={{ padding: '5px 10px', borderBottom: '1px solid var(--border)', fontWeight: 600, fontSize: 12 }}>
              메시지 이력 {msgs.length > 0 && <span className="ts" style={{ color: 'var(--text-muted)' }}>{msgs.length}건</span>}
            </div>
            <div style={{ maxHeight: 260, overflowY: 'auto' }}>
              {flow?.loading ? <div className="empty" style={{ padding: 8 }}>로딩 중...</div>
                : msgs.length === 0 ? <div className="ts" style={{ color: 'var(--text-muted)', padding: 8 }}>메시지 없음</div>
                  : (
                    <table className="data-table" style={{ width: '100%', borderCollapse: 'collapse' }}>
                      <thead>
                        <tr style={{ background: 'var(--surface-alt, #f7f9fc)', position: 'sticky', top: 0 }}>
                          <th style={{ ...hS, width: 28, textAlign: 'right' }}>#</th>
                          <th style={hS}>시각</th>
                          <th style={hS}>From → To</th>
                          <th style={hS}>Proto</th>
                          <th style={hS}>Method</th>
                        </tr>
                      </thead>
                      <tbody>
                        {msgs.map((m, i) => {
                          const proto = m.proto || 'SIP'
                          const sel = selIdx === i
                          return (
                            <tr key={i} onClick={() => select(i)}
                              style={{ borderTop: '1px solid var(--border)', cursor: 'pointer', background: sel ? 'var(--hover, #eef5ff)' : undefined }}>
                              <td style={{ ...dS, textAlign: 'right', color: 'var(--text-muted)' }}>{i + 1}</td>
                              <td style={dS} className="ts">{fmtClock(m.ts)}</td>
                              <td style={dS}>{actorLbl(m.from)} <span style={{ color: 'var(--text-muted)' }}>→</span> {actorLbl(m.to)}</td>
                              <td style={dS}><span style={{ fontSize: 9, fontWeight: 700, color: '#fff', background: protoColor(proto), borderRadius: 3, padding: '1px 5px' }}>{proto}</span></td>
                              <td style={{ ...dS, fontWeight: 600, color: protoColor(proto) }}>{m.label || ''}</td>
                            </tr>
                          )
                        })}
                      </tbody>
                    </table>
                  )}
            </div>
          </div>
        </div>

        {/* 우: 메시지 상세 */}
        <div style={{ flex: '1 1 340px', minWidth: 280, border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface, #fff)', display: 'flex', flexDirection: 'column' }}>
          <div style={{ padding: '5px 10px', borderBottom: '1px solid var(--border)', fontWeight: 600, fontSize: 12 }}>
            메시지 상세 {selIdx != null && msgs[selIdx] && <span className="ts" style={{ color: protoColor(msgs[selIdx].proto || 'SIP') }}>· {msgs[selIdx].label}</span>}
          </div>
          <div style={{ flex: 1, overflow: 'auto', minHeight: 200, maxHeight: 508 }}>
            {selIdx == null ? <div className="empty" style={{ padding: 16, fontSize: 12 }}>왼쪽에서 메시지를 선택하세요</div>
              : bodyLoading ? <div className="empty" style={{ padding: 16 }}>본문 로딩 중...</div>
                : <pre style={{ margin: 0, padding: 10, fontSize: 11, lineHeight: 1.5, whiteSpace: 'pre-wrap', wordBreak: 'break-all', fontFamily: 'monospace' }}>{body}</pre>}
          </div>
        </div>
      </div>
    </div>
  )
}
