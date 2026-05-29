// 범용 shape 위젯 프레임워크 — 코어는 "데이터 성격(shape)"을, 서비스 pack 은 "데이터 소스"를 제공.
// 같은 shape(차트/표/KPI/분포)에 소스만 다른 출력을 위젯마다 만들지 않고, shape 위젯 1개 +
// 소스 선택(dropdown)으로 합성. 새 소스가 생겨도 위젯 추가 없이 소스만 등록하면 된다.

import type { ComponentType } from 'react'

export type ShapeKind = 'time-bar' | 'kpi' | 'distribution' | 'table'

export const SHAPE_LABELS: Record<ShapeKind, string> = {
  'time-bar': '시계열 차트', kpi: 'KPI 지표', distribution: '분포', table: '표',
}

// ── shape별 데이터 계약 (소스의 adapter 가 raw → 아래 형태로 변환) ──
export interface TimeBarData { unit?: string; buckets: { label: string | number; value: number }[] }
export interface KpiData { items: { label: string; value: string | number; unit?: string }[] }
export interface DistributionData { total: number; items: { label: string; value: number }[] }
export interface TableData { columns: [string, string]; rows: { key: string; value: string | number }[] }

export type ShapeData = TimeBarData | KpiData | DistributionData | TableData

// 소스 fetch 시 위젯이 넘기는 공통 파라미터 (date/granularity 컨트롤).
export interface SourceParams { date: string; granularity: string }

// 데이터 소스 — 단일 load(raw) + 지원 shape별 adapter. shapes 에 든 shape 위젯에서만 선택지로 노출.
export interface DataSource<Raw = unknown> {
  id: string                       // 'cims.msg.sip'
  label: string                    // 'SIP 메시지'
  serviceId?: string               // 'cims'
  shapes: ShapeKind[]              // 이 소스가 채울 수 있는 shape 들
  needsControls?: boolean          // date/granularity 툴바 필요 여부 (기본 true)
  load: (p: SourceParams) => Promise<Raw>
  toTimeBar?: (raw: Raw) => TimeBarData
  toKpi?: (raw: Raw) => KpiData
  toDistribution?: (raw: Raw) => DistributionData
  toTable?: (raw: Raw) => TableData
}

// shape → adapter 메서드명 매핑 (ShapeWidget 이 동적 호출).
export const SHAPE_ADAPTER: Record<ShapeKind, keyof DataSource> = {
  'time-bar': 'toTimeBar', kpi: 'toKpi', distribution: 'toDistribution', table: 'toTable',
}

export interface ShapeRendererProps<D extends ShapeData = ShapeData> { data: D }
export type ShapeRenderer = ComponentType<ShapeRendererProps>
