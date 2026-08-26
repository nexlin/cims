// CIMS 위젯 — PTT 활성 그룹 테이블 (행 클릭 → 메시지 플로우, 발신자 클릭 → 가입자 상세).
import { useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useSharedHealth } from '@core/widgets/useSharedHealth'
import type { WidgetDef } from '@core/widgets/types'
import FlowPage from '@core/pages/FlowPage'
import { fmtTime } from './shared'

function ActivePttWidget() {
  const navigate = useNavigate()
  const { data } = useSharedHealth()
  const [flowId, setFlowId] = useState<string | null>(null)

  const gotoSubscriber = (e: React.MouseEvent, msisdn: string) => {
    e.stopPropagation(); e.preventDefault()
    if (msisdn) navigate(`/service/status?q=${encodeURIComponent(msisdn)}`)
  }
  const rows = data?.active_ptt ?? []
  return (
    <div className="panel">
      <div style={{ padding: '12px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)' }}>
        PTT 활성 그룹 ({rows.length}건)
      </div>
      {rows.length === 0 ? <div className="empty">활성 그룹 세션 없음</div> : (
      <table className="data-table">
        <thead><tr><th>그룹</th><th>발신자</th><th>상태</th><th>시작</th></tr></thead>
        <tbody>
          {rows.map(c => (
            <tr key={c.call_id} style={{ cursor: 'pointer' }}
              onClick={() => setFlowId(c.call_id)}
              title="행 클릭: 메시지 플로우 / 번호 클릭: 가입자 상세">
              <td>{c.group_id}</td>
              <td><a href="#" onClick={e => gotoSubscriber(e, c.initiator)}>{c.initiator}</a></td>
              <td><span className="badge badge--green">{c.state}</span></td>
              <td className="ts">{fmtTime(c.invite_time)}</td>
            </tr>
          ))}
        </tbody>
      </table>
      )}
      {flowId && <FlowPage callId={flowId} callType="ptt" onClose={() => setFlowId(null)} />}
    </div>
  )
}

export const activePttWidget: WidgetDef = {
  id: 'cims.active-ptt',
  apis: ['stats.health'],
  title: 'PTT 활성 그룹',
  category: 'service',
  component: ActivePttWidget,
  defaultSize: { w: 12 },
}
