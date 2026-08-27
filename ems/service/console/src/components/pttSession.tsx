/* pttSession.tsx — PTT 세션 상세 표현 (이력 페이지 · 그룹 활동 탭 공용)
 *
 * 세션 하나를 펼쳤을 때 보이는 것 전부 — 지표·발언권 레인 타임바·10분 슬롯 타임라인·
 * floor 이벤트·녹취 재생 — 이 여기 있다. 세션은 어느 축으로 도달하든 같은 것이므로
 * (평면 목록에서 오든, 그룹 활동에서 오든) 표현도 한 벌이어야 한다.
 */

import {
  useState, useMemo, useRef, useEffect, useLayoutEffect, useCallback,
  type CSSProperties,
} from 'react'
import type { PttSession, PttEvent, PttFloorEvent } from '@core/api/ptt'
import type { RecordingSegment } from '@core/api/recordings'
import DuplexCallPlayer from '@core/components/DuplexCallPlayer'
import { samePlay, type InlineAudio } from '@core/components/useInlineAudio'
import { useDirectory, type Directory } from '@core/components/useDirectory'


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
  GRANT:        { label: '발언권 부여', color: 'var(--success)' },
  RELEASE:      { label: '발언 종료',  color: 'var(--text-muted)' },
  IDLE:         { label: '유휴',      color: 'var(--text-muted)' },
  REVOKE:       { label: '회수 통지',  color: 'var(--warning)' },
  REVOKE_END:   { label: '회수 확정',  color: 'var(--danger)' },
  QUEUE:        { label: '대기열 등록', color: '#0891b2' },
  QUEUE_CANCEL: { label: '대기 취소',  color: 'var(--text-muted)' },
  DENY:         { label: '거절',      color: 'var(--danger)' },
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

/** 사람 한 명 — 표시는 이름, hover 로 번호. 이력 화면의 번호 표기는 전부 이걸 쓴다
 *  (사전에 없는 번호는 번호가 그대로 이름 자리에 온다 — [[useDirectory]]). */
export function Person({ id, names, style, className }: {
  id: string | null | undefined
  names: Directory
  style?: CSSProperties
  className?: string
}) {
  if (!id) return <span className={className} style={style}>—</span>
  return <span className={className} style={style} title={names.tipOf(id)}>{names.nameOf(id)}</span>
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
export function SessionRow({ sess, isOpen, detail, storeKey, isDuplex, audio, flowLoading, onToggle, onFlow, onPlayAll }: {
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
}) {
  const recId = recIdOf(storeKey, sess.dir)
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
                  isDuplex={isDuplex}
                  audio={audio}
                />
              )}
            </div>
          </td>
        </tr>
      )}
    </>
  )
}

// ── 이벤트 타임라인 아이템 (발언권 + 멤버·세션) ──
type TLItem =
  | { t: number; kind: 'floor'; floor: PttFloorEvent }
  | { t: number; kind: 'event'; ev: PttEvent }

/** floor GRANT ↔ 녹취 발언 턴 짝짓기 — GRANT 는 곧 한 발언의 시작이라 그 행에서 바로 듣는다.
 *  같은 화자(멀티토커면 같은 슬롯)의 턴 중 GRANT 시각에 가장 가까운 시작을 고른다.
 *  RTP 첫 패킷은 GRANT 뒤에 오므로 창은 앞으로 짧고 뒤로 넉넉하게 잡는다. */
export function turnOfGrant(turns: Turn[], f: PttFloorEvent): Turn | undefined {
  const t0 = tms(f.ts)
  if (!t0) return undefined
  let best: Turn | undefined
  let bestD = Infinity
  for (const t of turns) {
    if (t.spk !== f.user) continue
    if (f.slot != null && t.multi && t.slot !== f.slot) continue
    const d = t.start - t0
    if (d < -800 || d > 5000) continue
    if (Math.abs(d) < bestD) { bestD = Math.abs(d); best = t }
  }
  return best
}

// 펼친 세션 상세: 지표 + 발언권 타임바(재생) + 이벤트 타임라인
//
// 발언 목록을 따로 두지 않는다 — 발언은 타임바가 곧 목록이자 재생 진입점이고(막대 클릭 =
// 그 화자 단독, 겹침 라벨 클릭 = 믹스), 발언권 이벤트(GRANT/RELEASE)가 같은 사실의 원본
// 기록이라 목록을 한 벌 더 두면 같은 것을 두 번 그리는 셈이다. 아래는 "왜 그렇게 됐나"
// (발언권 중재 + 입퇴장) 한 줄기만 남긴다.
export function SessionDetail({ detail, sess, recId, isDuplex, audio, layout = 'inline' }: {
  detail: DetailState
  sess: PttSession
  recId: string | null
  isDuplex: boolean
  audio: InlineAudio
  /** inline = 목록 안(자체 높이 상한·한 흐름), panel = 드로어(참여자/발언/이벤트 3구획, [[PanelDetail]]) */
  layout?: 'inline' | 'panel'
}) {
  const names = useDirectory()

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

  if (layout === 'panel') {
    return (
      <PanelDetail
        detail={detail} recId={recId} isDuplex={isDuplex} audio={audio} names={names}
        turns={allTurns} speakerOrder={speakerOrder} live={sess.state === 'active'}
      />
    )
  }

  const talkMs = sess.talk_ms ?? allTurns.reduce((a, t) => a + t.durMs, 0)
  const maxCon = sess.max_concurrent ?? 0

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
      {/* 지표 — 발언 턴/세그먼트, 발화 구간/누적을 분리해 동시 발언을 왜곡 없이 읽는다.
          드로어(panel)에서는 좌측 요약 카드가 이 자리를 대신하므로 싣지 않는다 —
          같은 수치를 두 번 그리면 정작 봐야 할 발언·이벤트가 아래로 밀린다. */}
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
                labelOf={id => names.nameOf(id)}
              />
            )}
          </div>
        ))
      ) : (
        /* floor 레인 타임바 (시간 전체 개요) */
        <LaneTimebar
          turns={allTurns}
          speakerOrder={speakerOrder}
          recId={recId}
          audio={audio}
          names={names}
        />
      )}

      {/* ── 이벤트 타임라인 (발언권 중재 + 입퇴장) ── */}
      <EventTimeline
        floor={detail.floor}
        events={detail.events}
        participants={detail.participants}
        turns={allTurns}
        speakerOrder={speakerOrder}
        names={names}
        recId={recId}
        audio={audio}
        maxHeight={360}
      />
    </div>
  )
}

// ════════════════════════════════════════════════════════════════
// 드로어(panel) 상세 — 참여자 / 발언 / 이벤트 세 구획을 상하로 쌓는다.
// 높이 조절 손잡이는 두지 않는다 — 배분 수단은 접기 하나다(제목 클릭 = 토글,
// `cims.ptt.panel.fold` 에 저장). 모든 구획은 **내용 크기**로 선다 — 내용이 적으면
// 구획도 작고 남는 공간은 아래에 빈 채로 둔다(어느 구획도 빈 상자를 채우려 늘어나지
// 않는다). 내용 합이 패널보다 크면 구획이 줄며 자체 스크롤한다. 참여자·발언에는
// 상한(35%/55%)을 더 둬 이벤트가 헤더만 남게 밀리는 일을 막는다.
// ════════════════════════════════════════════════════════════════
const PANEL_SEC_MIN = 64        // 열린 구획 최소 높이 — 헤더 + 한두 줄이 보이는 정도
// 내용이 길 때의 상한 — 참여자가 이벤트를 밀어내지 않게. 넘치면 구획 안에서 스크롤.
const PANEL_SEC_MAX: Record<'part' | 'talk', string> = { part: '35%', talk: '55%' }

const PANEL_FOLD_KEY = 'cims.ptt.panel.fold'
type PanelFold = { part: boolean; talk: boolean; events: boolean }
const loadPanelFold = (): PanelFold => {
  try {
    const v = JSON.parse(localStorage.getItem(PANEL_FOLD_KEY) || '{}')
    return { part: !!v.part, talk: !!v.talk, events: !!v.events }
  } catch {
    return { part: false, talk: false, events: false }
  }
}

function PanelDetail({ detail, recId, isDuplex, audio, names, turns, speakerOrder, live }: {
  detail: DetailState
  recId: string | null
  isDuplex: boolean
  audio: InlineAudio
  names: Directory
  turns: Turn[]
  speakerOrder: string[]
  live: boolean
}) {
  const [fold, setFold] = useState<PanelFold>(loadPanelFold)

  const toggleFold = (k: keyof PanelFold) => setFold(prev => {
    const n = { ...prev, [k]: !prev[k] }
    localStorage.setItem(PANEL_FOLD_KEY, JSON.stringify(n))
    return n
  })

  // 참여자 = participants(입퇴장 기록) ∪ 화자(녹취 턴) — 기록이 어긋나도 발언한 사람은
  // 빠지지 않는다. 발언 통계는 녹취 턴에서 세므로 참가만 한 사람은 0 으로 남는다.
  const parts = useMemo(() => {
    const ids = detail.participants.map(p => p.msisdn)
    for (const s of speakerOrder) if (!ids.includes(s)) ids.push(s)
    return ids.map(id => {
      const p = detail.participants.find(x => x.msisdn === id)
      const mine = turns.filter(t => t.spk === id)
      return {
        id, role: p?.role, join: p?.join_time ?? null, leave: p?.leave_time ?? null,
        n: mine.length, ms: mine.reduce((a, t) => a + t.durMs, 0),
      }
    })
  }, [detail.participants, turns, speakerOrder])

  // 내용 크기(0 1 auto) — grow 없음. 내용 합이 패널을 넘으면 shrink 가 기여분(basis)
  // 비례로 걸려 사실상 큰 구획(대개 이벤트)이 줄며 자체 스크롤한다.
  const secStyle = (which: 'part' | 'talk'): CSSProperties => (fold[which]
    ? { flex: '0 0 auto' }
    : { flex: '0 1 auto', minHeight: PANEL_SEC_MIN, maxHeight: PANEL_SEC_MAX[which] })
  const foldHeaderProps = (which: keyof PanelFold) => ({
    onClick: () => toggleFold(which),
    title: fold[which] ? '펼치기' : '접기',
    style: {
      display: 'flex', alignItems: 'baseline', gap: 8, marginBottom: fold[which] ? 0 : 6,
      cursor: 'pointer', userSelect: 'none',
    } as CSSProperties,
  })
  const chev = (on: boolean) => <span style={{ color: 'var(--text-muted)' }}>{on ? '▸ ' : '▾ '}</span>

  return (
    <div style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column' }}>
      {/* ① 참여자 — 입퇴장·역할 + 발언 통계 */}
      <div style={{ ...secStyle('part'), display: 'flex', flexDirection: 'column', padding: fold.part ? '8px 14px' : '8px 14px 0' }}>
        <div {...foldHeaderProps('part')}>
          <span style={{ fontWeight: 600, fontSize: 13 }}>{chev(fold.part)}참여자</span>
          <span className="ts" style={{ fontSize: 11, color: 'var(--text-muted)' }}>{parts.length}명</span>
        </div>
        {!fold.part && <div style={{ minHeight: 0, overflowY: 'auto', paddingBottom: 8 }}>
          {parts.length === 0 ? (
            <div className="ts" style={{ color: 'var(--text-muted)' }}>참여자 기록이 없습니다</div>
          ) : (
            <div style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)' }}>
              {parts.map((p, i) => (
                <div key={p.id} style={{
                  display: 'flex', alignItems: 'center', gap: 8, padding: '4px 10px', fontSize: 12,
                  borderTop: i > 0 ? '1px solid var(--border)' : undefined,
                }}>
                  {/* 색점 = 타임바 레인 색. 발언 없는 참가자는 무채색 */}
                  <span style={{
                    width: 8, height: 8, borderRadius: 2, flex: '0 0 auto',
                    background: speakerOrder.includes(p.id) ? spkColor(speakerOrder, p.id) : 'var(--surface-2)',
                    border: speakerOrder.includes(p.id) ? undefined : '1px solid var(--border)',
                  }} />
                  <Person id={p.id} names={names}
                          style={{ fontWeight: 600, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }} />
                  {p.role === 'initiator' && <span className="badge badge--gray" style={{ fontSize: 9 }}>개시자</span>}
                  {(p.join || p.leave) && (
                    <span className="ts" style={{ color: 'var(--text-muted)', fontSize: 11 }}>
                      {fmtShortTime(p.join)} ~ {p.leave ? fmtShortTime(p.leave) : (live ? '참여중' : '--')}
                    </span>
                  )}
                  <span className="ts" style={{ marginLeft: 'auto', fontSize: 11, color: 'var(--text-muted)', whiteSpace: 'nowrap' }}>
                    턴 <b style={{ color: 'var(--text)' }}>{p.n}</b> · 발화 <b style={{ color: 'var(--text)' }}>{fmtSpeechMs(p.ms)}</b>
                  </span>
                </div>
              ))}
            </div>
          )}
        </div>}
      </div>

      {/* ② 발언 — floor 레인 타임바 (전이중은 통화 플레이어) */}
      <div style={{ ...secStyle('talk'), display: 'flex', flexDirection: 'column', padding: '8px 14px' }}>
        {isDuplex ? (
          <>
            <div {...foldHeaderProps('talk')}>
              <span style={{ fontWeight: 600, fontSize: 13 }}>{chev(fold.talk)}통화 녹취</span>
            </div>
            {!fold.talk && <div style={{ minHeight: 0, overflowY: 'auto', display: 'flex', flexDirection: 'column', gap: 8 }}>
              {detail.segments.filter(s => s.status !== 'recording').map(seg => (
                <div key={seg.seq} style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
                  <span className="ts" style={{ color: 'var(--text-muted)', fontSize: 11 }}>
                    {fmtShortTime(seg.start_time)} ~ {fmtShortTime(seg.end_time)} · {fmtSpeechMs(seg.duration_ms)}
                  </span>
                  {recId && (
                    <DuplexCallPlayer
                      recordingId={recId} segment={seg}
                      colorOf={id => spkColor(speakerOrder, id)} labelOf={id => names.nameOf(id)}
                    />
                  )}
                </div>
              ))}
            </div>}
          </>
        ) : turns.length === 0 ? (
          <>
            <div {...foldHeaderProps('talk')}>
              <span style={{ fontWeight: 600, fontSize: 13 }}>{chev(fold.talk)}발언권 타임라인</span>
            </div>
            {!fold.talk && <div className="ts" style={{ color: 'var(--text-muted)' }}>발언 녹취가 없습니다</div>}
          </>
        ) : (
          <LaneTimebar turns={turns} speakerOrder={speakerOrder} recId={recId} audio={audio} names={names}
                       fill collapsed={fold.talk} onToggle={() => toggleFold('talk')} />
        )}
      </div>

      {/* ③ 이벤트 — 내용 크기, 상한은 남는 공간(넘치면 자체 스크롤). 접히면 헤더만 */}
      <div style={{
        ...(fold.events ? { flex: '0 0 auto' } : { flex: '0 1 auto', minHeight: PANEL_SEC_MIN }),
        display: 'flex', flexDirection: 'column', padding: '8px 14px 10px',
      }}>
        <EventTimeline
          floor={detail.floor} events={detail.events} participants={detail.participants}
          turns={turns} speakerOrder={speakerOrder} names={names} recId={recId} audio={audio}
          fill collapsed={fold.events} onToggle={() => toggleFold('events')}
        />
      </div>
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

// ── 이벤트 타임라인 (발언권 중재 + 입퇴장 · 세션 전체) ──
//
// 발언 목록을 겸하지 않는다. 발언은 위 타임바에서 듣고, 여기서는 그 발언이 왜 그렇게
// 됐는지(부여/거절/대기/회수)를 규격 용어로 읽는다. 다만 GRANT 는 곧 한 발언의 시작이라
// 짝지어지는 녹취 턴이 있으면 그 행에서 바로 재생한다 — 아주 짧은 발언은 타임바 막대가
// 몇 px 이라 목록 쪽 클릭 지점이 필요하다.
export function EventTimeline({ floor, events, participants, turns, speakerOrder, names, recId, audio, maxHeight, fill, collapsed, onToggle }: {
  floor: PttFloorEvent[]
  events: PttEvent[]
  participants: Array<{ msisdn: string; role: string; join_time: string | null; leave_time: string | null }>
  turns: Turn[]
  speakerOrder: string[]
  names: Directory
  recId: string | null
  audio: InlineAudio
  /** 목록 자체 스크롤 상한 (inline 흐름용) */
  maxHeight?: number
  /** 부모 구획을 꽉 채우고 목록만 스크롤 (드로어 구획용) — 헤더는 고정으로 남는다 */
  fill?: boolean
  /** 접힘 — 헤더(제목·건수 칩)만 남긴다. onToggle 이 있어야 제목이 접기 손잡이가 된다 */
  collapsed?: boolean
  onToggle?: () => void
}) {
  const [layers, setLayers] = useState({ floor: true, event: true })
  const roleOf = (msisdn: string) => participants.find(p => p.msisdn === msisdn)?.role

  const timeline = useMemo<TLItem[]>(() => {
    const items: TLItem[] = []
    for (const f of floor) items.push({ t: tms(f.ts), kind: 'floor', floor: f })
    for (const ev of events) items.push({ t: tms(ev.ts), kind: 'event', ev })
    items.sort((a, b) => a.t - b.t)
    return items
  }, [floor, events])

  const counts = { floor: floor.length, event: events.length }
  const shown = timeline.filter(it => layers[it.kind])
  const chips: Array<{ key: 'floor' | 'event'; label: string; color: string }> = [
    { key: 'floor', label: '발언권', color: 'var(--success)' },
    { key: 'event', label: '멤버', color: '#9333ea' },
  ]

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 8, ...(fill ? { minHeight: 0 } : {}) }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <span onClick={onToggle}
              style={{ fontWeight: 600, fontSize: 13, ...(onToggle ? { cursor: 'pointer', userSelect: 'none' as const } : {}) }}
              title={onToggle ? (collapsed ? '펼치기' : '접기') : undefined}>
          {onToggle && <span style={{ color: 'var(--text-muted)' }}>{collapsed ? '▸ ' : '▾ '}</span>}이벤트 타임라인
        </span>
        <span className="ts" style={{ fontSize: 11 }}>발언권 중재 · 입퇴장</span>
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

      {!collapsed && (shown.length === 0 ? (
        <div className="ts" style={{ color: 'var(--text-muted)' }}>표시할 항목이 없습니다</div>
      ) : (
        <div style={{
          // fill 구획에서는 내용만큼 서고, 구획이 줄면 목록만 스크롤 (헤더는 밀리지 않는다)
          ...(fill ? { flex: '0 1 auto' as const, minHeight: 0, overflowY: 'auto' as const } : { maxHeight, overflowY: maxHeight ? 'auto' as const : undefined }),
          border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)',
        }}>
          {shown.map((it, i) => {
            const border = i > 0 ? '1px solid var(--border)' : undefined

            if (it.kind === 'floor') {
              return (
                <FloorRow
                  key={`f${i}`} f={it.floor} speakerOrder={speakerOrder} names={names} border={border}
                  role={it.floor.user ? roleOf(it.floor.user) : undefined}
                  turn={it.floor.op === 'GRANT' ? turnOfGrant(turns, it.floor) : undefined}
                  recId={recId} audio={audio}
                />
              )
            }

            const ev = it.ev
            const disp = getEventDisplay(ev.type)
            return (
              <div key={`e${i}`} style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '4px 10px', fontSize: 12, borderTop: border, borderLeft: '4px solid transparent' }}>
                <span className="ts" style={{ minWidth: 70, color: 'var(--text-muted)' }}>{fmtShortTime(ev.ts)}</span>
                <span style={{ minWidth: 30, textAlign: 'center', color: disp.color, fontSize: 14 }}>{disp.icon}</span>
                <span style={{ color: 'var(--text, #1a1d2e)' }}>
                  {ev.member && <><Person id={ev.member} names={names} style={{ fontWeight: 500 }} />{' '}</>}
                  {disp.label}
                  {ev.type === 'member_join' && ev.role === 'initiator' &&
                    <span className="badge badge--gray" style={{ fontSize: 9, marginLeft: 6 }}>개시자</span>}
                  {ev.duration != null && <span className="ts"> ({fmtDur(ev.duration)})</span>}
                </span>
              </div>
            )
          })}
        </div>
      ))}
    </div>
  )
}

// ── floor 이벤트 한 줄 — op 별 사유/부가정보를 규격 용어로 펼친다 ──
export function FloorRow({ f, speakerOrder, names, border, role, turn, recId, audio }: {
  f: PttFloorEvent
  speakerOrder: string[]
  names: Directory
  border?: string
  role?: string
  /** 이 GRANT 에 짝지어진 녹취 발언 턴 (있으면 행에서 바로 재생) */
  turn?: Turn
  recId?: string | null
  audio?: InlineAudio
}) {
  const st = FLOOR_OPS[f.op] || { label: f.op, color: 'var(--text-muted)' }
  const uColor = f.user ? spkColor(speakerOrder, f.user) : 'var(--text-muted)'
  // 사유·부가정보에 실린 번호도 사람으로 읽는다 (점유자·회수 대상·선점 상대).
  const who = (msisdn: string | undefined) => (msisdn ? names.nameOf(msisdn) : '')
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
    if (f.owner) extras.push(`점유 ${who(f.owner)}`)
    if (f.owner_tier) extras.push(`상대 tier ${f.owner_tier}`)
    if (!r && !f.owner) extras.push('다른 참가자 점유')
    if (f.cause != null) extras.push(`cause ${f.cause}`)
  } else if (f.op === 'QUEUE') {
    if (f.reason === 'preempt') extras.push(`선점 대기 · 회수대상 ${who(f.revoked) || '-'}`)
    else if (f.pos != null) extras.push(`대기 ${f.pos}/${f.qsize ?? '?'}`)
    if (f.owner) extras.push(`점유 ${who(f.owner)}`)
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

  // 행 전체 hover — 이름으로 표시된 번호들을 한 번에 확인한다.
  const rowTip = [
    f.user && names.tipOf(f.user),
    f.owner && `점유 ${names.tipOf(f.owner)}`,
    f.revoked && `회수대상 ${names.tipOf(f.revoked)}`,
    f.preempted_from && `선점 ${names.tipOf(f.preempted_from)}`,
  ].filter(Boolean).join('\n')

  // 짝지어진 녹취 턴이 있으면 이 행이 곧 재생 지점 (GRANT = 발언 시작)
  const slot = turn ? playSlot(turn) : undefined
  const ref = { recId: recId || '', seq: turn?.seq ?? -1, slot }
  const isPlaying = !!turn && !!audio && samePlay(audio.playing, ref)
  const isPrep = !!turn && !!audio && samePlay(audio.preparing, ref)

  return (
    <div title={rowTip || undefined}
         style={{
           display: 'flex', alignItems: 'center', gap: 10, padding: '4px 10px', fontSize: 12,
           borderTop: border, color: 'var(--text-muted)',
           // 발언(=재생 가능)인 행만 화자 색 띠로 세운다 — 목록에서 눈으로 바로 걸린다.
           borderLeft: `4px solid ${turn ? uColor : 'transparent'}`,
           background: isPlaying ? 'var(--hover)' : undefined,
         }}>
      <span className="ts" style={{ minWidth: 70 }}>{fmtShortTime(f.ts)}</span>
      {turn ? (
        <button
          className={`btn btn--sm ${isPlaying ? 'btn--primary' : 'btn--outline'}`}
          disabled={!recId || !turn.playable}
          style={{ minWidth: 30, padding: '1px 6px' }}
          onClick={() => recId && audio?.play(recId, turn.seq, slot)}
          title={turn.playable ? (turn.multi ? '이 화자만 재생 (동시 발언은 타임바의 “동시 N” 이 믹스)' : '재생/정지') : '녹취중'}
        >
          {isPrep ? '…' : isPlaying ? '❚❚' : '▶'}
        </button>
      ) : (
        <span style={{ minWidth: 30, textAlign: 'center', color: st.color }}>◆</span>
      )}
      <span style={{ color: st.color, fontWeight: 600, minWidth: 76 }}>{st.label}</span>
      {f.user
        ? <Person id={f.user} names={names} style={{ color: uColor, fontWeight: 600 }} />
        : <span style={{ color: uColor }}>-</span>}
      {role && <span className="badge badge--gray" style={{ fontSize: 9 }}>{role}</span>}
      {f.prio != null && f.prio >= 0 && <span className="ts">prio {f.prio}</span>}
      {f.preempt && <span className="ts" style={{ color: 'var(--warning)' }}>← 선점 {who(f.preempted_from)}</span>}
      {f.tier && f.tier !== 'normal' && <span className="badge badge--red" style={{ fontSize: 9 }}>{f.tier}</span>}
      {extras.length > 0 && <span className="ts" style={{ fontSize: 11 }}>{extras.join(' · ')}</span>}
      {turn?.hasVideo && <span className="badge badge--blue" style={{ fontSize: 9 }}>영상</span>}
      {turn && !turn.playable && <span className="badge badge--blue" style={{ fontSize: 9 }}>녹취중</span>}
      {turn && <span className="ts" style={{ marginLeft: 'auto', fontSize: 11 }}>{fmtMmss(turn.durMs)}</span>}
      <span className="ts" style={{ marginLeft: turn ? undefined : 'auto', fontSize: 10, opacity: .7 }}>{f.op}</span>
    </div>
  )
}

// ════════════════════════════════════════════════════════════════
// 화자 레인 타임바 — 화자마다 한 줄. 동시 발언 구간은 음영으로 드러난다.
// (단일 화자 세션은 레인이 1개라 종전 미니 타임바와 같은 모습)
//
// 확대/이동 — 짧은 발언이 몰린 구간은 전체 보기(1×)에서 몇 px 로 뭉개진다. 배율로
// 시간폭을 좁히고(최대 64×) 좌/우로 옮겨 본다. 화자 이름 열은 고정이고 트랙만 움직인다:
// 확대 상태에서도 "누구의 레인인지" 를 잃지 않아야 한다.
//   · 버튼 ‹ › = 반 화면씩 이동, − + = 배율, 전체 = 1× 복귀
//   · 트랙을 잡아끌어도 이동(드래그), ctrl/⌘ + 휠 = 커서 지점 기준 확대·축소
// ════════════════════════════════════════════════════════════════
const LANE_LABEL_W = 126        // 화자 이름 열 폭 (고정)
const LANE_H = 26
const ZOOM_MIN = 1
const ZOOM_MAX = 64
// 눈금 간격 후보(초) — 보이는 구간에 6개 이하가 놓이는 첫 값을 쓴다.
const TICK_STEPS = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 14400, 43200, 86400]
const clampZoom = (z: number) => Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, z))

function LaneTimebar({ turns, speakerOrder, recId, audio, names, fill, collapsed, onToggle }: {
  turns: Turn[]
  speakerOrder: string[]
  recId: string | null
  audio: InlineAudio
  names: Directory
  /** 부모 구획을 꽉 채우고 레인 박스만 세로 스크롤 (드로어 구획용) — 헤더·컨트롤은 고정 */
  fill?: boolean
  /** 접힘 — 헤더(제목·구간 요약)만 남긴다. 컨트롤은 숨긴다(가려진 트랙에만 작용하므로) */
  collapsed?: boolean
  onToggle?: () => void
}) {
  const [zoom, setZoom] = useState(1)
  const scRef = useRef<HTMLDivElement>(null)
  const railRef = useRef<HTMLDivElement>(null)
  const anchorRef = useRef(0.5)     // 확대 기준점 (콘텐츠 폭 대비 비율)
  const dragRef = useRef({ on: false, x: 0, left: 0, moved: false })
  const railOnRef = useRef(false)   // 위치 막대를 잡고 있는 중
  // 보이는 구간 계산용 — 스크롤/폭이 바뀔 때만 갱신한다.
  const [view, setView] = useState({ left: 0, w: 0, total: 0 })

  const syncView = useCallback(() => {
    const el = scRef.current
    if (!el) return
    // 스크롤 1회당 상태 변경은 값이 실제로 달라졌을 때만 — 이동 중 불필요한 재렌더 방지.
    setView(v => (v.left === el.scrollLeft && v.w === el.clientWidth && v.total === el.scrollWidth)
      ? v : { left: el.scrollLeft, w: el.clientWidth, total: el.scrollWidth })
  }, [])

  // 컨테이너 폭 변화(창 크기·패널 접힘)도 보이는 구간에 반영
  useEffect(() => {
    const el = scRef.current
    if (!el || typeof ResizeObserver === 'undefined') return
    const ro = new ResizeObserver(syncView)
    ro.observe(el)
    return () => ro.disconnect()
  }, [syncView])

  // 배율이 바뀌면 기준점을 화면 중앙에 되돌린다 — 확대해도 보던 자리를 잃지 않는다.
  useLayoutEffect(() => {
    const el = scRef.current
    if (!el) return
    el.scrollLeft = Math.max(0, anchorRef.current * el.scrollWidth - el.clientWidth / 2)
    syncView()
  }, [zoom, syncView])

  // ctrl/⌘ + 휠 = 확대·축소 (맨휠은 페이지 스크롤에 양보). React 의 onWheel 은 passive 라
  // preventDefault 가 듣지 않아 네이티브 리스너로 건다.
  useEffect(() => {
    const el = scRef.current
    if (!el) return
    const onWheel = (e: WheelEvent) => {
      if (!e.ctrlKey && !e.metaKey) return
      e.preventDefault()
      const r = el.getBoundingClientRect()
      anchorRef.current = el.scrollWidth
        ? (el.scrollLeft + (e.clientX - r.left)) / el.scrollWidth
        : 0.5
      setZoom(z => clampZoom(z * (e.deltaY < 0 ? 1.25 : 1 / 1.25)))
    }
    el.addEventListener('wheel', onWheel, { passive: false })
    return () => el.removeEventListener('wheel', onWheel)
  }, [])

  // 위치 막대의 x → 그 지점이 화면 중앙에 오도록 이동 (refs 만 읽어 항등 유지)
  const railTo = useCallback((clientX: number) => {
    const el = scRef.current
    const rail = railRef.current
    if (!el || !rail) return
    const r = rail.getBoundingClientRect()
    const ratio = Math.min(1, Math.max(0, (clientX - r.left) / (r.width || 1)))
    el.scrollLeft = ratio * el.scrollWidth - el.clientWidth / 2
  }, [])

  // 드래그 이동 — 놓을 때까지 window 에서 받아야 트랙/막대 밖으로 나가도 끊기지 않는다.
  useEffect(() => {
    const onMove = (e: MouseEvent) => {
      if (railOnRef.current) { railTo(e.clientX); return }
      const d = dragRef.current
      const el = scRef.current
      if (!d.on || !el) return
      const dx = e.clientX - d.x
      if (Math.abs(dx) > 3) d.moved = true
      el.scrollLeft = d.left - dx
    }
    // 드래그 직후의 click 은 재생이 아니다 — click 이 지나간 뒤에 moved 를 푼다.
    const onUp = () => {
      railOnRef.current = false
      dragRef.current.on = false
      setTimeout(() => { dragRef.current.moved = false }, 0)
    }
    window.addEventListener('mousemove', onMove)
    window.addEventListener('mouseup', onUp)
    return () => {
      window.removeEventListener('mousemove', onMove)
      window.removeEventListener('mouseup', onUp)
    }
  }, [railTo])

  const bands = useMemo(() => overlapBands(turns), [turns])

  const spanStart = turns.length ? Math.min(...turns.map(t => t.start)) : 0
  const spanEnd = turns.length ? Math.max(...turns.map(t => t.end)) : 0
  const span = Math.max(1000, spanEnd - spanStart)
  const pct = (x: number) => ((x - spanStart) / span) * 100
  const maxCon = bands.reduce((m, b) => Math.max(m, b.n), 1)
  const fmtClock = fmtClockMs
  const zoomed = zoom > 1.001

  // 보이는 구간 = 스크롤 위치를 시간으로 환산한 것 (측정 전에는 전체 구간)
  const total = view.total || 1
  const vFrom = spanStart + span * (view.left / total)
  const vTo = spanStart + span * ((view.left + (view.w || total)) / total)
  const stepMs = 1000 * (TICK_STEPS.find(s => (vTo - vFrom) / 1000 / s <= 6) || TICK_STEPS[TICK_STEPS.length - 1])
  const ticks: number[] = []
  for (let t = Math.ceil(vFrom / stepMs) * stepMs; t <= vTo; t += stepMs) ticks.push(t)
  // 위치 막대(자체 스크롤바) — 전체 대비 보이는 구간
  const railFrom = view.total ? view.left / view.total : 0
  const railW = view.total ? Math.min(1, (view.w || view.total) / view.total) : 1

  // 겹침 구간 → 그 세그먼트 (동시 발언은 한 세그먼트의 여러 슬롯이라 seq 가 하나다).
  // 라벨을 누르면 슬롯 단독이 아니라 믹스 = 실제로 들린 소리를 재생한다.
  const bandSeq = (b: { a: number; b: number }) => {
    const mid = (b.a + b.b) / 2
    return turns.find(t => t.start <= mid && mid < t.end && t.playable)?.seq
  }

  const zoomBy = (f: number) => {
    const el = scRef.current
    anchorRef.current = el && el.scrollWidth ? (el.scrollLeft + el.clientWidth / 2) / el.scrollWidth : 0.5
    setZoom(z => clampZoom(z * f))
  }
  const resetZoom = () => { anchorRef.current = 0.5; setZoom(1) }
  const panBy = (frac: number) => {
    const el = scRef.current
    if (el) el.scrollTo({ left: el.scrollLeft + el.clientWidth * frac, behavior: 'smooth' })
  }
  const onDown = (e: React.MouseEvent) => {
    const el = scRef.current
    if (!el || el.scrollWidth <= el.clientWidth) return
    dragRef.current = { on: true, x: e.clientX, left: el.scrollLeft, moved: false }
  }
  const onRailDown = (e: React.MouseEvent) => {
    if (!zoomed) return
    railOnRef.current = true
    railTo(e.clientX)
  }

  if (turns.length === 0) return null
  const bandsH = bands.length > 0 ? 13 : 0

  return (
    <div style={fill ? { minHeight: 0, display: 'flex', flexDirection: 'column' } : undefined}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: collapsed ? 0 : 6, flexWrap: 'wrap' }}>
        <span onClick={onToggle}
              style={{ fontWeight: 600, fontSize: 13, ...(onToggle ? { cursor: 'pointer', userSelect: 'none' as const } : {}) }}
              title={onToggle ? (collapsed ? '펼치기' : '접기') : undefined}>
          {onToggle && <span style={{ color: 'var(--text-muted)' }}>{collapsed ? '▸ ' : '▾ '}</span>}발언권 타임라인
        </span>
        <span className="ts" style={{ color: 'var(--text-muted)' }}>{fmtClock(spanStart)} ~ {fmtClock(spanEnd)} · {fmtSpeechMs(span)}</span>
        {maxCon > 1 && (
          <span style={{ fontSize: 11.5, color: 'var(--text-muted)' }}>· 최대 동시 발언 {maxCon}명</span>
        )}
        {!collapsed && zoomed && (
          <span className="ts" style={{ fontSize: 11, color: 'var(--primary)' }}>
            · 보이는 구간 {fmtClock(vFrom)} ~ {fmtClock(vTo)}
          </span>
        )}
        {!collapsed && (
          <span style={{ marginLeft: 'auto', display: 'inline-flex', alignItems: 'center', gap: 3 }}>
            <button className="btn btn--sm btn--ghost" disabled={!zoomed} onClick={() => panBy(-0.5)} title="왼쪽으로 이동 (반 화면)">‹</button>
            <button className="btn btn--sm btn--ghost" disabled={!zoomed} onClick={() => panBy(0.5)} title="오른쪽으로 이동 (반 화면)">›</button>
            <button className="btn btn--sm btn--ghost" disabled={zoom <= ZOOM_MIN} onClick={() => zoomBy(0.5)} title="축소 — 시간폭 넓히기">−</button>
            <span className="ts" style={{ fontSize: 11, minWidth: 32, textAlign: 'center', color: 'var(--text-muted)' }}>
              {zoom < 10 ? Number(zoom.toFixed(1)) : Math.round(zoom)}×
            </span>
            <button className="btn btn--sm btn--ghost" disabled={zoom >= ZOOM_MAX} onClick={() => zoomBy(2)} title="확대 — 시간폭 좁히기">+</button>
            <button className="btn btn--sm btn--outline" disabled={!zoomed} onClick={resetZoom} title="전체 구간 보기">전체</button>
          </span>
        )}
      </div>
      {!collapsed && <div style={{
        border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)',
        padding: '8px 10px 4px',
        // fill 구획에서는 레인이 많을 때 박스 안에서만 세로 스크롤 — 헤더는 밀리지 않는다.
        // 내용이 적으면 박스도 내용만큼만 선다 (구획이 내용 크기 기반이라 grow 하지 않는다)
        ...(fill ? { flex: '0 1 auto', minHeight: 0, overflowY: 'auto' as const } : {}),
      }}>
        <div style={{ display: 'flex', alignItems: 'stretch' }}>
          {/* 화자 이름 열 — 확대·이동해도 고정 */}
          <div style={{ flex: `0 0 ${LANE_LABEL_W}px`, minWidth: 0 }}>
            <div style={{ height: bandsH }} />
            {speakerOrder.map(spk => (
              <div key={spk} style={{
                height: LANE_H, fontSize: 11.5, display: 'flex', alignItems: 'center', gap: 5,
                overflow: 'hidden', whiteSpace: 'nowrap', paddingRight: 8,
              }}>
                <span style={{ width: 8, height: 8, borderRadius: 2, background: spkColor(speakerOrder, spk), flex: '0 0 auto' }} />
                <Person id={spk} names={names} style={{ overflow: 'hidden', textOverflow: 'ellipsis' }} />
              </div>
            ))}
            <div style={{ height: 18 }} />
          </div>

          {/* 트랙 — 배율만큼 넓힌 콘텐츠를 가로 스크롤로 옮겨 본다.
              가로 스크롤바는 감춘다(scroll-nobar): 확대할 때 바가 생기며 박스 높이가
              늘어나면 아래 내용이 밀린다. 위치·이동은 아래 위치 막대가 맡는다. */}
          <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}>
            <div
              ref={scRef}
              className="scroll-nobar"
              onScroll={syncView}
              onMouseDown={onDown}
              style={{
                overflowX: 'auto', overflowY: 'hidden',
                cursor: zoomed ? 'grab' : 'default', userSelect: 'none',
              }}
            >
              <div style={{ width: `${zoom * 100}%`, position: 'relative' }}>
                {/* 겹침 라벨 */}
                <div style={{ position: 'relative', height: bandsH }}>
                  {bands.map((b, i) => {
                    const seq = bandSeq(b)
                    const on = seq != null && samePlay(audio.playing, { recId: recId || '', seq })
                    const can = seq != null && !!recId
                    return (
                      <span key={i}
                        onClick={() => { if (can && !dragRef.current.moved) audio.play(recId!, seq!) }}
                        title={can ? '믹스 재생 — 동시 발언 화자 전원 합성(실제로 들린 소리)' : undefined}
                        style={{
                          position: 'absolute', left: `${(pct(b.a) + pct(b.b)) / 2}%`, transform: 'translateX(-50%)',
                          fontSize: 9.5, fontWeight: 700, color: 'var(--primary)', whiteSpace: 'nowrap',
                          cursor: can ? 'pointer' : 'default', textDecoration: on ? 'underline' : undefined,
                        }}>
                        {can ? (on ? '❚❚ ' : '▶ ') : ''}동시 {b.n}
                      </span>
                    )
                  })}
                </div>
                {speakerOrder.map(spk => {
                  const color = spkColor(speakerOrder, spk)
                  const mine = turns.filter(t => t.spk === spk)
                  return (
                    <div key={spk} style={{ position: 'relative', height: LANE_H, borderBottom: '1px dashed var(--border)' }}>
                      {/* 눈금선 — 확대할수록 시간 위치를 눈으로 잡기 위한 격자 */}
                      {ticks.map(t => (
                        <div key={t} style={{
                          position: 'absolute', top: 0, bottom: 0, left: `${pct(t)}%`, width: 1,
                          background: 'var(--border)', opacity: .55, pointerEvents: 'none',
                        }} />
                      ))}
                      {bands.map((b, i) => (
                        <div key={i} style={{
                          position: 'absolute', top: 0, bottom: 0,
                          left: `${pct(b.a)}%`, width: `${Math.max(0.3 / zoom, pct(b.b) - pct(b.a))}%`,
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
                            onClick={() => {
                              if (dragRef.current.moved) return   // 이동 드래그였다
                              if (recId && t.playable) audio.play(recId, t.seq, slot)
                            }}
                            title={`${names.tipOf(t.spk)} · ${fmtClock(t.start)} · ${fmtMmss(t.durMs)}${t.multi ? ` · 슬롯 ${t.slot}` : ''}`}
                            style={{
                              position: 'absolute',
                              left: `${Math.max(0, Math.min(100 - 0.6 / zoom, pct(t.start)))}%`,
                              width: `${Math.max(0.6 / zoom, pct(t.end) - pct(t.start))}%`,
                              top: 5, bottom: 5, minWidth: 3,
                              background: color, opacity: isPlaying ? 1 : 0.78, borderRadius: 2,
                              cursor: recId && t.playable ? 'pointer' : 'default',
                              boxShadow: isPlaying ? '0 0 0 2px var(--primary)' : undefined,
                            }}
                          />
                        )
                      })}
                    </div>
                  )
                })}
                {/* 시각 눈금 — 보이는 구간에만 (배율이 오르면 간격도 촘촘해진다) */}
                <div style={{ position: 'relative', height: 18, marginTop: 3 }}>
                  {ticks.map(t => (
                    <span key={t} className="ts" style={{
                      position: 'absolute', left: `${pct(t)}%`, transform: 'translateX(-50%)',
                      fontSize: 10, color: 'var(--text-muted)', whiteSpace: 'nowrap',
                    }}>
                      {fmtClock(t)}
                    </span>
                  ))}
                </div>
              </div>
            </div>

            {/* 위치 막대 — 감춘 스크롤바 대신. 1× 에도 항상 자리를 차지해 확대해도
                박스 높이가 변하지 않는다. 클릭·드래그로 그 지점을 가운데로 옮긴다. */}
            <div
              ref={railRef}
              onMouseDown={onRailDown}
              title={zoomed ? '보이는 구간 — 클릭·드래그로 이동' : undefined}
              style={{
                height: 6, marginTop: 2, borderRadius: 3, position: 'relative',
                background: 'var(--surface-2)', cursor: zoomed ? 'pointer' : 'default',
              }}
            >
              <div style={{
                position: 'absolute', top: 0, bottom: 0,
                left: `${railFrom * 100}%`, width: `${railW * 100}%`, minWidth: 8,
                borderRadius: 3, background: zoomed ? 'var(--primary)' : 'var(--border)',
                opacity: zoomed ? .75 : .45,
              }} />
            </div>
          </div>
        </div>
      </div>}
    </div>
  )
}
