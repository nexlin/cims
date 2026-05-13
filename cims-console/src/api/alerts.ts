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
}
