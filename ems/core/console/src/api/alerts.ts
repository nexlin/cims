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
  action: 'open' | 'close' | 'ack'
  message: string
  // 표준 필드 (P0)
  alarm_id?: string
  code?: string
  event_type?: string
  probable_cause?: string
  source?: AlertSource
  effect?: string
  recommended_action?: string
  // P1 ack 라이프사이클
  ack_state?: 'acknowledged' | 'unacknowledged'
  ack_user?: string
  ack_time?: string
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
}
