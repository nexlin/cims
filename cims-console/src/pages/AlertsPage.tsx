import { useState, useEffect, useCallback } from 'react'
import { alertsApi, type AlertEvent } from '../api/alerts'
import { useToast } from '../components/Toast'

const TYPE_LABEL: Record<string, string> = {
  csp_down: 'CSP 다운',
  cmp_down: 'CMP 다운',
  db_down: 'DB 다운',
  rtp_high: 'RTP 포트 사용률',
}

function typeLabel(t: string): string {
  return TYPE_LABEL[t] || t
}

function fmtTime(iso: string): string {
  if (!iso) return '-'
  return new Date(iso).toLocaleString('ko-KR', {
    year: '2-digit', month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit', second: '2-digit',
  })
}

function durationBetween(openTs: string, closeTs: string): string {
  const o = new Date(openTs).getTime()
  const c = new Date(closeTs).getTime()
  if (isNaN(o) || isNaN(c) || c < o) return '-'
  const sec = Math.round((c - o) / 1000)
  if (sec < 60) return `${sec}초`
  if (sec < 3600) return `${Math.floor(sec / 60)}분 ${sec % 60}초`
  return `${Math.floor(sec / 3600)}시간 ${Math.floor((sec % 3600) / 60)}분`
}

interface AlertRow extends AlertEvent {
  resolved_at?: string  // close 이벤트가 있으면 그 시각
  duration?: string
}

/**
 * open/close 페어링: 같은 type 의 가장 가까운 후속 close 를 찾아 매칭.
 * 매칭 안 된 open 은 currently open 으로 표시. close 는 standalone 으로도 표시.
 */
function pairEvents(events: AlertEvent[]): AlertRow[] {
  const sortedAsc = [...events].sort((a, b) => (a.ts || '').localeCompare(b.ts || ''))
  const rows: AlertRow[] = []
  const openByType: Record<string, AlertRow> = {}
  for (const ev of sortedAsc) {
    if (ev.action === 'open') {
      const row: AlertRow = { ...ev }
      rows.push(row)
      openByType[ev.type] = row
    } else {  // close
      const open = openByType[ev.type]
      if (open) {
        open.resolved_at = ev.ts
        open.duration = durationBetween(open.ts, ev.ts)
        delete openByType[ev.type]
      } else {
        // standalone close (open 이 보존 기간 이전이라 못 찾음)
        rows.push({ ...ev })
      }
    }
  }
  return rows.sort((a, b) => (b.ts || '').localeCompare(a.ts || ''))
}

export default function AlertsPage() {
  const { show } = useToast()
  const [events, setEvents] = useState<AlertEvent[]>([])
  const [types, setTypes] = useState<string[]>([])
  const [days, setDays] = useState(7)
  const [filterType, setFilterType] = useState('')
  const [showResolved, setShowResolved] = useState(true)
  const [loading, setLoading] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [list, typeList] = await Promise.all([
        alertsApi.list({ days, type: filterType || undefined, limit: 2000 }),
        alertsApi.types(),
      ])
      setEvents(list.events)
      setTypes(typeList.types)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [days, filterType, show])

  useEffect(() => { load() }, [load])

  const rows = pairEvents(events).filter(r => showResolved || !r.resolved_at)
  const openCount = rows.filter(r => r.action === 'open' && !r.resolved_at).length

  return (
    <div className="page">
      <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
        <span style={{ fontSize: 13, color: 'var(--text-muted)' }}>기간:</span>
        {[1, 7, 30, 90].map(d => (
          <button key={d}
            className={`btn btn--sm ${days === d ? 'btn--primary' : 'btn--ghost'}`}
            onClick={() => setDays(d)}>
            {d === 1 ? '오늘' : `${d}일`}
          </button>
        ))}
        <div style={{ width: 1, height: 24, background: 'var(--border)', margin: '0 8px' }} />
        <span style={{ fontSize: 13, color: 'var(--text-muted)' }}>유형:</span>
        <select className="form-input" value={filterType}
          onChange={e => setFilterType(e.target.value)} style={{ width: 160 }}>
          <option value="">전체</option>
          {types.map(t => <option key={t} value={t}>{typeLabel(t)}</option>)}
        </select>
        <label style={{ display: 'flex', alignItems: 'center', gap: 4, fontSize: 13 }}>
          <input type="checkbox" checked={showResolved}
            onChange={e => setShowResolved(e.target.checked)} />
          해제된 알람 포함
        </label>
        <button className="btn btn--ghost btn--sm" onClick={load} style={{ marginLeft: 'auto' }}>↻</button>
      </div>

      <div style={{ display: 'flex', gap: 12 }}>
        <div style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '12px 16px' }}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>현재 열린 알람</div>
          <div style={{ fontSize: 24, fontWeight: 700, color: openCount > 0 ? 'var(--danger)' : 'var(--text)' }}>{openCount}</div>
        </div>
        <div style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '12px 16px' }}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>최근 {days}일 발생</div>
          <div style={{ fontSize: 24, fontWeight: 700 }}>{rows.filter(r => r.action === 'open').length}</div>
        </div>
      </div>

      <div className="panel">
        <div style={{ padding: '12px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)' }}>
          알람 이력 ({rows.length}건)
        </div>
        {loading ? (
          <div className="empty">로딩 중…</div>
        ) : rows.length === 0 ? (
          <div className="empty">기록된 알람 없음</div>
        ) : (
          <table className="data-table">
            <thead>
              <tr>
                <th style={{ width: 80 }}>심각도</th>
                <th style={{ width: 140 }}>유형</th>
                <th>메시지</th>
                <th style={{ width: 160 }}>발생 시각</th>
                <th style={{ width: 160 }}>해제 시각</th>
                <th style={{ width: 100 }}>지속 시간</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((r, i) => {
                const isOpen = r.action === 'open' && !r.resolved_at
                const sev = r.severity || 'warning'
                const badgeCls = sev === 'critical' ? 'badge--red' : sev === 'warning' ? 'badge--yellow' : 'badge--blue'
                return (
                  <tr key={`${r.ts}-${r.type}-${i}`} style={isOpen ? { background: 'rgba(220, 53, 69, 0.08)' } : undefined}>
                    <td><span className={`badge ${badgeCls}`}>{sev}</span></td>
                    <td>{typeLabel(r.type)}</td>
                    <td>{r.message}{isOpen && <span style={{ marginLeft: 8, color: 'var(--danger)', fontSize: 11, fontWeight: 600 }}>OPEN</span>}</td>
                    <td className="ts">{fmtTime(r.ts)}</td>
                    <td className="ts">{r.resolved_at ? fmtTime(r.resolved_at) : (r.action === 'open' ? '—' : fmtTime(r.ts))}</td>
                    <td>{r.duration || (isOpen ? '진행 중' : '-')}</td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        )}
      </div>
    </div>
  )
}
