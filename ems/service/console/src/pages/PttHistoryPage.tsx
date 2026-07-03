import { useState, useEffect, useCallback, useMemo, Fragment, type CSSProperties } from 'react'
import { groupsApi, type Group } from '@core/api/groups'
import { pttApi, type PttSession, type PttEvent, type PttFloorEvent, type PttGroupSummary } from '@core/api/ptt'
import { recordingsApi, type RecordingSegment } from '@core/api/recordings'
import type { FlowMessage } from '@core/api/flow'
import FlowPage from '@core/pages/FlowPage'
import SegmentPlayer from '@core/components/SegmentPlayer'
import { useInlineAudio, type InlineAudio } from '@core/components/useInlineAudio'
import { useToast } from '@core/components/Toast'

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

// 'YYYYMMDD' → 'MM/DD'
function fmtDayShort(d: string) {
  return d.length >= 8 ? `${d.slice(4, 6)}/${d.slice(6, 8)}` : d
}

// 'YYYYMMDD' → 'YYYY-MM-DD' (flow/events 의 date 파라미터용)
function dateOf(dirOrDay: string): string {
  const d = (dirOrDay || '').replace(/\D/g, '')
  return d.length >= 8 ? `${d.slice(0, 4)}-${d.slice(4, 6)}-${d.slice(6, 8)}` : ''
}

const WEEKDAY = ['일', '월', '화', '수', '목', '금', '토']
function dayWeekday(d: string): string {
  if (d.length < 8) return ''
  const dt = new Date(Number(d.slice(0, 4)), Number(d.slice(4, 6)) - 1, Number(d.slice(6, 8)))
  return WEEKDAY[dt.getDay()] || ''
}

// ISO 타임스탬프 → {hh, mm}. (segments.start_time / floor.ts / events.ts 공통 ISO)
function parseHM(iso: string | null | undefined): { hh: number; mm: number } {
  if (!iso) return { hh: -1, mm: -1 }
  const t = iso.includes('T') ? iso.split('T')[1] : iso
  const hh = Number(t.slice(0, 2))
  const mm = Number(t.slice(3, 5))
  return { hh: Number.isFinite(hh) ? hh : -1, mm: Number.isFinite(mm) ? mm : -1 }
}
// 분 → 10분 슬롯(0/10/20/30/40/50)
const slotOfMin = (mm: number) => (mm < 0 ? -1 : Math.floor(mm / 10) * 10)

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

// 발언자 색 팔레트 (히트맵 막대/타임바/발언자 헤더 공통)
const SPK_COLORS = ['#2563eb', '#16a34a', '#d97706', '#9333ea', '#dc2626', '#0891b2', '#ca8a04', '#db2777', '#4f46e5', '#059669', '#e11d48', '#0d9488']
function spkColor(order: string[], id: string) {
  const i = order.indexOf(id)
  return SPK_COLORS[(i < 0 ? 0 : i) % SPK_COLORS.length]
}

const thStyle: CSSProperties = { padding: '7px 10px', fontWeight: 600, color: 'var(--text-muted)', cursor: 'pointer', userSelect: 'none', whiteSpace: 'nowrap' }
const tdStyle: CSSProperties = { padding: '6px 10px', whiteSpace: 'nowrap' }

const RANGE_OPTIONS = [5, 10, 20, 30]

// 시간창(YYYYMMDDHH) → 녹취 recId (ptt/{histKey}/{YYYY}/{MM}/{DD}/{HH})
function recIdOf(histKey: string, dir: string): string | null {
  const w = (dir || '').replace(/\D/g, '')
  if (w.length < 10) return null
  return `ptt/${histKey}/${w.slice(0, 4)}/${w.slice(4, 6)}/${w.slice(6, 8)}/${w.slice(8, 10)}`
}

interface SessionsState { sessions: PttSession[]; loading: boolean; loaded: boolean }
interface DetailState {
  events: PttEvent[]
  participants: Array<{ msisdn: string; role: string; join_time: string | null; leave_time: string | null }>
  floor: PttFloorEvent[]
  segments: RecordingSegment[]
  loading: boolean
  loaded: boolean
}

// 일별 집계 (일별 히트맵용)
interface DayAgg {
  day: string            // YYYYMMDD
  segs: number
  speakers: number       // 해당 일 시간버킷 화자수 합(근사)
  ms: number
  active: boolean
  hasData: boolean
}

const detailKey = (gid: string, dir: string) => `${gid}|${dir}`

export default function PttHistoryPage() {
  const { show } = useToast()
  const audio = useInlineAudio(useCallback((m: string) => show(m, 'err'), [show]))

  const [groups, setGroups] = useState<Group[]>([])
  const [loading, setLoading] = useState(false)
  const [rangeDays, setRangeDays] = useState(10)
  const [autoRefresh, setAR] = useState(false)

  const [selectedGroupId, setSelGroup] = useState<string | null>(null)
  const [selectedDay, setSelDay] = useState<string | null>(null)         // YYYYMMDD (일별 히트맵 드릴다운)
  const [selectedSessionDir, setSelSession] = useState<string | null>(null)  // YYYYMMDDHH (시간 행 펼침)

  const [sessionsByGroup, setSessionsByGroup] = useState<Map<string, SessionsState>>(new Map())
  const [detailByKey, setDetailByKey] = useState<Map<string, DetailState>>(new Map())
  const [summaries, setSummaries] = useState<Record<string, PttGroupSummary>>({})
  const [sort, setSort] = useState<{ key: keyof PttSession; dir: 'asc' | 'desc' }>({ key: 'dir', dir: 'desc' })

  const [flow, setFlow] = useState<{ groupId: string; sessionDir: string; date: string; nodes?: Record<string, FlowMessage[]>; messages?: FlowMessage[] } | null>(null)
  const [flowLoading, setFlowLoading] = useState(false)
  const [recPlayer, setRecPlayer] = useState<{ id: string; segments: RecordingSegment[]; groupId: string; title?: string } | null>(null)

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

  const loadSummaries = useCallback(async () => {
    try {
      const resp = await pttApi.summary()
      setSummaries(resp.summaries || {})
    } catch { /* 요약 실패는 무시 (좌측 보조정보) */ }
  }, [])

  useEffect(() => { loadSummaries() }, [loadSummaries])

  // 저장 디렉터리 키 = ptt_groups.id(surrogate)
  const histKey = useCallback((gid: string) => {
    const g = groups.find(x => x.id === gid)
    return g && g.db_id != null ? String(g.db_id) : gid
  }, [groups])

  // ── 그룹의 세션(시간버킷, 최근 rangeDays 일) lazy 로드 ──
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
      const resp = await pttApi.sessions(histKey(gid), { days: rangeDays })
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
  }, [sessionsByGroup, rangeDays, histKey])

  // ── 세션 상세(events + participants + floor + 녹취 segments) lazy 로드 ──
  const loadDetail = useCallback(async (gid: string, dir: string, force = false) => {
    const key = detailKey(gid, dir)
    if (!force) {
      const cur = detailByKey.get(key)
      if (cur && cur.loaded) return
    }
    setDetailByKey(prev => {
      const m = new Map(prev)
      m.set(key, { events: [], participants: [], floor: [], segments: [], loading: true, loaded: false })
      return m
    })
    const recId = recIdOf(histKey(gid), dir)
    const dt = dateOf(dir) || undefined
    try {
      const [ev, fl, rec] = await Promise.all([
        pttApi.events(histKey(gid), dir, dt),
        pttApi.floor(histKey(gid), dir, dt).catch(() => ({ floor: [] })),
        recId ? recordingsApi.get(recId).catch(() => ({ segments: [] })) : Promise.resolve({ segments: [] }),
      ])
      setDetailByKey(prev => {
        const m = new Map(prev)
        m.set(key, {
          events: ev.events || [],
          participants: ev.participants || [],
          floor: fl.floor || [],
          segments: (rec as { segments?: RecordingSegment[] }).segments || [],
          loading: false, loaded: true,
        })
        return m
      })
    } catch {
      setDetailByKey(prev => {
        const m = new Map(prev)
        m.set(key, { events: [], participants: [], floor: [], segments: [], loading: false, loaded: true })
        return m
      })
    }
  }, [detailByKey, histKey])

  const selGroup = groups.find(g => g.id === selectedGroupId) || null

  // ── 선택 그룹의 시간버킷 전체(범위 내) ──
  const allSessions = useMemo(
    () => (selectedGroupId ? (sessionsByGroup.get(selectedGroupId)?.sessions ?? []) : []),
    [selectedGroupId, sessionsByGroup],
  )

  // ── 최근 rangeDays 일 일별 집계 (빈 일자 포함, 오래된→최신) ──
  const dayAggs = useMemo<DayAgg[]>(() => {
    const byDay = new Map<string, DayAgg>()
    for (const s of allSessions) {
      const day = s.dir.slice(0, 8)
      const cur = byDay.get(day) || { day, segs: 0, speakers: 0, ms: 0, active: false, hasData: false }
      cur.segs += s.segment_count ?? 0
      cur.speakers += s.speaker_count ?? 0
      cur.ms += s.total_speech_ms ?? 0
      cur.active = cur.active || s.state === 'active'
      cur.hasData = true
      byDay.set(day, cur)
    }
    // rangeDays 만큼 연속 일자 생성 (오늘 포함)
    const out: DayAgg[] = []
    const today = new Date()
    for (let i = rangeDays - 1; i >= 0; i--) {
      const d = new Date(today)
      d.setDate(today.getDate() - i)
      const key = `${d.getFullYear()}${String(d.getMonth() + 1).padStart(2, '0')}${String(d.getDate()).padStart(2, '0')}`
      out.push(byDay.get(key) || { day: key, segs: 0, speakers: 0, ms: 0, active: false, hasData: false })
    }
    return out
  }, [allSessions, rangeDays])

  // 선택 그룹/범위 변경 → 세션 로드 + 활동 있는 최신 일자 자동선택
  useEffect(() => {
    if (!selectedGroupId) return
    let cancelled = false
    ;(async () => {
      const sessions = await loadSessions(selectedGroupId)
      if (cancelled) return
      const days = Array.from(new Set(sessions.map(s => s.dir.slice(0, 8)))).sort()
      const latest = days.length > 0 ? days[days.length - 1] : null
      setSelDay(latest)
    })()
    return () => { cancelled = true }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selectedGroupId, rangeDays])

  // 선택 일자의 시간버킷 (정렬 적용)
  const dayHourSessions = useMemo(() => {
    if (!selectedDay) return []
    const filtered = allSessions.filter(s => s.dir.startsWith(selectedDay))
    return [...filtered].sort((a, b) => {
      const va = a[sort.key] ?? ''
      const vb = b[sort.key] ?? ''
      const cmp = typeof va === 'number' && typeof vb === 'number'
        ? va - vb : String(va).localeCompare(String(vb))
      return sort.dir === 'asc' ? cmp : -cmp
    })
  }, [allSessions, selectedDay, sort])

  // 선택 일자 변경 → 그 날 최신 시간버킷 자동 펼침
  useEffect(() => {
    if (!selectedDay) { setSelSession(null); return }
    const day = allSessions.filter(s => s.dir.startsWith(selectedDay)).sort((a, b) => b.dir.localeCompare(a.dir))
    setSelSession(day.length > 0 ? day[0].dir : null)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selectedDay])

  // 펼친 버킷 변경 → 상세 로드 + 진행중 재생 정지
  useEffect(() => {
    audio.stop()
    if (selectedGroupId && selectedSessionDir) loadDetail(selectedGroupId, selectedSessionDir)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selectedGroupId, selectedSessionDir])

  // 범위 변경 → 캐시 비움
  useEffect(() => {
    setSessionsByGroup(new Map())
    setDetailByKey(new Map())
  }, [rangeDays])

  // 자동 갱신 (선택 그룹만)
  useEffect(() => {
    if (!autoRefresh || !selectedGroupId) return
    const iv = setInterval(() => { loadSessions(selectedGroupId, true) }, 15000)
    return () => clearInterval(iv)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [autoRefresh, selectedGroupId])

  // 녹취 재생 (시간 전체 또는 10분 슬롯 부분집합)
  const openRecPlayer = (groupId: string, sessionDir: string, segs: RecordingSegment[], title?: string) => {
    const recId = recIdOf(histKey(groupId), sessionDir)
    if (!recId) { show('잘못된 시간창', 'err'); return }
    const playable = segs.filter(s => s.status !== 'recording')
    if (playable.length === 0) { show('녹취 세그먼트 없음', 'err'); return }
    setRecPlayer({ id: recId, segments: playable, groupId, title })
  }

  const playRecording = async (groupId: string, sessionDir: string) => {
    const recId = recIdOf(histKey(groupId), sessionDir)
    if (!recId) { show('잘못된 시간창', 'err'); return }
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

  // Flow 열기 — slot(10분창) 지정 시 해당 시간대 메시지만 필터링
  const openFlow = async (groupId: string, sessionDir: string, slot?: { hh: number; min: number }) => {
    setFlowLoading(true)
    const dt = dateOf(sessionDir)
    try {
      const resp = await pttApi.flow(histKey(groupId), sessionDir, dt || undefined)
      let nodes = resp.nodes
      let messages = resp.messages
      if (slot) {
        const lo = `${String(slot.hh).padStart(2, '0')}:${String(slot.min).padStart(2, '0')}:00`
        const hi = `${String(slot.hh).padStart(2, '0')}:${String(slot.min + 9).padStart(2, '0')}:59.999999`
        const inWin = (m: FlowMessage) => (m.ts || '') >= lo && (m.ts || '') <= hi
        if (nodes) {
          const f: Record<string, FlowMessage[]> = {}
          for (const [k, arr] of Object.entries(nodes)) f[k] = arr.filter(inWin)
          nodes = f
        }
        if (messages) messages = messages.filter(inWin)
      }
      setFlow({ groupId, sessionDir, date: dt, nodes, messages })
    } catch (e: unknown) {
      show(String(e), 'err')
      setFlow({ groupId, sessionDir, date: dt })
    } finally {
      setFlowLoading(false)
    }
  }

  const toggleSort = (key: keyof PttSession) =>
    setSort(prev => prev.key === key ? { key, dir: prev.dir === 'asc' ? 'desc' : 'asc' } : { key, dir: 'desc' })
  const sortArrow = (key: keyof PttSession) => sort.key === key ? (sort.dir === 'asc' ? ' ▲' : ' ▼') : ''
  const selSessionsLoading = selectedGroupId ? (sessionsByGroup.get(selectedGroupId)?.loading ?? false) : false

  const toggleExpand = (dir: string) => setSelSession(prev => prev === dir ? null : dir)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%', minHeight: 480 }}>
      {/* ── 툴바: 최근 N일 범위 선택 ── */}
      <div className="toolbar">
        <span style={{ fontSize: 12, color: 'var(--text-muted)', fontWeight: 600 }}>최근</span>
        <div style={{ display: 'flex', gap: 2 }}>
          {RANGE_OPTIONS.map(d => (
            <button
              key={d}
              className={`btn btn--sm ${rangeDays === d ? 'btn--primary' : 'btn--ghost'}`}
              onClick={() => setRangeDays(d)}
            >
              {d}일
            </button>
          ))}
        </div>
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
          ) : groups.length === 0 ? (
            <div className="empty" style={{ padding: 16 }}>등록된 그룹이 없습니다</div>
          ) : (
            groups.map(g => {
              const isSel = g.id === selectedGroupId
              return (
                <div
                  key={g.id}
                  onClick={() => { setSelGroup(g.id); setSelDay(null); setSelSession(null) }}
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
                  {selSessionsLoading ? '세션 로딩...' : `최근 ${rangeDays}일${selectedDay ? ` · ${fmtDayShort(selectedDay)} ${dayHourSessions.length}개 시간대` : ''}`}
                </span>
              </div>

              {/* ── ① 일별 히트맵 (최근 N일) ── */}
              <DayHeatmap days={dayAggs} selectedDay={selectedDay} onPick={setSelDay} />

              {selectedDay ? (
                <>
                  {/* ── 시간별 히트맵 (선택 일자) ── */}
                  <div style={{ marginTop: 14 }}>
                    <ActivityHeatmap
                      sessions={dayHourSessions}
                      selectedDir={selectedSessionDir}
                      onPick={toggleExpand}
                    />
                  </div>

                  {/* ── 시간버킷 accordion 리스트 ── */}
                  {dayHourSessions.length === 0 && !selSessionsLoading ? (
                    <div className="empty" style={{ padding: 16 }}>이 날짜에 세션이 없습니다</div>
                  ) : (
                    <div style={{ marginTop: 14, border: '1px solid var(--border)', borderRadius: 8, overflow: 'hidden' }}>
                      <table className="data-table" style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
                        <thead>
                          <tr style={{ background: 'var(--surface-alt, #f7f9fc)', textAlign: 'left' }}>
                            <th style={{ ...thStyle, width: 24, cursor: 'default' }}></th>
                            <th onClick={() => toggleSort('dir')} style={thStyle}>시간대{sortArrow('dir')}</th>
                            <th onClick={() => toggleSort('start_time')} style={thStyle}>시작 ~ 종료{sortArrow('start_time')}</th>
                            <th onClick={() => toggleSort('state')} style={{ ...thStyle, textAlign: 'center' }}>상태{sortArrow('state')}</th>
                            <th onClick={() => toggleSort('segment_count')} style={{ ...thStyle, textAlign: 'right' }}>발언{sortArrow('segment_count')}</th>
                            <th onClick={() => toggleSort('speaker_count')} style={{ ...thStyle, textAlign: 'right' }}>화자{sortArrow('speaker_count')}</th>
                            <th onClick={() => toggleSort('total_speech_ms')} style={{ ...thStyle, textAlign: 'right' }}>발화시간{sortArrow('total_speech_ms')}</th>
                            <th style={{ ...thStyle, cursor: 'default', textAlign: 'right' }}></th>
                          </tr>
                        </thead>
                        <tbody>
                          {dayHourSessions.map(sess => {
                            const isOpen = sess.dir === selectedSessionDir
                            const detail = selectedGroupId ? detailByKey.get(detailKey(selectedGroupId, sess.dir)) : undefined
                            return (
                              <BucketRow
                                key={sess.dir}
                                sess={sess}
                                isOpen={isOpen}
                                detail={detail}
                                histKey={histKey(selGroup.id)}
                                audio={audio}
                                flowLoading={flowLoading}
                                onToggle={() => toggleExpand(sess.dir)}
                                onFlow={() => openFlow(selGroup.id, sess.dir)}
                                onPlayAll={() => playRecording(selGroup.id, sess.dir)}
                                onSlotFlow={(hh, min) => openFlow(selGroup.id, sess.dir, { hh, min })}
                                onSlotPlay={(segs, title) => openRecPlayer(selGroup.id, sess.dir, segs, title)}
                              />
                            )
                          })}
                        </tbody>
                      </table>
                    </div>
                  )}
                </>
              ) : (
                <div className="empty" style={{ padding: 20, marginTop: 8 }}>위 일별 히트맵에서 날짜를 선택하세요</div>
              )}
            </div>
          )}
        </div>
      </div>

      {/* 공유 인라인 오디오 element */}
      {audio.node}

      {/* PTT 녹취 SegmentPlayer 팝업 */}
      {recPlayer && (
        <div className="modal-overlay" onClick={() => setRecPlayer(null)}>
          <div className="modal-box" style={{ width: 800, maxWidth: 'calc(100vw - 40px)' }} onClick={e => e.stopPropagation()}>
            {recPlayer.title && <div style={{ fontWeight: 600, fontSize: 13, marginBottom: 8 }}>{recPlayer.title}</div>}
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

// ════════════════════════════════════════════════════════════════
// ① 일별 히트맵 — 최근 N일 발화량 색농도 (클릭 → 일자 드릴다운)
// ════════════════════════════════════════════════════════════════
function DayHeatmap({ days, selectedDay, onPick }: {
  days: DayAgg[]
  selectedDay: string | null
  onPick: (day: string) => void
}) {
  const [metric, setMetric] = useState<'segs' | 'speakers'>('segs')
  const valOf = (d: DayAgg) => (metric === 'segs' ? d.segs : d.speakers)
  const max = Math.max(1, ...days.map(valOf))
  // 30일도 한 행에 들어가도록 셀 최소폭 자동
  return (
    <div style={{ marginBottom: 4 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 4 }}>
        <span style={{ fontWeight: 600, fontSize: 12, color: 'var(--text-muted)' }}>일별 활동</span>
        <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>색 진할수록 많음 · 클릭→해당 일 시간대 보기</span>
        <span style={{ marginLeft: 'auto' }}>
          <button className={`btn btn--sm ${metric === 'segs' ? 'btn--primary' : 'btn--ghost'}`} onClick={() => setMetric('segs')}>발언수</button>
          <button className={`btn btn--sm ${metric === 'speakers' ? 'btn--primary' : 'btn--ghost'}`} onClick={() => setMetric('speakers')}>화자수</button>
        </span>
      </div>
      <div style={{ display: 'grid', gridTemplateColumns: `repeat(${days.length}, minmax(0, 1fr))`, gap: 3 }}>
        {days.map(d => {
          const v = valOf(d)
          const ratio = v > 0 ? 0.2 + 0.8 * (v / max) : 0
          const isSel = d.day === selectedDay
          return (
            <div key={d.day} onClick={() => onPick(d.day)}
              title={`${fmtDayShort(d.day)}(${dayWeekday(d.day)}) · 발언 ${d.segs} · 화자 ${d.speakers} · ${fmtSpeechMs(d.ms)}${d.active ? ' · 진행중' : ''}`}
              style={{
                height: 48, borderRadius: 4,
                background: d.hasData ? `rgba(37,99,235,${ratio || 0.12})` : 'var(--surface-alt, #f3f5f9)',
                border: isSel ? '2px solid var(--primary, #2563eb)' : '1px solid var(--border)',
                cursor: 'pointer', opacity: d.hasData ? 1 : 0.65,
                display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center',
                fontSize: 10, color: ratio > 0.55 ? '#fff' : 'var(--text-muted)', position: 'relative', overflow: 'hidden',
              }}>
              <span style={{ fontSize: 11, fontWeight: 700 }}>{v > 0 ? v : ''}</span>
              <span style={{ fontSize: 9, opacity: 0.85 }}>{fmtDayShort(d.day)}</span>
              {d.active && <span style={{ position: 'absolute', top: 2, right: 2, width: 5, height: 5, borderRadius: '50%', background: '#fff' }} />}
            </div>
          )
        })}
      </div>
    </div>
  )
}

// ════════════════════════════════════════════════════════════════
// 시간별 히트맵 — 선택 일자의 24시간 발화량 색농도 (클릭 → 시간버킷 펼침)
// ════════════════════════════════════════════════════════════════
function ActivityHeatmap({ sessions, selectedDir, onPick }: {
  sessions: PttSession[]
  selectedDir: string | null
  onPick: (dir: string) => void
}) {
  const [metric, setMetric] = useState<'segment_count' | 'speaker_count'>('segment_count')
  const byHour = new Map<number, PttSession>()
  for (const s of sessions) byHour.set(Number(s.dir.slice(8, 10)), s)
  const valOf = (s?: PttSession) => (s ? (metric === 'segment_count' ? (s.segment_count ?? 0) : (s.speaker_count ?? 0)) : 0)
  const max = Math.max(1, ...sessions.map(s => valOf(s)))
  return (
    <div style={{ marginBottom: 4 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 4 }}>
        <span style={{ fontWeight: 600, fontSize: 12, color: 'var(--text-muted)' }}>시간대별 활동</span>
        <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>색 진할수록 많음 · 숫자=값 · 클릭→펼치기</span>
        <span style={{ marginLeft: 'auto' }}>
          <button className={`btn btn--sm ${metric === 'segment_count' ? 'btn--primary' : 'btn--ghost'}`} onClick={() => setMetric('segment_count')}>발언수</button>
          <button className={`btn btn--sm ${metric === 'speaker_count' ? 'btn--primary' : 'btn--ghost'}`} onClick={() => setMetric('speaker_count')}>화자수</button>
        </span>
      </div>
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(24, 1fr)', gap: 3 }}>
        {Array.from({ length: 24 }, (_, h) => {
          const sess = byHour.get(h)
          const v = valOf(sess)
          const ratio = v > 0 ? 0.2 + 0.8 * (v / max) : 0
          const active = sess?.state === 'active'
          const isSel = sess && sess.dir === selectedDir
          return (
            <div key={h} onClick={() => sess && onPick(sess.dir)}
              title={sess
                ? `${String(h).padStart(2, '0')}시 · 발언 ${sess.segment_count ?? 0} · 화자 ${sess.speaker_count ?? 0} · ${fmtSpeechMs(sess.total_speech_ms ?? 0)}${active ? ' · 진행중' : ''}`
                : `${String(h).padStart(2, '0')}시 · 활동 없음`}
              style={{
                height: 34, borderRadius: 4,
                background: sess ? `rgba(37,99,235,${ratio || 0.12})` : 'var(--surface-alt, #f3f5f9)',
                border: isSel ? '2px solid var(--primary, #2563eb)' : '1px solid var(--border)',
                cursor: sess ? 'pointer' : 'default', opacity: sess ? 1 : 0.5,
                display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center',
                fontSize: 10, color: ratio > 0.55 ? '#fff' : 'var(--text-muted)', position: 'relative',
              }}>
              <span style={{ fontSize: 11, fontWeight: 600 }}>{v > 0 ? v : ''}</span>
              <span style={{ fontSize: 8, opacity: 0.8 }}>{String(h).padStart(2, '0')}</span>
              {active && <span style={{ position: 'absolute', top: 2, right: 2, width: 5, height: 5, borderRadius: '50%', background: '#fff' }} />}
            </div>
          )
        })}
      </div>
    </div>
  )
}

// ════════════════════════════════════════════════════════════════
// 시간버킷 accordion 행 (헤더 + 펼침 = 10분 슬롯 상세)
// ════════════════════════════════════════════════════════════════
function BucketRow({ sess, isOpen, detail, histKey, audio, flowLoading, onToggle, onFlow, onPlayAll, onSlotFlow, onSlotPlay }: {
  sess: PttSession
  isOpen: boolean
  detail: DetailState | undefined
  histKey: string
  audio: InlineAudio
  flowLoading: boolean
  onToggle: () => void
  onFlow: () => void
  onPlayAll: () => void
  onSlotFlow: (hh: number, min: number) => void
  onSlotPlay: (segs: RecordingSegment[], title: string) => void
}) {
  const recId = recIdOf(histKey, sess.dir)
  const hourNum = Number(sess.dir.slice(8, 10))
  return (
    <>
      <tr
        onClick={onToggle}
        style={{
          cursor: 'pointer',
          borderTop: '1px solid var(--border)',
          background: isOpen ? 'var(--hover, #eef5ff)' : 'transparent',
        }}
      >
        <td style={{ ...tdStyle, textAlign: 'center', color: 'var(--text-muted)' }}>{isOpen ? '▾' : '▸'}</td>
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
        <td style={{ ...tdStyle, textAlign: 'right' }} onClick={e => e.stopPropagation()}>
          <button className="btn btn--sm btn--outline" style={{ marginRight: 4 }} disabled={flowLoading} onClick={onFlow}>Flow</button>
          <button className="btn btn--sm btn--outline" onClick={onPlayAll}>&#9654; 전체</button>
        </td>
      </tr>
      {isOpen && (
        <tr>
          <td colSpan={8} style={{ padding: 0, background: 'var(--surface-alt, #fafbfd)', borderTop: '1px solid var(--border)' }}>
            <div style={{ padding: '12px 16px' }}>
              {!detail || detail.loading ? (
                <div className="empty" style={{ padding: 12 }}>상세 로딩 중...</div>
              ) : (
                <BucketDetail
                  detail={detail}
                  recId={recId}
                  hourNum={hourNum}
                  audio={audio}
                  flowLoading={flowLoading}
                  onSlotFlow={onSlotFlow}
                  onSlotPlay={onSlotPlay}
                />
              )}
            </div>
          </td>
        </tr>
      )}
    </>
  )
}

// ── 통합 타임라인 아이템 ──
type TLItem =
  | { t: number; ts: string; kind: 'seg'; seg: RecordingSegment }
  | { t: number; ts: string; kind: 'floor'; floor: PttFloorEvent }
  | { t: number; ts: string; kind: 'event'; ev: PttEvent }

function tms(iso: string | null | undefined): number {
  const n = Date.parse(iso || '')
  return Number.isFinite(n) ? n : 0
}

// 10분 슬롯 묶음
interface SlotGroup {
  min: number              // 0/10/20/30/40/50
  segs: RecordingSegment[]
  floor: PttFloorEvent[]
  events: PttEvent[]
  speakers: Set<string>
  ms: number
}

// 펼친 시간버킷 상세: floor 타임바(시간 전체) + 10분 슬롯 하위 테이블
function BucketDetail({ detail, recId, hourNum, audio, flowLoading, onSlotFlow, onSlotPlay }: {
  detail: DetailState
  recId: string | null
  hourNum: number
  audio: InlineAudio
  flowLoading: boolean
  onSlotFlow: (hh: number, min: number) => void
  onSlotPlay: (segs: RecordingSegment[], title: string) => void
}) {
  // 발언자 등장 순서 (색 배정 기준 — 타임바/타임라인 공통, 시간 전체)
  const speakerOrder = useMemo(() => {
    const seen: string[] = []
    for (const s of detail.segments) {
      if (s.speaker_id && !seen.includes(s.speaker_id)) seen.push(s.speaker_id)
    }
    return seen
  }, [detail.segments])

  // ── 10분 슬롯 그룹핑 (타임스탬프 분 기준) ──
  const slots = useMemo<SlotGroup[]>(() => {
    const map = new Map<number, SlotGroup>()
    const ensure = (min: number) => {
      let g = map.get(min)
      if (!g) { g = { min, segs: [], floor: [], events: [], speakers: new Set(), ms: 0 }; map.set(min, g) }
      return g
    }
    for (const s of detail.segments) {
      const slot = slotOfMin(parseHM(s.start_time).mm)
      if (slot < 0) continue
      const g = ensure(slot)
      g.segs.push(s)
      if (s.speaker_id) g.speakers.add(s.speaker_id)
      g.ms += s.duration_ms || 0
    }
    for (const f of detail.floor) {
      const slot = slotOfMin(parseHM(f.ts).mm)
      if (slot < 0) continue
      ensure(slot).floor.push(f)
    }
    for (const ev of detail.events) {
      const slot = slotOfMin(parseHM(ev.ts).mm)
      if (slot < 0) continue
      ensure(slot).events.push(ev)
    }
    return Array.from(map.values()).sort((a, b) => a.min - b.min)
  }, [detail])

  const hh2 = String(hourNum).padStart(2, '0')

  // 슬롯 자동 펼침: 슬롯이 1개뿐이면 펼쳐둠 (마운트 시 1회 결정)
  const [openSlot, setOpenSlot] = useState<number | null>(() => (slots.length === 1 ? slots[0].min : null))

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
      {/* floor 미니 타임바 (시간 전체 개요) */}
      <FloorTimebar
        segments={detail.segments}
        speakerOrder={speakerOrder}
        recId={recId}
        audio={audio}
      />

      {/* 발언자 색 범례 */}
      {speakerOrder.length > 0 && (
        <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
          <span style={{ fontWeight: 600, fontSize: 13 }}>10분 단위</span>
          {speakerOrder.map(spk => (
            <span key={spk} style={{ display: 'inline-flex', alignItems: 'center', gap: 4, fontSize: 11, color: 'var(--text-muted)' }}>
              <span style={{ width: 9, height: 9, borderRadius: 2, background: spkColor(speakerOrder, spk) }} />
              {spk}
            </span>
          ))}
        </div>
      )}

      {/* ── 10분 슬롯 하위 테이블 ── */}
      {slots.length === 0 ? (
        <div className="ts" style={{ color: 'var(--text-muted)' }}>표시할 항목이 없습니다</div>
      ) : (
        <div style={{ border: '1px solid var(--border)', borderRadius: 6, overflow: 'hidden', background: 'var(--surface, #fff)' }}>
          <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
            <thead>
              <tr style={{ background: 'var(--surface-alt, #f7f9fc)', textAlign: 'left', color: 'var(--text-muted)' }}>
                <th style={{ ...thStyle, width: 22, cursor: 'default' }}></th>
                <th style={{ ...thStyle, cursor: 'default' }}>구간(10분)</th>
                <th style={{ ...thStyle, cursor: 'default', textAlign: 'right' }}>발언</th>
                <th style={{ ...thStyle, cursor: 'default', textAlign: 'right' }}>화자</th>
                <th style={{ ...thStyle, cursor: 'default', textAlign: 'right' }}>발화시간</th>
                <th style={{ ...thStyle, cursor: 'default', textAlign: 'right' }}></th>
              </tr>
            </thead>
            <tbody>
              {slots.map(slot => {
                const isOpen = openSlot === slot.min
                const m0 = String(slot.min).padStart(2, '0')
                const m9 = String(slot.min + 9).padStart(2, '0')
                const playable = slot.segs.filter(s => s.status !== 'recording')
                return (
                  <Fragment key={slot.min}>
                    <tr
                      onClick={() => setOpenSlot(prev => prev === slot.min ? null : slot.min)}
                      style={{ cursor: 'pointer', borderTop: '1px solid var(--border)', background: isOpen ? 'var(--hover, #eef5ff)' : 'transparent' }}
                    >
                      <td style={{ ...tdStyle, textAlign: 'center', color: 'var(--text-muted)' }}>{isOpen ? '▾' : '▸'}</td>
                      <td style={{ ...tdStyle, fontWeight: 600 }} className="ts">{hh2}:{m0} ~ {hh2}:{m9}</td>
                      <td style={{ ...tdStyle, textAlign: 'right' }}>{slot.segs.length}</td>
                      <td style={{ ...tdStyle, textAlign: 'right' }}>{slot.speakers.size}</td>
                      <td style={{ ...tdStyle, textAlign: 'right' }} className="ts">{fmtSpeechMs(slot.ms)}</td>
                      <td style={{ ...tdStyle, textAlign: 'right' }} onClick={e => e.stopPropagation()}>
                        <button className="btn btn--sm btn--outline" style={{ marginRight: 4 }} disabled={flowLoading} onClick={() => onSlotFlow(hourNum, slot.min)}>Flow</button>
                        <button className="btn btn--sm btn--outline" disabled={playable.length === 0} onClick={() => onSlotPlay(playable, `${hh2}:${m0}~${hh2}:${m9} 녹취`)}>&#9654; 재생</button>
                      </td>
                    </tr>
                    {isOpen && (
                      <tr>
                        <td colSpan={6} style={{ padding: 0, background: 'var(--surface-alt, #fafbfd)', borderTop: '1px solid var(--border)' }}>
                          <div style={{ padding: '10px 12px' }}>
                            <SlotTimeline slot={slot} speakerOrder={speakerOrder} participants={detail.participants} recId={recId} audio={audio} />
                          </div>
                        </td>
                      </tr>
                    )}
                  </Fragment>
                )
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}

// ── 10분 슬롯 통합 타임라인 (발언 인라인재생 + 발언권 + 이벤트) ──
function SlotTimeline({ slot, speakerOrder, participants, recId, audio }: {
  slot: SlotGroup
  speakerOrder: string[]
  participants: Array<{ msisdn: string; role: string; join_time: string | null; leave_time: string | null }>
  recId: string | null
  audio: InlineAudio
}) {
  const [layers, setLayers] = useState({ seg: true, floor: true, event: true })
  const roleOf = (msisdn: string) => participants.find(p => p.msisdn === msisdn)?.role

  const timeline = useMemo<TLItem[]>(() => {
    const items: TLItem[] = []
    for (const seg of slot.segs) items.push({ t: tms(seg.start_time), ts: seg.start_time, kind: 'seg', seg })
    for (const f of slot.floor) items.push({ t: tms(f.ts), ts: f.ts, kind: 'floor', floor: f })
    for (const ev of slot.events) items.push({ t: tms(ev.ts), ts: ev.ts, kind: 'event', ev })
    items.sort((a, b) => a.t - b.t)
    return items
  }, [slot])

  const counts = { seg: slot.segs.length, floor: slot.floor.length, event: slot.events.length }
  const shown = timeline.filter(it => layers[it.kind])
  const chips: Array<{ key: 'seg' | 'floor' | 'event'; label: string; color: string }> = [
    { key: 'seg', label: '발언', color: '#2563eb' },
    { key: 'floor', label: '발언권', color: '#16a34a' },
    { key: 'event', label: '이벤트', color: '#9333ea' },
  ]

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
        <span style={{ fontWeight: 600, fontSize: 12, color: 'var(--text-muted)' }}>타임라인</span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 4 }}>
          {chips.map(c => (
            <button
              key={c.key}
              onClick={() => setLayers(l => ({ ...l, [c.key]: !l[c.key] }))}
              className="btn btn--sm"
              style={{
                padding: '2px 8px', fontSize: 11,
                border: `1px solid ${layers[c.key] ? c.color : 'var(--border)'}`,
                background: layers[c.key] ? c.color : 'transparent',
                color: layers[c.key] ? '#fff' : 'var(--text-muted)',
              }}
            >
              {c.label} {counts[c.key]}
            </button>
          ))}
        </div>
      </div>

      {shown.length === 0 ? (
        <div className="ts" style={{ color: 'var(--text-muted)' }}>표시할 항목이 없습니다</div>
      ) : (
        <div style={{ maxHeight: 360, overflowY: 'auto', border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface, #fff)' }}>
          {shown.map((it, i) => {
            const border = i > 0 ? '1px solid var(--border)' : undefined
            if (it.kind === 'seg') {
              const seg = it.seg
              const color = spkColor(speakerOrder, seg.speaker_id)
              const isPlaying = audio.playing?.recId === recId && audio.playing?.seq === seg.seq
              const isPrep = audio.preparing?.recId === recId && audio.preparing?.seq === seg.seq
              const playable = seg.status !== 'recording'
              const role = roleOf(seg.speaker_id)
              return (
                <div key={`s${seg.seq}`} style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '6px 10px', fontSize: 12, borderTop: border, borderLeft: `4px solid ${color}`, background: isPlaying ? 'var(--hover, #eef5ff)' : undefined }}>
                  <span className="ts" style={{ minWidth: 70, color: 'var(--text-muted)' }}>{fmtShortTime(seg.start_time)}</span>
                  <button
                    className={`btn btn--sm ${isPlaying ? 'btn--primary' : 'btn--outline'}`}
                    disabled={!recId || !playable}
                    style={{ minWidth: 30, padding: '2px 6px' }}
                    onClick={() => recId && audio.play(recId, seg.seq)}
                    title={playable ? '재생/정지' : '녹취중'}
                  >
                    {isPrep ? '…' : isPlaying ? '❚❚' : '▶'}
                  </button>
                  <span style={{ fontWeight: 600, color }}>{seg.speaker_id}</span>
                  <span style={{ color: 'var(--text-muted)' }}>발언</span>
                  {role && <span className="badge badge--gray" style={{ fontSize: 9 }}>{role}</span>}
                  {seg.has_video && <span className="badge badge--blue" style={{ fontSize: 9 }}>영상</span>}
                  {seg.status === 'recording' && <span className="badge badge--blue" style={{ fontSize: 9 }}>녹취중</span>}
                  {seg.status === 'failed' && <span className="badge badge--red" style={{ fontSize: 9 }}>실패</span>}
                  <span className="ts" style={{ marginLeft: 'auto', color: 'var(--text-muted)' }}>{fmtSpeechMs(seg.duration_ms)}</span>
                </div>
              )
            }
            if (it.kind === 'floor') {
              const f = it.floor
              const st = FLOOR_OPS[f.op] || { label: f.op, color: 'var(--text-muted)' }
              const uColor = f.user ? spkColor(speakerOrder, f.user) : 'var(--text-muted)'
              return (
                <div key={`f${i}`} style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '4px 10px', fontSize: 12, borderTop: border, borderLeft: '4px solid transparent', color: 'var(--text-muted)' }}>
                  <span className="ts" style={{ minWidth: 70 }}>{fmtShortTime(f.ts)}</span>
                  <span style={{ minWidth: 30, textAlign: 'center', color: st.color }}>◆</span>
                  <span style={{ color: st.color, fontWeight: 600, minWidth: 70 }}>{st.label}</span>
                  <span style={{ color: uColor, fontWeight: f.user ? 600 : 400 }}>{f.user || '-'}</span>
                  {f.prio != null && f.prio >= 0 && <span className="ts">prio {f.prio}</span>}
                  {f.preempt && <span className="ts" style={{ color: '#ff9800' }}>← 선점 {f.preempted_from || ''}</span>}
                  {f.op === 'REVOKE' && f.preempted_by && <span className="ts" style={{ color: '#ff9800' }}>→ {f.preempted_by}</span>}
                  {f.op === 'REVOKE' && f.reason != null && <span className="ts" style={{ color: '#ff9800' }}>({String(f.reason)})</span>}
                  {f.op === 'REJECT' && f.owner && <span className="ts">(점유: {f.owner})</span>}
                </div>
              )
            }
            const ev = it.ev
            const disp = getEventDisplay(ev.type)
            return (
              <div key={`e${i}`} style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '4px 10px', fontSize: 12, borderTop: border, borderLeft: '4px solid transparent' }}>
                <span className="ts" style={{ minWidth: 70, color: 'var(--text-muted)' }}>{fmtShortTime(ev.ts)}</span>
                <span style={{ minWidth: 30, textAlign: 'center', color: disp.color, fontSize: 14 }}>{disp.icon}</span>
                <span style={{ color: 'var(--text, #1a1d2e)' }}>
                  {ev.member && <span style={{ fontWeight: 500 }}>{ev.member} </span>}
                  {disp.label}
                  {ev.type === 'member_join' && ev.role === 'initiator' &&
                    <span className="badge badge--gray" style={{ fontSize: 9, marginLeft: 6 }}>개시자</span>}
                  {ev.duration != null && <span className="ts"> ({fmtDur(ev.duration)})</span>}
                </span>
              </div>
            )
          })}
        </div>
      )}
    </div>
  )
}

// ════════════════════════════════════════════════════════════════
// floor 미니 타임바 — 실제 발언 구간(첫 발언~마지막 발언)에 맞춰 발언자별 색구간 (클릭=재생)
// ════════════════════════════════════════════════════════════════
function FloorTimebar({ segments, speakerOrder, recId, audio }: {
  segments: RecordingSegment[]
  speakerOrder: string[]
  recId: string | null
  audio: InlineAudio
}) {
  const times = segments
    .map(s => ({ st: tms(s.start_time), en: Math.max(tms(s.start_time), tms(s.end_time) || (tms(s.start_time) + (s.duration_ms || 0))) }))
    .filter(x => x.st > 0)
  if (!times.length) return null
  const spanStart = Math.min(...times.map(x => x.st))
  const spanEnd = Math.max(...times.map(x => x.en))
  const span = Math.max(1000, spanEnd - spanStart)
  const fmtClock = (ms: number) => new Date(ms).toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })

  return (
    <div>
      <div style={{ display: 'flex', alignItems: 'baseline', gap: 8, marginBottom: 6 }}>
        <span style={{ fontWeight: 600, fontSize: 13 }}>발언권 타임라인</span>
        <span className="ts" style={{ color: 'var(--text-muted)' }}>{fmtClock(spanStart)} ~ {fmtClock(spanEnd)} · {fmtSpeechMs(span)}</span>
      </div>
      <div style={{ position: 'relative', height: 34, borderRadius: 6, background: 'var(--surface, #fff)', border: '1px solid var(--border)', overflow: 'hidden' }}>
        {[25, 50, 75].map(p => (
          <div key={p} style={{ position: 'absolute', left: `${p}%`, top: 0, bottom: 0, width: 1, background: 'var(--border)' }} />
        ))}
        {segments.map(seg => {
          const st = tms(seg.start_time)
          if (st <= 0) return null
          const left = ((st - spanStart) / span) * 100
          const width = Math.max(0.6, ((seg.duration_ms || 0) / span) * 100)
          const color = spkColor(speakerOrder, seg.speaker_id)
          const isPlaying = audio.playing?.recId === recId && audio.playing?.seq === seg.seq
          return (
            <div
              key={seg.seq}
              onClick={() => recId && seg.status !== 'recording' && audio.play(recId, seg.seq)}
              title={`${seg.speaker_id} · ${fmtShortTime(seg.start_time)} · ${fmtSpeechMs(seg.duration_ms)}`}
              style={{
                position: 'absolute',
                left: `${Math.max(0, Math.min(99.4, left))}%`,
                width: `${width}%`,
                top: 5, bottom: 5,
                minWidth: 3,
                background: color,
                opacity: isPlaying ? 1 : 0.78,
                borderRadius: 2,
                cursor: recId ? 'pointer' : 'default',
                boxShadow: isPlaying ? '0 0 0 2px var(--primary, #2563eb)' : undefined,
              }}
            />
          )
        })}
      </div>
      <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: 10, color: 'var(--text-muted)', marginTop: 2 }}>
        <span>{fmtClock(spanStart)}</span>
        <span>{fmtClock(spanStart + span / 2)}</span>
        <span>{fmtClock(spanEnd)}</span>
      </div>
    </div>
  )
}
