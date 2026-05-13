import { useState, useEffect, useCallback, useRef } from 'react'
import { useNavigate } from 'react-router-dom'
import { statsApi, type HealthResponse } from '../api/stats'
import FlowPage from './FlowPage'

const HISTORY_MAX = 60  // 5초 × 60 = 5분

function StatusDot({ status }: { status: string }) {
  const color = status === 'up' ? 'var(--success, #22c55e)' : 'var(--danger)'
  return <span style={{ display: 'inline-block', width: 10, height: 10, borderRadius: '50%', background: color, marginRight: 6 }} />
}

function Sparkline({ data, color = 'var(--primary)' }: { data: number[]; color?: string }) {
  if (data.length < 2) return <div style={{ height: 24 }} />
  const w = 140, h = 24, pad = 2
  const max = Math.max(...data, 1)
  const min = Math.min(...data, 0)
  const range = max - min || 1
  const step = (w - pad * 2) / Math.max(data.length - 1, 1)
  const pts = data.map((v, i) => {
    const x = pad + i * step
    const y = pad + (h - pad * 2) * (1 - (v - min) / range)
    return `${x.toFixed(1)},${y.toFixed(1)}`
  }).join(' ')
  return (
    <svg width={w} height={h} style={{ display: 'block', margin: '4px auto 0' }}>
      <polyline points={pts} fill="none" stroke={color} strokeWidth={1.5}
        strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  )
}

function KpiCard({ label, value, unit, series }: { label: string; value: string | number; unit?: string; series?: number[] }) {
  return (
    <div style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '16px 20px', textAlign: 'center' }}>
      <div style={{ fontSize: 13, color: 'var(--text-muted)', marginBottom: 4 }}>{label}</div>
      <div style={{ fontSize: 28, fontWeight: 700 }}>{value}<span style={{ fontSize: 14, color: 'var(--text-muted)', marginLeft: 4 }}>{unit}</span></div>
      {series && <Sparkline data={series} />}
    </div>
  )
}

interface HistorySample {
  registered_users: number
  active_calls: number
  ptt_groups: number
  rtp_used: number
}

function fmtTime(iso: string | null): string {
  if (!iso) return '-'
  return new Date(iso).toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

export default function DashboardPage() {
  const navigate = useNavigate()
  const [data, setData] = useState<HealthResponse | null>(null)
  const [error, setError] = useState('')
  const [flowTarget, setFlowTarget] = useState<{ callId: string; callType: 'volte' | 'ptt' } | null>(null)
  const [history, setHistory] = useState<HistorySample[]>([])
  const historyRef = useRef<HistorySample[]>([])

  function gotoSubscriber(e: React.MouseEvent, msisdn: string) {
    e.stopPropagation()  // 행 클릭 (FlowPage 열기) 와 충돌 방지
    if (!msisdn) return
    navigate(`/service/status?q=${encodeURIComponent(msisdn)}`)
  }

  const load = useCallback(async () => {
    try {
      const res = await statsApi.health()
      setData(res)
      setError('')
      const sample: HistorySample = {
        registered_users: res.csp.registered_users,
        active_calls: res.csp.active_calls,
        ptt_groups: res.cmp.groups,
        rtp_used: res.cmp.rtp_ports.used,
      }
      const next = [...historyRef.current, sample].slice(-HISTORY_MAX)
      historyRef.current = next
      setHistory(next)
    } catch (e: unknown) {
      setError(String(e))
    }
  }, [])

  useEffect(() => {
    load()
    const iv = setInterval(load, 5000)
    return () => clearInterval(iv)
  }, [load])

  if (!data && !error) return <div className="empty">로딩 중...</div>
  if (error && !data) return <div className="empty" style={{ color: 'var(--danger)' }}>오류: {error}</div>
  if (!data) return null

  const h = data.health
  const rtpPct = data.cmp.rtp_ports.total > 0
    ? Math.round(data.cmp.rtp_ports.used / data.cmp.rtp_ports.total * 100) : 0

  return (
    <div className="page">

      {/* 헬스체크 */}
      <div style={{ display: 'flex', gap: 12 }}>
        {[
          { name: 'CSP', status: h.csp },
          { name: 'CMP', status: h.cmp },
          { name: 'DB', status: h.db },
        ].map(s => (
          <div key={s.name} style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '12px 16px', display: 'flex', alignItems: 'center', gap: 8 }}>
            <StatusDot status={s.status} />
            <span style={{ fontWeight: 600 }}>{s.name}</span>
            <span style={{ marginLeft: 'auto', fontSize: 13, color: 'var(--text-muted)' }}>
              {s.status === 'up' ? '정상' : '연결 끊김'}
            </span>
          </div>
        ))}
      </div>

      {/* KPI */}
      <div style={{ display: 'flex', gap: 12 }}>
        <KpiCard label="등록 사용자" value={data.csp.registered_users} unit="명"
          series={history.map(s => s.registered_users)} />
        <KpiCard label="VoIP 활성 호" value={data.csp.active_calls} unit="건"
          series={history.map(s => s.active_calls)} />
        <KpiCard label="PTT 그룹 세션" value={data.cmp.groups} unit="건"
          series={history.map(s => s.ptt_groups)} />
        <KpiCard label="RTP 포트" value={`${data.cmp.rtp_ports.used}/${data.cmp.rtp_ports.total}`} unit={`(${rtpPct}%)`}
          series={history.map(s => s.rtp_used)} />
      </div>

      {/* 역할 + 타이머 설정 */}
      <div className="panel" style={{ padding: 16 }}>
        <div style={{ fontWeight: 600, marginBottom: 8, fontSize: 14 }}>CSP 모듈 역할</div>
        <div style={{ display: 'flex', gap: 16, flexWrap: 'wrap' }}>
          {Object.entries(data.csp.roles).map(([k, v]) => (
            <span key={k} className={`badge ${v ? 'badge--green' : 'badge--gray'}`}>
              {k}: {v ? 'ON' : 'OFF'}
            </span>
          ))}
          <span style={{ marginLeft: 'auto' }} className={`badge ${data.record_enable ? 'badge--blue' : 'badge--gray'}`}>
            녹취: {data.record_enable ? 'ON' : 'OFF'}
          </span>
        </div>
        {data.csp.timeouts && (
          <div style={{ display: 'flex', gap: 16, marginTop: 8, fontSize: 12, color: 'var(--text-muted)' }}>
            <span>등록 만료: {data.csp.timeouts.user_timeout}초</span>
            <span>Stale Call: {data.csp.timeouts.stale_call_timeout}초</span>
            <span>OPTIONS 주기: {data.csp.timeouts.send_options_period || '비활성'}초</span>
            {data.cmp.session_timeout != null && <span>CMP 세션: {data.cmp.session_timeout}초</span>}
          </div>
        )}
      </div>

      {/* 알람 */}
      {(h.csp === 'down' || h.cmp === 'down' || h.db === 'down' || rtpPct >= 80) && (
        <div style={{ background: '#fef2f2', border: '1px solid #fecaca', borderRadius: 'var(--radius)', padding: 12 }}>
          <div style={{ fontWeight: 600, color: 'var(--danger)', marginBottom: 4, display: 'flex', alignItems: 'center' }}>
            알람
            <a href="/dashboard/alerts" style={{ marginLeft: 'auto', fontSize: 12, fontWeight: 500, color: 'var(--danger)' }}>이력 보기 →</a>
          </div>
          {h.csp === 'down' && <div>CSP 프로세스 응답 없음</div>}
          {h.cmp === 'down' && <div>CMP 프로세스 응답 없음</div>}
          {h.db === 'down' && <div>DB 연결 끊김</div>}
          {rtpPct >= 80 && <div>RTP 포트 사용률 {rtpPct}% (80% 초과)</div>}
        </div>
      )}

      {/* VoIP 활성 통화 */}
      <div className="panel">
        <div style={{ padding: '12px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)' }}>
          VoIP 활성 통화 ({data.active_voip.length}건)
        </div>
        <table className="data-table">
          <thead><tr><th>발신</th><th>착신</th><th>상태</th><th>시작</th></tr></thead>
          <tbody>
            {data.active_voip.length === 0 ? (
              <tr><td colSpan={4} className="empty">활성 통화 없음</td></tr>
            ) : data.active_voip.map(c => (
              <tr key={c.call_id}
                style={{ cursor: 'pointer' }}
                onClick={() => setFlowTarget({ callId: c.call_id, callType: 'volte' })}
                title="행 클릭: 메시지 플로우 / 번호 클릭: 가입자 상세"
              >
                <td><a href="#" onClick={e => { e.preventDefault(); gotoSubscriber(e, c.initiator) }}>{c.initiator}</a></td>
                <td><a href="#" onClick={e => { e.preventDefault(); gotoSubscriber(e, c.callee) }}>{c.callee}</a></td>
                <td><span className={`badge ${c.state === 'active' ? 'badge--green' : 'badge--blue'}`}>{c.state}</span></td>
                <td className="ts">{fmtTime(c.invite_time)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {/* PTT 활성 그룹 */}
      <div className="panel">
        <div style={{ padding: '12px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)' }}>
          PTT 활성 그룹 ({data.active_ptt.length}건)
        </div>
        <table className="data-table">
          <thead><tr><th>그룹</th><th>발신자</th><th>상태</th><th>시작</th></tr></thead>
          <tbody>
            {data.active_ptt.length === 0 ? (
              <tr><td colSpan={4} className="empty">활성 그룹 세션 없음</td></tr>
            ) : data.active_ptt.map(c => (
              <tr key={c.call_id}
                style={{ cursor: 'pointer' }}
                onClick={() => setFlowTarget({ callId: c.call_id, callType: 'ptt' })}
                title="행 클릭: 메시지 플로우 / 번호 클릭: 가입자 상세"
              >
                <td>{c.group_id}</td>
                <td><a href="#" onClick={e => { e.preventDefault(); gotoSubscriber(e, c.initiator) }}>{c.initiator}</a></td>
                <td><span className="badge badge--green">{c.state}</span></td>
                <td className="ts">{fmtTime(c.invite_time)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {flowTarget && (
        <FlowPage
          callId={flowTarget.callId}
          callType={flowTarget.callType}
          onClose={() => setFlowTarget(null)}
        />
      )}
    </div>
  )
}
