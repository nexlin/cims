import { AlertTriangle, Maximize2, Play } from 'lucide-react'
import { useState, useRef, useEffect, useCallback } from 'react'
import { recordingsApi, type RecordingSegment } from '../api/recordings'
import { fetchMediaReady } from './useInlineAudio'
import { Button } from '@core/components/ui/button'
import { DataTable, Th, Td } from '@core/components/custom/data-table'
import { Badge } from '@core/components/ui/badge'
import { Checkbox } from '@core/components/ui/checkbox'

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

/** 세그먼트의 화자 표시 — 동시 발언이면 전원(믹스 재생본과 일치) */
function segSpeakers(seg: RecordingSegment): string {
  const ids = seg.speaker_ids?.length ? seg.speaker_ids : (seg.speaker_id ? [seg.speaker_id] : [])
  if (ids.length <= 1) return ids[0] || ''
  return `${ids.join(', ')} (동시 ${seg.max_concurrent ?? ids.length}명)`
}

function fmtMs(ms: number): string {
  const sec = Math.floor(ms / 1000)
  const m = Math.floor(sec / 60)
  const s = sec % 60
  return `${m}:${String(s).padStart(2, '0')}`
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
  // 받아 둔 세그먼트 Blob URL (url → blob:) — 인증 fetch 결과라 element 가 다시 요청하지 않는다. 언마운트 시 해제.
  const blobUrls = useRef<Map<string, string>>(new Map())
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
    const cached = blobUrls.current.get(url)
    if (cached) {
      setPreparingSeq(null); setPrepError('')
      if (el.getAttribute('src') !== cached) el.src = cached
      if (autoplay) el.play().catch(() => {})
      return
    }
    prepAbort.current?.abort()
    const ac = new AbortController()
    prepAbort.current = ac
    setPrepError('')
    if (!(readySeqs.current.has(seg.seq) || seg.status === 'ready')) setPreparingSeq(seg.seq)
    try {
      // failed 세그먼트는 첫 요청에 retry=1 — 실패 마커 해제+재변환 큐잉. 이후엔 일반 폴링(반복 재큐잉 방지).
      const objUrl = await fetchMediaReady(url, ac.signal, { retry: seg.status === 'failed' })
      if (ac.signal.aborted) { URL.revokeObjectURL(objUrl); return }
      readySeqs.current.add(seg.seq)
      blobUrls.current.set(url, objUrl)
      setPreparingSeq(null)
      el.src = objUrl
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

  // 언마운트 시 진행 중 폴링 취소 + Blob URL 해제
  useEffect(() => () => {
    prepAbort.current?.abort()
    blobUrls.current.forEach(u => URL.revokeObjectURL(u))
    blobUrls.current.clear()
  }, [])

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
      // 동시 발언 세그먼트는 화자가 여럿이고 재생본은 믹스다 — 대표 화자만 쓰면 오해를 준다.
      setSpeakerInfo(segSpeakers(current))
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
    return <div className="p-6 text-center text-muted-foreground">재생 가능한 세그먼트가 없습니다</div>
  }

  const totalDuration = selectedSegs.reduce((sum, s) => sum + s.duration_ms, 0)

  return (
    <div className="flex flex-col h-full">

      {/* ── 헤더 ── */}
      <div style={{
        display: 'flex', justifyContent: 'space-between', alignItems: 'center',
        padding: compact ? '6px 12px' : '12px 20px', borderBottom: '1px solid var(--border)',
      }}>
        <div>
          <div style={{ fontWeight: 700, fontSize: compact ? 13 : 16 }}>
            {callType === 'ptt' ? 'PTT 녹취' : '통화 녹취'}
          </div>
          <div style={{ fontSize: 12, color: 'var(--muted-foreground)', marginTop: 2, display: compact ? 'none' : 'block' }}>
            {callType === 'ptt'
              ? `그룹: ${caller || ''}`
              : `${caller || ''} \u2192 ${callee || ''}`}
          </div>
        </div>
        <div className="flex items-center gap-1">
          {onMaximize && (
            <button className="border-0 text-lg cursor-pointer text-muted-foreground py-0.5 px-1.5 leading-none" onClick={onMaximize}
              title="최대화"><Maximize2 size={13} /></button>
          )}
          {onClose && (
            <button className="border-0 text-2xl cursor-pointer text-muted-foreground py-1 px-2 leading-none" onClick={onClose}
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
            // 영상 무대는 테마 표면이 아니라 레터박스다 — 밝은 테마에서도 검정이어야 화면 경계가 보인다.
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
                background: 'rgba(0,0,0,0.55)', color: 'var(--cims-on-solid)',
                padding: '3px 10px', borderRadius: 4, fontSize: 12,
                pointerEvents: 'none', overflow: 'hidden',
              }}>
                <span>{wallTime}</span>
                <span className="overflow-hidden text-ellipsis whitespace-nowrap">{speakerInfo}</span>
              </div>
            )}
          </div>
        ) : (
          <audio className="w-full"
            ref={audioRef}
            controls
            onTimeUpdate={handleTimeUpdate}
            onEnded={handleEnded}
            onPlay={() => setIsPlaying(true)}
            onPause={() => setIsPlaying(false)}/>
        )}

        {/* 변환 진행 / 오류 안내 — 변환 완료 시 자동 재생 (닫았다 다시 열 필요 없음) */}
        {preparingSeq != null && (
          <div className="flex items-center gap-2 mt-2 py-2 px-3 rounded-sm bg-secondary text-md">
            <Badge variant="brandSoft"  style={{ fontSize: 10, animation: 'pulse 1.5s infinite' }}>변환중</Badge>
            <span>녹취를 변환하고 있습니다… 완료되면 자동으로 재생됩니다.</span>
          </div>
        )}
        {prepError && (
          <div style={{
            display: 'flex', alignItems: 'center', gap: 10,
            marginTop: 8, padding: '8px 12px', borderRadius: 6,
            background: 'rgba(220,38,38,0.08)', color: 'var(--destructive)', fontSize: 13,
          }}>
            <span className="inline-flex items-center gap-1"><AlertTriangle size={13} /> 재생 준비 실패: {prepError}</span>
            <Button onClick={() => { if (current) loadSegment(current, true) }}>다시 시도</Button>
          </div>
        )}
      </div>

      {/* 음성 재생 시 정보 바 */}
      {!isVideo && wallTime && (
        <div className="flex justify-between items-center py-1 px-5 text-md bg-secondary my-0 mx-5 rounded-sm">
          <span className="font-mono">{wallTime}</span>
          <span className="font-semibold">
            {callType === 'ptt' ? `화자: ${speakerInfo}` : speakerInfo}
          </span>
          <span className="text-muted-foreground">
            {currentIdx + 1} / {selectedSegs.length}
          </span>
        </div>
      )}

      {/* ── 재생 컨트롤 ── */}
      <div className="flex items-center gap-2.5 py-2 px-5 border-b border-border">
        <Button variant="default" onClick={handlePlayAll}
          disabled={selectedSegs.length === 0}>
          선택 재생 ({selectedSegs.length}건 / {fmtMs(totalDuration)})
        </Button>
        <span className="text-sm text-muted-foreground">
          전체 {playable.length}건
        </span>
      </div>

      {/* ── 세그먼트 목록 ── */}
      <div style={{ flex: 1, overflowY: 'auto', padding: compact ? '0 12px 8px' : '0 20px 12px', maxHeight: compact ? 150 : undefined }}>
        <DataTable sticky>
          <thead>
            <tr>
              <Th className="w-[32px]">
                <Checkbox
                  checked={checked.size === playable.length} onCheckedChange={toggleAll} />
              </Th>
              <Th className="w-[32px]">#</Th>
              {callType === 'ptt' && <Th>화자</Th>}
              <Th>시간 구간</Th>
              <Th className="w-[60px]">길이</Th>
              <Th className="w-[56px]">상태</Th>
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
                    background: isActive ? 'var(--cims-brand-soft)' : undefined,
                    boxShadow: isActive ? 'inset 3px 0 0 var(--primary)' : undefined,
                    fontWeight: isActive ? 600 : undefined,
                    opacity: isChecked ? 1 : 0.45,
                  }}
                  onClick={() => handleSegClick(seg)}
                >
                  <Td onClick={e => e.stopPropagation()}>
                    <Checkbox  checked={isChecked} onCheckedChange={() => toggleCheck(seg.seq)} />
                  </Td>
                  <Td>{isActive && isPlaying ? <Play size={11} /> : seg.seq}</Td>
                  {callType === 'ptt' && <Td>{segSpeakers(seg)}</Td>}
                  <Td className="text-sm text-muted-foreground">{fmtTimeRange(seg.start_time, seg.end_time)}</Td>
                  <Td className="text-sm text-muted-foreground">{fmtMs(seg.duration_ms)}</Td>
                  <Td>
                    {preparingSeq === seg.seq
                      ? <Badge variant="brandSoft"  style={{ fontSize: 10, whiteSpace: 'nowrap', animation: 'pulse 1.5s infinite' }}>변환중</Badge>
                      : seg.status === 'ready'
                      ? <Badge className="whitespace-nowrap" variant="successSoft">완료</Badge>
                      : seg.status === 'raw'
                      ? <Badge className="whitespace-nowrap" variant="neutralSoft">미변환</Badge>
                      : seg.status === 'transcoding'
                      ? <Badge className="whitespace-nowrap" variant="brandSoft">변환중</Badge>
                      : <Badge className="whitespace-nowrap" variant="dangerSoft"
                          title={seg.status_reason || '변환 실패 — 클릭 시 재시도'}>재생불가</Badge>}
                  </Td>
                </tr>
              )
            })}
            {/* 녹취 중 세그먼트 */}
            {segments.filter(s => s.status === 'recording').map(seg => (
              <tr key={`rec_${seg.seq}`} style={{ opacity: 0.4 }}>
                <Td><Checkbox disabled /></Td>
                <Td>{seg.seq}</Td>
                {callType === 'ptt' && <Td>{segSpeakers(seg)}</Td>}
                <Td className="text-sm text-muted-foreground">{fmtTimeRange(seg.start_time, null)}</Td>
                <Td>-</Td>
                <Td>
                  <Badge variant="brandSoft"  style={{ fontSize: 10, whiteSpace: 'nowrap', animation: 'pulse 1.5s infinite' }}>녹취중</Badge>
                </Td>
              </tr>
            ))}
          </tbody>
        </DataTable>
      </div>
    </div>
  )
}
