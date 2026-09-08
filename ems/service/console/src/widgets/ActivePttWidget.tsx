// CIMS 위젯 — PTT 활성 그룹 테이블 (행 클릭 → 메시지 플로우, 발신자 클릭 → 가입자 상세).
import { useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useSharedHealth } from '@core/widgets/useSharedHealth'
import type { WidgetDef } from '@core/widgets/types'
import FlowPage from '@core/pages/FlowPage'
import { fmtTime } from './shared'
import { DataTable, Th, Td } from '@core/components/custom/data-table'
import { Badge } from '@core/components/ui/badge'

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
      <DataTable sticky>
        <thead><tr><Th>그룹</Th><Th>발신자</Th><Th>상태</Th><Th>시작</Th></tr></thead>
        <tbody>
          {rows.map(c => (
            <tr key={c.call_id} style={{ cursor: 'pointer' }}
              onClick={() => setFlowId(c.call_id)}
              title="행 클릭: 메시지 플로우 / 번호 클릭: 가입자 상세">
              <Td>{c.group_id}</Td>
              <Td><a href="#" onClick={e => gotoSubscriber(e, c.initiator)}>{c.initiator}</a></Td>
              <Td><Badge variant="successSoft" >{c.state}</Badge></Td>
              <Td className="ts">{fmtTime(c.invite_time)}</Td>
            </tr>
          ))}
        </tbody>
      </DataTable>
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
