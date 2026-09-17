// 시간축 다중 선 차트 — SVG 인라인(콘솔은 차트 라이브러리를 쓰지 않는다 — MetricTrend·StatsPage 와 같은 방식).
// 선 색은 --chart-N 토큰만. 시리즈마다 자기 축 스케일(왼쪽 0..max) — 서로 단위가 다른 지표(SApS·ms·%)를
// 한 시간축에 겹치는 것이 목적(test_instrument.md §7 실행 화면)이라 값 축 눈금은 시리즈별 max 로 범례에 적는다.
import { useMemo, useState } from 'react'

export interface Series {
  key: string
  label: string
  values: (number | null | undefined)[]
  color: string             // var(--chart-N)
  unit?: string
  /** 시리즈별 고정 상한 (없으면 관측 max) */
  max?: number
}

export default function LineChart({ t, series, height = 180, bands, className }: {
  /** unix 초 (오름차순) */
  t: number[]
  series: Series[]
  height?: number
  /** 단계 진행 띠 — [start_t, end_t, label] */
  bands?: { from: number; to: number; label: string }[]
  className?: string
}) {
  const [hover, setHover] = useState<number | null>(null)
  const W = 1000, H = height, padL = 8, padR = 8, padT = 8, padB = 20
  const n = t.length
  const t0 = n ? t[0] : 0, t1 = n ? t[n - 1] : 1
  const span = Math.max(1, t1 - t0)
  const x = (tt: number) => padL + ((tt - t0) / span) * (W - padL - padR)
  const scaled = useMemo(() => series.map(s => {
    const nums = s.values.filter((v): v is number => typeof v === 'number' && isFinite(v))
    const max = Math.max(s.max ?? 0, ...nums, 1e-9)
    const pts: string[] = []
    let seg: string[] = []
    s.values.forEach((v, i) => {
      if (typeof v !== 'number' || !isFinite(v) || i >= n) { if (seg.length) { pts.push(seg.join(' ')); seg = [] }; return }
      const y = padT + (H - padT - padB) * (1 - v / max)
      seg.push(`${x(t[i]).toFixed(1)},${y.toFixed(1)}`)
    })
    if (seg.length) pts.push(seg.join(' '))
    return { ...s, max, segments: pts, last: nums.length ? nums[nums.length - 1] : null }
  }), [series, t, n, H])

  const ticks = useMemo(() => {
    if (n < 2) return []
    const k = Math.min(6, n)
    return Array.from({ length: k }, (_, i) => t0 + (span * i) / (k - 1))
  }, [n, t0, span])

  const fmtT = (tt: number) => {
    const d = new Date(tt * 1000)
    const p = (v: number) => String(v).padStart(2, '0')
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`
  }

  const onMove = (e: React.MouseEvent<SVGSVGElement>) => {
    if (!n) return
    const rect = e.currentTarget.getBoundingClientRect()
    const px = ((e.clientX - rect.left) / rect.width) * W
    const tt = t0 + ((px - padL) / (W - padL - padR)) * span
    let best = 0
    for (let i = 1; i < n; i++) if (Math.abs(t[i] - tt) < Math.abs(t[best] - tt)) best = i
    setHover(best)
  }

  return (
    <div className={`rounded-md border border-border bg-card ${className ?? ''}`}>
      <div className="flex flex-wrap items-center gap-x-4 gap-y-1 px-3 pt-2 text-xs">
        {scaled.map(s => (
          <span key={s.key} className="inline-flex items-center gap-1.5">
            <span className="inline-block h-[3px] w-4 rounded" style={{ background: s.color }} />
            <span className="text-muted-foreground">{s.label}</span>
            <span className="font-mono text-foreground">
              {hover != null && typeof s.values[hover] === 'number' ? (s.values[hover] as number).toFixed(s.unit === '%' ? 1 : 0)
                : s.last != null ? s.last.toFixed(s.unit === '%' ? 1 : 0) : '—'}{s.unit ?? ''}
            </span>
            <span className="text-muted-foreground">/ max {s.max >= 10 ? s.max.toFixed(0) : s.max.toFixed(1)}</span>
          </span>
        ))}
        {hover != null && n > 0 && <span className="ml-auto font-mono text-muted-foreground">{fmtT(t[hover])}</span>}
      </div>
      {n < 2 ? (
        <div className="flex items-center justify-center text-xs text-muted-foreground" style={{ height: H }}>
          데이터 부족 — 집계가 쌓이면 그려집니다
        </div>
      ) : (
        <svg viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" className="block w-full" style={{ height: H }}
             onMouseMove={onMove} onMouseLeave={() => setHover(null)}>
          {bands?.map((b, i) => (
            <g key={i}>
              <rect x={x(Math.max(b.from, t0))} y={padT} width={Math.max(0, x(Math.min(b.to, t1)) - x(Math.max(b.from, t0)))}
                    height={H - padT - padB} fill="var(--chart-muted)" opacity={i % 2 ? 0.18 : 0.08} />
              <text x={x(Math.max(b.from, t0)) + 3} y={padT + 10} fontSize={10} fill="var(--muted-foreground)">{b.label}</text>
            </g>
          ))}
          {ticks.map((tt, i) => (
            <g key={i}>
              <line x1={x(tt)} y1={padT} x2={x(tt)} y2={H - padB} stroke="var(--border)" strokeWidth={1} />
              <text x={x(tt)} y={H - 6} fontSize={10} textAnchor={i === 0 ? 'start' : i === ticks.length - 1 ? 'end' : 'middle'}
                    fill="var(--muted-foreground)">{fmtT(tt)}</text>
            </g>
          ))}
          <line x1={padL} y1={H - padB} x2={W - padR} y2={H - padB} stroke="var(--border)" strokeWidth={1} />
          {scaled.map(s => s.segments.map((seg, i) => (
            <polyline key={`${s.key}-${i}`} points={seg} fill="none" stroke={s.color} strokeWidth={1.6}
                      vectorEffect="non-scaling-stroke" strokeLinejoin="round" strokeLinecap="round" />
          )))}
          {hover != null && (
            <line x1={x(t[hover])} y1={padT} x2={x(t[hover])} y2={H - padB} stroke="var(--muted-foreground)" strokeWidth={1}
                  strokeDasharray="3 3" vectorEffect="non-scaling-stroke" />
          )}
        </svg>
      )}
    </div>
  )
}
