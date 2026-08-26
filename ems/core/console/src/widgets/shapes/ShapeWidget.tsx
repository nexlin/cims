// 범용 shape 위젯 — 배치가 지정한 데이터 소스를 fetch → 렌더.
// 소스는 배치 설정(config.source, 편집기 [⚙])이 정본이다. 단 같은 페이지에 `core.source-picker`
// 가 있으면 그 페이지 값(`src`)을 따른다 — 한 화면의 차트·표가 같은 대상을 함께 보게(메시지 통계).
// 기간·단위도 같은 규칙: `core.page-filter` 가 있으면 그쪽을 따르고(자기 컨트롤 접음), 없으면
// 자기 컨트롤을 쓴다 — 한 페이지에 날짜 선택기가 여러 개 생기지 않게 하는 규칙(pageParams).
import { useState, useEffect, useMemo, useCallback } from 'react'
import type { WidgetDef, WidgetProps } from '../types'
import type { ShapeData, KpiData, SourceParams } from './types'
import { SHAPE_LABELS, SHAPE_ADAPTER } from './types'
import { catalogSources, sourcesForShape, useDataSourceCatalog, loadSource } from './sourceRegistry'
import { GRAN_LABELS, todayIso, useHasPageControl, usePageParam } from '../pageParams'
import { TimeBarChart, StatValue, DistributionBars, KvTable } from './renderers'

const RENDERERS = {
  'time-bar': TimeBarChart, stat: StatValue, distribution: DistributionBars, table: KvTable,
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
  // 기간·단위: 페이지 컨트롤이 있으면 버스 값, 없으면 자기 값. 훅은 항상 둘 다 호출(조건부 호출 금지).
  const controlled = useHasPageControl('period')
  const [busDate] = usePageParam('date')
  const [busGran] = usePageParam('gran')
  const [ownDate, setOwnDate] = useState(todayIso())
  const [ownGran, setOwnGran] = useState('1h')
  const date = controlled ? busDate : ownDate
  const gran = controlled ? busGran : ownGran
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
      const raw = await loadSource(src, { date, granularity: gran } as SourceParams)
      const adapt = src[SHAPE_ADAPTER[shape]] as ((r: unknown) => ShapeData) | undefined
      const d = adapt ? adapt(raw) : null
      // stat: 지표 묶음(KpiData)에서 지정한 하나만 남긴다 — 카드 하나에 값 하나.
      setData(d && shape === 'stat'
        ? { items: [(d as KpiData).items[itemIndex(config)]].filter(Boolean) } as ShapeData
        : d)
    } catch (e) { setErr((e as Error).message); setData(null) }
    finally { setLoading(false) }
  }, [src, shape, date, gran, config])

  useEffect(() => { void load() }, [load])

  const body = catError ? <div style={{ color: 'var(--danger)', fontSize: 13 }}>※ 소스 카탈로그: {catError}</div>
    : catLoading && sources.length === 0 ? <div className="empty">소스 카탈로그 로딩 중...</div>
    : !src ? <div className="empty">소스를 선택하세요</div>
    : loading && !data ? <div className="empty">로딩 중...</div>
    : err ? <div style={{ color: 'var(--danger)', fontSize: 13 }}>※ {err}</div>
    : data ? <Renderer data={data as never} />
    : <div className="empty">데이터 없음</div>

  // 지표 카드는 chrome 최소화 — 값만. 소스·지표는 편집 모드 [⚙] 에서 정한다.
  if (shape === 'stat') {
    return <div className="panel" style={{ padding: 10, display: 'flex', flexDirection: 'column' }}>{body}</div>
  }

  return (
    <div className="panel" style={{ padding: 12 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 10, flexWrap: 'wrap' }}>
        {/* 제목 — 배치에서 지정(config.title)한 이름이 우선. 소스가 고정된 화면에서는 소스명보다
            "무엇을 그리는가"(호 시도 추이, 종료 사유 분포)가 읽기 쉽다. */}
        <span style={{ fontWeight: 600, fontSize: 13 }}>
          {typeof config?.title === 'string' && config.title ? config.title : SHAPE_LABELS[shape]}
        </span>
        {src?.needsControls !== false && (controlled ? (
          // 페이지 컨트롤이 조건을 소유 — 값만 표기(중복 컨트롤 제거).
          <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>
            {date} · {GRAN_LABELS[gran] ?? gran}
          </span>
        ) : (
          <>
            <input className="form-input" type="date" value={date} onChange={e => setOwnDate(e.target.value)}
                   style={{ width: 140, fontSize: 12 }} />
            {Object.entries(GRAN_LABELS).map(([g, lb]) => (
              <button key={g} className={`btn btn--sm ${gran === g ? 'btn--primary' : 'btn--ghost'}`}
                      onClick={() => setOwnGran(g)}>{lb}</button>
            ))}
          </>
        ))}
        <button className="btn btn--sm btn--outline" style={{ marginLeft: 'auto' }} onClick={() => void load()}>↻</button>
      </div>
      {body}
    </div>
  )
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
export const statWidget         = makeShapeWidget('stat', 'shape.stat', '지표 (소스·항목 선택)')
export const distributionWidget = makeShapeWidget('distribution', 'shape.distribution', '분포')
export const tableShapeWidget   = makeShapeWidget('table', 'shape.table', '표')

export const SHAPE_WIDGETS: WidgetDef[] = [
  statWidget, timeBarWidget, distributionWidget, tableShapeWidget,
]
