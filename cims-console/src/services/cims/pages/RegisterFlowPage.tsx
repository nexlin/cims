import { useState } from 'react'
import { flowApi, type FlowMessage } from '../../../api/flow'
import FlowPage from '../../../pages/FlowPage'

export default function RegisterFlowPage() {
  const today = new Date().toISOString().slice(0, 10)
  const [user, setUser] = useState('')
  const [date, setDate] = useState(today)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [flow, setFlow] = useState<{
    user: string
    date: string
    nodes: Record<string, FlowMessage[]>
  } | null>(null)

  const search = async () => {
    const u = user.trim()
    if (!u) return
    setLoading(true)
    setError(null)
    try {
      const resp = await flowApi.getRegisterFlow(u, date)
      if (resp.error) {
        setError(resp.error)
        setFlow(null)
      } else {
        setFlow(resp)
      }
    } catch (e: unknown) {
      const msg = String(e)
      // 404 = 해당 날짜에 등록 이력 없음
      setError(msg.includes('404') ? `'${u}' 사용자의 등록 이력이 없습니다 (${date})` : msg)
      setFlow(null)
    } finally {
      setLoading(false)
    }
  }

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter') search()
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      {/* ── 툴바 ── */}
      <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
        <span style={{ fontWeight: 600, fontSize: 13, color: 'var(--text, #1a1d2e)', whiteSpace: 'nowrap' }}>
          단말 등록 플로우
        </span>

        <div style={{ display: 'flex', alignItems: 'center', gap: 6, flexWrap: 'wrap' }}>
          <label style={{ fontSize: 12, color: 'var(--text-muted)', whiteSpace: 'nowrap' }}>사용자 ID</label>
          <input
            value={user}
            onChange={e => setUser(e.target.value)}
            onKeyDown={handleKeyDown}
            placeholder="예: 1001"
            style={{
              width: 140,
              padding: '4px 8px',
              borderRadius: 4,
              border: '1px solid var(--border)',
              fontSize: 13,
              fontFamily: 'monospace',
            }}
          />
        </div>

        <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
          <label style={{ fontSize: 12, color: 'var(--text-muted)', whiteSpace: 'nowrap' }}>날짜</label>
          <input
            type="date"
            value={date}
            onChange={e => setDate(e.target.value)}
            style={{
              padding: '4px 8px',
              borderRadius: 4,
              border: '1px solid var(--border)',
              fontSize: 13,
            }}
          />
        </div>

        <button
          className="btn btn--primary btn--sm"
          onClick={search}
          disabled={loading || !user.trim()}
        >
          {loading ? '조회 중…' : '조회'}
        </button>

        {error && (
          <span style={{ fontSize: 12, color: '#e96', marginLeft: 4 }}>{error}</span>
        )}
      </div>

      {/* ── 본문 ── */}
      {!flow && !loading && (
        <div className="empty" style={{ flex: 1 }}>
          <div style={{ textAlign: 'center' }}>
            <div style={{ fontSize: 14, marginBottom: 8 }}>사용자 ID를 입력하고 조회하세요</div>
            <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>
              REGISTER → 401 → REGISTER(Digest) → 200 OK → SUBSCRIBE → NOTIFY → PUBLISH 흐름을 표시합니다
            </div>
          </div>
        </div>
      )}

      {/* ── 플로우 Modal ── */}
      {flow && (
        <FlowPage
          callId={`${flow.user} 등록 플로우 (${flow.date})`}
          date={flow.date}
          onClose={() => setFlow(null)}
          prefetchedNodes={flow.nodes}
        />
      )}
    </div>
  )
}
