// CIMS 위젯 — VoIP 활성 통화 테이블 (행 클릭 → 메시지 플로우, 번호 클릭 → 가입자 상세).
import { useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useSharedHealth } from '@core/widgets/useSharedHealth'
import type { WidgetDef } from '@core/widgets/types'
import FlowPage from '@core/pages/FlowPage'
import { fmtTime } from './shared'

function ActiveVoipWidget() {
  const navigate = useNavigate()
  const { data } = useSharedHealth()
  const [flowId, setFlowId] = useState<string | null>(null)

  const gotoSubscriber = (e: React.MouseEvent, msisdn: string) => {
    e.stopPropagation(); e.preventDefault()
    if (msisdn) navigate(`/service/status?q=${encodeURIComponent(msisdn)}`)
  }
  const rows = data?.active_voip ?? []
  return (
    <div className="panel">
      <div style={{ padding: '12px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)' }}>
        VoIP 활성 통화 ({rows.length}건)
      </div>
      {rows.length === 0 ? <div className="empty">활성 통화 없음</div> : (
      <table className="data-table">
        <thead><tr><th>발신</th><th>착신</th><th>상태</th><th>시작</th></tr></thead>
        <tbody>
          {rows.map(c => (
            <tr key={c.call_id} style={{ cursor: 'pointer' }}
              onClick={() => setFlowId(c.call_id)}
              title="행 클릭: 메시지 플로우 / 번호 클릭: 가입자 상세">
              <td><a href="#" onClick={e => gotoSubscriber(e, c.initiator)}>{c.initiator}</a></td>
              <td><a href="#" onClick={e => gotoSubscriber(e, c.callee)}>{c.callee}</a></td>
              <td><span className={`badge ${c.state === 'active' ? 'badge--green' : 'badge--blue'}`}>{c.state}</span></td>
              <td className="ts">{fmtTime(c.invite_time)}</td>
            </tr>
          ))}
        </tbody>
      </table>
      )}
      {flowId && <FlowPage callId={flowId} callType="volte" onClose={() => setFlowId(null)} />}
    </div>
  )
}

export const activeVoipWidget: WidgetDef = {
  id: 'cims.active-voip',
  apis: ['stats.health'],
  title: 'VoIP 활성 통화',
  category: 'service',
  component: ActiveVoipWidget,
  defaultSize: { w: 12 },
}
