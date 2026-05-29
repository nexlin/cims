import { useState, useEffect, useCallback } from 'react'
import { alertsApi, type AlertEvent, type AlertSummaryResponse, type AlertRulesResponse } from '../api/alerts'
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
  return formatSec(sec)
}

function formatSec(sec: number): string {
  if (sec < 0) return '-'
  if (sec < 60) return `${sec}초`
  if (sec < 3600) return `${Math.floor(sec / 60)}분 ${sec % 60}초`
  return `${Math.floor(sec / 3600)}시간 ${Math.floor((sec % 3600) / 60)}분`
}

interface AlertRow extends AlertEvent {
  resolved_at?: string  // close 이벤트가 있으면 그 시각
  duration?: string
}

function DailyBars({ data }: { data: { date: string; opens: number }[] }) {
  if (data.length === 0) return <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>—</div>
  const max = Math.max(1, ...data.map(d => d.opens))
  const H = 36
  return (
    <div style={{ display: 'flex', alignItems: 'flex-end', gap: 3, height: H + 14 }}>
      {data.map(d => {
        const h = d.opens > 0 ? Math.max(2, Math.round((d.opens / max) * H)) : 1
        const mmdd = d.date.slice(5).replace('-', '/')
        return (
          <div key={d.date} style={{ flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center', minWidth: 0 }}
            title={`${d.date}: ${d.opens}건`}>
            <div style={{
              width: '100%',
              height: h,
              background: d.opens > 0 ? 'var(--danger)' : 'var(--border)',
              borderRadius: 2,
              opacity: d.opens > 0 ? 0.85 : 0.4,
            }} />
            <div style={{ fontSize: 9, color: 'var(--text-muted)', marginTop: 2, whiteSpace: 'nowrap', overflow: 'hidden' }}>
              {mmdd}
            </div>
          </div>
        )
      })}
    </div>
  )
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
  const [summary, setSummary] = useState<AlertSummaryResponse | null>(null)
  const [rules, setRules] = useState<AlertRulesResponse | null>(null)
  const [days, setDays] = useState(7)
  const [filterType, setFilterType] = useState('')
  const [showResolved, setShowResolved] = useState(true)
  const [loading, setLoading] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [list, typeList, sum] = await Promise.all([
        alertsApi.list({ days, type: filterType || undefined, limit: 2000 }),
        alertsApi.types(),
        alertsApi.summary(days),
      ])
      setEvents(list.events)
      setTypes(typeList.types)
      setSummary(sum)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [days, filterType, show])

  useEffect(() => { load() }, [load])
  // 활성 알림 규칙 — days 와 무관, 1회 로드. 실패해도 이력 화면엔 영향 없음.
  useEffect(() => { alertsApi.rules().then(setRules).catch(() => setRules(null)) }, [])

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
        <div style={{ flex: 2, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '12px 16px' }}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 6 }}>일별 발생량</div>
          <DailyBars data={summary?.daily || []} />
        </div>
      </div>

      {rules && rules.rules.length > 0 && (
        <div className="panel">
          <div style={{ padding: '10px 16px', fontWeight: 600, fontSize: 13, borderBottom: '1px solid var(--border)',
                        display: 'flex', alignItems: 'center', gap: 8 }}>
            알림 규칙 ({rules.rules.length})
            <span style={{ fontSize: 11, fontWeight: 400, color: 'var(--text-muted)' }}>
              점검 주기 {rules.sweep_sec}초 · {rules.editable ? '편집 가능' : '읽기 전용 (oam.json 설정 기반)'}
            </span>
          </div>
          <table className="data-table">
            <thead>
              <tr>
                <th style={{ width: 80 }}>심각도</th>
                <th style={{ width: 160 }}>유형</th>
                <th>대상 지표</th>
                <th style={{ width: 200 }}>발생 조건</th>
              </tr>
            </thead>
            <tbody>
              {rules.rules.map(r => {
                const badgeCls = r.severity === 'critical' ? 'badge--red'
                  : r.severity === 'warning' ? 'badge--yellow' : 'badge--blue'
                return (
                  <tr key={r.type}>
                    <td><span className={`badge ${badgeCls}`}>{r.severity}</span></td>
                    <td>{typeLabel(r.type)}</td>
                    <td>{r.metric}</td>
                    <td style={{ fontFamily: 'monospace', fontSize: 12 }}>
                      {r.condition}
                      {r.threshold != null && (
                        <span style={{ marginLeft: 6, color: 'var(--text-muted)', fontFamily: 'inherit' }}>
                          (threshold {r.threshold}{r.unit || ''})
                        </span>
                      )}
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        </div>
      )}

      {summary && summary.by_type.length > 0 && (
        <div className="panel">
          <div style={{ padding: '10px 16px', fontWeight: 600, fontSize: 13, borderBottom: '1px solid var(--border)' }}>
            유형별 통계 (최근 {summary.days}일)
          </div>
          <table className="data-table">
            <thead>
              <tr>
                <th style={{ width: 160 }}>유형</th>
                <th style={{ width: 80, textAlign: 'right' }}>발생</th>
                <th style={{ width: 80, textAlign: 'right' }}>해제</th>
                <th style={{ width: 100 }}>현재 상태</th>
                <th style={{ width: 140, textAlign: 'right' }}>평균 지속시간</th>
                <th>마지막 이벤트</th>
              </tr>
            </thead>
            <tbody>
              {summary.by_type.map(s => (
                <tr key={s.type}>
                  <td>{typeLabel(s.type)}</td>
                  <td style={{ textAlign: 'right' }}>{s.opens}</td>
                  <td style={{ textAlign: 'right' }}>{s.resolved}</td>
                  <td>
                    {s.currently_open
                      ? <span className="badge badge--red">OPEN</span>
                      : <span style={{ color: 'var(--text-muted)' }}>정상</span>}
                  </td>
                  <td style={{ textAlign: 'right' }}>
                    {s.avg_duration_sec != null ? formatSec(Math.round(s.avg_duration_sec)) : '-'}
                  </td>
                  <td className="ts">{s.last_ts ? fmtTime(s.last_ts) : '-'}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

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
