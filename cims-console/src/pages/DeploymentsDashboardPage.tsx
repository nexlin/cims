import { useState, useEffect, useCallback, useMemo } from 'react'
import {
  deploymentApi,
  type Agent, type SipPackage, type Deployment, type JobType, type AgentMetric,
} from '../api/deployment'
import { ApiError } from '../api/client'
import { useToast } from '../components/Toast'

// ──────────────────────────────────────────────────────────────
//  서비스 종류 카탈로그
// ──────────────────────────────────────────────────────────────
const SERVICE_KINDS = [
  'csp', 'cmp', 'psp', 'pmp', 'isp', 'imp',
  'csc', 'console', 'phone', 'cwrtc',
]

// ──────────────────────────────────────────────────────────────
//  메인 페이지
// ──────────────────────────────────────────────────────────────

export default function DeploymentsDashboardPage() {
  const { show } = useToast()
  const [agents, setAgents]           = useState<Agent[]>([])
  const [packages, setPackages]       = useState<SipPackage[]>([])
  const [deployments, setDeployments] = useState<Deployment[]>([])
  const [loading, setLoading]         = useState(true)

  // 모달 상태
  const [agentModalOpen, setAgentModalOpen]     = useState(false)
  const [packageModalOpen, setPackageModalOpen] = useState(false)
  const [deployModal, setDeployModal]           = useState<{ agent: Agent } | null>(null)
  const [metricsFor, setMetricsFor]             = useState<Agent | null>(null)

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

  // agent_id → 해당 agent 의 deployments
  const depsByAgent = useMemo(() => {
    const m = new Map<number, Deployment[]>()
    for (const d of deployments) {
      if (!m.has(d.agent_id)) m.set(d.agent_id, [])
      m.get(d.agent_id)!.push(d)
    }
    return m
  }, [deployments])

  // 통계 (상단 summary)
  const stats = useMemo(() => ({
    agents_total:   agents.length,
    agents_online:  agents.filter(a => a.status === 'online').length,
    agents_pending: agents.filter(a => a.status === 'pending').length,
    packages:       packages.length,
    deployments_total:  deployments.length,
    deployments_running: deployments.filter(d => d.status === 'running').length,
  }), [agents, packages, deployments])

  // 공통 액션
  async function refresh() { await load() }

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

  async function removePackage(p: SipPackage) {
    if (!confirm(`${p.name} ${p.version} 을 삭제할까요?`)) return
    try { await deploymentApi.deletePackage(p.id); show('삭제됨', 'ok'); await load() }
    catch (e) { show((e as Error).message, 'err') }
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
    <div>
      {/* 상단 요약 + 액션 */}
      <div style={{ display: 'flex', gap: 12, alignItems: 'center', marginBottom: 16, flexWrap: 'wrap' }}>
        <StatChip label="서버"   value={`${stats.agents_online}/${stats.agents_total}`}  sub="online" color="#2ecc71" />
        {stats.agents_pending > 0 &&
          <StatChip label="승인대기" value={stats.agents_pending} color="#f39c12" />}
        <StatChip label="패키지" value={stats.packages} color="#3498db" />
        <StatChip label="배포" value={`${stats.deployments_running}/${stats.deployments_total}`} sub="running" color="#9b59b6" />
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 8 }}>
          <button className="btn btn--outline" onClick={refresh}>↻</button>
          <button className="btn btn--primary" onClick={() => setAgentModalOpen(true)}>＋ Agent 등록</button>
          <button className="btn btn--primary" onClick={() => setPackageModalOpen(true)}>＋ 패키지 업로드</button>
        </div>
      </div>

      {/* 서버/Agent 섹션 */}
      <SectionHeader title="서버 Agent" count={agents.length} />
      {agents.length === 0 ? (
        <div className="empty">등록된 서버 없음 — "＋ Agent 등록" 으로 추가</div>
      ) : (
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(380px, 1fr))', gap: 12 }}>
          {agents.map(a => (
            <AgentCard key={a.id} agent={a}
              deployments={depsByAgent.get(a.id) || []}
              packages={packages}
              onApprove={approveAgent}
              onRevoke={revokeAgent}
              onRemove={removeAgent}
              onUpgrade={upgradeAgent}
              onMetrics={setMetricsFor}
              onAddDeploy={(agt) => setDeployModal({ agent: agt })}
              onJob={queueJob}
              onRemoveDep={removeDeployment}
            />
          ))}
        </div>
      )}

      {/* 패키지 섹션 */}
      <div style={{ marginTop: 24 }}>
        <SectionHeader title="배포 패키지" count={packages.length} />
        {packages.length === 0 ? (
          <div className="empty">등록된 패키지 없음</div>
        ) : (
          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(280px, 1fr))', gap: 12 }}>
            {packages.map(p => <PackageCard key={p.id} pkg={p} onRemove={removePackage} />)}
          </div>
        )}
      </div>

      {/* 모달들 */}
      {agentModalOpen &&
        <AgentCreateModal onClose={() => setAgentModalOpen(false)} onDone={load} />}
      {packageModalOpen &&
        <PackageUploadModal onClose={() => setPackageModalOpen(false)} onDone={load} />}
      {deployModal &&
        <DeploymentCreateModal agent={deployModal.agent} packages={packages}
          onClose={() => setDeployModal(null)} onDone={load} />}
      {metricsFor &&
        <MetricsModal agent={metricsFor} onClose={() => setMetricsFor(null)} />}
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  상단 stat chip
// ──────────────────────────────────────────────────────────────
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

function SectionHeader({ title, count }: { title: string; count: number }) {
  return (
    <div style={{
      display: 'flex', alignItems: 'baseline', gap: 10,
      marginBottom: 10, paddingBottom: 6, borderBottom: '1px solid #eee',
    }}>
      <h3 style={{ margin: 0 }}>{title}</h3>
      <span style={{ color: '#888', fontSize: 13 }}>({count})</span>
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  Agent card (deployment 들을 포함)
// ──────────────────────────────────────────────────────────────
function AgentCard({ agent: a, deployments, packages,
                     onApprove, onRevoke, onRemove, onUpgrade, onMetrics,
                     onAddDeploy, onJob, onRemoveDep }: {
  agent: Agent
  deployments: Deployment[]
  packages: SipPackage[]
  onApprove:   (a: Agent) => void
  onRevoke:    (a: Agent) => void
  onRemove:    (a: Agent) => void
  onUpgrade:   (a: Agent) => void
  onMetrics:   (a: Agent) => void
  onAddDeploy: (a: Agent) => void
  onJob:       (d: Deployment, jt: JobType) => void
  onRemoveDep: (d: Deployment) => void
}) {
  const sc = statusColor(a.status)
  return (
    <div style={{
      border: `1px solid ${sc.border}`, borderLeft: `4px solid ${sc.bar}`,
      borderRadius: 6, padding: 14, background: '#fff',
    }}>
      {/* 헤더 */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <span style={{
          width: 10, height: 10, borderRadius: '50%', background: sc.bar, display: 'inline-block',
        }} />
        <b style={{ fontSize: 15 }}>{a.name}</b>
        <span className="tag" style={{
          background: sc.bar, color: '#fff', fontSize: 10, padding: '1px 6px', borderRadius: 3,
        }}>{a.status}</span>
        <span style={{ marginLeft: 'auto', color: '#888', fontSize: 11 }}>#{a.id}</span>
      </div>

      {/* 메타 */}
      <div style={{ fontSize: 12, color: '#555', marginTop: 6, lineHeight: 1.6 }}>
        {a.hostname && <>🖥 {a.hostname} ({a.ip_address})<br/></>}
        {a.os_info && <>📟 {a.os_info}<br/></>}
        {(a.cpu_cores || a.memory_mb) && (
          <>🔧 {a.cpu_cores && `${a.cpu_cores}코어`}
             {a.memory_mb && ` / ${Math.round(a.memory_mb/1024)}GB`}
             {a.disk_gb && ` / ${a.disk_gb}GB`}
             {a.agent_version && <> <span style={{ color: '#888' }}>v{a.agent_version}</span></>}
             <br/></>
        )}
        ⏱ {fmtTime(a.last_heartbeat)}
      </div>

      {/* Deployments */}
      <div style={{ marginTop: 10, borderTop: '1px dashed #eee', paddingTop: 8 }}>
        <div style={{ fontSize: 12, color: '#666', marginBottom: 6, fontWeight: 600 }}>
          📦 Deployments ({deployments.length})
        </div>
        {deployments.length === 0 ? (
          <div style={{ fontSize: 12, color: '#999', padding: '4px 0' }}>
            아직 배포된 모듈 없음
          </div>
        ) : (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
            {deployments.map(d => (
              <DeploymentRow key={d.id} dep={d} agent={a}
                onJob={onJob} onRemove={onRemoveDep} />
            ))}
          </div>
        )}
        <button className="btn btn--sm btn--outline" style={{ marginTop: 8, width: '100%' }}
          disabled={a.status !== 'online' || packages.length === 0}
          title={packages.length === 0 ? '패키지 먼저 업로드 필요' : ''}
          onClick={() => onAddDeploy(a)}>＋ 모듈 추가</button>
      </div>

      {/* Agent 전용 액션 */}
      <div style={{ marginTop: 10, display: 'flex', gap: 6, flexWrap: 'wrap' }}>
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
        <button className="btn btn--sm btn--danger" style={{ marginLeft: 'auto' }}
          onClick={() => onRemove(a)}>삭제</button>
      </div>
    </div>
  )
}

function DeploymentRow({ dep: d, agent, onJob, onRemove }: {
  dep: Deployment
  agent: Agent
  onJob: (d: Deployment, jt: JobType) => void
  onRemove: (d: Deployment) => void
}) {
  const sc = depStatusColor(d.status)
  const online = agent.status === 'online'
  const canStart = online && (d.status === 'running' || d.status === 'stopped' || d.status === 'pending')
  const canOps   = online && (d.status === 'running' || d.status === 'stopped')
  return (
    <div style={{
      display: 'flex', alignItems: 'center', gap: 8,
      padding: '6px 8px', background: '#f7f7f7', borderRadius: 4, fontSize: 12,
      borderLeft: `3px solid ${sc}`,
    }}>
      <b style={{ minWidth: 60 }}>{d.service_kind}</b>
      <span style={{ color: '#666' }}>
        {d.package_name} <span style={{ color: '#999' }}>v{d.package_version}</span>
      </span>
      <span className="tag" style={{ background: sc, color: '#fff', fontSize: 10 }}>{d.status}</span>
      <div style={{ marginLeft: 'auto', display: 'flex', gap: 4 }}>
        <button className="btn btn--sm" disabled={!online} title="install/upgrade"
          onClick={() => onJob(d, 'install')}>↓</button>
        <button className="btn btn--sm" disabled={!canStart} title="start"
          onClick={() => onJob(d, 'start')}>▶</button>
        <button className="btn btn--sm" disabled={!canOps} title="restart"
          onClick={() => onJob(d, 'restart')}>↻</button>
        <button className="btn btn--sm" disabled={!canOps} title="stop"
          onClick={() => onJob(d, 'stop')}>■</button>
        <button className="btn btn--sm btn--danger" title="delete"
          onClick={() => onRemove(d)}>✕</button>
      </div>
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  Package card
// ──────────────────────────────────────────────────────────────
function PackageCard({ pkg: p, onRemove }: { pkg: SipPackage; onRemove: (p: SipPackage) => void }) {
  return (
    <div style={{
      border: '1px solid #ddd', borderLeft: '4px solid #3498db', borderRadius: 6,
      padding: 12, background: '#fff',
    }}>
      <div style={{ display: 'flex', alignItems: 'baseline', gap: 8 }}>
        <b>{p.name}</b>
        <span style={{ color: '#888', fontSize: 13 }}>v{p.version}</span>
        <span style={{ marginLeft: 'auto', color: '#888', fontSize: 11 }}>#{p.id}</span>
      </div>
      <div style={{ marginTop: 6, fontSize: 12, color: '#666', lineHeight: 1.6 }}>
        📦 {fmtSize(p.file_size)}<br/>
        🔒 <code style={{ fontSize: 11 }}>{p.sha256.substring(0, 16)}…</code><br/>
        ⏱ {p.uploaded_at} {p.uploaded_by && `· ${p.uploaded_by}`}
      </div>
      {p.description && (
        <div style={{ marginTop: 4, fontSize: 11, color: '#888',
                      textOverflow: 'ellipsis', overflow: 'hidden', whiteSpace: 'nowrap' }}
          title={p.description}>
          {p.description}
        </div>
      )}
      <div style={{ marginTop: 8, textAlign: 'right' }}>
        <button className="btn btn--sm btn--danger" onClick={() => onRemove(p)}>삭제</button>
      </div>
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  Modals
// ──────────────────────────────────────────────────────────────

function AgentCreateModal({ onClose, onDone }: { onClose: () => void; onDone: () => void }) {
  const { show } = useToast()
  const [name, setName] = useState(''); const [note, setNote] = useState('')
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
    <Modal title="Agent 등록" onClose={onClose} minWidth={560}>
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
            ✓ Agent 등록됨. 아래 명령을 대상 서버의 운영 계정에서 실행:
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
      <ModalFooter>
        {!result ? (
          <>
            <button className="btn btn--outline" onClick={onClose}>취소</button>
            <button className="btn btn--primary" onClick={create}>등록</button>
          </>
        ) : (
          <button className="btn btn--primary" onClick={onClose}>닫기</button>
        )}
      </ModalFooter>
    </Modal>
  )
}

function DeploymentCreateModal({ agent, packages, onClose, onDone }: {
  agent: Agent; packages: SipPackage[]
  onClose: () => void; onDone: () => void
}) {
  const { show } = useToast()
  const [pkgId, setPkgId] = useState(0)
  const [svc, setSvc]     = useState('csp')
  const [note, setNote]   = useState('')

  async function create() {
    if (!pkgId) { show('패키지 선택', 'err'); return }
    try {
      const d = await deploymentApi.createDeployment({
        agent_id: agent.id, package_id: pkgId, service_kind: svc,
        note: note || undefined,
      })
      // 생성 즉시 install job 큐잉
      await deploymentApi.queueJob(d.id, 'install')
      show(`${agent.name} 에 ${svc} 배포 생성 + install 시작`, 'ok')
      await onDone(); onClose()
    } catch (e) { show((e as Error).message, 'err') }
  }

  return (
    <Modal title={`${agent.name} — 모듈 추가`} onClose={onClose} minWidth={500}>
      <div className="form-grid">
        <label>패키지 *</label>
        <select className="form-input" value={pkgId}
          onChange={e => setPkgId(Number(e.target.value))}>
          <option value={0}>(선택)</option>
          {packages.map(p => (
            <option key={p.id} value={p.id}>#{p.id} {p.name} v{p.version}</option>
          ))}
        </select>
        <label>서비스 종류 *</label>
        <select className="form-input" value={svc} onChange={e => setSvc(e.target.value)}>
          {SERVICE_KINDS.map(k => <option key={k} value={k}>{k}</option>)}
        </select>
        <label>메모</label>
        <input className="form-input" value={note} onChange={e => setNote(e.target.value)} />
      </div>
      <div style={{ marginTop: 10, fontSize: 12, color: '#666' }}>
        ℹ Agent 가 자기 디렉토리의 <code>modules/{svc}/</code> 에 자동 배치합니다.
        <br/>생성 후 자동으로 install job 이 큐잉됩니다.
      </div>
      <ModalFooter>
        <button className="btn btn--outline" onClick={onClose}>취소</button>
        <button className="btn btn--primary" onClick={create}>배포 생성</button>
      </ModalFooter>
    </Modal>
  )
}

interface UploadRow {
  id: string
  file: File
  state: 'pending' | 'uploading' | 'done' | 'failed' | 'aborted' | 'skipped'
  pct: number
  loaded: number
  speedBps: number
  msg?: string
}

function PackageUploadModal({ onClose, onDone }: { onClose: () => void; onDone: () => void }) {
  const { show } = useToast()
  const [rows, setRows] = useState<UploadRow[]>([])
  const [busy, setBusy] = useState(false)
  // 진행 중 XHR abort 핸들 보관
  const [aborts] = useState<Map<string, () => void>>(() => new Map())

  function update(id: string, patch: Partial<UploadRow>) {
    setRows(rs => rs.map(r => r.id === id ? { ...r, ...patch } : r))
  }

  function addFiles(fl: FileList | null) {
    if (!fl) return
    const next: UploadRow[] = []
    for (const f of Array.from(fl)) {
      next.push({
        id: `${f.name}-${f.size}-${f.lastModified}-${Math.random().toString(36).slice(2, 8)}`,
        file: f, state: 'pending', pct: 0, loaded: 0, speedBps: 0,
      })
    }
    setRows(prev => [...prev, ...next])
  }

  function abortOne(id: string) {
    const a = aborts.get(id)
    if (a) { a(); aborts.delete(id) }
  }
  function abortAll() {
    for (const a of aborts.values()) { try { a() } catch { /* */ } }
    aborts.clear()
  }

  // 언마운트 / 모달 닫기 시 진행 중 XHR abort
  useEffect(() => () => abortAll(), [])   // eslint-disable-line react-hooks/exhaustive-deps

  async function uploadOne(row: UploadRow, force: boolean): Promise<void> {
    update(row.id, { state: 'uploading', pct: 0, loaded: 0, speedBps: 0, msg: undefined })
    let lastPct = 0
    let lastTs = performance.now()
    let lastLoaded = 0
    const handle = deploymentApi.uploadPackageFile(row.file, force, p => {
      const now = performance.now()
      if (p.pct - lastPct >= 5 || now - lastTs >= 150 || p.pct === 100) {
        const dt = Math.max(now - lastTs, 1)
        const speed = ((p.loaded - lastLoaded) * 1000) / dt
        lastPct = p.pct; lastTs = now; lastLoaded = p.loaded
        update(row.id, { pct: p.pct, loaded: p.loaded, speedBps: speed })
      }
    })
    aborts.set(row.id, handle.abort)
    try {
      await handle.promise
      update(row.id, { state: 'done', pct: 100, loaded: row.file.size, msg: '완료' })
    } catch (e) {
      if (e instanceof ApiError && e.data?.error === 'aborted') {
        update(row.id, { state: 'aborted', msg: '취소됨' })
        return
      }
      if (e instanceof ApiError && e.status === 409) {
        // 덮어쓰기 재시도
        update(row.id, { state: 'uploading', msg: '덮어쓰는 중' })
        try {
          const h2 = deploymentApi.uploadPackageFile(row.file, true, p => {
            update(row.id, { pct: p.pct, loaded: p.loaded })
          })
          aborts.set(row.id, h2.abort)
          await h2.promise
          update(row.id, { state: 'done', pct: 100, loaded: row.file.size, msg: '덮어씀' })
        } catch (e2) {
          if (e2 instanceof ApiError && e2.data?.error === 'aborted') {
            update(row.id, { state: 'aborted', msg: '취소됨' })
          } else {
            update(row.id, { state: 'failed', msg: (e2 as Error).message })
          }
        }
      } else {
        update(row.id, { state: 'failed', msg: (e as Error).message })
      }
    } finally {
      aborts.delete(row.id)
    }
  }

  async function uploadAll() {
    setBusy(true)
    try {
      // 순차 (서버 부하 제어). 병렬이 필요하면 여기서 Promise.allSettled 로 변경
      for (const r of rows) {
        if (r.state === 'pending') {
          await uploadOne(r, false)
        }
      }
      await onDone()
      show('업로드 처리 완료', 'ok')
    } finally {
      setBusy(false)
    }
  }

  function closeModal() {
    abortAll()
    onClose()
  }

  const stats = useMemo(() => {
    const pending = rows.filter(r => r.state === 'pending').length
    const done    = rows.filter(r => r.state === 'done').length
    const failed  = rows.filter(r => r.state === 'failed').length
    return { pending, done, failed }
  }, [rows])

  return (
    <Modal title="패키지 업로드" onClose={closeModal} minWidth={720} maxHeight="85vh">
      <div style={{ display: 'flex', gap: 10, alignItems: 'center', flexWrap: 'wrap' }}>
        <label htmlFor="pkg-files" className="btn btn--outline" style={{ cursor: 'pointer' }}>
          📁 파일 선택 (여러 개 가능)
        </label>
        <input id="pkg-files" type="file" accept=".tar.gz,.tgz" multiple
          style={{ display: 'none' }}
          onChange={e => { addFiles(e.target.files); e.target.value = '' }} />
        {rows.length > 0 && (
          <span className="text-muted" style={{ fontSize: 12 }}>
            총 {rows.length} · 대기 {stats.pending} · 완료 {stats.done} · 실패 {stats.failed}
          </span>
        )}
      </div>

      <div style={{ marginTop: 10, fontSize: 12, color: '#888' }}>
        ℹ 각 파일 내부의 <code>meta.json</code> 으로 이름/버전/설명/빌드정보 자동 추출.
        동일 (name, version) 이 이미 있으면 자동으로 덮어씁니다.
      </div>

      {rows.length === 0 ? (
        <div className="empty" style={{ marginTop: 16 }}>업로드할 파일을 선택하세요</div>
      ) : (
        <table className="data-table" style={{ marginTop: 12 }}>
          <thead>
            <tr>
              <th>파일</th>
              <th style={{ width: 80 }}>크기</th>
              <th>진행</th>
              <th style={{ width: 100 }}>상태</th>
              <th style={{ width: 70 }}>작업</th>
            </tr>
          </thead>
          <tbody>
            {rows.map(r => <UploadProgressRow key={r.id} row={r}
              onAbort={() => abortOne(r.id)}
              onRemove={() => setRows(rs => rs.filter(x => x.id !== r.id))}
              onRetry={() => uploadOne(r, false)} />)}
          </tbody>
        </table>
      )}

      <ModalFooter>
        <button className="btn btn--outline" onClick={closeModal}>닫기</button>
        <button className="btn btn--primary" disabled={busy || stats.pending === 0}
          onClick={uploadAll}>
          {busy ? '업로드 중...' : stats.pending > 0 ? `업로드 (${stats.pending}개)` : '완료'}
        </button>
      </ModalFooter>
    </Modal>
  )
}

function UploadProgressRow({ row, onAbort, onRemove, onRetry }: {
  row: UploadRow
  onAbort: () => void
  onRemove: () => void
  onRetry: () => void
}) {
  const bar = (color: string) => ({
    width: `${row.pct}%`, height: '100%', background: color,
    transition: 'width 0.15s',
  })
  const remain = row.file.size - row.loaded
  const eta = row.speedBps > 0 ? remain / row.speedBps : 0
  const stateBadge: Record<UploadRow['state'], { bg: string; label: string }> = {
    pending:   { bg: '#bbb',    label: '대기' },
    uploading: { bg: '#3498db', label: '업로드' },
    done:      { bg: '#2ecc71', label: '완료' },
    failed:    { bg: '#e74c3c', label: '실패' },
    aborted:   { bg: '#95a5a6', label: '취소' },
    skipped:   { bg: '#7f8c8d', label: '건너뜀' },
  }
  const sb = stateBadge[row.state]

  return (
    <tr>
      <td style={{ fontSize: 12, fontFamily: 'monospace' }}>{row.file.name}</td>
      <td style={{ fontSize: 12 }}>{fmtSize(row.file.size)}</td>
      <td>
        {(row.state === 'uploading' || row.state === 'done') && (
          <>
            <div style={{ width: 240, height: 8, background: '#eee', borderRadius: 4, overflow: 'hidden' }}>
              <div style={bar(row.state === 'done' ? '#2ecc71' : '#3498db')} />
            </div>
            <span className="text-muted" style={{ fontSize: 11 }}>
              {row.pct}% · {fmtSize(row.loaded)}/{fmtSize(row.file.size)}
              {row.speedBps > 0 && row.state === 'uploading' && (
                <> · {fmtSpeed(row.speedBps)} · ETA {fmtEta(eta)}</>
              )}
            </span>
          </>
        )}
        {row.state === 'failed' && (
          <span style={{ color: '#e74c3c', fontSize: 12 }}>{row.msg}</span>
        )}
      </td>
      <td>
        <span className="tag" style={{
          background: sb.bg, color: '#fff', fontSize: 10, padding: '1px 6px', borderRadius: 3,
        }}>{sb.label}</span>
        {row.msg && row.state === 'done' && (
          <div style={{ fontSize: 10, color: '#888' }}>{row.msg}</div>
        )}
      </td>
      <td>
        {row.state === 'uploading' && (
          <button className="btn btn--sm btn--outline" onClick={onAbort}>✕ 취소</button>
        )}
        {row.state === 'failed' && (
          <button className="btn btn--sm" onClick={onRetry}>재시도</button>
        )}
        {(row.state === 'pending' || row.state === 'aborted') && (
          <button className="btn btn--sm btn--outline" onClick={onRemove}>제거</button>
        )}
      </td>
    </tr>
  )
}

function fmtSpeed(bps: number) {
  if (bps < 1024) return `${bps.toFixed(0)} B/s`
  if (bps < 1024 * 1024) return `${(bps / 1024).toFixed(0)} KB/s`
  return `${(bps / 1024 / 1024).toFixed(1)} MB/s`
}
function fmtEta(sec: number) {
  if (!sec || sec <= 0) return '—'
  if (sec < 1) return '<1s'
  if (sec < 60) return `${sec.toFixed(0)}s`
  return `${Math.floor(sec/60)}m ${Math.floor(sec%60)}s`
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
           onClose={onClose} minWidth={720} maxHeight="85vh">
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
      <ModalFooter>
        <button className="btn btn--outline" onClick={onClose}>닫기</button>
      </ModalFooter>
    </Modal>
  )
}

// ──────────────────────────────────────────────────────────────
//  Helpers
// ──────────────────────────────────────────────────────────────

function Modal({ title, onClose, minWidth, maxHeight, children }: {
  title: string; onClose: () => void
  minWidth?: number; maxHeight?: string
  children: React.ReactNode
}) {
  return (
    <div className="modal-overlay" onClick={onClose}>
      <div className="modal-box" style={{ minWidth, maxHeight, overflow: 'auto' }}
           onClick={e => e.stopPropagation()}>
        <div className="modal-header">
          <span className="modal-title">{title}</span>
          <button className="modal-close" onClick={onClose}>✕</button>
        </div>
        <div className="modal-body">{children}</div>
      </div>
    </div>
  )
}

function ModalFooter({ children }: { children: React.ReactNode }) {
  return <div className="modal-footer" style={{ marginTop: 16 }}>{children}</div>
}

function statusColor(s: Agent['status']) {
  const m: Record<Agent['status'], { bar: string; border: string }> = {
    pending:  { bar: '#f39c12', border: '#fce8cc' },
    approved: { bar: '#3498db', border: '#d6e9f7' },
    online:   { bar: '#2ecc71', border: '#cfeee0' },
    offline:  { bar: '#95a5a6', border: '#dde2e3' },
    error:    { bar: '#e74c3c', border: '#f6d2cf' },
    revoked:  { bar: '#7f8c8d', border: '#d3d7d8' },
  }
  return m[s] || m.offline
}

function depStatusColor(s: Deployment['status']) {
  return {
    pending: '#f39c12', deploying: '#3498db', running: '#2ecc71',
    stopped: '#95a5a6', failed: '#e74c3c', removed: '#7f8c8d',
  }[s] || '#bbb'
}

function fmtTime(iso: string | null) {
  if (!iso) return '—'
  const d = new Date(iso)
  const delta = Math.floor((Date.now() - d.getTime()) / 1000)
  if (delta < 60)    return `${delta}초 전`
  if (delta < 3600)  return `${Math.floor(delta/60)}분 전`
  if (delta < 86400) return `${Math.floor(delta/3600)}시간 전`
  return d.toLocaleDateString('ko-KR')
}

function fmtSize(n: number) {
  if (n < 1024) return `${n} B`
  if (n < 1024*1024) return `${(n/1024).toFixed(1)} KB`
  if (n < 1024*1024*1024) return `${(n/1024/1024).toFixed(1)} MB`
  return `${(n/1024/1024/1024).toFixed(2)} GB`
}
