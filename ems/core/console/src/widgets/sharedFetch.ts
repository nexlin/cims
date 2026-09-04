// 키(조회 조건)별 공유 로더 — 같은 조건을 보는 위젯이 여러 개여도 조회는 1회.
//
// 화면을 위젯으로 잘게 나누면 같은 응답을 여러 블록이 함께 본다. 블록마다 fetch 하면 같은 요청이
// N번 나가므로, 조회 조건을 키로 삼아 상태를 모듈 단위로 공유한다. 캐시가 있어도 다시 받되 기존 값을
// 계속 보여주므로(stale-while-revalidate) 재진입·조건 전환에서 항상 최신이고 깜빡임이 없다.
import { useEffect, useReducer } from 'react'

export interface Shared<T> { data: T | null; loading: boolean; error: string; updatedAt?: number }

export function makeSharedByKey<T>(fetcher: (key: string) => Promise<T>) {
  const state = new Map<string, Shared<T>>()
  const subs = new Set<() => void>()
  const notify = () => subs.forEach(f => f())
  async function load(key: string) {
    await null                      // 동기 구간에서 상태를 건드리지 않는다(effect 중 setState 방지)
    const cur = state.get(key)
    if (cur?.loading) return        // 진행 중이면 중복 요청 안 함
    state.set(key, { data: cur?.data ?? null, loading: true, error: '' }); notify()
    try { state.set(key, { data: await fetcher(key), loading: false, error: '', updatedAt: Date.now() }) }
    catch (e) {
      state.set(key, { data: cur?.data ?? null, loading: false, error: (e as Error).message,
                       updatedAt: cur?.updatedAt })
    }
    notify()
  }
  // 자동 갱신 타이머 — **키당 하나**다. 같은 조건을 보는 블록이 여러 개여도 폴링은 한 벌이고
  // (load 의 in-flight 가드와 별개로 요청 자체가 늘지 않는다), 마지막 구독자가 떠날 때 멈춘다.
  const timers = new Map<string, { ms: number; id: ReturnType<typeof setInterval>; n: number }>()
  return function useShared(key: string, pollMs = 0): Shared<T> & { reload: () => void } {
    const [, bump] = useReducer((x: number) => x + 1, 0)
    useEffect(() => {
      subs.add(bump); void load(key)
      return () => { subs.delete(bump) }
    }, [key])
    useEffect(() => {
      if (!pollMs) return
      let t = timers.get(key)
      if (t && t.ms !== pollMs) { clearInterval(t.id); timers.delete(key); t = undefined }
      if (!t) {
        t = { ms: pollMs, id: setInterval(() => void load(key), pollMs), n: 0 }
        timers.set(key, t)
      }
      t.n += 1
      return () => {
        const cur = timers.get(key)
        if (!cur) return
        cur.n -= 1
        if (cur.n <= 0) { clearInterval(cur.id); timers.delete(key) }
      }
    }, [key, pollMs])
    return { ...(state.get(key) ?? { data: null, loading: true, error: '' }), reload: () => void load(key) }
  }
}
