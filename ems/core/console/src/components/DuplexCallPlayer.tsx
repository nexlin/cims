// 전이중(full-duplex) 통화 녹취 플레이어 — floor 중재가 없는 1:1 private call 용.
//
// floor 가 없으면 발언 턴 경계가 없어 세그먼트가 통화 전체 1개이고, 멤버마다 슬롯 트랙이
// 하나씩 기록된다. 따라서 "발언 목록"이 아니라 통화형 UI 가 맞다:
//   · 기본 재생 = 믹스(양측 합성) — 실제 통화에서 들린 소리
//   · 드롭다운으로 화자 단독 전환 (화자 식별·증거용)
//   · 화자별 파형 레인 — floor 이벤트 없이 "누가 언제 말했나"를 보여주는 유일한 수단
import { Pause, Play } from 'lucide-react'
import { useState, useRef, useEffect, useCallback, useMemo } from 'react'
import { recordingsApi, type RecordingSegment, type SegmentTrack } from '../api/recordings'
import { waitSegmentReady } from './useInlineAudio'

interface Props {
  recordingId: string
  segment: RecordingSegment
  /** 화자 색 (발언자 팔레트 공유) */
  colorOf: (speakerId: string) => string
  /** 화자 표시명 (없으면 ID 그대로) */
  labelOf?: (speakerId: string) => string
}

const MIX = 'mix'

function fmtMs(ms: number): string {
  const sec = Math.max(0, Math.floor(ms / 1000))
  const m = Math.floor(sec / 60)
  return `${m}:${String(sec % 60).padStart(2, '0')}`
}

// 슬롯 트랙의 대표 화자 (전이중은 트랙당 화자 1명)
const trackSpeaker = (t: SegmentTrack) => t.speakers?.[0]?.id || ''

export default function DuplexCallPlayer({ recordingId, segment, colorOf, labelOf }: Props) {
  const audioRef = useRef<HTMLAudioElement>(null)
  const [sel, setSel] = useState<string>(MIX)          // 'mix' | slot 번호 문자열
  const [playing, setPlaying] = useState(false)
  const [pos, setPos] = useState(0)                    // 재생 위치 (ms)
  const [prep, setPrep] = useState(false)
  const [err, setErr] = useState('')
  const [peaks, setPeaks] = useState<Record<string, number[]>>({})
  const abortRef = useRef<AbortController | null>(null)

  const audioTracks = useMemo(
    () => (segment.tracks || []).filter(t => t.kind === 'audio').sort((a, b) => a.slot - b.slot),
    [segment.tracks],
  )
  const durMs = segment.duration_ms || 0
  const name = useCallback((id: string) => (labelOf ? labelOf(id) : id) || '—', [labelOf])

  const selSlot = sel === MIX ? undefined : Number(sel)

  // ── 파형 피크 로드 (슬롯별 1회) ──
  useEffect(() => {
    let cancelled = false
    const load = async (key: string, slot?: number) => {
      if (peaks[key]) return
      try {
        const r = await recordingsApi.segmentPeaks(recordingId, segment.seq, slot)
        if (!cancelled && Array.isArray(r?.peaks)) setPeaks(p => ({ ...p, [key]: r.peaks }))
      } catch {
        /* 파형은 보조 정보 — 없으면 레인만 비운다 (구 녹취는 피크 미생성) */
      }
    }
    audioTracks.forEach(t => load(String(t.slot), t.slot))
    return () => { cancelled = true }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [recordingId, segment.seq, audioTracks])

  // ── 선택 변경 → 소스 교체 (변환 대기 폴링 포함) ──
  useEffect(() => {
    const el = audioRef.current
    if (!el) return
    abortRef.current?.abort()
    const ac = new AbortController()
    abortRef.current = ac
    const url = recordingsApi.segmentAudioUrl(recordingId, segment.seq, selSlot)
    const wasPlaying = playing
    setErr(''); setPrep(true)
    ;(async () => {
      try {
        await waitSegmentReady(url, ac.signal)
        if (ac.signal.aborted) return
        setPrep(false)
        el.src = url
        if (wasPlaying) el.play().catch(() => {})
      } catch (e) {
        if (!ac.signal.aborted) {
          setPrep(false)
          setErr(e instanceof Error ? e.message : '재생 준비 실패')
        }
      }
    })()
    return () => ac.abort()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [recordingId, segment.seq, sel])

  useEffect(() => () => { abortRef.current?.abort() }, [])

  const toggle = () => {
    const el = audioRef.current
    if (!el || prep) return
    if (el.paused) { el.play().catch(() => {}); } else { el.pause() }
  }

  const seekTo = (ratio: number) => {
    const el = audioRef.current
    if (!el || !Number.isFinite(el.duration)) return
    el.currentTime = Math.max(0, Math.min(el.duration, el.duration * ratio))
  }

  const el = audioRef.current
  const totalMs = el && Number.isFinite(el.duration) ? el.duration * 1000 : durMs
  const ratio = totalMs > 0 ? Math.min(1, pos / totalMs) : 0

  return (
    <div style={{
      border: '1px solid var(--border)', borderRadius: 8, padding: '13px 14px',
      background: 'var(--muted)', display: 'flex', flexDirection: 'column', gap: 11,
    }}>
      {/* ── transport ── */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 11 }}>
        <button
          className="btn btn--primary"
          onClick={toggle}
          disabled={prep || !!err}
          title={playing ? '일시정지' : '재생'}
          style={{
            width: 34, height: 34, borderRadius: '50%', padding: 0, flex: '0 0 auto',
            display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 13,
          }}
        >
          {prep ? '…' : playing ? <Pause size={13} /> : <Play size={13} />}
        </button>
        <span className="ts" style={{ fontSize: 12, color: 'var(--muted-foreground)', minWidth: 38 }}>
          {fmtMs(pos)}
        </span>
        <div
          onClick={e => {
            const r = e.currentTarget.getBoundingClientRect()
            seekTo((e.clientX - r.left) / r.width)
          }}
          style={{ flex: 1, height: 5, borderRadius: 999, background: 'var(--border)', position: 'relative', cursor: 'pointer' }}
        >
          <div style={{ position: 'absolute', inset: '0 auto 0 0', width: `${ratio * 100}%`, background: 'var(--primary)', borderRadius: 999 }} />
          <div style={{
            position: 'absolute', left: `${ratio * 100}%`, top: '50%', width: 12, height: 12,
            margin: '-6px 0 0 -6px', borderRadius: '50%', background: 'var(--primary)',
            boxShadow: '0 0 0 3px var(--card)',
          }} />
        </div>
        <span className="ts" style={{ fontSize: 12, color: 'var(--muted-foreground)', minWidth: 38 }}>
          {fmtMs(totalMs)}
        </span>
        <select
          value={sel}
          onChange={e => setSel(e.target.value)}
          className="form-input"
          style={{ fontSize: 12, padding: '4px 9px', width: 'auto' }}
          title="믹스 = 통화에서 실제로 들린 소리 / 단독 = 해당 화자만"
        >
          <option value={MIX}>믹스 (양측)</option>
          {audioTracks.map(t => (
            <option key={t.slot} value={String(t.slot)}>{name(trackSpeaker(t))} 단독</option>
          ))}
        </select>
      </div>

      {err && <div style={{ fontSize: 11, color: 'var(--destructive)' }}>{err}</div>}

      {/* ── 화자별 파형 레인 ── */}
      {audioTracks.map(t => {
        const spk = trackSpeaker(t)
        const color = colorOf(spk)
        const arr = peaks[String(t.slot)] || []
        const dim = sel !== MIX && Number(sel) !== t.slot
        return (
          <div key={t.slot} style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
            {/* 표시는 이름(labelOf), 번호는 hover — 이력 화면 공통 규약 */}
            <div title={spk} style={{ flex: '0 0 128px', fontSize: 11.5, display: 'flex', alignItems: 'center', gap: 5, minWidth: 0 }}>
              <span style={{ width: 8, height: 8, borderRadius: 2, background: color, flex: '0 0 auto' }} />
              <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{name(spk)}</span>
              <span className="ts" style={{ color: 'var(--muted-foreground)', fontSize: 10 }}>슬롯 {t.slot}</span>
            </div>
            <div
              onClick={e => {
                const r = e.currentTarget.getBoundingClientRect()
                seekTo((e.clientX - r.left) / r.width)
              }}
              style={{ flex: 1, height: 30, display: 'flex', alignItems: 'flex-end', gap: 1, cursor: 'pointer', position: 'relative' }}
            >
              {arr.length === 0 ? (
                <div style={{ width: '100%', height: 1, background: 'var(--border)', alignSelf: 'center' }} />
              ) : arr.map((v, i) => (
                <span key={i} style={{
                  flex: 1, minWidth: 0, height: `${Math.max(3, (v / 255) * 100)}%`,
                  background: color, borderRadius: 1,
                  opacity: dim ? 0.2 : (i / arr.length <= ratio ? 0.95 : 0.45),
                }} />
              ))}
              {/* 재생 위치 */}
              <div style={{
                position: 'absolute', left: `${ratio * 100}%`, top: 0, bottom: 0, width: 1,
                background: 'var(--primary)', pointerEvents: 'none',
              }} />
            </div>
          </div>
        )
      })}

      <audio
        ref={audioRef}
        style={{ display: 'none' }}
        onPlay={() => setPlaying(true)}
        onPause={() => setPlaying(false)}
        onEnded={() => { setPlaying(false); setPos(0) }}
        onTimeUpdate={e => setPos(e.currentTarget.currentTime * 1000)}
      />
    </div>
  )
}
