// 지표별 소형 차트 — 지표마다 자기 값 축 + 붉은 점선 기대치(임계) + 단계 띠(공유) + 십자선 호버(부모가 인덱스를 공유해 전 차트 동기).
// SVG 인라인, 선 색은 --chart-N 토큰만 (test_instrument.md §7 실행 라이브 — 한 축 겹침 차트를 대체한다).
import { useMemo } from 'react'

export interface MiniSeries { key: string; values: (number | null | undefined)[]; color: string; label?: string; dashed?: boolean }

export default function MiniChart({ t, series, label, unit = '', threshold, thresholdLabel, bands, hover, onHover, height = 104, max: fixedMax, markers }: {
  t: number[]
  series: MiniSeries[]
  label: string
  unit?: string
  /** 붉은 점선 — 시나리오 expect 에서 유도(없으면 안 그림) */
  threshold?: number | null
  thresholdLabel?: string
  bands?: { from: number; to: number; label: string }[]
  /** 공유 호버 인덱스(null = 없음) */
  hover: number | null
  onHover: (i: number | null) => void
  height?: number
  max?: number
  /** 세로 마커(알람·이벤트) — unix 초 + 라벨 + 톤 */
  markers?: { t: number; label: string; tone?: 'danger' | 'warning' | 'info' }[]
}) {
  const W = 600, H = height, padL = 6, padR = 6, padT = 6, padB = 14
  const n = t.length
  const t0 = n ? t[0] : 0, t1 = n ? t[n - 1] : 1
  const span = Math.max(1, t1 - t0)
  const x = (tt: number) => padL + ((tt - t0) / span) * (W - padL - padR)
  const { max, lines } = useMemo(() => {
    const nums = series.flatMap(s => s.values.filter((v): v is number => typeof v === 'number' && isFinite(v)))
    const m = Math.max(fixedMax ?? 0, ...nums, (threshold ?? 0) * 1.15, 1e-9)
    const y = (v: number) => padT + (H - padT - padB) * (1 - Math.min(v, m) / m)
    const lines = series.map(s => {
      const segs: string[] = []
      let seg: string[] = []
      s.values.forEach((v, i) => {
        if (typeof v !== 'number' || !isFinite(v) || i >= n) { if (seg.length) { segs.push(seg.join(' ')); seg = [] }; return }
        seg.push(`${x(t[i]).toFixed(1)},${y(v).toFixed(1)}`)
      })
      if (seg.length) segs.push(seg.join(' '))
      const last = [...s.values].reverse().find((v): v is number => typeof v === 'number' && isFinite(v)) ?? null
      return { ...s, segs, last }
    })
    return { max: m, y, lines }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [series, t, n, H, threshold, fixedMax])
  const yOf = (v: number) => padT + (H - padT - padB) * (1 - Math.min(v, max) / max)
  const fmt = (v: number | null) => v == null ? '—' : unit === '%' ? v.toFixed(2) : unit === 'ms' ? v.toFixed(0) : (Number.isInteger(v) ? String(v) : v.toFixed(1))
  const cur = (s: typeof lines[number]) => hover != null && typeof s.values[hover] === 'number' ? (s.values[hover] as number) : s.last
  const first = lines[0]
  const over = first && threshold != null && cur(first) != null && (unit === '%' && label.includes('SER') ? (cur(first) as number) < threshold : (cur(first) as number) > threshold)

  const onMove = (e: React.MouseEvent<SVGSVGElement>) => {
    if (!n) return
    const rect = e.currentTarget.getBoundingClientRect()
    const px = ((e.clientX - rect.left) / rect.width) * W
    const tt = t0 + ((px - padL) / (W - padL - padR)) * span
    let best = 0
    for (let i = 1; i < n; i++) if (Math.abs(t[i] - tt) < Math.abs(t[best] - tt)) best = i
    onHover(best)
  }

  return (
    <div className="flex min-w-0 flex-col rounded-md border border-border bg-card">
      <div className="flex items-center gap-2 px-2.5 pt-1.5 text-xs">
        <span className="font-semibold text-foreground">{label}</span>
        {threshold != null && <span className="text-destructive">{thresholdLabel ?? '기대'} {fmt(threshold)}{unit}</span>}
        <span className={`ml-auto font-mono ${over ? 'text-destructive font-semibold' : 'text-foreground'}`}>
          {lines.map((s, i) => <span key={s.key} className={i ? 'ml-2' : ''} style={i ? { color: s.color } : undefined}>{fmt(cur(s))}{i === 0 ? unit : ''}</span>)}
        </span>
      </div>
      {n < 2 ? (
        <div className="flex items-center justify-center text-xs text-muted-foreground" style={{ height: H }}>집계 대기</div>
      ) : (
        <svg viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" className="block w-full" style={{ height: H }}
             onMouseMove={onMove} onMouseLeave={() => onHover(null)}>
          {bands?.map((b, i) => (
            <rect key={i} x={x(Math.max(b.from, t0))} y={padT} width={Math.max(0, x(Math.min(b.to, t1)) - x(Math.max(b.from, t0)))}
                  height={H - padT - padB} fill="var(--chart-muted)" opacity={i % 2 ? 0.16 : 0.07} />
          ))}
          <line x1={padL} y1={H - padB} x2={W - padR} y2={H - padB} stroke="var(--border)" strokeWidth={1} />
          {threshold != null && threshold <= max && (
            <line x1={padL} y1={yOf(threshold)} x2={W - padR} y2={yOf(threshold)} stroke="var(--destructive)" strokeWidth={1}
                  strokeDasharray="4 3" vectorEffect="non-scaling-stroke" />
          )}
          {markers?.map((m, i) => m.t >= t0 && m.t <= t1 ? (
            <line key={`m${i}`} x1={x(m.t)} y1={padT} x2={x(m.t)} y2={H - padB} strokeWidth={1.2} vectorEffect="non-scaling-stroke"
                  stroke={m.tone === 'warning' ? 'var(--warning)' : m.tone === 'info' ? 'var(--info)' : 'var(--destructive)'} opacity={0.7}>
              <title>{m.label}</title>
            </line>
          ) : null)}
          {lines.map(s => s.segs.map((seg, i) => (
            <polyline key={`${s.key}-${i}`} points={seg} fill="none" stroke={s.color} strokeWidth={1.5} strokeDasharray={s.dashed ? '3 2' : undefined}
                      vectorEffect="non-scaling-stroke" strokeLinejoin="round" strokeLinecap="round" />
          )))}
          {hover != null && hover < n && (
            <line x1={x(t[hover])} y1={padT} x2={x(t[hover])} y2={H - padB} stroke="var(--muted-foreground)" strokeWidth={1}
                  strokeDasharray="3 3" vectorEffect="non-scaling-stroke" />
          )}
        </svg>
      )}
      {bands && bands.length > 0 && n >= 2 && (
        <div className="px-2.5 pb-1 text-[10px] text-muted-foreground">{bands.filter(b => b.to >= t0 && b.from <= t1).slice(-1).map(b => b.label).join('')}</div>
      )}
    </div>
  )
}
