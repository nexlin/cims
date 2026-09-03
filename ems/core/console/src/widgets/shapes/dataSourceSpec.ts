// 데이터 소스 "스펙" → DataSource 빌더 (완전 데이터 구동).
// 소스는 코드가 아니라 Service Descriptor 의 data_sources[] 데이터로 등록된다. 이 스펙(endpoint +
// shape별 필드 매핑)을 범용 로더가 해석해 이질적 응답을 정규화된 shape 데이터로 변환한다.
// → 새 소스 = descriptor 편집만 (프론트 코드 0).
import { api } from '../../api/client'
import type {
  DataSource, ShapeKind, SourceParams,
  TimeBarData, SeriesBarData, KpiData, DistributionData, TableData, MatrixData,
} from './types'

// shape별 매핑 선언 (descriptor 데이터의 map[shape]).
// value 는 버킷 행 기준 **중첩 경로**를 받는다('ptt.sessions'). 한 겹 이름도 그대로 동작.
interface TimeBarMap { from: string; label: string[]; value: string; unit?: string }
// 계열 시계열 — 한 버킷 행에서 계열마다 다른 필드를 읽는다. 색은 선언 순서대로 --chart-1..5.
interface SeriesBarMap {
  from: string; label: string[]; unit?: string
  series: { key: string; label: string; value: string; color?: string; includes?: string[] }[]
}
interface KpiMap { items: { label: string; path: string; unit?: string; format?: string }[] }
interface DistMap {
  fromObject?: string; totalPath: string; from?: string; label?: string[]; value?: string
  // 계열 분해(선택) — partsObject[항목라벨] = {계열키: 수}. series 는 색·순서를 정한다.
  partsObject?: string
  series?: { key: string; label: string; value: string; color?: string }[]
}
interface TableMap { fromObject?: string; from?: string; key?: string; value?: string; columns: [string, string] }
/**
 * 교차표 — 행은 버킷 배열, 칸은 버킷 안의 {항목: 수} map.
 *   from        버킷 배열 경로
 *   label       행 라벨 후보 필드
 *   cells       버킷 행 기준, {항목: 수} map 의 경로. 여러 개면 **합산**한다
 *               (예: ['in','out'] → 수신+송신 합).
 *   limit       열 상한(합계 내림차순). 초과분은 '기타' 로 접는다.
 */
interface MatrixMap {
  from: string; label: string[]; limit?: number; unit?: string
  /** 동적 열 — 버킷 안의 {항목: 수} map 경로들. 여러 개면 합산(예: ['in','out']). */
  cells?: string[]
  /**
   * 고정 열 — 열이 미리 정해진 표(호 통계의 시도·성립·소통·완료).
   *   path      버킷 행 기준 경로
   *   totalPath 합계 행 값의 **절대 경로**. 비율처럼 합산이 무의미한 열에 쓴다
   *             (없으면 행들의 합).
   */
  columns?: { key: string; label: string; path: string; totalPath?: string; unit?: string }[]
}

export interface DataSourceSpec {
  id: string
  label: string
  serviceId?: string
  shapes: ShapeKind[]
  endpoint: string                 // '/stats/messages/sip'
  query?: string[]                 // {date,granularity} 중 query 로 붙일 것
  needsControls?: boolean
  map: Partial<Record<ShapeKind, TimeBarMap | SeriesBarMap | KpiMap | DistMap | TableMap | MatrixMap>>
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

// 계열 색 — 소스가 토큰 이름을 적으면 그것, 아니면 선언 순서대로 --chart-1..5.
// 토큰 이름만 받는다(리터럴 금지) — 색은 테마가 정한다(console_platform §3.6).
function seriesColor(token: string | undefined, i: number): string {
  return `var(--${token && /^[a-z0-9-]+$/.test(token) ? token : `chart-${(i % 5) + 1}`})`
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
    shapes: spec.shapes, needsControls: spec.needsControls, endpoint: spec.endpoint,
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
      buckets: asArray(raw, c.from).map(it => ({ label: firstField(it, c.label) as string | number, value: Number(getPath(it, c.value)) || 0 })),
    })
  }
  if (m['series-bar']) {
    const c = m['series-bar'] as SeriesBarMap
    ds.toSeriesBar = (raw): SeriesBarData => ({
      unit: c.unit,
      // 색은 선언 순서에 고정 — 조회 조건이 바뀌어도 같은 계열이 같은 색을 유지한다.
      series: c.series.map((sp, i) => ({
        key: sp.key, label: sp.label, includes: sp.includes, color: seriesColor(sp.color, i),
      })),
      buckets: asArray(raw, c.from).map(it => ({
        label: firstField(it, c.label) as string | number,
        values: Object.fromEntries(c.series.map(sp => [sp.key, Number(getPath(it, sp.value)) || 0])),
      })),
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
  if (m.matrix) {
    const c = m.matrix as MatrixMap
    // 고정 열 — 열이 같은 축이 아니므로 행 합계를 내지 않는다.
    if (c.columns?.length) {
      ds.toMatrix = (raw): MatrixData => {
        const specs = c.columns as NonNullable<MatrixMap['columns']>
        const rows = asArray(raw, c.from).map(it => {
          const cells: Record<string, number> = {}
          for (const sp of specs) cells[sp.key] = Number(getPath(it, sp.path)) || 0
          return { label: String(firstField(it, c.label) ?? ''), cells, total: 0 }
        })
        const columns = specs.map(sp => ({
          key: sp.key, label: sp.label, unit: sp.unit,
          total: sp.totalPath !== undefined
            ? (Number(getPath(raw, sp.totalPath)) || 0)
            : rows.reduce((a, r) => a + (r.cells[sp.key] ?? 0), 0),
        }))
        return { unit: c.unit, columns, rows, rowTotal: false, grandTotal: 0 }
      }
    } else {
    ds.toMatrix = (raw): MatrixData => {
      // 버킷마다 {항목: 수} 를 모아 열 목록을 먼저 정한다 — 열은 **전 구간 합계 내림차순**
      // 이라 자주 나오는 메시지가 왼쪽에 온다(조회 구간이 바뀌어도 읽는 순서가 안정적).
      const rowsRaw = asArray(raw, c.from).map(it => {
        const cells: Record<string, number> = {}
        for (const path of (c.cells ?? [])) {
          const m0 = getPath(it, path)
          if (!m0 || typeof m0 !== 'object') continue
          for (const [k, v] of Object.entries(m0 as Record<string, unknown>)) {
            cells[k] = (cells[k] ?? 0) + (Number(v) || 0)
          }
        }
        return { label: String(firstField(it, c.label) ?? ''), cells }
      })
      const totals: Record<string, number> = {}
      for (const r of rowsRaw) {
        for (const [k, v] of Object.entries(r.cells)) totals[k] = (totals[k] ?? 0) + v
      }
      let keys = Object.keys(totals).sort((a, b) => totals[b] - totals[a] || a.localeCompare(b))
      let folded: string[] = []
      const lim = c.limit ?? 0
      if (lim > 0 && keys.length > lim) {
        folded = keys.slice(lim)
        keys = keys.slice(0, lim)
      }
      const ETC = '기타'
      const columns: MatrixData['columns'] = keys.map(k => ({ key: k, label: k, total: totals[k] }))
      if (folded.length) {
        columns.push({ key: ETC, label: `${ETC}(${folded.length})`,
                       total: folded.reduce((a, k) => a + totals[k], 0) })
      }
      const rows = rowsRaw.map(r => {
        const cells: Record<string, number> = {}
        for (const k of keys) cells[k] = r.cells[k] ?? 0
        if (folded.length) cells[ETC] = folded.reduce((a, k) => a + (r.cells[k] ?? 0), 0)
        const total = Object.values(cells).reduce((a, v) => a + v, 0)
        return { label: r.label, cells, total }
      })
      return {
        unit: c.unit, columns, rows, rowTotal: true,
        grandTotal: rows.reduce((a, r) => a + r.total, 0),
      }
    }
    }
  }
  if (m.distribution) {
    const c = m.distribution as DistMap
    ds.toDistribution = (raw): DistributionData => {
      const parts = c.partsObject
        ? (getPath(raw, c.partsObject) as Record<string, Record<string, number>> | undefined)
        : undefined
      return {
        total: Number(getPath(raw, c.totalPath)) || 0,
        series: c.series?.map((sp, i) => ({ key: sp.key, label: sp.label, color: seriesColor(sp.color, i) })),
        items: c.fromObject
          ? objEntries(raw, c.fromObject).map(([k, v]) => ({ label: k, value: v, parts: parts?.[k] }))
          : asArray(raw, c.from || '').map(it => ({ label: String(firstField(it, c.label || [])), value: Number(it[c.value || '']) || 0 })),
      }
    }
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
