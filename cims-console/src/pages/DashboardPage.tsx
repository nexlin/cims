import { useState, useEffect, useCallback } from 'react'
import { statsApi, type HealthResponse } from '../api/stats'

function StatusDot({ status }: { status: string }) {
  const color = status === 'up' ? 'var(--success, #22c55e)' : 'var(--danger)'
  return <span style={{ display: 'inline-block', width: 10, height: 10, borderRadius: '50%', background: color, marginRight: 6 }} />
}

function KpiCard({ label, value, unit }: { label: string; value: string | number; unit?: string }) {
  return (
    <div style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '16px 20px', textAlign: 'center' }}>
      <div style={{ fontSize: 13, color: 'var(--text-muted)', marginBottom: 4 }}>{label}</div>
      <div style={{ fontSize: 28, fontWeight: 700 }}>{value}<span style={{ fontSize: 14, color: 'var(--text-muted)', marginLeft: 4 }}>{unit}</span></div>
    </div>
  )
}

function fmtTime(iso: string | null): string {
  if (!iso) return '-'
  return new Date(iso).toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

export default function DashboardPage() {
  const [data, setData] = useState<HealthResponse | null>(null)
  const [error, setError] = useState('')

  const load = useCallback(async () => {
    try {
      const res = await statsApi.health()
      setData(res)
      setError('')
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
        <KpiCard label="등록 사용자" value={data.csp.registered_users} unit="명" />
        <KpiCard label="VoIP 활성 호" value={data.csp.active_calls} unit="건" />
        <KpiCard label="PTT 그룹 세션" value={data.cmp.groups} unit="건" />
        <KpiCard label="RTP 포트" value={`${data.cmp.rtp_ports.used}/${data.cmp.rtp_ports.total}`} unit={`(${rtpPct}%)`} />
      </div>

      {/* 역할 상태 */}
      <div className="panel" style={{ padding: 16 }}>
        <div style={{ fontWeight: 600, marginBottom: 8, fontSize: 14 }}>CSP 모듈 역할</div>
        <div style={{ display: 'flex', gap: 16 }}>
          {Object.entries(data.csp.roles).map(([k, v]) => (
            <span key={k} className={`badge ${v ? 'badge--green' : 'badge--gray'}`}>
              {k}: {v ? 'ON' : 'OFF'}
            </span>
          ))}
          <span style={{ marginLeft: 'auto' }} className={`badge ${data.cmp.record_enable ? 'badge--blue' : 'badge--gray'}`}>
            녹취: {data.cmp.record_enable ? 'ON' : 'OFF'}
          </span>
        </div>
      </div>

      {/* 알람 */}
      {(h.csp === 'down' || h.cmp === 'down' || h.db === 'down' || rtpPct >= 80) && (
        <div style={{ background: '#fef2f2', border: '1px solid #fecaca', borderRadius: 'var(--radius)', padding: 12 }}>
          <div style={{ fontWeight: 600, color: 'var(--danger)', marginBottom: 4 }}>알람</div>
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
              <tr key={c.call_id}>
                <td>{c.initiator}</td><td>{c.callee}</td>
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
              <tr key={c.call_id}>
                <td>{c.group_id}</td><td>{c.initiator}</td>
                <td><span className="badge badge--green">{c.state}</span></td>
                <td className="ts">{fmtTime(c.invite_time)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}
