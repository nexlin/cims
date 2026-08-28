// CIMS 위젯 — CSP/CMP/DB 헬스 점.
import { useSharedHealth } from '@core/widgets/useSharedHealth'
import type { WidgetDef } from '@core/widgets/types'
import { StatusDot } from './shared'

function HealthDotsWidget() {
  const { data } = useSharedHealth()
  // 로딩 중에도 자리를 지킨다(팝인 방지) — 점만 흐리게.
  if (!data) {
    return (
      <div style={{ display: 'flex', gap: 12, opacity: 0.5 }}>
        {['CSP', 'CMP', 'DB'].map(n => (
          <span key={n} style={{ fontSize: 12, color: 'var(--text-muted)' }}>{n} …</span>
        ))}
      </div>
    )
  }
  const h = data.health
  return (
    <div style={{ display: 'flex', gap: 12 }}>
      {[
        { name: 'CSP', status: h.csp },
        { name: 'CMP', status: h.cmp },
        { name: 'DB', status: h.db },
      ].map(s => (
        <div key={s.name} style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '12px 16px', display: 'flex', alignItems: 'center', gap: 8 }}>
          <StatusDot status={s.status} />
          <span style={{ fontWeight: 600 }}>{s.name}</span>
          <span style={{ marginLeft: 'auto', fontSize: 13, color: 'var(--text-muted)' }}>
            {s.status === 'up' ? '정상' : '연결 끊김'}
          </span>
        </div>
      ))}
    </div>
  )
}

export const healthDotsWidget: WidgetDef = {
  id: 'cims.health-dots',
  apis: ['stats.health'],
  title: 'CSP/CMP/DB 상태',
  category: 'service',
  component: HealthDotsWidget,
  defaultSize: { w: 12 },
}
