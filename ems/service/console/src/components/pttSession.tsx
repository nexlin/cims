/* pttSession.tsx — PTT 세션 상세 표현 (이력 페이지 · 그룹 활동 탭 공용)
 *
 * 세션 하나를 펼쳤을 때 보이는 것 전부 — 지표·발언권 레인 타임바·10분 슬롯 타임라인·
 * floor 이벤트·녹취 재생 — 이 여기 있다. 세션은 어느 축으로 도달하든 같은 것이므로
 * (평면 목록에서 오든, 그룹 활동에서 오든) 표현도 한 벌이어야 한다.
 */

import { useState, useMemo, Fragment, type CSSProperties } from 'react'
import type { PttSession, PttEvent, PttFloorEvent } from '@core/api/ptt'
import type { RecordingSegment } from '@core/api/recordings'
import DuplexCallPlayer from '@core/components/DuplexCallPlayer'
import { samePlay, type InlineAudio } from '@core/components/useInlineAudio'


export function fmtShortTime(iso: string | null | undefined) {
  if (!iso) return '--'
  const s = iso.replace('T', ' ')
  const idx = s.indexOf(' ')
  return idx >= 0 ? s.substring(idx + 1, idx + 9) : s.substring(0, 8)
}

export function fmtDur(seconds: number | null | undefined) {
  if (!seconds || seconds <= 0) return '--'
  const m = Math.floor(seconds / 60)
  const s = seconds % 60
  return m > 0 ? `${m}분 ${s}초` : `${s}초`
}

export function fmtSpeechMs(ms: number | null | undefined) {
  if (!ms || ms <= 0) return '--'
  const total = Math.round(ms / 1000)
  const m = Math.floor(total / 60)
  const s = total % 60
  return m > 0 ? `${m}분 ${s}초` : `${s}초`
}

export function fmtMmss(ms: number | null | undefined) {
  const total = Math.max(0, Math.round((ms || 0) / 1000))
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, '0')}`
}

// epoch ms → 로컬 HH:MM:SS. 녹취 타임스탬프는 타임존 없는 로컬 ISO 라
// Date.parse 는 로컬로 읽는다 — 다시 문자열로 만들 때 UTC(toISOString)로 가면 시각이 밀린다.
export function fmtClockMs(ms: number) {
  return new Date(ms).toLocaleTimeString('ko-KR', {
    hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit',
  })
}

// 'YYYYMMDDHH' → 'MM/DD HH시'
export function fmtWindow(w: string | null | undefined) {
  if (!w || w.length < 10) return '--'
  return `${w.slice(4, 6)}/${w.slice(6, 8)} ${w.slice(8, 10)}시`
}

// 'YYYYMMDD' → 'MM/DD'
export function fmtDayShort(d: string) {
  return d.length >= 8 ? `${d.slice(4, 6)}/${d.slice(6, 8)}` : d
}

// 'YYYYMMDD' → 'YYYY-MM-DD' (flow/events 의 date 파라미터용)
export function dateOf(dirOrDay: string): string {
  const d = (dirOrDay || '').replace(/\D/g, '')
  return d.length >= 8 ? `${d.slice(0, 4)}-${d.slice(4, 6)}-${d.slice(6, 8)}` : ''
}

export const WEEKDAY = ['일', '월', '화', '수', '목', '금', '토']
export function dayWeekday(d: string): string {
  if (d.length < 8) return ''
  const dt = new Date(Number(d.slice(0, 4)), Number(d.slice(4, 6)) - 1, Number(d.slice(6, 8)))
  return WEEKDAY[dt.getDay()] || ''
}

// ISO 타임스탬프 → {hh, mm}. (segments.start_time / floor.ts / events.ts 공통 ISO)
export function parseHM(iso: string | null | undefined): { hh: number; mm: number } {
  if (!iso) return { hh: -1, mm: -1 }
  const t = iso.includes('T') ? iso.split('T')[1] : iso
  const hh = Number(t.slice(0, 2))
  const mm = Number(t.slice(3, 5))
  return { hh: Number.isFinite(hh) ? hh : -1, mm: Number.isFinite(mm) ? mm : -1 }
}
// 분 → 10분 슬롯(0/10/20/30/40/50)
export const slotOfMin = (mm: number) => (mm < 0 ? -1 : Math.floor(mm / 10) * 10)

export const EVENT_ICONS: Record<string, { icon: string; label: string; color: string }> = {
  session_start:  { icon: '●', label: '세션 시작',  color: '#4caf50' },
  session_end:    { icon: '■', label: '세션 종료',  color: '#f44336' },
  member_join:    { icon: '✚', label: '입장',      color: '#2196f3' },
  member_leave:   { icon: '✖', label: '퇴장',      color: '#ff9800' },
  'floor-grant':  { icon: '▶', label: '발언 시작',  color: '#4caf50' },
  'floor-release':{ icon: '■', label: '발언 종료',  color: 'var(--text-muted)' },
  config_change:  { icon: '⚙', label: '설정 변경',  color: '#9c27b0' },
  member_invite:  { icon: '→', label: '초대',      color: '#00bcd4' },
}

export function getEventDisplay(type: string) {
  return EVENT_ICONS[type] || { icon: '•', label: type, color: 'var(--text-muted)' }
}

// floor.jsonl op → 표시 스타일 (TS 24.380). CMP 가 기록하는 8종 전부를 다룬다 —
// GRANT/RELEASE/IDLE/REVOKE/REVOKE_END/QUEUE/QUEUE_CANCEL/DENY.
export const FLOOR_OPS: Record<string, { label: string; color: string }> = {
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
export const DENY_REASON: Record<string, string> = {
  recv_only: '수신전용(ambient)',
  only_one:  '참가자 1인',
  broadcast: 'broadcast 비개시자',
}

// 발언자 색 팔레트 (히트맵 막대/타임바/발언자 헤더 공통)
export const SPK_COLORS = ['#2563eb', '#16a34a', '#d97706', '#9333ea', '#dc2626', '#0891b2', '#ca8a04', '#db2777', '#4f46e5', '#059669', '#e11d48', '#0d9488']
export function spkColor(order: string[], id: string) {
  const i = order.indexOf(id)
  return SPK_COLORS[(i < 0 ? 0 : i) % SPK_COLORS.length]
}

export const thStyle: CSSProperties = { padding: '7px 10px', fontWeight: 600, color: 'var(--text-muted)', cursor: 'pointer', userSelect: 'none', whiteSpace: 'nowrap' }
export const tdStyle: CSSProperties = { padding: '6px 10px', whiteSpace: 'nowrap' }
export const RANGE_OPTIONS = [5, 10, 20, 30]
// 세션키 파싱 — 'S{yyyymmddHHMMSSuuuuuu}_{n}'(세션 디렉터리) 또는 'YYYYMMDDHH'(구 녹취).
// 어느 쪽이든 앞 10자리 숫자가 시작 시간버킷이라 날짜·시각 추출은 한 가지로 끝난다.
export const SES_KEY_RE = /^S\d{14,20}_\d+$/
export const digitsOf = (dir: string) => (dir || '').replace(/\D/g, '')
export const dayOf = (dir: string) => digitsOf(dir).slice(0, 8)
export const hourOf = (dir: string) => Number(digitsOf(dir).slice(8, 10))

// 세션키 → 녹취 recId. 신형은 ptt/{저장키}/{YYYY}/{MM}/{DD}/{HH}/{세션키} — 세션이
// 시간을 넘겨 이어지는 버킷은 서버가 찾아 붙인다(recording.py _ptt_part_dirs).
export function recIdOf(storeKey: string, dir: string): string | null {
  const w = digitsOf(dir)
  if (w.length < 10) return null
  const base = `ptt/${storeKey}/${w.slice(0, 4)}/${w.slice(4, 6)}/${w.slice(6, 8)}/${w.slice(8, 10)}`
  return SES_KEY_RE.test(dir) ? `${base}/${dir}` : base
}

// ── 발언 턴 ─────────────────────────────────────────────────────
// 한 화자가 한 슬롯 트랙을 점유한 구간. 동시 발언 세그먼트는 턴이 여럿이고,
// 선점 회수로 슬롯이 재사용되면 같은 트랙에서도 턴이 갈린다.
export interface Turn {
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
export const playSlot = (t: Turn) => (t.multi ? t.slot : undefined)

export function tms(iso: string | null | undefined): number {
  const n = Date.parse(iso || '')
  return Number.isFinite(n) ? n : 0
}

/** 세그먼트 → 발언 턴 목록. tracks 가 없는 구 녹취는 대표 화자 1턴으로 환원. */
export function segTurns(seg: RecordingSegment): Turn[] {
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
export function overlapBands(turns: Turn[]): Array<{ a: number; b: number; n: number }> {
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
export interface SessionsState { sessions: PttSession[]; loading: boolean; loaded: boolean }
export interface DetailState {
  events: PttEvent[]
  participants: Array<{ msisdn: string; role: string; join_time: string | null; leave_time: string | null }>
  floor: PttFloorEvent[]
  segments: RecordingSegment[]
  loading: boolean
  loaded: boolean
}

// 일별 집계 (일별 히트맵용)
export interface DayAgg {
  day: string            // YYYYMMDD
  turns: number
  speakers: number       // 해당 일 시간버킷 화자수 합(근사)
  ms: number
  active: boolean
  hasData: boolean
}

// ── 좌측 목록 항목 ───────────────────────────────────────────────
// 출처는 DB 그룹 ∪ 녹취 디렉터리 요약이다. DB 만 보면 1:1 private call·ad-hoc 세션이
export const detailKey = (key: string, dir: string) => `${key}|${dir}`
// ════════════════════════════════════════════════════════════════
// ① 일별 히트맵 — 최근 N일 발화량 색농도 (클릭 → 일자 드릴다운)
// ════════════════════════════════════════════════════════════════
export function DayHeatmap({ days, selectedDay, onPick }: {
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
                background: d.hasData ? `color-mix(in srgb, var(--primary) ${Math.round((ratio || 0.12) * 100)}%, var(--surface))` : 'var(--surface-2)',
                border: isSel ? '2px solid var(--primary)' : '1px solid var(--border)',
                cursor: 'pointer', opacity: d.hasData ? 1 : 0.65,
                display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center',
                fontSize: 10, color: ratio > 0.55 ? '#fff' : 'var(--text-muted)', position: 'relative', overflow: 'hidden',
              }}>
              <span style={{ fontSize: 11, fontWeight: 700 }}>{v > 0 ? v : ''}</span>
              <span style={{ fontSize: 9, opacity: 0.85 }}>{fmtDayShort(d.day)}</span>
              {d.active && <span style={{ position: 'absolute', top: 2, right: 2, width: 5, height: 5, borderRadius: '50%', background: 'var(--success)',
                // 셀 배경이 밝든 어둡든 읽히도록 표면색 링을 두른다
                boxShadow: '0 0 0 1px var(--surface)' }} />}
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
export function ActivityHeatmap({ sessions, selectedDir, onPick }: {
  sessions: PttSession[]
  selectedDir: string | null
  onPick: (dir: string) => void
}) {
  const [metric, setMetric] = useState<'turns' | 'speakers'>('turns')
  // 한 시간대에 세션이 여럿일 수 있다 (같은 시간에 두 번 통화). 셀 값은 그 시간대 합이고
  // 클릭은 가장 최근 세션을 펼친다 — 나머지는 아래 목록에서 각자의 행으로 보인다.
  const byHour = new Map<number, PttSession[]>()
  for (const s of sessions) {
    const h = hourOf(s.dir)
    const arr = byHour.get(h)
    if (arr) arr.push(s); else byHour.set(h, [s])
  }
  for (const arr of byHour.values()) arr.sort((a, b) => (b.start_time || '').localeCompare(a.start_time || ''))
  const one = (s: PttSession) => (metric === 'turns' ? (s.turn_count ?? s.segment_count ?? 0) : (s.speaker_count ?? 0))
  const valOf = (list?: PttSession[]) => (list || []).reduce((n, s) => n + one(s), 0)
  const max = Math.max(1, ...Array.from(byHour.values(), valOf))
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
          const list = byHour.get(h)
          const sess = list?.[0]
          const v = valOf(list)
          const ratio = v > 0 ? 0.2 + 0.8 * (v / max) : 0
          const active = (list || []).some(s => s.state === 'active')
          const isSel = !!list?.some(s => s.dir === selectedDir)
          const turns = (list || []).reduce((n, s) => n + (s.turn_count ?? s.segment_count ?? 0), 0)
          const spk = (list || []).reduce((n, s) => n + (s.speaker_count ?? 0), 0)
          const maxCon = Math.max(0, ...(list || []).map(s => s.max_concurrent ?? 0))
          const ms = (list || []).reduce((n, s) => n + (s.total_speech_ms ?? 0), 0)
          return (
            <div key={h} onClick={() => sess && onPick(sess.dir)}
              title={list && list.length
                ? `${String(h).padStart(2, '0')}시 · 세션 ${list.length} · 발언 턴 ${turns} · 화자 ${spk}${maxCon > 1 ? ` · 최대 동시 ${maxCon}명` : ''} · ${fmtSpeechMs(ms)}${active ? ' · 진행중' : ''}`
                : `${String(h).padStart(2, '0')}시 · 활동 없음`}
              style={{
                height: 34, borderRadius: 4,
                background: sess ? `color-mix(in srgb, var(--primary) ${Math.round((ratio || 0.12) * 100)}%, var(--surface))` : 'var(--surface-2)',
                border: isSel ? '2px solid var(--primary)' : '1px solid var(--border)',
                cursor: sess ? 'pointer' : 'default', opacity: sess ? 1 : 0.5,
                display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center',
                fontSize: 10, color: ratio > 0.55 ? '#fff' : 'var(--text-muted)', position: 'relative',
              }}>
              <span style={{ fontSize: 11, fontWeight: 600 }}>{v > 0 ? v : ''}</span>
              <span style={{ fontSize: 8, opacity: 0.8 }}>
                {String(h).padStart(2, '0')}{list && list.length > 1 ? ` ·${list.length}` : ''}
              </span>
              {active && <span style={{ position: 'absolute', top: 2, right: 2, width: 5, height: 5, borderRadius: '50%', background: 'var(--success)',
                // 셀 배경이 밝든 어둡든 읽히도록 표면색 링을 두른다
                boxShadow: '0 0 0 1px var(--surface)' }} />}
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
export function SessionRow({ sess, isOpen, detail, storeKey, isDuplex, audio, flowLoading, onToggle, onFlow, onPlayAll, onSlotFlow, onSlotPlay }: {
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
  const hourNum = hourOf(sess.dir)
  const maxCon = sess.max_concurrent ?? 0
  return (
    <>
      <tr
        onClick={onToggle}
        style={{
          cursor: 'pointer',
          borderTop: '1px solid var(--border)',
          background: isOpen ? 'var(--hover)' : 'transparent',
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
          <td colSpan={9} style={{ padding: 0, background: 'var(--bg-soft)', borderTop: '1px solid var(--border)' }}>
            <div style={{ padding: '12px 16px' }}>
              {!detail || detail.loading ? (
                <div className="empty" style={{ padding: 12 }}>상세 로딩 중...</div>
              ) : (
                <SessionDetail
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
export interface SlotGroup {
  min: number              // 0/10/20/30/40/50
  segs: RecordingSegment[]
  floor: PttFloorEvent[]
  events: PttEvent[]
  speakers: Set<string>
  turns: number
  ms: number
}

// 펼친 시간버킷 상세: 지표 + floor 레인 타임바 + 10분 슬롯 하위 테이블
export function SessionDetail({ detail, sess, recId, hourNum, isDuplex, audio, flowLoading, onSlotFlow, onSlotPlay }: {
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
        background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6,
      }}>
        <Metric k="발언 턴" v={String(sess.turn_count ?? allTurns.length)} s="건" />
        <Metric k="녹취 세그먼트" v={String(sess.segment_count ?? detail.segments.length)} s="개" />
        {maxCon > 1 && <Metric k="최대 동시 발언" v={String(maxCon)} s="명" />}
        <Metric k="발화 구간" v={fmtSpeechMs(sess.total_speech_ms)} s="" hint="겹침을 1회로 센 실제 무전 점유 시간" />
        <Metric k="발화 누적" v={fmtSpeechMs(talkMs)} s="" hint="화자별 발언 시간의 합" />
        <Metric k="화자" v={String(sess.speaker_count ?? speakerOrder.length)} s="명" />
        {/* 세션 당시 floor 축 (시간버킷 session.json) — 그룹 최신 스냅샷과 다를 수 있다 */}
        {sess.floor_control === 'off' ? (
          <span className="badge badge--green" style={{ alignSelf: 'center', marginLeft: 'auto' }}
                title="floor 중재 없음 — 양측 상시 송신(통화형)">전이중 · 통화</span>
        ) : sess.floor_control === 'on' ? (
          <span style={{ alignSelf: 'center', marginLeft: 'auto', display: 'inline-flex', gap: 6 }}>
            <span className="badge badge--yellow" title="floor 중재 있음 — 발언권 기반(무전형)">반이중 · 무전</span>
            {sess.floor_policy && (
              <span className="badge badge--gray" title="세션 당시 동시 발언 정책 (TS 24.380)">
                {sess.floor_policy === 'multi' ? `multi · 최대 ${sess.max_talkers || '?'}명`
                  : sess.floor_policy === 'dual' ? 'dual · 최대 2명' : 'single'}
              </span>
            )}
          </span>
        ) : null}
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
        <div style={{ border: '1px solid var(--border)', borderRadius: 6, overflow: 'hidden', background: 'var(--surface)' }}>
          <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
            <thead>
              <tr style={{ background: 'var(--bg-soft)', textAlign: 'left', color: 'var(--text-muted)' }}>
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
                      style={{ cursor: 'pointer', borderTop: '1px solid var(--border)', background: isOpen ? 'var(--hover)' : 'transparent' }}
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
                        <td colSpan={6} style={{ padding: 0, background: 'var(--bg-soft)', borderTop: '1px solid var(--border)' }}>
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

export function Metric({ k, v, s, hint }: { k: string; v: string; s: string; hint?: string }) {
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
export function SlotTimeline({ slot, speakerOrder, participants, recId, audio }: {
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
        <div style={{ maxHeight: 400, overflowY: 'auto', border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)' }}>
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
                  borderTop: border, borderLeft: '4px solid var(--primary)',
                  background: 'var(--bg-soft)',
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
                  background: isPlaying ? 'var(--hover)' : undefined,
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
export function FloorRow({ f, speakerOrder, border }: { f: PttFloorEvent; speakerOrder: string[]; border?: string }) {
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
        border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)',
        padding: '8px 10px 4px', overflowX: 'auto',
      }}>
        <div style={{ minWidth: 480 }}>
          {/* 겹침 라벨 */}
          <div style={{ position: 'relative', height: bands.length > 0 ? 13 : 0, marginLeft: 126 }}>
            {bands.map((b, i) => (
              <span key={i} style={{
                position: 'absolute', left: `${(pct(b.a) + pct(b.b)) / 2}%`, transform: 'translateX(-50%)',
                fontSize: 9.5, fontWeight: 700, color: 'var(--primary)', whiteSpace: 'nowrap',
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
                      background: 'color-mix(in srgb, var(--primary) 10%, transparent)', pointerEvents: 'none',
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
                          boxShadow: isPlaying ? '0 0 0 2px var(--primary)' : undefined,
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
