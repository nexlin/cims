// CIMS 위젯 — 활성 알람 (표준 알람 스트림 /alerts). severity/소스/승인 표시. 정상이면 "활성 알람 없음".
// 알람 표준화(X.733/32.111) P0/P1 을 대시보드에 노출.
import { useState, useEffect, useCallback } from 'react'
import { useNavigate } from 'react-router-dom'
import { alertsApi, type AlertEvent } from '@core/api/alerts'
import type { WidgetDef } from '@core/widgets/types'

const SEV_RANK: Record<string, number> = { critical: 0, major: 1, minor: 2, warning: 3, indeterminate: 4 }
const SEV_COLOR: Record<string, string> = {
  critical: '#e74c3c', major: '#e67e22', minor: '#f39c12', warning: '#eab308', indeterminate: '#3498db',
}
const CLASS_LABEL: Record<string, string> = {
  process_down: '프로세스 다운', service_unresponsive: '서비스 무응답',
  connection_lost: '연결 끊김', threshold_crossed: '임계 초과',
  csp_down: '프로세스 다운', cmp_down: '프로세스 다운', module_down: '프로세스 다운',
  db_down: '연결 끊김', rtp_high: '임계 초과', disk_high: '임계 초과',
}

export interface ActiveAlarm extends AlertEvent { acked?: boolean; ackUser?: string }

// 이벤트 스트림 → 현재 활성(open, close 없음) 알람 + 승인 상태. akey=alarm_id(occurrence epoch 제거)/type.
// AlertBannerWidget 도 같은 판정을 공유한다 (알람 스트림 단일 소스).
export function computeActive(events: AlertEvent[]): ActiveAlarm[] {
  const asc = [...events].sort((a, b) => (a.ts || '').localeCompare(b.ts || ''))
  const open: Record<string, ActiveAlarm> = {}
  for (const ev of asc) {
    const k = ev.alarm_id ? ev.alarm_id.replace(/@\d+$/, '') : ev.type
    if (ev.action === 'open') open[k] = { ...ev }
    else if (ev.action === 'ack') { if (open[k]) { open[k].acked = true; open[k].ackUser = ev.ack_user } }
    else if (ev.action === 'change') {   // severity 변경 — 활성 유지, 현재값 갱신 (ack 보존)
      const o = open[k]
      if (o) Object.assign(o, ev, { action: 'open', acked: o.acked, ackUser: o.ackUser })
    }
    else if (ev.action === 'close') delete open[k]
  }
  return Object.values(open).sort((a, b) => {
    const ra = SEV_RANK[a.perceived_severity || a.severity || ''] ?? 5
    const rb = SEV_RANK[b.perceived_severity || b.severity || ''] ?? 5
    return ra - rb || (b.ts || '').localeCompare(a.ts || '')
  })
}

function ActiveAlarmsWidget() {
  const navigate = useNavigate()
  const [events, setEvents] = useState<AlertEvent[]>([])
  const [loaded, setLoaded] = useState(false)

  const load = useCallback(() => {
    alertsApi.list({ days: 7, limit: 1000 })
      .then(r => { setEvents(r.events); setLoaded(true) })
      .catch(() => setLoaded(true))
  }, [])
  useEffect(() => { load(); const iv = setInterval(load, 15000); return () => clearInterval(iv) }, [load])

  const active = computeActive(events)

  return (
    <div className="panel">
      <div style={{ padding: '12px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)',
                    display: 'flex', alignItems: 'center', gap: 8 }}>
        활성 알람 ({active.length})
        <a href="#" onClick={e => { e.preventDefault(); navigate('/alerts/history') }}
           style={{ marginLeft: 'auto', fontSize: 12, fontWeight: 500 }}>이력 →</a>
      </div>
      {!loaded ? (
        <div className="empty">로딩 중…</div>
      ) : active.length === 0 ? (
        <div className="empty" style={{ color: 'var(--success, #16a34a)' }}>✓ 활성 알람 없음</div>
      ) : (
        <table className="data-table" style={{ fontSize: 13 }}>
          <thead><tr><th style={{ width: 70 }}>심각도</th><th style={{ width: 110 }}>클래스</th><th style={{ width: 150 }}>소스</th><th>메시지</th><th style={{ width: 90 }}>승인</th></tr></thead>
          <tbody>
            {active.map((a, i) => {
              const sev = a.perceived_severity || a.severity || 'warning'
              return (
                <tr key={`${a.alarm_id || a.type}-${i}`}>
                  <td><span style={{ display: 'inline-flex', alignItems: 'center', gap: 5 }}>
                    <span style={{ width: 9, height: 9, borderRadius: '50%', background: SEV_COLOR[sev] || '#888', display: 'inline-block' }} />
                    <span style={{ fontSize: 11 }}>{sev}</span></span></td>
                  <td>{CLASS_LABEL[a.type] || a.type}</td>
                  <td><code style={{ fontSize: 11 }}>{a.source?.mo_instance || '-'}</code></td>
                  <td>{a.message}</td>
                  <td>{a.acked
                    ? <span style={{ fontSize: 11, color: 'var(--success, #16a34a)' }}>✓ {a.ackUser || ''}</span>
                    : <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>미승인</span>}</td>
                </tr>
              )
            })}
          </tbody>
        </table>
      )}
    </div>
  )
}

export const activeAlarmsWidget: WidgetDef = {
  id: 'cims.active-alarms',
  title: '활성 알람',
  category: 'event',
  component: ActiveAlarmsWidget,
  defaultSize: { w: 12 },
}
