import { useState, useEffect, useCallback } from 'react'
import { groupsApi, type Group } from '../../../api/groups'
import { pttApi, type PttSession, type PttEvent } from '../../../api/ptt'
import { recordingsApi, type RecordingSegment } from '../../../api/recordings'
import type { FlowMessage } from '../../../api/flow'
import FlowPage from '../../../pages/FlowPage'
import SegmentPlayer from '../../../components/SegmentPlayer'
import { useToast } from '../../../components/Toast'

export function fmtTime(iso: string | null | undefined) {
  if (!iso) return '--'
  // Handle both ISO and compact formats
  const s = iso.replace('T', ' ')
  return s.substring(0, 19)
}

function fmtShortTime(iso: string | null | undefined) {
  if (!iso) return '--'
  const s = iso.replace('T', ' ')
  // Extract HH:MM:SS
  const idx = s.indexOf(' ')
  return idx >= 0 ? s.substring(idx + 1, idx + 9) : s.substring(0, 8)
}

function fmtDur(seconds: number | null | undefined) {
  if (!seconds || seconds <= 0) return '--'
  const m = Math.floor(seconds / 60)
  const s = seconds % 60
  return m > 0 ? `${m}분 ${s}초` : `${s}초`
}

const EVENT_ICONS: Record<string, { icon: string; label: string; color: string }> = {
  session_start:  { icon: '\u25CF', label: '세션 시작',  color: '#4caf50' },
  session_end:    { icon: '\u25A0', label: '세션 종료',  color: '#f44336' },
  member_join:    { icon: '\u271A', label: '입장',      color: '#2196f3' },
  member_leave:   { icon: '\u2716', label: '퇴장',      color: '#ff9800' },
  'floor-grant':  { icon: '\u25B6', label: '발언 시작',  color: '#4caf50' },
  'floor-release':{ icon: '\u25A0', label: '발언 종료',  color: '#9e9e9e' },
  config_change:  { icon: '\u2699', label: '설정 변경',  color: '#9c27b0' },
  member_invite:  { icon: '\u2192', label: '초대',      color: '#00bcd4' },
}

function getEventDisplay(type: string) {
  return EVENT_ICONS[type] || { icon: '\u2022', label: type, color: '#7a8fa8' }
}

interface GroupCard {
  group: Group
  sessions: PttSession[]
  events: PttEvent[]
  loading: boolean
  expanded: boolean
}

export default function PttHistoryPage() {
  const { show } = useToast()
  const [groups, setGroups] = useState<Group[]>([])
  const [cards, setCards] = useState<Map<string, GroupCard>>(new Map())
  const [loading, setLoading] = useState(false)
  const [fGroup, setFG] = useState('')
  const [fDate, setFD] = useState(() => new Date().toISOString().substring(0, 10))
  const [autoRefresh, setAR] = useState(false)
  const [flow, setFlow] = useState<{ groupId: string; sessionDir: string; date: string; nodes?: Record<string, FlowMessage[]>; messages?: FlowMessage[] } | null>(null)
  const [flowLoading, setFlowLoading] = useState(false)

  // Recording playback state (SegmentPlayer 팝업)
  const [recPlayer, setRecPlayer] = useState<{id:string, segments:RecordingSegment[], groupId:string}|null>(null)

  // Load groups
  const loadGroups = useCallback(async () => {
    setLoading(true)
    try {
      const gs = await groupsApi.list()
      setGroups(gs)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [show])

  useEffect(() => { loadGroups() }, [loadGroups])

  // Load sessions for visible groups
  const loadSessions = useCallback(async (targetGroups: Group[]) => {
    const newCards = new Map(cards)

    for (const g of targetGroups) {
      const existing = newCards.get(g.id)
      if (existing && !existing.expanded) continue

      newCards.set(g.id, {
        group: g,
        sessions: existing?.sessions ?? [],
        events: existing?.events ?? [],
        loading: true,
        expanded: existing?.expanded ?? true,
      })
    }
    setCards(new Map(newCards))

    for (const g of targetGroups) {
      try {
        const resp = await pttApi.sessions(g.id, fDate || undefined)
        const sessions = resp.sessions || []

        // Load events for the most recent session
        let events: PttEvent[] = []
        if (sessions.length > 0) {
          const latest = sessions[0]
          try {
            const evResp = await pttApi.events(g.id, latest.dir, fDate || undefined)
            events = evResp.events || []
          } catch { /* ignore */ }
        }

        newCards.set(g.id, {
          group: g,
          sessions,
          events,
          loading: false,
          expanded: newCards.get(g.id)?.expanded ?? true,
        })
      } catch {
        newCards.set(g.id, {
          group: g,
          sessions: [],
          events: [],
          loading: false,
          expanded: newCards.get(g.id)?.expanded ?? true,
        })
      }
    }
    setCards(new Map(newCards))
  }, [cards, fDate])

  // Filter groups
  const filteredGroups = fGroup
    ? groups.filter(g => g.id.includes(fGroup) || g.name.includes(fGroup))
    : groups

  // Initial load of sessions when groups or date changes
  useEffect(() => {
    if (filteredGroups.length > 0) {
      loadSessions(filteredGroups)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [groups, fDate, fGroup])

  // Auto-refresh
  useEffect(() => {
    if (!autoRefresh) return
    const iv = setInterval(() => {
      if (filteredGroups.length > 0) loadSessions(filteredGroups)
    }, 15000)
    return () => clearInterval(iv)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [autoRefresh, filteredGroups])

  const toggleCard = (gid: string) => {
    const c = cards.get(gid)
    if (c) {
      const updated = new Map(cards)
      updated.set(gid, { ...c, expanded: !c.expanded })
      setCards(updated)
    }
  }

  // Play PTT session recording (SegmentPlayer 팝업)
  const playRecording = async (groupId: string, sessionDir: string) => {
    // 녹취 디렉터리: ptt/{groupId}/sessions/{sessionDir}.d
    const dirName = sessionDir.endsWith('.d') ? sessionDir : `${sessionDir}.d`
    const recId = `ptt/${groupId}/sessions/${dirName}`
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
      const resp = await pttApi.flow(groupId, sessionDir, fDate || undefined)
      setFlow({ groupId, sessionDir, date: fDate, nodes: resp.nodes, messages: resp.messages })
    } catch (e: unknown) {
      show(String(e), 'err')
      setFlow({ groupId, sessionDir, date: fDate })
    } finally {
      setFlowLoading(false)
    }
  }

  return (
    <div>
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
        <button className="btn btn--primary btn--sm" onClick={() => loadSessions(filteredGroups)}>
          검색
        </button>
        <label style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 4, fontSize: 12, color: 'var(--text-muted)', cursor: 'pointer' }}>
          <input type="checkbox" checked={autoRefresh} onChange={e => setAR(e.target.checked)} />
          자동갱신
        </label>
      </div>

      {loading ? (
        <div className="empty">그룹 로딩 중...</div>
      ) : filteredGroups.length === 0 ? (
        <div className="empty">등록된 그룹이 없습니다</div>
      ) : (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 12, padding: '8px 0' }}>
          {filteredGroups.map(g => {
            const card = cards.get(g.id)
            const sessions = card?.sessions ?? []
            const events = card?.events ?? []
            const isExpanded = card?.expanded ?? true
            const isCardLoading = card?.loading ?? false

            return (
              <div
                key={g.id}
                style={{
                  border: '1px solid var(--border)',
                  borderRadius: 8,
                  background: 'var(--surface, #ffffff)',
                  overflow: 'hidden',
                }}
              >
                {/* Card Header */}
                <div
                  style={{
                    padding: '10px 16px',
                    display: 'flex',
                    alignItems: 'center',
                    gap: 12,
                    cursor: 'pointer',
                    background: '#f0f4f8',
                    borderBottom: isExpanded ? '1px solid var(--border)' : 'none',
                  }}
                  onClick={() => toggleCard(g.id)}
                >
                  <span style={{ fontSize: 10, color: 'var(--text-muted)', transition: 'transform 0.2s', transform: isExpanded ? 'rotate(90deg)' : 'rotate(0)' }}>
                    {'\u25B6'}
                  </span>
                  <span style={{ fontWeight: 600, fontSize: 14 }}>
                    {g.name || g.id}
                  </span>
                  <span className={`badge ${g.video_enabled ? 'badge--blue' : 'badge--gray'}`}
                        style={{ fontSize: 10, padding: '1px 6px' }}>
                    {g.video_enabled ? '영상' : '음성'}
                  </span>
                  <span className="ts" style={{ color: 'var(--text-muted)' }}>
                    ({g.id})
                  </span>
                  <span style={{ marginLeft: 'auto', fontSize: 12, color: 'var(--text-muted)' }}>
                    {sessions.length > 0
                      ? `${sessions.length}개 세션`
                      : '세션 없음'}
                  </span>
                  {isCardLoading && (
                    <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>로딩...</span>
                  )}
                </div>

                {/* Card Body */}
                {isExpanded && (
                  <div style={{ padding: '12px 16px' }}>
                    {sessions.length === 0 && !isCardLoading ? (
                      <div style={{ color: 'var(--text-muted)', fontSize: 13, textAlign: 'center', padding: 16 }}>
                        세션 없음
                      </div>
                    ) : (
                      sessions.map((sess, si) => {
                        const isLatest = si === 0
                        return (
                          <div
                            key={sess.dir}
                            style={{
                              marginBottom: si < sessions.length - 1 ? 12 : 0,
                              padding: 12,
                              borderRadius: 6,
                              border: '1px solid var(--border)',
                              background: isLatest ? '#f7f9fc' : 'transparent',
                            }}
                          >
                            {/* Session header */}
                            <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 8, flexWrap: 'wrap' }}>
                              <span style={{ fontWeight: 600, fontSize: 13 }}>
                                {sess.session_id || sess.dir}
                              </span>
                              <span className="ts">
                                ({fmtShortTime(sess.start_time)} ~ {sess.state === 'active' ? 'active' : fmtShortTime(sess.end_time)})
                              </span>
                              <span className={`badge ${sess.state === 'active' ? 'badge--green' : 'badge--gray'}`}>
                                {sess.state === 'active' ? '진행중' : '종료'}
                              </span>
                              {sess.initiator && (
                                <span className="ts">발신: {sess.initiator}</span>
                              )}
                              {sess.member_count != null && (
                                <span className="ts">멤버: {sess.member_count}명</span>
                              )}
                              {isLatest && events.length > 0 && (
                                <span className="ts">이벤트: {events.length}건</span>
                              )}
                              <div style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
                                <button
                                  className="btn btn--sm btn--outline"
                                  disabled={flowLoading}
                                  onClick={e => { e.stopPropagation(); openFlow(g.id, sess.dir) }}
                                >
                                  Flow 보기
                                </button>
                                <button
                                  className="btn btn--sm btn--outline"
                                  onClick={e => { e.stopPropagation(); playRecording(g.id, sess.dir) }}
                                >
                                  &#9654; 녹취
                                </button>
                              </div>
                            </div>

                            {/* Event timeline (only for latest session) */}
                            {isLatest && events.length > 0 && (
                              <div style={{
                                maxHeight: 280,
                                overflowY: 'auto',
                                borderTop: '1px solid var(--border)',
                                paddingTop: 8,
                              }}>
                                {events.map((ev, ei) => {
                                  const disp = getEventDisplay(ev.type)
                                  return (
                                    <div
                                      key={ei}
                                      style={{
                                        display: 'flex',
                                        alignItems: 'center',
                                        gap: 8,
                                        padding: '3px 0',
                                        fontSize: 12,
                                        lineHeight: '18px',
                                      }}
                                    >
                                      <span className="ts" style={{ minWidth: 64 }}>
                                        {fmtShortTime(ev.ts)}
                                      </span>
                                      <span style={{ color: disp.color, fontSize: 14, width: 18, textAlign: 'center' }}>
                                        {disp.icon}
                                      </span>
                                      <span style={{ color: 'var(--text, #1a1d2e)' }}>
                                        {ev.member && <span style={{ fontWeight: 500 }}>{ev.member} </span>}
                                        {disp.label}
                                        {ev.duration != null && (
                                          <span className="ts"> ({fmtDur(ev.duration)})</span>
                                        )}
                                      </span>
                                    </div>
                                  )
                                })}
                              </div>
                            )}

                            {/* Recording section removed - use inline player via 녹취 button */}
                          </div>
                        )
                      })
                    )}
                  </div>
                )}
              </div>
            )
          })}
        </div>
      )}

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
