// 전역 알람 store — 셸 상주 구독 1원화 (alarm_pipeline.md §8.2).
//   /alerts 를 10s 주기로 폴링해 §8.3 판독 규율로 활성 상태를 접고, 헤더 인디케이터·
//   토스트·위젯(활성 알람/배너/토폴로지)이 이 store 하나를 구독한다 — 표시 일관성 +
//   요청 수 절감(종전 위젯 3곳 개별 15s 폴링).
//   useSharedHealth 의 모듈 싱글톤+구독 패턴을 따르되, 셸 상주(헤더가 항상 구독자)라
//   refCount 정지 없이 폴링을 유지한다. 미로그인(토큰 부재) 시 fetch 를 건너뛴다 —
//   401 은 client 가 전체 리로드하므로 폴링이 로그인 화면을 깨지 않게.
import { useSyncExternalStore } from 'react'
import { alertsApi, eventsApi, type AlertEvent, type EventRecord } from '../api/alerts'

const ALERT_POLL_MS = 10_000   // 스윕 30s·FM push 실시간의 절충 (§8.2)
const EVENT_POLL_MS = 60_000   // 드로어 "최근 이벤트" 카운트용 — 토스트 없음 (§8.2 소음 통제)

export const SEV_RANK: Record<string, number> = {
  critical: 5, major: 4, minor: 3, warning: 2, indeterminate: 1,
}
export const SEV_COLOR: Record<string, string> = {
  critical: '#e74c3c', major: '#e67e22', minor: '#f39c12', warning: '#eab308', indeterminate: '#3498db',
}

export interface ActiveAlarm extends AlertEvent {
  acked?: boolean
  ackUser?: string
  occurrences?: number        // 재통지 ×N (§8.3 — 연속 open 은 행 갱신 + 배지)
  last_open_ts?: string
  comments?: { user?: string; text?: string; ts?: string }[]
}

export function severityOf(a: AlertEvent): string {
  return a.perceived_severity || a.severity || 'indeterminate'
}

// §8.3 판독 규율 — 활성 판정은 스트림 접기: open→활성, close→해소, change→현재값 갱신
// (새 행 ❌ close 오인 ❌), ack/comment→활성 행에 누적, 미지 action 무시(전방 호환).
export function foldActive(events: AlertEvent[]): ActiveAlarm[] {
  const asc = [...events].sort((a, b) => (a.ts || '').localeCompare(b.ts || ''))
  const open: Record<string, ActiveAlarm> = {}
  for (const ev of asc) {
    const k = ev.alarm_id ? ev.alarm_id.replace(/@\d+$/, '') : ev.type
    if (!k) continue
    switch (ev.action) {
      case 'open': {
        const cur = open[k]
        if (cur) {   // 재통지 — 기존 행 갱신 + ×N (발생시각 최초 유지)
          Object.assign(cur, ev, {
            ts: cur.ts, action: 'open', acked: cur.acked, ackUser: cur.ackUser,
            occurrences: (cur.occurrences || 1) + 1, last_open_ts: ev.ts,
            comments: cur.comments,
          })
        } else {
          open[k] = { ...ev, occurrences: 1 }
        }
        break
      }
      case 'change': {
        const cur = open[k]
        if (cur) Object.assign(cur, ev, { ts: cur.ts, action: 'open', acked: cur.acked, ackUser: cur.ackUser, occurrences: cur.occurrences, comments: cur.comments })
        break
      }
      case 'ack': {
        const cur = open[k]
        if (cur) { cur.acked = true; cur.ackUser = ev.ack_user }
        break
      }
      case 'comment': {
        const cur = open[k]
        if (cur) (cur.comments = cur.comments || []).push({ user: (ev as any).comment_user, text: (ev as any).comment, ts: ev.ts })
        break
      }
      case 'close':
        delete open[k]
        break
      default:
        break   // 미지 action — 무시 (전방 호환)
    }
  }
  return Object.values(open).sort((a, b) => {
    const d = (SEV_RANK[severityOf(b)] ?? 0) - (SEV_RANK[severityOf(a)] ?? 0)
    return d || (b.ts || '').localeCompare(a.ts || '')
  })
}

// 토폴로지 소비용 — mo_instance 별 최고 severity rank (SystemTopologyWidget 의 서열: critical=4).
export function activeSevByMo(active: ActiveAlarm[]): Map<string, number> {
  const m = new Map<string, number>()
  for (const a of active) {
    const mo = a.source?.mo_instance
    if (!mo) continue
    const r = Math.max(0, (SEV_RANK[severityOf(a)] ?? 1) - 1)   // 5단계 → 토폴로지 4단계 서열
    m.set(mo, Math.max(m.get(mo) ?? 0, r))
  }
  return m
}

export interface AlarmTransition { alarm: ActiveAlarm; kind: 'open' | 'moreSevere' }

export interface AlarmStoreState {
  active: ActiveAlarm[]
  recentEvents: EventRecord[]
  loaded: boolean
  error: boolean            // 마지막 폴링 실패 — "표시 없음 ≠ 정상" 구분 (§8.2 인디케이터)
  lastUpdated: number
}

let state: AlarmStoreState = { active: [], recentEvents: [], loaded: false, error: false, lastUpdated: 0 }
const listeners = new Set<() => void>()
const transitionListeners = new Set<(t: AlarmTransition[]) => void>()

// 신규 판정 = alarm_id 기준 high-water mark (§8.2) — 폴링 중복·재통지를 새 알람으로
// 오인하지 않는다. alarm_id = code@mo@epoch 의 occurrence epoch 를 물마루로 쓴다.
let highWater = 0
let primed = false          // 첫 로드는 물마루만 세팅 (기존 알람 토스트 폭주 방지)
let prevSevByAid: Record<string, string> = {}

function aidEpoch(alarmId?: string): number {
  const m = /@(\d+)$/.exec(alarmId || '')
  return m ? parseInt(m[1], 10) : 0
}

function emit() {
  listeners.forEach(fn => fn())
}

function applyAlerts(events: AlertEvent[]) {
  const active = foldActive(events)
  const transitions: AlarmTransition[] = []
  if (primed) {
    for (const a of active) {
      const sev = severityOf(a)
      if (!(sev === 'critical' || sev === 'major')) continue
      const epoch = aidEpoch(a.alarm_id)
      const prev = a.alarm_id ? prevSevByAid[a.alarm_id] : undefined
      if (epoch > highWater) {
        transitions.push({ alarm: a, kind: 'open' })
      } else if (prev && prev !== sev && (SEV_RANK[sev] ?? 0) > (SEV_RANK[prev] ?? 0)) {
        transitions.push({ alarm: a, kind: 'moreSevere' })   // change 승격 포함 (§8.2)
      }
    }
  }
  highWater = active.reduce((hw, a) => Math.max(hw, aidEpoch(a.alarm_id)), highWater)
  prevSevByAid = {}
  for (const a of active) if (a.alarm_id) prevSevByAid[a.alarm_id] = severityOf(a)
  primed = true
  state = { ...state, active, loaded: true, error: false, lastUpdated: Date.now() }
  emit()
  if (transitions.length) transitionListeners.forEach(fn => fn(transitions))
}

function hasToken(): boolean {
  try { return !!localStorage.getItem('cims_token') } catch { return false }
}

let started = false

function pollAlerts() {
  if (!hasToken()) return
  alertsApi.list({ days: 7, limit: 1000 })
    .then(r => applyAlerts(r.events))
    .catch(() => { state = { ...state, error: true }; emit() })
}

function pollEvents() {
  if (!hasToken()) return
  eventsApi.list({ days: 1, limit: 100 })
    .then(r => { state = { ...state, recentEvents: r.events }; emit() })
    .catch(() => {})
}

function start() {
  if (started) return
  started = true
  pollAlerts(); pollEvents()
  setInterval(pollAlerts, ALERT_POLL_MS)
  setInterval(pollEvents, EVENT_POLL_MS)
}

export function refreshAlarms() {   // ack 등 조작 직후 즉시 반영용
  pollAlerts()
}

export function onAlarmTransition(fn: (t: AlarmTransition[]) => void): () => void {
  transitionListeners.add(fn)
  return () => transitionListeners.delete(fn)
}

function subscribe(fn: () => void): () => void {
  start()
  listeners.add(fn)
  return () => listeners.delete(fn)
}

export function useAlarms(): AlarmStoreState {
  return useSyncExternalStore(subscribe, () => state)
}
