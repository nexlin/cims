// shape별 순수 렌더러 — shape 데이터만 받아 그린다 (소스/fetch 무관). 테마 토큰 사용.
//
// **차트 높이는 담긴 칸을 따라간다** — px 를 박아 두면 카드를 키워도 여백만 생기고 줄이면 잘린다.
// 캔버스가 고정 예산(화면 한 장)이라 카드 크기가 배치마다 다르므로, 막대 높이는 플롯 영역 대비
// **비율(%)** 로 그린다(플롯 영역은 flex:1 로 남은 높이를 전부 차지).
import { useRef, useState } from 'react'
import type { TimeBarData, SeriesBarData, KpiData, DistributionData, TableData } from './types'

export function TimeBarChart({ data }: { data: TimeBarData }) {
  const { buckets, unit } = data
  const vals = buckets.map(b => b.value)
  const max = Math.max(...vals, 1)
  if (buckets.length === 0) return <div className="empty">데이터 없음</div>
  return (
    <div style={{ flex: 1, minHeight: 0, display: 'flex', alignItems: 'flex-end', gap: 2, padding: '0 4px' }}>
      {buckets.map((b, i) => (
        <div key={i} style={{ flex: 1, minWidth: 0, height: '100%',
                              display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
          <div style={{ flex: 'none', fontSize: 10, color: 'var(--text-muted)', marginBottom: 2 }}>
            {b.value > 0 ? b.value : ''}
          </div>
          {/* 막대 영역 — 남은 높이 전부. 막대는 그 안에서 값 비율만큼 차지한다. */}
          <div style={{ flex: 1, minHeight: 0, width: '100%',
                        display: 'flex', alignItems: 'flex-end', justifyContent: 'center' }}>
            <div title={`${b.label}: ${b.value}${unit || ''}`}
                 style={{ width: '100%', maxWidth: 32, height: `${(b.value / max) * 100}%`, minHeight: 2,
                          background: 'var(--primary)', borderRadius: '2px 2px 0 0' }} />
          </div>
          <div style={{ flex: 'none', fontSize: 10, color: 'var(--text-muted)', marginTop: 2 }}>{String(b.label)}</div>
        </div>
      ))}
    </div>
  )
}

// 계열 시계열 — 한 버킷에 **막대 하나**, 고른 계열을 색으로 **쌓아** 올린다(아래→위 = 선언 순서).
// 쌓기는 "부분의 합"을 뜻하므로 막대 높이가 곧 고른 계열의 합계다 — VoLTE 4 위에 PTT 4 를 얹으면
// 8 짜리 막대가 된다. 포함관계인 계열(전체 ⊃ VoLTE)을 같이 켜면 그 합이 중복이라, 그럴 때만
// 범례 아래에 한 줄로 알린다(선택을 막지는 않는다 — 참조선으로 겹쳐 보고 싶을 수 있다).
export function SeriesBarChart({ data }: { data: SeriesBarData }) {
  const { buckets, series, unit } = data
  const [hover, setHover] = useState<
    { x: number; y: number; bucket: string; key: string; total: number } | null>(null)
  const wrap = useRef<HTMLDivElement>(null)

  if (series.length === 0) return <div className="empty">표시할 계열을 선택하세요</div>
  if (buckets.length === 0) return <div className="empty">데이터 없음</div>

  const sum = (b: typeof buckets[number]) => series.reduce((a, sp) => a + (b.values[sp.key] || 0), 0)
  const max = Math.max(1, ...buckets.map(sum))
  // 라벨이 촘촘하면(버킷이 많으면) 몇 칸 걸러 하나만 적는다 — 겹쳐 뭉개지는 것보다 낫다.
  const every = Math.ceil(buckets.length / 24)
  // 고른 계열 중 포함관계로 겹치는 쌍이 있으면 합계가 중복된다.
  const shownKeys = new Set(series.map(sp => sp.key))
  const overlap = series.filter(sp => (sp.includes ?? []).some(k => shownKeys.has(k)))

  const move = (e: React.MouseEvent, bucket: string, key: string, total: number) => {
    const r = wrap.current?.getBoundingClientRect()
    if (!r) return
    setHover({ x: e.clientX - r.left, y: e.clientY - r.top, bucket, key, total })
  }
  const hoveredBucket = hover ? buckets.find(b => String(b.label) === hover.bucket) : undefined

  return (
    <div ref={wrap} style={{ position: 'relative', flex: 1, minHeight: 0,
                             display: 'flex', flexDirection: 'column' }}
         onMouseLeave={() => setHover(null)}>
      <div style={{ flex: 'none', display: 'flex', gap: 14, flexWrap: 'wrap', marginBottom: 8 }}>
        {series.map(sp => (
          <span key={sp.key} style={{ display: 'flex', alignItems: 'center', gap: 5, fontSize: 12 }}>
            <span style={{ width: 10, height: 10, borderRadius: 2, background: sp.color }} />
            {sp.label}
          </span>
        ))}
      </div>
      {overlap.length > 0 && (
        <div style={{ flex: 'none', fontSize: 11, color: 'var(--text-muted)', marginBottom: 8 }}>
          ※ {overlap.map(sp => sp.label).join(' · ')} 은(는) 다른 계열을 포함합니다 — 함께 쌓으면 합계가 중복됩니다.
        </div>
      )}
      <div style={{ flex: 1, minHeight: 0, display: 'flex', alignItems: 'flex-end', gap: 2, padding: '0 4px' }}>
        {buckets.map((b, i) => {
          const total = sum(b)
          const on = hover?.bucket === String(b.label)
          return (
            <div key={i} style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column',
                                  alignItems: 'center', height: '100%' }}>
              <div style={{ flex: 1, width: '100%', minHeight: 0, display: 'flex',
                            flexDirection: 'column-reverse', alignItems: 'center',
                            justifyContent: 'flex-start' }}>
                {/* column-reverse — 선언 순서 첫 계열이 바닥에 깔린다 */}
                {series.map(sp => {
                  const v = b.values[sp.key] || 0
                  if (v <= 0) return null
                  return (
                    <div key={sp.key}
                         onMouseMove={e => move(e, String(b.label), sp.key, total)}
                         style={{ width: '100%', maxWidth: 26, height: `${(v / max) * 100}%`, minHeight: 2,
                                  background: sp.color,
                                  opacity: !hover || hover.key === sp.key ? 1 : 0.45,
                                  cursor: 'default' }} />
                  )
                })}
                {total === 0 && (
                  <div onMouseMove={e => move(e, String(b.label), '', 0)}
                       style={{ width: '100%', maxWidth: 26, height: 2, background: 'var(--border)' }} />
                )}
              </div>
              <div style={{ flex: 'none', fontSize: 10, marginTop: 2, whiteSpace: 'nowrap', overflow: 'hidden',
                            color: on ? 'var(--text)' : 'var(--text-muted)',
                            fontWeight: on ? 600 : 400 }}>
                {i % every === 0 || on ? String(b.label) : ''}
              </div>
            </div>
          )
        })}
      </div>
      {hover && hoveredBucket && (
        <div style={{
          position: 'absolute', left: Math.min(hover.x + 12, (wrap.current?.clientWidth ?? 0) - 190),
          top: Math.max(hover.y - 12, 0), zIndex: 30, pointerEvents: 'none', width: 178,
          background: 'var(--surface)', border: '1px solid var(--border)',
          borderRadius: 'var(--radius)', boxShadow: 'var(--shadow-lg)', padding: '8px 10px', fontSize: 12,
        }}>
          <div style={{ color: 'var(--text-muted)', marginBottom: 5 }}>{hover.bucket}</div>
          {series.map(sp => {
            const v = hoveredBucket.values[sp.key] || 0
            const cur = sp.key === hover.key
            return (
              <div key={sp.key} style={{ display: 'flex', alignItems: 'center', gap: 6, marginTop: 2,
                                         fontWeight: cur ? 700 : 400, opacity: cur || v > 0 ? 1 : 0.5 }}>
                <span style={{ width: 8, height: 8, borderRadius: 2, background: sp.color, flex: 'none' }} />
                <span style={{ flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                  {sp.label}
                </span>
                <span>{v}{unit || ''}</span>
              </div>
            )
          })}
          {series.length > 1 && (
            <div style={{ display: 'flex', justifyContent: 'space-between', marginTop: 6, paddingTop: 5,
                          borderTop: '1px solid var(--border)', color: 'var(--text-muted)' }}>
              <span>합계</span><span style={{ fontWeight: 700, color: 'var(--text)' }}>{hover.total}{unit || ''}</span>
            </div>
          )}
        </div>
      )}
    </div>
  )
}

export function KpiCards({ data }: { data: KpiData }) {
  return (
    <div style={{ display: 'flex', gap: 12, flexWrap: 'wrap' }}>
      {data.items.map((k, i) => (
        <div key={i} style={{ flex: '1 1 120px', background: 'var(--surface)', border: '1px solid var(--border)',
                              borderRadius: 'var(--radius)', padding: '14px 16px', textAlign: 'center' }}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 4 }}>{k.label}</div>
          <div style={{ fontSize: 24, fontWeight: 700 }}>
            {k.value}<span style={{ fontSize: 12, color: 'var(--text-muted)', marginLeft: 2 }}>{k.unit}</span>
          </div>
        </div>
      ))}
    </div>
  )
}

// 지표 카드 — 값 하나. 카드가 자기 칸을 채우고 값은 세로 중앙.
export function StatValue({ data }: { data: KpiData }) {
  const k = data.items[0]
  if (!k) return <div className="empty" style={{ fontSize: 12 }}>지표 없음</div>
  return (
    <div style={{ flex: '1 1 auto', minHeight: 0, display: 'flex', flexDirection: 'column',
                  justifyContent: 'center', alignItems: 'center', textAlign: 'center' }}>
      <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 4 }}>{k.label}</div>
      <div style={{ fontSize: 24, fontWeight: 700, lineHeight: 1.1 }}>
        {k.value}<span style={{ fontSize: 12, color: 'var(--text-muted)', marginLeft: 2 }}>{k.unit}</span>
      </div>
    </div>
  )
}

export function DistributionBars({ data }: { data: DistributionData }) {
  const { items, total, series } = data
  const [hover, setHover] = useState<{ i: number; key: string } | null>(null)
  if (items.length === 0) return <div className="empty">데이터 없음</div>
  // 계열이 선언돼 있으면 막대 하나를 계열별 조각으로 나눠 색칠한다(시계열 차트와 같은 색).
  const seg = (series ?? []).length > 0
  return (
    <div onMouseLeave={() => setHover(null)}>
      {seg && (
        <div style={{ display: 'flex', gap: 14, flexWrap: 'wrap', marginBottom: 8 }}>
          {series!.map(sp => (
            <span key={sp.key} style={{ display: 'flex', alignItems: 'center', gap: 5, fontSize: 12 }}>
              <span style={{ width: 10, height: 10, borderRadius: 2, background: sp.color }} />
              {sp.label}
            </span>
          ))}
        </div>
      )}
      {items.slice().sort((a, b) => b.value - a.value).map((it, i) => {
        const pct = total > 0 ? Math.round(it.value / total * 100) : 0
        const on = hover?.i === i
        return (
          <div key={i} style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
            <div style={{ width: 90, fontSize: 13, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{it.label || 'unknown'}</div>
            <div style={{ flex: 1, background: 'var(--surface-2)', borderRadius: 4, height: 18, display: 'flex', overflow: 'hidden' }}>
              {seg ? series!.map(sp => {
                const v = it.parts?.[sp.key] || 0
                if (v <= 0) return null
                const w = total > 0 ? v / total * 100 : 0
                return (
                  <div key={sp.key}
                       title={`${it.label} · ${sp.label}: ${v}`}
                       onMouseEnter={() => setHover({ i, key: sp.key })}
                       style={{ width: `${w}%`, minWidth: 3, background: sp.color, height: 18,
                                opacity: !hover || hover.key === sp.key ? 1 : 0.45 }} />
                )
              }) : (
                <div style={{ width: `${pct}%`, background: 'var(--primary)', borderRadius: 4, height: 18, minWidth: pct > 0 ? 4 : 0 }} />
              )}
            </div>
            <div style={{ width: 78, fontSize: 12, textAlign: 'right',
                          color: on ? 'var(--text)' : 'var(--text-muted)', fontWeight: on ? 600 : 400 }}>
              {on && hover ? `${hover.key}: ${it.parts?.[hover.key] ?? 0}` : `${it.value} (${pct}%)`}
            </div>
          </div>
        )
      })}
    </div>
  )
}

export function KvTable({ data }: { data: TableData }) {
  return (
    <table className="data-table" style={{ fontSize: 12 }}>
      <thead><tr><th>{data.columns[0]}</th><th style={{ width: 90, textAlign: 'right' }}>{data.columns[1]}</th></tr></thead>
      <tbody>
        {data.rows.length === 0 ? <tr><td colSpan={2} className="empty-cell">데이터 없음</td></tr>
          : data.rows.map((r, i) => (
            <tr key={i}><td>{r.key}</td><td style={{ textAlign: 'right', fontWeight: 600 }}>{r.value}</td></tr>
          ))}
      </tbody>
    </table>
  )
}
