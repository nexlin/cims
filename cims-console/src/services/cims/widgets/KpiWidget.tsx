// CIMS 위젯 — KPI 카드 행 (등록 사용자 / VoIP 활성호 / PTT 그룹 / RTP 포트) + sparkline.
import { useSharedHealth } from '../../../widgets/useSharedHealth'
import type { WidgetDef } from '../../../widgets/types'
import { Sparkline } from './shared'

function KpiCard({ label, value, unit, series }: { label: string; value: string | number; unit?: string; series?: number[] }) {
  return (
    <div style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '16px 20px', textAlign: 'center' }}>
      <div style={{ fontSize: 13, color: 'var(--text-muted)', marginBottom: 4 }}>{label}</div>
      <div style={{ fontSize: 28, fontWeight: 700 }}>{value}<span style={{ fontSize: 14, color: 'var(--text-muted)', marginLeft: 4 }}>{unit}</span></div>
      {series && <Sparkline data={series} />}
    </div>
  )
}

function KpiSkeleton() {
  return (
    <div style={{ display: 'flex', gap: 12 }}>
      {['등록 사용자', 'VoIP 활성 호', 'PTT 그룹 세션', 'RTP 포트'].map(l => (
        <div key={l} style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)',
                              borderRadius: 'var(--radius)', padding: '16px 20px', textAlign: 'center' }}>
          <div style={{ fontSize: 13, color: 'var(--text-muted)', marginBottom: 8 }}>{l}</div>
          <div style={{ height: 28, background: 'var(--surface-2)', borderRadius: 4, opacity: 0.6 }} />
        </div>
      ))}
    </div>
  )
}

function KpiWidget() {
  const { data, history } = useSharedHealth()
  if (!data) return <KpiSkeleton />   // 로딩 중 스켈레톤(공간 확보, 팝인 방지)
  const rtpPct = data.cmp.rtp_ports.total > 0
    ? Math.round(data.cmp.rtp_ports.used / data.cmp.rtp_ports.total * 100) : 0
  return (
    <div style={{ display: 'flex', gap: 12 }}>
      <KpiCard label="등록 사용자" value={data.csp.registered_users} unit="명"
        series={history.map(s => s.registered_users)} />
      <KpiCard label="VoIP 활성 호" value={data.csp.active_calls} unit="건"
        series={history.map(s => s.active_calls)} />
      <KpiCard label="PTT 그룹 세션" value={data.cmp.groups} unit="건"
        series={history.map(s => s.ptt_groups)} />
      <KpiCard label="RTP 포트" value={`${data.cmp.rtp_ports.used}/${data.cmp.rtp_ports.total}`} unit={`(${rtpPct}%)`}
        series={history.map(s => s.rtp_used)} />
    </div>
  )
}

export const kpiWidget: WidgetDef = {
  id: 'cims.kpi',
  title: 'KPI (사용자/호/그룹/RTP)',
  category: 'service',
  component: KpiWidget,
  defaultSize: { w: 12 },
}
