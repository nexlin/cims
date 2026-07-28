import { useState, useRef, useEffect, useCallback } from 'react'
import { recordingsApi, type RecordingSegment } from '../api/recordings'

interface SegmentPlayerProps {
  segments: RecordingSegment[]
  recordingId: string
  callType: 'volte' | 'ptt' | 'volte_video'
  caller?: string
  callee?: string
  onClose?: () => void
  compact?: boolean          // 인라인(accordion) 축소 배치
  onMaximize?: () => void    // 최대화(모달) 버튼 — compact 에서 오버레이 표시
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

/**
 * 세그먼트 변환 완료까지 대기.
 * 서버는 재생 요청(GET audio|video) 시 raw → mp4 변환을 비동기 시작하고
 * 변환 중에는 202(transcoding), 완료되면 200 을 반환한다. 같은 URL 을 폴링하여
 * 200 이 될 때까지 기다린 뒤 호출자가 미디어 element 에 src 를 지정한다.
 * (본문은 받지 않고 상태코드만 확인 — element 가 다시 요청해 재생.)
 */
async function waitSegmentReady(url: string, signal: AbortSignal): Promise<void> {
  const deadline = Date.now() + 120_000
  let first = true
  while (Date.now() < deadline) {
    if (signal.aborted) return
    const res = await fetch(url, { method: 'GET', signal, credentials: 'same-origin' })
    if (res.status === 200) {
      try { await res.body?.cancel() } catch { /* noop */ }
      return
    }
    if (res.status === 202) {
      try { await res.body?.cancel() } catch { /* noop */ }
      await new Promise(r => setTimeout(r, first ? 700 : 1500))
      first = false
      continue
    }
    // failed 등 — 서버가 사유(message/reason)를 주면 그대로 표기
    let detail = ''
    try {
      const body = await res.json()
      detail = body?.message || body?.reason || body?.error || ''
    } catch { /* noop */ }
    throw new Error(detail || `재생 준비 실패 (HTTP ${res.status})`)
  }
  throw new Error('변환 시간 초과')
}

export default function SegmentPlayer({ segments, recordingId, callType, caller, callee, onClose, compact, onMaximize }: SegmentPlayerProps) {
  // 재생 가능한 세그먼트만 (recording 상태 제외)
  const playable = segments.filter(s => s.status !== 'recording')

  // 체크박스: 기본 전체 선택 — 단 failed(재생불가)는 제외해 연속재생이 걸리지 않게 한다
  const [checked, setChecked] = useState<Set<number>>(() => new Set(playable.filter(s => s.status !== 'failed').map(s => s.seq)))
  const selectedSegs = playable.filter(s => checked.has(s.seq))

  const [currentIdx, setCurrentIdx] = useState(0)
  const [isPlaying, setIsPlaying] = useState(false)
  const [wallTime, setWallTime] = useState('')
  const [speakerInfo, setSpeakerInfo] = useState('')
  const [preparingSeq, setPreparingSeq] = useState<number | null>(null)  // 변환 대기 중인 seq
  const [prepError, setPrepError] = useState('')
  const [playToken, setPlayToken] = useState(0)                          // 같은 세그먼트 재요청 트리거
  const audioRef = useRef<HTMLAudioElement>(null)
  const videoRef = useRef<HTMLVideoElement>(null)
  const readySeqs = useRef<Set<number>>(new Set(playable.filter(s => s.status === 'ready').map(s => s.seq)))
  const prepAbort = useRef<AbortController | null>(null)
  const activeRowRef = useRef<HTMLTableRowElement | null>(null)

  const current = selectedSegs[currentIdx]
  const isVideo = current?.has_video

  const getMediaUrl = useCallback((seg: RecordingSegment) => {
    if (seg.has_video) return recordingsApi.segmentVideoUrl(recordingId, seg.seq)
    return recordingsApi.segmentAudioUrl(recordingId, seg.seq)
  }, [recordingId])

  // 세그먼트 로드. 변환 전(raw/transcoding)이면 완료까지 폴링한 뒤 재생 —
  // 다이얼로그를 닫았다 다시 열 필요 없이 "변환 중" 표시 후 자동 재생.
  // failed 세그먼트는 ?retry=1 로 폴링해 실패 마커를 지우고 1회 재변환을 시도한다.
  const loadSegment = useCallback(async (seg: RecordingSegment, autoplay: boolean) => {
    const el = seg.has_video ? videoRef.current : audioRef.current
    if (!el) return
    const url = getMediaUrl(seg)
    if (readySeqs.current.has(seg.seq) || seg.status === 'ready') {
      setPreparingSeq(null); setPrepError('')
      if (el.getAttribute('src') !== url) el.src = url
      if (autoplay) el.play().catch(() => {})
      return
    }
    prepAbort.current?.abort()
    const ac = new AbortController()
    prepAbort.current = ac
    setPrepError(''); setPreparingSeq(seg.seq)
    try {
      if (seg.status === 'failed') {
        // 재시도 priming 1회 — 실패 마커 해제+재변환 큐잉. 이후엔 일반 폴링(반복 재큐잉 방지).
        const res = await fetch(`${url}${url.includes('?') ? '&' : '?'}retry=1`,
          { method: 'GET', signal: ac.signal, credentials: 'same-origin' })
        if (res.status !== 200 && res.status !== 202) {
          let detail = ''
          try { const b = await res.json(); detail = b?.message || b?.reason || '' } catch { /* noop */ }
          throw new Error(detail || `재생 준비 실패 (HTTP ${res.status})`)
        }
        try { await res.body?.cancel() } catch { /* noop */ }
      }
      await waitSegmentReady(url, ac.signal)
      if (ac.signal.aborted) return
      readySeqs.current.add(seg.seq)
      setPreparingSeq(null)
      el.src = url
      if (autoplay) el.play().catch(() => {})
    } catch (e) {
      if (!ac.signal.aborted) {
        setPreparingSeq(null)
        setPrepError(e instanceof Error ? e.message : '변환 실패')
      }
    }
  }, [getMediaUrl])

  // 현재 세그먼트 변경/재생요청 시 로드 (변환 전이면 폴링→자동재생)
  useEffect(() => {
    if (!current) return
    loadSegment(current, isPlaying)
    // isPlaying 은 playToken/currentIdx 변경 시점의 값만 사용 (pause 시 재로드 방지)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [currentIdx, playToken, current?.seq])

  // 언마운트 시 진행 중 폴링 취소
  useEffect(() => () => { prepAbort.current?.abort() }, [])

  // 재생 세그먼트 변경 시 목록에서 현재 행이 보이도록 자동 스크롤
  useEffect(() => {
    activeRowRef.current?.scrollIntoView({ block: 'nearest' })
  }, [current?.seq])

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
      // 미디어 종료 시 'pause' 이벤트가 'ended' 보다 먼저 와 isPlaying 이 꺼진다 —
      // 연속 재생 의도를 복원해야 다음 세그먼트가 자동 재생된다.
      setIsPlaying(true)
      setCurrentIdx(prev => prev + 1)
    } else {
      setIsPlaying(false)
    }
  }, [currentIdx, selectedSegs.length])

  function handlePlayAll() {
    if (selectedSegs.length === 0) return
    setCurrentIdx(0)
    setIsPlaying(true)
    setPlayToken(t => t + 1)
  }

  function handleSegClick(seg: RecordingSegment) {
    let idx = selectedSegs.findIndex(s => s.seq === seg.seq)
    if (idx < 0) {
      // 미선택(예: failed 기본 제외) 세그먼트 클릭 — 선택에 포함시키고 그 위치에서 재생
      const next = new Set(checked)
      next.add(seg.seq)
      setChecked(next)
      idx = playable.filter(s => next.has(s.seq)).findIndex(s => s.seq === seg.seq)
      if (idx < 0) return
    }
    setCurrentIdx(idx)
    setIsPlaying(true)
    setPlayToken(t => t + 1)
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
        padding: compact ? '6px 12px' : '12px 20px', borderBottom: '1px solid var(--border)',
      }}>
        <div>
          <div style={{ fontWeight: 700, fontSize: compact ? 13 : 16 }}>
            {callType === 'ptt' ? 'PTT 녹취' : '통화 녹취'}
          </div>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 2, display: compact ? 'none' : 'block' }}>
            {callType === 'ptt'
              ? `그룹: ${caller || ''}`
              : `${caller || ''} \u2192 ${callee || ''}`}
          </div>
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
          {onMaximize && (
            <button onClick={onMaximize}
              style={{ background: 'none', border: 'none', fontSize: 16, cursor: 'pointer',
                color: 'var(--text-muted)', padding: '2px 6px', lineHeight: 1 }}
              title="최대화">⛶</button>
          )}
          {onClose && (
            <button onClick={onClose}
              style={{
                background: 'none', border: 'none', fontSize: 20, cursor: 'pointer',
                color: 'var(--text-muted)', padding: '4px 8px', lineHeight: 1,
              }}
              title="닫기">X</button>
          )}
        </div>
      </div>

      {/* ── 미디어 플레이어 ── */}
      <div style={{ padding: compact ? '8px 12px' : '12px 20px' }}>
        {isVideo ? (
          <div style={{
            position: 'relative',
            width: compact ? 360 : (callType === 'ptt' ? 640 : 1280),
            height: compact ? 220 : 640,
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

        {/* 변환 진행 / 오류 안내 — 변환 완료 시 자동 재생 (닫았다 다시 열 필요 없음) */}
        {preparingSeq != null && (
          <div style={{
            display: 'flex', alignItems: 'center', gap: 8,
            marginTop: 8, padding: '8px 12px', borderRadius: 6,
            background: 'var(--bg-secondary, #f5f7fa)', fontSize: 13,
          }}>
            <span className="badge badge--blue" style={{ fontSize: 10, animation: 'pulse 1.5s infinite' }}>변환중</span>
            <span>녹취를 변환하고 있습니다… 완료되면 자동으로 재생됩니다.</span>
          </div>
        )}
        {prepError && (
          <div style={{
            display: 'flex', alignItems: 'center', gap: 10,
            marginTop: 8, padding: '8px 12px', borderRadius: 6,
            background: 'rgba(220,38,38,0.08)', color: 'var(--danger, #dc2626)', fontSize: 13,
          }}>
            <span>⚠️ 재생 준비 실패: {prepError}</span>
            <button className="btn btn--sm" onClick={() => { if (current) loadSegment(current, true) }}>다시 시도</button>
          </div>
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
      <div style={{ flex: 1, overflowY: 'auto', padding: compact ? '0 12px 8px' : '0 20px 12px', maxHeight: compact ? 150 : undefined }}>
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
              <th style={{ width: 56 }}>상태</th>
            </tr>
          </thead>
          <tbody>
            {playable.map((seg) => {
              const isActive = current?.seq === seg.seq
              const isChecked = checked.has(seg.seq)
              return (
                <tr key={seg.seq}
                  ref={isActive ? activeRowRef : undefined}
                  style={{
                    cursor: 'pointer',
                    background: isActive ? 'var(--bg-accent, #e8f0fe)' : undefined,
                    boxShadow: isActive ? 'inset 3px 0 0 var(--primary, #4f6ef7)' : undefined,
                    fontWeight: isActive ? 600 : undefined,
                    opacity: isChecked ? 1 : 0.45,
                  }}
                  onClick={() => handleSegClick(seg)}
                >
                  <td onClick={e => e.stopPropagation()}>
                    <input type="checkbox" checked={isChecked}
                      onChange={() => toggleCheck(seg.seq)} />
                  </td>
                  <td>{isActive && isPlaying ? '▶' : seg.seq}</td>
                  {callType === 'ptt' && <td>{seg.speaker_id}</td>}
                  <td className="ts">{fmtTimeRange(seg.start_time, seg.end_time)}</td>
                  <td className="ts">{fmtMs(seg.duration_ms)}</td>
                  <td>
                    {preparingSeq === seg.seq
                      ? <span className="badge badge--blue" style={{ fontSize: 10, whiteSpace: 'nowrap', animation: 'pulse 1.5s infinite' }}>변환중</span>
                      : seg.status === 'ready'
                      ? <span className="badge badge--green" style={{ fontSize: 10, whiteSpace: 'nowrap' }}>완료</span>
                      : seg.status === 'raw'
                      ? <span className="badge badge--gray" style={{ fontSize: 10, whiteSpace: 'nowrap' }}>미변환</span>
                      : seg.status === 'transcoding'
                      ? <span className="badge badge--blue" style={{ fontSize: 10, whiteSpace: 'nowrap' }}>변환중</span>
                      : <span className="badge badge--red" style={{ fontSize: 10, whiteSpace: 'nowrap' }}
                          title={seg.status_reason || '변환 실패 — 클릭 시 재시도'}>재생불가</span>}
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
                  <span className="badge badge--blue" style={{ fontSize: 10, whiteSpace: 'nowrap', animation: 'pulse 1.5s infinite' }}>녹취중</span>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}
