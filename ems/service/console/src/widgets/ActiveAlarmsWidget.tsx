// CIMS 위젯 — 활성 알람 (표준 알람 스트림 /alerts). severity/소스/승인 표시. 정상이면 "활성 알람 없음".
// 알람 표준화(X.733/32.111) P0/P1 을 대시보드에 노출. 데이터는 전역 알람 store 구독
// (개별 fetch 없음 — alarm_pipeline.md §8.2 구독 1원화, 접기는 useAlarms.foldActive).
import { useNavigate } from 'react-router-dom'
import { SEV_COLOR, useAlarms } from '@core/widgets/useAlarms'
import type { WidgetDef } from '@core/widgets/types'

const CLASS_LABEL: Record<string, string> = {
  process_down: '프로세스 다운', service_unresponsive: '서비스 무응답',
  connection_lost: '연결 끊김', threshold_crossed: '임계 초과',
  csp_down: '프로세스 다운', cmp_down: '프로세스 다운', module_down: '프로세스 다운',
  db_down: '연결 끊김', rtp_high: '임계 초과', disk_high: '임계 초과',
}

function ActiveAlarmsWidget() {
  const navigate = useNavigate()
  const { active, loaded } = useAlarms()

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
