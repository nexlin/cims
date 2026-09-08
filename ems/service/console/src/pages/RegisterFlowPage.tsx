import { useState } from 'react'
import { flowApi, type FlowMessage } from '@core/api/flow'
import FlowPage from '@core/pages/FlowPage'
import { Button } from '@core/components/ui/button'
import { ArrowLeft } from 'lucide-react'
import { EmptyState } from '@core/components/custom/empty-state'

type PageState = 'form' | 'flow'

export default function RegisterFlowPage() {
  const today = new Date().toISOString().slice(0, 10)
  const [user, setUser] = useState('')
  const [date, setDate] = useState(today)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const [pageState, setPageState] = useState<PageState>('form')
  const [flow, setFlow] = useState<{ user: string; date: string; nodes: Record<string, FlowMessage[]> } | null>(null)

  const search = async () => {
    const u = user.trim()
    if (!u) return
    setLoading(true)
    setError(null)
    try {
      const resp = await flowApi.getUserFlow(u, date)
      if (resp.error) {
        setError(resp.error)
      } else {
        setFlow(resp)
        setPageState('flow')
      }
    } catch (e: unknown) {
      const msg = String(e)
      setError(msg.includes('404') ? `'${u}' 사용자의 메세지 이력이 없습니다 (${date})` : msg)
    } finally {
      setLoading(false)
    }
  }

  const backToForm = () => { setFlow(null); setError(null); setPageState('form') }

  return (
    <div className="flex flex-col h-full">
      {/* ── 툴바 ── */}
      <div className="toolbar flex-wrap gap-2">
        <span className="font-semibold text-md text-foreground whitespace-nowrap">
          메세지 이력
        </span>

        {pageState === 'flow' && (
          <Button className="mr-1" onClick={backToForm}><ArrowLeft size={13} /> 검색</Button>
        )}

        {pageState === 'form' && (
          <>
            <div className="flex items-center gap-1.5">
              <label className="text-sm text-muted-foreground whitespace-nowrap">사용자 ID</label>
              <input className="w-[160px] py-1 px-2 rounded-[4px] border border-border text-md font-mono"
                value={user}
                onChange={e => setUser(e.target.value)}
                onKeyDown={e => e.key === 'Enter' && search()}
                placeholder="예: +821000000001"/>
            </div>
            <div className="flex items-center gap-1.5">
              <label className="text-sm text-muted-foreground whitespace-nowrap">날짜</label>
              <input className="py-1 px-2 rounded-[4px] border border-border text-md"
                type="date"
                value={date}
                onChange={e => setDate(e.target.value)}/>
            </div>
            <Button variant="default"
              onClick={search}
              disabled={loading || !user.trim()}
            >
              {loading ? '조회 중…' : '조회'}
            </Button>
          </>
        )}

        {pageState === 'flow' && flow && (
          <span className="text-sm text-muted-foreground">
            {flow.user} / {flow.date}
          </span>
        )}

        {error && (
          <span className="text-sm text-destructive ml-1">{error}</span>
        )}
      </div>

      {/* ── form: 안내 ── */}
      {pageState === 'form' && (
        <EmptyState className="flex-1 justify-center"
                    title="사용자 ID를 입력하고 조회하세요"
                    description="해당 날짜에 이 사용자가 주고받은 모든 메시지 흐름이 표시됩니다 (REGISTER · SUBSCRIBE · PUBLISH · INVITE/BYE 호 처리 · NOTIFY 등)" />
      )}

      {/* ── flow: FlowPage (Modal 없이 페이지 내 바로 표시) ── */}
      {pageState === 'flow' && flow && (
        <FlowPage
          inline
          callId={`${flow.user} 메세지 이력 (${flow.date})`}
          date={flow.date}
          onClose={backToForm}
          prefetchedNodes={flow.nodes}
        />
      )}
    </div>
  )
}
