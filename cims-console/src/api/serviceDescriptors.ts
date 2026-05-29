// Service Descriptor API (OAM /api/v1/service-descriptors) — 5-6.
import { api } from './client'

export interface ServiceModule {
  name: string
  port?: number
  proto?: string
  controllable?: boolean
  service_id?: string
}

export interface AlertRule {
  type: string
  severity?: string
  check?: string
  target?: string
  threshold?: number
  unit?: string
  metric?: string
  msg_open?: string
  msg_close?: string
}

import type { DataSourceSpec } from '../widgets/shapes/dataSourceSpec'

export interface ServiceDescriptor {
  id: string
  label?: string
  modules: ServiceModule[]
  alert_rules?: AlertRule[]
  data_sources?: DataSourceSpec[]
  update_time?: string
}

export const serviceDescriptorsApi = {
  list:   () => api.get<{ services: ServiceDescriptor[] }>('/service-descriptors'),
  get:    (id: string) => api.get<ServiceDescriptor>(`/service-descriptors/${encodeURIComponent(id)}`),
  put:    (id: string, body: ServiceDescriptor) =>
    api.put<ServiceDescriptor>(`/service-descriptors/${encodeURIComponent(id)}`, body),
  remove: (id: string) => api.delete<{ deleted: boolean }>(`/service-descriptors/${encodeURIComponent(id)}`),
  // 전 descriptor 의 data_sources 병합 — shape 위젯 소스 카탈로그.
  dataSources: () => api.get<{ data_sources: DataSourceSpec[] }>('/service-descriptors/data-sources'),
}
