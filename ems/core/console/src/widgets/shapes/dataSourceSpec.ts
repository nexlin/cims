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
  columns?: {
    key: string; label: string; path: string; totalPath?: string; unit?: string
    /** 열 이름만으로 뜻이 안 서는 칸의 설명(헤더 툴팁) — `사유 모름`·`보존초과` 처럼
     *  숫자를 어떻게 읽어야 하는지가 이름에 다 담기지 않는 열에 쓴다. */
    help?: string
    /** 묶음 키 — 비율 하나와 그 비율을 설명하는 건수들이 한 세트다(§2.3 열 순서).
     *  열이 16개를 넘어 가로로 길어지면 어디까지가 한 벌인지가 안 보인다. */
    group?: string
    /**
     * 이 열의 숫자가 **왜** 나왔는지를 담은 버킷 안 {원인: 수} map 경로.
     * detailLabels 에 있는 키만 읽는다 — 원인 축 하나(`ptt.causes`)를 여러 열이 나눠 갖기
     * 때문이다(거부 열은 비멤버·권한·시간창, 오류 열은 코덱·SRTP·leg 확립 실패).
     */
    detailFrom?: string
    /** 원인 슬러그 → 표시 이름. **이 열이 품는 원인만** 적는다(그게 곧 필터다). */
    detailLabels?: Record<string, string>
    /**
     * 원인으로 못 채운 건수를 메우는 **차선 축**(보통 응답코드). 원인 기록이 시작되기 전의
     * 옛 자료는 원인이 비어 있는데, 그 줄에도 응답코드는 있다 — 코드는 원인보다 거칠지만
     * (같은 488 이 둘) 아무것도 안 보여 주는 것보다 낫다. 여기도 **이 열이 품는 코드만** 적는다.
     */
    statusFrom?: string
    statusLabels?: Record<string, string>
    /** 원인도 코드도 없는 잔여 건수에 붙일 이름. 없으면 잔여를 적지 않는다. */
    detailUnknownLabel?: string
    /**
     * 값 **0 을 눈에 보이게 칠한다** — 높을수록 좋은 비율에서 0 은 "전부 실패" 인데, 기본
     * 칸 색칠은 값에 비례해서 0 을 가장 흐리게 그린다(가장 나쁜 값이 가장 안 보인다).
     * 분모가 0 인 구간은 서버가 `null` 로 내리므로(`—`) 여기 0 은 "호가 있었는데 하나도
     * 안 됐다" 만 뜻한다.
     *
     * 색은 다른 칸과 같은 계열이다 — **색으로 성격을 나누지 않는다**. 왜 0 인지는 실패 사유
     * 칸(거부·오류·무응답)과 그 툴팁이 말한다.
     */
    paintZero?: boolean
  }[]
  /** 표 아래 각주 — 비율 열은 이름만으로 분자·분모를 알 수 없어 소스가 계산식을 함께 준다. */
  notes?: string[]
  /**
   * 값 없는 구간을 접었을 때 그 줄에 적을 말 — **무엇이 없었는지는 소스만 안다**.
   * 호 통계는 "호가 없는 시간", 메시지 교차표는 "메시지가 없는 시간" 이다.
   * 없으면 "자료가 없는 시간".
   */
  blankLabel?: string
  /** 동적 열의 표시 이름 — 열 키가 코드(488·503)일 때 뜻을 붙인다. 없는 키는 키를 그대로 쓴다. */
  cellLabels?: Record<string, string>
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

/**
  * 서버가 `null` 로 내려보낸 값은 **집계 불가**다 — "분모가 원천에 없어 비율을 낼 수 없다"
  * (sip_statistics.md §2.1 `rate_gap`). `Number(null) === 0` 이고 0 은 유한하므로, 그대로
  * 흘리면 **0% 로 보인다** — 실측(2026-09-16): 성공률·완료율이 전 단위에서 0% 로 나왔다.
  * 거짓 0 대신 빈 자리를 낸다. 0 은 "정말 0 건" 일 때만 쓴다.
  */
export const NO_VALUE = '—'

function applyFormat(v: unknown, format?: string): string | number {
  if (v === null || v === undefined) return NO_VALUE
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
        // 칸의 숫자를 **끝까지 설명한다** — 원인으로 채우고, 남으면 응답코드로, 그래도
        // 남으면 "미기록" 으로. 열의 값과 상세의 합이 어긋난 채로 두면 읽는 사람이
        // "나머지는 뭐냐" 를 물을 수밖에 없다. 칸이 0 이면 상세도 없다.
        const pickDetail = (sp: typeof specs[number], src: unknown,
                            v: number | null): [string, Record<string, number>] => {
          const total = typeof v === 'number' ? v : 0
          if (total <= 0) return ['', {}]
          const got: Record<string, number> = {}
          // 선언된 키만, 남은 예산만큼 가져온다(축이 열끼리 겹쳐도 넘치지 않게).
          const take = (path?: string, labels?: Record<string, string>, budget = Infinity) => {
            if (!path || !labels) return 0
            const m0 = getPath(src as never, path)
            if (!m0 || typeof m0 !== 'object') return 0
            let used = 0
            for (const [k, lbl] of Object.entries(labels)) {
              const n = Math.min(Number((m0 as Record<string, unknown>)[k]) || 0, budget - used)
              if (n > 0) { got[lbl] = (got[lbl] ?? 0) + n; used += n }
            }
            return used
          }
          let rest = total - take(sp.detailFrom, sp.detailLabels, total)
          if (rest > 0) rest -= take(sp.statusFrom, sp.statusLabels, rest)
          if (rest > 0 && sp.detailUnknownLabel) got[sp.detailUnknownLabel] = rest
          const txt = Object.entries(got).sort((a, b) => b[1] - a[1])
            .map(([lbl, n]) => `${lbl} ${n}건`).join(' · ')
          return [txt, got]
        }
        // **null · 없음 · 0 을 가른다.**
        //   null      = 서버가 일부러 비운 값(집계 불가) → 빈 자리
        //   undefined = 그 버킷에 그 축이 없다(그 기간에 통화가 없었다)
        //                 · 건수 열 → **0**. 0 이 사실이다("한 건도 없었다")
        //                 · 비율 열 → **빈 자리**. 통화가 없던 날의 "성공률 0%" 는 거짓
        //                   경보다 — 다 실패한 게 아니라 셀 것이 없었던 것이다
        //   그 외      = 그대로 숫자. 시도가 있는데 성립이 0 이면 성공률 0% 가 사실이다.
        const numOrNull = (v: unknown, isRate = false): number | null =>
          v === null ? null
            : (v === undefined ? (isRate ? null : 0) : (Number(v) || 0))
        // 합계 칸 전용 — **undefined 를 0 으로 읽지 않는다.** 버킷과 달리 합계는 서버가
        //   읽은 구간이면 0 으로 채워 보내므로(ensure_svc), 없는 것은 곧 모르는 것이다.
        const numOrNullStrict = (v: unknown): number | null =>
          (v === null || v === undefined) ? null : (Number(v) || 0)
        // 합계 행의 상세도 **같은 규칙**으로 만든다. 행 상세를 그냥 더하면 합계 숫자와
        //   어긋날 수 있다(합계는 `totalPath` 에서 오고 행 합은 버킷들의 합이다) — 그러면
        //   툴팁이 칸의 숫자를 설명하지 못한다. 그래서 원인·코드 축을 **날것으로** 모아 두고,
        //   합계 숫자를 예산 삼아 다시 접는다.
        const rawSums: Record<string, Record<string, Record<string, number>>> = {}
        const addRaw = (colKey: string, path: string | undefined, src: unknown) => {
          if (!path) return
          const m0 = getPath(src as never, path)
          if (!m0 || typeof m0 !== 'object') return
          const bag = (rawSums[colKey] = rawSums[colKey] ?? {})
          const at = (bag[path] = bag[path] ?? {})
          for (const [k, v] of Object.entries(m0 as Record<string, unknown>)) {
            at[k] = (at[k] ?? 0) + (Number(v) || 0)
          }
        }
        // 모아 둔 날것을 경로 그대로 되살려 pickDetail 에 넘긴다(같은 코드가 돌게).
        const rawSrc = (colKey: string) => {
          const out: Record<string, unknown> = {}
          for (const [path, map] of Object.entries(rawSums[colKey] ?? {})) {
            const seg = path.split('.')
            let cur = out
            for (let i = 0; i < seg.length - 1; i++) {
              cur[seg[i]] = cur[seg[i]] ?? {}
              cur = cur[seg[i]] as Record<string, unknown>
            }
            cur[seg[seg.length - 1]] = map
          }
          return out
        }
        const rows = asArray(raw, c.from).map(it => {
          const cells: Record<string, number | null> = {}
          const details: Record<string, string> = {}
          // **자료가 없는 날의 행은 통째로 빈칸이다.** 서버가 `missing` 을 세운 행은 그 날
          //   집계도 원본도 없어 아무것도 모르는 구간이다(sip_statistics.md §7.2). 없는 축을
          //   0 으로 읽는 기본 규칙(numOrNull)을 그대로 적용하면 **철거·보존기간 경과로
          //   자료가 사라진 날이 "통화 0 건" 으로 보인다** — 운영자는 그것을 트래픽 감소로
          //   읽는다(실측 2026-09-17: 9/1~9/3 행이 9/17 행과 똑같이 0 이었다).
          const rowMissing = (it as Record<string, unknown>).missing === true
          for (const sp of specs) {
            cells[sp.key] = rowMissing ? null : numOrNull(getPath(it, sp.path), sp.unit === '%')
            const [txt] = pickDetail(sp, it, cells[sp.key])
            if (txt) details[sp.key] = txt
            addRaw(sp.key, sp.detailFrom, it)
            addRaw(sp.key, sp.statusFrom, it)
          }
          return { label: String(firstField(it, c.label) ?? ''), cells, total: 0, details }
        })
        const columns = specs.map(sp => {
          // 합계 칸은 `totalPath`(응답의 totals) 에서 온다. 서버는 **구간을 온전히 읽었을
          //   때만** 요청한 서비스 칸을 0 으로 채우므로(§2.1a-1), 칸이 아예 없다는 것은
          //   "못 읽었다" 는 뜻이다 — 0 이 아니라 빈칸으로 낸다.
          const total = sp.totalPath !== undefined
            ? numOrNullStrict(getPath(raw, sp.totalPath))
            : rows.reduce((a, r) => a + (r.cells[sp.key] ?? 0), 0)
          return {
            key: sp.key, label: sp.label, unit: sp.unit, total, help: sp.help, group: sp.group,
            paintZero: sp.paintZero === true,
            detail: pickDetail(sp, rawSrc(sp.key), total)[0] || undefined,
          }
        })
        return { unit: c.unit, columns, rows, rowTotal: false, grandTotal: 0, notes: c.notes,
                 blankLabel: c.blankLabel }
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
      const columns: MatrixData['columns'] = keys.map(k => ({
        key: k, label: c.cellLabels?.[k] ?? k, total: totals[k],
      }))
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
        grandTotal: rows.reduce((a, r) => a + r.total, 0), notes: c.notes,
        blankLabel: c.blankLabel,
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
