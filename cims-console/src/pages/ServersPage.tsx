import { useCallback, useEffect, useMemo, useState } from 'react'
import { Link, useSearchParams } from 'react-router-dom'
import {
  deploymentApi,
  type Agent, type SipPackage, type Deployment, type JobType, type AgentMetric, type AgentNetTuning,
} from '../api/deployment'
import { haGroupsApi, type HaGroup, type VipBinding,
         type FailoverOptions, FAILOVER_DEFAULTS } from '../api/ha_groups'
import { ServiceIpPanel } from './ha/ServiceIpPanel'
import { MountPanel } from './ha/MountPanel'
import { NetTuningPanel } from './ha/NetTuningPanel'
import { splitPrefixHost } from './ha/helpers'
import { useToast } from '../components/Toast'
import Modal from '../components/Modal'
import { agentStatusColor, depStatusColor, fmtRelTime } from './deploy/deployHelpers'
import ModuleConfigModal from '../components/module/ModuleConfigModal'
import { GroupServiceConfigModal } from '../components/group/GroupServiceConfigModal'
import HealthCheckModal from '../components/HealthCheckModal'
import MetricTrend from '../components/MetricTrend'
import { agentDisplayName } from '../components/agentDisplay'
import { useAdminCapable } from '../hooks/useAdminCapable'
import AdminElevateDialog from '../components/AdminElevateDialog'
import { clearElevatedToken, elevationActive } from '../api/client'
import { useAuth } from '../contexts/AuthContext'

type Selection =
  | { kind: 'agent'; id: number }
  | { kind: 'group'; id: number }
  | null

// 상단 페이지 탭 — UX 개편 (2026-06-10): 좌측 선택(서버/그룹) 공유 + 우측 내용 분리.
//  infra(시스템/서버 구성)·install(패키지 설치) = 조회 operator+, 변이 admin/승격.
//  config(패키지 설정) = operator+ 편집 가능 (동적 반영 설정).
type PageTab = 'infra' | 'install' | 'config'
const PAGE_TABS: Array<{ key: PageTab; label: string; adminGated: boolean }> = [
  { key: 'infra',   label: '시스템/서버 구성', adminGated: true },
  { key: 'install', label: '패키지 설치',      adminGated: true },
  { key: 'config',  label: '패키지 설정',      adminGated: false },
]
// fieldset 잠금 래퍼 — 내부 input/button 일괄 disable (조회는 가능)
const LOCK_FIELDSET_STYLE: React.CSSProperties = {
  border: 0, margin: 0, padding: 0, minWidth: 0,
  flex: 1, display: 'flex', flexDirection: 'column', overflow: 'hidden',
}

export default function ServersPage() {
  const { show } = useToast()
  const [searchParams] = useSearchParams()
  const initialSelection = ((): Selection => {
    const ag = searchParams.get('agent')
    const gp = searchParams.get('group')
    const agN = ag ? Number(ag) : NaN
    const gpN = gp ? Number(gp) : NaN
    if (Number.isFinite(agN) && agN > 0) return { kind: 'agent', id: agN }
    if (Number.isFinite(gpN) && gpN > 0) return { kind: 'group', id: gpN }
    return null
  })()
  const [agents, setAgents]           = useState<Agent[]>([])
  const [packages, setPackages]       = useState<SipPackage[]>([])
  const [deployments, setDeployments] = useState<Deployment[]>([])
  const [haGroups, setHaGroups]       = useState<HaGroup[]>([])
  const [loading, setLoading]         = useState(true)
  const [selection, setSelection]     = useState<Selection>(initialSelection)
  const [filter, setFilter]           = useState('')
  const [expandedGroups, setExpandedGroups] = useState<Set<number>>(new Set())  // -1 = standalone

  const [systemModalOpen, setSystemModalOpen] = useState(false)
  const [pendingMember, setPendingMember] = useState<{
    groupName: string; serverName: string;
    enrollment_token: string; install_command: string;
  } | null>(null)
  const [deployModal, setDeployModal]       = useState<{ agent: Agent } | null>(null)
  const [metricsFor, setMetricsFor]         = useState<Agent | null>(null)
  const [healthCheckFor, setHealthCheckFor] = useState<Agent | null>(null)
  const [configFor, setConfigFor]           = useState<Deployment | null>(null)
  const [pageTab, setPageTab] = useState<PageTab>(() => {
    const t = searchParams.get('t')
    return (t === 'install' || t === 'config') ? t : 'infra'
  })
  const [elevateOpen, setElevateOpen] = useState(false)
  const { user } = useAuth()
  const canEdit = useAdminCapable()   // admin 세션 또는 admin 승격(sudo) 활성

  const load = useCallback(async () => {
    try {
      const [a, p, d, g] = await Promise.all([
        deploymentApi.listAgents(),
        deploymentApi.listPackages(),
        deploymentApi.listDeployments(),
        haGroupsApi.list(),
      ])
      setAgents(a); setPackages(p); setDeployments(d); setHaGroups(g)
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

  useEffect(() => {
    if (loading) return
    if (agents.length === 0) { setSelection(null); return }
    if (!selection ||
        (selection.kind === 'agent' && !agents.find(a => a.id === selection.id)) ||
        (selection.kind === 'group' && !haGroups.find(g => g.id === selection.id))) {
      setSelection({ kind: 'agent', id: agents[0].id })
    }
  }, [agents, haGroups, selection, loading])

  // 처음 로드 시 모든 group + standalone 펼침
  useEffect(() => {
    if (haGroups.length > 0 && expandedGroups.size === 0) {
      const s = new Set<number>(haGroups.map(g => g.id))
      s.add(-1)  // standalone
      setExpandedGroups(s)
    }
  }, [haGroups, expandedGroups])

  const selectedAgent = useMemo(
    () => selection?.kind === 'agent' ? (agents.find(a => a.id === selection.id) || null) : null,
    [agents, selection]
  )
  const selectedGroup = useMemo(
    () => selection?.kind === 'group' ? (haGroups.find(g => g.id === selection.id) || null) : null,
    [haGroups, selection]
  )
  // 전역 VIP IP 집합 — 모든 HA group vip_bindings 의 IP. keepalived 가 관리하는 부동 IP 라
  // ServiceIpPanel 에서 망/용도 편집 불가, 'VIP' 표시만 (서버 고정 IP 아님).
  const vipIps = useMemo(
    () => new Set(haGroups.flatMap(g => (g.vip_bindings || []).map(b => b.ip)).filter(Boolean)),
    [haGroups]
  )

  // group 별 멤버 분류 + standalone
  const groupedAgents = useMemo(() => {
    const byGroup = new Map<number, Agent[]>()  // -1 = standalone
    const q = filter.trim().toLowerCase()
    const matches = (a: Agent) => !q ||
      a.name.toLowerCase().includes(q) ||
      (a.hostname || '').toLowerCase().includes(q) ||
      (a.ip_address || '').includes(q)
    for (const a of agents) {
      if (!matches(a)) continue
      const gid = a.ha_group?.id ?? -1
      if (!byGroup.has(gid)) byGroup.set(gid, [])
      byGroup.get(gid)!.push(a)
    }
    return byGroup
  }, [agents, filter])

  const toggleGroupExpand = (gid: number) => {
    setExpandedGroups(prev => {
      const next = new Set(prev)
      if (next.has(gid)) next.delete(gid); else next.add(gid)
      return next
    })
  }

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
  async function restartAgent(a: Agent) {
    if (!confirm(`${a.name} 의 agent 프로세스를 재시작할까요?\n(현재 binary 그대로 self-exec — 약 수 초 끊김)`)) return
    try {
      const r = await deploymentApi.restartAgent(a.id)
      show(`재시작 job 큐잉 (#${r.job_id})`, 'ok')
      await load()
    } catch (e) { show((e as Error).message, 'err') }
  }
  async function queueJob(d: Deployment, jt: JobType) {
    // destructive / 서비스 영향 큰 job 은 confirm.
    const destructiveDesc: Partial<Record<JobType, string>> = {
      uninstall: '모듈 파일 + 프로세스 제거 (config 도 같이 삭제됨)',
      stop:      '서비스 프로세스 중단',
      restart:   '서비스 재기동 (단기 다운타임)',
    }
    const desc = destructiveDesc[jt as JobType]
    if (desc) {
      if (!confirm(`${d.package_name} 모듈에 [${jt}] 진행할까요?\n  ${desc}`)) return
    }
    try {
      const r = await deploymentApi.queueJob(d.id, jt)
      show(`${jt} 큐 등록 (#${r.job_id})`, 'ok')
      await load()
    } catch (e) { show((e as Error).message, 'err') }
  }
  async function rollbackDeployment(d: Deployment) {
    const target = d.prev_install_path
    const targetVer = d.prev_package_version
    if (!target) { show('롤백 대상 없음 (이전 버전 설치 이력 없음)', 'err'); return }
    if (!confirm(`${d.package_name} 모듈을 이전 버전으로 롤백할까요?\n\n` +
                 `  현재: v${d.package_version} (${d.install_path})\n` +
                 `  대상: v${targetVer || '?'} (${target})\n\n` +
                 `collection 재동기 후 재기동됩니다 (단기 다운타임)`)) return
    try {
      const r = await deploymentApi.rollbackDeployment(d.id)
      show(`롤백 큐 등록 (restart #${r.restart_job_id} → ${r.install_path})`, 'ok')
      await load()
    } catch (e) { show((e as Error).message, 'err') }
  }
  async function removeDeployment(d: Deployment) {
    if (!confirm(`Deployment #${d.id} (${d.package_name}) 을 제거할까요?`)) return
    try { await deploymentApi.deleteDeployment(d.id); show('삭제됨', 'ok'); await load() }
    catch (e) { show((e as Error).message, 'err') }
  }
  async function deleteSystem(g: HaGroup) {
    const memberNames = g.members.map(m => m.agent_name || `#${m.agent_id}`).join(', ')
    if (!confirm(`시스템 "${g.name}" 을 삭제합니다.\n\n  · HA 그룹 (mode=${g.mode}, vrid=${g.vrid}) 제거\n  · 멤버 ${g.members.length} 개 삭제: ${memberNames || '(없음)'}\n\n계속할까요?`)) return
    try {
      // 1) 모든 멤버 agent 삭제 (관련 deployment 도 cascade)
      for (const m of g.members) {
        try { await deploymentApi.deleteAgent(m.agent_id) }
        catch (e) { console.warn(`agent ${m.agent_id} 삭제 실패:`, e) }
      }
      // 2) HA 그룹 자체 삭제
      await haGroupsApi.delete(g.id)
      show(`시스템 "${g.name}" 삭제됨`, 'ok')
      setSelection(null)
      await load()
    } catch (e) { show((e as Error).message, 'err') }
  }
  async function addMemberToGroup(g: HaGroup) {
    // HaServicesPage 의 addServer 와 동일한 동선 — 새 agent 자동 생성 + 그룹 가입.
    const existing = (g.members || []).length
    const nm = `${g.name}-${String(existing + 1).padStart(2, '0')}`
    try {
      const r = await deploymentApi.createAgent(nm, '')
      await deploymentApi.approveAgent(r.id)
      await haGroupsApi.addMember(g.id, { agent_id: r.id, role: 'backup', priority: 90 })
      setPendingMember({
        groupName: g.name, serverName: nm,
        enrollment_token: r.enrollment_token, install_command: r.install_command,
      })
      // 새 멤버 자동 선택 + 트리에서 그룹 펼침 — InstallSection 즉시 노출.
      setSelection({ kind: 'agent', id: r.id })
      setExpandedGroups(prev => prev.has(g.id) ? prev : new Set(prev).add(g.id))
      await load()
    } catch (e) { show((e as Error).message, 'err') }
  }
  async function removeMemberFromGroup(g: HaGroup, a: Agent) {
    // AA 만 호출 — AS 는 row 에 [×] 없음 (lifecycle 표준: AS 멤버 단독 삭제 차단).
    if (!confirm(`멤버 [${agentDisplayName(a.name)}] 를 그룹 [${g.name}] 에서 제거할까요?\n\n` +
                 `agent 자체는 삭제되지 않고 standalone (SA) 으로 트리에 남습니다.`)) return
    try {
      await haGroupsApi.removeMember(g.id, a.id)
      show(`${a.name} 그룹에서 제거됨`, 'ok')
      await load()
    } catch (e) { show((e as Error).message, 'err') }
  }

  if (loading) return <div className="empty">로딩 중...</div>

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 12, height: 'calc(100vh - 120px)' }}>
      {/* 상단 요약 + 액션 */}
      <div style={{ display: 'flex', gap: 10, alignItems: 'center', flexWrap: 'wrap' }}>
        <StatChip label="서버" value={`${stats.online}/${stats.total}`} sub="online" color="#2ecc71" />
        {stats.pending > 0 &&
          <StatChip label="승인대기" value={stats.pending} color="#f39c12" />}
        <StatChip label="HA 그룹" value={haGroups.length} color="#3498db" />
        <input className="form-input" placeholder="이름/호스트/IP 검색..."
          value={filter} onChange={e => setFilter(e.target.value)}
          style={{ width: 240 }} />
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 8 }}>
          <button className="btn btn--outline" onClick={() => void load()}>↻</button>
          <button className="btn btn--primary" onClick={() => setSystemModalOpen(true)}
                  disabled={!canEdit}
                  title={canEdit ? 'AS 이중화 (서버 2 자동) / AA 다중화 / SA 단일 서버' : 'admin 권한 필요 (관리자 인증)'}>
            ＋ 시스템 추가
          </button>
        </div>
      </div>

      {/* 페이지 탭 — 좌측 선택(서버/그룹) 공유, 우측 내용 전환 */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 2, borderBottom: '2px solid var(--border)' }}>
        {PAGE_TABS.map(t => {
          const active = pageTab === t.key
          const locked = t.adminGated && !canEdit
          return (
            <button key={t.key} onClick={() => setPageTab(t.key)}
                    style={{
                      padding: '9px 20px', fontSize: 13.5, fontWeight: active ? 700 : 400,
                      background: active ? 'var(--surface)' : 'transparent',
                      color: active ? '#1976d2' : 'var(--text-muted)',
                      border: 'none',
                      borderBottom: active ? '2px solid #1976d2' : '2px solid transparent',
                      marginBottom: -2, cursor: 'pointer',
                    }}
                    title={locked ? '조회 가능 — 변경은 admin 권한 필요 (관리자 인증)' : ''}>
              {t.label}{locked && <span style={{ marginLeft: 5, fontSize: 11 }}>🔒</span>}
            </button>
          )
        })}
        <div style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 8, paddingBottom: 4 }}>
          {user?.role !== 'admin' && (
            canEdit && elevationActive() ? (
              <span style={{ fontSize: 12, color: '#27ae60' }}>
                🔓 admin 승격 중
                <button className="btn btn--sm btn--outline" style={{ marginLeft: 6 }}
                        onClick={() => clearElevatedToken()}>해제</button>
              </span>
            ) : (
              <button className="btn btn--sm" onClick={() => setElevateOpen(true)}
                      title="admin 패스워드로 30분 승격 — 시스템 구성/패키지 설치 변경 허용">
                🔐 관리자 인증
              </button>
            )
          )}
        </div>
      </div>

      {/* 좌측 트리 + 우측 Inspector */}
      <div style={{ flex: 1, display: 'flex', gap: 12, overflow: 'hidden' }}>
        {/* 좌측 트리 */}
        <div style={{
          flex: '0 0 320px', overflow: 'auto',
          border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)',
        }}>
          <ServerTree
            haGroups={haGroups}
            groupedAgents={groupedAgents}
            depsByAgent={depsByAgent}
            expanded={expandedGroups}
            onToggleExpand={toggleGroupExpand}
            selection={selection}
            onSelect={setSelection}
            onAddMember={addMemberToGroup}
            onRemoveMember={removeMemberFromGroup} />
        </div>
        {/* 우측 Inspector */}
        <div style={{
          flex: 1, overflow: 'hidden', display: 'flex', flexDirection: 'column',
          border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)',
        }}>
          {selectedAgent ? (
            pageTab === 'config' ? (
              <AgentConfigTab key={`${selectedAgent.id}:${packages.length > 0}`}
                agent={selectedAgent}
                deployments={depsByAgent.get(selectedAgent.id) || []}
                onDone={load} />
            ) : (
              // infra/install: 조회는 operator+, 변이는 admin/승격 — fieldset 일괄 잠금
              <fieldset disabled={!canEdit} style={LOCK_FIELDSET_STYLE}>
                <ServerInspector agent={selectedAgent} mode={pageTab}
                  deployments={depsByAgent.get(selectedAgent.id) || []}
                  packages={packages}
                  vipIps={vipIps}
                  onApprove={approveAgent}
                  onRevoke={revokeAgent}
                  onRemove={removeAgent}
                  onUpgrade={upgradeAgent}
                  onRestart={restartAgent}
                  onMetrics={setMetricsFor}
                  onHealthCheck={setHealthCheckFor}
                  onAddDeploy={() => setDeployModal({ agent: selectedAgent })}
                  onConfigure={setConfigFor}
                  onJob={queueJob}
                  onRollback={rollbackDeployment}
                  onRemoveDep={removeDeployment} />
              </fieldset>
            )
          ) : selectedGroup ? (
            pageTab === 'config' ? (
              <GroupServiceConfigModal key={`${selectedGroup.id}:${packages.length > 0}`}
                open inline
                onClose={() => { /* inline — 닫기 없음 */ }}
                groupName={selectedGroup.name}
                members={selectedGroup.members.map(m => ({
                  id: m.agent_id,
                  name: m.agent_name || agents.find(a => a.id === m.agent_id)?.name || `#${m.agent_id}`,
                }))}
                deployments={deployments}
                packages={packages}
                haMode={selectedGroup.mode}
                onApplied={async () => { await load() }} />
            ) : pageTab === 'install' ? (
              <GroupInstallOverview group={selectedGroup} agents={agents}
                depsByAgent={depsByAgent}
                onSelectMember={(aid) => setSelection({ kind: 'agent', id: aid })} />
            ) : (
              <fieldset disabled={!canEdit} style={LOCK_FIELDSET_STYLE}>
                <GroupInspector group={selectedGroup} agents={agents}
                  onSelectMember={(aid) => setSelection({ kind: 'agent', id: aid })}
                  onReload={load}
                  onOpenConfig={() => setPageTab('config')}
                  onDeleteSystem={deleteSystem} />
              </fieldset>
            )
          ) : (
            <div className="empty" style={{ padding: 40 }}>
              왼쪽 트리에서 서버 또는 HA 그룹을 선택하세요
            </div>
          )}
        </div>
      </div>

      {systemModalOpen &&
        <SystemCreateModal
          onClose={() => setSystemModalOpen(false)}
          onDone={load}
          onCreated={(firstAgentId) => {
            if (firstAgentId) {
              setSelection({ kind: 'agent', id: firstAgentId })
              // standalone 또는 새 그룹 자동 펼침 — 새 멤버 트리에서 노출.
              setExpandedGroups(prev => {
                const next = new Set(prev)
                next.add(-1)  // standalone
                return next
              })
            }
          }} />}
      {pendingMember &&
        <PendingMemberModal info={pendingMember} onClose={() => setPendingMember(null)} />}
      {deployModal &&
        <DeploymentCreateModal agent={deployModal.agent} packages={packages}
          onClose={() => setDeployModal(null)} onDone={load} />}
      {metricsFor &&
        <MetricsModal agent={metricsFor} onClose={() => setMetricsFor(null)} />}
      {healthCheckFor &&
        <HealthCheckModal agents={[healthCheckFor]} onClose={() => setHealthCheckFor(null)} />}
      {configFor &&
        <ModuleConfigModal source={{ type: 'deployment', deployment: configFor }}
          onClose={() => setConfigFor(null)} onDone={load} />}
      {elevateOpen &&
        <AdminElevateDialog onClose={() => setElevateOpen(false)} />}
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  Tree (좌측 패널) — HA group + standalone 계층
// ──────────────────────────────────────────────────────────────

function ServerTree({ haGroups, groupedAgents, depsByAgent, expanded,
                      onToggleExpand, selection, onSelect, onAddMember, onRemoveMember }: {
  haGroups: HaGroup[]
  groupedAgents: Map<number, Agent[]>
  depsByAgent: Map<number, Deployment[]>
  expanded: Set<number>
  onToggleExpand: (gid: number) => void
  selection: Selection
  onSelect: (s: Selection) => void
  onAddMember: (g: HaGroup) => void
  onRemoveMember: (g: HaGroup, a: Agent) => void   // AA 만 호출됨 (AS 는 row 에 [×] 없음)
}) {
  const standalone = groupedAgents.get(-1) || []
  return (
    <div style={{ fontSize: 13 }}>
      {/* HA groups */}
      {haGroups.map(g => {
        const members = groupedAgents.get(g.id) || []
        const isOpen = expanded.has(g.id)
        const isSelected = selection?.kind === 'group' && selection.id === g.id
        const modeChip = g.mode === 'active_standby' ? 'AS' : 'AA'
        const modeColor = g.mode === 'active_standby' ? '#3498db' : '#27ae60'
        const canAddMember = g.mode === 'all_active'  // AS 는 master/backup 2 fixed
        return (
          <div key={g.id}>
            <div onClick={() => onSelect({ kind: 'group', id: g.id })}
                 style={{
                   display: 'flex', alignItems: 'center', gap: 6, padding: '8px 10px',
                   borderBottom: '1px solid #eee', cursor: 'pointer',
                   background: isSelected ? '#eef5ff' : '#fafafa',
                 }}>
              <span onClick={e => { e.stopPropagation(); onToggleExpand(g.id) }}
                    style={{ width: 14, color: 'var(--text-muted)' }}>{isOpen ? '▼' : '▶'}</span>
              <span style={{
                background: modeColor, color: '#fff', fontSize: 10,
                padding: '1px 5px', borderRadius: 3,
              }}>{modeChip}</span>
              <b style={{ flex: 1 }}>{g.name}</b>
              {g.vip && (
                <span style={{ fontSize: 10, color: 'var(--text-muted)' }} title={`VIP ${g.vip}/${g.vip_mask}`}>
                  VIP {g.vip}
                </span>
              )}
              <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>{members.length}</span>
              {canAddMember && (
                <button onClick={e => { e.stopPropagation(); onAddMember(g) }}
                        title="새 멤버 자동 생성 (이름 자동, install_command 발급)"
                        style={{
                          border: '1px solid #b8d4f5', background: 'var(--surface)', color: '#3498db',
                          fontSize: 11, padding: '0 6px', borderRadius: 3, cursor: 'pointer',
                          fontWeight: 600,
                        }}>+</button>
              )}
            </div>
            {isOpen && members.map(a => (
              <ServerTreeRow key={a.id} agent={a}
                depCount={(depsByAgent.get(a.id) || []).length}
                role={g.mode === 'active_standby' ? a.ha_group?.role : undefined}
                active={selection?.kind === 'agent' && selection.id === a.id}
                indent
                onClick={() => onSelect({ kind: 'agent', id: a.id })}
                onRemove={g.mode === 'all_active' ? () => onRemoveMember(g, a) : undefined} />
            ))}
          </div>
        )
      })}
      {/* Standalone — 그룹화 없이 각자 시스템 row */}
      {standalone.map(a => {
        const isSelected = selection?.kind === 'agent' && selection.id === a.id
        const sc = agentStatusColor(a.status)
        return (
          <div key={`sa-${a.id}`}
               onClick={() => onSelect({ kind: 'agent', id: a.id })}
               style={{
                 display: 'flex', alignItems: 'center', gap: 6, padding: '8px 10px',
                 borderBottom: '1px solid #eee', cursor: 'pointer',
                 background: isSelected ? '#eef5ff' : '#fafafa',
               }}>
            <span style={{ width: 14 }} />  {/* expand 자리 비움 — group 정렬 맞춤 */}
            <span style={{
              background: '#95a5a6', color: '#fff', fontSize: 10,
              padding: '1px 5px', borderRadius: 3,
            }}>SA</span>
            <b style={{ flex: 1 }}>{agentDisplayName(a.name)}</b>
            <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>{a.ip_address || '—'}</span>
            <span style={{ width: 8, height: 8, borderRadius: '50%', background: sc.bar,
                            display: 'inline-block', marginLeft: 4 }} />
            <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>
              {(depsByAgent.get(a.id) || []).length}m
            </span>
          </div>
        )
      })}
    </div>
  )
}

function ServerTreeRow({ agent: a, depCount, role, active, indent, onClick, onRemove }: {
  agent: Agent
  depCount: number
  role?: 'master' | 'backup'
  active: boolean
  indent?: boolean
  onClick: () => void
  onRemove?: () => void   // AA 멤버만 제공 — 그룹에서 멤버 제거 (agent 자체는 standalone 으로 남음)
}) {
  const sc = agentStatusColor(a.status)
  return (
    <div onClick={onClick}
         style={{
           display: 'flex', alignItems: 'center', gap: 6,
           padding: '6px 10px', paddingLeft: indent ? 32 : 10,
           borderBottom: '1px solid #f4f4f4', cursor: 'pointer',
           background: active ? '#eef5ff' : undefined,
         }}>
      <span style={{ width: 8, height: 8, borderRadius: '50%', background: sc.bar }} />
      <span style={{ flex: 1, fontWeight: active ? 600 : 400 }}>{agentDisplayName(a.name)}</span>
      {role && (
        <span title={role === 'master' ? 'Master — priority 100 (절체 우선순위)' : 'Backup — priority 90'}
              style={{
                fontSize: 10, padding: '1px 5px', borderRadius: 3, fontWeight: 600,
                background: role === 'master' ? '#3498db' : '#95a5a6', color: '#fff',
              }}>{role === 'master' ? 'M' : 'B'}</span>
      )}
      <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>{depCount}m</span>
      {onRemove && (
        <button onClick={e => { e.stopPropagation(); onRemove() }}
                title="그룹에서 멤버 제거 (agent 자체는 standalone 으로 유지)"
                style={{
                  border: '1px solid #f5b8b8', background: 'var(--surface)', color: '#e74c3c',
                  fontSize: 10, padding: '0 5px', borderRadius: 3, cursor: 'pointer',
                  fontWeight: 600,
                }}>×</button>
      )}
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  Group Inspector (HA 그룹 선택 시)
// ──────────────────────────────────────────────────────────────

function GroupInspector({ group, agents, onSelectMember, onReload, onOpenConfig, onDeleteSystem }: {
  group: HaGroup
  agents: Agent[]
  onSelectMember: (aid: number) => void
  onReload: () => Promise<void>
  onOpenConfig: () => void
  onDeleteSystem: (g: HaGroup) => void
}) {
  const { show } = useToast()
  const [editName, setEditName]         = useState(group.name)
  // mode 는 readonly — 생성 후 변경 불가 (변경 원하면 시스템 삭제 후 재생성).
  const [editAuthPass, setEditAuthPass] = useState(group.auth_pass)
  const [editNote, setEditNote]         = useState(group.note || '')
  // 백엔드 vip_bindings 에는 bid 가 없음 → 안정적 bid 부여(누락 시 removeBinding 이 전체 삭제됨).
  const [editBindings, setEditBindings] = useState<VipBinding[]>((group.vip_bindings || []).map((b, i) => ({ ...b, bid: b.bid ?? i + 1 })))
  const [editFailover, setEditFailover] = useState<FailoverOptions>(
    { ...FAILOVER_DEFAULTS, ...(group.failover_options || {}),
      health: { ...FAILOVER_DEFAULTS.health, ...(group.failover_options?.health || {}) } })
  const [failoverOpen, setFailoverOpen] = useState(false)
  // Master 멤버 1명 선택 — AS 만 의미. 현재 priority 가 가장 큰 멤버를 default 로.
  const initialMaster = (() => {
    if (group.members.length === 0) return null
    return [...group.members].sort(
      (a, b) => (b.priority - a.priority) || (a.agent_id - b.agent_id)
    )[0].agent_id
  })()
  const [editMasterAid, setEditMasterAid] = useState<number | null>(initialMaster)
  // A/S 실측 결과 — 멤버 agent.id → 관측된 VIP 보유 상태. checkVipHolders() 가 채움.
  const [vipObs, setVipObs] = useState<Record<number, 'active' | 'standby' | 'fail'>>({})
  const [vipChecking, setVipChecking] = useState(false)
  // group prop 이 바뀌면 (다른 group 선택 또는 reload) state 재설정.
  useEffect(() => {
    setEditName(group.name)
    setEditAuthPass(group.auth_pass)
    setEditNote(group.note || '')
    setEditBindings((group.vip_bindings || []).map((b, i) => ({ ...b, bid: b.bid ?? i + 1 })))
    setEditFailover({ ...FAILOVER_DEFAULTS, ...(group.failover_options || {}),
      health: { ...FAILOVER_DEFAULTS.health, ...(group.failover_options?.health || {}) } })
    if (group.members.length > 0) {
      setEditMasterAid([...group.members].sort(
        (a, b) => (b.priority - a.priority) || (a.agent_id - b.agent_id)
      )[0].agent_id)
    } else {
      setEditMasterAid(null)
    }
    setVipObs({})   // 다른 group 선택/reload 시 실측 결과 초기화 (stale 표시 방지)
  }, [group.id, group.update_time])

  const memberAgents = group.members.map(m => ({
    ...m, agent: agents.find(a => a.id === m.agent_id)
  }))

  // dirty 검출 — 영역별로 분리. 각 영역의 [▶ 적용] 이 자체 dirty 만 보고 활성.
  const metaDirty = editName !== group.name
    || editAuthPass !== group.auth_pass
    || editNote !== (group.note || '')
  const failoverDirty = JSON.stringify(editFailover) !== JSON.stringify(group.failover_options || FAILOVER_DEFAULTS)
  const vipDirty = JSON.stringify(editBindings) !== JSON.stringify(group.vip_bindings || [])
  const masterChanged = editMasterAid !== initialMaster

  // 영역별 적용 — 그 영역의 변경만 backend 로 push. backend 의 _update_group 은 부분 업데이트
  // 지원 + _enqueue_update_ha_for_members 자동 호출 → 멤버 agent 의 keepalived 즉시 반영.
  async function applyMeta() {
    if (!metaDirty) return
    try {
      await haGroupsApi.update(group.id, {
        name: editName,
        auth_pass: group.mode === 'active_standby' ? editAuthPass : '',
        note: editNote,
      })
      show('메타 적용됨', 'ok'); await onReload()
    } catch (e) { show((e as Error).message, 'err') }
  }
  async function applyFailover() {
    if (!failoverDirty) return
    try {
      await haGroupsApi.update(group.id, { failover_options: editFailover })
      show('절체 조건 적용됨', 'ok'); await onReload()
    } catch (e) { show((e as Error).message, 'err') }
  }
  async function applyVipBindings() {
    if (!vipDirty) return
    try {
      await haGroupsApi.update(group.id, { vip_bindings: editBindings })
      show('VIP 적용됨', 'ok'); await onReload()
    } catch (e) { show((e as Error).message, 'err') }
  }
  async function applyMembers() {
    // Master 선택 변경 → 해당 멤버 priority=100, 나머지=90. AS 에만 의미.
    if (!masterChanged || editMasterAid === null) return
    try {
      for (const m of group.members) {
        const newPrio = m.agent_id === editMasterAid ? 100 : 90
        if (newPrio !== m.priority) {
          await haGroupsApi.addMember(group.id, { agent_id: m.agent_id, priority: newPrio })
        }
      }
      show('멤버 적용됨', 'ok'); await onReload()
    } catch (e) { show((e as Error).message, 'err') }
  }
  // ── A/S 실측 — 멤버별 health-check (sync REST) 로 실제 VIP 보유(Active) 여부 관측 ──
  // 설정상 role(M/B) 과 달리, 절체 직후엔 실제 VIP 보유 멤버가 바뀔 수 있어 on-demand 로 확인.
  async function checkVipHolders() {
    const vipIps = new Set(editBindings.map(b => b.ip).filter(Boolean))
    if (vipIps.size === 0) { show('확인할 VIP 가 없습니다 — VIP binding 을 먼저 설정하세요', 'err'); return }
    setVipChecking(true)
    const next: Record<number, 'active' | 'standby' | 'fail'> = {}
    await Promise.all(memberAgents.map(async (m) => {
      const ag = m.agent
      if (!ag || ag.status !== 'online') { next[m.agent_id] = 'fail'; return }
      try {
        const hc = await deploymentApi.healthCheck(ag.id, 'ha')
        next[m.agent_id] = (hc.ha?.vips || []).some(v => vipIps.has(v.ip)) ? 'active' : 'standby'
      } catch { next[m.agent_id] = 'fail' }
    }))
    setVipObs(next)
    setVipChecking(false)
    const active = Object.values(next).filter(s => s === 'active').length
    show(`VIP 실측 완료 — Active ${active}명 / ${memberAgents.length}명`, active === 1 ? 'ok' : 'err')
  }

  // VIP 행 편집 모드 — bid 또는 'new' (새 binding 추가 중). null = 모두 readonly.
  const [bindingEditMode, setBindingEditMode] = useState<number | 'new' | null>(null)
  function updateBinding(bid: number, patch: Partial<VipBinding>) {
    setEditBindings(editBindings.map(b => b.bid === bid ? { ...b, ...patch } : b))
  }
  function removeBinding(bid: number) {
    const b = editBindings.find(x => x.bid === bid)
    if (!b) return
    const desc = `${b.slot || '(slot 미지정)'} — ${b.ip || '(IP 미입력)'}${b.mask ? `/${b.mask}` : ''}`
    if (!confirm(`VIP binding 을 제거할까요?\n  ${desc}\n\n저장하기 전 까지는 적용되지 않습니다.`)) return
    setEditBindings(editBindings.filter(x => x.bid !== bid))
    if (bindingEditMode === bid) setBindingEditMode(null)
  }
  // 멤버들의 service_ip_rows 에서 slot 매핑 추출. slot → (agentId → {iface, ip, mask}).
  const slotMap = (() => {
    const m = new Map<string, Map<number, { iface: string; ip: string; mask: number }>>()
    for (const memberAg of memberAgents) {
      const ag = memberAg.agent
      if (!ag) continue
      for (const r of (ag.service_ip_rows || [])) {
        if (!r.slot) continue
        if (!m.has(r.slot)) m.set(r.slot, new Map())
        m.get(r.slot)!.set(ag.id, { iface: r.iface, ip: r.ip, mask: r.mask })
      }
    }
    return m
  })()
  const availableSlots = Array.from(slotMap.keys()).sort()
  // slot 의 subnet 정합 — 모든 멤버 IP 의 prefix/mask 동일하면 prefix 반환.
  function slotSubnetInfo(slot: string): { prefix: string | null; mask: number; conflict: boolean; conflictDetail: string } {
    const m = slotMap.get(slot)
    if (!m || m.size === 0) return { prefix: null, mask: 24, conflict: false, conflictDetail: '' }
    const entries = Array.from(m.values())
    const first = splitPrefixHost(entries[0].ip, entries[0].mask)
    if (!first) return { prefix: null, mask: entries[0].mask, conflict: true,
                         conflictDetail: `비표준 mask=${entries[0].mask}` }
    for (const e of entries.slice(1)) {
      const p = splitPrefixHost(e.ip, e.mask)
      if (!p || p.prefix !== first.prefix || e.mask !== entries[0].mask) {
        return { prefix: first.prefix, mask: entries[0].mask, conflict: true,
                 conflictDetail: `${entries[0].ip}/${entries[0].mask} ≠ ${e.ip}/${e.mask}` }
      }
    }
    return { prefix: first.prefix, mask: entries[0].mask, conflict: false, conflictDetail: '' }
  }
  function autoMemberIfaces(slot: string): { [id: number]: string } {
    const result: { [id: number]: string } = {}
    const m = slotMap.get(slot)
    if (m) for (const [aid, info] of m) result[aid] = info.iface
    return result
  }
  function beginAddBinding() {
    const newBid = Math.max(0, ...editBindings.map(b => b.bid)) + 1
    const defaultSlot = availableSlots[0] || ''
    const info = defaultSlot ? slotSubnetInfo(defaultSlot) : { prefix: null, mask: 24, conflict: false }
    setEditBindings([...editBindings, {
      bid: newBid, slot: defaultSlot,
      ip: info.prefix || '',
      mask: info.mask,
      status: 'unknown',
      memberIfaces: defaultSlot ? autoMemberIfaces(defaultSlot) : {},
    }])
    setBindingEditMode(newBid)
  }
  // slot 변경 시 prefix/mask/memberIfaces 자동 갱신, host 보존 시도.
  function changeBindingSlot(bid: number, newSlot: string) {
    const info = slotSubnetInfo(newSlot)
    const b = editBindings.find(x => x.bid === bid)
    if (!b) return
    const oldHost = splitPrefixHost(b.ip, b.mask ?? 24)?.host || ''
    updateBinding(bid, {
      slot: newSlot,
      ip:   info.prefix ? info.prefix + oldHost : '',
      mask: info.mask,
      memberIfaces: autoMemberIfaces(newSlot),
    })
  }
  function changeBindingHost(bid: number, newHost: string) {
    const b = editBindings.find(x => x.bid === bid)
    if (!b) return
    const info = b.slot ? slotSubnetInfo(b.slot) : { prefix: null, mask: b.mask }
    if (!info.prefix) {
      updateBinding(bid, { ip: newHost })  // fallback — slot 없으면 raw 그대로
      return
    }
    updateBinding(bid, { ip: info.prefix + newHost })
  }

  return (
    <>
      <div style={{ padding: '12px 16px', borderBottom: '1px solid #eee' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
          {/* 유형 변경 불가 — 변경 원하면 [🗑 시스템 삭제] 후 [+ 시스템 추가] 재생성. */}
          <span title={`mode=${group.mode} (생성 후 변경 불가)`}
                style={{
                  background: group.mode === 'active_standby' ? '#3498db' : '#27ae60',
                  color: '#fff', fontSize: 11, padding: '4px 10px', borderRadius: 3, fontWeight: 600,
                }}>{group.mode === 'active_standby' ? 'AS' : 'AA'}</span>
          <input className="form-input" value={editName} onChange={e => setEditName(e.target.value)}
                 style={{ flex: 1, minWidth: 180 }} />
          <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>#{group.id} · vrid {group.vrid}</span>
          <div style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
            <button className="btn btn--sm" onClick={onOpenConfig}
                    title="그룹 멤버 공통 서비스 설정 — 모듈별 탭, 동적 반영(scope=service) 항목 전용">
              ⚙ 그룹 설정
            </button>
            <button className="btn btn--sm btn--danger" onClick={() => onDeleteSystem(group)}
                    title="HA 그룹 + 모든 멤버 일괄 삭제">
              🗑 시스템 삭제
            </button>
          </div>
        </div>
        <div style={{ display: 'flex', gap: 10, alignItems: 'center', marginTop: 8, fontSize: 12 }}>
          {group.mode === 'active_standby' && (
            <>
              <label style={{ color: 'var(--text-muted)' }}>auth_pass:</label>
              <input type="password" className="form-input" value={editAuthPass}
                     onChange={e => setEditAuthPass(e.target.value)}
                     maxLength={8}
                     style={{ width: 140 }}
                     title="VRRP 인증 (active_standby 만 사용, 최대 8글자)" />
            </>
          )}
          <label style={{ color: 'var(--text-muted)' }}>note:</label>
          <input className="form-input" value={editNote} onChange={e => setEditNote(e.target.value)}
                 style={{ flex: 1 }} />
          <button className="btn btn--sm btn--primary" onClick={applyMeta} disabled={!metaDirty}
                  title="이름/auth_pass/note 변경을 backend 에 적용 (즉시 keepalived 반영)">
            ▶ 적용
          </button>
        </div>
      </div>
      <div style={{ flex: 1, overflow: 'auto', padding: 16 }}>
        {/* 절체 조건 — 그룹 단위 설정. 자체 [▶ 적용] 으로 그 영역만 backend push. AS 만. */}
        {group.mode === 'active_standby' && (
          <div style={{ marginBottom: 20 }}>
            <FailoverSection
              value={editFailover}
              onChange={setEditFailover}
              open={failoverOpen}
              onToggle={() => setFailoverOpen(v => !v)}
              dirty={failoverDirty}
              onApply={applyFailover}
            />
          </div>
        )}

        {/* 멤버 — 추가/삭제는 좌측 트리에서 일괄 처리 (트리의 [+] / [×]).
            여기는 표시 + AS 의 Master 선택만 담당. */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 8 }}>
          <div style={{ fontWeight: 600 }}>멤버 ({memberAgents.length})</div>
          <span style={{ fontSize: 11, color: 'var(--text-muted)', marginLeft: 8 }}>
            추가/삭제는 좌측 트리에서
          </span>
          <div style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
            {group.mode === 'active_standby' && (
              <button className="btn btn--sm" onClick={checkVipHolders} disabled={vipChecking}
                      title="멤버별 health-check 로 실제 VIP 보유(Active) 상태를 관측 (sync REST — 수 초 소요)">
                {vipChecking ? '점검 중…' : '🔄 실측'}
              </button>
            )}
            {group.mode === 'active_standby' && (
              <button className="btn btn--sm btn--primary" onClick={applyMembers} disabled={!masterChanged}
                      title="Master 변경을 backend 에 적용 (priority swap + keepalived 즉시 반영)">
                ▶ 적용
              </button>
            )}
          </div>
        </div>
        <table className="data-table" style={{ margin: 0, fontSize: 13 }}>
          <thead>
            <tr><th>이름</th>
                {group.mode === 'active_standby' && (
                  <>
                    <th style={{ width: 60 }} title="Master 선택 — 절체 시 우선순위가 가장 높은 노드. 1명만 선택 가능.">Master</th>
                    <th style={{ width: 50 }} title="설정상 역할 (Master/Backup). priority 의 결과 — Master 선택 결과.">설정</th>
                    <th style={{ width: 50 }} title="현재 실제 상태 (Active/Standby). VIP 를 실제로 보유 중인지. 절체 직후엔 설정과 다를 수 있음.">상태</th>
                  </>
                )}
                <th>접속</th><th>IP</th><th>v</th></tr>
          </thead>
          <tbody>
            {memberAgents.map(m => {
              const a = m.agent
              const colCount = group.mode === 'active_standby' ? 7 : 4
              if (!a) return (
                <tr key={m.agent_id}><td colSpan={colCount}>(agent #{m.agent_id} not found)</td></tr>
              )
              const sc = agentStatusColor(a.status)
              const isMasterSel = editMasterAid === a.id
              return (
                <tr key={a.id}>
                  <td onClick={() => onSelectMember(a.id)} style={{ cursor: 'pointer' }}>
                    <b>{agentDisplayName(a.name)}</b>
                  </td>
                  {group.mode === 'active_standby' && (
                    <>
                      <td style={{ textAlign: 'center' }}>
                        <input type="radio" name={`master-${group.id}`}
                               checked={isMasterSel}
                               onChange={() => setEditMasterAid(a.id)}
                               title="이 멤버를 Master 로 설정 (priority 100, 나머지 90)" />
                      </td>
                      <td>
                        <span title={isMasterSel ? 'Master — 절체 우선순위 100' : 'Backup — 절체 우선순위 90'}
                              style={{
                                background: isMasterSel ? '#3498db' : '#95a5a6',
                                color: '#fff', fontSize: 10, padding: '1px 6px',
                                borderRadius: 3, fontWeight: 600,
                              }}>{isMasterSel ? 'M' : 'B'}</span>
                      </td>
                      <td>
                        {/* A/S = 실제 VIP 보유 (관측값) — [🔄 실측] 으로 health-check 후 채워짐. */}
                        {(() => {
                          const o = vipObs[a.id]
                          if (o === 'active') return (
                            <span title="VIP 실제 보유 — Active"
                                  style={{ fontSize: 11, color: '#27ae60', fontWeight: 600 }}>● Active</span>)
                          if (o === 'standby') return (
                            <span title="VIP 미보유 — Standby"
                                  style={{ fontSize: 11, color: 'var(--text-muted)' }}>○ Standby</span>)
                          if (o === 'fail') return (
                            <span title="점검 실패 — offline 또는 health-check 오류"
                                  style={{ fontSize: 11, color: '#c0392b' }}>✕</span>)
                          if (vipChecking) return (
                            <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>…</span>)
                          return (
                            <span title="미점검 — 상단 [🔄 실측] 버튼으로 확인"
                                  style={{ fontSize: 10, color: 'var(--text-muted)' }}>—</span>)
                        })()}
                      </td>
                    </>
                  )}
                  <td>
                    <span style={{ background: sc.bar, color: '#fff', fontSize: 10,
                                    padding: '1px 6px', borderRadius: 3 }}>{a.status}</span>
                  </td>
                  <td style={{ fontSize: 12, color: 'var(--text-muted)' }}>{a.ip_address || '—'}</td>
                  <td style={{ fontSize: 12, color: 'var(--text-muted)' }}>{a.agent_version || '—'}</td>
                </tr>
              )
            })}
          </tbody>
        </table>

        {/* VIP Bindings */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginTop: 20, marginBottom: 8 }}>
          <div style={{ fontWeight: 600 }}>VIP Bindings ({editBindings.length})</div>
          <div style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
            <button className="btn btn--sm" onClick={beginAddBinding}
                    disabled={availableSlots.length === 0 || bindingEditMode !== null}
                    title={availableSlots.length === 0
                      ? '먼저 멤버 서버의 [네트워크] 탭에서 IP 의 용도를 입력하세요'
                      : '새 VIP 행 추가 (편집 모드 — 저장 후 [▶ 적용] 로 backend 반영)'}>
              + VIP 추가
            </button>
            <button className="btn btn--sm btn--primary" onClick={applyVipBindings}
                    disabled={!vipDirty || bindingEditMode !== null}
                    title={bindingEditMode !== null
                      ? '편집 중인 행을 먼저 [저장] 또는 [×] 로 닫으세요'
                      : 'VIP 변경을 backend 에 적용 (즉시 keepalived 반영)'}>
              ▶ 적용
            </button>
          </div>
        </div>
        {editBindings.length === 0 ? (
          <div className="empty" style={{ padding: 12, fontSize: 12, color: 'var(--text-muted)' }}>
            VIP 없음 — all_active 그룹은 비워둬도 됨 (keepalived 안 깔림). active_standby 는 1개 이상 권장.
            {availableSlots.length === 0 && (
              <div style={{ marginTop: 6, color: '#e67e22' }}>
                ⚠ 멤버 서버에 용도(service IP) 가 없습니다 — 멤버의 [네트워크] 탭에서 IP 별 용도 입력 필요.
              </div>
            )}
          </div>
        ) : (
          <table className="data-table" style={{ margin: 0, fontSize: 12 }}>
            <thead>
              <tr><th>용도</th><th>VIP (네트워크 + host)</th><th>mask</th>
                  <th>멤버 iface</th><th style={{ width: 100 }}>액션</th></tr>
            </thead>
            <tbody>
              {editBindings.map(b => {
                const isEditing = bindingEditMode === b.bid
                const info = b.slot ? slotSubnetInfo(b.slot) : null
                const host = splitPrefixHost(b.ip, b.mask ?? 24)?.host ?? ''
                const ifaceStr = Object.entries(b.memberIfaces || {})
                  .map(([sid, iface]) => `#${sid}:${iface}`).join(', ') || '—'
                if (!isEditing) {
                  return (
                    <tr key={b.bid}>
                      <td><b>{b.slot || '(미지정)'}</b></td>
                      <td style={{ fontFamily: 'monospace' }}>{b.ip || '—'}</td>
                      <td>{b.mask || 24}</td>
                      <td style={{ fontSize: 11, color: 'var(--text-muted)' }}>{ifaceStr}</td>
                      <td>
                        <button className="btn btn--sm" style={{ fontSize: 10, padding: '1px 5px' }}
                                disabled={bindingEditMode !== null}
                                onClick={() => setBindingEditMode(b.bid)}>✎ 수정</button>
                        <button className="btn btn--sm btn--danger"
                                style={{ fontSize: 10, padding: '1px 5px', marginLeft: 4 }}
                                disabled={bindingEditMode !== null}
                                onClick={() => removeBinding(b.bid)}>×</button>
                      </td>
                    </tr>
                  )
                }
                // edit mode
                return (
                  <tr key={b.bid} style={{ background: 'var(--warn-soft)' }}>
                    <td>
                      <select className="form-input" value={b.slot}
                              onChange={e => changeBindingSlot(b.bid, e.target.value)}
                              style={{ width: 110, fontSize: 11, padding: 2 }}>
                        <option value="">(용도 선택)</option>
                        {availableSlots.map(s => <option key={s} value={s}>{s}</option>)}
                      </select>
                    </td>
                    <td>
                      {info?.prefix ? (
                        <span style={{ display: 'inline-flex', alignItems: 'center', gap: 2 }}>
                          <span style={{ fontFamily: 'monospace', color: 'var(--text-muted)' }}>{info.prefix}</span>
                          <input className="form-input" value={host}
                                 onChange={e => changeBindingHost(b.bid, e.target.value)}
                                 placeholder="host"
                                 style={{ width: 60, fontSize: 11, padding: 2,
                                          fontFamily: 'monospace' }} />
                        </span>
                      ) : (
                        <span style={{ fontSize: 11, color: '#e67e22' }}>
                          {b.slot ? (info?.conflictDetail || '용도의 멤버 IP 가 같은 네트워크 아님')
                                  : '(용도 선택 필요)'}
                        </span>
                      )}
                    </td>
                    <td style={{ fontFamily: 'monospace' }}>{b.mask || 24}</td>
                    <td style={{ fontSize: 11, color: 'var(--text-muted)' }}>{ifaceStr}</td>
                    <td>
                      <button className="btn btn--sm btn--primary"
                              style={{ fontSize: 10, padding: '1px 5px' }}
                              disabled={!b.slot || !host || !info?.prefix}
                              onClick={() => setBindingEditMode(null)}>저장</button>
                      <button className="btn btn--sm btn--danger"
                              style={{ fontSize: 10, padding: '1px 5px', marginLeft: 4 }}
                              onClick={() => removeBinding(b.bid)}>×</button>
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        )}
        <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 8 }}>
          VIP 의 네트워크/마스크 는 멤버의 용도(service IP) 에서 자동 매핑 — host (마지막 옥텟) 만 입력.
          상세 (수동 IP 입력) → <Link to={`/deploy/services?group=${group.id}`}>📋 상세 편집</Link>.
        </div>
      </div>
    </>
  )
}

// AS 절체 조건 — keepalived advert_int / vrrp_script health / preempt / track_interface / tracked_modules.
// 모든 default = 현재 hardcoded 와 동일 (호환성 보장).
const TRACKABLE_MODULE_CANDIDATES = ['csp', 'cmp', 'csc', 'psp', 'isp', 'imp', 'pmp']

function FailoverSection({ value, onChange, open, onToggle, dirty, onApply }: {
  value: FailoverOptions
  onChange: (v: FailoverOptions) => void
  open: boolean
  onToggle: () => void
  dirty: boolean
  onApply: () => void
}) {
  const set = <K extends keyof FailoverOptions>(k: K, v: FailoverOptions[K]) =>
    onChange({ ...value, [k]: v })
  const setHealth = (k: keyof FailoverOptions['health'], v: number) =>
    onChange({ ...value, health: { ...value.health, [k]: v } })
  const toggleMod = (mod: string) => {
    const cur = value.tracked_modules || []
    const next = cur.includes(mod) ? cur.filter(x => x !== mod) : [...cur, mod]
    set('tracked_modules', next)
  }
  return (
    <div style={{ marginTop: 0, border: '1px solid #e0e0e0', borderRadius: 4 }}>
      <div style={{ padding: '8px 12px', background: 'var(--bg-soft)',
                    display: 'flex', alignItems: 'center', gap: 8, fontWeight: 600, fontSize: 13 }}
           title="A/S (active_standby) 시스템에만 적용 — VRRP 절체 동작 세부 조건">
        <span onClick={onToggle} style={{ fontSize: 11, cursor: 'pointer' }}>{open ? '▼' : '▶'}</span>
        <span onClick={onToggle} style={{ cursor: 'pointer' }}>절체 조건 (A/S 전용)</span>
        <span style={{ fontSize: 11, color: 'var(--text-muted)', fontWeight: 400, cursor: 'pointer' }} onClick={onToggle}>
          감시주기 {value.advert_int}s · 장애판정 {value.health.fall}회 · {value.preempt === 'preempt' ? '자동복귀' : '복귀없음'}
        </span>
        <button className="btn btn--sm btn--primary"
                style={{ marginLeft: 'auto' }}
                onClick={(e) => { e.stopPropagation(); onApply() }}
                disabled={!dirty}
                title="절체 조건 변경을 backend 에 적용 (즉시 keepalived 반영)">
          ▶ 적용
        </button>
      </div>
      {open && (
        <div style={{ padding: 12, fontSize: 12, display: 'flex', flexDirection: 'column', gap: 12 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
            <label style={{ width: 150, color: 'var(--text-muted)' }}
                   title="VRRP 광고 주기 (초). Master 가 Backup 에게 살아있음을 알리는 주기. 짧을수록 절체가 빨라지지만 네트워크 트래픽 증가.">
              감시 주기 (초)
            </label>
            <input type="number" min={0.5} max={5} step={0.5}
                   value={value.advert_int}
                   onChange={e => set('advert_int', Number(e.target.value) || 1)}
                   className="form-input" style={{ width: 80 }} />
            <span style={{ color: 'var(--text-muted)' }}>기본 1초 · 범위 0.5~5초</span>
          </div>

          <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
            <label style={{ width: 150, color: 'var(--text-muted)' }}
                   title="cims-health 가 모듈 상태(포트 listen + 선택적 프로세스)를 점검하는 주기">
              점검 주기 (초)
            </label>
            <input type="number" min={1} max={60}
                   value={value.health.interval}
                   onChange={e => setHealth('interval', Number(e.target.value) || 2)}
                   className="form-input" style={{ width: 70 }} />
            <label style={{ color: 'var(--text-muted)', marginLeft: 12 }}
                   title="연속 실패 N회 → 장애로 판정. 절체까지의 시간 = 점검주기 × 장애판정.">장애 판정 (회)</label>
            <input type="number" min={1} max={60}
                   value={value.health.fall}
                   onChange={e => setHealth('fall', Number(e.target.value) || 2)}
                   className="form-input" style={{ width: 60 }} />
            <label style={{ color: 'var(--text-muted)', marginLeft: 12 }}
                   title="연속 성공 N회 → 정상 복귀로 판정">복귀 판정 (회)</label>
            <input type="number" min={1} max={60}
                   value={value.health.rise}
                   onChange={e => setHealth('rise', Number(e.target.value) || 2)}
                   className="form-input" style={{ width: 60 }} />
            <label style={{ color: 'var(--text-muted)', marginLeft: 12 }}
                   title="단일 점검 명령의 최대 실행 시간 (초과 시 실패)">제한 시간 (초)</label>
            <input type="number" min={1} max={60}
                   value={value.health.timeout}
                   onChange={e => setHealth('timeout', Number(e.target.value) || 3)}
                   className="form-input" style={{ width: 60 }} />
          </div>

          <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
            <label style={{ width: 150, color: 'var(--text-muted)' }}>NIC 링크 감시</label>
            <input type="checkbox" checked={value.track_interface}
                   onChange={e => set('track_interface', e.target.checked)} />
            <span style={{ color: 'var(--text-muted)' }}>
              서비스 NIC 의 링크 다운을 즉시 감지 (점검 주기 기다리지 않고 바로 절체)
            </span>
          </div>

          <div>
            <label style={{ width: 150, color: 'var(--text-muted)', display: 'inline-block' }}>
              프로세스 감시
            </label>
            <span style={{ color: 'var(--text-muted)' }}>
              포트 listen 외에 해당 프로세스 실행 여부도 점검 (pgrep -x). 비워두면 포트만 점검.
            </span>
            <div style={{ marginTop: 6, display: 'flex', flexWrap: 'wrap', gap: 8, paddingLeft: 150 }}>
              {TRACKABLE_MODULE_CANDIDATES.map(mod => (
                <label key={mod} style={{ display: 'inline-flex', alignItems: 'center', gap: 4,
                                          padding: '2px 8px', border: '1px solid var(--border)', borderRadius: 3,
                                          background: (value.tracked_modules || []).includes(mod) ? '#e3f2fd' : '#fff',
                                          cursor: 'pointer' }}>
                  <input type="checkbox"
                         checked={(value.tracked_modules || []).includes(mod)}
                         onChange={() => toggleMod(mod)} />
                  {mod}
                </label>
              ))}
            </div>
          </div>

          <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
            <label style={{ width: 150, color: 'var(--text-muted)' }}
                   title={
                     '복귀 없음: 절체 후 옛 Master 가 살아 돌아와도 Backup 으로 머무름 — 추가 절체 없음(운영 안정).\n' +
                     '자동 복귀: 옛 Master 의 priority 가 더 높으면 자동으로 Master 권한을 되찾음 — priority 의도 유지되나 복구 시점에 한 번 더 절체 발생.'
                   }>권한 복귀 정책</label>
            <select value={value.preempt}
                    onChange={e => set('preempt', e.target.value as 'preempt' | 'nopreempt')}
                    className="form-input" style={{ width: 200 }}>
              <option value="nopreempt">복귀 없음 (운영 안정)</option>
              <option value="preempt">자동 복귀 (priority 우선)</option>
            </select>
            {value.preempt === 'preempt' && (
              <>
                <label style={{ color: 'var(--text-muted)', marginLeft: 8 }}
                       title="옛 Master 가 살아 돌아온 뒤, 권한을 되찾기 전에 N초간 안정화 대기">복귀 지연 (초)</label>
                <input type="number" min={0} max={300}
                       value={value.preempt_delay}
                       onChange={e => set('preempt_delay', Number(e.target.value) || 0)}
                       className="form-input" style={{ width: 70 }} />
              </>
            )}
          </div>
          {value.preempt === 'preempt' && (
            <div style={{ paddingLeft: 150, fontSize: 11, color: '#e67e22' }}>
              ⚠ 자동 복귀 모드는 옛 Master 가 살아 돌아올 때 한 번 더 절체가 발생합니다 (서비스 추가 단절).
              priority 가 의미 있는 비대칭 환경 (사양 차이, 주/부 사이트) 에서만 권장.
            </div>
          )}

          <div style={{ fontSize: 11, color: 'var(--text-muted)' }}>
            오른쪽 위 [▶ 적용] 을 누르면 멤버 서버의 keepalived 설정이 재생성되어 즉시 반영됩니다.
          </div>
        </div>
      )}
    </div>
  )
}

function StatChip({ label, value, sub, color }:
  { label: string; value: string | number; sub?: string; color?: string }) {
  return (
    <div style={{
      background: 'var(--surface)', border: '1px solid var(--border)', padding: '8px 14px',
      borderRadius: 6, minWidth: 110, display: 'flex', flexDirection: 'column', gap: 2,
    }}>
      <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>{label}</span>
      <span style={{ fontWeight: 600, fontSize: 18 }}>
        <span style={{ color: color || '#333' }}>{value}</span>
        {sub && <span style={{ fontSize: 11, color: 'var(--text-muted)', marginLeft: 6 }}>{sub}</span>}
      </span>
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  Inspector (선택된 서버 상세)
// ──────────────────────────────────────────────────────────────

type InspectorTab = 'install' | 'info' | 'network' | 'modules'

function ServerInspector({ agent: a, mode, deployments, packages, vipIps,
                          onApprove, onRevoke, onRemove, onUpgrade, onRestart, onMetrics, onHealthCheck,
                          onAddDeploy, onConfigure, onJob, onRollback, onRemoveDep }: {
  agent: Agent
  // infra=시스템/서버 구성 (설치안내/정보/네트워크), install=패키지 설치 (모듈)
  mode: 'infra' | 'install'
  deployments: Deployment[]
  packages: SipPackage[]
  vipIps?: Set<string>
  onApprove: (a: Agent) => void
  onRevoke: (a: Agent) => void
  onRemove: (a: Agent) => void
  onUpgrade: (a: Agent) => void
  onRestart: (a: Agent) => void
  onMetrics: (a: Agent) => void
  onHealthCheck: (a: Agent) => void
  onAddDeploy: () => void
  onConfigure: (d: Deployment) => void
  onJob: (d: Deployment, jt: JobType) => void
  onRollback: (d: Deployment) => void
  onRemoveDep: (d: Deployment) => void
}) {
  // online 은 이미 enroll 완료 — token 재발급 의미 없음. InstallSection 자체 hidden.
  // 재설치 원하면 [폐기] 또는 [삭제] 후 offline / pending 전이로 진입.
  const showInstall = a.status !== 'online'
  // pending 또는 enrollment_token 발급된 상태 — default 펼침. 그 외 (offline) default 접힘.
  const hasPendingInstall = a.status === 'pending' || a.has_pending_enrollment
  const [openSections, setOpenSections] = useState<Set<InspectorTab>>(() => {
    const init = new Set<InspectorTab>(['info', 'network', 'modules'])
    if (hasPendingInstall) init.add('install')
    return init
  })
  // 헤더 [🔄 재설치] 클릭 시 InstallSection 자동 펼침 + 즉시 token 재발급.
  const [autoRegenSignal, setAutoRegenSignal] = useState(0)
  function onClickReinstall() {
    setOpenSections(prev => new Set(prev).add('install'))
    setAutoRegenSignal(s => s + 1)
  }
  const toggleSection = (s: InspectorTab) => {
    setOpenSections(prev => {
      const next = new Set(prev)
      if (next.has(s)) next.delete(s); else next.add(s)
      return next
    })
  }
  const sc = agentStatusColor(a.status)

  return (
    <>
      {/* 헤더 */}
      <div style={{ padding: '12px 16px', borderBottom: '1px solid #eee' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
          <span style={{
            width: 10, height: 10, borderRadius: '50%', background: sc.bar, display: 'inline-block',
          }} />
          <b style={{ fontSize: 16 }}>{agentDisplayName(a.name)}</b>
          {agentDisplayName(a.name) !== a.name && (
            <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>{a.name}</span>
          )}
          <span className="tag" style={{
            background: sc.bar, color: '#fff', fontSize: 10, padding: '1px 6px', borderRadius: 3,
          }}>{a.status}</span>
          <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>#{a.id}</span>
          {a.agent_version && <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>· v{a.agent_version}</span>}

          {/* 서버 전용 액션 */}
          <div style={{ marginLeft: 'auto', display: 'flex', gap: 6, flexWrap: 'wrap' }}>
            {a.status === 'pending' && (
              <button className="btn btn--sm btn--primary" onClick={() => onApprove(a)}>승인</button>
            )}
            {(a.status === 'online' || a.status === 'offline') && (
              <>
                <button className="btn btn--sm" onClick={() => onMetrics(a)}>메트릭</button>
                <button className="btn btn--sm" onClick={() => onHealthCheck(a)}
                  disabled={a.status !== 'online'} title="keepalived + 모듈 + VIP 실시간 점검 (sync REST)">
                  🩺 점검
                </button>
                <button className="btn btn--sm" onClick={() => onRestart(a)}
                  disabled={a.status !== 'online'} title="agent 프로세스 self-restart (execv)">
                  ↻ 재시작
                </button>
                <button className="btn btn--sm" onClick={() => onUpgrade(a)}
                  disabled={a.status !== 'online'} title="agent 바이너리를 최신 버전으로 교체">
                  ↑ 업그레이드
                </button>
                {a.status !== 'online' && (
                  <button className="btn btn--sm" onClick={onClickReinstall}
                    title="물리 서버 교체 / 신규 install — 새 enrollment_token 발급 + InstallSection 펼침">
                    🔄 재설치
                  </button>
                )}
                <button className="btn btn--sm btn--outline" onClick={() => onRevoke(a)}>폐기</button>
              </>
            )}
            <button className="btn btn--sm btn--danger" onClick={() => onRemove(a)}
                    disabled={a.ha_group?.mode === 'active_standby'}
                    title={a.ha_group?.mode === 'active_standby'
                      ? 'AS 그룹의 멤버는 단독 삭제 불가 — 그룹 삭제 또는 [폐기]+[🔄 재설치] 사용'
                      : '서버 삭제 (관련 deployment 도 같이 제거)'}>
              삭제
            </button>
          </div>
        </div>
      </div>

      {/* 섹션 stack — 페이지 탭에 따라: infra=구성(설치안내/정보/네트워크), install=모듈 */}
      <div style={{ flex: 1, overflow: 'auto' }}>
        {mode === 'infra' && (
          <>
            {showInstall && (
              <InspectorSection title={hasPendingInstall
                                        ? `설치 안내 — ${a.name}`
                                        : `재설치 / 토큰 재발급 — ${a.name}`}
                                expanded={openSections.has('install')}
                                onToggle={() => toggleSection('install')}>
                <InstallSection agent={a} autoRegenSignal={autoRegenSignal} />
              </InspectorSection>
            )}
            <InspectorSection title="정보" expanded={openSections.has('info')}
                              onToggle={() => toggleSection('info')}>
              <InfoTab agent={a} />
            </InspectorSection>
            <InspectorSection title="네트워크" expanded={openSections.has('network')}
                              onToggle={() => toggleSection('network')}>
              <NetworkTab agent={a} vipIps={vipIps} />
            </InspectorSection>
          </>
        )}
        {mode === 'install' && (
          <InspectorSection title={`모듈 (${deployments.length})`}
                            expanded={openSections.has('modules')}
                            onToggle={() => toggleSection('modules')}>
            <ModulesTab agent={a} deployments={deployments} packagesAvailable={packages.length > 0}
              onAddDeploy={onAddDeploy} onConfigure={onConfigure}
              onJob={onJob} onRollback={onRollback} onRemoveDep={onRemoveDep} />
          </InspectorSection>
        )}
      </div>
    </>
  )
}

// ── [패키지 설정] 탭 — 서버 선택: 모듈별 탭 + 설정 패널 (다이얼로그의 페이지화) ──
function AgentConfigTab({ agent, deployments, onDone }: {
  agent: Agent
  deployments: Deployment[]
  onDone: () => Promise<void> | void
}) {
  // 폴링 identity churn 차단 — mount 시 스냅샷 (모듈 전환은 key 리마운트)
  const [deps] = useState(() => deployments.filter(d => d.status !== 'removed' && d.status !== 'pending'))
  const [selDep, setSelDep] = useState<number>(deps[0]?.id ?? 0)
  const dep = deps.find(d => d.id === selDep)
  const source = useMemo(
    () => dep ? ({ type: 'deployment' as const, deployment: dep }) : null,
    [dep])
  if (deps.length === 0) {
    return <div className="empty" style={{ padding: 40 }}>
      {agent.name} 에 설치된 모듈 없음 — [패키지 설치] 탭에서 모듈을 먼저 배포하세요
    </div>
  }
  return (
    <div style={{ height: '100%', display: 'flex', flexDirection: 'column' }}>
      <div style={{ display: 'flex', gap: 2, padding: '10px 16px 0', borderBottom: '1px solid var(--border)',
                    background: 'var(--bg-soft)' }}>
        {deps.map(d => {
          const active = d.id === selDep
          return (
            <button key={d.id} onClick={() => setSelDep(d.id)}
                    style={{
                      padding: '8px 18px', fontSize: 13, fontWeight: active ? 700 : 400,
                      background: active ? 'var(--surface)' : 'transparent',
                      color: active ? '#1976d2' : 'var(--text-muted)',
                      border: '1px solid var(--border)', borderBottom: 'none',
                      borderRadius: '6px 6px 0 0', cursor: 'pointer',
                    }}>
              {d.package_name} <span style={{ fontSize: 10 }}>v{d.package_version}</span>
            </button>
          )
        })}
      </div>
      <div style={{ flex: 1, overflow: 'hidden' }}>
        {source && (
          <ModuleConfigModal key={selDep} inline source={source}
            onClose={() => { /* inline */ }} onDone={onDone} />
        )}
      </div>
    </div>
  )
}

// ── [패키지 설치] 탭 — 그룹 선택: 멤버별 배포 현황 요약 (작업은 멤버 서버에서) ──
function GroupInstallOverview({ group, agents, depsByAgent, onSelectMember }: {
  group: HaGroup
  agents: Agent[]
  depsByAgent: Map<number, Deployment[]>
  onSelectMember: (aid: number) => void
}) {
  return (
    <div style={{ padding: 20, overflow: 'auto' }}>
      <h4 style={{ marginTop: 0 }}>멤버별 패키지 배포 현황 — {group.name}</h4>
      <p style={{ fontSize: 12, color: 'var(--text-muted)' }}>
        설치/업그레이드/롤백 등 작업은 좌측 트리(또는 아래 멤버 클릭)에서 서버를 선택해 수행합니다.
      </p>
      <table className="data-table">
        <thead>
          <tr><th>서버</th><th>상태</th><th>배포 모듈</th></tr>
        </thead>
        <tbody>
          {group.members.map(m => {
            const ag = agents.find(a => a.id === m.agent_id)
            const deps = depsByAgent.get(m.agent_id) || []
            return (
              <tr key={m.agent_id} style={{ cursor: 'pointer' }}
                  onClick={() => onSelectMember(m.agent_id)}>
                <td><b>{agentDisplayName(ag?.name || `#${m.agent_id}`)}</b></td>
                <td>
                  <span style={{ color: agentStatusColor(ag?.status || 'offline'), fontSize: 12 }}>
                    ● {ag?.status || '—'}
                  </span>
                </td>
                <td>
                  {deps.length === 0 ? <span style={{ color: 'var(--text-muted)' }}>—</span> :
                    deps.map(d => (
                      <span key={d.id} className="tag" style={{
                        background: depStatusColor(d.status), color: '#fff',
                        fontSize: 11, padding: '2px 8px', borderRadius: 3, marginRight: 6,
                      }}>
                        {d.package_name} v{d.package_version} · {d.status}
                      </span>
                    ))}
                </td>
              </tr>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}

function InspectorSection({ title, expanded, onToggle, children }: {
  title: string
  expanded: boolean
  onToggle: () => void
  children: React.ReactNode
}) {
  return (
    <div style={{ borderBottom: '1px solid #eee' }}>
      <div onClick={onToggle}
           style={{
             display: 'flex', alignItems: 'center', gap: 8,
             padding: '10px 16px', cursor: 'pointer',
             background: 'var(--bg-soft)', userSelect: 'none',
             borderBottom: expanded ? '1px solid #eee' : 'none',
           }}>
        <span style={{ width: 14, color: 'var(--text-muted)', fontSize: 12 }}>{expanded ? '▼' : '▶'}</span>
        <span style={{ fontWeight: 600, fontSize: 14 }}>{title}</span>
      </div>
      {expanded && (
        <div style={{ padding: 16 }}>
          {children}
        </div>
      )}
    </div>
  )
}

function ModulesTab({ agent: a, deployments, packagesAvailable,
                     onAddDeploy, onConfigure, onJob, onRollback, onRemoveDep }: {
  agent: Agent
  deployments: Deployment[]
  packagesAvailable: boolean
  onAddDeploy: () => void
  onConfigure: (d: Deployment) => void
  onJob: (d: Deployment, jt: JobType) => void
  onRollback: (d: Deployment) => void
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
                onConfigure={onConfigure} onJob={onJob} onRollback={onRollback}
                onRemove={onRemoveDep} />
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

function DeploymentRow({ dep: d, agent, onConfigure, onJob, onRollback, onRemove }: {
  dep: Deployment; agent: Agent
  onConfigure: (d: Deployment) => void
  onJob: (d: Deployment, jt: JobType) => void
  onRollback: (d: Deployment) => void
  onRemove: (d: Deployment) => void
}) {
  const sc = depStatusColor(d.status)
  const online = agent.status === 'online'
  // pending = 생성만 됨 (파일 없음), stopped = 설치됐지만 실행 안됨
  const notInstalled = d.status === 'pending'
  const canStart = online && (d.status === 'stopped' || d.status === 'running')
  const canOps   = online && (d.status === 'running' || d.status === 'stopped')
  // 버전 단위 설치: 이전 버전 경로가 보존돼 있을 때만 롤백 가능
  const canRollback = canOps && !!d.prev_install_path
  const histTip = (d.install_history || [])
    .map(h => `v${h.version || '?'} ${h.at} ${h.install_path}`).join('\n')
  return (
    <tr>
      <td style={{ padding: 0 }}>
        <div style={{ width: 4, background: sc, height: 32 }} />
      </td>
      <td><b>{d.process_name || '—'}</b></td>
      <td style={{ fontSize: 12, color: 'var(--text-muted)' }}>
        {d.service_functions.length === 0 ? '—' : d.service_functions.join(', ')}
      </td>
      <td style={{ fontSize: 12 }}
          title={`설치 경로: ${d.install_path || '—'}${histTip ? `\n\n설치 이력:\n${histTip}` : ''}`}>
        {d.package_name} <span style={{ color: 'var(--text-muted)' }}>v{d.package_version}</span>
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
          <button className="btn btn--sm" disabled={!canRollback}
            title={canRollback
              ? `이전 버전으로 롤백 (v${d.prev_package_version || '?'} · ${d.prev_install_path})`
              : '롤백 대상 없음 (이전 버전 설치 이력 없음)'}
            onClick={() => onRollback(d)}>⤺ 롤백</button>
          <button className="btn btn--sm btn--danger" title="delete"
            onClick={() => onRemove(d)}>✕</button>
        </div>
      </td>
    </tr>
  )
}

function NetworkTab({ agent: a, vipIps }: { agent: Agent; vipIps?: Set<string> }) {
  const { show } = useToast()
  const [applying, setApplying] = useState(false)

  async function onApply(
    ops: {
      service_ip_rows?: Array<{ op: 'add'|'del'; iface: string; ip: string; mask: number; slot?: string }>
      routes?:          Array<{ op: 'add'|'del'; dst: string; via: string; dev: string }>
    },
    label: string,
  ) {
    setApplying(true)
    try {
      const r = await deploymentApi.applyIpConfig(a.id, ops)
      if (r.ok) show(`${label} — ${r.rows} IP / ${r.routes} route 적용`, 'ok')
      else      show(`${label} — rc=${r.rc} ${r.stderr || r.stdout}`, 'err')
    } catch (e) { show((e as Error).message, 'err') }
    finally { setApplying(false) }
  }

  async function onUpdateSlot(iface: string, ip: string, mask: number, slot: string) {
    // service_ip_rows file_store 갱신 (ip addr 변경 없음, slot 라벨만).
    const next = (a.service_ip_rows || []).filter(r => !(r.iface === iface && r.ip === ip))
    if (slot) next.push({ iface, ip, mask, slot })
    try {
      await deploymentApi.updateAgent(a.id, { service_ip_rows: next })
      show(`${iface}:${ip} → slot=${slot || '(none)'}`, 'ok')
    } catch (e) { show((e as Error).message, 'err') }
  }

  async function onApplyMounts(
    ops: Array<{ op: 'add'|'del'; fstype?: string; source?: string; target: string; options?: string }>,
    label: string,
  ) {
    setApplying(true)
    try {
      const r = await deploymentApi.applyMounts(a.id, ops)
      if (r.ok) show(`${label} — 적용 (fstab 영속)`, 'ok')
      else      show(`${label} — rc=${r.rc} ${r.stderr || r.stdout}`, 'err')
    } catch (e) { show((e as Error).message, 'err') }
    finally { setApplying(false) }
  }

  async function onApplyNetTuning(tuning: AgentNetTuning, label: string) {
    setApplying(true)
    try {
      const r = await deploymentApi.applyNetTuning(a.id, tuning)
      show(`${label} — job #${r.job_id} 큐잉 (sysctl ${r.sysctl}/rps ${r.rps}, agent 적용 대기)`, 'ok')
    } catch (e) { show((e as Error).message, 'err') }
    finally { setApplying(false) }
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
    <ServiceIpPanel
      title={`${a.name} — IP / Routing`}
      interfaces={a.interfaces || []}
      storedRows={(a.service_ip_rows || []).map(r => ({ ...r }))}
      storedRoutes={a.routes || []}
      slots={[]}
      applying={applying}
      onApply={onApply}
      onUpdateSlot={onUpdateSlot}
      vipIps={vipIps}
    />
    <MountPanel
      title={`${a.name} — 마운트`}
      mounts={a.mounts || []}
      applying={applying}
      onApply={onApplyMounts}
    />
    <NetTuningPanel
      title={`${a.name} — 네트워크 튜닝 (RPS / sysctl)`}
      agent={a}
      applying={applying}
      onApply={onApplyNetTuning}
    />
    </div>
  )
}

function InstallSection({ agent: a, autoRegenSignal }: {
  agent: Agent
  autoRegenSignal?: number  // 부모가 increment 시 재발급 자동 호출 (헤더 [🔄 재설치] 트리거)
}) {
  const { show } = useToast()
  const [data, setData] = useState<{ install_command: string; enrollment_token_expires_at?: string } | null>(null)
  const [err, setErr] = useState('')
  const [loading, setLoading] = useState(true)
  const [copied, setCopied] = useState(false)
  const [regenerating, setRegenerating] = useState(false)
  // 1분마다 re-render 강제 — 만료 카운트다운 갱신.
  const [, force] = useState(0)
  useEffect(() => {
    const iv = setInterval(() => force(x => x + 1), 60_000)
    return () => clearInterval(iv)
  }, [])

  const load = useCallback(async () => {
    setLoading(true); setErr('')
    try {
      const r = await deploymentApi.getInstallCommand(a.id)
      setData(r)
    } catch (e) { setErr((e as Error).message) }
    finally { setLoading(false) }
  }, [a.id])
  useEffect(() => { void load() }, [load])

  async function copy() {
    if (!data) return
    try {
      await navigator.clipboard.writeText(data.install_command)
      setCopied(true); setTimeout(() => setCopied(false), 1500)
    } catch (e) { show((e as Error).message, 'err') }
  }
  const regenerate = useCallback(async () => {
    setRegenerating(true)
    try {
      const r = await deploymentApi.regenerateToken(a.id)
      setData({
        install_command: r.install_command,
        enrollment_token_expires_at: r.enrollment_token_expires_at,
      })
      show('token 재발급됨', 'ok')
    } catch (e) { show((e as Error).message, 'err') }
    finally { setRegenerating(false) }
  }, [a.id, show])
  // 부모 (헤더 [🔄 재설치]) 가 autoRegenSignal 증가시키면 자동 재발급.
  useEffect(() => {
    if (autoRegenSignal && autoRegenSignal > 0) void regenerate()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [autoRegenSignal])

  // 만료 시간 카운트다운 — 분 단위. 음수면 expired.
  const expiresAt = data?.enrollment_token_expires_at
  const minsLeft = expiresAt
    ? Math.floor((new Date(expiresAt).getTime() - Date.now()) / 60_000)
    : null
  const expired = minsLeft !== null && minsLeft < 0

  return (
    <div>
      <div style={{ fontSize: 13, color: 'var(--text-muted)', marginBottom: 8 }}>
        대상 서버에서 다음 명령 실행 (ssh 1회) — systemd --user + linger 자동 (die 시 자동 재기동).
      </div>
      {loading && <div className="empty" style={{ padding: 8 }}>불러오는 중...</div>}
      {err && <div style={{ color: '#e74c3c', marginBottom: 8 }}>※ {err}</div>}
      {data && (
        <>
          <div style={{ position: 'relative' }}>
            <pre style={{
              background: '#0d1117', color: '#c9d1d9', padding: 12, paddingRight: 88,
              borderRadius: 4, fontSize: 12, whiteSpace: 'pre-wrap', margin: 0,
              opacity: expired ? 0.5 : 1,
            }}>{data.install_command}</pre>
            <button className="btn btn--sm btn--outline"
              style={{ position: 'absolute', top: 8, right: 8 }}
              onClick={copy} disabled={expired}>{copied ? '✓' : '📋'} 복사</button>
          </div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginTop: 8 }}>
            <div style={{ fontSize: 12, color: expired ? '#e74c3c' : '#666' }}>
              {expiresAt
                ? expired
                  ? <>⚠ token 만료됨 ({expiresAt}) — 재발급 필요</>
                  : <>token 만료까지 약 <b>{minsLeft}분</b> (만료 시각: {expiresAt})</>
                : <>token 만료 시각 미상</>}
            </div>
            <button className="btn btn--sm" onClick={regenerate} disabled={regenerating}
                    style={{ marginLeft: 'auto' }}>
              {regenerating ? '재발급 중...' : '↻ 재발급'}
            </button>
          </div>
          <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 6 }}>
            실행 후 <code>./init.sh</code> 로 sudoers + enrollment + systemd unit 일괄 설정 (sudo 비번 1회).
          </div>
        </>
      )}
    </div>
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
      <span style={{ color: 'var(--text-muted)' }}>{label}</span>
      <span>{value}</span>
    </>
  )
}

// ──────────────────────────────────────────────────────────────
//  Modals
// ──────────────────────────────────────────────────────────────

function PendingMemberModal({ info, onClose }: {
  info: { groupName: string; serverName: string; enrollment_token: string; install_command: string }
  onClose: () => void
}) {
  const { show } = useToast()
  const [copied, setCopied] = useState(false)
  async function copy() {
    try {
      await navigator.clipboard.writeText(info.install_command)
      setCopied(true); setTimeout(() => setCopied(false), 1500)
    } catch (e) { show((e as Error).message, 'err') }
  }
  return (
    <Modal title={`${info.groupName} — 새 멤버 추가됨`} onClose={onClose} width={640}>
      <div style={{ color: '#2ecc71', marginBottom: 10 }}>
        ✓ <b>{info.serverName}</b> 그룹 멤버로 등록됨. 다음 명령을 대상 서버에서 실행:
      </div>
      <div style={{ position: 'relative' }}>
        <pre style={{
          background: '#0d1117', color: '#c9d1d9', padding: 12, paddingRight: 88,
          borderRadius: 4, fontSize: 12, whiteSpace: 'pre-wrap', margin: 0,
        }}>{info.install_command}</pre>
        <button className="btn btn--sm btn--outline"
          style={{ position: 'absolute', top: 8, right: 8 }}
          onClick={copy}>{copied ? '✓' : '📋'} 복사</button>
      </div>
      <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 6 }}>
        token: <code>{info.enrollment_token}</code>
      </div>
      <div className="modal-footer" style={{ marginTop: 16 }}>
        <button className="btn btn--primary" onClick={onClose}>닫기</button>
      </div>
    </Modal>
  )
}

type SystemMode = 'active_standby' | 'all_active' | 'standalone'

function SystemCreateModal({ onClose, onDone, onCreated }: {
  onClose: () => void
  onDone: () => Promise<void> | void
  onCreated: (firstAgentId: number | null) => void
}) {
  const { show } = useToast()
  const [name, setName] = useState('')
  const [mode, setMode] = useState<SystemMode>('active_standby')
  const [authPass, setAuthPass] = useState('00000000')  // active_standby 만 사용 (VRRP)
  const [creating, setCreating] = useState(false)
  // 생성 결과 — Standalone 1건, AS 2건, AA 0건 (이후 그룹에서 추가)
  const [results, setResults] = useState<Array<{ name: string; enrollment_token: string; install_command: string }> | null>(null)
  const [copiedIdx, setCopiedIdx] = useState<number | null>(null)

  async function create() {
    const base = name.trim()
    if (!base) { show('이름 필수', 'err'); return }
    setCreating(true)
    try {
      let firstAgentId: number | null = null
      if (mode === 'standalone') {
        const r = await deploymentApi.createAgent(base, '')
        await deploymentApi.approveAgent(r.id)
        firstAgentId = r.id
        setResults([{ name: base, enrollment_token: r.enrollment_token, install_command: r.install_command }])
      } else {
        const memberCount = mode === 'active_standby' ? 2 : 0
        const memberAgents: Array<{ id: number; name: string; enrollment_token: string; install_command: string }> = []
        for (let i = 1; i <= memberCount; i++) {
          const nm = `${base}-${String(i).padStart(2, '0')}`
          const r = await deploymentApi.createAgent(nm, '')
          await deploymentApi.approveAgent(r.id)
          memberAgents.push({ id: r.id, name: nm, enrollment_token: r.enrollment_token, install_command: r.install_command })
        }
        if (memberAgents.length > 0) firstAgentId = memberAgents[0].id
        await haGroupsApi.create({
          name: base,
          mode,
          vip: '',
          vip_mask: 24,
          // auth_pass — active_standby 만 의미 (VRRP 인증). all_active 는 keepalived 미사용이라 빈값.
          auth_pass: mode === 'active_standby' ? authPass : '',
          members: memberAgents.map((m, i) => ({
            agent_id: m.id,
            role: (i === 0 && mode === 'active_standby' ? 'master' : 'backup'),
            priority: i === 0 ? 100 : 90,
          })),
        })
        setResults(memberAgents.map(m => ({
          name: m.name, enrollment_token: m.enrollment_token, install_command: m.install_command,
        })))
      }
      show(`시스템 "${base}" 추가 (${mode === 'active_standby' ? 'AS' : mode === 'all_active' ? 'AA' : 'Standalone'})`, 'ok')
      await onDone()
      // 첫 멤버 (있으면) 자동 선택 — 사용자가 modal 닫은 후 ServerInspector 의 InstallSection 으로 진입.
      onCreated(firstAgentId)
    } catch (e) { show((e as Error).message, 'err') }
    finally { setCreating(false) }
  }

  async function copyCmd(idx: number) {
    if (!results) return
    try {
      await navigator.clipboard.writeText(results[idx].install_command)
      setCopiedIdx(idx); setTimeout(() => setCopiedIdx(null), 1500)
    } catch (e) { show((e as Error).message, 'err') }
  }

  const modeLabel = mode === 'active_standby' ? 'AS (서버 2 자동)'
                  : mode === 'all_active'     ? 'AA (서버 0 — 이후 추가)'
                  :                             'Standalone (서버 1)'

  return (
    <Modal title="시스템 추가" onClose={onClose} width={640}>
      {!results ? (
        <div className="form-grid">
          <label>이름 *</label>
          <input className="form-input" value={name} placeholder="예: Control-Server"
            onChange={e => setName(e.target.value)} disabled={creating} />
          <label>유형 *</label>
          <select className="form-input" value={mode} onChange={e => setMode(e.target.value as SystemMode)}
                  disabled={creating}>
            <option value="active_standby">AS — Active/Standby (master + backup 2서버 자동)</option>
            <option value="all_active">AA — All Active (다중화, 그룹만 생성 + 이후 멤버 추가)</option>
            <option value="standalone">Standalone — 단일 서버 (HA 그룹 없음)</option>
          </select>
          {mode === 'active_standby' && (
            <>
              <label>auth_pass *</label>
              <input className="form-input" value={authPass} type="password"
                onChange={e => setAuthPass(e.target.value)} disabled={creating}
                placeholder="최대 8글자 — VRRP 인증" maxLength={8} />
            </>
          )}
          <label style={{ gridColumn: '1 / -1', fontSize: 12, color: 'var(--text-muted)', marginTop: 4 }}>
            선택: <b>{modeLabel}</b>
            {mode === 'active_standby' && <> · 멤버 이름: <code>{name || '<이름>'}-01</code> (master), <code>{name || '<이름>'}-02</code> (backup)</>}
          </label>
        </div>
      ) : results.length === 0 ? (
        <div style={{ color: '#2ecc71' }}>
          ✓ AA 그룹 생성됨. 좌측 트리에서 그룹 선택 후 [+ 멤버 추가] 로 서버를 추가하세요.
        </div>
      ) : (
        <div>
          <div style={{ color: '#2ecc71', marginBottom: 10 }}>
            ✓ {results.length} 서버 등록됨. 각 서버에서 다음 명령 실행:
          </div>
          {results.map((r, i) => (
            <div key={i} style={{ marginBottom: 12 }}>
              <div style={{ fontWeight: 600, fontSize: 13, marginBottom: 4 }}>
                ⓘ {r.name}
              </div>
              <div style={{ position: 'relative' }}>
                <pre style={{
                  background: '#0d1117', color: '#c9d1d9', padding: 12, paddingRight: 88,
                  borderRadius: 4, fontSize: 12, whiteSpace: 'pre-wrap', margin: 0,
                }}>{r.install_command}</pre>
                <button className="btn btn--sm btn--outline"
                  style={{ position: 'absolute', top: 8, right: 8 }}
                  onClick={() => copyCmd(i)}>{copiedIdx === i ? '✓' : '📋'} 복사</button>
              </div>
              <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 4 }}>
                token: <code>{r.enrollment_token}</code>
              </div>
            </div>
          ))}
        </div>
      )}
      <div className="modal-footer" style={{ marginTop: 16 }}>
        {!results ? (
          <>
            <button className="btn btn--outline" onClick={onClose} disabled={creating}>취소</button>
            <button className="btn btn--primary" onClick={create} disabled={creating || !name.trim()}>
              {creating ? '생성 중...' : '생성'}
            </button>
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

  // HA capability 검증 — backend (csc/handlers/agents.py:_create_deployment) 와
  // 동일 정책: ha_group 정의 시 strict, 미정의 시 모두 허용.
  // moduleNames 중 어느 모듈이 mismatch 인지 사전 표시.
  function moduleMismatch(modName: string): string | null {
    const grp = agent.ha_group
    if (!grp) return null  // ha_group 미정의 → 모두 허용
    const list = pkgsByModule.get(modName) || []
    const cap = (list[0]?.meta?.ha_capability) || 'standalone'
    if (cap === 'standalone') return null
    if (cap !== grp.mode) {
      return `이 agent 는 HA 그룹 "${grp.name}" (mode=${grp.mode}) — 이 모듈은 ${cap} 만 가능`
    }
    return null
  }
  const selectedMismatch = selectedPkg ? moduleMismatch(selectedPkg.name) : null

  // 모듈 바뀌면 버전/process/functions 리셋
  /* eslint-disable react-hooks/set-state-in-effect */
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
  /* eslint-enable react-hooks/set-state-in-effect */

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
      {agent.ha_group && (
        <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 8,
                      padding: '6px 10px', background: 'var(--primary-soft)', border: '1px solid #d0e3ff',
                      borderRadius: 4 }}>
          이 agent 는 HA 그룹 <b>{agent.ha_group.name}</b> (mode={agent.ha_group.mode}, role={agent.ha_group.role}) 소속 —
          {' '}<b>{agent.ha_group.mode}</b> 가능 모듈 + standalone 모듈만 install 가능
        </div>
      )}
      <div className="form-grid">
        <label>1. 모듈 *</label>
        <select className="form-input" value={moduleName}
          onChange={e => setModuleName(e.target.value)}>
          <option value="">(선택)</option>
          {moduleNames.map(m => {
            const mm = moduleMismatch(m)
            return (
              <option key={m} value={m} disabled={!!mm}>
                {m} ({pkgsByModule.get(m)!.length}개 버전){mm ? ` — ${mm}` : ''}
              </option>
            )
          })}
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
              border: '1px solid var(--border)', borderRadius: 4, padding: 8,
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
      <div style={{ marginTop: 12, fontSize: 12, color: 'var(--text-muted)' }}>
        ℹ 추가 후 <b>pending</b> 상태로 생성됩니다. 설정을 확인한 뒤
        <b>설치</b> → <b>Start</b> 순으로 진행하세요.
      </div>
      {selectedMismatch && (
        <div style={{ marginTop: 8, fontSize: 12, color: '#c00',
                      padding: '6px 10px', background: 'var(--danger-soft)', border: '1px solid #ffcaca',
                      borderRadius: 4 }}>
          ⚠ {selectedMismatch} — install 시 backend 400 reject
        </div>
      )}
      <div className="modal-footer" style={{ marginTop: 16 }}>
        <button className="btn btn--outline" onClick={onClose}>취소</button>
        <button className="btn btn--primary" onClick={create}
                disabled={!!selectedMismatch}>추가</button>
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
  // sparkline 은 시간순(오래된→최신). API 정렬에 의존하지 않도록 ts 로 재정렬.
  const chrono = [...metrics].sort((a, b) => (a.ts || '').localeCompare(b.ts || ''))
  return (
    <Modal title={`${agent.name} — 메트릭 (최근 ${metrics.length}건)`}
           onClose={onClose} width={760}>
      {chrono.length >= 2 && (
        <div style={{ display: 'flex', gap: 10, marginBottom: 16 }}>
          <MetricTrend label="CPU" values={chrono.map(m => m.cpu_pct)} color="#3498db" warn={85} />
          <MetricTrend label="MEM" values={chrono.map(m => m.mem_pct)} color="#27ae60" warn={90} />
          <MetricTrend label="Disk" values={chrono.map(m => m.disk_pct)} color="#9b59b6" warn={90} />
        </div>
      )}
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

