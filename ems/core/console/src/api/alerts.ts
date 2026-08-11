import { api } from './client'

// X.733/3GPP 32.111 perceived severity.
export type PerceivedSeverity = 'critical' | 'major' | 'minor' | 'warning' | 'indeterminate' | 'cleared' | 'info'

export interface AlertSource {
  mo_class?: string
  mo_instance?: string
  detected_by?: string
}

export interface AlertEvent {
  ts: string
  type: string                  // 조건 클래스 (process_down/connection_lost/threshold_crossed)
  severity?: PerceivedSeverity   // 구 호환
  perceived_severity?: PerceivedSeverity
  action: 'open' | 'close' | 'ack' | 'change' | 'comment'   // change = severity 변경 (notifyChangedAlarm)
  message: string
  // 표준 필드 (P0)
  alarm_id?: string
  code?: string
  event_type?: string
  probable_cause?: string
  source?: AlertSource
  effect?: string
  recommended_action?: string
  // 발생/해제/변경 시각 (32.111 alarmRaisedTime/ClearedTime/ChangedTime)
  raised_time?: string
  clear_time?: string
  change_time?: string
  trend_indication?: 'moreSevere' | 'lessSevere'   // change 동반
  threshold_info?: { observed: number; threshold: number; unit?: string }   // 임계 계열
  // P1 ack/코멘트 라이프사이클
  ack_state?: 'acknowledged' | 'unacknowledged'
  ack_user?: string
  ack_time?: string
  comment?: string              // action=comment (setComment)
  comment_user?: string
  comment_time?: string
}

export interface AlertsResponse {
  days: number
  count: number
  events: AlertEvent[]
}

export interface AlertSummaryByType {
  key?: string
  type: string
  code?: string
  mo_instance?: string
  perceived_severity?: PerceivedSeverity
  opens: number
  resolved: number
  currently_open: boolean
  avg_duration_sec: number | null
  last_ts: string
}

export interface AlertSummaryDaily {
  date: string
  opens: number
}

export interface AlertSummaryResponse {
  days: number
  by_type: AlertSummaryByType[]
  daily: AlertSummaryDaily[]
}

export interface AlertRule {
  type: string
  code?: string
  severity?: PerceivedSeverity
  perceived_severity?: PerceivedSeverity
  event_type?: string
  probable_cause?: string
  mo_class?: string
  mo_instance?: string
  metric: string
  condition: string
  threshold: number | null
  thresholds?: Record<string, number> | null   // 단계 임계 {severity: value}
  unit: string | null
  effect?: string
  recommended_action?: string
  scope?: string
}

export interface AlertRulesResponse {
  editable: boolean
  sweep_sec: number
  rules: AlertRule[]
}

export interface AlarmCatalogItem {
  code: string
  type: string
  perceived_severity?: PerceivedSeverity
  event_type?: string
  probable_cause?: string
  mo_class?: string
  metric?: string
  effect?: string
  recommended_action?: string
  origin?: string               // 'rule' | 'module:<module>' (자기보고 등록분)
}

// 이벤트(정상 동작 통지) — 알람과 스트림 분리 (X.730/731 stateChange · X.740 audit).
export interface EventRecord {
  ts: string
  type: string                  // process_started / config_reloaded / ...
  code?: string                 // E-STC-* / E-AUD-* (재정의 이벤트 코드 — kind=DOMAIN 약어 STC/AUD)
  kind?: 'stateChange' | 'audit' | string
  source?: AlertSource
  message: string
  params?: Record<string, unknown>
}

export interface EventsResponse {
  days: number
  count: number
  events: EventRecord[]
}

export const alertsApi = {
  list: (params: { days?: number; type?: string; limit?: number } = {}) => {
    const p = new URLSearchParams()
    if (params.days) p.set('days', String(params.days))
    if (params.type) p.set('type', params.type)
    if (params.limit) p.set('limit', String(params.limit))
    const s = p.toString()
    return api.get<AlertsResponse>(`/alerts${s ? '?' + s : ''}`)
  },
  types: () => api.get<{ types: string[] }>('/alerts/types'),
  summary: (days?: number) => {
    const q = days ? `?days=${days}` : ''
    return api.get<AlertSummaryResponse>(`/alerts/summary${q}`)
  },
  rules: () => api.get<AlertRulesResponse>('/alerts/rules'),
  catalog: () => api.get<{ catalog: AlarmCatalogItem[] }>('/alerts/catalog'),
  ack: (alarmId: string) => api.post<{ ok: boolean; ack_user: string; ack_time: string }>('/alerts/ack', { alarm_id: alarmId }),
  comment: (alarmId: string, text: string) =>
    api.post<{ ok: boolean; comment_user: string; comment_time: string }>('/alerts/comment', { alarm_id: alarmId, text }),
}

export const eventsApi = {
  list: (params: { days?: number; type?: string; kind?: string; limit?: number } = {}) => {
    const p = new URLSearchParams()
    if (params.days) p.set('days', String(params.days))
    if (params.type) p.set('type', params.type)
    if (params.kind) p.set('kind', params.kind)
    if (params.limit) p.set('limit', String(params.limit))
    const s = p.toString()
    return api.get<EventsResponse>(`/events${s ? '?' + s : ''}`)
  },
  types: () => api.get<{ types: string[] }>('/events/types'),
}
