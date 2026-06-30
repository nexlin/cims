// 범용 shape 위젯 — 자기 shape 를 지원하는 데이터 소스를 dropdown 으로 선택 → fetch → 렌더.
// config.source 로 기본 소스 영속, 운영자가 위젯 내에서 즉시 전환.
import { useState, useEffect, useMemo, useCallback } from 'react'
import type { WidgetDef, WidgetProps } from '../types'
import type { ShapeKind, ShapeData, SourceParams } from './types'
import { SHAPE_LABELS, SHAPE_ADAPTER } from './types'
import { sourcesForShape, useDataSourceCatalog, loadSource } from './sourceRegistry'
import { TimeBarChart, KpiCards, DistributionBars, KvTable } from './renderers'

const GRAN_LABELS: Record<string, string> = { '1h': '시간', '1d': '일', '1M': '월' }

const RENDERERS = {
  'time-bar': TimeBarChart, kpi: KpiCards, distribution: DistributionBars, table: KvTable,
} as const

function ShapeWidgetBody({ shape, config }: { shape: ShapeKind; config?: Record<string, unknown> }) {
  // 소스 카탈로그 = Service Descriptor data_sources (백엔드 데이터 구동). 비동기 로드.
  const { sources: catalog, loading: catLoading, error: catError } = useDataSourceCatalog()
  const sources = useMemo(() => sourcesForShape(shape, catalog), [shape, catalog])
  const wanted = typeof config?.source === 'string' ? config.source as string : ''
  const [sourceId, setSourceId] = useState(wanted)
  const [date, setDate] = useState(new Date().toISOString().slice(0, 10))
  const [gran, setGran] = useState('1h')
  const [data, setData] = useState<ShapeData | null>(null)
  const [loading, setLoading] = useState(false)
  const [err, setErr] = useState('')

  // 카탈로그 도착 후 소스 기본값 확정 (config.source 가 유효하면 그것, 아니면 첫 소스).
  useEffect(() => {
    if (sources.length === 0) return
    if (!sources.some(s => s.id === sourceId)) {
      setSourceId(sources.some(s => s.id === wanted) ? wanted : sources[0].id)
    }
  }, [sources, sourceId, wanted])

  const src = sources.find(s => s.id === sourceId)
  const Renderer = RENDERERS[shape]

  const load = useCallback(async () => {
    if (!src) return
    setLoading(true); setErr('')
    try {
      const raw = await loadSource(src, { date, granularity: gran } as SourceParams)
      const adapt = src[SHAPE_ADAPTER[shape]] as ((r: unknown) => ShapeData) | undefined
      setData(adapt ? adapt(raw) : null)
    } catch (e) { setErr((e as Error).message); setData(null) }
    finally { setLoading(false) }
  }, [src, shape, date, gran])

  useEffect(() => { void load() }, [load])

  return (
    <div className="panel" style={{ padding: 12 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 10, flexWrap: 'wrap' }}>
        <span style={{ fontWeight: 600, fontSize: 13 }}>{SHAPE_LABELS[shape]}</span>
        <select className="form-input" value={sourceId} onChange={e => setSourceId(e.target.value)}
                style={{ fontSize: 12, minWidth: 130 }}>
          {sources.length === 0 && <option value="">{catLoading ? '소스 로딩…' : '(소스 없음)'}</option>}
          {sources.map(s => <option key={s.id} value={s.id}>{s.label}</option>)}
        </select>
        {src?.needsControls !== false && (
          <>
            <input className="form-input" type="date" value={date} onChange={e => setDate(e.target.value)}
                   style={{ width: 140, fontSize: 12 }} />
            {Object.entries(GRAN_LABELS).map(([g, lb]) => (
              <button key={g} className={`btn btn--sm ${gran === g ? 'btn--primary' : 'btn--ghost'}`}
                      onClick={() => setGran(g)}>{lb}</button>
            ))}
          </>
        )}
        <button className="btn btn--sm btn--outline" style={{ marginLeft: 'auto' }} onClick={() => void load()}>↻</button>
      </div>
      {catError ? <div style={{ color: 'var(--danger)', fontSize: 13 }}>※ 소스 카탈로그: {catError}</div>
        : catLoading && sources.length === 0 ? <div className="empty">소스 카탈로그 로딩 중...</div>
        : !src ? <div className="empty">소스를 선택하세요</div>
        : loading ? <div className="empty">로딩 중...</div>
        : err ? <div style={{ color: 'var(--danger)', fontSize: 13 }}>※ {err}</div>
        : data ? <Renderer data={data as never} />
        : <div className="empty">데이터 없음</div>}
    </div>
  )
}

// shape → WidgetDef 팩토리. 4개 코어(서비스 무관) shape 위젯을 생성.
function makeShapeWidget(shape: ShapeKind, id: string, title: string): WidgetDef {
  return {
    id, title, category: 'view', defaultSize: { w: shape === 'kpi' ? 12 : 6 },
    component: (p: WidgetProps) => <ShapeWidgetBody shape={shape} config={p.config} />,
  }
}

export const timeBarWidget      = makeShapeWidget('time-bar', 'shape.time-bar', '시계열 차트')
export const kpiShapeWidget     = makeShapeWidget('kpi', 'shape.kpi', 'KPI 지표')
export const distributionWidget = makeShapeWidget('distribution', 'shape.distribution', '분포')
export const tableShapeWidget   = makeShapeWidget('table', 'shape.table', '표')

export const SHAPE_WIDGETS: WidgetDef[] = [
  timeBarWidget, kpiShapeWidget, distributionWidget, tableShapeWidget,
]
