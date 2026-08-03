// CIMS 위젯 — CSP 모듈 역할 + 녹취 + 타이머 설정.
import { useSharedHealth } from '@core/widgets/useSharedHealth'
import type { WidgetDef } from '@core/widgets/types'

function CspRolesWidget() {
  const { data } = useSharedHealth()
  if (!data) return null
  return (
    <div className="panel" style={{ padding: 16 }}>
      <div style={{ fontWeight: 600, marginBottom: 8, fontSize: 14 }}>CSP 모듈 역할</div>
      <div style={{ display: 'flex', gap: 16, flexWrap: 'wrap' }}>
        {Object.entries(data.csp.roles).map(([k, v]) => (
          <span key={k} className={`badge ${v ? 'badge--green' : 'badge--gray'}`}>
            {k}: {v ? 'ON' : 'OFF'}
          </span>
        ))}
        <span style={{ marginLeft: 'auto' }} className={`badge ${data.record_enable ? 'badge--blue' : 'badge--gray'}`}>
          녹취: {data.record_enable ? 'ON' : 'OFF'}
        </span>
      </div>
      {data.csp.timeouts && (
        <div style={{ display: 'flex', gap: 16, marginTop: 8, fontSize: 12, color: 'var(--text-muted)' }}>
          <span>등록 만료: {data.csp.timeouts.user_timeout}초</span>
          <span>Stale Call: {data.csp.timeouts.stale_call_timeout}초</span>
          <span>OPTIONS 주기: {data.csp.timeouts.send_options_period || '비활성'}초</span>
          {data.cmp.session_timeout != null && <span>CMP 세션: {data.cmp.session_timeout}초</span>}
        </div>
      )}
    </div>
  )
}

export const cspRolesWidget: WidgetDef = {
  id: 'cims.csp-roles',
  apis: ['stats.health'],
  title: 'CSP 모듈 역할',
  category: 'service',
  component: CspRolesWidget,
  defaultSize: { w: 12 },
}
