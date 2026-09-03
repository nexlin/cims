// 비정상 세션 이력 — 조회 조건·결과 store (모듈 싱글톤).
//
// 화면이 카드 하나로 묶여 있어도 **안의 블록은 각각 위젯**이라(console_platform §3.0.1) 조회 조건
// 블록이 고른 날짜를 지표·IP·표 블록이 함께 봐야 한다. 그래서 조건과 결과를 store 로 끌어올린다.
import { useEffect, useState } from 'react'
import { api } from '@core/api/client'

export interface AbnSession {
  sesid: string; peer_ip: string; date: string
  caller: string; callee: string; ua: string
  methods: string[]; statuses: string[]
  attempts: number; first_ts: string; last_ts: string
  got_2xx: boolean; reasons: string[]; severity: 'critical' | 'major' | 'minor'
}
export interface AbnResp {
  date: string; days: number; total: number
  by_ip: Record<string, number>
  by_reason: Record<string, number>
  sessions: AbnSession[]
}

export interface AbnState {
  date: string
  days: number
  data: AbnResp | null
  loading: boolean
  page: number
  pageSize: number
}

const today = () => new Date().toISOString().substring(0, 10)

let state: AbnState = { date: today(), days: 1, data: null, loading: false, page: 0, pageSize: 100 }
const subs = new Set<() => void>()
function set(patch: Partial<AbnState>) {
  state = { ...state, ...patch }
  subs.forEach(fn => fn())
}
let started = false

export type Notify = (msg: string, kind?: 'ok' | 'err') => void

export const abnormal = {
  get: () => state,
  setDate: (date: string) => set({ date }),
  setDays: (days: number) => set({ days }),
  setPage: (page: number) => set({ page }),
  setPageSize: (pageSize: number) => set({ pageSize, page: 0 }),
  async load(notify?: Notify) {
    set({ loading: true })
    try {
      const r = await api.get<AbnResp>(
        `/security/abnormal-sessions?date=${state.date}&days=${state.days}`)
      set({ data: r, page: 0 })
    } catch (e) {
      notify?.(String(e), 'err')
    } finally { set({ loading: false }) }
  },
}

// 파생값 — 블록마다 같은 계산을 반복하지 않게 한곳에서.
export function abnDerived(s: AbnState) {
  const sessions = s.data?.sessions ?? []
  return {
    sessions,
    critical: sessions.filter(x => x.severity === 'critical').length,
    scanners: s.data?.by_reason?.scanner_ua ?? 0,
    srcIps: Object.keys(s.data?.by_ip ?? {}).length,
    topIps: Object.entries(s.data?.by_ip ?? {}).sort((a, b) => b[1] - a[1]).slice(0, 8),
    pageCount: Math.max(1, Math.ceil(sessions.length / s.pageSize)),
    pageRows: sessions.slice(s.page * s.pageSize, (s.page + 1) * s.pageSize),
  }
}

// 구독 훅 — 블록이 몇 개든 첫 구독자만 조회를 건다. 조건이 바뀌면 그때 다시.
export function useAbnormal(notify?: Notify): AbnState {
  const [, tick] = useState(0)
  useEffect(() => {
    const fn = () => tick(t => t + 1)
    subs.add(fn)
    if (!started) { started = true; void abnormal.load(notify) }
    return () => { subs.delete(fn) }
  }, [])   // eslint-disable-line react-hooks/exhaustive-deps
  return state
}
