// CIMS 위젯 — 심각 알람 배너. 표준 알람 스트림(/alerts)의 활성 critical/major 만 표시 —
// sweeper/자기보고의 open/close/ack 상태와 항상 일치한다 (자체 임계 판정 없음).
// 정상이면 렌더 안 함. 데이터는 전역 알람 store 구독 (alarm_pipeline.md §8.2 구독 1원화).
import { ArrowRight, Check } from 'lucide-react'
import type { WidgetDef } from '@core/widgets/types'
import { severityOf, useAlarms } from '@core/widgets/useAlarms'

const BANNER_SEV = new Set(['critical', 'major'])

function AlertBannerWidget() {
  const { active } = useAlarms()
  const severe = active.filter(a => BANNER_SEV.has(severityOf(a)))
  if (severe.length === 0) return null
  return (
    <div style={{
      background: 'color-mix(in srgb, var(--destructive) 8%, var(--card))',
      border: '1px solid color-mix(in srgb, var(--destructive) 35%, var(--border))',
      borderRadius: 'var(--radius)', padding: 12,
    }}>
      <div className="font-semibold text-destructive mb-1 flex items-center">
        알람 ({severe.length})
        <a className="ml-auto inline-flex items-center gap-1 text-sm font-medium text-destructive" href="/alerts/history">이력 보기 <ArrowRight size={13} /></a>
      </div>
      {severe.map((a, i) => (
        <div className="text-md" key={`${a.alarm_id || a.type}-${i}`}>
          {a.message}
          {a.acked && <span className="ml-1.5 inline-flex items-center gap-1 text-xs text-muted-foreground">
        <Check size={12} /> {a.ackUser || '승인'}</span>}
        </div>
      ))}
    </div>
  )
}

export const alertBannerWidget: WidgetDef = {
  id: 'cims.alert-banner',
  title: '알람 배너',
  category: 'event',
  component: AlertBannerWidget,
  apis: ['alerts.list'],
  defaultSize: { w: 12 },
}
