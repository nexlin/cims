// 단일 <audio> 를 공유하는 인라인 재생 훅 — 이력 페이지의 발언/발화별 ▶ 버튼·타임라인 막대가 호출.
// 서버는 GET segments/{seq}/audio|video 요청 시 raw→mp4 변환을 비동기 시작하고,
// 변환 중 202·완료 200 을 반환 → 200 이 될 때까지 폴링한 뒤 element src 지정.
// 영상이 있는 발화(`video: true`)는 audio 대신 video 를 받아 화면 오른쪽 아래 **미니 영상 도크**(360×202, 레터박스)에 튼다 —
// 타임라인 클릭 한 번으로 영상이 보이고, ✕ 나 음성 발화 재생이 도크를 닫는다. [전체] 모달(SegmentPlayer)은 그대로.
import { useState, useRef, useCallback, useEffect, type ReactElement } from 'react'
import { X } from 'lucide-react'
import { recordingsApi } from '../api/recordings'
import { authHeaders } from '../api/client'
import { Button } from './ui/button'

// slot: 동시 발언·전이중 세그먼트의 슬롯 단독 재생. undefined = 믹스(화자 전원 합성).
export type PlayRef = { recId: string; seq: number; slot?: number } | null

export const samePlay = (a: PlayRef, b: PlayRef) =>
  !!a && !!b && a.recId === b.recId && a.seq === b.seq && a.slot === b.slot

/**
 * 세그먼트 미디어를 인증 fetch 로 받아 Blob URL 을 돌려준다.
 * 서버는 GET audio|video 요청 시 raw→mp4 변환을 비동기 시작하고 변환 중 202, 완료 200 을 반환한다 —
 * 200 이 될 때까지 같은 URL 을 폴링한 뒤 본문을 Blob 으로 받는다. 이력·녹취 API 가 role ≥ monitor
 * 인증 게이트이므로 미디어 element 에 URL 을 직접 주지 않고(헤더를 못 붙인다) 이 Blob URL 을 src 로 쓴다.
 * retry=true 면 첫 요청에 ?retry=1 을 붙여 failed 마커를 지우고 1회 재변환을 큐잉한다(이후 일반 폴링).
 * 호출자는 다 쓴 URL 을 URL.revokeObjectURL 로 해제한다.
 */
export async function fetchMediaReady(url: string, signal: AbortSignal, opts: { retry?: boolean } = {}): Promise<string> {
  const deadline = Date.now() + 120_000
  let first = true
  let retry = !!opts.retry
  while (Date.now() < deadline) {
    if (signal.aborted) throw new DOMException('aborted', 'AbortError')
    const reqUrl = retry ? `${url}${url.includes('?') ? '&' : '?'}retry=1` : url
    retry = false
    const res = await fetch(reqUrl, { method: 'GET', signal, headers: authHeaders() })
    if (res.status === 200) {
      const blob = await res.blob()
      return URL.createObjectURL(blob)
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

/** element 의 Blob src 교체 — 이전 Blob URL 해제. */
export function setMediaSrc(el: HTMLMediaElement, objectUrl: string) {
  const prev = el.getAttribute('src')
  if (prev && prev.startsWith('blob:') && prev !== objectUrl) URL.revokeObjectURL(prev)
  el.src = objectUrl
}

export interface InlineAudio {
  /** opts.video = 영상 있는 발화 — 미니 영상 도크로 튼다 */
  play: (recId: string, seq: number, slot?: number, opts?: { video?: boolean }) => Promise<void>
  stop: () => void
  playing: PlayRef
  preparing: PlayRef
  node: ReactElement
}

export function useInlineAudio(onError: (m: string) => void): InlineAudio {
  const audioRef = useRef<HTMLAudioElement>(null)
  const videoRef = useRef<HTMLVideoElement>(null)
  const [playing, setPlaying] = useState<PlayRef>(null)
  const [preparing, setPreparing] = useState<PlayRef>(null)
  const [dock, setDock] = useState<PlayRef>(null)   // 미니 영상 도크에 올라 있는 발화(끝나도 남아 controls 로 다시 볼 수 있다)
  const abortRef = useRef<AbortController | null>(null)

  const closeDock = useCallback(() => {
    const v = videoRef.current
    if (v) { v.pause(); const src = v.getAttribute('src'); if (src && src.startsWith('blob:')) URL.revokeObjectURL(src); v.removeAttribute('src') }
    setDock(null)
  }, [])
  const stop = useCallback(() => {
    abortRef.current?.abort()
    audioRef.current?.pause()
    closeDock()
    setPlaying(null); setPreparing(null)
  }, [closeDock])

  const play = useCallback(async (recId: string, seq: number, slot?: number, opts?: { video?: boolean }) => {
    const video = !!opts?.video
    const ref: PlayRef = { recId, seq, slot }
    // 같은 발언 재클릭 = 토글 정지
    if (samePlay(playing, ref)) {
      (video ? videoRef.current : audioRef.current)?.pause(); setPlaying(null); return
    }
    abortRef.current?.abort()
    const ac = new AbortController()
    abortRef.current = ac
    if (video) { audioRef.current?.pause(); setDock(ref) } else closeDock()
    const url = video ? recordingsApi.segmentVideoUrl(recId, seq, slot) : recordingsApi.segmentAudioUrl(recId, seq, slot)
    setPreparing(ref)
    try {
      const objUrl = await fetchMediaReady(url, ac.signal)
      if (ac.signal.aborted) { URL.revokeObjectURL(objUrl); return }
      setPreparing(null)
      const el = video ? videoRef.current : audioRef.current   // 도크는 setDock 뒤 렌더돼야 ref 가 생긴다
      if (!el) { URL.revokeObjectURL(objUrl); return }
      setMediaSrc(el, objUrl)
      setPlaying(ref)
      el.play().catch(() => {})
    } catch (e) {
      if (!ac.signal.aborted) {
        setPreparing(null)
        onError(e instanceof Error ? e.message : '재생 실패')
      }
    }
  }, [playing, onError, closeDock])

  useEffect(() => () => {
    abortRef.current?.abort()
    for (const el of [audioRef.current, videoRef.current]) {
      const src = el?.getAttribute('src')
      if (src && src.startsWith('blob:')) URL.revokeObjectURL(src)
    }
  }, [])

  const node = (
    <>
      <audio className="hidden" ref={audioRef} onEnded={() => setPlaying(null)}/>
      {dock && (
        <div className="fixed bottom-4 right-4 z-[140] w-[360px] max-w-[calc(100vw-32px)] overflow-hidden rounded-md border border-border bg-card shadow-lg" role="dialog" aria-label="영상 발화 재생">
          <div className="flex items-center gap-2 border-b border-border px-2 py-1 text-xs">
            <b>영상 발화</b><span className="font-mono text-muted-foreground">#{dock.seq}{dock.slot != null ? ` · 슬롯 ${dock.slot}` : ''}</span>
            {preparing && samePlay(preparing, dock) && <span className="text-muted-foreground">변환 중…</span>}
            <Button variant="ghost" size="iconSm" className="ml-auto" onClick={() => { closeDock(); if (samePlay(playing, dock)) setPlaying(null) }} title="닫기"><X size={14} /></Button>
          </div>
          {/* 영상 무대는 테마 표면이 아니라 레터박스 — SegmentPlayer 와 같은 검정 */}
          <video ref={videoRef} controls playsInline className="block h-[202px] w-full bg-black object-contain" onEnded={() => setPlaying(null)} />
        </div>
      )}
    </>
  )
  return { play, stop, playing, preparing, node }
}
