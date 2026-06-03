import { useState, useEffect, useCallback, type CSSProperties } from 'react'
import { groupsApi, type Group } from '../../../api/groups'
import { pttApi, type PttSession, type PttEvent, type PttFloorEvent, type PttGroupSummary } from '../../../api/ptt'
import { recordingsApi, type RecordingSegment } from '../../../api/recordings'
import type { FlowMessage } from '../../../api/flow'
import FlowPage from '../../../pages/FlowPage'
import SegmentPlayer from '../../../components/SegmentPlayer'
import { useToast } from '../../../components/Toast'

function fmtShortTime(iso: string | null | undefined) {
  if (!iso) return '--'
  const s = iso.replace('T', ' ')
  const idx = s.indexOf(' ')
  return idx >= 0 ? s.substring(idx + 1, idx + 9) : s.substring(0, 8)
}

function fmtDur(seconds: number | null | undefined) {
  if (!seconds || seconds <= 0) return '--'
  const m = Math.floor(seconds / 60)
  const s = seconds % 60
  return m > 0 ? `${m}분 ${s}초` : `${s}초`
}

function fmtSpeechMs(ms: number | null | undefined) {
  if (!ms || ms <= 0) return '--'
  const total = Math.round(ms / 1000)
  const m = Math.floor(total / 60)
  const s = total % 60
  return m > 0 ? `${m}분 ${s}초` : `${s}초`
}

// 'YYYYMMDDHH' → 'MM/DD HH시'
function fmtWindow(w: string | null | undefined) {
  if (!w || w.length < 10) return '--'
  return `${w.slice(4, 6)}/${w.slice(6, 8)} ${w.slice(8, 10)}시`
}

const EVENT_ICONS: Record<string, { icon: string; label: string; color: string }> = {
  session_start:  { icon: '●', label: '세션 시작',  color: '#4caf50' },
  session_end:    { icon: '■', label: '세션 종료',  color: '#f44336' },
  member_join:    { icon: '✚', label: '입장',      color: '#2196f3' },
  member_leave:   { icon: '✖', label: '퇴장',      color: '#ff9800' },
  'floor-grant':  { icon: '▶', label: '발언 시작',  color: '#4caf50' },
  'floor-release':{ icon: '■', label: '발언 종료',  color: 'var(--text-muted)' },
  config_change:  { icon: '⚙', label: '설정 변경',  color: '#9c27b0' },
  member_invite:  { icon: '→', label: '초대',      color: '#00bcd4' },
}

function getEventDisplay(type: string) {
  return EVENT_ICONS[type] || { icon: '•', label: type, color: 'var(--text-muted)' }
}

// floor.jsonl op → 표시 스타일 (TS 24.380)
const FLOOR_OPS: Record<string, { label: string; color: string }> = {
  GRANT:   { label: '발언권 부여', color: '#4caf50' },
  TAKEN:   { label: '발언 시작',  color: '#2196f3' },
  RELEASE: { label: '발언 종료',  color: 'var(--text-muted)' },
  IDLE:    { label: '유휴',      color: 'var(--text-muted)' },
  REVOKE:  { label: '선점 회수',  color: '#ff9800' },
  REJECT:  { label: '거절',      color: '#f44336' },
}

const thStyle: CSSProperties = { padding: '7px 10px', fontWeight: 600, color: 'var(--text-muted)', cursor: 'pointer', userSelect: 'none', whiteSpace: 'nowrap' }
const tdStyle: CSSProperties = { padding: '6px 10px', whiteSpace: 'nowrap' }

interface SessionsState { sessions: PttSession[]; loading: boolean; loaded: boolean }
interface DetailState {
  events: PttEvent[]
  participants: Array<{ msisdn: string; role: string; join_time: string | null; leave_time: string | null }>
  floor: PttFloorEvent[]
  loading: boolean
  loaded: boolean
}

const detailKey = (gid: string, dir: string) => `${gid}|${dir}`

export default function PttHistoryPage() {
  const { show } = useToast()
  const [groups, setGroups] = useState<Group[]>([])
  const [loading, setLoading] = useState(false)
  const [fGroup, setFG] = useState('')
  const [fDate, setFD] = useState(() => new Date().toISOString().substring(0, 10))
  const [autoRefresh, setAR] = useState(false)

  const [selectedGroupId, setSelGroup] = useState<string | null>(null)
  const [selectedSessionDir, setSelSession] = useState<string | null>(null)

  const [sessionsByGroup, setSessionsByGroup] = useState<Map<string, SessionsState>>(new Map())
  const [detailByKey, setDetailByKey] = useState<Map<string, DetailState>>(new Map())
  // 그룹키(ptt_groups.id) → 요약(세션수/최근 시간창)
  const [summaries, setSummaries] = useState<Record<string, PttGroupSummary>>({})
  // 세션 이력 테이블 정렬
  const [sort, setSort] = useState<{ key: keyof PttSession; dir: 'asc' | 'desc' }>({ key: 'dir', dir: 'desc' })

  const [flow, setFlow] = useState<{ groupId: string; sessionDir: string; date: string; nodes?: Record<string, FlowMessage[]>; messages?: FlowMessage[] } | null>(null)
  const [flowLoading, setFlowLoading] = useState(false)
  const [recPlayer, setRecPlayer] = useState<{ id: string; segments: RecordingSegment[]; groupId: string } | null>(null)

  // ── 그룹 로드 ──
  const loadGroups = useCallback(async () => {
    setLoading(true)
    try {
      const gs = await groupsApi.list()
      setGroups(gs)
      setSelGroup(prev => prev ?? (gs.length > 0 ? gs[0].id : null))
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [show])

  useEffect(() => { loadGroups() }, [loadGroups])

  // ── 그룹별 요약(세션수/최근활동) 일괄 로드 ──
  const loadSummaries = useCallback(async () => {
    try {
      const resp = await pttApi.summary()
      setSummaries(resp.summaries || {})
    } catch { /* 요약 실패는 무시 (좌측 보조정보) */ }
  }, [])

  useEffect(() => { loadSummaries() }, [loadSummaries])

  const filteredGroups = fGroup
    ? groups.filter(g => g.id.includes(fGroup) || g.name.includes(fGroup))
    : groups

  // 저장 디렉터리 키 = ptt_groups.id(surrogate). 이력 API 는 이 키로 조회(경로 ptt/{id}/...).
  const histKey = useCallback((gid: string) => {
    const g = groups.find(x => x.id === gid)
    return g && g.db_id != null ? String(g.db_id) : gid
  }, [groups])

  // ── 그룹의 세션 목록 lazy 로드 ──
  const loadSessions = useCallback(async (gid: string, force = false) => {
    if (!force) {
      const cur = sessionsByGroup.get(gid)
      if (cur && cur.loaded) return cur.sessions
    }
    setSessionsByGroup(prev => {
      const m = new Map(prev)
      m.set(gid, { sessions: prev.get(gid)?.sessions ?? [], loading: true, loaded: false })
      return m
    })
    try {
      const resp = await pttApi.sessions(histKey(gid), fDate || undefined)
      const sessions = resp.sessions || []
      setSessionsByGroup(prev => {
        const m = new Map(prev)
        m.set(gid, { sessions, loading: false, loaded: true })
        return m
      })
      return sessions
    } catch {
      setSessionsByGroup(prev => {
        const m = new Map(prev)
        m.set(gid, { sessions: [], loading: false, loaded: true })
        return m
      })
      return []
    }
  }, [sessionsByGroup, fDate, histKey])

  // ── 세션 상세(events + participants + floor) lazy 로드 ──
  const loadDetail = useCallback(async (gid: string, dir: string, force = false) => {
    const key = detailKey(gid, dir)
    if (!force) {
      const cur = detailByKey.get(key)
      if (cur && cur.loaded) return
    }
    setDetailByKey(prev => {
      const m = new Map(prev)
      m.set(key, { events: [], participants: [], floor: [], loading: true, loaded: false })
      return m
    })
    try {
      const [ev, fl] = await Promise.all([
        pttApi.events(histKey(gid), dir, fDate || undefined),
        pttApi.floor(histKey(gid), dir, fDate || undefined).catch(() => ({ floor: [] })),
      ])
      setDetailByKey(prev => {
        const m = new Map(prev)
        m.set(key, {
          events: ev.events || [],
          participants: ev.participants || [],
          floor: fl.floor || [],
          loading: false, loaded: true,
        })
        return m
      })
    } catch {
      setDetailByKey(prev => {
        const m = new Map(prev)
        m.set(key, { events: [], participants: [], floor: [], loading: false, loaded: true })
        return m
      })
    }
  }, [detailByKey, fDate, histKey])

  // 선택 그룹 변경 → 세션 로드 + 최신 세션 자동 선택
  useEffect(() => {
    if (!selectedGroupId) return
    let cancelled = false
    ;(async () => {
      const sessions = await loadSessions(selectedGroupId)
      if (cancelled) return
      setSelSession(sessions.length > 0 ? sessions[0].dir : null)
    })()
    return () => { cancelled = true }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selectedGroupId, fDate])

  // 선택 세션 변경 → 상세 로드
  useEffect(() => {
    if (selectedGroupId && selectedSessionDir) loadDetail(selectedGroupId, selectedSessionDir)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selectedGroupId, selectedSessionDir])

  // 날짜 변경 → 캐시 비우고 선택 그룹 재로드
  useEffect(() => {
    setSessionsByGroup(new Map())
    setDetailByKey(new Map())
  }, [fDate])

  // 자동 갱신 (선택 그룹만)
  useEffect(() => {
    if (!autoRefresh || !selectedGroupId) return
    const iv = setInterval(() => { loadSessions(selectedGroupId, true) }, 15000)
    return () => clearInterval(iv)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [autoRefresh, selectedGroupId])

  const playRecording = async (groupId: string, sessionDir: string) => {
    // sessionDir = 시간창 'YYYYMMDDHH' → ptt/{id}/{YYYY}/{MM}/{DD}/{HH}
    const w = (sessionDir || '').replace(/\D/g, '')
    if (w.length < 10) { show('잘못된 시간창', 'err'); return }
    const recId = `ptt/${histKey(groupId)}/${w.slice(0,4)}/${w.slice(4,6)}/${w.slice(6,8)}/${w.slice(8,10)}`
    try {
      const rec = await recordingsApi.get(recId)
      if (rec.segments && rec.segments.length > 0) {
        setRecPlayer({ id: recId, segments: rec.segments, groupId })
      } else {
        show('녹취 세그먼트 없음', 'err')
      }
    } catch (e: unknown) {
      show(String(e), 'err')
    }
  }

  const openFlow = async (groupId: string, sessionDir: string) => {
    setFlowLoading(true)
    try {
      const resp = await pttApi.flow(histKey(groupId), sessionDir, fDate || undefined)
      setFlow({ groupId, sessionDir, date: fDate, nodes: resp.nodes, messages: resp.messages })
    } catch (e: unknown) {
      show(String(e), 'err')
      setFlow({ groupId, sessionDir, date: fDate })
    } finally {
      setFlowLoading(false)
    }
  }

  const selGroup = groups.find(g => g.id === selectedGroupId) || null
  const selSessionsRaw = selectedGroupId ? (sessionsByGroup.get(selectedGroupId)?.sessions ?? []) : []
  const selSessions = [...selSessionsRaw].sort((a, b) => {
    const va = a[sort.key] ?? ''
    const vb = b[sort.key] ?? ''
    let cmp: number
    if (typeof va === 'number' && typeof vb === 'number') cmp = va - vb
    else cmp = String(va).localeCompare(String(vb))
    return sort.dir === 'asc' ? cmp : -cmp
  })
  const toggleSort = (key: keyof PttSession) =>
    setSort(prev => prev.key === key ? { key, dir: prev.dir === 'asc' ? 'desc' : 'asc' } : { key, dir: 'desc' })
  const sortArrow = (key: keyof PttSession) => sort.key === key ? (sort.dir === 'asc' ? ' ▲' : ' ▼') : ''
  const selSessionsLoading = selectedGroupId ? (sessionsByGroup.get(selectedGroupId)?.loading ?? false) : false
  const selDetail = (selectedGroupId && selectedSessionDir)
    ? detailByKey.get(detailKey(selectedGroupId, selectedSessionDir)) : undefined

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%', minHeight: 480 }}>
      <div className="toolbar">
        <input
          className="search-input"
          placeholder="그룹 ID/이름 필터"
          value={fGroup}
          onChange={e => setFG(e.target.value)}
          style={{ maxWidth: 200 }}
        />
        <input
          type="date"
          className="form-input"
          value={fDate}
          onChange={e => setFD(e.target.value)}
          style={{ width: 150 }}
        />
        <button className="btn btn--primary btn--sm" onClick={() => { if (selectedGroupId) loadSessions(selectedGroupId, true) }}>
          새로고침
        </button>
        <label style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 4, fontSize: 12, color: 'var(--text-muted)', cursor: 'pointer' }}>
          <input type="checkbox" checked={autoRefresh} onChange={e => setAR(e.target.checked)} />
          자동갱신
        </label>
      </div>

      <div style={{ flex: 1, display: 'flex', gap: 12, overflow: 'hidden', minHeight: 0 }}>
        {/* ── 좌: 그룹 리스트 ── */}
        <div style={{ flex: '0 0 300px', overflow: 'auto', border: '1px solid var(--border)', borderRadius: 8, background: 'var(--surface, #fff)' }}>
          {loading ? (
            <div className="empty" style={{ padding: 16 }}>그룹 로딩 중...</div>
          ) : filteredGroups.length === 0 ? (
            <div className="empty" style={{ padding: 16 }}>등록된 그룹이 없습니다</div>
          ) : (
            filteredGroups.map(g => {
              const isSel = g.id === selectedGroupId
              return (
                <div
                  key={g.id}
                  onClick={() => { setSelGroup(g.id); setSelSession(null) }}
                  style={{
                    padding: '10px 14px',
                    cursor: 'pointer',
                    borderBottom: '1px solid var(--border)',
                    background: isSel ? 'var(--hover, #eef5ff)' : 'transparent',
                    borderLeft: isSel ? '3px solid var(--primary, #2563eb)' : '3px solid transparent',
                  }}
                >
                  <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                    <span style={{ fontWeight: 600, fontSize: 13, flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                      {g.name || g.id}
                    </span>
                    <span className={`badge ${g.video_enabled ? 'badge--blue' : 'badge--gray'}`} style={{ fontSize: 10, padding: '1px 6px' }}>
                      {g.video_enabled ? '영상' : '음성'}
                    </span>
                  </div>
                  <div className="ts" style={{ color: 'var(--text-muted)', marginTop: 2 }}>{g.id}</div>
                  {(() => {
                    const sm = summaries[histKey(g.id)]
                    const memberCount = g.members?.length ?? 0
                    return (
                      <div style={{ display: 'flex', flexWrap: 'wrap', gap: 6, marginTop: 4, fontSize: 11, color: 'var(--text-muted)' }}>
                        <span>멤버 {memberCount}명</span>
                        <span>· 세션 {sm?.session_count ?? 0}</span>
                        {sm?.last_window && <span>· 최근 {fmtWindow(sm.last_window)}</span>}
                        {g.authorized_user_name && <span style={{ flexBasis: '100%' }}>소유 {g.authorized_user_name}</span>}
                      </div>
                    )
                  })()}
                </div>
              )
            })
          )}
        </div>

        {/* ── 우: 그룹 상세 ── */}
        <div style={{ flex: 1, overflow: 'auto', border: '1px solid var(--border)', borderRadius: 8, background: 'var(--surface, #fff)', minWidth: 0 }}>
          {!selGroup ? (
            <div className="empty" style={{ padding: 24 }}>왼쪽에서 그룹을 선택하세요</div>
          ) : (
            <div style={{ padding: 16 }}>
              {/* 그룹 헤더 */}
              <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap', marginBottom: 12 }}>
                <span style={{ fontWeight: 700, fontSize: 16 }}>{selGroup.name || selGroup.id}</span>
                <span className="ts" style={{ color: 'var(--text-muted)' }}>({selGroup.id})</span>
                <span className={`badge ${selGroup.video_enabled ? 'badge--blue' : 'badge--gray'}`}>{selGroup.video_enabled ? '영상' : '음성'}</span>
                <span style={{ marginLeft: 'auto', fontSize: 12, color: 'var(--text-muted)' }}>
                  {selSessionsLoading ? '세션 로딩...' : `${selSessions.length}개 세션`}
                </span>
              </div>

              {/* 세션 이력 테이블 */}
              {selSessions.length === 0 && !selSessionsLoading ? (
                <div className="empty" style={{ padding: 16 }}>이 날짜에 세션이 없습니다</div>
              ) : (
                <div style={{ marginBottom: 14, border: '1px solid var(--border)', borderRadius: 8, overflow: 'hidden' }}>
                  <table className="data-table" style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
                    <thead>
                      <tr style={{ background: 'var(--surface-alt, #f7f9fc)', textAlign: 'left' }}>
                        <th onClick={() => toggleSort('dir')} style={thStyle}>시간창{sortArrow('dir')}</th>
                        <th onClick={() => toggleSort('start_time')} style={thStyle}>시작 ~ 종료{sortArrow('start_time')}</th>
                        <th onClick={() => toggleSort('state')} style={{ ...thStyle, textAlign: 'center' }}>상태{sortArrow('state')}</th>
                        <th onClick={() => toggleSort('segment_count')} style={{ ...thStyle, textAlign: 'right' }}>세그먼트{sortArrow('segment_count')}</th>
                        <th onClick={() => toggleSort('speaker_count')} style={{ ...thStyle, textAlign: 'right' }}>화자{sortArrow('speaker_count')}</th>
                        <th onClick={() => toggleSort('total_speech_ms')} style={{ ...thStyle, textAlign: 'right' }}>발화시간{sortArrow('total_speech_ms')}</th>
                      </tr>
                    </thead>
                    <tbody>
                      {selSessions.map(sess => {
                        const isSel = sess.dir === selectedSessionDir
                        return (
                          <tr
                            key={sess.dir}
                            onClick={() => setSelSession(sess.dir)}
                            style={{
                              cursor: 'pointer',
                              borderTop: '1px solid var(--border)',
                              background: isSel ? 'var(--hover, #eef5ff)' : 'transparent',
                            }}
                          >
                            <td style={{ ...tdStyle, fontWeight: 600 }}>{fmtWindow(sess.dir)}</td>
                            <td style={tdStyle} className="ts">
                              {fmtShortTime(sess.start_time)} ~ {sess.state === 'active' ? 'active' : fmtShortTime(sess.end_time)}
                            </td>
                            <td style={{ ...tdStyle, textAlign: 'center' }}>
                              <span className={`badge ${sess.state === 'active' ? 'badge--green' : 'badge--gray'}`}>{sess.state === 'active' ? '진행중' : '종료'}</span>
                            </td>
                            <td style={{ ...tdStyle, textAlign: 'right' }}>{sess.segment_count ?? 0}</td>
                            <td style={{ ...tdStyle, textAlign: 'right' }}>{sess.speaker_count ?? 0}</td>
                            <td style={{ ...tdStyle, textAlign: 'right' }} className="ts">{fmtSpeechMs(sess.total_speech_ms)}</td>
                          </tr>
                        )
                      })}
                    </tbody>
                  </table>
                </div>
              )}

              {/* 선택 세션 상세 */}
              {selectedSessionDir && (
                <div style={{ borderTop: '1px solid var(--border)', paddingTop: 12 }}>
                  <div style={{ display: 'flex', gap: 6, marginBottom: 10 }}>
                    <button className="btn btn--sm btn--outline" disabled={flowLoading} onClick={() => openFlow(selGroup.id, selectedSessionDir)}>Flow 보기</button>
                    <button className="btn btn--sm btn--outline" onClick={() => playRecording(selGroup.id, selectedSessionDir)}>&#9654; 녹취</button>
                  </div>

                  {selDetail?.loading ? (
                    <div className="empty" style={{ padding: 12 }}>상세 로딩 중...</div>
                  ) : (
                    <div style={{ display: 'flex', gap: 16, flexWrap: 'wrap' }}>
                      {/* Floor 타임라인 */}
                      <div style={{ flex: '1 1 320px', minWidth: 280 }}>
                        <div style={{ fontWeight: 600, fontSize: 13, marginBottom: 6 }}>발언권(Floor) 타임라인</div>
                        {selDetail && selDetail.floor.length > 0 ? (
                          <div style={{ maxHeight: 320, overflowY: 'auto', border: '1px solid var(--border)', borderRadius: 6, padding: 8 }}>
                            {selDetail.floor.map((f, i) => {
                              const st = FLOOR_OPS[f.op] || { label: f.op, color: 'var(--text-muted)' }
                              return (
                                <div key={i} style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '3px 0', fontSize: 12 }}>
                                  <span className="ts" style={{ minWidth: 78 }}>{fmtShortTime(f.ts)}</span>
                                  <span style={{ color: st.color, fontWeight: 600, minWidth: 72 }}>{st.label}</span>
                                  <span style={{ color: 'var(--text, #1a1d2e)' }}>
                                    {f.user || '-'}
                                    {f.prio != null && f.prio >= 0 && <span className="ts"> (prio {f.prio})</span>}
                                    {f.preempt && <span className="ts" style={{ color: '#ff9800' }}> ← 선점 {f.preempted_from || ''}</span>}
                                    {f.op === 'REVOKE' && f.preempted_by && <span className="ts" style={{ color: '#ff9800' }}> → {f.preempted_by}</span>}
                                    {f.op === 'REJECT' && f.owner && <span className="ts"> (점유: {f.owner})</span>}
                                  </span>
                                </div>
                              )
                            })}
                          </div>
                        ) : (
                          <div className="ts" style={{ color: 'var(--text-muted)' }}>floor 기록 없음</div>
                        )}
                      </div>

                      {/* 이벤트 + 참여자 */}
                      <div style={{ flex: '1 1 320px', minWidth: 280 }}>
                        <div style={{ fontWeight: 600, fontSize: 13, marginBottom: 6 }}>세션 이벤트</div>
                        {selDetail && selDetail.events.length > 0 ? (
                          <div style={{ maxHeight: 200, overflowY: 'auto', border: '1px solid var(--border)', borderRadius: 6, padding: 8, marginBottom: 12 }}>
                            {selDetail.events.map((ev, ei) => {
                              const disp = getEventDisplay(ev.type)
                              return (
                                <div key={ei} style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '3px 0', fontSize: 12 }}>
                                  <span className="ts" style={{ minWidth: 78 }}>{fmtShortTime(ev.ts)}</span>
                                  <span style={{ color: disp.color, fontSize: 14, width: 18, textAlign: 'center' }}>{disp.icon}</span>
                                  <span style={{ color: 'var(--text, #1a1d2e)' }}>
                                    {ev.member && <span style={{ fontWeight: 500 }}>{ev.member} </span>}
                                    {disp.label}
                                    {ev.duration != null && <span className="ts"> ({fmtDur(ev.duration)})</span>}
                                  </span>
                                </div>
                              )
                            })}
                          </div>
                        ) : (
                          <div className="ts" style={{ color: 'var(--text-muted)', marginBottom: 12 }}>이벤트 없음</div>
                        )}

                        <div style={{ fontWeight: 600, fontSize: 13, marginBottom: 6 }}>참여자</div>
                        {selDetail && selDetail.participants.length > 0 ? (
                          <div style={{ border: '1px solid var(--border)', borderRadius: 6, padding: 8, maxHeight: 160, overflowY: 'auto' }}>
                            {selDetail.participants.map((p, pi) => (
                              <div key={pi} style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '2px 0', fontSize: 12 }}>
                                <span style={{ fontWeight: 500 }}>{p.msisdn}</span>
                                {p.role && <span className="badge badge--gray" style={{ fontSize: 10 }}>{p.role}</span>}
                                <span className="ts" style={{ marginLeft: 'auto' }}>{fmtShortTime(p.join_time)}{p.leave_time ? ` ~ ${fmtShortTime(p.leave_time)}` : ''}</span>
                              </div>
                            ))}
                          </div>
                        ) : (
                          <div className="ts" style={{ color: 'var(--text-muted)' }}>참여자 없음</div>
                        )}
                      </div>
                    </div>
                  )}
                </div>
              )}
            </div>
          )}
        </div>
      </div>

      {/* PTT 녹취 SegmentPlayer 팝업 */}
      {recPlayer && (
        <div className="modal-overlay" onClick={() => setRecPlayer(null)}>
          <div className="modal-box" style={{ width: 800, maxWidth: 'calc(100vw - 40px)' }} onClick={e => e.stopPropagation()}>
            <SegmentPlayer
              segments={recPlayer.segments}
              recordingId={recPlayer.id}
              callType="ptt"
              onClose={() => setRecPlayer(null)}
            />
          </div>
        </div>
      )}

      {/* Flow Modal */}
      {flow && (
        <FlowPage
          callId={flow.groupId}
          date={flow.date}
          callType="ptt"
          onClose={() => setFlow(null)}
          prefetchedNodes={flow.nodes}
          prefetchedMessages={flow.messages}
        />
      )}
    </div>
  )
}
