import { useState, useEffect, useCallback, useMemo, Fragment, type CSSProperties } from 'react'
import { groupsApi, type Group } from '@core/api/groups'
import {
  pttApi, type PttSession, type PttEvent, type PttFloorEvent,
  type PttGroupSummary, type PttSessionKind,
} from '@core/api/ptt'
import { recordingsApi, type RecordingSegment } from '@core/api/recordings'
import type { FlowMessage } from '@core/api/flow'
import FlowPage from '@core/pages/FlowPage'
import SegmentPlayer from '@core/components/SegmentPlayer'
import DuplexCallPlayer from '@core/components/DuplexCallPlayer'
import { useInlineAudio, samePlay, type InlineAudio } from '@core/components/useInlineAudio'
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

function fmtMmss(ms: number | null | undefined) {
  const total = Math.max(0, Math.round((ms || 0) / 1000))
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, '0')}`
}

// epoch ms → 로컬 HH:MM:SS. 녹취 타임스탬프는 타임존 없는 로컬 ISO 라
// Date.parse 는 로컬로 읽는다 — 다시 문자열로 만들 때 UTC(toISOString)로 가면 시각이 밀린다.
function fmtClockMs(ms: number) {
  return new Date(ms).toLocaleTimeString('ko-KR', {
    hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit',
  })
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

// floor.jsonl op → 표시 스타일 (TS 24.380). CMP 가 기록하는 8종 전부를 다룬다 —
// GRANT/RELEASE/IDLE/REVOKE/REVOKE_END/QUEUE/QUEUE_CANCEL/DENY.
const FLOOR_OPS: Record<string, { label: string; color: string }> = {
  GRANT:        { label: '발언권 부여', color: '#16a34a' },
  RELEASE:      { label: '발언 종료',  color: 'var(--text-muted)' },
  IDLE:         { label: '유휴',      color: 'var(--text-muted)' },
  REVOKE:       { label: '회수 통지',  color: '#d97706' },
  REVOKE_END:   { label: '회수 확정',  color: '#dc2626' },
  QUEUE:        { label: '대기열 등록', color: '#0891b2' },
  QUEUE_CANCEL: { label: '대기 취소',  color: 'var(--text-muted)' },
  DENY:         { label: '거절',      color: '#dc2626' },
}

// DENY reason(CMP) → 한국어. 규격상 거절 사유가 이력에서 읽혀야 한다.
const DENY_REASON: Record<string, string> = {
  recv_only: '수신전용(ambient)',
  only_one:  '참가자 1인',
  broadcast: 'broadcast 비개시자',
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

// 시간창(YYYYMMDDHH) → 녹취 recId (ptt/{저장키}/{YYYY}/{MM}/{DD}/{HH})
function recIdOf(storeKey: string, dir: string): string | null {
  const w = (dir || '').replace(/\D/g, '')
  if (w.length < 10) return null
  return `ptt/${storeKey}/${w.slice(0, 4)}/${w.slice(4, 6)}/${w.slice(6, 8)}/${w.slice(8, 10)}`
}

// ── 발언 턴 ─────────────────────────────────────────────────────
// 한 화자가 한 슬롯 트랙을 점유한 구간. 동시 발언 세그먼트는 턴이 여럿이고,
// 선점 회수로 슬롯이 재사용되면 같은 트랙에서도 턴이 갈린다.
interface Turn {
  seq: number
  slot: number
  spk: string
  start: number      // epoch ms
  end: number
  durMs: number
  hasVideo: boolean
  playable: boolean
  /** 이 세그먼트에 턴이 여럿인가(동시 발언·슬롯 재사용). 단일 턴이면 슬롯 단독본을 따로
   *  만들 이유가 없어 믹스본(=종전 seg_NNNN.mp4)을 그대로 쓴다 — 변환·캐시 중복 방지. */
  multi: boolean
}

/** 재생 URL 의 slot 파라미터 — 단일 턴 세그먼트는 믹스(undefined) */
const playSlot = (t: Turn) => (t.multi ? t.slot : undefined)

function tms(iso: string | null | undefined): number {
  const n = Date.parse(iso || '')
  return Number.isFinite(n) ? n : 0
}

/** 세그먼트 → 발언 턴 목록. tracks 가 없는 구 녹취는 대표 화자 1턴으로 환원. */
function segTurns(seg: RecordingSegment): Turn[] {
  const base = tms(seg.start_time)
  const playable = seg.status !== 'recording'
  const audio = (seg.tracks || []).filter(t => t.kind === 'audio')
  if (audio.length === 0) {
    return [{
      seq: seg.seq, slot: 0, spk: seg.speaker_id, start: base,
      end: base + (seg.duration_ms || 0), durMs: seg.duration_ms || 0,
      hasVideo: !!seg.has_video, playable, multi: false,
    }]
  }
  const out: Turn[] = []
  for (const t of audio) {
    const spans = t.speakers?.length ? t.speakers : [{ id: seg.speaker_id, offset_ms: 0, dur_ms: seg.duration_ms || 0 }]
    for (const sp of spans) {
      out.push({
        seq: seg.seq, slot: t.slot, spk: sp.id || seg.speaker_id,
        start: base + (sp.offset_ms || 0),
        end: base + (sp.offset_ms || 0) + (sp.dur_ms || 0),
        durMs: sp.dur_ms || 0,
        hasVideo: !!t.has_video,
        playable: playable && t.status !== 'recording',
        multi: false,
      })
    }
  }
  out.sort((a, b) => a.start - b.start || a.slot - b.slot)
  if (out.length > 1) for (const t of out) t.multi = true
  return out
}

/** 겹침 구간 — [start,end,count] (count>=2 만) */
function overlapBands(turns: Turn[]): Array<{ a: number; b: number; n: number }> {
  const pts = Array.from(new Set(turns.flatMap(t => [t.start, t.end]))).sort((x, y) => x - y)
  const out: Array<{ a: number; b: number; n: number }> = []
  for (let i = 0; i < pts.length - 1; i++) {
    const a = pts[i], b = pts[i + 1], mid = (a + b) / 2
    const n = turns.filter(t => t.start <= mid && mid < t.end).length
    if (n < 2) continue
    const last = out[out.length - 1]
    if (last && last.b === a && last.n === n) last.b = b
    else out.push({ a, b, n })
  }
  return out
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
  turns: number
  speakers: number       // 해당 일 시간버킷 화자수 합(근사)
  ms: number
  active: boolean
  hasData: boolean
}

// ── 좌측 목록 항목 ───────────────────────────────────────────────
// 출처는 DB 그룹 ∪ 녹취 디렉터리 요약이다. DB 만 보면 1:1 private call·ad-hoc 세션이
// 통째로 누락되고(행이 없다), 요약만 보면 아직 통화가 없는 그룹이 사라진다.
interface RailItem {
  key: string            // 녹취 저장 키 (= ptt/{key})
  kind: PttSessionKind
  title: string
  sub: string
  video: boolean
  memberCount: number
  floorControl: string   // 'on' | 'off'(전이중) | ''(구 세션)
  floorPolicy: string
  maxTalkers: number
  groupType: string
  summary?: PttGroupSummary
}

const KIND_SECTIONS: Array<{ kind: PttSessionKind; label: string }> = [
  { kind: 'group',   label: '그룹' },
  { kind: 'private', label: '1:1 private call' },
  { kind: 'adhoc',   label: '임시 / ad-hoc' },
  { kind: 'unknown', label: '분류 미상' },
]
const TABS: Array<{ id: 'all' | PttSessionKind; label: string }> = [
  { id: 'all', label: '전체' },
  { id: 'group', label: '그룹' },
  { id: 'private', label: '1:1' },
  { id: 'adhoc', label: '임시' },
]

const detailKey = (key: string, dir: string) => `${key}|${dir}`

export default function PttHistoryPage() {
  const { show } = useToast()
  const audio = useInlineAudio(useCallback((m: string) => show(m, 'err'), [show]))

  const [groups, setGroups] = useState<Group[]>([])
  const [summaries, setSummaries] = useState<Record<string, PttGroupSummary>>({})
  const [loading, setLoading] = useState(false)
  const [rangeDays, setRangeDays] = useState(10)
  const [autoRefresh, setAR] = useState(false)
  const [tab, setTab] = useState<'all' | PttSessionKind>('all')

  const [selectedKey, setSelKey] = useState<string | null>(null)
  const [selectedDay, setSelDay] = useState<string | null>(null)         // YYYYMMDD (일별 히트맵 드릴다운)
  const [selectedSessionDir, setSelSession] = useState<string | null>(null)  // YYYYMMDDHH (시간 행 펼침)

  const [sessionsByKey, setSessionsByKey] = useState<Map<string, SessionsState>>(new Map())
  const [detailByKey, setDetailByKey] = useState<Map<string, DetailState>>(new Map())
  const [sort, setSort] = useState<{ key: keyof PttSession; dir: 'asc' | 'desc' }>({ key: 'dir', dir: 'desc' })

  const [flow, setFlow] = useState<{ storeKey: string; sessionDir: string; date: string; nodes?: Record<string, FlowMessage[]>; messages?: FlowMessage[] } | null>(null)
  const [flowLoading, setFlowLoading] = useState(false)
  const [recPlayer, setRecPlayer] = useState<{ id: string; segments: RecordingSegment[]; title?: string } | null>(null)

  // ── 그룹 + 녹취 요약 로드 ──
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

  const loadSummaries = useCallback(async () => {
    try {
      const resp = await pttApi.summary()
      setSummaries(resp.summaries || {})
    } catch { /* 요약 실패는 무시 (좌측 보조정보) */ }
  }, [])

  useEffect(() => { loadGroups(); loadSummaries() }, [loadGroups, loadSummaries])

  // ── 좌측 목록 구성 ──
  const railItems = useMemo<RailItem[]>(() => {
    const items = new Map<string, RailItem>()
    // ① DB 등록 그룹 — 아직 통화가 없어도 노출한다(기존 동선 유지)
    for (const g of groups) {
      const key = g.db_id != null ? String(g.db_id) : g.id
      items.set(key, {
        key, kind: 'group', title: g.name || g.id, sub: g.id,
        video: !!g.video_enabled, memberCount: g.members?.length ?? 0,
        floorControl: 'on', floorPolicy: '', maxTalkers: 0, groupType: g.group_type || '',
      })
    }
    // ② 녹취 디렉터리 — DB 행이 없는 1:1 private call·ad-hoc 세션이 여기서 드러난다
    for (const [key, sm] of Object.entries(summaries)) {
      const cur = items.get(key)
      if (cur) {
        cur.summary = sm
        cur.floorControl = sm.floor_control || cur.floorControl
        cur.floorPolicy = sm.floor_policy || cur.floorPolicy
        cur.maxTalkers = sm.max_talkers || cur.maxTalkers
        cur.groupType = sm.group_type || cur.groupType
        cur.video = cur.video || !!sm.video_enabled
        continue
      }
      const kind = (sm.kind || 'unknown') as PttSessionKind
      const peers = sm.peers || []
      items.set(key, {
        key, kind,
        title: kind === 'private' && peers.length === 2
          ? `${peers[0]} ↔ ${peers[1]}`
          : (sm.name || sm.mcptt_group_id || key),
        sub: sm.mcptt_group_id || key,
        video: !!sm.video_enabled, memberCount: sm.member_count ?? 0,
        floorControl: sm.floor_control || '', floorPolicy: sm.floor_policy || '',
        maxTalkers: sm.max_talkers || 0, groupType: sm.group_type || '',
        summary: sm,
      })
    }
    return Array.from(items.values()).sort((a, b) => {
      const la = a.summary?.last_window || '', lb = b.summary?.last_window || ''
      if (la !== lb) return lb.localeCompare(la)        // 최근 활동 우선
      return a.title.localeCompare(b.title)
    })
  }, [groups, summaries])

  const visibleItems = useMemo(
    () => (tab === 'all' ? railItems : railItems.filter(i => i.kind === tab)),
    [railItems, tab],
  )

  // 초기/탭 변경 시 선택 보정
  useEffect(() => {
    if (visibleItems.length === 0) { setSelKey(null); return }
    if (!selectedKey || !visibleItems.some(i => i.key === selectedKey)) {
      setSelKey(visibleItems[0].key)
      setSelDay(null); setSelSession(null)
    }
  }, [visibleItems, selectedKey])

  const selItem = railItems.find(i => i.key === selectedKey) || null
  // floor 중재가 없는 세션(private call without floor) = 전이중 통화
  const isDuplex = selItem?.floorControl === 'off'

  // ── 시간버킷 목록 lazy 로드 ──
  const loadSessions = useCallback(async (key: string, force = false) => {
    if (!force) {
      const cur = sessionsByKey.get(key)
      if (cur && cur.loaded) return cur.sessions
    }
    setSessionsByKey(prev => {
      const m = new Map(prev)
      m.set(key, { sessions: prev.get(key)?.sessions ?? [], loading: true, loaded: false })
      return m
    })
    try {
      const resp = await pttApi.sessions(key, { days: rangeDays })
      const sessions = resp.sessions || []
      setSessionsByKey(prev => {
        const m = new Map(prev)
        m.set(key, { sessions, loading: false, loaded: true })
        return m
      })
      return sessions
    } catch {
      setSessionsByKey(prev => {
        const m = new Map(prev)
        m.set(key, { sessions: [], loading: false, loaded: true })
        return m
      })
      return []
    }
  }, [sessionsByKey, rangeDays])

  // ── 세션 상세(events + participants + floor + 녹취 segments) lazy 로드 ──
  const loadDetail = useCallback(async (key: string, dir: string, force = false) => {
    const dk = detailKey(key, dir)
    if (!force) {
      const cur = detailByKey.get(dk)
      if (cur && cur.loaded) return
    }
    setDetailByKey(prev => {
      const m = new Map(prev)
      m.set(dk, { events: [], participants: [], floor: [], segments: [], loading: true, loaded: false })
      return m
    })
    const recId = recIdOf(key, dir)
    const dt = dateOf(dir) || undefined
    try {
      const [ev, fl, rec] = await Promise.all([
        pttApi.events(key, dir, dt),
        pttApi.floor(key, dir, dt).catch(() => ({ floor: [] })),
        recId ? recordingsApi.get(recId).catch(() => ({ segments: [] })) : Promise.resolve({ segments: [] }),
      ])
      setDetailByKey(prev => {
        const m = new Map(prev)
        m.set(dk, {
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
        m.set(dk, { events: [], participants: [], floor: [], segments: [], loading: false, loaded: true })
        return m
      })
    }
  }, [detailByKey])

  // ── 선택 세션의 시간버킷 전체(범위 내) ──
  const allSessions = useMemo(
    () => (selectedKey ? (sessionsByKey.get(selectedKey)?.sessions ?? []) : []),
    [selectedKey, sessionsByKey],
  )

  // 세션의 발언 턴 수 — 구 응답(turn_count 없음)은 세그먼트 수로 대체
  const turnsOf = (s: PttSession) => s.turn_count ?? s.segment_count ?? 0

  // ── 최근 rangeDays 일 일별 집계 (빈 일자 포함, 오래된→최신) ──
  const dayAggs = useMemo<DayAgg[]>(() => {
    const byDay = new Map<string, DayAgg>()
    for (const s of allSessions) {
      const day = s.dir.slice(0, 8)
      const cur = byDay.get(day) || { day, turns: 0, speakers: 0, ms: 0, active: false, hasData: false }
      cur.turns += turnsOf(s)
      cur.speakers += s.speaker_count ?? 0
      cur.ms += s.total_speech_ms ?? 0
      cur.active = cur.active || s.state === 'active'
      cur.hasData = true
      byDay.set(day, cur)
    }
    const out: DayAgg[] = []
    const today = new Date()
    for (let i = rangeDays - 1; i >= 0; i--) {
      const d = new Date(today)
      d.setDate(today.getDate() - i)
      const key = `${d.getFullYear()}${String(d.getMonth() + 1).padStart(2, '0')}${String(d.getDate()).padStart(2, '0')}`
      out.push(byDay.get(key) || { day: key, turns: 0, speakers: 0, ms: 0, active: false, hasData: false })
    }
    return out
  }, [allSessions, rangeDays])

  // 선택 세션/범위 변경 → 버킷 로드 + 활동 있는 최신 일자 자동선택
  useEffect(() => {
    if (!selectedKey) return
    let cancelled = false
    ;(async () => {
      const sessions = await loadSessions(selectedKey)
      if (cancelled) return
      const days = Array.from(new Set(sessions.map(s => s.dir.slice(0, 8)))).sort()
      setSelDay(days.length > 0 ? days[days.length - 1] : null)
    })()
    return () => { cancelled = true }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selectedKey, rangeDays])

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
    if (selectedKey && selectedSessionDir) loadDetail(selectedKey, selectedSessionDir)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selectedKey, selectedSessionDir])

  // 범위 변경 → 캐시 비움
  useEffect(() => {
    setSessionsByKey(new Map())
    setDetailByKey(new Map())
  }, [rangeDays])

  // 자동 갱신 (선택 세션만)
  useEffect(() => {
    if (!autoRefresh || !selectedKey) return
    const iv = setInterval(() => { loadSessions(selectedKey, true) }, 15000)
    return () => clearInterval(iv)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [autoRefresh, selectedKey])

  // 녹취 재생 (시간 전체 또는 10분 슬롯 부분집합)
  const openRecPlayer = (storeKey: string, sessionDir: string, segs: RecordingSegment[], title?: string) => {
    const recId = recIdOf(storeKey, sessionDir)
    if (!recId) { show('잘못된 시간창', 'err'); return }
    const playable = segs.filter(s => s.status !== 'recording')
    if (playable.length === 0) { show('녹취 세그먼트 없음', 'err'); return }
    setRecPlayer({ id: recId, segments: playable, title })
  }

  const playRecording = async (storeKey: string, sessionDir: string) => {
    const recId = recIdOf(storeKey, sessionDir)
    if (!recId) { show('잘못된 시간창', 'err'); return }
    try {
      const rec = await recordingsApi.get(recId)
      if (rec.segments && rec.segments.length > 0) {
        setRecPlayer({ id: recId, segments: rec.segments })
      } else {
        show('녹취 세그먼트 없음', 'err')
      }
    } catch (e: unknown) {
      show(String(e), 'err')
    }
  }

  // Flow 열기 — slot(10분창) 지정 시 해당 시간대 메시지만 필터링
  const openFlow = async (storeKey: string, sessionDir: string, slot?: { hh: number; min: number }) => {
    setFlowLoading(true)
    const dt = dateOf(sessionDir)
    try {
      const resp = await pttApi.flow(storeKey, sessionDir, dt || undefined)
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
      setFlow({ storeKey, sessionDir, date: dt, nodes, messages })
    } catch (e: unknown) {
      show(String(e), 'err')
      setFlow({ storeKey, sessionDir, date: dt })
    } finally {
      setFlowLoading(false)
    }
  }

  const toggleSort = (key: keyof PttSession) =>
    setSort(prev => prev.key === key ? { key, dir: prev.dir === 'asc' ? 'desc' : 'asc' } : { key, dir: 'desc' })
  const sortArrow = (key: keyof PttSession) => sort.key === key ? (sort.dir === 'asc' ? ' ▲' : ' ▼') : ''
  const selSessionsLoading = selectedKey ? (sessionsByKey.get(selectedKey)?.loading ?? false) : false

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
        <button className="btn btn--primary btn--sm" onClick={() => { loadSummaries(); if (selectedKey) loadSessions(selectedKey, true) }}>
          새로고침
        </button>
        <label style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 4, fontSize: 12, color: 'var(--text-muted)', cursor: 'pointer' }}>
          <input type="checkbox" checked={autoRefresh} onChange={e => setAR(e.target.checked)} />
          자동갱신
        </label>
      </div>

      <div style={{ flex: 1, display: 'flex', gap: 12, overflow: 'hidden', minHeight: 0 }}>
        {/* ── 좌: 세션 리스트 (그룹 / 1:1 / 임시) ── */}
        <div style={{ flex: '0 0 300px', overflow: 'auto', border: '1px solid var(--border)', borderRadius: 8, background: 'var(--surface, #fff)' }}>
          <div style={{ display: 'flex', gap: 2, padding: '8px 8px 0', position: 'sticky', top: 0, background: 'var(--surface, #fff)', zIndex: 2 }}>
            {TABS.map(t => (
              <button
                key={t.id}
                className={`btn btn--sm ${tab === t.id ? 'btn--primary' : 'btn--ghost'}`}
                style={{ flex: 1, fontSize: 11.5 }}
                onClick={() => setTab(t.id)}
              >
                {t.label}
              </button>
            ))}
          </div>
          {loading ? (
            <div className="empty" style={{ padding: 16 }}>목록 로딩 중...</div>
          ) : visibleItems.length === 0 ? (
            <div className="empty" style={{ padding: 16 }}>표시할 세션이 없습니다</div>
          ) : (
            KIND_SECTIONS.map(sec => {
              const items = visibleItems.filter(i => i.kind === sec.kind)
              if (items.length === 0) return null
              return (
                <Fragment key={sec.kind}>
                  <div style={{
                    padding: '10px 14px 4px', fontSize: 10.5, fontWeight: 700, letterSpacing: '.08em',
                    textTransform: 'uppercase', color: 'var(--text-muted)', display: 'flex', gap: 6,
                  }}>
                    {sec.label}
                    <span style={{ fontWeight: 600, letterSpacing: 0, textTransform: 'none', opacity: .75 }}>{items.length}</span>
                  </div>
                  {items.map(it => (
                    <RailRow
                      key={it.key}
                      item={it}
                      selected={it.key === selectedKey}
                      onPick={() => { setSelKey(it.key); setSelDay(null); setSelSession(null) }}
                    />
                  ))}
                </Fragment>
              )
            })
          )}
        </div>

        {/* ── 우: 세션 상세 ── */}
        <div style={{ flex: 1, overflow: 'auto', border: '1px solid var(--border)', borderRadius: 8, background: 'var(--surface, #fff)', minWidth: 0 }}>
          {!selItem ? (
            <div className="empty" style={{ padding: 24 }}>왼쪽에서 세션을 선택하세요</div>
          ) : (
            <div style={{ padding: 16 }}>
              {/* 세션 헤더 */}
              <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap', marginBottom: 12 }}>
                <span style={{ fontWeight: 700, fontSize: 16 }}>{selItem.title}</span>
                <span className="ts" style={{ color: 'var(--text-muted)' }}>({selItem.sub})</span>
                <SessionBadges item={selItem} />
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
                            <th onClick={() => toggleSort('turn_count')} style={{ ...thStyle, textAlign: 'right' }}>발언 턴{sortArrow('turn_count')}</th>
                            <th onClick={() => toggleSort('speaker_count')} style={{ ...thStyle, textAlign: 'right' }}>화자{sortArrow('speaker_count')}</th>
                            <th onClick={() => toggleSort('max_concurrent')} style={{ ...thStyle, textAlign: 'right' }}>동시{sortArrow('max_concurrent')}</th>
                            <th onClick={() => toggleSort('total_speech_ms')} style={{ ...thStyle, textAlign: 'right' }}>발화시간{sortArrow('total_speech_ms')}</th>
                            <th style={{ ...thStyle, cursor: 'default', textAlign: 'right' }}></th>
                          </tr>
                        </thead>
                        <tbody>
                          {dayHourSessions.map(sess => {
                            const isOpen = sess.dir === selectedSessionDir
                            const detail = selectedKey ? detailByKey.get(detailKey(selectedKey, sess.dir)) : undefined
                            return (
                              <BucketRow
                                key={sess.dir}
                                sess={sess}
                                isOpen={isOpen}
                                detail={detail}
                                storeKey={selItem.key}
                                isDuplex={isDuplex}
                                audio={audio}
                                flowLoading={flowLoading}
                                onToggle={() => toggleExpand(sess.dir)}
                                onFlow={() => openFlow(selItem.key, sess.dir)}
                                onPlayAll={() => playRecording(selItem.key, sess.dir)}
                                onSlotFlow={(hh, min) => openFlow(selItem.key, sess.dir, { hh, min })}
                                onSlotPlay={(segs, title) => openRecPlayer(selItem.key, sess.dir, segs, title)}
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
          callId={flow.storeKey}
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
// 세션 성격 배지 — 반이중/전이중, floor 정책, 영상
// ════════════════════════════════════════════════════════════════
function SessionBadges({ item }: { item: RailItem }) {
  const policyLabel = item.floorPolicy === 'multi'
    ? `multi · 최대 ${item.maxTalkers || '?'}명`
    : item.floorPolicy === 'dual' ? 'dual · 최대 2명'
    : item.floorPolicy === 'single' ? 'single' : ''
  return (
    <>
      <span className={`badge ${item.video ? 'badge--blue' : 'badge--gray'}`}>{item.video ? '영상' : '음성'}</span>
      {item.floorControl === 'off' ? (
        <span className="badge badge--green" title="floor 중재 없음 — 양측 상시 송신(통화형)">전이중 · 통화</span>
      ) : item.floorControl === 'on' ? (
        <span className="badge badge--yellow" title="floor 중재 있음 — 발언권 기반(무전형)">반이중 · 무전</span>
      ) : null}
      {policyLabel && <span className="badge badge--gray" title="동시 발언 정책 (TS 24.380)">{policyLabel}</span>}
      {item.kind === 'private' && <span className="badge badge--gray">private</span>}
      {item.kind === 'adhoc' && <span className="badge badge--gray">임시</span>}
    </>
  )
}

// ════════════════════════════════════════════════════════════════
// 좌측 목록 행
// ════════════════════════════════════════════════════════════════
function RailRow({ item, selected, onPick }: { item: RailItem; selected: boolean; onPick: () => void }) {
  const sm = item.summary
  return (
    <div
      onClick={onPick}
      style={{
        padding: '10px 14px',
        cursor: 'pointer',
        borderBottom: '1px solid var(--border)',
        background: selected ? 'var(--hover, #eef5ff)' : 'transparent',
        borderLeft: selected ? '3px solid var(--primary, #2563eb)' : '3px solid transparent',
      }}
    >
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <span style={{ fontWeight: 600, fontSize: 13, flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
          {item.title}
        </span>
        {item.floorControl === 'off' && (
          <span className="badge badge--green" style={{ fontSize: 10, padding: '1px 6px' }}>전이중</span>
        )}
        <span className={`badge ${item.video ? 'badge--blue' : 'badge--gray'}`} style={{ fontSize: 10, padding: '1px 6px' }}>
          {item.video ? '영상' : '음성'}
        </span>
      </div>
      <div className="ts" style={{ color: 'var(--text-muted)', marginTop: 2, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
        {item.sub}
      </div>
      <div style={{ display: 'flex', flexWrap: 'wrap', gap: 6, marginTop: 4, fontSize: 11, color: 'var(--text-muted)' }}>
        {item.kind !== 'private' && <span>멤버 {item.memberCount}명</span>}
        <span>{item.kind !== 'private' ? '· ' : ''}세션 {sm?.session_count ?? 0}</span>
        {sm?.last_window && <span>· 최근 {fmtWindow(sm.last_window)}</span>}
      </div>
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
  const [metric, setMetric] = useState<'turns' | 'speakers'>('turns')
  const valOf = (d: DayAgg) => (metric === 'turns' ? d.turns : d.speakers)
  const max = Math.max(1, ...days.map(valOf))
  return (
    <div style={{ marginBottom: 4 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 4 }}>
        <span style={{ fontWeight: 600, fontSize: 12, color: 'var(--text-muted)' }}>일별 활동</span>
        <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>색 진할수록 많음 · 클릭→해당 일 시간대 보기</span>
        <span style={{ marginLeft: 'auto' }}>
          <button className={`btn btn--sm ${metric === 'turns' ? 'btn--primary' : 'btn--ghost'}`} onClick={() => setMetric('turns')}>발언 턴</button>
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
              title={`${fmtDayShort(d.day)}(${dayWeekday(d.day)}) · 발언 턴 ${d.turns} · 화자 ${d.speakers} · ${fmtSpeechMs(d.ms)}${d.active ? ' · 진행중' : ''}`}
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
  const [metric, setMetric] = useState<'turns' | 'speakers'>('turns')
  const byHour = new Map<number, PttSession>()
  for (const s of sessions) byHour.set(Number(s.dir.slice(8, 10)), s)
  const valOf = (s?: PttSession) => {
    if (!s) return 0
    return metric === 'turns' ? (s.turn_count ?? s.segment_count ?? 0) : (s.speaker_count ?? 0)
  }
  const max = Math.max(1, ...sessions.map(s => valOf(s)))
  return (
    <div style={{ marginBottom: 4 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 4 }}>
        <span style={{ fontWeight: 600, fontSize: 12, color: 'var(--text-muted)' }}>시간대별 활동</span>
        <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>색 진할수록 많음 · 숫자=값 · 클릭→펼치기</span>
        <span style={{ marginLeft: 'auto' }}>
          <button className={`btn btn--sm ${metric === 'turns' ? 'btn--primary' : 'btn--ghost'}`} onClick={() => setMetric('turns')}>발언 턴</button>
          <button className={`btn btn--sm ${metric === 'speakers' ? 'btn--primary' : 'btn--ghost'}`} onClick={() => setMetric('speakers')}>화자수</button>
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
                ? `${String(h).padStart(2, '0')}시 · 발언 턴 ${sess.turn_count ?? sess.segment_count ?? 0} · 화자 ${sess.speaker_count ?? 0}${sess.max_concurrent && sess.max_concurrent > 1 ? ` · 최대 동시 ${sess.max_concurrent}명` : ''} · ${fmtSpeechMs(sess.total_speech_ms ?? 0)}${active ? ' · 진행중' : ''}`
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
function BucketRow({ sess, isOpen, detail, storeKey, isDuplex, audio, flowLoading, onToggle, onFlow, onPlayAll, onSlotFlow, onSlotPlay }: {
  sess: PttSession
  isOpen: boolean
  detail: DetailState | undefined
  storeKey: string
  isDuplex: boolean
  audio: InlineAudio
  flowLoading: boolean
  onToggle: () => void
  onFlow: () => void
  onPlayAll: () => void
  onSlotFlow: (hh: number, min: number) => void
  onSlotPlay: (segs: RecordingSegment[], title: string) => void
}) {
  const recId = recIdOf(storeKey, sess.dir)
  const hourNum = Number(sess.dir.slice(8, 10))
  const maxCon = sess.max_concurrent ?? 0
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
        <td style={{ ...tdStyle, textAlign: 'right' }}>
          {sess.turn_count ?? sess.segment_count ?? 0}
          {sess.turn_count != null && sess.segment_count != null && sess.turn_count !== sess.segment_count && (
            <span style={{ color: 'var(--text-muted)', fontSize: 11 }}> / {sess.segment_count}세그</span>
          )}
        </td>
        <td style={{ ...tdStyle, textAlign: 'right' }}>{sess.speaker_count ?? 0}</td>
        <td style={{ ...tdStyle, textAlign: 'right' }}>
          {maxCon > 1
            ? <span className="badge badge--blue" style={{ fontSize: 10 }}>{maxCon}명</span>
            : <span style={{ color: 'var(--text-muted)' }}>—</span>}
        </td>
        <td style={{ ...tdStyle, textAlign: 'right' }} className="ts">{fmtSpeechMs(sess.total_speech_ms)}</td>
        <td style={{ ...tdStyle, textAlign: 'right' }} onClick={e => e.stopPropagation()}>
          <button className="btn btn--sm btn--outline" style={{ marginRight: 4 }} disabled={flowLoading} onClick={onFlow}>Flow</button>
          <button className="btn btn--sm btn--outline" onClick={onPlayAll}>&#9654; 전체</button>
        </td>
      </tr>
      {isOpen && (
        <tr>
          <td colSpan={9} style={{ padding: 0, background: 'var(--surface-alt, #fafbfd)', borderTop: '1px solid var(--border)' }}>
            <div style={{ padding: '12px 16px' }}>
              {!detail || detail.loading ? (
                <div className="empty" style={{ padding: 12 }}>상세 로딩 중...</div>
              ) : (
                <BucketDetail
                  detail={detail}
                  sess={sess}
                  recId={recId}
                  hourNum={hourNum}
                  isDuplex={isDuplex}
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
  | { t: number; kind: 'segmix'; seg: RecordingSegment; turns: Turn[] }
  | { t: number; kind: 'turn'; turn: Turn }
  | { t: number; ts: string; kind: 'floor'; floor: PttFloorEvent }
  | { t: number; ts: string; kind: 'event'; ev: PttEvent }

// 10분 슬롯 묶음
interface SlotGroup {
  min: number              // 0/10/20/30/40/50
  segs: RecordingSegment[]
  floor: PttFloorEvent[]
  events: PttEvent[]
  speakers: Set<string>
  turns: number
  ms: number
}

// 펼친 시간버킷 상세: 지표 + floor 레인 타임바 + 10분 슬롯 하위 테이블
function BucketDetail({ detail, sess, recId, hourNum, isDuplex, audio, flowLoading, onSlotFlow, onSlotPlay }: {
  detail: DetailState
  sess: PttSession
  recId: string | null
  hourNum: number
  isDuplex: boolean
  audio: InlineAudio
  flowLoading: boolean
  onSlotFlow: (hh: number, min: number) => void
  onSlotPlay: (segs: RecordingSegment[], title: string) => void
}) {
  // 세그먼트 → 발언 턴 (동시 발언·슬롯 재사용 반영)
  const allTurns = useMemo(
    () => detail.segments.flatMap(segTurns).filter(t => t.start > 0).sort((a, b) => a.start - b.start),
    [detail.segments],
  )

  // 발언자 등장 순서 (색 배정 기준 — 타임바/타임라인 공통, 시간 전체)
  const speakerOrder = useMemo(() => {
    const seen: string[] = []
    for (const t of allTurns) if (t.spk && !seen.includes(t.spk)) seen.push(t.spk)
    return seen
  }, [allTurns])

  // ── 10분 슬롯 그룹핑 (타임스탬프 분 기준) ──
  const slots = useMemo<SlotGroup[]>(() => {
    const map = new Map<number, SlotGroup>()
    const ensure = (min: number) => {
      let g = map.get(min)
      if (!g) { g = { min, segs: [], floor: [], events: [], speakers: new Set(), turns: 0, ms: 0 }; map.set(min, g) }
      return g
    }
    for (const s of detail.segments) {
      const slot = slotOfMin(parseHM(s.start_time).mm)
      if (slot < 0) continue
      const g = ensure(slot)
      g.segs.push(s)
      const ts = segTurns(s)
      g.turns += ts.length
      for (const t of ts) if (t.spk) g.speakers.add(t.spk)
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
  const [openSlot, setOpenSlot] = useState<number | null>(() => (slots.length === 1 ? slots[0].min : null))

  const talkMs = sess.talk_ms ?? allTurns.reduce((a, t) => a + t.durMs, 0)
  const maxCon = sess.max_concurrent ?? 0

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
      {/* 지표 — 발언 턴/세그먼트, 발화 구간/누적을 분리해 동시 발언을 왜곡 없이 읽는다 */}
      <div style={{
        display: 'flex', gap: 18, flexWrap: 'wrap', padding: '9px 12px',
        background: 'var(--surface, #fff)', border: '1px solid var(--border)', borderRadius: 6,
      }}>
        <Metric k="발언 턴" v={String(sess.turn_count ?? allTurns.length)} s="건" />
        <Metric k="녹취 세그먼트" v={String(sess.segment_count ?? detail.segments.length)} s="개" />
        {maxCon > 1 && <Metric k="최대 동시 발언" v={String(maxCon)} s="명" />}
        <Metric k="발화 구간" v={fmtSpeechMs(sess.total_speech_ms)} s="" hint="겹침을 1회로 센 실제 무전 점유 시간" />
        <Metric k="발화 누적" v={fmtSpeechMs(talkMs)} s="" hint="화자별 발언 시간의 합" />
        <Metric k="화자" v={String(sess.speaker_count ?? speakerOrder.length)} s="명" />
      </div>

      {isDuplex ? (
        // ── 전이중(floor 없음) — 발언 턴이 없으므로 통화형 플레이어 ──
        detail.segments.filter(s => s.status !== 'recording').map(seg => (
          <div key={seg.seq} style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
            <div style={{ display: 'flex', alignItems: 'baseline', gap: 8 }}>
              <span style={{ fontWeight: 600, fontSize: 13 }}>통화 녹취</span>
              <span className="ts" style={{ color: 'var(--text-muted)' }}>
                {fmtShortTime(seg.start_time)} ~ {fmtShortTime(seg.end_time)} · {fmtSpeechMs(seg.duration_ms)}
              </span>
            </div>
            {recId && (
              <DuplexCallPlayer
                recordingId={recId}
                segment={seg}
                colorOf={id => spkColor(speakerOrder, id)}
              />
            )}
          </div>
        ))
      ) : (
        <>
          {/* floor 레인 타임바 (시간 전체 개요) */}
          <LaneTimebar
            turns={allTurns}
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
        </>
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
                <th style={{ ...thStyle, cursor: 'default', textAlign: 'right' }}>발언 턴</th>
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
                      <td style={{ ...tdStyle, textAlign: 'right' }}>{slot.turns}</td>
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

function Metric({ k, v, s, hint }: { k: string; v: string; s: string; hint?: string }) {
  return (
    <div title={hint}>
      <div style={{ fontSize: 10.5, letterSpacing: '.06em', textTransform: 'uppercase', color: 'var(--text-muted)', fontWeight: 600 }}>{k}</div>
      <div className="ts" style={{ fontSize: 15, fontWeight: 700, marginTop: 1 }}>
        {v}{s && <small style={{ fontSize: 11, fontWeight: 500, color: 'var(--text-muted)', marginLeft: 3 }}>{s}</small>}
      </div>
    </div>
  )
}

// ── 10분 슬롯 통합 타임라인 (발언 턴 인라인재생 + 발언권 + 이벤트) ──
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
    for (const seg of slot.segs) {
      const turns = segTurns(seg)
      // 동시 발언 세그먼트만 믹스 행을 앞세운다 — 단일 화자는 종전처럼 턴 1행뿐이다.
      if (turns.length > 1) items.push({ t: tms(seg.start_time), kind: 'segmix', seg, turns })
      for (const turn of turns) items.push({ t: turn.start, kind: 'turn', turn })
    }
    for (const f of slot.floor) items.push({ t: tms(f.ts), ts: f.ts, kind: 'floor', floor: f })
    for (const ev of slot.events) items.push({ t: tms(ev.ts), ts: ev.ts, kind: 'event', ev })
    items.sort((a, b) => a.t - b.t)
    return items
  }, [slot])

  const turnCount = timeline.filter(i => i.kind === 'turn').length
  const counts = { seg: turnCount, floor: slot.floor.length, event: slot.events.length }
  const layerOf = (it: TLItem) => (it.kind === 'segmix' || it.kind === 'turn' ? 'seg' : it.kind)
  const shown = timeline.filter(it => layers[layerOf(it) as keyof typeof layers])
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
        <div style={{ maxHeight: 400, overflowY: 'auto', border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface, #fff)' }}>
          {shown.map((it, i) => {
            const border = i > 0 ? '1px solid var(--border)' : undefined

            // ── 동시 발언 세그먼트 머리 행 — 믹스 재생(실제로 들린 소리) ──
            if (it.kind === 'segmix') {
              const { seg, turns } = it
              const names = Array.from(new Set(turns.map(t => t.spk)))
              const ref = { recId: recId || '', seq: seg.seq }
              const isPlaying = samePlay(audio.playing, ref)
              const isPrep = samePlay(audio.preparing, ref)
              return (
                <div key={`m${seg.seq}`} style={{
                  display: 'flex', alignItems: 'center', gap: 10, padding: '6px 10px', fontSize: 12,
                  borderTop: border, borderLeft: '4px solid var(--primary, #2563eb)',
                  background: 'var(--surface-alt, #f7f9fc)',
                }}>
                  <span className="ts" style={{ minWidth: 70, color: 'var(--text-muted)' }}>{fmtShortTime(seg.start_time)}</span>
                  <button
                    className={`btn btn--sm ${isPlaying ? 'btn--primary' : 'btn--outline'}`}
                    disabled={!recId || seg.status === 'recording'}
                    style={{ minWidth: 30, padding: '2px 6px' }}
                    onClick={() => recId && audio.play(recId, seg.seq)}
                    title="믹스 재생 — 동시 발언 화자 전원 합성(실제로 들린 소리)"
                  >
                    {isPrep ? '…' : isPlaying ? '❚❚' : '▶'}
                  </button>
                  <span style={{ display: 'inline-flex', gap: 3 }}>
                    {names.map(n => (
                      <span key={n} style={{ width: 3, height: 13, borderRadius: 2, background: spkColor(speakerOrder, n) }} />
                    ))}
                  </span>
                  <span style={{ fontWeight: 600 }}>동시 {names.length}명</span>
                  <span style={{ color: 'var(--text-muted)' }}>{names.join(', ')}</span>
                  <span className="badge badge--blue" style={{ fontSize: 9 }}>믹스</span>
                  <span className="ts" style={{ marginLeft: 'auto', color: 'var(--text-muted)' }}>{fmtSpeechMs(seg.duration_ms)}</span>
                </div>
              )
            }

            // ── 발언 턴 (화자 1명 × 슬롯 1개) ──
            if (it.kind === 'turn') {
              const turn = it.turn
              const color = spkColor(speakerOrder, turn.spk)
              const slot = playSlot(turn)
              const ref = { recId: recId || '', seq: turn.seq, slot }
              const isPlaying = samePlay(audio.playing, ref)
              const isPrep = samePlay(audio.preparing, ref)
              const role = roleOf(turn.spk)
              return (
                <div key={`t${turn.seq}-${turn.slot}-${turn.start}`} style={{
                  display: 'flex', alignItems: 'center', gap: 10, padding: '6px 10px', fontSize: 12,
                  borderTop: border, borderLeft: `4px solid ${color}`,
                  background: isPlaying ? 'var(--hover, #eef5ff)' : undefined,
                }}>
                  <span className="ts" style={{ minWidth: 70, color: 'var(--text-muted)' }}>{fmtClockMs(turn.start)}</span>
                  <button
                    className={`btn btn--sm ${isPlaying ? 'btn--primary' : 'btn--outline'}`}
                    disabled={!recId || !turn.playable}
                    style={{ minWidth: 30, padding: '2px 6px' }}
                    onClick={() => recId && audio.play(recId, turn.seq, slot)}
                    title={turn.playable ? (turn.multi ? '이 화자만 재생' : '재생/정지') : '녹취중'}
                  >
                    {isPrep ? '…' : isPlaying ? '❚❚' : '▶'}
                  </button>
                  <span style={{ fontWeight: 600, color }}>{turn.spk}</span>
                  <span style={{ color: 'var(--text-muted)' }}>발언</span>
                  {turn.multi && <span className="ts" style={{ color: 'var(--text-muted)', fontSize: 11 }}>슬롯 {turn.slot}</span>}
                  {role && <span className="badge badge--gray" style={{ fontSize: 9 }}>{role}</span>}
                  {turn.hasVideo && <span className="badge badge--blue" style={{ fontSize: 9 }}>영상</span>}
                  {!turn.playable && <span className="badge badge--blue" style={{ fontSize: 9 }}>녹취중</span>}
                  <span className="ts" style={{ marginLeft: 'auto', color: 'var(--text-muted)' }}>{fmtMmss(turn.durMs)}</span>
                </div>
              )
            }

            if (it.kind === 'floor') return <FloorRow key={`f${i}`} f={it.floor} speakerOrder={speakerOrder} border={border} />

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

// ── floor 이벤트 한 줄 — op 별 사유/부가정보를 규격 용어로 펼친다 ──
function FloorRow({ f, speakerOrder, border }: { f: PttFloorEvent; speakerOrder: string[]; border?: string }) {
  const st = FLOOR_OPS[f.op] || { label: f.op, color: 'var(--text-muted)' }
  const uColor = f.user ? spkColor(speakerOrder, f.user) : 'var(--text-muted)'
  const extras: string[] = []

  if (f.op === 'GRANT') {
    if (f.slot != null) extras.push(`슬롯 ${f.slot}`)
    if (f.talkers != null) extras.push(`동시 ${f.talkers}명`)
    if (f.policy) extras.push(`정책 ${f.policy}`)
  } else if (f.op === 'RELEASE') {
    if (f.talkers != null) extras.push(`잔여 ${f.talkers}명`)
    if (f.reason === 'end_of_rtp') extras.push(`무RTP 회수(T1${f.idle_ms != null ? ` ${f.idle_ms}ms` : ''})`)
  } else if (f.op === 'DENY') {
    const r = f.reason ? (DENY_REASON[f.reason] || f.reason) : ''
    if (r) extras.push(`사유 ${r}`)
    if (f.owner) extras.push(`점유 ${f.owner}`)
    if (f.owner_tier) extras.push(`상대 tier ${f.owner_tier}`)
    if (!r && !f.owner) extras.push('다른 참가자 점유')
    if (f.cause != null) extras.push(`cause ${f.cause}`)
  } else if (f.op === 'QUEUE') {
    if (f.reason === 'preempt') extras.push(`선점 대기 · 회수대상 ${f.revoked || '-'}`)
    else if (f.pos != null) extras.push(`대기 ${f.pos}/${f.qsize ?? '?'}`)
    if (f.owner) extras.push(`점유 ${f.owner}`)
    if (f.grace_sec != null) extras.push(`유예 ${f.grace_sec}초`)
  } else if (f.op === 'QUEUE_CANCEL') {
    if (f.removed != null) extras.push(`취소 ${f.removed}건`)
  } else if (f.op === 'REVOKE') {
    if (f.reason === 'policy_change') extras.push('정원 축소')
    if (f.cause != null) extras.push(`cause ${f.cause}`)
    if (f.grace_sec != null) extras.push(`유예 ${f.grace_sec}초`)
  } else if (f.op === 'REVOKE_END') {
    extras.push(f.reason === 'revoke_grace' ? '유예 만료 강제 회수' : (f.reason || '회수 확정'))
  }

  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '4px 10px', fontSize: 12, borderTop: border, borderLeft: '4px solid transparent', color: 'var(--text-muted)' }}>
      <span className="ts" style={{ minWidth: 70 }}>{fmtShortTime(f.ts)}</span>
      <span style={{ minWidth: 30, textAlign: 'center', color: st.color }}>◆</span>
      <span style={{ color: st.color, fontWeight: 600, minWidth: 76 }}>{st.label}</span>
      <span style={{ color: uColor, fontWeight: f.user ? 600 : 400 }}>{f.user || '-'}</span>
      {f.prio != null && f.prio >= 0 && <span className="ts">prio {f.prio}</span>}
      {f.preempt && <span className="ts" style={{ color: '#d97706' }}>← 선점 {f.preempted_from || ''}</span>}
      {f.tier && f.tier !== 'normal' && <span className="badge badge--red" style={{ fontSize: 9 }}>{f.tier}</span>}
      {extras.length > 0 && <span className="ts" style={{ fontSize: 11 }}>{extras.join(' · ')}</span>}
      <span className="ts" style={{ marginLeft: 'auto', fontSize: 10, opacity: .7 }}>{f.op}</span>
    </div>
  )
}

// ════════════════════════════════════════════════════════════════
// 화자 레인 타임바 — 화자마다 한 줄. 동시 발언 구간은 음영으로 드러난다.
// (단일 화자 세션은 레인이 1개라 종전 미니 타임바와 같은 모습)
// ════════════════════════════════════════════════════════════════
function LaneTimebar({ turns, speakerOrder, recId, audio }: {
  turns: Turn[]
  speakerOrder: string[]
  recId: string | null
  audio: InlineAudio
}) {
  if (turns.length === 0) return null
  const spanStart = Math.min(...turns.map(t => t.start))
  const spanEnd = Math.max(...turns.map(t => t.end))
  const span = Math.max(1000, spanEnd - spanStart)
  const pct = (x: number) => ((x - spanStart) / span) * 100
  const bands = overlapBands(turns)
  const maxCon = bands.reduce((m, b) => Math.max(m, b.n), 1)
  const fmtClock = fmtClockMs

  return (
    <div>
      <div style={{ display: 'flex', alignItems: 'baseline', gap: 8, marginBottom: 6, flexWrap: 'wrap' }}>
        <span style={{ fontWeight: 600, fontSize: 13 }}>발언권 타임라인</span>
        <span className="ts" style={{ color: 'var(--text-muted)' }}>{fmtClock(spanStart)} ~ {fmtClock(spanEnd)} · {fmtSpeechMs(span)}</span>
        {maxCon > 1 && (
          <span style={{ fontSize: 11.5, color: 'var(--text-muted)' }}>· 최대 동시 발언 {maxCon}명</span>
        )}
      </div>
      <div style={{
        border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface, #fff)',
        padding: '8px 10px 4px', overflowX: 'auto',
      }}>
        <div style={{ minWidth: 480 }}>
          {/* 겹침 라벨 */}
          <div style={{ position: 'relative', height: bands.length > 0 ? 13 : 0, marginLeft: 126 }}>
            {bands.map((b, i) => (
              <span key={i} style={{
                position: 'absolute', left: `${(pct(b.a) + pct(b.b)) / 2}%`, transform: 'translateX(-50%)',
                fontSize: 9.5, fontWeight: 700, color: 'var(--primary, #2563eb)', whiteSpace: 'nowrap',
              }}>
                동시 {b.n}
              </span>
            ))}
          </div>
          {speakerOrder.map(spk => {
            const color = spkColor(speakerOrder, spk)
            const mine = turns.filter(t => t.spk === spk)
            return (
              <div key={spk} style={{ display: 'flex', alignItems: 'center', gap: 8, height: 26 }}>
                <div style={{ flex: '0 0 118px', fontSize: 11.5, display: 'flex', alignItems: 'center', gap: 5, overflow: 'hidden', whiteSpace: 'nowrap' }}>
                  <span style={{ width: 8, height: 8, borderRadius: 2, background: color, flex: '0 0 auto' }} />
                  <span style={{ overflow: 'hidden', textOverflow: 'ellipsis' }}>{spk}</span>
                </div>
                <div style={{ position: 'relative', flex: 1, height: '100%', borderBottom: '1px dashed var(--border)' }}>
                  {bands.map((b, i) => (
                    <div key={i} style={{
                      position: 'absolute', top: 0, bottom: 0,
                      left: `${pct(b.a)}%`, width: `${Math.max(0.3, pct(b.b) - pct(b.a))}%`,
                      background: 'rgba(37,99,235,.10)', pointerEvents: 'none',
                    }} />
                  ))}
                  {mine.map((t, i) => {
                    const slot = playSlot(t)
                    const ref = { recId: recId || '', seq: t.seq, slot }
                    const isPlaying = samePlay(audio.playing, ref)
                    return (
                      <div
                        key={i}
                        onClick={() => recId && t.playable && audio.play(recId, t.seq, slot)}
                        title={`${spk} · ${fmtClock(t.start)} · ${fmtMmss(t.durMs)}${t.multi ? ` · 슬롯 ${t.slot}` : ''}`}
                        style={{
                          position: 'absolute',
                          left: `${Math.max(0, Math.min(99.4, pct(t.start)))}%`,
                          width: `${Math.max(0.6, pct(t.end) - pct(t.start))}%`,
                          top: 5, bottom: 5, minWidth: 3,
                          background: color, opacity: isPlaying ? 1 : 0.78, borderRadius: 2,
                          cursor: recId && t.playable ? 'pointer' : 'default',
                          boxShadow: isPlaying ? '0 0 0 2px var(--primary, #2563eb)' : undefined,
                        }}
                      />
                    )
                  })}
                </div>
              </div>
            )
          })}
          <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: 10, color: 'var(--text-muted)', marginTop: 3, marginLeft: 126 }}>
            <span className="ts">{fmtClock(spanStart)}</span>
            <span className="ts">{fmtClock(spanStart + span / 2)}</span>
            <span className="ts">{fmtClock(spanEnd)}</span>
          </div>
        </div>
      </div>
    </div>
  )
}
