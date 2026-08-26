// 데이터 소스 "스펙" → DataSource 빌더 (완전 데이터 구동).
// 소스는 코드가 아니라 Service Descriptor 의 data_sources[] 데이터로 등록된다. 이 스펙(endpoint +
// shape별 필드 매핑)을 범용 로더가 해석해 이질적 응답을 정규화된 shape 데이터로 변환한다.
// → 새 소스 = descriptor 편집만 (프론트 코드 0).
import { api } from '../../api/client'
import type {
  DataSource, ShapeKind, SourceParams,
  TimeBarData, KpiData, DistributionData, TableData,
} from './types'

// shape별 매핑 선언 (descriptor 데이터의 map[shape]).
interface TimeBarMap { from: string; label: string[]; value: string; unit?: string }
interface KpiMap { items: { label: string; path: string; unit?: string; format?: string }[] }
interface DistMap { fromObject?: string; totalPath: string; from?: string; label?: string[]; value?: string }
interface TableMap { fromObject?: string; from?: string; key?: string; value?: string; columns: [string, string] }

export interface DataSourceSpec {
  id: string
  label: string
  serviceId?: string
  shapes: ShapeKind[]
  endpoint: string                 // '/stats/messages/sip'
  query?: string[]                 // {date,granularity} 중 query 로 붙일 것
  needsControls?: boolean
  map: Partial<Record<ShapeKind, TimeBarMap | KpiMap | DistMap | TableMap>>
}

// 중첩 경로 접근 — 'voip.buckets' → obj.voip.buckets. 정규화 매핑의 핵심.
function getPath(obj: unknown, path: string): unknown {
  if (!path) return obj
  return path.split('.').reduce<unknown>((o, k) => (o == null ? undefined : (o as Record<string, unknown>)[k]), obj)
}

// 후보 필드 중 처음 존재하는 값 (예: bucket 의 'hour' | 'date' — 응답마다 다른 필드명 흡수).
function firstField(item: Record<string, unknown>, fields: string[]): unknown {
  for (const f of fields) if (item[f] !== undefined && item[f] !== null) return item[f]
  return ''
}

function applyFormat(v: unknown, format?: string): string | number {
  const n = Number(v)
  if (format === 'duration') {
    const s = Number.isFinite(n) ? n : 0
    return `${Math.floor(s / 60)}:${String(Math.round(s % 60)).padStart(2, '0')}`
  }
  return (typeof v === 'number' || typeof v === 'string') ? v : (Number.isFinite(n) ? n : '—')
}

function objEntries(raw: unknown, path?: string): [string, number][] {
  const o = path ? getPath(raw, path) : raw
  if (!o || typeof o !== 'object') return []
  return Object.entries(o as Record<string, unknown>).map(([k, v]) => [k, Number(v) || 0])
}

function asArray(raw: unknown, path: string): Record<string, unknown>[] {
  const a = getPath(raw, path)
  return Array.isArray(a) ? a as Record<string, unknown>[] : []
}

// 스펙 → DataSource. 각 adapter 는 raw 응답을 정규화된 shape 데이터로 변환.
export function buildDataSource(spec: DataSourceSpec): DataSource {
  const ds: DataSource = {
    id: spec.id, label: spec.label, serviceId: spec.serviceId,
    shapes: spec.shapes, needsControls: spec.needsControls,
    load: (p: SourceParams) => {
      const pv = p as unknown as Record<string, string>
      const qs = (spec.query ?? [])
        .map(k => `${k}=${encodeURIComponent(pv[k] ?? '')}`)
        .join('&')
      return api.get(`${spec.endpoint}${qs ? (spec.endpoint.includes('?') ? '&' : '?') + qs : ''}`)
    },
  }
  const m = spec.map || {}
  if (m['time-bar']) {
    const c = m['time-bar'] as TimeBarMap
    ds.toTimeBar = (raw): TimeBarData => ({
      unit: c.unit,
      buckets: asArray(raw, c.from).map(it => ({ label: firstField(it, c.label) as string | number, value: Number(it[c.value]) || 0 })),
    })
  }
  if (m.kpi) {
    const c = m.kpi as KpiMap
    // 지표 라벨(선언 순서) — stat 위젯의 [⚙] 지표 선택지. 데이터 도착 전에도 필요하다.
    ds.kpiItems = c.items.map(it => it.label)
    ds.toKpi = (raw): KpiData => ({
      items: c.items.map(it => ({ label: it.label, value: applyFormat(getPath(raw, it.path), it.format), unit: it.unit })),
    })
  }
  if (m.distribution) {
    const c = m.distribution as DistMap
    ds.toDistribution = (raw): DistributionData => ({
      total: Number(getPath(raw, c.totalPath)) || 0,
      items: c.fromObject
        ? objEntries(raw, c.fromObject).map(([k, v]) => ({ label: k, value: v }))
        : asArray(raw, c.from || '').map(it => ({ label: String(firstField(it, c.label || [])), value: Number(it[c.value || '']) || 0 })),
    })
  }
  if (m.table) {
    const c = m.table as TableMap
    ds.toTable = (raw): TableData => ({
      columns: c.columns,
      rows: c.fromObject
        ? objEntries(raw, c.fromObject).map(([k, v]) => ({ key: k, value: v }))
        : asArray(raw, c.from || '').map(it => ({ key: String(it[c.key || '']), value: it[c.value || ''] as string | number })),
    })
  }
  return ds
}
