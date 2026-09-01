import { useState, useEffect, useCallback } from 'react'
import { api } from '@core/api/client'
import { useToast } from '@core/components/Toast'
import { InfoDot } from '@core/components/InfoDot'

// CMP sweeper 가 회수한 누수(고아) relay 세션 목록.
//   RtpMap 포트단독키 버그 수정(session_id 키 전환) 후 정상 환경에서는 0 건이 기대값.
//   항목이 있으면 = owner(CSP) 비정상 종료(crash/kill)나 teardown 누락으로 고아가 된 relay 를
//   CMP 안전망(sweeper)이 회수했다는 신호 → 새 누수/장애 추적 단서.
//   reason: orphan_no_rtp(setup 실패/무RTP, OrphanReclaimSec 회수) | hold_timeout(RTP 받았으나
//           REMOVE 미수신 = CSP crash/BYE 누락, SessionTimeout 회수).
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

export default function LeakReclaimsPage() {
  const { show } = useToast()
  const [data, setData] = useState<ReclaimResp | null>(null)
  const [date, setDate] = useState(new Date().toISOString().substring(0, 10))
  const [loading, setLoading] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try { setData(await api.get<ReclaimResp>(`/stats/leak-reclaims?date=${date}`)) }
    catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [date, show])

  useEffect(() => { load() }, [load])

  const c = data?.counts
  return (
    <div>
      <div className="toolbar">
        <InfoDot label="누수 회수란?">
          CMP sweeper 가 회수한 <b>고아 relay</b> 목록입니다. 정상 운영에서는 <b>0건</b>이 기대값이며,
          항목이 나타나면 CSP 비정상 종료(crash) 또는 teardown 누락으로 누수된 relay 를
          안전망이 회수한 것입니다.
        </InfoDot>
        <input type="date" className="form-input" value={date} onChange={e => setDate(e.target.value)} style={{ width: 150 }} />
        <button className="btn btn--primary btn--sm" onClick={load}>조회</button>
        {data && <span className="ts" style={{ marginLeft: 'auto' }}>총 {c?.total ?? 0}건 회수</span>}
      </div>

      {loading ? <div className="empty">로딩 중...</div> : data && (
        <>
          <div style={{ display: 'flex', gap: 12, marginBottom: 16 }}>
            <KpiCard label="총 회수" value={c?.total ?? 0} tone={c && c.total > 0 ? 'warn' : 'ok'} />
            <KpiCard label="무RTP (orphan)" value={c?.orphan_no_rtp ?? 0} />
            <KpiCard label="RTP후 미해제 (hold)" value={c?.hold_timeout ?? 0} tone={c && c.hold_timeout > 0 ? 'warn' : undefined} />
            <KpiCard label="노드" value={Object.entries(data.by_node).map(([n, v]) => `${n}:${v}`).join('  ') || '-'} isText />
          </div>

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
              {data.items.map((it, i) => (
                <tr key={i}>
                  <td style={{ fontSize: 12 }}>{it.ts}</td>
                  <td style={{ fontSize: 12 }}>{it.node}</td>
                  <td style={{ fontSize: 12, fontFamily: 'monospace' }}>{it.session_id}</td>
                  <td style={{ fontSize: 11, fontFamily: 'monospace', color: 'var(--text-muted)' }}>{it.sesid}</td>
                  <td style={{ fontSize: 12 }}>{it.service}</td>
                  <td style={{ fontSize: 12 }}>
                    <span style={{ color: it.reason === 'hold_timeout' ? 'var(--danger)' : 'var(--text)' }}>
                      {REASON_LABEL[it.reason] || it.reason}
                    </span>
                  </td>
                  <td style={{ fontSize: 12, textAlign: 'right' }}>{it.held_sec}</td>
                </tr>
              ))}
              {data.items.length === 0 && <tr><td colSpan={7} className="empty-cell">회수된 누수 세션 없음 (정상)</td></tr>}
            </tbody>
          </table>
        </>
      )}
    </div>
  )
}

function KpiCard({ label, value, tone, isText }: { label: string; value: number | string; tone?: 'ok' | 'warn'; isText?: boolean }) {
  const color = tone === 'warn' ? 'var(--danger)' : tone === 'ok' ? 'var(--success)' : 'var(--text)'
  return (
    <div className="panel" style={{ padding: '10px 16px', minWidth: 120 }}>
      <div style={{ fontSize: 11, color: 'var(--text-muted)' }}>{label}</div>
      <div style={{ fontSize: isText ? 13 : 22, fontWeight: 700, color, marginTop: 2 }}>{value}</div>
    </div>
  )
}
