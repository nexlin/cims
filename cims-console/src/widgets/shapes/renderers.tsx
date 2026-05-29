// shape별 순수 렌더러 — shape 데이터만 받아 그린다 (소스/fetch 무관). 테마 토큰 사용.
import type { TimeBarData, KpiData, DistributionData, TableData } from './types'

export function TimeBarChart({ data }: { data: TimeBarData }) {
  const { buckets, unit } = data
  const vals = buckets.map(b => b.value)
  const max = Math.max(...vals, 1)
  const maxH = 180
  if (buckets.length === 0) return <div className="empty">데이터 없음</div>
  return (
    <div>
      <div style={{ display: 'flex', alignItems: 'flex-end', gap: 2, height: maxH, padding: '0 4px' }}>
        {buckets.map((b, i) => (
          <div key={i} style={{ flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
            <div style={{ fontSize: 10, color: 'var(--text-muted)', marginBottom: 2 }}>{b.value > 0 ? b.value : ''}</div>
            <div title={`${b.label}: ${b.value}${unit || ''}`}
                 style={{ width: '100%', maxWidth: 32, height: Math.max(b.value / max * (maxH - 20), 2),
                          background: 'var(--primary)', borderRadius: '2px 2px 0 0' }} />
            <div style={{ fontSize: 10, color: 'var(--text-muted)', marginTop: 2 }}>{String(b.label)}</div>
          </div>
        ))}
      </div>
    </div>
  )
}

export function KpiCards({ data }: { data: KpiData }) {
  return (
    <div style={{ display: 'flex', gap: 12, flexWrap: 'wrap' }}>
      {data.items.map((k, i) => (
        <div key={i} style={{ flex: '1 1 120px', background: 'var(--surface)', border: '1px solid var(--border)',
                              borderRadius: 'var(--radius)', padding: '14px 16px', textAlign: 'center' }}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 4 }}>{k.label}</div>
          <div style={{ fontSize: 24, fontWeight: 700 }}>
            {k.value}<span style={{ fontSize: 12, color: 'var(--text-muted)', marginLeft: 2 }}>{k.unit}</span>
          </div>
        </div>
      ))}
    </div>
  )
}

export function DistributionBars({ data }: { data: DistributionData }) {
  const { items, total } = data
  if (items.length === 0) return <div className="empty">데이터 없음</div>
  return (
    <div>
      {items.slice().sort((a, b) => b.value - a.value).map((it, i) => {
        const pct = total > 0 ? Math.round(it.value / total * 100) : 0
        return (
          <div key={i} style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
            <div style={{ width: 90, fontSize: 13, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{it.label || 'unknown'}</div>
            <div style={{ flex: 1, background: 'var(--surface-2)', borderRadius: 4, height: 18 }}>
              <div style={{ width: `${pct}%`, background: 'var(--primary)', borderRadius: 4, height: 18, minWidth: pct > 0 ? 4 : 0 }} />
            </div>
            <div style={{ width: 72, fontSize: 12, textAlign: 'right', color: 'var(--text-muted)' }}>{it.value} ({pct}%)</div>
          </div>
        )
      })}
    </div>
  )
}

export function KvTable({ data }: { data: TableData }) {
  return (
    <table className="data-table" style={{ fontSize: 12 }}>
      <thead><tr><th>{data.columns[0]}</th><th style={{ width: 90, textAlign: 'right' }}>{data.columns[1]}</th></tr></thead>
      <tbody>
        {data.rows.length === 0 ? <tr><td colSpan={2} className="empty-cell">데이터 없음</td></tr>
          : data.rows.map((r, i) => (
            <tr key={i}><td>{r.key}</td><td style={{ textAlign: 'right', fontWeight: 600 }}>{r.value}</td></tr>
          ))}
      </tbody>
    </table>
  )
}
