// 범용 shape 위젯 — 배치가 지정한 데이터 소스를 fetch → 렌더.
// 소스는 배치 설정(config.source, 편집기 [⚙])이 정본이다. 단 같은 페이지에 `core.source-picker`
// 가 있으면 그 페이지 값(`src`)을 따른다 — 한 화면의 차트·표가 같은 대상을 함께 보게(메시지 통계).
// 기간·단위도 같은 규칙: `core.page-filter` 가 있으면 그쪽을 따르고(자기 컨트롤 접음), 없으면
// 자기 컨트롤을 쓴다 — 한 페이지에 날짜 선택기가 여러 개 생기지 않게 하는 규칙(pageParams).
import { useState, useEffect, useMemo, useCallback } from 'react'
import type { WidgetDef, WidgetProps } from '../types'
import type { ShapeData, KpiData, SeriesBarData, DistributionData, SourceParams } from './types'
import { SHAPE_LABELS, SHAPE_ADAPTER } from './types'
import { catalogSources, sourcesForShape, useDataSourceCatalog, loadSource } from './sourceRegistry'
import { GRAN_LABELS, defaultRange, granFits, useHasPageControl, usePageParam, usePageControl } from '../pageParams'
import { TimeBarChart, SeriesBarChart, StatValue, DistributionBars, KvTable, MatrixTable } from './renderers'

const RENDERERS = {
  'time-bar': TimeBarChart, 'series-bar': SeriesBarChart,
  stat: StatValue, distribution: DistributionBars, table: KvTable, matrix: MatrixTable,
} as const
type WidgetShape = keyof typeof RENDERERS

// 지표 인덱스 — stat 은 소스의 kpi 계약에서 몇 번째 지표를 그릴지 config.item(0-based)으로 받는다.
// 인덱스인 이유: 표시 이름은 데이터에서 오므로 항목 이름이 바뀌어도 화면은 항상 맞는다.
function itemIndex(config?: Record<string, unknown>): number {
  const n = Number(config?.item ?? 0)
  return Number.isFinite(n) && n >= 0 ? Math.floor(n) : 0
}

function ShapeWidgetBody({ shape, config }: { shape: WidgetShape; config?: Record<string, unknown> }) {
  // 소스 카탈로그 = Service Descriptor data_sources (백엔드 데이터 구동). 비동기 로드.
  const { sources: catalog, loading: catLoading, error: catError } = useDataSourceCatalog()
  const sources = useMemo(() => sourcesForShape(shape, catalog), [shape, catalog])
  // 소스: 페이지에 대상 선택 컨트롤이 있으면 그 값이 우선(없으면 배치 설정).
  const srcControlled = useHasPageControl('source')
  const [busSrc] = usePageParam('src')
  const placed = typeof config?.source === 'string' ? config.source as string : ''
  const wanted = srcControlled && busSrc ? busSrc : placed
  const [sourceId, setSourceId] = useState(wanted)
  // 구간·단위: 페이지 컨트롤이 있으면 버스 값, 없으면 자기 값. 훅은 항상 둘 다 호출(조건부 호출 금지).
  const controlled = useHasPageControl('period')
  const [busFrom] = usePageParam('from')
  const [busTo] = usePageParam('to')
  const [busGran] = usePageParam('gran')
  const [ownRange, setOwnRange] = useState(() => defaultRange())
  const [ownGran, setOwnGran] = useState('1h')
  const from = controlled ? busFrom : ownRange.from
  const to = controlled ? busTo : ownRange.to
  const gran = controlled ? busGran : ownGran
  const date = from.slice(0, 10)      // 구간을 안 쓰는 옛 소스용 축약(그 날 하루)
  // 계열 선택: 페이지에 선택 타일이 있으면 그 값만 그린다(비면 전 계열). 다시 fetch 하지 않는다 —
  // 계열은 이미 받은 응답 안에 다 들어 있어 켜고 끄는 건 표시 문제다.
  const seriesControlled = useHasPageControl('series')
  const [busSeries] = usePageParam('series')
  const [data, setData] = useState<ShapeData | null>(null)
  const [loading, setLoading] = useState(false)
  const [err, setErr] = useState('')

  // 카탈로그 도착 / 대상 컨트롤 전환에 따라 소스 확정 (wanted 가 유효하면 그것, 아니면 첫 소스).
  // wanted 가 바뀌면(페이지 컨트롤 클릭) 지금 소스가 유효해도 따라가야 한다.
  useEffect(() => {
    if (sources.length === 0) return
    const valid = sources.some(s => s.id === wanted)
    if (valid) { if (sourceId !== wanted) setSourceId(wanted); return }
    if (!sources.some(s => s.id === sourceId)) setSourceId(sources[0].id)
  }, [sources, sourceId, wanted])

  const src = sources.find(s => s.id === sourceId)
  const Renderer = RENDERERS[shape]

  const load = useCallback(async () => {
    if (!src) return
    setLoading(true); setErr('')
    try {
      const raw = await loadSource(src, { date, granularity: gran, from, to } as SourceParams)
      const adapt = src[SHAPE_ADAPTER[shape]] as ((r: unknown) => ShapeData) | undefined
      const d = adapt ? adapt(raw) : null
      // stat: 지표 묶음(KpiData)에서 지정한 하나만 남긴다 — 카드 하나에 값 하나.
      setData(d && shape === 'stat'
        ? { items: [(d as KpiData).items[itemIndex(config)]].filter(Boolean) } as ShapeData
        : d)
    } catch (e) { setErr((e as Error).message); setData(null) }
    finally { setLoading(false) }
  }, [src, shape, from, to, gran, date, config])

  useEffect(() => { void load() }, [load])

  // 계열 선택은 시계열(series-bar)과 분포(계열 분해가 있는 경우)에 **같이** 걸린다 — 한 화면의
  // 두 그림이 다른 대상을 보면 안 된다. 분포는 값·합계까지 고른 계열 기준으로 다시 센다.
  const shown = useMemo(() => {
    if (!data) return data
    const sel = (seriesControlled ? busSeries : '').split(',').map(x => x.trim()).filter(Boolean)
    if (sel.length === 0) return data
    if (shape === 'series-bar') {
      const d = data as SeriesBarData
      return { ...d, series: d.series.filter(sp => sel.includes(sp.key)) } as ShapeData
    }
    if (shape === 'distribution') {
      const d = data as DistributionData
      if (!d.series?.length) return data
      const items = d.items.map(it => ({
        ...it,
        value: sel.reduce((a, k) => a + (it.parts?.[k] || 0), 0),
      }))
      return {
        series: d.series.filter(sp => sel.includes(sp.key)),
        items: items.filter(it => it.value > 0),
        total: items.reduce((a, it) => a + it.value, 0),
      } as ShapeData
    }
    return data
  }, [data, shape, seriesControlled, busSeries])

  const body = catError ? <div style={{ color: 'var(--destructive)', fontSize: 13 }}>※ 소스 카탈로그: {catError}</div>
    : catLoading && sources.length === 0 ? <div className="empty">소스 카탈로그 로딩 중...</div>
    : !src ? <div className="empty">소스를 선택하세요</div>
    : loading && !data ? <div className="empty">로딩 중...</div>
    : err ? <div style={{ color: 'var(--destructive)', fontSize: 13 }}>※ {err}</div>
    : shown ? <Renderer data={shown as never} />
    : <div className="empty">데이터 없음</div>

  // 지표 카드는 chrome 최소화 — 값만. 소스·지표는 편집 모드 [⚙] 에서 정한다.
  if (shape === 'stat') {
    return <div className="panel" style={{ padding: 10, display: 'flex', flexDirection: 'column' }}>{body}</div>
  }

  return (
    <div className="panel" style={{ padding: 12, display: 'flex', flexDirection: 'column', minHeight: 0 }}>
      <div style={{ flex: 'none', display: 'flex', alignItems: 'center', gap: 8, marginBottom: 10, flexWrap: 'wrap' }}>
        {/* 제목 — 배치에서 지정(config.title)한 이름이 우선. 소스가 고정된 화면에서는 소스명보다
            "무엇을 그리는가"(호 시도 추이, 종료 사유 분포)가 읽기 쉽다. */}
        <span style={{ fontWeight: 600, fontSize: 13 }}>
          {typeof config?.title === 'string' && config.title ? config.title : SHAPE_LABELS[shape]}
        </span>
        {src?.needsControls !== false && (controlled ? (
          // 페이지 컨트롤이 조건을 소유 — 값만 표기(중복 컨트롤 제거).
          <span style={{ fontSize: 11, color: 'var(--muted-foreground)' }}>
            {from} ~ {to} · {GRAN_LABELS[gran] ?? gran}
          </span>
        ) : (
          // 페이지 컨트롤이 없는 배치 — 자기 구간 컨트롤을 쓴다(감당 못 할 단위는 비활성).
          <>
            <input className="form-input" type="datetime-local" value={from.replace(' ', 'T').slice(0, 16)}
                   onChange={e => setOwnRange(r => ({ ...r, from: e.target.value.replace('T', ' ') }))}
                   style={{ width: 176, fontSize: 12 }} />
            <span style={{ color: 'var(--muted-foreground)' }}>~</span>
            <input className="form-input" type="datetime-local" value={to.replace(' ', 'T').slice(0, 16)}
                   onChange={e => setOwnRange(r => ({ ...r, to: e.target.value.replace('T', ' ') }))}
                   style={{ width: 176, fontSize: 12 }} />
            {Object.entries(GRAN_LABELS).map(([g, lb]) => (
              <button key={g} className={`btn btn--sm ${gran === g ? 'btn--primary' : 'btn--ghost'}`}
                      disabled={!granFits(from, to, g)}
                      onClick={() => setOwnGran(g)}>{lb}</button>
            ))}
          </>
        ))}
        <button className="btn btn--sm btn--outline" style={{ marginLeft: 'auto' }} onClick={() => void load()}>↻</button>
      </div>
      {/* 본문은 남은 높이를 전부 받는다 — 차트는 그 높이를 채우고(비율 렌더), 표는 넘치면 스크롤. */}
      <div className="scroll-fill">{body}</div>
    </div>
  )
}

// 계열 선택 타일 — 계열마다 카드 하나(색 + 이름 + 구간 합계). 클릭하면 그 계열만/함께 본다.
//
// 왜 카드 하나짜리 위젯(shape.stat)으로 쪼개지 않는가: 이 카드들은 **같은 축의 분포**이자 동시에
// 차트의 **선택 컨트롤**이다(console_platform §3.1 — 심각도 타일이 `sev` 를 소유하는 것과 같다).
// 낱개로 쪼개면 "무엇을 고를 수 있는가"가 배치에 흩어지고 선택 상태를 나눠 가질 수 없다.
//
// 합계는 받은 버킷을 더해 낸다 — 차트와 **같은 응답·같은 구간**이라 숫자와 그림이 어긋나지 않는다.
function SeriesSelectBody({ config }: { config?: Record<string, unknown> }) {
  usePageControl('series')
  const [sel, setSel] = usePageParam('series')
  const { sources: catalog, loading: catLoading } = useDataSourceCatalog()
  const sources = useMemo(() => sourcesForShape('series-bar', catalog), [catalog])
  const srcControlled = useHasPageControl('source')
  const [busSrc] = usePageParam('src')
  const placed = typeof config?.source === 'string' ? config.source as string : ''
  const wanted = srcControlled && busSrc ? busSrc : placed
  const src = sources.find(s => s.id === wanted) ?? sources[0]

  const controlled = useHasPageControl('period')
  const [busFrom] = usePageParam('from')
  const [busTo] = usePageParam('to')
  const [busGran] = usePageParam('gran')
  const [ownRange] = useState(() => defaultRange())
  const from = controlled ? busFrom : ownRange.from
  const to = controlled ? busTo : ownRange.to
  const gran = controlled ? busGran : '1h'
  const [data, setData] = useState<SeriesBarData | null>(null)
  const [err, setErr] = useState('')

  useEffect(() => {
    let dead = false
    if (!src?.toSeriesBar) return
    void (async () => {
      try {
        const raw = await loadSource(src, { date: from.slice(0, 10), granularity: gran, from, to } as SourceParams)
        if (!dead) { setData(src.toSeriesBar!(raw)); setErr('') }
      } catch (e) { if (!dead) { setErr((e as Error).message); setData(null) } }
    })()
    return () => { dead = true }
  }, [src, from, to, gran])

  const active = sel.split(',').map(x => x.trim()).filter(Boolean)

  if (err) return <div style={{ color: 'var(--destructive)', fontSize: 13 }}>※ {err}</div>
  if (!data) return <div className="empty">{catLoading ? '소스 카탈로그 로딩 중...' : '로딩 중...'}</div>

  const totals = (k: string) => data.buckets.reduce((a, b) => a + (b.values[k] || 0), 0)
  // 구간 내내 0 인 계열은 숨긴다 — '미분류'처럼 정상일 때 비어 있는 계열이 자리만 차지하지 않게.
  // 전부 0 이면(데이터 없음) 구조를 보여주기 위해 그대로 다 낸다.
  const all = data.series
  const shownSeries = all.some(sp => totals(sp.key) > 0) ? all.filter(sp => totals(sp.key) > 0) : all
  const isOn = (k: string) => active.length === 0 || active.includes(k)
  const toggle = (k: string) => {
    const cur = active.length === 0 ? shownSeries.map(sp => sp.key) : active
    const next = cur.includes(k) ? cur.filter(x => x !== k) : [...cur, k]
    // 전부 끄면 빈 차트가 되니 전체로 되돌린다 — 막다른 상태를 만들지 않는다.
    setSel(next.length === 0 || next.length === shownSeries.length ? '' : next.join(','))
  }
  const allOn = active.length === 0 || active.length === shownSeries.length
  const grand = shownSeries.reduce((a, sp) => a + totals(sp.key), 0)
  const allLabel = typeof config?.allLabel === 'string' && config.allLabel ? config.allLabel : '전체'

  // 계열이 하나면 쪼갤 축이 없다 — 고를 것도 없으므로 합계 타일만 낸다(같은 값 카드 두 장 방지).
  const single = shownSeries.length <= 1

  return (
    <div style={{ display: 'flex', gap: 10, flexWrap: 'wrap' }}>
      {/* '전체' 타일 — 계열이 아니라 **전부 선택** 버튼이다. 계열은 서로 겹치지 않으므로
          전부 켠 막대가 곧 전체이고, 그래서 전체를 따로 쌓을 계열로 두지 않는다. */}
      <button type="button" onClick={() => setSel('')} title={`${allLabel} — 모든 계열 표시`}
              style={{
                flex: '1 1 130px', textAlign: 'left', cursor: 'pointer', font: 'inherit',
                background: 'var(--card)', padding: '12px 14px', borderRadius: 'var(--radius)',
                border: `1px solid ${allOn ? 'var(--primary)' : 'var(--border)'}`,
                borderLeft: `4px solid ${allOn ? 'var(--primary)' : 'var(--border)'}`,
                opacity: allOn ? 1 : 0.5,
              }}>
        <div style={{ fontSize: 12, color: 'var(--muted-foreground)' }}>{allLabel}</div>
        <div style={{ fontSize: 22, fontWeight: 700, color: 'var(--foreground)', marginTop: 3 }}>
          {grand}<span style={{ fontSize: 12, color: 'var(--muted-foreground)', marginLeft: 2 }}>건</span>
        </div>
      </button>
      {!single && shownSeries.map(sp => {
        const on = isOn(sp.key)
        return (
          <button key={sp.key} type="button" onClick={() => toggle(sp.key)}
                  title={on ? `${sp.label} 숨기기` : `${sp.label} 표시`}
                  style={{
                    flex: '1 1 130px', textAlign: 'left', cursor: 'pointer', font: 'inherit',
                    background: 'var(--card)', padding: '12px 14px', borderRadius: 'var(--radius)',
                    border: `1px solid ${on ? sp.color : 'var(--border)'}`,
                    borderLeft: `4px solid ${on ? sp.color : 'var(--border)'}`,
                    opacity: on ? 1 : 0.5,
                  }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 12, color: 'var(--muted-foreground)' }}>
              <span style={{ width: 9, height: 9, borderRadius: 2, background: on ? sp.color : 'var(--border)' }} />
              {sp.label}
            </div>
            <div style={{ fontSize: 22, fontWeight: 700, color: 'var(--foreground)', marginTop: 3 }}>
              {totals(sp.key)}<span style={{ fontSize: 12, color: 'var(--muted-foreground)', marginLeft: 2 }}>건</span>
            </div>
          </button>
        )
      })}
    </div>
  )
}

export const seriesSelectWidget: WidgetDef = {
  id: 'core.series-select',
  title: '계열 선택 (지표 카드 = 차트 필터)',
  category: 'control',
  usesPageParams: true,
  apiSources: cfg => (typeof cfg?.source === 'string' && cfg.source ? [cfg.source] : []),
  configFields: [
    { key: 'source', label: '데이터 소스', type: 'select' as const,
      options: () => sourcesForShape('series-bar', catalogSources()).map(s => ({ value: s.id, label: s.label })) },
    { key: 'allLabel', label: '전체 타일 이름', type: 'text' as const, placeholder: '전체' },
  ],
  defaultSize: { w: 48, h: 8 },
  component: (p: WidgetProps) => <SeriesSelectBody config={p.config} />,
}

// shape → WidgetDef 팩토리. 지표 묶음 위젯은 없다 — 지표는 stat 하나씩 놓는다.
function makeShapeWidget(shape: WidgetShape, id: string, title: string): WidgetDef {
  const sourceField = {
    key: 'source', label: '데이터 소스', type: 'select' as const,
    // 소스 목록은 런타임 카탈로그(descriptor) — 함수 옵션으로 편집기 [⚙] 에 그때그때 채운다.
    options: () => sourcesForShape(shape, catalogSources()).map(s => ({ value: s.id, label: s.label })),
  }
  const itemField = {
    key: 'item', label: '지표', type: 'select' as const,
    // 앞에서 고른 소스가 선언한 지표 목록. 값은 인덱스.
    options: (cfg?: Record<string, unknown>) => {
      const all = sourcesForShape('stat', catalogSources())
      const src = all.find(s => s.id === cfg?.source) ?? all[0]
      return (src?.kpiItems ?? []).map((label, i) => ({ value: String(i), label }))
    },
  }
  const titleField = {
    key: 'title', label: '제목', type: 'text' as const, placeholder: SHAPE_LABELS[shape],
  }
  return {
    id, title,
    category: shape === 'stat' ? 'metric' : 'view',
    defaultSize: shape === 'stat' ? { w: 3, h: 7 } : { w: 6 },
    usesPageParams: true,
    // 호출 API 는 고른 소스에 따라 갈린다 — API id 를 정적으로 못 적으므로 소스 id 를 넘기고
    // 경로 환산·문서 대조는 배지가 한다. (같은 계열 소스는 path 파라미터만 다른 한 API 로 모인다)
    apiSources: cfg => {
      const id = typeof cfg?.source === 'string' ? cfg.source : ''
      return id ? [id] : []
    },
    configFields: shape === 'stat' ? [sourceField, itemField] : [sourceField, titleField],
    component: (p: WidgetProps) => <ShapeWidgetBody shape={shape} config={p.config} />,
  }
}

export const timeBarWidget      = makeShapeWidget('time-bar', 'shape.time-bar', '시계열 차트')
export const seriesBarWidget    = makeShapeWidget('series-bar', 'shape.series-bar', '시계열 차트 (계열 비교)')
export const statWidget         = makeShapeWidget('stat', 'shape.stat', '지표 (소스·항목 선택)')
export const distributionWidget = makeShapeWidget('distribution', 'shape.distribution', '분포')
export const tableShapeWidget   = makeShapeWidget('table', 'shape.table', '표')
export const matrixWidget       = makeShapeWidget('matrix', 'shape.matrix', '교차표 (시간 × 항목)')

export const SHAPE_WIDGETS: WidgetDef[] = [
  statWidget, timeBarWidget, seriesBarWidget, distributionWidget, tableShapeWidget,
  matrixWidget, seriesSelectWidget,
]
