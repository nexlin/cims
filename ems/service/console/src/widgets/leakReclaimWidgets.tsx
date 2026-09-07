// 누수 회수(sweeper) 화면 위젯 — CMP sweeper 가 회수한 고아 relay 세션.
//   RtpMap 포트단독키 버그 수정(session_id 키 전환) 후 정상 환경에서는 0 건이 기대값.
//   항목이 있으면 = owner(CSP) 비정상 종료(crash/kill)나 teardown 누락으로 고아가 된 relay 를
//   CMP 안전망(sweeper)이 회수했다는 신호 → 새 누수/장애 추적 단서.
//   reason: orphan_no_rtp(setup 실패/무RTP, OrphanReclaimSec 회수) | hold_timeout(RTP 받았으나
//           REMOVE 미수신 = CSP crash/BYE 누락, SessionTimeout 회수).
//
// **화면 = 카드 하나**(`cims.leak-reclaims`)이고 안의 여섯 블록(조회 조건 · 지표 4 낱개 · 목록)은
// 각각 위젯이라 카드 안 편집으로 재배치할 수 있다(console_platform §3.0.1).
// 지표 4개는 서로 다른 축(총 회수 / 무RTP / RTP후 미해제 / 노드별)이라 낱개다(§3.1).
// 조회는 날짜(페이지 파라미터 `date`)를 키로 공유하므로 블록이 몇 개든 요청은 1회다(makeSharedByKey).
import { type CSSProperties } from 'react'
import { api } from '@core/api/client'
import { InfoDot } from '@core/components/InfoDot'
import { makeCardWidget } from '@core/widgets/CardLayout'
import { GRID_ROWS } from '@core/widgets/gridLayout'
import { makeSharedByKey } from '@core/widgets/sharedFetch'
import { usePageParam, todayIso } from '@core/widgets/pageParams'
import type { WidgetDef, WidgetPlacement } from '@core/widgets/types'
import { RotateCw } from 'lucide-react'

interface ReclaimItem {
  ts: string; node: string; session_id: string; sesid: string
  service: string; reason: string; held_sec: number
}
interface ReclaimResp {
  date: string
  counts: { total: number; orphan_no_rtp: number; hold_timeout: number }
  by_node: Record<string, number>
  items: ReclaimItem[]
}

const REASON_LABEL: Record<string, string> = {
  orphan_no_rtp: '무RTP(setup 실패)',
  hold_timeout: 'RTP수신 후 미해제(CSP crash/BYE 누락)',
}

const useReclaimsRaw = makeSharedByKey<ReclaimResp>(
  date => api.get<ReclaimResp>(`/stats/leak-reclaims?date=${date}`))

function useReclaims() {
  const [date] = usePageParam('date')
  return useReclaimsRaw(date || todayIso())
}

// 지표 카드 — 통계 화면의 지표 카드(shape.stat)와 같은 모양으로 통일.
const CARD: CSSProperties = {
  flex: '1 1 auto', minHeight: 0, display: 'flex', flexDirection: 'column',
  justifyContent: 'center', alignItems: 'center', textAlign: 'center',
}

function CountCard({ label, value, tone, loading, error }: {
  label: string; value: number; tone?: 'ok' | 'warn'; loading?: boolean; error?: string
}) {
  const color = tone === 'warn' ? 'var(--destructive)' : tone === 'ok' ? 'var(--cims-success)' : 'var(--foreground)'
  return (
    <div className="panel" style={{ padding: 10, display: 'flex', flexDirection: 'column' }}>
      <div style={CARD}>
        <div style={{ fontSize: 12, color: 'var(--muted-foreground)', marginBottom: 4 }}>
          {label}{loading && ' ·'}
        </div>
        {error
          ? <div style={{ fontSize: 12, color: 'var(--destructive)' }}>조회 실패</div>
          : <div style={{ fontSize: 24, fontWeight: 700, lineHeight: 1.1, color }}>{value}<span
              style={{ fontSize: 12, color: 'var(--muted-foreground)', marginLeft: 2 }}>건</span></div>}
      </div>
    </div>
  )
}

function TotalBlock() {
  const { data, loading, error } = useReclaims()
  const n = data?.counts.total ?? 0
  return <CountCard label="총 회수" value={n} tone={n > 0 ? 'warn' : 'ok'} loading={loading} error={error} />
}
function OrphanBlock() {
  const { data, loading, error } = useReclaims()
  return <CountCard label="무RTP (setup 실패)" value={data?.counts.orphan_no_rtp ?? 0}
                    loading={loading} error={error} />
}
function HoldBlock() {
  const { data, loading, error } = useReclaims()
  const n = data?.counts.hold_timeout ?? 0
  return <CountCard label="RTP후 미해제 (CSP crash/BYE 누락)" value={n} tone={n > 0 ? 'warn' : undefined}
                    loading={loading} error={error} />
}

// 노드별 회수 — 노드마다 한 행. 어느 미디어 노드에서 누수가 나는지 보는 용도라 표가 맞다.
function ByNodeBlock() {
  const { data, loading, error } = useReclaims()
  const rows = Object.entries(data?.by_node ?? {}).sort((a, b) => b[1] - a[1])
  return (
    <div className="panel" style={{ padding: 12, display: 'flex', flexDirection: 'column', minHeight: 0 }}>
      <div style={{ fontSize: 12, color: 'var(--muted-foreground)', marginBottom: 6, flex: 'none' }}>
        노드별 회수{loading && ' · 갱신 중…'}{error && <span style={{ color: 'var(--destructive)' }}> · 조회 실패</span>}
      </div>
      {rows.length === 0 ? <div className="empty" style={{ fontSize: 12 }}>회수 없음</div> : (
        <div className="scroll-fill">
          <table className="data-table" style={{ fontSize: 12 }}>
            <thead><tr><th>노드</th><th style={{ width: 70, textAlign: 'right' }}>건수</th></tr></thead>
            <tbody>
              {rows.map(([node, n]) => (
                <tr key={node}><td>{node}</td><td style={{ textAlign: 'right' }}>{n}</td></tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}

// 조회 조건 — 날짜(페이지 파라미터 `date`)를 소유한다. 화면의 뜻은 ⓘ 로 접는다
// (0건이 정상이라는 걸 모르면 오해하기 쉬운 화면이라 설명 자체는 남겨 둔다).
function FilterBlock() {
  const [date, setDate] = usePageParam('date')
  const { data, loading, error, reload } = useReclaims()
  const n = data?.counts.total ?? 0
  return (
    <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
      <input type="date" className="form-input" value={date || todayIso()} style={{ width: 150 }}
             onChange={e => setDate(e.target.value)} />
      <button className="btn btn--sm btn--ghost" title="다시 조회" onClick={reload}><RotateCw size={14} /></button>
      <InfoDot label="누수 회수란?">
        CMP sweeper 가 회수한 <b>고아 relay</b> 목록입니다. 정상 운영에서는 <b>0건</b>이 기대값이며,
        항목이 나타나면 CSP 비정상 종료(crash) 또는 teardown 누락으로 누수된 relay 를
        안전망이 회수한 것입니다.
      </InfoDot>
      {loading && <span style={{ fontSize: 12, color: 'var(--muted-foreground)' }}>갱신 중…</span>}
      {error && <span style={{ fontSize: 12, color: 'var(--destructive)' }}>조회 실패</span>}
      <span className="ts" style={{ marginLeft: 'auto' }}>총 {n}건 회수</span>
    </div>
  )
}

function ListBlock() {
  const { data, loading, error } = useReclaims()
  const items = data?.items ?? []
  return (
    <div className="panel" style={{ display: 'flex', flexDirection: 'column', minHeight: 0 }}>
      <div style={{ padding: '10px 16px', fontWeight: 600, fontSize: 14, flex: 'none',
                    borderBottom: '1px solid var(--border)' }}>
        회수 세션 ({items.length}건)
        {loading && <span style={{ fontSize: 11, fontWeight: 400, color: 'var(--muted-foreground)' }}> · 갱신 중…</span>}
        {error && <span style={{ fontSize: 11, fontWeight: 400, color: 'var(--destructive)' }}> · 조회 실패</span>}
      </div>
      <div className="scroll-fill">
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 110 }}>시각</th>
              <th style={{ width: 70 }}>노드</th>
              <th>session_id</th>
              <th>sesid</th>
              <th style={{ width: 70 }}>service</th>
              <th>reason</th>
              <th style={{ width: 80 }}>점유(초)</th>
            </tr>
          </thead>
          <tbody>
            {items.map((it, i) => (
              <tr key={i}>
                <td style={{ fontSize: 12 }}>{it.ts}</td>
                <td style={{ fontSize: 12 }}>{it.node}</td>
                <td style={{ fontSize: 12, fontFamily: 'monospace' }}>{it.session_id}</td>
                <td style={{ fontSize: 11, fontFamily: 'monospace', color: 'var(--muted-foreground)' }}>{it.sesid}</td>
                <td style={{ fontSize: 12 }}>{it.service}</td>
                <td style={{ fontSize: 12 }}>
                  <span style={{ color: it.reason === 'hold_timeout' ? 'var(--destructive)' : 'var(--foreground)' }}>
                    {REASON_LABEL[it.reason] || it.reason}
                  </span>
                </td>
                <td style={{ fontSize: 12, textAlign: 'right' }}>{it.held_sec}</td>
              </tr>
            ))}
            {items.length === 0 && (
              <tr><td colSpan={7} className="empty-cell">회수된 누수 세션 없음 (정상)</td></tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  )
}

const w = (id: string, title: string, component: WidgetDef['component'],
           width: number, h: number): WidgetDef =>
  ({ id, title, category: 'stats', apis: ['stats.leak-reclaims'], component,
     defaultSize: { w: width, h }, adminOnly: true })

// 카드 안 기본 배치 — 지표 4장은 48칸을 12·12·12·12 로 나눠 한 줄에. 세로 합 = GRID_ROWS.
export const LEAK_CARD_LAYOUT: WidgetPlacement[] = [
  { widgetId: 'cims.leak.filter',  x: 0,  y: 0,  w: 48, h: 4 },
  { widgetId: 'cims.leak.total',   x: 0,  y: 4,  w: 12, h: 7 },
  { widgetId: 'cims.leak.orphan',  x: 12, y: 4,  w: 12, h: 7 },
  { widgetId: 'cims.leak.hold',    x: 24, y: 4,  w: 12, h: 7 },
  { widgetId: 'cims.leak.by-node', x: 36, y: 4,  w: 12, h: 7 },
  { widgetId: 'cims.leak.list',    x: 0,  y: 11, w: 48, h: 37 },
]

export const leakCardWidget: WidgetDef = makeCardWidget({
  id: 'cims.leak-reclaims', title: '누수 회수 화면', category: 'stats',
  defaultSize: { w: 12, h: GRID_ROWS }, layout: LEAK_CARD_LAYOUT,
})

export const LEAK_RECLAIM_WIDGETS: WidgetDef[] = [
  leakCardWidget,
  { id: 'cims.leak.filter', title: '누수 회수 — 조회 조건', category: 'control',
    apis: ['stats.leak-reclaims'], component: FilterBlock, defaultSize: { w: 12, h: 4 } },
  w('cims.leak.total', '누수 회수 — 총 회수', TotalBlock, 3, 7),
  w('cims.leak.orphan', '누수 회수 — 무RTP', OrphanBlock, 3, 7),
  w('cims.leak.hold', '누수 회수 — RTP후 미해제', HoldBlock, 3, 7),
  w('cims.leak.by-node', '누수 회수 — 노드별', ByNodeBlock, 3, 7),
  w('cims.leak.list', '누수 회수 — 회수 세션 목록', ListBlock, 12, 26),
]
