import { useEffect, useState } from 'react'
import { api } from '../api/client'

/**
 * 관리 store 소유권(리스) 미보유 상태 배너.
 *
 * 관리평면 이중화에서 OAM 은 단일 writer 다(docs/design/features/oam_ha.md §4.4). 소유권이
 * 없으면 **조회는 되고 변경만 409(`not_lease_owner`)로 거부**되는데, 배너가 없으면 운영자는
 * "저장이 안 되는 이유" 를 알 수 없다. 상태는 base 평면 상태 응답(`/gateway/health`)에서 읽는다.
 *
 * 표시 조건: `read_only === true`. 정상 상태에서는 아무것도 렌더하지 않는다(레이아웃 무영향).
 */
type LeaseState = {
  read_only?: boolean
  lease?: { active?: boolean; reason?: string; node_id?: string; epoch?: number; lost_at?: string | null }
  /** agent 가 기동 실패 후 **직전 정상 설정으로 되돌린** 시각(ISO). 정상이면 없음. */
  config_rolled_back?: string | null
}

const POLL_MS = 15000

export default function ReadOnlyBanner() {
  const [st, setSt] = useState<LeaseState | null>(null)

  useEffect(() => {
    let alive = true
    const tick = async () => {
      try {
        const r = await api.get<LeaseState>('/gateway/health')
        if (alive) setSt(r)
      } catch {
        // 조회 실패는 배너를 띄우지 않는다 — 네트워크 순단으로 오탐하지 않게.
        if (alive) setSt(null)
      }
    }
    void tick()
    const h = setInterval(tick, POLL_MS)
    return () => { alive = false; clearInterval(h) }
  }, [])

  // 설정 자가 복구 알림 — 저장한 설정으로 기동에 실패해 직전 값으로 되돌아간 상태.
  // 조용히 넘어가면 운영자는 자기 설정이 적용된 줄 안다.
  if (st?.config_rolled_back) {
    return (
      <div role="alert" style={{
        background: '#7c2d12', color: '#fff', padding: '8px 16px',
        fontSize: 13, lineHeight: 1.5, display: 'flex', gap: 12, alignItems: 'baseline',
      }}>
        <strong style={{ whiteSpace: 'nowrap' }}>설정 되돌림</strong>
        <span>
          방금 저장한 설정으로는 OAM 이 기동하지 못해 <b>직전 정상 설정으로 되돌렸습니다</b>
          ({st.config_rolled_back}). 실패한 설정은 서버에 <code>config.json.failed-*</code> 로
          보관돼 있습니다. 값을 고쳐 다시 적용하세요.
        </span>
      </div>
    )
  }

  if (!st?.read_only) return null
  const reason = st.lease?.reason || 'unknown'
  const detail = st.lease?.lost_at
    ? `소유권 상실 ${st.lease.lost_at}`
    : `node=${st.lease?.node_id || '?'} epoch=${st.lease?.epoch ?? '?'}`

  return (
    <div role="alert" style={{
      background: '#7f1d1d', color: '#fff', padding: '8px 16px',
      fontSize: 13, lineHeight: 1.5, display: 'flex', gap: 12, alignItems: 'baseline',
    }}>
      <strong style={{ whiteSpace: 'nowrap' }}>읽기 전용</strong>
      <span>
        이 OAM 은 관리 데이터의 소유권(리스)을 갖고 있지 않아 <b>변경이 거부됩니다</b>.
        조회는 정상입니다. 다른 노드가 Active 이거나(절체 중), 같은 노드에서 OAM 이 이중
        기동되었을 수 있습니다. — <code>{reason}</code> · {detail}
      </span>
    </div>
  )
}
