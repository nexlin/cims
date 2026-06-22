// CIMS 위젯 — KPI 카드 행 (가입자/VoLTE·PTT 번호+등록/활성호/그룹/RTP VoIP·PTT) + sparkline.
// 등록수 = csp.{volte,ptt}_registered (현재 SIP REGISTER 단말; DB register_time 기반). 번호수 = 프로비저닝 총수.
// 카드는 auto-fit 그리드(좁아지면 wrap)로 축소 — 높이 최소화.
import { useSharedHealth } from '@core/widgets/useSharedHealth'
import type { WidgetDef } from '@core/widgets/types'
import { Sparkline } from './shared'

function KpiCard({ label, value, sub, unit, series }: {
  label: string; value: string | number; sub?: string; unit?: string; series?: number[]
}) {
  return (
    <div style={{ background: 'var(--surface)', border: '1px solid var(--border)',
                  borderRadius: 'var(--radius)', padding: '10px 12px', textAlign: 'center' }}>
      <div style={{ fontSize: 11, color: 'var(--text-muted)', marginBottom: 2 }}>{label}</div>
      <div style={{ fontSize: 20, fontWeight: 700, lineHeight: 1.15 }}>
        {value}
        {unit && <span style={{ fontSize: 11, color: 'var(--text-muted)', marginLeft: 3 }}>{unit}</span>}
      </div>
      {sub && <div style={{ fontSize: 10, color: 'var(--text-muted)', marginTop: 1 }}>{sub}</div>}
      {series && <Sparkline data={series} height={16} />}
    </div>
  )
}

const CARD_LABELS = ['가입자', 'VoLTE 번호', 'PTT 번호', 'VoIP 활성 호', 'PTT 그룹', 'RTP VoIP', 'RTP PTT']

function KpiSkeleton() {
  return (
    <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(140px, 1fr))', gap: 8 }}>
      {CARD_LABELS.map(l => (
        <div key={l} style={{ background: 'var(--surface)', border: '1px solid var(--border)',
                              borderRadius: 'var(--radius)', padding: '10px 12px', textAlign: 'center' }}>
          <div style={{ fontSize: 11, color: 'var(--text-muted)', marginBottom: 6 }}>{l}</div>
          <div style={{ height: 20, background: 'var(--surface-2)', borderRadius: 4, opacity: 0.6 }} />
        </div>
      ))}
    </div>
  )
}

function KpiWidget() {
  const { data, history } = useSharedHealth()
  if (!data) return <KpiSkeleton />   // 로딩 중 스켈레톤(공간 확보, 팝인 방지)
  const c = data.csp
  const m = data.cmp
  const ptt = m.rtp_ports_ptt
  const pct = (used: number, total: number) => (total > 0 ? Math.round(used / total * 100) : 0)
  return (
    <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(140px, 1fr))', gap: 8 }}>
      <KpiCard label="가입자" value={c.subscribers_total ?? '–'} unit="명" />
      <KpiCard label="VoLTE 번호" value={`${c.volte_registered ?? 0}/${c.volte_numbers ?? 0}`} sub="등록/전체"
        series={history.map(s => s.volte_registered)} />
      <KpiCard label="PTT 번호" value={`${c.ptt_registered ?? 0}/${c.ptt_numbers ?? 0}`} sub="등록/전체"
        series={history.map(s => s.ptt_registered)} />
      <KpiCard label="VoIP 활성 호" value={c.active_calls} unit="건"
        series={history.map(s => s.active_calls)} />
      <KpiCard label="PTT 그룹" value={`${m.groups}/${c.ptt_groups_total ?? 0}`} sub="활성/전체"
        series={history.map(s => s.ptt_groups)} />
      <KpiCard label="RTP VoIP" value={`${m.rtp_ports.used}/${m.rtp_ports.total}`} sub={`사용 ${pct(m.rtp_ports.used, m.rtp_ports.total)}%`}
        series={history.map(s => s.rtp_used)} />
      <KpiCard label="RTP PTT" value={`${ptt?.used ?? 0}/${ptt?.total ?? 0}`} sub={`사용 ${pct(ptt?.used ?? 0, ptt?.total ?? 0)}%`}
        series={history.map(s => s.rtp_ptt_used)} />
    </div>
  )
}

export const kpiWidget: WidgetDef = {
  id: 'cims.kpi',
  title: 'KPI (가입자/번호/호/그룹/RTP)',
  category: 'service',
  component: KpiWidget,
  defaultSize: { w: 12 },
}
