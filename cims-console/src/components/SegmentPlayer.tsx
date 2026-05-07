import { useState, useRef, useEffect, useCallback } from 'react'
import { recordingsApi, type RecordingSegment } from '../api/recordings'

interface SegmentPlayerProps {
  segments: RecordingSegment[]
  recordingId: string
  callType: 'volte' | 'ptt' | 'volte_video'
  caller?: string
  callee?: string
  onClose?: () => void
}

function fmtWallTime(iso: string | null, offsetMs: number): string {
  if (!iso) return '--:--:--'
  const base = new Date(iso).getTime()
  const d = new Date(base + offsetMs)
  return d.toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

function fmtTimeRange(start: string | null, end: string | null): string {
  const s = start ? new Date(start).toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', second: '2-digit' }) : '--:--:--'
  const e = end ? new Date(end).toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', second: '2-digit' }) : '--:--:--'
  return `${s} ~ ${e}`
}

function fmtMs(ms: number): string {
  const sec = Math.floor(ms / 1000)
  const m = Math.floor(sec / 60)
  const s = sec % 60
  return `${m}:${String(s).padStart(2, '0')}`
}

export default function SegmentPlayer({ segments, recordingId, callType, caller, callee, onClose }: SegmentPlayerProps) {
  // 재생 가능한 세그먼트만 (recording 상태 제외)
  const playable = segments.filter(s => s.status !== 'recording')

  // 체크박스: 기본 전체 선택
  const [checked, setChecked] = useState<Set<number>>(() => new Set(playable.map(s => s.seq)))
  const selectedSegs = playable.filter(s => checked.has(s.seq))

  const [currentIdx, setCurrentIdx] = useState(0)
  const [isPlaying, setIsPlaying] = useState(false)
  const [wallTime, setWallTime] = useState('')
  const [speakerInfo, setSpeakerInfo] = useState('')
  const audioRef = useRef<HTMLAudioElement>(null)
  const videoRef = useRef<HTMLVideoElement>(null)

  const current = selectedSegs[currentIdx]
  const isVideo = current?.has_video

  const getMediaUrl = useCallback((seg: RecordingSegment) => {
    if (seg.has_video) return recordingsApi.segmentVideoUrl(recordingId, seg.seq)
    return recordingsApi.segmentAudioUrl(recordingId, seg.seq)
  }, [recordingId])

  // 세그먼트 변경 시 미디어 로드
  useEffect(() => {
    if (!current) return
    const el = isVideo ? videoRef.current : audioRef.current
    if (!el) return
    el.src = getMediaUrl(current)
    if (isPlaying) el.play().catch(() => {})
  }, [currentIdx, current, isVideo, getMediaUrl, isPlaying])

  const handleTimeUpdate = useCallback(() => {
    if (!current) return
    const el = isVideo ? videoRef.current : audioRef.current
    if (!el) return
    const offsetMs = el.currentTime * 1000
    setWallTime(fmtWallTime(current.start_time, offsetMs))
    if (callType === 'ptt') {
      setSpeakerInfo(current.speaker_id || '')
    } else {
      setSpeakerInfo(`${caller || ''} \u2192 ${callee || ''}`)
    }
  }, [current, isVideo, callType, caller, callee])

  const handleEnded = useCallback(() => {
    if (currentIdx < selectedSegs.length - 1) {
      setCurrentIdx(prev => prev + 1)
    } else {
      setIsPlaying(false)
    }
  }, [currentIdx, selectedSegs.length])

  function handlePlayAll() {
    if (selectedSegs.length === 0) return
    setCurrentIdx(0)
    setIsPlaying(true)
    setTimeout(() => {
      const el = (selectedSegs[0]?.has_video ? videoRef.current : audioRef.current)
      if (el && selectedSegs[0]) { el.src = getMediaUrl(selectedSegs[0]); el.play().catch(() => {}) }
    }, 50)
  }

  function handleSegClick(seg: RecordingSegment) {
    const idx = selectedSegs.findIndex(s => s.seq === seg.seq)
    if (idx < 0) return
    setCurrentIdx(idx)
    setIsPlaying(true)
  }

  function toggleCheck(seq: number) {
    setChecked(prev => {
      const next = new Set(prev)
      if (next.has(seq)) next.delete(seq); else next.add(seq)
      return next
    })
  }

  function toggleAll() {
    if (checked.size === playable.length) {
      setChecked(new Set())
    } else {
      setChecked(new Set(playable.map(s => s.seq)))
    }
  }

  if (playable.length === 0) {
    return <div style={{ padding: 24, textAlign: 'center', color: 'var(--text-muted)' }}>재생 가능한 세그먼트가 없습니다</div>
  }

  const totalDuration = selectedSegs.reduce((sum, s) => sum + s.duration_ms, 0)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>

      {/* ── 헤더 ── */}
      <div style={{
        display: 'flex', justifyContent: 'space-between', alignItems: 'center',
        padding: '12px 20px', borderBottom: '1px solid var(--border)',
      }}>
        <div>
          <div style={{ fontWeight: 700, fontSize: 16 }}>
            {callType === 'ptt' ? 'PTT 녹취 재생' : '통화 녹취 재생'}
          </div>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 2 }}>
            {callType === 'ptt'
              ? `그룹: ${caller || ''}`
              : `${caller || ''} \u2192 ${callee || ''}`}
          </div>
        </div>
        {onClose && (
          <button onClick={onClose}
            style={{
              background: 'none', border: 'none', fontSize: 20, cursor: 'pointer',
              color: 'var(--text-muted)', padding: '4px 8px', lineHeight: 1,
            }}
            title="닫기">X</button>
        )}
      </div>

      {/* ── 미디어 플레이어 ── */}
      <div style={{ padding: '12px 20px' }}>
        {isVideo ? (
          <div style={{
            position: 'relative',
            width: callType === 'ptt' ? 640 : 1280,
            height: 640,
            maxWidth: '100%',
            overflow: 'hidden', borderRadius: 6,
            background: '#000',
            flexShrink: 0,
          }}>
            <video
              ref={videoRef}
              controls
              onTimeUpdate={handleTimeUpdate}
              onEnded={handleEnded}
              onPlay={() => setIsPlaying(true)}
              onPause={() => setIsPlaying(false)}
              style={{
                width: '100%', height: '100%',
                display: 'block',
                objectFit: 'contain',
              }}
            />
            {/* 영상 내부 오버레이 */}
            {wallTime && (
              <div style={{
                position: 'absolute', top: 8, left: 8, right: 8,
                display: 'flex', justifyContent: 'space-between',
                background: 'rgba(0,0,0,0.55)', color: '#fff',
                padding: '3px 10px', borderRadius: 4, fontSize: 12,
                pointerEvents: 'none', overflow: 'hidden',
              }}>
                <span>{wallTime}</span>
                <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{speakerInfo}</span>
              </div>
            )}
          </div>
        ) : (
          <audio
            ref={audioRef}
            controls
            onTimeUpdate={handleTimeUpdate}
            onEnded={handleEnded}
            onPlay={() => setIsPlaying(true)}
            onPause={() => setIsPlaying(false)}
            style={{ width: '100%' }}
          />
        )}
      </div>

      {/* 음성 재생 시 정보 바 */}
      {!isVideo && wallTime && (
        <div style={{
          display: 'flex', justifyContent: 'space-between', alignItems: 'center',
          padding: '4px 20px', fontSize: 13,
          background: 'var(--bg-secondary, #f5f7fa)', margin: '0 20px', borderRadius: 4,
        }}>
          <span style={{ fontFamily: 'monospace' }}>{wallTime}</span>
          <span style={{ fontWeight: 600 }}>
            {callType === 'ptt' ? `화자: ${speakerInfo}` : speakerInfo}
          </span>
          <span style={{ color: 'var(--text-muted)' }}>
            {currentIdx + 1} / {selectedSegs.length}
          </span>
        </div>
      )}

      {/* ── 재생 컨트롤 ── */}
      <div style={{
        display: 'flex', alignItems: 'center', gap: 10,
        padding: '8px 20px', borderBottom: '1px solid var(--border)',
      }}>
        <button className="btn btn--primary btn--sm" onClick={handlePlayAll}
          disabled={selectedSegs.length === 0}>
          선택 재생 ({selectedSegs.length}건 / {fmtMs(totalDuration)})
        </button>
        <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>
          전체 {playable.length}건
        </span>
      </div>

      {/* ── 세그먼트 목록 ── */}
      <div style={{ flex: 1, overflowY: 'auto', padding: '0 20px 12px' }}>
        <table className="data-table" style={{ fontSize: 13 }}>
          <thead>
            <tr>
              <th style={{ width: 32 }}>
                <input type="checkbox"
                  checked={checked.size === playable.length}
                  onChange={toggleAll} />
              </th>
              <th style={{ width: 32 }}>#</th>
              {callType === 'ptt' && <th>화자</th>}
              <th>시간 구간</th>
              <th style={{ width: 60 }}>길이</th>
              <th style={{ width: 50 }}>상태</th>
            </tr>
          </thead>
          <tbody>
            {playable.map((seg) => {
              const isActive = current?.seq === seg.seq
              const isChecked = checked.has(seg.seq)
              return (
                <tr key={seg.seq}
                  style={{
                    cursor: 'pointer',
                    background: isActive ? 'var(--bg-accent, #e8f0fe)' : undefined,
                    opacity: isChecked ? 1 : 0.45,
                  }}
                  onClick={() => handleSegClick(seg)}
                >
                  <td onClick={e => e.stopPropagation()}>
                    <input type="checkbox" checked={isChecked}
                      onChange={() => toggleCheck(seg.seq)} />
                  </td>
                  <td>{seg.seq}</td>
                  {callType === 'ptt' && <td>{seg.speaker_id}</td>}
                  <td className="ts">{fmtTimeRange(seg.start_time, seg.end_time)}</td>
                  <td className="ts">{fmtMs(seg.duration_ms)}</td>
                  <td>
                    {seg.status === 'ready'
                      ? <span className="badge badge--green" style={{ fontSize: 10 }}>완료</span>
                      : seg.status === 'raw'
                      ? <span className="badge badge--gray" style={{ fontSize: 10 }}>미변환</span>
                      : seg.status === 'transcoding'
                      ? <span className="badge badge--blue" style={{ fontSize: 10 }}>변환중</span>
                      : <span className="badge badge--red" style={{ fontSize: 10 }}>실패</span>}
                  </td>
                </tr>
              )
            })}
            {/* 녹취 중 세그먼트 */}
            {segments.filter(s => s.status === 'recording').map(seg => (
              <tr key={`rec_${seg.seq}`} style={{ opacity: 0.4 }}>
                <td><input type="checkbox" disabled /></td>
                <td>{seg.seq}</td>
                {callType === 'ptt' && <td>{seg.speaker_id}</td>}
                <td className="ts">{fmtTimeRange(seg.start_time, null)}</td>
                <td>-</td>
                <td>
                  <span className="badge badge--blue" style={{ fontSize: 10, animation: 'pulse 1.5s infinite' }}>녹취중</span>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}
