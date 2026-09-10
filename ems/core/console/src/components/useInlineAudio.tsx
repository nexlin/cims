// 단일 <audio> 를 공유하는 인라인 재생 훅 — 이력 페이지의 발언/발화별 ▶ 버튼이 호출.
// 서버는 GET segments/{seq}/audio 요청 시 raw→mp4 변환을 비동기 시작하고,
// 변환 중 202·완료 200 을 반환 → 200 이 될 때까지 폴링한 뒤 <audio src> 지정.
import { useState, useRef, useCallback, useEffect, type ReactElement } from 'react'
import { recordingsApi } from '../api/recordings'
import { authHeaders } from '../api/client'

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
  play: (recId: string, seq: number, slot?: number) => Promise<void>
  stop: () => void
  playing: PlayRef
  preparing: PlayRef
  node: ReactElement
}

export function useInlineAudio(onError: (m: string) => void): InlineAudio {
  const audioRef = useRef<HTMLAudioElement>(null)
  const [playing, setPlaying] = useState<PlayRef>(null)
  const [preparing, setPreparing] = useState<PlayRef>(null)
  const abortRef = useRef<AbortController | null>(null)

  const stop = useCallback(() => {
    abortRef.current?.abort()
    audioRef.current?.pause()
    setPlaying(null); setPreparing(null)
  }, [])

  const play = useCallback(async (recId: string, seq: number, slot?: number) => {
    const el = audioRef.current
    if (!el) return
    const ref: PlayRef = { recId, seq, slot }
    // 같은 발언 재클릭 = 토글 정지
    if (samePlay(playing, ref)) {
      el.pause(); setPlaying(null); return
    }
    abortRef.current?.abort()
    const ac = new AbortController()
    abortRef.current = ac
    const url = recordingsApi.segmentAudioUrl(recId, seq, slot)
    setPreparing(ref)
    try {
      const objUrl = await fetchMediaReady(url, ac.signal)
      if (ac.signal.aborted) { URL.revokeObjectURL(objUrl); return }
      setPreparing(null)
      setMediaSrc(el, objUrl)
      setPlaying(ref)
      el.play().catch(() => {})
    } catch (e) {
      if (!ac.signal.aborted) {
        setPreparing(null)
        onError(e instanceof Error ? e.message : '재생 실패')
      }
    }
  }, [playing, onError])

  useEffect(() => () => {
    abortRef.current?.abort()
    const src = audioRef.current?.getAttribute('src')
    if (src && src.startsWith('blob:')) URL.revokeObjectURL(src)
  }, [])

  const node = (
    <audio className="hidden" ref={audioRef} onEnded={() => setPlaying(null)}/>
  )
  return { play, stop, playing, preparing, node }
}
