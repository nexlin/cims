// 코어 컨트롤 위젯 — 페이지 조회 조건(기간·집계 단위)을 이 페이지의 데이터 위젯들에게 한 번에 건다.
// 이 위젯을 레이아웃에 놓으면 같은 페이지의 shape 위젯들이 자기 날짜/단위 컨트롤을 접고 여기를 따른다
// (pageParams.tsx — 컨트롤 소유 선언). 빼면 각 위젯이 다시 자기 컨트롤을 쓴다.
import type { WidgetDef, WidgetProps } from '../types'
import { GRAN_LABELS, todayIso, usePageControl, usePageParam } from '../pageParams'

// 날짜 이동(±1일) — 단위와 무관하게 기준일을 옮긴다.
function shiftDay(iso: string, days: number): string {
  const t = Date.parse(`${iso}T00:00:00Z`)
  if (!Number.isFinite(t)) return todayIso()
  return new Date(t + days * 86400000).toISOString().slice(0, 10)
}

function PageFilterWidget({ config }: WidgetProps) {
  usePageControl('period')
  const [date, setDate] = usePageParam('date')
  const [gran, setGran] = usePageParam('gran')
  const today = todayIso()
  // 집계 단위가 없는 조회(일자 단위 목록 등)에서는 단위 버튼을 감춘다.
  const showGran = config?.showGran !== false
  return (
    <div className="panel" style={{ padding: '10px 12px' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap' }}>
        <span style={{ fontWeight: 600, fontSize: 13 }}>조회 조건</span>
        <button className="btn btn--sm btn--ghost" title="하루 전"
                onClick={() => setDate(shiftDay(date, -1))}>◀</button>
        <input className="form-input" type="date" value={date} max={today}
               onChange={e => setDate(e.target.value)} style={{ width: 140, fontSize: 12 }} />
        <button className="btn btn--sm btn--ghost" title="하루 후" disabled={date >= today}
                onClick={() => setDate(shiftDay(date, 1))}>▶</button>
        <button className="btn btn--sm" disabled={date === today} onClick={() => setDate(today)}>오늘</button>
        {showGran && (
          <>
            <div style={{ width: 1, height: 20, background: 'var(--border)', margin: '0 4px' }} />
            {Object.entries(GRAN_LABELS).map(([g, label]) => (
              <button key={g} className={`btn btn--sm ${gran === g ? 'btn--primary' : 'btn--ghost'}`}
                      onClick={() => setGran(g)}>{label}</button>
            ))}
          </>
        )}
        <span style={{ fontSize: 11, color: 'var(--text-muted)', marginLeft: 'auto' }}>
          이 페이지의 조회 위젯에 함께 적용
        </span>
      </div>
    </div>
  )
}

export const pageFilterWidget: WidgetDef = {
  id: 'core.page-filter',
  title: '조회 조건 (기간·단위)',
  category: 'control',
  component: PageFilterWidget,
  configFields: [{ key: 'showGran', label: '단위 버튼', type: 'bool' }],
  defaultSize: { w: 12, h: 4 },
}
