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
  type: string                  // 조건 클래스 (process_down/connection_lost/threshold_crossed)
  code?: string                 // 알람 정의 코드 (A-PRC-001 …)
  perceived_severity?: string   // critical|major|minor|warning|indeterminate
  severity?: string             // 구 호환
  event_type?: string           // communications|qualityOfService|processingError|equipment|environmental
  probable_cause?: string       // X.733 Annex
  mo_class?: string             // software|service|equipment|host|network
  mo_instance?: string          // 명시 시 고정 소스 — 비우면 관측 신원으로 런타임 합성 (표준화 §3.4(b))
  check?: string                // 평가 방식
  target?: string
  threshold?: number
  unit?: string
  metric?: string
  msg_open?: string
  msg_close?: string
  effect?: string
  recommended_action?: string
  scope?: string
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
