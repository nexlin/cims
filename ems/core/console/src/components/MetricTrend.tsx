// 메트릭 시계열 sparkline — 값(null 허용) 배열을 받아 추세선 + 현재/peak 표시.
// Agent Observability: heartbeat 가 1~2s 주기로 쌓는 cpu/mem/disk raw metric 을 시각화.
//
// 정본 = Figma M1 메트릭 모달(193:3205). 선 색은 토큰 실측: CPU=`--cims-info`(#2563eb) ·
// MEM=`--cims-success`(#16a34a) · Disk=`--primary`(#4f46e5). **임계치는 붉은 점선**이고
// 그 사실을 모달 하단 안내문이 알린다.
export default function MetricTrend({ label, values, unit = '%', color, warn, width = 200, height = 40 }: {
  label: string; values: (number | null)[]; unit?: string
  /** 토큰 참조(`var(--…)`)를 넘긴다 — hex 직접 사용 금지 */
  color: string
  warn?: number; width?: number; height?: number
}) {
  const nums = values.filter((v): v is number => v != null)
  const w = width, h = height, pad = 3
  const cur = nums.length ? nums[nums.length - 1] : null
  const peak = nums.length ? Math.max(...nums) : null
  const overWarn = warn != null && cur != null && cur >= warn
  let body: React.ReactNode = (
    <div className="flex items-center text-xs text-muted-foreground" style={{ height: h }}>
      데이터 부족
    </div>
  )
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
      <svg width={w} height={h} className="block">
        {warnY != null && (
          <line x1={pad} y1={warnY} x2={w - pad} y2={warnY} stroke="var(--destructive)"
                strokeWidth={0.8} strokeDasharray="3 2" opacity={0.6} />
        )}
        <polyline points={pts} fill="none" stroke={overWarn ? 'var(--destructive)' : color}
                  strokeWidth={1.5} strokeLinecap="round" strokeLinejoin="round" />
      </svg>
    )
  }
  return (
    <div className="min-w-0 flex-1 rounded-md border border-border bg-card px-2.5 py-2">
      <div className="mb-1 flex items-baseline gap-1.5">
        <span className="text-sm text-muted-foreground">{label}</span>
        <span className={`text-lg font-bold ${overWarn ? 'text-destructive' : ''}`}>
          {cur != null ? `${cur}${unit}` : '—'}
        </span>
        {peak != null && (
          <span className="ml-auto text-xs text-muted-foreground">peak {peak}{unit}</span>
        )}
      </div>
      {body}
    </div>
  )
}
