import { useCallback, useEffect, useState } from 'react'
import { deploymentApi, type Agent, type AgentHealthCheck } from '../api/deployment'
import Modal from './Modal'

// 단일 agent 의 on-demand 점검 결과 패널 (모달 wrapper 없음 — 재사용 단위).
// agent sync REST (/health-check) 를 csc admin proxy 로 호출.
function HealthCheckPanel({ agent }: { agent: Agent }) {
  const [data, setData] = useState<AgentHealthCheck | null>(null)
  const [err,  setErr]  = useState<string>('')
  const [loading, setLoading] = useState(true)

  const refresh = useCallback(async () => {
    setLoading(true); setErr(''); setData(null)
    try {
      const r = await deploymentApi.healthCheck(agent.id, 'all')
      setData(r)
    } catch (e) { setErr((e as Error).message) }
    finally { setLoading(false) }
  }, [agent.id])

  useEffect(() => { void refresh() }, [refresh])

  const verdictColor = data?.verdict === 'healthy' ? '#27ae60'
                    : data?.verdict === 'partial' ? '#f39c12'
                    : data?.verdict === 'broken'  ? '#e74c3c'
                    : '#888'
  const verdictLabel = data?.verdict === 'healthy' ? '🟢 healthy'
                    : data?.verdict === 'partial' ? '🟡 partial'
                    : data?.verdict === 'broken'  ? '🔴 broken'
                    : ''

  return (
    <div>
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 12 }}>
        <span style={{ fontWeight: 700 }}>{agent.name}</span>
        {data && (
          <span style={{ background: verdictColor, color: '#fff', padding: '4px 10px',
                         borderRadius: 4, fontWeight: 600 }}>{verdictLabel}</span>
        )}
        {data?.ts && <span style={{ fontSize: 12, color: '#666' }}>ts: {data.ts}</span>}
        <button className="btn btn--sm btn--outline" onClick={() => void refresh()}
                style={{ marginLeft: 'auto' }}>↻ 새로고침</button>
      </div>
      {loading && <div className="empty">점검 중...</div>}
      {err && <div style={{ color: '#e74c3c' }}>※ {err}</div>}
      {data && (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
          {/* issues */}
          {data.issues.length > 0 && (
            <div style={{ background: 'var(--warn-soft)', border: '1px solid #ffeaa7',
                          padding: 10, borderRadius: 4, fontSize: 13 }}>
              <b>⚠ 발견된 이슈:</b>
              <ul style={{ margin: '4px 0 0 20px', padding: 0 }}>
                {data.issues.map((it, i) => <li key={i}>{it}</li>)}
              </ul>
            </div>
          )}
          {/* HA */}
          {data.ha && (
            <div>
              <div style={{ fontWeight: 600, marginBottom: 4 }}>HA (keepalived)</div>
              <div style={{ fontSize: 13, color: '#444' }}>
                {data.ha.keepalived_installed
                  ? (data.ha.keepalived_active
                      ? <>✓ <code>keepalived</code> active</>
                      : <span style={{ color: '#e74c3c' }}>✗ keepalived installed but inactive</span>)
                  : <span style={{ color: '#888' }}>(keepalived 미설치)</span>}
              </div>
              {data.ha.vips.length > 0 && (
                <div style={{ fontSize: 12, color: '#666', marginTop: 4 }}>
                  VIP: {data.ha.vips.map(v => `${v.iface}:${v.ip}/${v.mask}`).join(', ')}
                </div>
              )}
              {data.ha.journal_tail && data.ha.journal_tail.length > 0 && (
                <details style={{ marginTop: 6 }}>
                  <summary style={{ cursor: 'pointer', fontSize: 12, color: '#888' }}>
                    journal tail ({data.ha.journal_tail.length} lines)
                  </summary>
                  <pre style={{ background: '#0d1117', color: '#c9d1d9', padding: 8,
                                fontSize: 11, marginTop: 4, maxHeight: 200, overflow: 'auto' }}>
                    {data.ha.journal_tail.join('\n')}
                  </pre>
                </details>
              )}
            </div>
          )}
          {/* Modules */}
          {data.modules && (
            <div>
              <div style={{ fontWeight: 600, marginBottom: 4 }}>모듈</div>
              <table className="data-table" style={{ fontSize: 12 }}>
                <thead>
                  <tr><th>이름</th><th>실행</th><th>pid</th><th>CPU%</th><th>RSS(MB)</th><th>uptime</th></tr>
                </thead>
                <tbody>
                  {data.modules.map(m => (
                    <tr key={m.name}>
                      <td><b>{m.name}</b></td>
                      <td>{m.running
                        ? <span style={{ color: '#27ae60' }}>✓</span>
                        : <span style={{ color: '#888' }}>—</span>}</td>
                      <td>{m.pid ?? '—'}</td>
                      <td>{m.cpu_pct ?? '—'}</td>
                      <td>{m.mem_mb ?? '—'}</td>
                      <td>{m.uptime_sec != null ? `${Math.floor(m.uptime_sec/60)}m` : '—'}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
          {/* Metrics */}
          {data.metrics && (
            <div>
              <div style={{ fontWeight: 600, marginBottom: 4 }}>시스템 메트릭</div>
              <div style={{ fontSize: 13, color: '#444' }}>
                MEM: {data.metrics.mem_pct ?? '—'}% &nbsp;·&nbsp;
                Disk: {data.metrics.disk_pct ?? '—'}% &nbsp;·&nbsp;
                Load: {data.metrics.load_avg ?? '—'}
              </div>
              {data.metrics.per_iface && data.metrics.per_iface.length > 0 && (
                <table className="data-table" style={{ fontSize: 12, marginTop: 6 }}>
                  <thead>
                    <tr><th>iface</th><th>RX rate</th><th>TX rate</th><th>RX errors</th><th>TX errors</th></tr>
                  </thead>
                  <tbody>
                    {data.metrics.per_iface.map(i => (
                      <tr key={i.name}>
                        <td><b>{i.name}</b></td>
                        <td>{i.rx_rate != null ? `${(i.rx_rate/1024).toFixed(1)} KB/s` : '—'}</td>
                        <td>{i.tx_rate != null ? `${(i.tx_rate/1024).toFixed(1)} KB/s` : '—'}</td>
                        <td>{i.rx_errors}</td>
                        <td>{i.tx_errors}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              )}
            </div>
          )}
          {data.agent_version && (
            <div style={{ fontSize: 11, color: '#888' }}>
              agent v{data.agent_version} · {data.hostname}
            </div>
          )}
        </div>
      )}
    </div>
  )
}

// 1개(서버 단위) 또는 N개(HA 그룹 멤버) agent 를 한 모달에 점검.
export default function HealthCheckModal({ agents, onClose }: { agents: Agent[]; onClose: () => void }) {
  const title = agents.length === 1
    ? `${agents[0].name} — 실시간 점검 (sync REST)`
    : `그룹 점검 — ${agents.map(a => a.name).join(', ')}`
  return (
    <Modal title={title} onClose={onClose} width={agents.length > 1 ? 760 : 720}>
      <div style={{ display: 'flex', flexDirection: 'column', gap: 20 }}>
        {agents.map((a, i) => (
          <div key={a.id} style={i > 0 ? { borderTop: '1px solid var(--border, #e0e0e0)', paddingTop: 16 } : undefined}>
            <HealthCheckPanel agent={a} />
          </div>
        ))}
      </div>
      <div className="modal-footer" style={{ marginTop: 16 }}>
        <button className="btn btn--primary" onClick={onClose}>닫기</button>
      </div>
    </Modal>
  )
}
