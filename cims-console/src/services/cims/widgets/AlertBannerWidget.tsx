// CIMS 위젯 — 활성 알람 배너 (CSP/CMP/DB down 또는 RTP 80%↑). 정상이면 렌더 안 함.
import { useSharedHealth } from '../../../widgets/useSharedHealth'
import type { WidgetDef } from '../../../widgets/types'

function AlertBannerWidget() {
  const { data } = useSharedHealth()
  if (!data) return null
  const h = data.health
  const rtpPct = data.cmp.rtp_ports.total > 0
    ? Math.round(data.cmp.rtp_ports.used / data.cmp.rtp_ports.total * 100) : 0
  const hasAlert = h.csp === 'down' || h.cmp === 'down' || h.db === 'down' || rtpPct >= 80
  if (!hasAlert) return null
  return (
    <div style={{ background: '#fef2f2', border: '1px solid #fecaca', borderRadius: 'var(--radius)', padding: 12 }}>
      <div style={{ fontWeight: 600, color: 'var(--danger)', marginBottom: 4, display: 'flex', alignItems: 'center' }}>
        알람
        <a href="/dashboard/alerts" style={{ marginLeft: 'auto', fontSize: 12, fontWeight: 500, color: 'var(--danger)' }}>이력 보기 →</a>
      </div>
      {h.csp === 'down' && <div>CSP 프로세스 응답 없음</div>}
      {h.cmp === 'down' && <div>CMP 프로세스 응답 없음</div>}
      {h.db === 'down' && <div>DB 연결 끊김</div>}
      {rtpPct >= 80 && <div>RTP 포트 사용률 {rtpPct}% (80% 초과)</div>}
    </div>
  )
}

export const alertBannerWidget: WidgetDef = {
  id: 'cims.alert-banner',
  title: '알람 배너',
  category: 'event',
  component: AlertBannerWidget,
  defaultSize: { w: 12 },
}
