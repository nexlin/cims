// 코어 컨트롤 위젯 — 페이지 조회 조건(구간·집계 단위)을 이 페이지의 데이터 위젯들에게 한 번에 건다.
// 이 위젯을 레이아웃에 놓으면 같은 페이지의 shape 위젯들이 자기 컨트롤을 접고 여기를 따른다
// (pageParams.tsx — 컨트롤 소유 선언). 빼면 각 위젯이 다시 자기 컨트롤을 쓴다.
//
// **구간 + 단위** 모델: 기준일 하나에 단위를 곁들이면 "8/28 에 '월' 단위" 가 무엇을 보는지
// 모호했다(그 달인지, 최근 30일인지). 시작~끝을 먼저 정하고 그 안을 단위로 쪼갠다.
// 단위마다 최대 구간이 있고(GRAN_MAX_DAYS ≈ 800버킷) 넘는 단위는 누를 수 없다 —
// 프리셋으로 구간을 바꿀 때는 감당 가능한 가장 세밀한 단위로 자동 승격한다.
import type { WidgetDef, WidgetProps } from '../types'
import {
  GRAN_LABELS, GRAN_MAX_DAYS, bestGran, defaultRange, fmtDt, granFits,
  usePageControl, usePageParam,
} from '../pageParams'

// 'YYYY-MM-DD HH:MM' ↔ datetime-local('YYYY-MM-DDTHH:MM')
const toInput = (v: string) => (v || '').replace(' ', 'T').slice(0, 16)
const fromInput = (v: string) => (v || '').replace('T', ' ').slice(0, 16)

// 프리셋 — 끝은 항상 '지금', 시작만 뒤로 민다.
const PRESETS: { key: string; label: string; days: number }[] = [
  { key: 'today', label: '오늘', days: 0 },
  { key: 'd7', label: '7일', days: 7 },
  { key: 'd30', label: '30일', days: 30 },
]

function PageFilterWidget({ config }: WidgetProps) {
  usePageControl('period')
  const [from, setFrom] = usePageParam('from')
  const [to, setTo] = usePageParam('to')
  const [gran, setGran] = usePageParam('gran')
  // 집계 단위가 없는 조회(일자 단위 목록 등)에서는 단위 버튼을 감춘다.
  const showGran = config?.showGran !== false

  // 구간을 바꾸면 지금 단위가 감당 못 할 수 있다 — 그때만 자동 승격.
  const applyRange = (f: string, t: string) => {
    setFrom(f); setTo(t)
    if (showGran && !granFits(f, t, gran)) setGran(bestGran(f, t))
  }
  const applyPreset = (days: number) => {
    const now = new Date()
    const f = days === 0 ? new Date(now.getFullYear(), now.getMonth(), now.getDate())
                         : new Date(now.getTime() - days * 86400000)
    applyRange(fmtDt(f), fmtDt(now))
  }
  const reset = () => { const r = defaultRange(); applyRange(r.from, r.to) }

  return (
    <div className="panel" style={{ padding: '10px 12px' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap' }}>
        <span style={{ fontWeight: 600, fontSize: 13 }}>조회 구간</span>
        <input className="form-input" type="datetime-local" value={toInput(from)}
               onChange={e => applyRange(fromInput(e.target.value), to)}
               style={{ width: 190, fontSize: 12 }} />
        <span style={{ color: 'var(--text-muted)' }}>~</span>
        <input className="form-input" type="datetime-local" value={toInput(to)}
               onChange={e => applyRange(from, fromInput(e.target.value))}
               style={{ width: 190, fontSize: 12 }} />
        {PRESETS.map(p => (
          <button key={p.key} className="btn btn--sm" onClick={() => applyPreset(p.days)}>{p.label}</button>
        ))}
        <button className="btn btn--sm btn--ghost" title="오늘 00:00 ~ 지금" onClick={reset}>↺</button>

        {showGran && (
          <>
            <div style={{ width: 1, height: 20, background: 'var(--border)', margin: '0 4px' }} />
            {Object.entries(GRAN_LABELS).map(([g, label]) => {
              const fits = granFits(from, to, g)
              const lim = GRAN_MAX_DAYS[g]
              return (
                <button key={g} className={`btn btn--sm ${gran === g ? 'btn--primary' : 'btn--ghost'}`}
                        disabled={!fits}
                        title={fits ? undefined : `${label} 단위는 ${lim}일까지 볼 수 있습니다`}
                        onClick={() => setGran(g)}>{label}</button>
              )
            })}
          </>
        )}
      </div>
    </div>
  )
}

export const pageFilterWidget: WidgetDef = {
  id: 'core.page-filter',
  title: '조회 조건 (구간·단위)',
  category: 'control',
  component: PageFilterWidget,
  configFields: [{ key: 'showGran', label: '단위 버튼', type: 'bool' }],
  defaultSize: { w: 12, h: 4 },
}
