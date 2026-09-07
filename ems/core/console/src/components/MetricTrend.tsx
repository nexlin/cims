// 메트릭 시계열 sparkline — 값(null 허용) 배열을 받아 추세선 + 현재/peak 표시.
// Agent Observability: heartbeat 가 1~2s 주기로 쌓는 cpu/mem/disk raw metric 을 시각화.
export default function MetricTrend({ label, values, unit = '%', color, warn, width = 200, height = 40 }: {
  label: string; values: (number | null)[]; unit?: string; color: string
  warn?: number; width?: number; height?: number
}) {
  const nums = values.filter((v): v is number => v != null)
  const w = width, h = height, pad = 3
  const cur = nums.length ? nums[nums.length - 1] : null
  const peak = nums.length ? Math.max(...nums) : null
  const overWarn = warn != null && cur != null && cur >= warn
  let body: React.ReactNode = <div style={{ height: h, color: 'var(--muted-foreground)', fontSize: 11, display: 'flex', alignItems: 'center' }}>데이터 부족</div>
  if (nums.length >= 2) {
    const max = Math.max(...nums, warn ?? 0, 1)
    const min = Math.min(...nums, 0)
    const range = max - min || 1
    const step = (w - pad * 2) / Math.max(nums.length - 1, 1)
    const pts = nums.map((v, i) => {
      const x = pad + i * step
      const y = pad + (h - pad * 2) * (1 - (v - min) / range)
      return `${x.toFixed(1)},${y.toFixed(1)}`
    }).join(' ')
    const warnY = warn != null ? pad + (h - pad * 2) * (1 - (warn - min) / range) : null
    body = (
      <svg width={w} height={h} style={{ display: 'block' }}>
        {warnY != null && (
          <line x1={pad} y1={warnY} x2={w - pad} y2={warnY} stroke="#e74c3c"
                strokeWidth={0.8} strokeDasharray="3 2" opacity={0.6} />
        )}
        <polyline points={pts} fill="none" stroke={overWarn ? '#e74c3c' : color}
                  strokeWidth={1.5} strokeLinecap="round" strokeLinejoin="round" />
      </svg>
    )
  }
  return (
    <div style={{ flex: 1, background: 'var(--card)', border: '1px solid var(--border)',
                  borderRadius: 4, padding: '8px 10px', minWidth: 0 }}>
      <div style={{ display: 'flex', alignItems: 'baseline', gap: 6, marginBottom: 4 }}>
        <span style={{ fontSize: 12, color: 'var(--muted-foreground)' }}>{label}</span>
        <span style={{ fontSize: 16, fontWeight: 700, color: overWarn ? '#e74c3c' : 'inherit' }}>
          {cur != null ? `${cur}${unit}` : '—'}
        </span>
        {peak != null && <span style={{ marginLeft: 'auto', fontSize: 10, color: 'var(--muted-foreground)' }}>peak {peak}{unit}</span>}
      </div>
      {body}
    </div>
  )
}
