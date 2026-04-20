import { useState, useEffect, useCallback } from 'react'
import {
  deploymentApi,
  type Agent, type SipPackage, type Deployment, type JobType,
} from '../api/deployment'
import { useToast } from '../components/Toast'

const SERVICE_KINDS = [
  'csp', 'cmp',         // VoLTE 시그널/미디어
  'psp', 'pmp',         // PTT 시그널/미디어
  'isp', 'imp',         // IBCF 시그널/미디어
  'csc',                // 관리/인증 서버
  'console', 'phone',   // 웹 프론트엔드
  'cwrtc',              // WebRTC 게이트웨이
]

export default function DeploymentsPage() {
  const { show } = useToast()
  const [items, setItems] = useState<Deployment[]>([])
  const [agents, setAgents] = useState<Agent[]>([])
  const [packages, setPackages] = useState<SipPackage[]>([])
  const [loading, setLoading] = useState(true)

  const [createOpen, setCreateOpen] = useState(false)
  const [cAgent, setCAgent] = useState<number>(0)
  const [cPackage, setCPackage] = useState<number>(0)
  const [cServiceKind, setCServiceKind] = useState<string>('csp')
  const [cInstallPath, setCInstallPath] = useState<string>('/opt/cims')
  const [cNote, setCNote] = useState<string>('')

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [d, a, p] = await Promise.all([
        deploymentApi.listDeployments(),
        deploymentApi.listAgents(),
        deploymentApi.listPackages(),
      ])
      setItems(d); setAgents(a); setPackages(p)
    } catch (e) { show((e as Error).message, 'err') }
    finally { setLoading(false) }
  }, [show])

  useEffect(() => { void load() }, [load])
  useEffect(() => {
    const iv = setInterval(() => void load(), 10_000)
    return () => clearInterval(iv)
  }, [load])

  async function createDeployment() {
    if (!cAgent || !cPackage) { show('Agent/패키지 선택 필수', 'err'); return }
    try {
      await deploymentApi.createDeployment({
        agent_id: cAgent,
        package_id: cPackage,
        service_kind: cServiceKind,
        install_path: cInstallPath || undefined,
        note: cNote || undefined,
      })
      show('Deployment 생성됨', 'ok')
      setCreateOpen(false)
      setCAgent(0); setCPackage(0); setCServiceKind('csp')
      setCInstallPath('/opt/cims'); setCNote('')
      await load()
    } catch (e) { show((e as Error).message, 'err') }
  }

  async function queueJob(d: Deployment, jt: JobType, confirmMsg?: string) {
    if (confirmMsg && !confirm(confirmMsg)) return
    try {
      const r = await deploymentApi.queueJob(d.id, jt)
      show(`Job 큐 등록: ${jt} #${r.job_id}`, 'ok')
      await load()
    } catch (e) { show((e as Error).message, 'err') }
  }

  async function remove(d: Deployment) {
    if (!confirm(`Deployment #${d.id} (${d.agent_name} / ${d.package_name}) 을 제거할까요?`)) return
    try { await deploymentApi.deleteDeployment(d.id); show('삭제됨', 'ok'); await load() }
    catch (e) { show((e as Error).message, 'err') }
  }

  function statusBadge(s: Deployment['status']) {
    const map: Record<string, { bg: string; fg: string }> = {
      pending:   { bg: '#f39c12', fg: '#fff' },
      deploying: { bg: '#3498db', fg: '#fff' },
      running:   { bg: '#2ecc71', fg: '#fff' },
      stopped:   { bg: '#95a5a6', fg: '#fff' },
      failed:    { bg: '#e74c3c', fg: '#fff' },
      removed:   { bg: '#7f8c8d', fg: '#fff' },
    }
    const c = map[s] || { bg: '#bbb', fg: '#000' }
    return <span className="tag" style={{ background: c.bg, color: c.fg }}>{s}</span>
  }

  function agentStatusOf(agentId: number): Agent['status'] | undefined {
    return agents.find(a => a.id === agentId)?.status
  }

  return (
    <div>
      <div style={{ marginBottom: 16, display: 'flex', gap: 12, alignItems: 'center' }}>
        <h3 style={{ margin: 0 }}>배포 관리</h3>
        <span className="text-muted" style={{ fontSize: 13 }}>
          Agent × 패키지 매트릭스. install 후 start/stop/restart 로 프로세스 제어.
        </span>
        <div style={{ marginLeft: 'auto' }}>
          <button className="btn btn--outline" onClick={() => void load()}>↻</button>{' '}
          <button className="btn btn--primary" onClick={() => setCreateOpen(true)}>＋ 배포 생성</button>
        </div>
      </div>

      {loading ? <div className="empty">로딩 중...</div> :
        items.length === 0 ? <div className="empty">등록된 배포 없음</div> : (
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 40 }}>ID</th>
              <th>Agent</th>
              <th>패키지</th>
              <th style={{ width: 100 }}>서비스</th>
              <th style={{ width: 100 }}>상태</th>
              <th>설치 경로</th>
              <th style={{ width: 110 }}>배포 시각</th>
              <th style={{ width: 460 }}>작업</th>
            </tr>
          </thead>
          <tbody>
            {items.map(d => {
              const online = agentStatusOf(d.agent_id) === 'online'
              const canStart  = online && (d.status === 'running' || d.status === 'stopped' || d.status === 'pending')
              const canOps    = online && (d.status === 'running' || d.status === 'stopped')
              return (
                <tr key={d.id}>
                  <td>{d.id}</td>
                  <td>
                    {d.agent_name}{' '}
                    <span className="text-muted" style={{ fontSize: 11 }}>#{d.agent_id}</span>
                  </td>
                  <td>
                    {d.package_name}{' '}
                    <span className="text-muted" style={{ fontSize: 11 }}>{d.package_version}</span>
                  </td>
                  <td style={{ fontSize: 12 }}>{d.service_kind || '—'}</td>
                  <td>{statusBadge(d.status)}</td>
                  <td style={{ fontSize: 12, fontFamily: 'monospace' }}>{d.install_path || '—'}</td>
                  <td style={{ fontSize: 11 }}>{d.deployed_at || '—'}</td>
                  <td>
                    <button className="btn btn--sm" disabled={!online}
                      onClick={() => queueJob(d, 'install', `#${d.id} install 실행할까요?`)}>설치</button>{' '}
                    <button className="btn btn--sm btn--primary" disabled={!canStart}
                      onClick={() => queueJob(d, 'start')}>시작</button>{' '}
                    <button className="btn btn--sm" disabled={!canOps}
                      onClick={() => queueJob(d, 'stop')}>중지</button>{' '}
                    <button className="btn btn--sm" disabled={!canOps}
                      onClick={() => queueJob(d, 'restart')}>재시작</button>{' '}
                    <button className="btn btn--sm btn--outline" disabled={!online}
                      onClick={() => queueJob(d, 'health_check')}>헬스</button>{' '}
                    <button className="btn btn--sm btn--danger"
                      onClick={() => remove(d)}>삭제</button>
                  </td>
                </tr>
              )
            })}
          </tbody>
        </table>
      )}

      {createOpen && (
        <div className="modal-overlay" onClick={() => setCreateOpen(false)}>
          <div className="modal-box" style={{ minWidth: 520 }} onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">배포 생성</span>
              <button className="modal-close" onClick={() => setCreateOpen(false)}>✕</button>
            </div>
            <div className="modal-body">
              <div className="form-grid">
                <label>Agent *</label>
                <select className="form-input" value={cAgent}
                  onChange={e => setCAgent(Number(e.target.value))}>
                  <option value={0}>(선택)</option>
                  {agents.filter(a => a.status !== 'revoked').map(a => (
                    <option key={a.id} value={a.id}>
                      #{a.id} {a.name} [{a.status}] {a.hostname || ''}
                    </option>
                  ))}
                </select>

                <label>패키지 *</label>
                <select className="form-input" value={cPackage}
                  onChange={e => setCPackage(Number(e.target.value))}>
                  <option value={0}>(선택)</option>
                  {packages.map(p => (
                    <option key={p.id} value={p.id}>
                      #{p.id} {p.name} {p.version}
                    </option>
                  ))}
                </select>

                <label>서비스 종류</label>
                <select className="form-input" value={cServiceKind}
                  onChange={e => setCServiceKind(e.target.value)}>
                  {SERVICE_KINDS.map(k => <option key={k} value={k}>{k}</option>)}
                </select>

                <label>설치 경로</label>
                <input className="form-input" value={cInstallPath}
                  onChange={e => setCInstallPath(e.target.value)} />

                <label>메모</label>
                <input className="form-input" value={cNote}
                  onChange={e => setCNote(e.target.value)} />
              </div>
              <div style={{ marginTop: 10, fontSize: 12, color: '#666' }}>
                생성 후 "설치" 버튼으로 install job 을 큐에 등록하면 Agent 가 pull 하여 압축해제합니다.
              </div>
            </div>
            <div className="modal-footer">
              <button className="btn btn--outline" onClick={() => setCreateOpen(false)}>취소</button>
              <button className="btn btn--primary" onClick={createDeployment}>생성</button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
