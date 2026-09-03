// 범용 shape 위젯 프레임워크 — 코어는 "데이터 성격(shape)"을, 서비스 pack 은 "데이터 소스"를 제공.
// 같은 shape(차트/표/KPI/분포)에 소스만 다른 출력을 위젯마다 만들지 않고, shape 위젯 1개 +
// 소스 선택(dropdown)으로 합성. 새 소스가 생겨도 위젯 추가 없이 소스만 등록하면 된다.

import type { ComponentType } from 'react'

// 'kpi' 는 **데이터 계약**(descriptor 가 선언하는 지표 목록)이고, 화면에 놓는 것은 그중 하나를
// 그리는 'stat'(지표 카드)이다 — 통계 화면의 지표는 카드 하나에 값 하나로 둔다.
// 'series-bar' 는 계열이 여럿인 시계열 — time-bar 와 달리 한 버킷이 값 하나가 아니라 계열별 값을
// 갖는다. 계열을 켜고 끄는 선택은 페이지 파라미터 `series` 가 소유한다(core.series-select).
export type ShapeKind = 'time-bar' | 'series-bar' | 'kpi' | 'stat' | 'distribution' | 'table' | 'matrix'

export const SHAPE_LABELS: Record<ShapeKind, string> = {
  'time-bar': '시계열 차트', 'series-bar': '시계열 차트 (계열 비교)',
  kpi: '지표 묶음(계약)', stat: '지표', distribution: '분포', table: '표',
  matrix: '교차표 (시간 × 항목)',
}

// ── shape별 데이터 계약 (소스의 adapter 가 raw → 아래 형태로 변환) ──
export interface TimeBarData { unit?: string; buckets: { label: string | number; value: number }[] }
/**
 * 교차표 — 행=시간 버킷, 열=항목(SIP 메서드 등), 칸=건수.
 * 한 축만 있는 time-bar·distribution 으로는 "시간대별 × 메서드별" 을 표현할 수 없다.
 * columns 는 전 구간 합계 내림차순 — 자주 나오는 것이 왼쪽에 온다.
 */
export interface MatrixData {
  unit?: string
  /** total = 합계 행에 쓸 값. 비율 열은 합산이 무의미하므로 소스가 집계값을 따로 준다. */
  columns: { key: string; label: string; total: number; unit?: string }[]
  rows: { label: string; cells: Record<string, number>; total: number }[]
  /** 행 합계 열을 낼지. 열이 **같은 축**일 때만 의미가 있다(메서드별 건수 O, 시도+성립+비율 X). */
  rowTotal: boolean
  grandTotal: number
}
// 계열 시계열 — series 는 **선언 순서**가 색 순서(--chart-1..5)이자 쌓는 순서(아래→위)다.
// includes = 이 계열이 **품고 있는** 다른 계열들(예: '전체'는 volte/ptt 를 포함). 쌓기는 "부분의
// 합"이라 포함관계인 둘을 같이 켜면 막대가 중복으로 커진다 — 렌더러가 그때만 알려준다.
export interface SeriesSpec { key: string; label: string; color: string; includes?: string[] }
export interface SeriesBarData {
  unit?: string
  series: SeriesSpec[]
  buckets: { label: string | number; values: Record<string, number> }[]
}
export interface KpiData { items: { label: string; value: string | number; unit?: string }[] }
// 분포 — parts 가 있으면 항목 하나를 계열별 조각으로 쪼개 색으로 나눠 그린다(parts 합 = value).
export interface DistributionData {
  total: number
  series?: SeriesSpec[]
  items: { label: string; value: number; parts?: Record<string, number> }[]
}
export interface TableData { columns: [string, string]; rows: { key: string; value: string | number }[] }

export type ShapeData = TimeBarData | SeriesBarData | KpiData | DistributionData | TableData | MatrixData

// 소스 fetch 시 위젯이 넘기는 공통 파라미터 (date/granularity 컨트롤).
// 조회 조건 — 구간(from~to)이 정본이고 `date` 는 옛 소스를 위한 축약형(그 날 하루).
// descriptor 의 `query` 에 적힌 키만 실제 URL 로 나간다(dataSourceSpec.buildDataSource).
export interface SourceParams { date: string; granularity: string; from: string; to: string }

// 데이터 소스 — 단일 load(raw) + 지원 shape별 adapter. shapes 에 든 shape 위젯에서만 선택지로 노출.
export interface DataSource<Raw = unknown> {
  id: string                       // 'cims.msg.sip'
  label: string                    // 'SIP 메시지'
  serviceId?: string               // 'cims'
  shapes: ShapeKind[]              // 이 소스가 채울 수 있는 shape 들
  endpoint: string                 // '/stats/messages/sip' — [API] 배지가 이 경로로 API 문서를 찾는다
  kpiItems?: string[]              // kpi 계약의 지표 라벨(선언 순서) — stat 위젯의 지표 선택지
  needsControls?: boolean          // date/granularity 툴바 필요 여부 (기본 true)
  load: (p: SourceParams) => Promise<Raw>
  toTimeBar?: (raw: Raw) => TimeBarData
  toSeriesBar?: (raw: Raw) => SeriesBarData
  toKpi?: (raw: Raw) => KpiData
  toDistribution?: (raw: Raw) => DistributionData
  toTable?: (raw: Raw) => TableData
  toMatrix?: (raw: Raw) => MatrixData
}

// shape → adapter 메서드명 매핑 (ShapeWidget 이 동적 호출).
// stat 은 kpi 계약을 그대로 쓴다(같은 응답에서 지표 하나를 골라 그림).
export const SHAPE_ADAPTER: Record<ShapeKind, keyof DataSource> = {
  'time-bar': 'toTimeBar', 'series-bar': 'toSeriesBar',
  kpi: 'toKpi', stat: 'toKpi', distribution: 'toDistribution', table: 'toTable',
  matrix: 'toMatrix',
}

export interface ShapeRendererProps<D extends ShapeData = ShapeData> { data: D }
export type ShapeRenderer = ComponentType<ShapeRendererProps>
