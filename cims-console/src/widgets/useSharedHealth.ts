// 공유 health 폴러 — 여러 health 위젯이 동시에 떠도 statsApi.health() 는 5초당 1회만.
// 모듈 싱글톤 + 구독. 마지막 구독자가 unmount 되면 폴링 정지.

import { useState, useEffect } from 'react'
import { statsApi, type HealthResponse } from '../api/stats'

export interface HistorySample {
  registered_users: number
  active_calls: number
  ptt_groups: number
  rtp_used: number
}

const HISTORY_MAX = 60  // 5초 × 60 = 5분

export interface HealthState {
  data: HealthResponse | null
  history: HistorySample[]
  error: string
}

let state: HealthState = { data: null, history: [], error: '' }
const listeners = new Set<(s: HealthState) => void>()
let timer: ReturnType<typeof setInterval> | null = null
let refCount = 0

async function poll() {
  try {
    const res = await statsApi.health()
    const sample: HistorySample = {
      registered_users: res.csp.registered_users,
      active_calls: res.csp.active_calls,
      ptt_groups: res.cmp.groups,
      rtp_used: res.cmp.rtp_ports.used,
    }
    const history = [...state.history, sample].slice(-HISTORY_MAX)
    state = { data: res, history, error: '' }
  } catch (e: unknown) {
    state = { ...state, error: String(e) }
  }
  listeners.forEach(l => l(state))
}

export function useSharedHealth(): HealthState {
  const [s, setS] = useState<HealthState>(state)
  useEffect(() => {
    listeners.add(setS)
    refCount++
    if (timer === null) { poll(); timer = setInterval(poll, 5000) }
    return () => {
      listeners.delete(setS)
      refCount--
      if (refCount === 0 && timer !== null) { clearInterval(timer); timer = null }
    }
  }, [])
  return s
}
