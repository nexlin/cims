// 단일 <audio> 를 공유하는 인라인 재생 훅 — 이력 페이지의 발언/발화별 ▶ 버튼이 호출.
// 서버는 GET segments/{seq}/audio 요청 시 raw→mp4 변환을 비동기 시작하고,
// 변환 중 202·완료 200 을 반환 → 200 이 될 때까지 폴링한 뒤 <audio src> 지정.
import { useState, useRef, useCallback, useEffect, type ReactElement } from 'react'
import { recordingsApi } from '../api/recordings'

// slot: 동시 발언·전이중 세그먼트의 슬롯 단독 재생. undefined = 믹스(화자 전원 합성).
export type PlayRef = { recId: string; seq: number; slot?: number } | null

export const samePlay = (a: PlayRef, b: PlayRef) =>
  !!a && !!b && a.recId === b.recId && a.seq === b.seq && a.slot === b.slot

export async function waitSegmentReady(url: string, signal: AbortSignal): Promise<void> {
  const deadline = Date.now() + 120_000
  let first = true
  while (Date.now() < deadline) {
    if (signal.aborted) return
    const res = await fetch(url, { method: 'GET', signal, credentials: 'same-origin' })
    try { await res.body?.cancel() } catch { /* noop */ }
    if (res.status === 200) return
    if (res.status === 202) {
      await new Promise(r => setTimeout(r, first ? 700 : 1500))
      first = false
      continue
    }
    throw new Error(`재생 준비 실패 (HTTP ${res.status})`)
  }
  throw new Error('변환 시간 초과')
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
      await waitSegmentReady(url, ac.signal)
      if (ac.signal.aborted) return
      setPreparing(null)
      el.src = url
      setPlaying(ref)
      el.play().catch(() => {})
    } catch (e) {
      if (!ac.signal.aborted) {
        setPreparing(null)
        onError(e instanceof Error ? e.message : '재생 실패')
      }
    }
  }, [playing, onError])

  useEffect(() => () => { abortRef.current?.abort() }, [])

  const node = (
    <audio ref={audioRef} style={{ display: 'none' }} onEnded={() => setPlaying(null)} />
  )
  return { play, stop, playing, preparing, node }
}
