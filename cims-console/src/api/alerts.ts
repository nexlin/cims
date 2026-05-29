import { api } from './client'

export interface AlertEvent {
  ts: string
  type: string
  severity: 'critical' | 'warning' | 'info'
  action: 'open' | 'close'
  message: string
}

export interface AlertsResponse {
  days: number
  count: number
  events: AlertEvent[]
}

export interface AlertSummaryByType {
  type: string
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
  severity: 'critical' | 'warning' | 'info'
  metric: string
  condition: string
  threshold: number | null
  unit: string | null
}

export interface AlertRulesResponse {
  editable: boolean
  sweep_sec: number
  rules: AlertRule[]
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
}
