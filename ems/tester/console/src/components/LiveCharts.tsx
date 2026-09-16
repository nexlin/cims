// 지표별 소형 차트 6장 — 시도율·실패/초 · 동시 세션 · SER · SRD p95 · RTP 손실 · 워커/대상 CPU (test_instrument.md §7).
// 입력은 초 단위 열 형태(RunSeries 와 같은 꼴) — 라이브 패널은 SSE 버킷을, 결과 화면은 /series 를 같은 꼴로 넘긴다.
import { useState } from 'react'
import MiniChart from '@tester/components/MiniChart'
import type { Thresholds } from '@tester/lib/metrics'

export interface ColumnSeries {
  t: number[]
  counters: Record<string, (number | null | undefined)[]>
  gauges: Record<string, (number | null | undefined)[]>
  timers: Record<string, { p95: (number | null | undefined)[] }>
}

export default function LiveCharts({ data, thresholds, bands, markers, height, stopOn, hover: extHover, onHover: extOnHover }: {
  data: ColumnSeries
  thresholds: Thresholds
  bands?: { from: number; to: number; label: string }[]
  markers?: { t: number; label: string; tone?: 'danger' | 'warning' | 'info' }[]
  height?: number
  stopOn?: { target_cpu_pct?: number; csp_5xx_pct?: number; ser_pct_min?: number }
  hover?: number | null
  onHover?: (i: number | null) => void
}) {
  const [innerHover, setInnerHover] = useState<number | null>(null)
  const hover = extHover !== undefined ? extHover : innerHover
  const onHover = extOnHover ?? setInnerHover
  const { t, counters: c, gauges: g, timers: tm } = data
  const ratio = t.map((_, i) => { const a = c.attempts?.[i] ?? 0; return a ? (100 * (c.sessions?.[i] ?? 0)) / a : null })
  const loss = t.map((_, i) => { const tot = (c.rtp_rx?.[i] ?? 0) + (c.rtp_lost?.[i] ?? 0); return tot ? (100 * (c.rtp_lost?.[i] ?? 0)) / tot : null })
  const common = { t, bands, hover, onHover, height, markers }
  return (
    <div className="grid gap-2 sm:grid-cols-2 xl:grid-cols-3">
      <MiniChart {...common} label="시도율 · 실패/초" unit="/s"
                 series={[{ key: 'att', values: c.attempts ?? [], color: 'var(--chart-1)' }, { key: 'fail', values: c.failed ?? [], color: 'var(--chart-10)' }]} />
      <MiniChart {...common} label="동시 세션" series={[{ key: 'conc', values: g.concurrent_sessions ?? [], color: 'var(--chart-2)' }]} />
      <MiniChart {...common} label="SER" unit="%" max={100} threshold={thresholds.ser_pct ?? stopOn?.ser_pct_min ?? null} thresholdLabel="최소"
                 series={[{ key: 'ser', values: ratio, color: 'var(--chart-3)' }]} />
      <MiniChart {...common} label="SRD p95" unit="ms" threshold={thresholds.srd_ms ?? null}
                 series={[{ key: 'srd', values: tm.srd_ms?.p95 ?? [], color: 'var(--chart-4)' }]} />
      <MiniChart {...common} label="RTP 손실" unit="%" threshold={thresholds.rtp_loss_pct ?? null} thresholdLabel="최대"
                 series={[{ key: 'loss', values: loss, color: 'var(--chart-5)' }]} />
      <MiniChart {...common} label="워커 · 대상 CPU" unit="%" max={100} threshold={stopOn?.target_cpu_pct ?? null} thresholdLabel="stop_on"
                 series={[{ key: 'cpu', values: g.cpu_pct ?? [], color: 'var(--chart-6)' }, { key: 'tcpu', values: g.target_cpu_pct ?? [], color: 'var(--chart-8)', dashed: true }]} />
    </div>
  )
}
