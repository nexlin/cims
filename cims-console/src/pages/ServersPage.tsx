import { useCallback, useEffect, useMemo, useState } from 'react'
import {
  deploymentApi,
  type Agent, type SipPackage, type Deployment, type JobType, type AgentMetric,
} from '../api/deployment'
import { useToast } from '../components/Toast'
import Modal from '../components/Modal'
import { agentStatusColor, depStatusColor, fmtRelTime } from './deploy/deployHelpers'
import DeploymentConfigModal from './deploy/DeploymentConfigModal'

export default function ServersPage() {
  const { show } = useToast()
  const [agents, setAgents]           = useState<Agent[]>([])
  const [packages, setPackages]       = useState<SipPackage[]>([])
  const [deployments, setDeployments] = useState<Deployment[]>([])
  const [loading, setLoading]         = useState(true)
  const [selectedId, setSelectedId]   = useState<number | null>(null)
  const [filter, setFilter]           = useState('')

  const [agentModalOpen, setAgentModalOpen] = useState(false)
  const [deployModal, setDeployModal]       = useState<{ agent: Agent } | null>(null)
  const [metricsFor, setMetricsFor]         = useState<Agent | null>(null)
  const [configFor, setConfigFor]           = useState<Deployment | null>(null)

  const load = useCallback(async () => {
    try {
      const [a, p, d] = await Promise.all([
        deploymentApi.listAgents(),
        deploymentApi.listPackages(),
        deploymentApi.listDeployments(),
      ])
      setAgents(a); setPackages(p); setDeployments(d)
    } catch (e) { show((e as Error).message, 'err') }
    finally { setLoading(false) }
  }, [show])

  useEffect(() => { void load() }, [load])
  useEffect(() => {
    const iv = setInterval(() => void load(), 10_000)
    return () => clearInterval(iv)
  }, [load])

  const depsByAgent = useMemo(() => {
    const m = new Map<number, Deployment[]>()
    for (const d of deployments) {
      if (!m.has(d.agent_id)) m.set(d.agent_id, [])
      m.get(d.agent_id)!.push(d)
    }
    return m
  }, [deployments])

  const filteredAgents = useMemo(() => {
    const q = filter.trim().toLowerCase()
    if (!q) return agents
    return agents.filter(a =>
      a.name.toLowerCase().includes(q) ||
      (a.hostname || '').toLowerCase().includes(q) ||
      (a.ip_address || '').includes(q)
    )
  }, [agents, filter])

  useEffect(() => {
    if (loading) return
    if (agents.length === 0) { setSelectedId(null); return }
    if (!selectedId || !agents.find(a => a.id === selectedId)) {
      setSelectedId(agents[0].id)
    }
  }, [agents, selectedId, loading])

  const selected = useMemo(
    () => agents.find(a => a.id === selectedId) || null,
    [agents, selectedId]
  )

  const stats = useMemo(() => ({
    total:  agents.length,
    online: agents.filter(a => a.status === 'online').length,
    pending: agents.filter(a => a.status === 'pending').length,
  }), [agents])

  // 액션들
  async function approveAgent(a: Agent) {
    try { await deploymentApi.approveAgent(a.id); show(`${a.name} 승인`, 'ok'); await load() }
    catch (e) { show((e as Error).message, 'err') }
  }
  async function revokeAgent(a: Agent) {
    if (!confirm(`${a.name} 세션을 폐기할까요?`)) return
    try { await deploymentApi.revokeAgent(a.id); show(`${a.name} 폐기`, 'ok'); await load() }
    catch (e) { show((e as Error).message, 'err') }
  }
  async function removeAgent(a: Agent) {
    if (!confirm(`Agent "${a.name}" 을 삭제할까요? 관련 deployment 도 같이 제거됨`)) return
    try { await deploymentApi.deleteAgent(a.id); show('삭제됨', 'ok'); await load() }
    catch (e) { show((e as Error).message, 'err') }
  }
  async function upgradeAgent(a: Agent) {
    if (!confirm(`${a.name} 의 agent 바이너리를 최신 버전으로 업그레이드할까요?\n(agent 가 재기동됩니다)`)) return
    try {
      const r = await deploymentApi.upgradeAgent(a.id)
      show(`업그레이드 job 큐잉 (#${r.job_id})`, 'ok')
      await load()
    } catch (e) { show((e as Error).message, 'err') }
  }
  async function queueJob(d: Deployment, jt: JobType) {
    try {
      const r = await deploymentApi.queueJob(d.id, jt)
      show(`${jt} 큐 등록 (#${r.job_id})`, 'ok')
      await load()
    } catch (e) { show((e as Error).message, 'err') }
  }
  async function removeDeployment(d: Deployment) {
    if (!confirm(`Deployment #${d.id} (${d.package_name}) 을 제거할까요?`)) return
    try { await deploymentApi.deleteDeployment(d.id); show('삭제됨', 'ok'); await load() }
    catch (e) { show((e as Error).message, 'err') }
  }

  if (loading) return <div className="empty">로딩 중...</div>

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 12, height: 'calc(100vh - 120px)' }}>
      {/* 상단 요약 + 액션 */}
      <div style={{ display: 'flex', gap: 10, alignItems: 'center', flexWrap: 'wrap' }}>
        <StatChip label="서버" value={`${stats.online}/${stats.total}`} sub="online" color="#2ecc71" />
        {stats.pending > 0 &&
          <StatChip label="승인대기" value={stats.pending} color="#f39c12" />}
        <input className="form-input" placeholder="이름/호스트/IP 검색..."
          value={filter} onChange={e => setFilter(e.target.value)}
          style={{ width: 240 }} />
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 8 }}>
          <button className="btn btn--outline" onClick={() => void load()}>↻</button>
          <button className="btn btn--primary" onClick={() => setAgentModalOpen(true)}>
            ＋ 서버 등록
          </button>
        </div>
      </div>

      {/* 서버 테이블 */}
      <div style={{
        flex: '0 0 auto', maxHeight: '40%', overflow: 'auto',
        border: '1px solid #e5e5e5', borderRadius: 6, background: '#fff',
      }}>
        {filteredAgents.length === 0 ? (
          <div className="empty" style={{ padding: 20 }}>
            {agents.length === 0 ? '등록된 서버 없음 — "＋ 서버 등록" 으로 추가' : '검색 결과 없음'}
          </div>
        ) : (
          <table className="data-table" style={{ margin: 0 }}>
            <thead>
              <tr>
                <th style={{ width: 14 }}></th>
                <th>이름</th>
                <th>IP</th>
                <th>상태</th>
                <th>호스트</th>
                <th style={{ textAlign: 'right' }}>모듈</th>
                <th>Agent v.</th>
                <th>Heartbeat</th>
              </tr>
            </thead>
            <tbody>
              {filteredAgents.map(a => (
                <ServerRow key={a.id} agent={a}
                  active={a.id === selectedId}
                  depCount={(depsByAgent.get(a.id) || []).length}
                  onClick={() => setSelectedId(a.id)} />
              ))}
            </tbody>
          </table>
        )}
      </div>

      {/* 하단 Inspector */}
      <div style={{
        flex: 1, overflow: 'hidden', display: 'flex', flexDirection: 'column',
        border: '1px solid #e5e5e5', borderRadius: 6, background: '#fff',
      }}>
        {!selected ? (
          <div className="empty" style={{ padding: 40 }}>
            위에서 서버를 선택하세요
          </div>
        ) : (
          <ServerInspector agent={selected}
            deployments={depsByAgent.get(selected.id) || []}
            packages={packages}
            onApprove={approveAgent}
            onRevoke={revokeAgent}
            onRemove={removeAgent}
            onUpgrade={upgradeAgent}
            onMetrics={setMetricsFor}
            onAddDeploy={() => setDeployModal({ agent: selected })}
            onConfigure={setConfigFor}
            onJob={queueJob}
            onRemoveDep={removeDeployment} />
        )}
      </div>

      {agentModalOpen &&
        <AgentCreateModal onClose={() => setAgentModalOpen(false)} onDone={load} />}
      {deployModal &&
        <DeploymentCreateModal agent={deployModal.agent} packages={packages}
          onClose={() => setDeployModal(null)} onDone={load} />}
      {metricsFor &&
        <MetricsModal agent={metricsFor} onClose={() => setMetricsFor(null)} />}
      {configFor &&
        <DeploymentConfigModal deployment={configFor}
          onClose={() => setConfigFor(null)} onDone={load} />}
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  Server table row
// ──────────────────────────────────────────────────────────────

function ServerRow({ agent: a, active, depCount, onClick }: {
  agent: Agent; active: boolean; depCount: number; onClick: () => void
}) {
  const sc = agentStatusColor(a.status)
  return (
    <tr onClick={onClick}
      style={{
        cursor: 'pointer',
        background: active ? '#eef5ff' : undefined,
      }}>
      <td style={{ padding: 0 }}>
        <div style={{ width: 4, background: sc.bar, height: 32 }} />
      </td>
      <td><b>{a.name}</b></td>
      <td style={{ fontSize: 12, color: '#555' }}>{a.ip_address || '—'}</td>
      <td>
        <span className="tag" style={{
          background: sc.bar, color: '#fff', fontSize: 10, padding: '1px 6px', borderRadius: 3,
        }}>{a.status}</span>
      </td>
      <td style={{ fontSize: 12, color: '#666' }}>{a.hostname || '—'}</td>
      <td style={{ textAlign: 'right', fontSize: 12 }}>{depCount}</td>
      <td style={{ fontSize: 12, color: '#888' }}>{a.agent_version || '—'}</td>
      <td style={{ fontSize: 12, color: '#888' }}>{fmtRelTime(a.last_heartbeat)}</td>
    </tr>
  )
}

function StatChip({ label, value, sub, color }:
  { label: string; value: string | number; sub?: string; color?: string }) {
  return (
    <div style={{
      background: '#fff', border: '1px solid #ddd', padding: '8px 14px',
      borderRadius: 6, minWidth: 110, display: 'flex', flexDirection: 'column', gap: 2,
    }}>
      <span style={{ fontSize: 11, color: '#888' }}>{label}</span>
      <span style={{ fontWeight: 600, fontSize: 18 }}>
        <span style={{ color: color || '#333' }}>{value}</span>
        {sub && <span style={{ fontSize: 11, color: '#888', marginLeft: 6 }}>{sub}</span>}
      </span>
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  Inspector (선택된 서버 상세)
// ──────────────────────────────────────────────────────────────

type InspectorTab = 'modules' | 'info'

function ServerInspector({ agent: a, deployments, packages,
                          onApprove, onRevoke, onRemove, onUpgrade, onMetrics,
                          onAddDeploy, onConfigure, onJob, onRemoveDep }: {
  agent: Agent
  deployments: Deployment[]
  packages: SipPackage[]
  onApprove: (a: Agent) => void
  onRevoke: (a: Agent) => void
  onRemove: (a: Agent) => void
  onUpgrade: (a: Agent) => void
  onMetrics: (a: Agent) => void
  onAddDeploy: () => void
  onConfigure: (d: Deployment) => void
  onJob: (d: Deployment, jt: JobType) => void
  onRemoveDep: (d: Deployment) => void
}) {
  const [tab, setTab] = useState<InspectorTab>('modules')
  const sc = agentStatusColor(a.status)

  return (
    <>
      {/* 헤더 */}
      <div style={{ padding: '12px 16px', borderBottom: '1px solid #eee' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
          <span style={{
            width: 10, height: 10, borderRadius: '50%', background: sc.bar, display: 'inline-block',
          }} />
          <b style={{ fontSize: 16 }}>{a.name}</b>
          <span className="tag" style={{
            background: sc.bar, color: '#fff', fontSize: 10, padding: '1px 6px', borderRadius: 3,
          }}>{a.status}</span>
          <span style={{ color: '#888', fontSize: 12 }}>#{a.id}</span>
          {a.agent_version && <span style={{ color: '#888', fontSize: 12 }}>· v{a.agent_version}</span>}

          {/* 서버 전용 액션 */}
          <div style={{ marginLeft: 'auto', display: 'flex', gap: 6, flexWrap: 'wrap' }}>
            {a.status === 'pending' && (
              <button className="btn btn--sm btn--primary" onClick={() => onApprove(a)}>승인</button>
            )}
            {(a.status === 'online' || a.status === 'offline') && (
              <>
                <button className="btn btn--sm" onClick={() => onMetrics(a)}>메트릭</button>
                <button className="btn btn--sm" onClick={() => onUpgrade(a)}
                  disabled={a.status !== 'online'} title="agent 바이너리를 최신 버전으로 교체">
                  ↑ 업그레이드
                </button>
                <button className="btn btn--sm btn--outline" onClick={() => onRevoke(a)}>폐기</button>
              </>
            )}
            <button className="btn btn--sm btn--danger" onClick={() => onRemove(a)}>삭제</button>
          </div>
        </div>
      </div>

      {/* 탭 */}
      <div style={{ display: 'flex', gap: 0, borderBottom: '1px solid #eee', background: '#fafafa' }}>
        <TabButton active={tab === 'modules'} onClick={() => setTab('modules')}>
          모듈 ({deployments.length})
        </TabButton>
        <TabButton active={tab === 'info'} onClick={() => setTab('info')}>
          정보
        </TabButton>
      </div>

      {/* 탭 컨텐츠 */}
      <div style={{ flex: 1, overflow: 'auto', padding: 16 }}>
        {tab === 'modules' && (
          <ModulesTab agent={a} deployments={deployments} packagesAvailable={packages.length > 0}
            onAddDeploy={onAddDeploy} onConfigure={onConfigure}
            onJob={onJob} onRemoveDep={onRemoveDep} />
        )}
        {tab === 'info' && <InfoTab agent={a} />}
      </div>
    </>
  )
}

function TabButton({ active, children, onClick }: {
  active: boolean; children: React.ReactNode; onClick: () => void
}) {
  return (
    <button onClick={onClick}
      style={{
        padding: '10px 16px', border: 'none',
        background: active ? '#fff' : 'transparent',
        borderBottom: `2px solid ${active ? '#3498db' : 'transparent'}`,
        fontWeight: active ? 600 : 400, cursor: 'pointer',
      }}>
      {children}
    </button>
  )
}

function ModulesTab({ agent: a, deployments, packagesAvailable,
                     onAddDeploy, onConfigure, onJob, onRemoveDep }: {
  agent: Agent
  deployments: Deployment[]
  packagesAvailable: boolean
  onAddDeploy: () => void
  onConfigure: (d: Deployment) => void
  onJob: (d: Deployment, jt: JobType) => void
  onRemoveDep: (d: Deployment) => void
}) {
  return (
    <>
      {deployments.length === 0 ? (
        <div className="empty">배포된 모듈 없음</div>
      ) : (
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 10 }}></th>
              <th>프로세스</th>
              <th>기능</th>
              <th>모듈 · 버전</th>
              <th>상태</th>
              <th style={{ width: 280 }}>작업</th>
            </tr>
          </thead>
          <tbody>
            {deployments.map(d => (
              <DeploymentRow key={d.id} dep={d} agent={a}
                onConfigure={onConfigure} onJob={onJob} onRemove={onRemoveDep} />
            ))}
          </tbody>
        </table>
      )}
      <div style={{ marginTop: 12, textAlign: 'right' }}>
        <button className="btn btn--primary btn--sm"
          disabled={a.status !== 'online' || !packagesAvailable}
          title={!packagesAvailable ? '패키지 먼저 업로드 필요' : ''}
          onClick={onAddDeploy}>＋ 모듈 추가</button>
      </div>
    </>
  )
}

function DeploymentRow({ dep: d, agent, onConfigure, onJob, onRemove }: {
  dep: Deployment; agent: Agent
  onConfigure: (d: Deployment) => void
  onJob: (d: Deployment, jt: JobType) => void
  onRemove: (d: Deployment) => void
}) {
  const sc = depStatusColor(d.status)
  const online = agent.status === 'online'
  // pending = 생성만 됨 (파일 없음), stopped = 설치됐지만 실행 안됨
  const notInstalled = d.status === 'pending'
  const canStart = online && (d.status === 'stopped' || d.status === 'running')
  const canOps   = online && (d.status === 'running' || d.status === 'stopped')
  return (
    <tr>
      <td style={{ padding: 0 }}>
        <div style={{ width: 4, background: sc, height: 32 }} />
      </td>
      <td><b>{d.process_name || '—'}</b></td>
      <td style={{ fontSize: 12, color: '#555' }}>
        {d.service_functions.length === 0 ? '—' : d.service_functions.join(', ')}
      </td>
      <td style={{ fontSize: 12 }}>
        {d.package_name} <span style={{ color: '#888' }}>v{d.package_version}</span>
      </td>
      <td>
        <span className="tag" style={{
          background: sc, color: '#fff', fontSize: 10, padding: '1px 6px', borderRadius: 3,
        }}>{d.status}</span>
      </td>
      <td>
        <div style={{ display: 'flex', gap: 4, flexWrap: 'wrap' }}>
          <button className="btn btn--sm" title="설정"
            onClick={() => onConfigure(d)}>⚙ 설정</button>
          <button className="btn btn--sm" disabled={!online} title="install (파일 배치 + 설정 적용)"
            onClick={() => onJob(d, 'install')}>
            {notInstalled ? '설치' : '재설치'}
          </button>
          <button className="btn btn--sm" disabled={!canStart} title="start"
            onClick={() => onJob(d, 'start')}>▶</button>
          <button className="btn btn--sm" disabled={!canOps} title="restart"
            onClick={() => onJob(d, 'restart')}>↻</button>
          <button className="btn btn--sm" disabled={!canOps} title="stop"
            onClick={() => onJob(d, 'stop')}>■</button>
          <button className="btn btn--sm btn--danger" title="delete"
            onClick={() => onRemove(d)}>✕</button>
        </div>
      </td>
    </tr>
  )
}

function InfoTab({ agent: a }: { agent: Agent }) {
  return (
    <div style={{ display: 'grid', gridTemplateColumns: '120px 1fr', rowGap: 8, columnGap: 12, fontSize: 13 }}>
      <Field label="이름" value={a.name} />
      <Field label="호스트" value={a.hostname || '—'} />
      <Field label="IP" value={a.ip_address || '—'} />
      <Field label="OS" value={a.os_info || '—'} />
      <Field label="CPU 코어" value={a.cpu_cores ? `${a.cpu_cores}` : '—'} />
      <Field label="메모리" value={a.memory_mb ? `${Math.round(a.memory_mb / 1024)} GB` : '—'} />
      <Field label="디스크" value={a.disk_gb ? `${a.disk_gb} GB` : '—'} />
      <Field label="Agent 버전" value={a.agent_version || '—'} />
      <Field label="등록 시각" value={a.enrolled_at || '—'} />
      <Field label="승인 시각" value={a.approved_at || '—'} />
      <Field label="마지막 heartbeat" value={`${a.last_heartbeat || '—'} (${fmtRelTime(a.last_heartbeat)})`} />
      <Field label="메모" value={a.note || '—'} />
    </div>
  )
}

function Field({ label, value }: { label: string; value: string }) {
  return (
    <>
      <span style={{ color: '#888' }}>{label}</span>
      <span>{value}</span>
    </>
  )
}

// ──────────────────────────────────────────────────────────────
//  Modals
// ──────────────────────────────────────────────────────────────

function AgentCreateModal({ onClose, onDone }: { onClose: () => void; onDone: () => void }) {
  const { show } = useToast()
  const [name, setName] = useState('')
  const [note, setNote] = useState('')
  const [result, setResult] = useState<{ enrollment_token: string; install_command: string } | null>(null)
  const [copied, setCopied] = useState(false)

  async function create() {
    if (!name) { show('이름 필수', 'err'); return }
    try {
      const r = await deploymentApi.createAgent(name, note)
      setResult({ enrollment_token: r.enrollment_token, install_command: r.install_command })
      await onDone()
    } catch (e) { show((e as Error).message, 'err') }
  }
  async function copyCmd() {
    if (!result) return
    try {
      await navigator.clipboard.writeText(result.install_command)
      setCopied(true); setTimeout(() => setCopied(false), 1500)
    } catch (e) { show((e as Error).message, 'err') }
  }

  return (
    <Modal title="서버 등록" onClose={onClose} width={580}>
      {!result ? (
        <div className="form-grid">
          <label>이름 *</label>
          <input className="form-input" value={name} placeholder="예: vlt-sig-pri"
            onChange={e => setName(e.target.value)} />
          <label>메모</label>
          <input className="form-input" value={note} onChange={e => setNote(e.target.value)} />
        </div>
      ) : (
        <div>
          <div style={{ color: '#2ecc71', marginBottom: 10 }}>
            ✓ 서버 등록됨. 아래 명령을 대상 서버의 운영 계정에서 실행:
          </div>
          <div style={{ position: 'relative' }}>
            <pre style={{
              background: '#0d1117', color: '#c9d1d9', padding: 12, paddingRight: 88,
              borderRadius: 4, fontSize: 12, whiteSpace: 'pre-wrap', margin: 0,
            }}>{result.install_command}</pre>
            <button className="btn btn--sm btn--outline"
              style={{ position: 'absolute', top: 8, right: 8 }}
              onClick={copyCmd}>{copied ? '✓' : '📋'} 복사</button>
          </div>
          <div style={{ fontSize: 12, color: '#666', marginTop: 8 }}>
            Enrollment token: <code>{result.enrollment_token}</code> (1회용)
          </div>
        </div>
      )}
      <div className="modal-footer" style={{ marginTop: 16 }}>
        {!result ? (
          <>
            <button className="btn btn--outline" onClick={onClose}>취소</button>
            <button className="btn btn--primary" onClick={create}>등록</button>
          </>
        ) : (
          <button className="btn btn--primary" onClick={onClose}>닫기</button>
        )}
      </div>
    </Modal>
  )
}

function DeploymentCreateModal({ agent, packages, onClose, onDone }: {
  agent: Agent; packages: SipPackage[]
  onClose: () => void; onDone: () => void
}) {
  const { show } = useToast()
  const [moduleName, setModuleName] = useState<string>('')
  const [pkgId, setPkgId]           = useState(0)
  const [processName, setProcessName] = useState<string>('')
  const [functions, setFunctions]   = useState<Set<string>>(new Set())
  const [note, setNote]             = useState('')

  // 모듈별로 버전 그룹
  const pkgsByModule = useMemo(() => {
    const m = new Map<string, SipPackage[]>()
    for (const p of packages) {
      if (!m.has(p.name)) m.set(p.name, [])
      m.get(p.name)!.push(p)
    }
    for (const list of m.values()) {
      list.sort((a, b) => {
        const ta = a.uploaded_at ? Date.parse(a.uploaded_at) : 0
        const tb = b.uploaded_at ? Date.parse(b.uploaded_at) : 0
        if (tb !== ta) return tb - ta
        return b.id - a.id
      })
    }
    return m
  }, [packages])

  const moduleNames = useMemo(
    () => Array.from(pkgsByModule.keys()).sort((a, b) => a.localeCompare(b)),
    [pkgsByModule]
  )
  const versions = moduleName ? (pkgsByModule.get(moduleName) || []) : []
  const selectedPkg = versions.find(v => v.id === pkgId) || null

  // package 의 meta.service 구조
  const svcMeta = selectedPkg?.meta?.service
  const processOptions = svcMeta?.processes || []
  const functionOptions = svcMeta?.functions || []

  // 모듈 바뀌면 버전/process/functions 리셋
  useEffect(() => {
    if (!moduleName) { setPkgId(0); setProcessName(''); setFunctions(new Set()); return }
    const latest = (pkgsByModule.get(moduleName) || [])[0]
    setPkgId(latest ? latest.id : 0)
  }, [moduleName, pkgsByModule])

  // 버전 바뀌면 process/functions 디폴트 반영
  useEffect(() => {
    if (!selectedPkg) { setProcessName(''); setFunctions(new Set()); return }
    const procs = selectedPkg.meta?.service?.processes || []
    setProcessName(procs.length > 0 ? procs[0] : (selectedPkg.name || '').toUpperCase())
    const funcs = selectedPkg.meta?.service?.functions || []
    // 기본으로 모든 functions 체크
    setFunctions(new Set(funcs.map(f => f.name)))
  }, [selectedPkg])

  function toggleFunc(name: string) {
    setFunctions(prev => {
      const n = new Set(prev)
      if (n.has(name)) n.delete(name); else n.add(name)
      return n
    })
  }

  async function create() {
    if (!pkgId) { show('모듈/버전 선택 필요', 'err'); return }
    if (!processName.trim()) { show('프로세스 이름 필수', 'err'); return }
    try {
      await deploymentApi.createDeployment({
        agent_id: agent.id,
        package_id: pkgId,
        process_name: processName.trim(),
        service_functions: Array.from(functions),
        note: note || undefined,
      })
      show(`${agent.name} 에 ${processName} 배포 추가 (설치 전)`, 'ok')
      await onDone(); onClose()
    } catch (e) { show((e as Error).message, 'err') }
  }

  return (
    <Modal title={`${agent.name} — 모듈 추가`} onClose={onClose} width={600}>
      <div className="form-grid">
        <label>1. 모듈 *</label>
        <select className="form-input" value={moduleName}
          onChange={e => setModuleName(e.target.value)}>
          <option value="">(선택)</option>
          {moduleNames.map(m => (
            <option key={m} value={m}>{m} ({pkgsByModule.get(m)!.length}개 버전)</option>
          ))}
        </select>

        <label>2. 버전 *</label>
        <select className="form-input" value={pkgId} disabled={!moduleName}
          onChange={e => setPkgId(Number(e.target.value))}>
          <option value={0}>(선택)</option>
          {versions.map((p, i) => (
            <option key={p.id} value={p.id}>
              v{p.version}{i === 0 ? '  (최신)' : ''}
            </option>
          ))}
        </select>

        {selectedPkg && (
          <>
            <label>3. 프로세스 *</label>
            {processOptions.length > 1 ? (
              <select className="form-input" value={processName}
                onChange={e => setProcessName(e.target.value)}>
                {processOptions.map(p => <option key={p} value={p}>{p}</option>)}
              </select>
            ) : (
              <input className="form-input" value={processName}
                onChange={e => setProcessName(e.target.value)}
                placeholder={selectedPkg.name.toUpperCase()} />
            )}

            <label>4. 기능</label>
            <div style={{
              border: '1px solid #ddd', borderRadius: 4, padding: 8,
              display: 'flex', flexDirection: 'column', gap: 4,
            }}>
              {functionOptions.length === 0 ? (
                <span className="text-muted" style={{ fontSize: 12 }}>
                  (패키지 meta.json 에 functions 정의 없음 — 기능 선택 불필요)
                </span>
              ) : (
                functionOptions.map(f => (
                  <label key={f.name} style={{ display: 'flex', gap: 8, alignItems: 'center', fontSize: 13 }}>
                    <input type="checkbox" checked={functions.has(f.name)}
                      onChange={() => toggleFunc(f.name)} />
                    <span>{f.desc || f.name}</span>
                    <span className="text-muted" style={{ fontSize: 11 }}>({f.name})</span>
                  </label>
                ))
              )}
            </div>
          </>
        )}

        <label>메모</label>
        <input className="form-input" value={note} onChange={e => setNote(e.target.value)} />
      </div>
      <div style={{ marginTop: 12, fontSize: 12, color: '#666' }}>
        ℹ 추가 후 <b>pending</b> 상태로 생성됩니다. 설정을 확인한 뒤
        <b>설치</b> → <b>Start</b> 순으로 진행하세요.
      </div>
      <div className="modal-footer" style={{ marginTop: 16 }}>
        <button className="btn btn--outline" onClick={onClose}>취소</button>
        <button className="btn btn--primary" onClick={create}>추가</button>
      </div>
    </Modal>
  )
}

function MetricsModal({ agent, onClose }: { agent: Agent; onClose: () => void }) {
  const { show } = useToast()
  const [metrics, setMetrics] = useState<AgentMetric[]>([])
  useEffect(() => {
    deploymentApi.agentMetrics(agent.id)
      .then(r => setMetrics(r.items))
      .catch(e => show((e as Error).message, 'err'))
  }, [agent.id, show])
  return (
    <Modal title={`${agent.name} — 메트릭 (최근 ${metrics.length}건)`}
           onClose={onClose} width={760}>
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
                {m.processes.length === 0 ? '—'
                  : m.processes.map(p => `${p.name}(${p.pid})`).join(', ')}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <div className="modal-footer" style={{ marginTop: 16 }}>
        <button className="btn btn--outline" onClick={onClose}>닫기</button>
      </div>
    </Modal>
  )
}
