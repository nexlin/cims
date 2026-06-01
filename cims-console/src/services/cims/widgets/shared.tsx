// CIMS 대시보드 위젯 공용 조각 — Sparkline / StatusDot / 시각 포맷.

export function StatusDot({ status }: { status: string }) {
  const color = status === 'up' ? 'var(--success, #22c55e)' : 'var(--danger)'
  return <span style={{ display: 'inline-block', width: 10, height: 10, borderRadius: '50%', background: color, marginRight: 6 }} />
}

export function Sparkline({ data, color = 'var(--primary)', height = 24 }: { data: number[]; color?: string; height?: number }) {
  if (data.length < 2) return <div style={{ height }} />
  const w = 140, h = height, pad = 2
  const max = Math.max(...data, 1)
  const min = Math.min(...data, 0)
  const range = max - min || 1
  const step = (w - pad * 2) / Math.max(data.length - 1, 1)
  const pts = data.map((v, i) => {
    const x = pad + i * step
    const y = pad + (h - pad * 2) * (1 - (v - min) / range)
    return `${x.toFixed(1)},${y.toFixed(1)}`
  }).join(' ')
  return (
    <svg width={w} height={h} style={{ display: 'block', margin: '4px auto 0' }}>
      <polyline points={pts} fill="none" stroke={color} strokeWidth={1.5}
        strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  )
}

export function fmtTime(iso: string | null): string {
  if (!iso) return '-'
  return new Date(iso).toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}
