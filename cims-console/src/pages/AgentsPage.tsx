import { useState, useEffect, useCallback } from 'react'
import { deploymentApi, type Agent, type AgentMetric } from '../api/deployment'
import { useToast } from '../components/Toast'

export default function AgentsPage() {
  const { show } = useToast()
  const [items, setItems] = useState<Agent[]>([])
  const [loading, setLoading] = useState(true)

  const [createOpen, setCreateOpen] = useState(false)
  const [newName, setNewName] = useState('')
  const [newNote, setNewNote] = useState('')
  const [lastResult, setLastResult] = useState<{ enrollment_token: string; install_command: string } | null>(null)
  const [copied, setCopied] = useState(false)

  const [metricsOpen, setMetricsOpen] = useState(false)
  const [metrics, setMetrics] = useState<AgentMetric[]>([])
  const [metricsAgent, setMetricsAgent] = useState<Agent | null>(null)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const list = await deploymentApi.listAgents()
      setItems(list)
    } catch (e) { show((e as Error).message, 'err') }
    finally { setLoading(false) }
  }, [show])

  useEffect(() => { void load() }, [load])
  useEffect(() => {
    const iv = setInterval(() => void load(), 10_000)
    return () => clearInterval(iv)
  }, [load])

  async function createAgent() {
    if (!newName) { show('이름 필수', 'err'); return }
    try {
      const r = await deploymentApi.createAgent(newName, newNote)
      setLastResult({ enrollment_token: r.enrollment_token, install_command: r.install_command })
      setNewName(''); setNewNote('')
      await load()
    } catch (e) { show((e as Error).message, 'err') }
  }

  async function approve(a: Agent) {
    try { await deploymentApi.approveAgent(a.id); show(`${a.name} 승인`, 'ok'); await load() }
    catch (e) { show((e as Error).message, 'err') }
  }
  async function revoke(a: Agent) {
    if (!confirm(`${a.name} 세션을 폐기할까요?`)) return
    try { await deploymentApi.revokeAgent(a.id); show(`${a.name} 폐기`, 'ok'); await load() }
    catch (e) { show((e as Error).message, 'err') }
  }
  async function remove(a: Agent) {
    if (!confirm(`Agent "${a.name}" 을 삭제할까요? (배치된 deployment 도 함께 제거됨)`)) return
    try { await deploymentApi.deleteAgent(a.id); show('삭제됨', 'ok'); await load() }
    catch (e) { show((e as Error).message, 'err') }
  }
  async function copyToClipboard(text: string) {
    try {
      if (navigator.clipboard?.writeText) {
        await navigator.clipboard.writeText(text)
      } else {
        const ta = document.createElement('textarea')
        ta.value = text; ta.style.position = 'fixed'; ta.style.opacity = '0'
        document.body.appendChild(ta); ta.select()
        document.execCommand('copy'); document.body.removeChild(ta)
      }
      setCopied(true)
      setTimeout(() => setCopied(false), 1500)
    } catch (e) {
      show(`복사 실패: ${(e as Error).message}`, 'err')
    }
  }

  async function viewMetrics(a: Agent) {
    setMetricsAgent(a); setMetricsOpen(true)
    try { const r = await deploymentApi.agentMetrics(a.id); setMetrics(r.items) }
    catch (e) { show((e as Error).message, 'err') }
  }

  function statusBadge(s: Agent['status']) {
    const map: Record<string, { bg: string; fg: string }> = {
      pending:   { bg: '#f39c12', fg: '#fff' },
      approved:  { bg: '#3498db', fg: '#fff' },
      online:    { bg: '#2ecc71', fg: '#fff' },
      offline:   { bg: '#95a5a6', fg: '#fff' },
      error:     { bg: '#e74c3c', fg: '#fff' },
      revoked:   { bg: '#7f8c8d', fg: '#fff' },
    }
    const c = map[s] || { bg: '#bbb', fg: '#000' }
    return <span className="tag" style={{ background: c.bg, color: c.fg }}>{s}</span>
  }

  function fmtTime(t: string | null) {
    if (!t) return '—'
    const d = new Date(t)
    const delta = Math.floor((Date.now() - d.getTime()) / 1000)
    if (delta < 60)   return `${delta}초 전`
    if (delta < 3600) return `${Math.floor(delta/60)}분 전`
    if (delta < 86400)return `${Math.floor(delta/3600)}시간 전`
    return d.toLocaleDateString('ko-KR')
  }

  return (
    <div>
      <div style={{ marginBottom: 16, display: 'flex', gap: 12, alignItems: 'center' }}>
        <h3 style={{ margin: 0 }}>서버 Agent</h3>
        <span className="text-muted" style={{ fontSize: 13 }}>
          CIMS 배포 대상 서버의 에이전트 목록. Create → install token 획득 → 대상 서버에서 install-agent.sh 실행.
        </span>
        <div style={{ marginLeft: 'auto' }}>
          <button className="btn btn--outline" onClick={() => void load()}>↻</button>{' '}
          <button className="btn btn--primary" onClick={() => setCreateOpen(true)}>＋ Agent 등록</button>
        </div>
      </div>

      {loading ? <div className="empty">로딩 중...</div> : (
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 40 }}>ID</th>
              <th>이름</th>
              <th style={{ width: 100 }}>상태</th>
              <th>Host</th>
              <th>OS</th>
              <th style={{ width: 120 }}>CPU/MEM/Disk</th>
              <th style={{ width: 110 }}>Last HB</th>
              <th style={{ width: 280 }}>작업</th>
            </tr>
          </thead>
          <tbody>
            {items.map(a => (
              <tr key={a.id}>
                <td>{a.id}</td>
                <td>{a.name}</td>
                <td>{statusBadge(a.status)}</td>
                <td style={{ fontSize: 12 }}>
                  {a.hostname || '—'}<br/><span className="text-muted">{a.ip_address}</span>
                </td>
                <td style={{ fontSize: 12 }}>{a.os_info || '—'}</td>
                <td style={{ fontSize: 12 }}>
                  {a.cpu_cores ? `${a.cpu_cores}코어` : '—'} /{' '}
                  {a.memory_mb ? `${Math.round(a.memory_mb/1024)}GB` : '—'} /{' '}
                  {a.disk_gb ? `${a.disk_gb}GB` : '—'}
                </td>
                <td style={{ fontSize: 12 }}>{fmtTime(a.last_heartbeat)}</td>
                <td>
                  {a.status === 'pending' && (
                    <button className="btn btn--sm btn--primary" onClick={() => approve(a)}>승인</button>
                  )}
                  {(a.status === 'online' || a.status === 'offline' || a.status === 'approved') && (
                    <>
                      <button className="btn btn--sm" onClick={() => viewMetrics(a)}>메트릭</button>{' '}
                      <button className="btn btn--sm btn--outline" onClick={() => revoke(a)}>폐기</button>
                    </>
                  )}{' '}
                  <button className="btn btn--sm btn--danger" onClick={() => remove(a)}>삭제</button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}

      {createOpen && (
        <div className="modal-overlay" onClick={() => { setCreateOpen(false); setLastResult(null); setCopied(false) }}>
          <div className="modal-box" style={{ minWidth: 560 }} onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">Agent 등록</span>
              <button className="modal-close" onClick={() => { setCreateOpen(false); setLastResult(null); setCopied(false) }}>✕</button>
            </div>
            <div className="modal-body">
              {!lastResult ? (
                <div className="form-grid">
                  <label>이름 *</label>
                  <input className="form-input" value={newName} placeholder="예: vlt-sig-pri"
                    onChange={e => setNewName(e.target.value)} />
                  <label>메모</label>
                  <input className="form-input" value={newNote}
                    onChange={e => setNewNote(e.target.value)} />
                </div>
              ) : (
                <div>
                  <div style={{ color: '#2ecc71', marginBottom: 10 }}>
                    ✓ Agent 등록됨. 아래 명령을 대상 서버에서 <b>CIMS 운영 계정</b>으로 실행 (sudo 불필요):
                  </div>
                  <div style={{ position: 'relative' }}>
                    <pre style={{
                      background: '#0d1117', color: '#c9d1d9', padding: 12,
                      paddingRight: 88,
                      borderRadius: 4, fontSize: 12, whiteSpace: 'pre-wrap',
                      userSelect: 'text', margin: 0,
                    }}>{lastResult.install_command}</pre>
                    <button
                      className="btn btn--sm btn--outline"
                      style={{ position: 'absolute', top: 8, right: 8 }}
                      onClick={() => copyToClipboard(lastResult.install_command)}
                    >{copied ? '✓ 복사됨' : '📋 복사'}</button>
                  </div>
                  <div style={{ fontSize: 12, color: '#666', marginTop: 8 }}>
                    Enrollment token: <code>{lastResult.enrollment_token}</code>
                    <br/>(이 토큰은 1회용이며 enroll 완료 시 무효화됩니다)
                  </div>
                </div>
              )}
            </div>
            <div className="modal-footer">
              {!lastResult ? (
                <>
                  <button className="btn btn--outline" onClick={() => setCreateOpen(false)}>취소</button>
                  <button className="btn btn--primary" onClick={createAgent}>등록</button>
                </>
              ) : (
                <button className="btn btn--primary" onClick={() => { setCreateOpen(false); setLastResult(null); setCopied(false) }}>닫기</button>
              )}
            </div>
          </div>
        </div>
      )}

      {metricsOpen && metricsAgent && (
        <div className="modal-overlay" onClick={() => setMetricsOpen(false)}>
          <div className="modal-box" style={{ minWidth: 720, maxHeight: '85vh', overflow: 'auto' }}
               onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">{metricsAgent.name} — 리소스 메트릭 (최근 {metrics.length}건)</span>
              <button className="modal-close" onClick={() => setMetricsOpen(false)}>✕</button>
            </div>
            <div className="modal-body">
              <table className="data-table">
                <thead>
                  <tr><th>시각</th><th>CPU%</th><th>MEM%</th><th>Disk%</th><th>Load</th><th>CIMS 프로세스</th></tr>
                </thead>
                <tbody>
                  {metrics.map(m => (
                    <tr key={m.ts}>
                      <td style={{ fontSize: 12 }}>{m.ts}</td>
                      <td>{m.cpu_pct ?? '—'}</td>
                      <td>{m.mem_pct ?? '—'}</td>
                      <td>{m.disk_pct ?? '—'}</td>
                      <td style={{ fontSize: 12 }}>{m.load_avg}</td>
                      <td style={{ fontSize: 12 }}>
                        {m.processes.length === 0 ? '—' : m.processes.map(p => `${p.name}(${p.pid})`).join(', ')}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
            <div className="modal-footer">
              <button className="btn btn--outline" onClick={() => setMetricsOpen(false)}>닫기</button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
