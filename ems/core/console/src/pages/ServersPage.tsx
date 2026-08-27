import { Fragment, useCallback, useEffect, useMemo, useState } from 'react'
import { useSearchParams } from 'react-router-dom'
import {
  deploymentApi,
  type Agent, type SipPackage, type Deployment, type JobType, type AgentMetric, type AgentNetTuning,
} from '../api/deployment'
import { haGroupsApi, type HaGroup, type VipBinding, type MountOp,
         type FailoverOptions, FAILOVER_DEFAULTS,
         type ModuleSpec, MODULE_SPEC_DEFAULT, type SafetyClass } from '../api/ha_groups'
import { ServiceIpPanel } from './ha/ServiceIpPanel'
import { MountPanel } from './ha/MountPanel'
import { GroupMountPanel } from './ha/GroupMountPanel'
import { NetTuningPanel } from './ha/NetTuningPanel'
import { OamUrlPanel } from './ha/OamUrlPanel'
import type { PendingMount } from '../api/deployment'
import type { GroupMount } from '../api/ha_groups'
import { splitPrefixHost } from './ha/helpers'
import { ApiError } from '../api/client'
import { useToast } from '../components/Toast'
import Modal from '../components/Modal'
import { agentStatusColor, depStatusColor, depEffectiveStatus, fmtRelTime } from './deploy/deployHelpers'
import ModuleConfigModal from '../components/module/ModuleConfigModal'
import { GroupConfigCompareView } from '../components/group/GroupConfigCompareView'
import HealthCheckModal from '../components/HealthCheckModal'
import MetricTrend from '../components/MetricTrend'
import { agentDisplayName } from '../components/agentDisplay'
import { useAdminCapable } from '../hooks/useAdminCapable'
import { hasRole } from '../utils/permissions'
import AdminElevateDialog from '../components/AdminElevateDialog'
import { clearElevatedToken, elevationActive } from '../api/client'
import { useAuth } from '../contexts/AuthContext'

type Selection =
  | { kind: 'agent'; id: number }
  | { kind: 'group'; id: number }
  | null

// 상단 페이지 탭 — UX 개편 (2026-06-10): 좌측 선택(서버/그룹) 공유 + 우측 내용 분리.
//  infra(시스템/서버 구성)·install(패키지 설치)·control(패키지 제어) = 조회 operator+, 변이 admin/승격.
//  config(패키지 설정) = operator+ 편집 가능 (동적 반영 설정).
//  역할 분리: install=파일 배치(설치/재설치/롤백/삭제), control=프로세스(start/stop/restart),
//  config=설정 — 한 탭에 섞여 있던 작업을 라이프사이클 단계별로 분리.
type PageTab = 'infra' | 'install' | 'config' | 'control'
const PAGE_TABS: Array<{ key: PageTab; label: string; adminGated: boolean }> = [
  { key: 'infra',   label: '시스템/서버 구성', adminGated: true },
  { key: 'install', label: '패키지 설치',      adminGated: true },
  { key: 'config',  label: '패키지 설정',      adminGated: false },
  { key: 'control', label: '패키지 제어',      adminGated: true },
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
  // 패키지 목록 "도착함" 래치 — 설정 탭 key 의 remount 트리거용. 최초 로드 시
  // 패키지가 도착하면 1회 remount(스냅샷 재캡처)가 의도인데, 원시 packages.length>0
  // 를 key 에 그대로 쓰면 폴링 응답이 일시적으로 비는 순간 true→false→true 로
  // 뒤집혀 편집 중인 설정 화면 전체가 주기적으로 remount 된다 (스크롤 리셋 +
  // 컬렉션 추가행 닫힘). 한 번 true 가 되면 되돌리지 않는다.
  const [pkgsReady, setPkgsReady]     = useState(false)
  useEffect(() => {
    if (packages.length > 0) setPkgsReady(true)
  }, [packages])
  const [selection, setSelection]     = useState<Selection>(initialSelection)
  const [expandedGroups, setExpandedGroups] = useState<Set<number>>(new Set())  // -1 = standalone

  const [systemModalOpen, setSystemModalOpen] = useState(false)
  // [+ 멤버 추가] 선택 단계 (마운트 여부·위치) — 확정 시 createGroupMember 가 만든다.
  const [memberDraft, setMemberDraft] = useState<{ group: HaGroup; serverName: string } | null>(null)
  const [pendingMember, setPendingMember] = useState<{
    appliedMounts?: PendingMount[]
    groupName: string; serverName: string;
    enrollment_token: string; install_command: string;
  } | null>(null)
  const [upgradeModal, setUpgradeModal] = useState<{ dep: Deployment } | null>(null)
  const [deployModal, setDeployModal]       = useState<{ agent: Agent } | null>(null)
  const [metricsFor, setMetricsFor]         = useState<Agent | null>(null)
  const [healthCheckFor, setHealthCheckFor] = useState<Agent | null>(null)
  const [pageTab, setPageTab] = useState<PageTab>(() => {
    const t = searchParams.get('t')
    return (t === 'install' || t === 'config' || t === 'control') ? t : 'infra'
  })
  const [elevateOpen, setElevateOpen] = useState(false)
  const { user } = useAuth()
  const canEdit = useAdminCapable()   // admin 세션 또는 admin 승격(sudo) 활성

  // 폴링을 **비용별로 분리**한다. store 가 공유 스토리지(NFS)로 옮겨간 뒤 파일 1건 읽기가
  // ~5ms 라, 모든 목록을 2초마다 다 긁으면 콘솔 조작(시스템 추가 등)이 체감상 느려진다.
  //   · agents/deployments = 실측 상태(heartbeat 반영) → 2초 유지
  //   · packages/ha-groups = 거의 안 바뀌고 응답이 무겁다 → 6초
  // 변이 직후에는 load(true) 로 전체를 즉시 갱신하므로 반영 지연이 체감되지 않는다.
  const load = useCallback(async (full = true) => {
    try {
      const [a, d] = await Promise.all([
        deploymentApi.listAgents(),
        deploymentApi.listDeployments(),
      ])
      setAgents(a); setDeployments(d)
      if (full) {
        const [p, g] = await Promise.all([
          deploymentApi.listPackages(),
          haGroupsApi.list(),
        ])
        setPackages(p); setHaGroups(g)
      }
    } catch (e) { show((e as Error).message, 'err') }
    finally { setLoading(false) }
  }, [show])

  useEffect(() => { void load(true) }, [load])
  useEffect(() => {
    let n = 0
    const iv = setInterval(() => { n += 1; void load(n % 3 === 0) }, 2_000)
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
  // 관리평면 VIP — agent 가 OAM 에 접속할 주소의 권장값. 판정 기준은 백엔드
  // `_agents_not_on_vip` 와 같다: **oam 을 호스팅하는 AS 그룹**만 본다 (Signaling 처럼
  // oam 이 없는 그룹의 VIP 와 비교하면 전원이 어긋남으로 잡힌다 — 실측).
  const mgmtVip = useMemo(() => {
    const oamAgentIds = new Set(deployments
      .filter(d => (d.process_name || '').toLowerCase() === 'oam' && d.status !== 'removed')
      .map(d => d.agent_id))
    for (const g of haGroups) {
      if (g.mode !== 'active_standby') continue
      if (!(g.members || []).some(m => oamAgentIds.has(m.agent_id))) continue
      const binds = g.vip_bindings || []
      const admin = binds.find(b => /admin|oam|mgmt/i.test(b.slot || ''))
      const ip = ((admin || binds[0])?.ip || g.vip || '').trim()
      if (ip) return ip
    }
    return null
  }, [haGroups, deployments])

  // 마운트 기본값 제안 — 이 설치가 **이미 쓰고 있는** cims-managed 마운트를 읽어 온다.
  // 새 저장소를 만들지 않는다(값의 정본은 각 노드 fstab 이고 agent 가 heartbeat 로 보고).
  // oam 을 호스팅하는 노드의 것을 우선 — 관리 store 가 놓인 마운트가 설치의 기준이다.
  const mountSuggestion = useMemo<PendingMount | null>(() => {
    const oamAgentIds = new Set(deployments
      .filter(d => (d.process_name || '').toLowerCase() === 'oam' && d.status !== 'removed')
      .map(d => d.agent_id))
    const pick = (list: Agent[]) => {
      for (const a of list) {
        for (const m of a.mounts || []) {
          if (m.target && m.source && m.fstype) {
            return { fstype: m.fstype, source: m.source, target: m.target,
                     options: m.options || 'defaults' }
          }
        }
      }
      return null
    }
    return pick(agents.filter(a => oamAgentIds.has(a.id))) ?? pick(agents)
  }, [agents, deployments])

  const vipIps = useMemo(
    () => new Set(haGroups.flatMap(g => (g.vip_bindings || []).map(b => b.ip)).filter(Boolean)),
    [haGroups]
  )

  // group 별 멤버 분류 + standalone
  const groupedAgents = useMemo(() => {
    const byGroup = new Map<number, Agent[]>()  // -1 = standalone
    for (const a of agents) {
      const gid = a.ha_group?.id ?? -1
      if (!byGroup.has(gid)) byGroup.set(gid, [])
      byGroup.get(gid)!.push(a)
    }
    return byGroup
  }, [agents])

  const toggleGroupExpand = (gid: number) => {
    setExpandedGroups(prev => {
      const next = new Set(prev)
      if (next.has(gid)) next.delete(gid); else next.add(gid)
      return next
    })
  }

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
  // 서버 이름 변경 — **라벨만 바꾼다.** 배포·job·메트릭·알람 식별은 전부 agent_id 라
  //   개명에 딸린 보상 동작이 없다 (identifier_model.md). 노드 로컬의 state.json·systemd
  //   `--name` 은 설치 시점 값이라 옛 이름으로 남지만 인증·보고는 토큰과 id 로 하므로 무해.
  async function renameAgent(a: Agent) {
    const next = prompt(`서버 이름을 입력하세요 (#${a.id})`, a.name || '')
    if (next === null) return
    const nm = next.trim()
    if (!nm || nm === a.name) return
    try {
      await deploymentApi.updateAgent(a.id, { name: nm })
      show(`이름 변경: ${a.name} → ${nm}`, 'ok')
      await load()
    } catch (e) { show((e as Error).message, 'err') }
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
  async function rollbackAgent(a: Agent) {
    // 롤백 대상 = current(현재 버전) 제외 설치된 버전 중 직전(mtime 최신). 여러 개면 선택.
    const others = (a.agent_versions || []).filter(v => v && v !== a.agent_version)
    if (others.length === 0) {
      show('롤백 가능한 직전 agent 버전이 없습니다 (단일 버전)', 'err'); return
    }
    let target = others[0]
    if (others.length > 1) {
      const pick = prompt(`롤백할 agent 버전을 입력하세요 (현재 v${a.agent_version}).\n설치됨: ${others.join(', ')}`, others[0])
      if (!pick) return
      target = pick.trim()
    } else if (!confirm(`${a.name} 의 agent 를 v${target} 로 롤백할까요? (현재 v${a.agent_version} — self-exec 재기동)`)) {
      return
    }
    try {
      const r = await deploymentApi.rollbackAgent(a.id, target)
      show(`롤백 job 큐잉 (#${r.job_id} → v${r.target_version || target})`, 'ok')
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
    } catch (e) {
      // 안전 가드(409)는 막다른 골목이 아니다 — 사유를 보여주고 강행 여부를 묻는다.
      const guard = e instanceof ApiError && e.status === 409 &&
        (e.data?.error === 'leader_lease_precondition' ||
         e.data?.error === 'upgrade_order_active_first')
      if (guard) {
        if (!confirm(`${(e as Error).message}\n\n그래도 강행할까요? (안전 가드 우회)`)) {
          show('취소됨 — 안전 가드 유지', 'err'); return
        }
        try {
          const r = await deploymentApi.queueJob(d.id, jt, undefined, true)
          show(`${jt} 큐 등록 (#${r.job_id}) — 가드 우회`, 'ok')
          await load()
        } catch (e2) { show((e2 as Error).message, 'err') }
        return
      }
      show((e as Error).message, 'err')
    }
  }
  // 모듈 업그레이드 — 버전 선택은 모달에서. 실행은 서버의 단일 액션
  //   (`POST /deployments/{id}/upgrade`)에 위임한다: 버전 전환과 job 큐잉을 콘솔이 두 번에
  //   나눠 하면 그 사이 실패했을 때 레코드만 새 버전을 가리키는 어긋남이 남고, 되돌리는
  //   것도 답이 아니다 — 전환이 컬렉션 SoT 를 대상 스키마로 정렬한 뒤라 역방향 이관으로
  //   복구되지 않는다(새 스키마가 없앤 필드는 사라진다).
  function upgradeDeployment(d: Deployment) {
    setUpgradeModal({ dep: d })
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
  // [+ 멤버 추가] — 바로 만들지 않고 **선택 단계**를 띄운다. 이 경로로 들어오는 서버는
  // (AA 는 이 경로가 유일하다) 며칠 뒤에 추가될 수도 있어, 그룹 선언을 조용히 상속하면
  // 운영자가 "이 서버는 마운트가 되는가" 를 알 방법이 없다. 그래서 그 자리에서 묻는다.
  function addMemberToGroup(g: HaGroup) {
    const existing = (g.members || []).length
    setMemberDraft({ group: g, serverName: `${g.name}-${String(existing + 1).padStart(2, '0')}` })
  }

  // 선택 완료 → agent 생성 + 승인 + 그룹 가입. mounts=null 이면 "마운트하지 않음"(명시).
  async function createGroupMember(g: HaGroup, nm: string, mounts: PendingMount[]) {
    try {
      const r = await deploymentApi.createAgent(nm, '', mounts)
      await deploymentApi.approveAgent(r.id)
      await haGroupsApi.addMember(g.id, { agent_id: r.id, role: 'backup', priority: 90 })
      setMemberDraft(null)
      setPendingMember({
        groupName: g.name, serverName: nm,
        enrollment_token: r.enrollment_token, install_command: r.install_command,
        appliedMounts: mounts,
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
          {!hasRole(user, 'admin') && (
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
          flex: '0 0 320px', overflow: 'hidden', display: 'flex', flexDirection: 'column',
          border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)',
        }}>
          <div style={{ flex: 1, overflow: 'auto' }}>
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
          {/* 시스템 추가 — 시스템 목록 바로 아래. 구성 작업이므로 [시스템/서버 구성] 탭에서만 노출 */}
          {pageTab === 'infra' && (
            <div style={{ flex: '0 0 auto', padding: 10, borderTop: '1px solid var(--border)' }}>
              <button className="btn btn--primary btn--sm" style={{ width: '100%' }}
                      onClick={() => setSystemModalOpen(true)}
                      disabled={!canEdit}
                      title={canEdit ? 'AS 이중화 (서버 2 자동) / AA 다중화 / SA 단일 서버' : 'admin 권한 필요 (관리자 인증)'}>
                ＋ 시스템 추가
              </button>
            </div>
          )}
        </div>
        {/* 우측 Inspector */}
        <div style={{
          flex: 1, overflow: 'hidden', display: 'flex', flexDirection: 'column',
          border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)',
        }}>
          {selectedAgent ? (
            pageTab === 'config' ? (
              <AgentConfigTab key={`${selectedAgent.id}:${pkgsReady}`}
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
                  mgmtVip={mgmtVip}
                  onApprove={approveAgent}
                  onRevoke={revokeAgent}
                  onRemove={removeAgent}
                  onRename={renameAgent}
                  onUpgrade={upgradeAgent}
                  onRestart={restartAgent}
                  onRollbackAgent={rollbackAgent}
                  onMetrics={setMetricsFor}
                  onHealthCheck={setHealthCheckFor}
                  onAddDeploy={() => setDeployModal({ agent: selectedAgent })}
                  onJob={queueJob}
                  onUpgradeDep={upgradeDeployment}
                  onRollback={rollbackDeployment}
                  onRemoveDep={removeDeployment} />
              </fieldset>
            )
          ) : selectedGroup ? (
            pageTab === 'config' ? (
              // 그룹 = 모듈 운영 명세(감시·절체 모드) + 멤버별 앱 설정 비교/동기화.
              // 앱 설정 편집은 멤버 서버 선택 → 패키지 설정 탭 (항상 그 서버에만 저장).
              <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
                <div style={{ padding: '12px 12px 0' }}>
                  <ModuleSpecSection group={selectedGroup} deployments={deployments} onReload={load} />
                </div>
                <div style={{ flex: 1, minHeight: 0 }}>
                  <GroupConfigCompareView key={`${selectedGroup.id}:${pkgsReady}`}
                    group={selectedGroup}
                    members={selectedGroup.members.map(m => ({
                      id: m.agent_id,
                      name: m.agent_name || agents.find(a => a.id === m.agent_id)?.name || `#${m.agent_id}`,
                    }))}
                    deployments={deployments}
                    packages={packages}
                    onSelectMember={(aid) => setSelection({ kind: 'agent', id: aid })} />
                </div>
              </div>
            ) : pageTab === 'install' ? (
              <GroupInstallOverview group={selectedGroup} agents={agents}
                depsByAgent={depsByAgent}
                onSelectMember={(aid) => setSelection({ kind: 'agent', id: aid })} />
            ) : pageTab === 'control' ? (
              <fieldset disabled={!canEdit} style={LOCK_FIELDSET_STYLE}>
                <GroupControlMatrix group={selectedGroup} agents={agents}
                  depsByAgent={depsByAgent}
                  onJob={queueJob}
                  onSelectMember={(aid) => setSelection({ kind: 'agent', id: aid })}
                  onReload={load} />
              </fieldset>
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
          saAgents={agents.filter(a => !a.ha_group && (a.status === 'online' || a.status === 'approved'))}
          mountSuggestion={mountSuggestion}
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
      {memberDraft && (
        <AddMemberModal
          group={memberDraft.group}
          serverName={memberDraft.serverName}
          mountSuggestion={memberDraft.group.mounts?.[0] ?? mountSuggestion}
          onClose={() => setMemberDraft(null)}
          onSubmit={(nm, mounts) => createGroupMember(memberDraft.group, nm, mounts)} />
      )}
      {pendingMember &&
        <PendingMemberModal info={pendingMember} onClose={() => setPendingMember(null)} />}
      {deployModal &&
        <DeploymentCreateModal agent={deployModal.agent} packages={packages}
          onClose={() => setDeployModal(null)} onDone={load} />}
      {upgradeModal &&
        <DeploymentUpgradeModal dep={upgradeModal.dep} packages={packages}
          onClose={() => setUpgradeModal(null)} onDone={load} />}
      {metricsFor &&
        <MetricsModal agent={metricsFor} onClose={() => setMetricsFor(null)} />}
      {healthCheckFor &&
        <HealthCheckModal agents={[healthCheckFor]} onClose={() => setHealthCheckFor(null)} />}
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
                   borderBottom: '1px solid var(--border)', cursor: 'pointer',
                   background: isSelected ? 'var(--primary-soft, #eef5ff)' : 'var(--bg-soft)',
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
                          border: '1px solid var(--border)', background: 'var(--surface)', color: '#3498db',
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
                 borderBottom: '1px solid var(--border)', cursor: 'pointer',
                 background: isSelected ? 'var(--primary-soft, #eef5ff)' : 'var(--bg-soft)',
               }}>
            <span style={{ width: 14 }} />  {/* expand 자리 비움 — group 정렬 맞춤 */}
            <span style={{
              background: '#6b7280', color: '#fff', fontSize: 10,
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
           borderBottom: '1px solid var(--border)', cursor: 'pointer',
           background: active ? 'var(--primary-soft, #eef5ff)' : undefined,
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
  // 그룹 공통 마운트 fan-out 진행 중 — 버튼 중복 클릭 차단.
  const [mountApplying, setMountApplying] = useState(false)
  // VIP 행 수동 입력 — 슬롯(용도) 자동 매핑으로 표현 안 되는 구성(다른 서브넷·전용 iface)용.
  const [vipManual, setVipManual] = useState(false)
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
  // 값 변경 없는 재렌더 — 재설치·복구된 노드가 그룹의 VIP 설정을 따라잡게 한다.
  async function reapplyVip() {
    try {
      const r = await haGroupsApi.apply(group.id)
      show(`VIP 재적용 — 멤버 ${r.jobs_queued}대에 keepalived 재렌더 큐잉`, 'ok')
      await onReload()
    } catch (e) { show((e as Error).message, 'err') }
  }
  // VIP 를 실제로 들고 있는 멤버 이름 — heartbeat 의 interfaces[] 대조 (iface 매핑과 무관하게
  // IP 로만 판정한다. 매핑이 비었다고 미보유로 읽으면 실제 보유 노드를 놓친다).
  function vipHolders(ip: string): string[] {
    if (!ip) return []
    return memberAgents
      .filter(m => (m.agent?.interfaces || []).some(x => x.ip === ip))
      .map(m => (m.agent ? agentDisplayName(m.agent.name) : `#${m.agent_id}`))
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
  // ── 그룹 공통 마운트 — 선언 갱신 + 전 멤버 fan-out (오프라인 멤버는 결과에 사유) ──
  async function applyGroupMounts(ops: MountOp[], label: string) {
    setMountApplying(true)
    try {
      const r = await haGroupsApi.applyMounts(group.id, ops)
      const failed = r.results.filter(x => !x.ok)
      if (failed.length === 0) show(`${label} — 멤버 ${r.applied}대 적용 (fstab 영속)`, 'ok')
      else show(`${label} — ${r.applied}대 적용 / ${failed.length}대 실패: ` +
                failed.map(f => `${f.name}(${f.error})`).join(', '), 'err')
      await onReload()
    } catch (e) { show((e as Error).message, 'err') }
    finally { setMountApplying(false) }
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
    // 수동 입력 모드에서는 용도를 붙이지 않는다 — IP·iface 를 직접 지정하는 행이다.
    const defaultSlot = vipManual ? '' : (availableSlots[0] || '')
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
      <div style={{ padding: '12px 16px', borderBottom: '1px solid var(--border)' }}>
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
                    title="멤버별 설정값 나란히 비교 (읽기 전용) — 편집은 각 멤버 서버의 패키지 설정 탭">
              🔍 설정 비교
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
                        {/* A/S = 실제 VIP 보유. 기본은 heartbeat 관측(≤30s 지연, R4) —
                            [🔄 실측] 은 sync health-check 로 즉시 재확인 (관측 override). */}
                        {(() => {
                          const o = vipObs[a.id]
                          if (o === 'active') return (
                            <span title="VIP 실제 보유 — Active (실측)"
                                  style={{ fontSize: 11, color: '#27ae60', fontWeight: 600 }}>● Active</span>)
                          if (o === 'standby') return (
                            <span title="VIP 미보유 — Standby (실측)"
                                  style={{ fontSize: 11, color: 'var(--text-muted)' }}>○ Standby</span>)
                          if (o === 'fail') return (
                            <span title="점검 실패 — offline 또는 health-check 오류"
                                  style={{ fontSize: 11, color: '#c0392b' }}>✕</span>)
                          if (vipChecking) return (
                            <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>…</span>)
                          const hb = group.members.find(gm => gm.agent_id === a.id)?.vip_observed
                          if (hb === true) return (
                            <span title="VIP 실제 보유 — Active (heartbeat 관측, ≤30s 지연)"
                                  style={{ fontSize: 11, color: '#27ae60', fontWeight: 600 }}>● Active</span>)
                          if (hb === false) return (
                            <span title="VIP 미보유 — Standby (heartbeat 관측, ≤30s 지연)"
                                  style={{ fontSize: 11, color: 'var(--text-muted)' }}>○ Standby</span>)
                          return (
                            <span title="판정 불가 (heartbeat stale·VIP 미설정) — [🔄 실측] 으로 확인"
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
          <div style={{ marginLeft: 'auto', display: 'flex', gap: 6, alignItems: 'center' }}>
            {/* 수동 입력 — 용도(slot) 자동 매핑으로 표현 안 되는 구성(멤버 IP 와 다른 서브넷,
                멤버별 전용 iface)을 위해 IP·iface 를 직접 지정한다. */}
            <label style={{ fontSize: 11, color: 'var(--text-muted)', display: 'flex',
                            alignItems: 'center', gap: 4, cursor: 'pointer' }}
                   title="용도 자동 매핑 대신 IP·멤버 iface 직접 입력 (다른 서브넷·전용 NIC 구성)">
              <input type="checkbox" checked={vipManual}
                     onChange={e => setVipManual(e.target.checked)} />
              수동 입력
            </label>
            <button className="btn btn--sm" onClick={beginAddBinding}
                    disabled={(!vipManual && availableSlots.length === 0) || bindingEditMode !== null}
                    title={(!vipManual && availableSlots.length === 0)
                      ? '먼저 멤버 서버의 [네트워크] 탭에서 IP 의 용도를 입력하세요 (또는 [수동 입력])'
                      : '새 VIP 행 추가 (편집 모드 — 저장 후 [▶ 적용] 로 backend 반영)'}>
              + VIP 추가
            </button>
            {/* 값 변경 없이 keepalived 만 다시 렌더 — 노드가 재설치·복구된 뒤 VIP 설정을
                따라잡게 하는 통로. update 는 값이 바뀌어야 job 이 나간다. */}
            <button className="btn btn--sm" onClick={reapplyVip}
                    disabled={vipDirty || bindingEditMode !== null}
                    title={vipDirty
                      ? '먼저 [▶ 적용] 으로 변경을 저장하세요'
                      : '저장된 VIP 설정을 전 멤버 keepalived 에 다시 내려보냄 (값 변경 없음)'}>
              ↻ 재적용
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
                  <th>멤버 iface</th>
                  <th style={{ width: 120 }}
                      title="이 VIP 를 실제로 들고 있는 멤버 (heartbeat 관측, ≤30s 지연)">보유</th>
                  <th style={{ width: 100 }}>액션</th></tr>
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
                      <td><VipHolderCell holders={vipHolders(b.ip)} /></td>
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
                // edit mode — 수동 입력이면 IP/mask/멤버 iface 를 직접 지정, 아니면 용도 기반 자동 매핑.
                const manualOk = !!b.ip.trim()
                return (
                  <tr key={b.bid} style={{ background: 'var(--warn-soft)' }}>
                    <td>
                      <select className="form-input" value={b.slot}
                              onChange={e => changeBindingSlot(b.bid, e.target.value)}
                              style={{ width: 110, fontSize: 11, padding: 2 }}>
                        <option value="">{vipManual ? '(용도 없음)' : '(용도 선택)'}</option>
                        {availableSlots.map(s => <option key={s} value={s}>{s}</option>)}
                      </select>
                    </td>
                    <td>
                      {vipManual ? (
                        <input className="form-input" value={b.ip}
                               onChange={e => updateBinding(b.bid, { ip: e.target.value })}
                               placeholder="121.161.164.140"
                               style={{ width: 130, fontSize: 11, padding: 2, fontFamily: 'monospace' }} />
                      ) : info?.prefix ? (
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
                                  : '(용도 선택 필요 — 또는 [수동 입력])'}
                        </span>
                      )}
                    </td>
                    <td style={{ fontFamily: 'monospace' }}>
                      {vipManual ? (
                        <input className="form-input" type="number" min={8} max={32}
                               value={b.mask || 24}
                               onChange={e => updateBinding(b.bid, { mask: Number(e.target.value) || 24 })}
                               style={{ width: 55, fontSize: 11, padding: 2 }} />
                      ) : (b.mask || 24)}
                    </td>
                    <td style={{ fontSize: 11, color: 'var(--text-muted)' }}>
                      {vipManual ? (
                        <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
                          {memberAgents.map(m => (
                            <span key={m.agent_id} style={{ display: 'flex', alignItems: 'center', gap: 3 }}>
                              <span style={{ width: 54, overflow: 'hidden', textOverflow: 'ellipsis' }}>
                                {m.agent ? agentDisplayName(m.agent.name) : `#${m.agent_id}`}
                              </span>
                              <input className="form-input"
                                     value={b.memberIfaces?.[m.agent_id] || ''}
                                     onChange={e => updateBinding(b.bid, {
                                       memberIfaces: { ...(b.memberIfaces || {}),
                                                       [m.agent_id]: e.target.value },
                                     })}
                                     placeholder="ens3"
                                     style={{ width: 60, fontSize: 11, padding: 2,
                                              fontFamily: 'monospace' }} />
                            </span>
                          ))}
                        </div>
                      ) : ifaceStr}
                    </td>
                    <td><VipHolderCell holders={[]} editing /></td>
                    <td>
                      <button className="btn btn--sm btn--primary"
                              style={{ fontSize: 10, padding: '1px 5px' }}
                              disabled={vipManual ? !manualOk : (!b.slot || !host || !info?.prefix)}
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
          다른 서브넷·전용 NIC 구성은 [수동 입력] 으로 IP·iface 직접 지정.
        </div>

        {/* 그룹 공통 마운트 — 멤버 전체에 같은 경로. 모듈 로그 수집처(NAS) 등. */}
        <div style={{ marginTop: 20 }}>
          <GroupMountPanel
            declared={group.mounts || []}
            members={memberAgents.map(m => ({
              id: m.agent_id,
              name: m.agent ? agentDisplayName(m.agent.name) : `#${m.agent_id}`,
              online: m.agent?.status === 'online',
              mounts: m.agent?.mounts || [],
            }))}
            applying={mountApplying}
            onApply={applyGroupMounts}
          />
        </div>

        {/* 공유 store 는 이 탭에 없다 — oam/oam-svc 의 [패키지 설정] > 관리 store 로 귀속.
            HA 편입 여부는 그 값에서 유도되고, 미충족 사유는 [패키지 제어] 탭 배너가 알린다. */}
      </div>
    </>
  )
}

// VIP 보유 멤버 셀 — heartbeat 관측(≤30s 지연). 정확히 1명이 정상, 0명은 이동 중/미적용,
// 2명 이상은 split-brain 의심이라 색으로 구분한다.
function VipHolderCell({ holders, editing }: { holders: string[]; editing?: boolean }) {
  if (editing) return <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>—</span>
  if (holders.length === 1) {
    return <span title="이 VIP 를 실제로 보유 (heartbeat 관측)"
                 style={{ fontSize: 11, color: '#27ae60', fontWeight: 600 }}>● {holders[0]}</span>
  }
  if (holders.length === 0) {
    return <span title="어느 멤버도 이 VIP 를 갖고 있지 않음 — 미적용이거나 이동 중"
                 style={{ fontSize: 11, color: '#e67e22' }}>○ 미할당</span>
  }
  return <span title={`동시 보유: ${holders.join(', ')} — split-brain 의심`}
               style={{ fontSize: 11, color: '#c0392b', fontWeight: 600 }}>⚠ {holders.length}곳 보유</span>
}

// AS 절체 조건 (그룹/시스템 스코프) — keepalived advert_int / vrrp_script health /
// preempt / track_interface / restart_limit. 모듈별 값(프로세스 감시·절체 모드)은
// 패키지 설정의 모듈 운영 명세(ModuleSpecSection)로 이관됨.
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
  const rl = value.restart_limit || { max_fails: 3, window_sec: 300 }
  const setRestart = (k: 'max_fails' | 'window_sec', v: number) =>
    set('restart_limit', { ...rl, [k]: v })
  return (
    <div style={{ marginTop: 0, border: '1px solid var(--border)', borderRadius: 4 }}>
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

          <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
            <label style={{ width: 150, color: 'var(--text-muted)' }}
                   title="MASTER 승격 후 이 시간 동안 헬스 실패를 유예 — cold 모듈 기동 시간 흡수 (승격 직후 재장애 방지)">
              승격 유예 (초)
            </label>
            <input type="number" min={0} max={600}
                   value={value.health.grace_sec ?? 30}
                   onChange={e => setHealth('grace_sec', Number(e.target.value) || 0)}
                   className="form-input" style={{ width: 80 }} />
            <span style={{ color: 'var(--text-muted)' }}>기본 30초 (0=유예 없음)</span>
          </div>

          <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
            <label style={{ width: 150, color: 'var(--text-muted)' }}
                   title={
                     '모듈 장애 시 watchdog 이 로컬 재기동을 먼저 시도하고, 윈도우 내 연속 N회 실패하면 ' +
                     '그 노드를 포기하고 절체(VIP 이양)한다. 일시적 crash 1회로 절체하지 않도록 하는 방어선.\n' +
                     '값이 클수록 flap 은 줄지만 진짜 장애의 절체가 늦어진다 (절체 지연 ≈ N × 재기동 backoff).'
                   }>
              재기동 임계
            </label>
            <span style={{ color: 'var(--text-muted)' }}>연속</span>
            <input type="number" min={1} max={20}
                   value={rl.max_fails}
                   onChange={e => setRestart('max_fails', Number(e.target.value) || 3)}
                   className="form-input" style={{ width: 60 }} />
            <span style={{ color: 'var(--text-muted)' }}>회 실패 (윈도우</span>
            <input type="number" min={10} max={3600}
                   value={rl.window_sec}
                   onChange={e => setRestart('window_sec', Number(e.target.value) || 300)}
                   className="form-input" style={{ width: 80 }} />
            <span style={{ color: 'var(--text-muted)' }}>초) → 절체. 기본 3회/300초</span>
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


// ──────────────────────────────────────────────────────────────
//  모듈 운영 명세 (패키지 설정 — 그룹 선택) : 프로세스 감시 / 절체 모드 / 절체 관여
//  앱 config.json 과 물리 분리된 group.module_specs → agent modules/<mod>/service.json.
// ──────────────────────────────────────────────────────────────

const HA_DAEMON_MODULES = ['csp', 'cmp', 'csc', 'psp', 'isp', 'imp', 'pmp']

function _seedSpecs(group: HaGroup, modules: string[]): Record<string, ModuleSpec> {
  const seed: Record<string, ModuleSpec> = {}
  for (const m of modules) {
    const s = group.module_specs?.[m]
    seed[m] = {
      supervision: { watchdog: s?.supervision?.watchdog ?? MODULE_SPEC_DEFAULT.supervision.watchdog },
      ha: {
        failover_mode:     s?.ha?.failover_mode ?? MODULE_SPEC_DEFAULT.ha.failover_mode,
        failover_relevant: s?.ha?.failover_relevant ?? MODULE_SPEC_DEFAULT.ha.failover_relevant,
      },
      safety: { class: s?.safety?.class ?? 'unknown',
                latch_clear_mode: s?.safety?.latch_clear_mode },
      ...(s?.health ? { health: s.health } : {}),
    }
  }
  return seed
}

function ModuleSpecSection({ group, deployments, onReload }: {
  group: HaGroup
  deployments: Deployment[]
  onReload: () => Promise<void> | void
}) {
  const { show } = useToast()
  const [open, setOpen] = useState(false)
  const [saving, setSaving] = useState(false)
  const modules = useMemo(() => {
    const ids = new Set(group.members.map(m => m.agent_id))
    const s = new Set<string>()
    for (const d of deployments) {
      if (!ids.has(d.agent_id) || d.status === 'removed') continue
      const p = (d.process_name || '').toLowerCase()
      if (HA_DAEMON_MODULES.includes(p)) s.add(p)
    }
    return [...s].sort()
  }, [deployments, group.id, group.members])
  const baseline = useMemo(() => _seedSpecs(group, modules),
    [group.id, group.update_time, modules])
  const [specs, setSpecs] = useState<Record<string, ModuleSpec>>(baseline)
  useEffect(() => { setSpecs(baseline) }, [baseline])
  const dirty = JSON.stringify(specs) !== JSON.stringify(baseline)

  const setSup = (m: string, watchdog: boolean) =>
    setSpecs(s => ({ ...s, [m]: { ...s[m], supervision: { watchdog } } }))
  const setMode = (m: string, mode: 'cold' | 'hot') =>
    setSpecs(s => ({ ...s, [m]: { ...s[m], ha: { ...s[m].ha, failover_mode: mode } } }))
  const setRelevant = (m: string, v: boolean) =>
    setSpecs(s => ({ ...s, [m]: { ...s[m], ha: { ...s[m].ha, failover_relevant: v } } }))
  const setSafety = (m: string, cls: SafetyClass) =>
    setSpecs(s => ({ ...s, [m]: { ...s[m], safety: { class: cls } } }))

  async function save() {
    if (!dirty) return
    setSaving(true)
    try {
      await haGroupsApi.update(group.id, { module_specs: specs })
      show('모듈 운영 명세 적용됨 (각 노드 service.json 반영)', 'ok')
      await onReload()
    } catch (e) {
      show(`저장 실패: ${e instanceof Error ? e.message : e}`, 'err')
    } finally {
      setSaving(false)
    }
  }

  if (modules.length === 0) return null
  return (
    <div style={{ marginBottom: 16, border: '1px solid var(--border)', borderRadius: 4 }}>
      <div style={{ padding: '8px 12px', background: 'var(--bg-soft)',
                    display: 'flex', alignItems: 'center', gap: 8, fontWeight: 600, fontSize: 13 }}
           title="모듈별 운영 설정 (앱 설정과 별개) — 각 노드 modules/<mod>/service.json 으로 반영">
        <span onClick={() => setOpen(v => !v)} style={{ fontSize: 11, cursor: 'pointer' }}>{open ? '▼' : '▶'}</span>
        <span onClick={() => setOpen(v => !v)} style={{ cursor: 'pointer' }}>모듈 운영 명세 (감시 · 절체 모드)</span>
        <span style={{ fontSize: 11, color: 'var(--text-muted)', fontWeight: 400, cursor: 'pointer' }}
              onClick={() => setOpen(v => !v)}>
          {modules.join(', ')}
        </span>
        <button className="btn btn--sm btn--primary" style={{ marginLeft: 'auto' }}
                onClick={save} disabled={!dirty || saving}
                title="모듈 운영 명세 변경을 각 멤버 노드에 반영 (service.json + keepalived 재렌더)">
          ▶ 적용
        </button>
      </div>
      {open && (
        <div style={{ padding: 12, fontSize: 12, overflowX: 'auto' }}>
          <table className="data-table" style={{ minWidth: 560 }}>
            <thead>
              <tr>
                <th style={{ textAlign: 'left' }}>모듈</th>
                <th title="프로세스 감시(watchdog) — 죽으면 자동 재기동. 끄면 재기동 안 함(장애 시 즉시 절체 판정).">프로세스 감시</th>
                <th title="Cold(기본): standby 정지 + 승격 시 기동 / Hot: 양쪽 상시 기동(VIP-only). AS 만 적용.">절체 모드</th>
                <th title="이 모듈 실패가 절체 사유가 되는지. 끄면 이 모듈이 죽어도 절체하지 않음(부가 모듈).">절체 관여</th>
                <th title="안전 등급 — shared_writer/unknown 은 자동 래치 해제 금지(수동 확인 필요). VIP 없이 DB/파일에 쓰는 모듈은 fencing/lease 전제.">안전 등급</th>
              </tr>
            </thead>
            <tbody>
              {modules.map(m => {
                const sp = specs[m] || MODULE_SPEC_DEFAULT
                return (
                  <tr key={m}>
                    <td><b>{m}</b></td>
                    <td style={{ textAlign: 'center' }}>
                      <input type="checkbox" checked={sp.supervision.watchdog}
                             onChange={e => setSup(m, e.target.checked)} />
                    </td>
                    <td style={{ textAlign: 'center' }}>
                      <select value={sp.ha.failover_mode}
                              onChange={e => setMode(m, e.target.value as 'cold' | 'hot')}
                              className="form-input" style={{ fontSize: 11, height: 22 }}
                              disabled={group.mode !== 'active_standby'}>
                        <option value="cold">Cold</option>
                        <option value="hot">Hot</option>
                      </select>
                    </td>
                    <td style={{ textAlign: 'center' }}>
                      <input type="checkbox" checked={sp.ha.failover_relevant}
                             onChange={e => setRelevant(m, e.target.checked)} />
                    </td>
                    <td style={{ textAlign: 'center' }}>
                      <select value={sp.safety?.class ?? 'unknown'}
                              onChange={e => setSafety(m, e.target.value as SafetyClass)}
                              className="form-input" style={{ fontSize: 11, height: 22 }}>
                        <option value="stateless">stateless</option>
                        <option value="read_only">read_only</option>
                        <option value="shared_writer">shared_writer</option>
                        <option value="unknown">unknown</option>
                      </select>
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
          <div style={{ marginTop: 8, fontSize: 11, color: 'var(--text-muted)' }}>
            이 설정은 앱 설정(config.json)과 별개 파일(service.json)로 각 노드에 저장되며 agent 가 감시·절체 판정에 사용합니다.
            안전 등급 shared_writer/unknown 은 절체 후 자동 복귀(래치 해제)를 하지 않고 운영자 확인을 요구합니다.
          </div>
        </div>
      )}
    </div>
  )
}


// ──────────────────────────────────────────────────────────────
//  Inspector (선택된 서버 상세)
// ──────────────────────────────────────────────────────────────

type InspectorTab = 'install' | 'info' | 'network' | 'modules'

function ServerInspector({ agent: a, mode, deployments, packages, vipIps, mgmtVip,
                          onApprove, onRevoke, onRemove, onRename, onUpgrade, onRestart, onRollbackAgent, onMetrics, onHealthCheck,
                          onAddDeploy, onJob, onUpgradeDep, onRollback, onRemoveDep }: {
  agent: Agent
  // infra=시스템/서버 구성 (설치안내/정보/네트워크), install=패키지 설치 (모듈 파일 배치),
  // control=패키지 제어 (프로세스 start/stop/restart)
  mode: 'infra' | 'install' | 'control'
  deployments: Deployment[]
  packages: SipPackage[]
  vipIps?: Set<string>
  /** 관리평면(oam 호스팅) 그룹의 VIP — OAM 접속 주소 권장값 */
  mgmtVip?: string | null
  onApprove: (a: Agent) => void
  onRevoke: (a: Agent) => void
  onRemove: (a: Agent) => void
  onRename: (a: Agent) => void
  onUpgrade: (a: Agent) => void
  onRestart: (a: Agent) => void
  onRollbackAgent: (a: Agent) => void
  onMetrics: (a: Agent) => void
  onHealthCheck: (a: Agent) => void
  onAddDeploy: () => void
  onJob: (d: Deployment, jt: JobType) => void
  onUpgradeDep: (d: Deployment) => void
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
      <div style={{ padding: '12px 16px', borderBottom: '1px solid var(--border)' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
          <span style={{
            width: 10, height: 10, borderRadius: '50%', background: sc.bar, display: 'inline-block',
          }} />
          <b style={{ fontSize: 16 }}>{agentDisplayName(a.name)}</b>
          {agentDisplayName(a.name) !== a.name && (
            <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>{a.name}</span>
          )}
          {/* 이름은 표시 라벨이다 — 시스템은 #id 로 동작하므로 바꿔도 파급이 없다
              (identifier_model.md). 그래서 별도 확인·경고 없이 바로 고친다. */}
          <button className="btn btn--sm btn--ghost" style={{ padding: '0 6px' }}
                  title="서버 이름 변경 (표시용 — 시스템은 #id 로 동작)"
                  onClick={() => onRename(a)}>✎</button>
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
                {(a.agent_versions || []).filter(v => v && v !== a.agent_version).length > 0 && (
                  <button className="btn btn--sm" onClick={() => onRollbackAgent(a)}
                    disabled={a.status !== 'online'} title="agent 를 직전(또는 선택) 버전으로 롤백 (current flip + execv)">
                    ↓ 롤백
                  </button>
                )}
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
              <NetworkTab agent={a} vipIps={vipIps} mgmtVip={mgmtVip} />
            </InspectorSection>
          </>
        )}
        {mode === 'install' && (
          <InspectorSection title={`모듈 (${deployments.length})`}
                            expanded={openSections.has('modules')}
                            onToggle={() => toggleSection('modules')}>
            <ModulesTab agent={a} deployments={deployments} packages={packages} packagesAvailable={packages.length > 0}
              onAddDeploy={onAddDeploy}
              onJob={onJob} onUpgrade={onUpgradeDep} onRollback={onRollback} onRemoveDep={onRemoveDep} />
          </InspectorSection>
        )}
        {mode === 'control' && (
          <InspectorSection title={`모듈 제어 (${deployments.length})`}
                            expanded={openSections.has('modules')}
                            onToggle={() => toggleSection('modules')}>
            <ControlTab agent={a} deployments={deployments} packages={packages} onJob={onJob} />
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
  // pending(설치 전) 도 포함 — DB/notify/시크릿을 설치 전에 미리 지정(overlay 저장→설치 시 반영).
  const [deps] = useState(() => deployments.filter(d => d.status !== 'removed'))
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
                  <span style={{ color: agentStatusColor(ag?.status || 'offline').bar, fontSize: 12 }}>
                    ● {ag?.status || '—'}
                  </span>
                </td>
                <td>
                  {deps.length === 0 ? <span style={{ color: 'var(--text-muted)' }}>—</span> :
                    deps.map(d => {
                      // 실측 우선 — [패키지 제어] 탭과 같은 상태로 보이게(설치·제어 일치)
                      const shown = depEffectiveStatus(d)
                      return (
                      <span key={d.id} className="tag" style={{
                        background: depStatusColor(shown), color: '#fff',
                        fontSize: 11, padding: '2px 8px', borderRadius: 3, marginRight: 6,
                      }}>
                        {d.package_name} v{d.package_version} · {shown}
                      </span>
                      )
                    })}
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
    <div style={{ borderBottom: '1px solid var(--border)' }}>
      <div onClick={onToggle}
           style={{
             display: 'flex', alignItems: 'center', gap: 8,
             padding: '10px 16px', cursor: 'pointer',
             background: 'var(--bg-soft)', userSelect: 'none',
             borderBottom: expanded ? '1px solid var(--border)' : 'none',
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

function ModulesTab({ agent: a, deployments, packages, packagesAvailable,
                     onAddDeploy, onJob, onUpgrade, onRollback, onRemoveDep }: {
  agent: Agent
  deployments: Deployment[]
  packages: SipPackage[]
  packagesAvailable: boolean
  onAddDeploy: () => void
  onJob: (d: Deployment, jt: JobType) => void
  onUpgrade: (d: Deployment) => void
  onRollback: (d: Deployment) => void
  onRemoveDep: (d: Deployment) => void
}) {
  const pkgDesc = new Map(packages.map(p => [p.name, p.description]))
  return (
    <>
      {deployments.length === 0 ? (
        <div className="empty">배포된 모듈 없음</div>
      ) : (
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 10 }}></th>
              <th>이름</th>
              <th>설명</th>
              <th>모듈 · 버전</th>
              <th>상태</th>
              <th style={{ width: 300 }}>작업</th>
            </tr>
          </thead>
          <tbody>
            {deployments.map(d => (
              <DeploymentRow key={d.id} dep={d} agent={a} packages={packages}
                desc={pkgDesc.get(d.package_name || '') ?? null}
                onJob={onJob} onUpgrade={onUpgrade} onRollback={onRollback}
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

// [패키지 설치] 탭 모듈 행 — 파일 배치 작업만 (설치/재설치/업그레이드/롤백/삭제).
// 프로세스 start/stop/restart 는 [패키지 제어] 탭, 설정은 [패키지 설정] 탭.
function DeploymentRow({ dep: d, agent, packages, desc, onJob, onUpgrade, onRollback, onRemove }: {
  dep: Deployment; agent: Agent
  packages: SipPackage[]
  desc: string | null
  onJob: (d: Deployment, jt: JobType) => void
  onUpgrade: (d: Deployment) => void
  onRollback: (d: Deployment) => void
  onRemove: (d: Deployment) => void
}) {
  // 상태 배지·색은 실측 우선(depEffectiveStatus) — [패키지 제어] 탭과 동일 기준.
  // 죽어 있으면 마지막 job 결과가 running 이어도 stopped 로 보인다(두 탭 일치).
  const shown = depEffectiveStatus(d)
  const sc = depStatusColor(shown)
  const online = agent.status === 'online'
  // pending = 생성만 됨 (파일 없음), stopped = 설치됐지만 실행 안됨
  const notInstalled = d.status === 'pending'
  // 버전 단위 설치: 이전 버전 경로가 보존돼 있을 때만 롤백 가능
  const canRollback = online && (d.status === 'running' || d.status === 'stopped') && !!d.prev_install_path
  // 업그레이드 대상 = 같은 모듈의 다른 버전 패키지. 설치 전(pending)은 [설치] 가 할 일이라 제외.
  // **실행 중이면 불가** — 서버도 거부하지만(우회 없음), 눌러보고 알게 하지 않는다.
  //   정지가 곧 "서비스에서 뺐다"는 확인 절차다. A/A(cmp·cmdp)는 active/standby 개념이
  //   없어 이 전제가 두 노드 동시 다운을 막는 유일한 장치다.
  const upCands = packages.filter(p => p.name === d.package_name && p.id !== d.package_id)
  const isRunning = shown === 'running'
  const canUpgrade = online && !notInstalled && !isRunning && upCands.length > 0
  const histTip = (d.install_history || [])
    .map(h => `v${h.version || '?'} ${h.at} ${h.install_path}`).join('\n')
  return (
    <tr>
      <td style={{ padding: 0 }}>
        <div style={{ width: 4, background: sc, height: 32 }} />
      </td>
      <td><b>{d.process_name || '—'}</b></td>
      <td style={{ fontSize: 12, color: 'var(--text-muted)' }}>
        {desc || '—'}
      </td>
      <td style={{ fontSize: 12 }}
          title={`설치 경로: ${d.install_path || '—'}${histTip ? `\n\n설치 이력:\n${histTip}` : ''}`}>
        {d.package_name} <span style={{ color: 'var(--text-muted)' }}>v{d.package_version}</span>
      </td>
      <td>
        <span className="tag" style={{
          background: sc, color: '#fff', fontSize: 10, padding: '1px 6px', borderRadius: 3,
        }}>{shown}</span>
      </td>
      <td>
        <div style={{ display: 'flex', gap: 4, flexWrap: 'wrap' }}>
          <button className="btn btn--sm" disabled={!online} title="install (파일 배치 + 설정 적용)"
            onClick={() => onJob(d, 'install')}>
            {notInstalled ? '설치' : '재설치'}
          </button>
          <button className="btn btn--sm" disabled={!canUpgrade}
            title={!online ? 'agent 오프라인'
              : notInstalled ? '아직 설치 전 — [설치] 를 먼저 하세요'
              : isRunning ? '실행 중에는 업그레이드할 수 없습니다 — [패키지 제어] 에서 정지 후 진행하세요'
              : upCands.length === 0 ? `${d.package_name} 의 다른 버전 패키지가 없음 (릴리스에 업로드 필요)`
              : `버전을 골라 업그레이드 (등록됨: ${upCands.map(p => 'v' + p.version).join(', ')})`}
            onClick={() => onUpgrade(d)}>↑ 업그레이드</button>
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

// ── [패키지 제어] 탭 — 서버 선택: 모듈별 프로세스 start/stop/restart ──
function ControlTab({ agent: a, deployments, packages, onJob }: {
  agent: Agent
  deployments: Deployment[]
  packages: SipPackage[]
  onJob: (d: Deployment, jt: JobType) => void
}) {
  const pkgDesc = new Map(packages.map(p => [p.name, p.description]))
  if (deployments.length === 0) {
    return <div className="empty">배포된 모듈 없음 — [패키지 설치] 탭에서 모듈을 먼저 배포하세요</div>
  }
  return (
    <table className="data-table">
      <thead>
        <tr>
          <th style={{ width: 10 }}></th>
          <th>이름</th>
          <th>설명</th>
          <th>모듈 · 버전</th>
          <th>상태</th>
          <th style={{ width: 220 }}>제어</th>
        </tr>
      </thead>
      <tbody>
        {deployments.map(d => (
          <tr key={d.id}>
            <td style={{ padding: 0 }}>
              <div style={{ width: 4, background: depStatusColor(depEffectiveStatus(d)), height: 32 }} />
            </td>
            <td><b>{d.process_name || '—'}</b></td>
            <td style={{ fontSize: 12, color: 'var(--text-muted)' }}>
              {pkgDesc.get(d.package_name || '') ?? '—'}
            </td>
            <td style={{ fontSize: 12 }}>
              {d.package_name} <span style={{ color: 'var(--text-muted)' }}>v{d.package_version}</span>
            </td>
            <td>
              <DepStatusCell dep={d} />
            </td>
            <td><ProcessControlButtons dep={d} agent={a} onJob={onJob} /></td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}

// 실측 우선 유효 상태 — running/stopped 구간에서는 실측(live_state)이 정본.
// 기록(status)은 운영자 지시 이력일 뿐, HA 절체(notify)가 로컬에서 모듈을 켜고
// 끄면 현실과 어긋난다. pending/deploying/failed/removed 는 lifecycle 상태라 기록 유지.

// 모듈 상태 셀 — 실측(depEffectiveStatus) 단일 표시. running/stopped 는 실제
// 프로세스 상태 그 자체다 (metric 주기상 최대 30초 지연만 존재).
function DepStatusCell({ dep: d }: { dep: Deployment }) {
  const shown = depEffectiveStatus(d)
  return (
    <span className="tag" style={{
      background: depStatusColor(shown), color: '#fff', fontSize: 10, padding: '1px 6px', borderRadius: 3,
    }}>{shown}</span>
  )
}

// 프로세스 제어 버튼 3종 — ControlTab(서버)·GroupControlMatrix(그룹) 공용.
// pending(미설치) 은 전부 비활성 — 설치는 [패키지 설치] 탭.
function ProcessControlButtons({ dep: d, agent, onJob }: {
  dep: Deployment; agent?: Agent
  onJob: (d: Deployment, jt: JobType) => void
}) {
  const online = agent?.status === 'online'
  const notInstalled = d.status === 'pending'
  const canStart = online && !notInstalled && (d.status === 'stopped' || d.status === 'running')
  const canOps   = online && !notInstalled && (d.status === 'running' || d.status === 'stopped')
  const pendingTip = notInstalled ? '설치 필요 — [패키지 설치] 탭에서 먼저 설치' : ''
  return (
    <div style={{ display: 'flex', gap: 4, flexWrap: 'wrap' }}>
      <button className="btn btn--sm" disabled={!canStart} title={pendingTip || 'start'}
        onClick={() => onJob(d, 'start')}>▶ 시작</button>
      <button className="btn btn--sm" disabled={!canOps} title={pendingTip || 'restart'}
        onClick={() => onJob(d, 'restart')}>↻ 재시작</button>
      <button className="btn btn--sm" disabled={!canOps} title={pendingTip || 'stop'}
        onClick={() => onJob(d, 'stop')}>■ 정지</button>
    </div>
  )
}

// ── [패키지 제어] 탭 — 그룹 선택: 일괄 제어 바 + 멤버 × 모듈 프로세스 상태/제어 매트릭스 ──
function GroupControlMatrix({ group, agents, depsByAgent, onJob, onSelectMember, onReload }: {
  group: HaGroup
  agents: Agent[]
  depsByAgent: Map<number, Deployment[]>
  onJob: (d: Deployment, jt: JobType) => void
  onSelectMember: (aid: number) => void
  onReload: () => Promise<void> | void
}) {
  const { show } = useToast()
  const [busy, setBusy] = useState<string | null>(null)
  const isAS = group.mode === 'active_standby'
  const activeName = group.active_agent_id != null
    ? agentDisplayName(agents.find(a => a.id === group.active_agent_id)?.name || `#${group.active_agent_id}`)
    : null

  async function batch(action: 'start' | 'stop' | 'restart') {
    const label = action === 'start' ? '일괄 시작' : action === 'stop' ? '일괄 중지' : '일괄 재시작'
    if (action === 'stop' && !window.confirm(
        `[${group.name}] 그룹의 서비스를 전부 중지합니다.\nVIP(가상 IP)도 내려가 서비스가 완전히 중단됩니다. 계속할까요?`))
      return
    setBusy(action)
    try {
      const r = await haGroupsApi.control(group.id, action)
      show(`${label} — job ${r.jobs}건 큐잉 (모듈: ${r.modules.join(', ') || '없음'})`, 'ok')
      await onReload()
    } catch (e) {
      show(`${label} 실패: ${e instanceof Error ? e.message : e}`, 'err')
    } finally {
      setBusy(null)
    }
  }

  async function doFailover(force = false) {
    if (!force && !window.confirm(
        `[${group.name}] 수동 절체 — 현재 Active(${activeName || '?'}) 에서 Standby 로 서비스를 넘깁니다.\n` +
        `절체 중 수 초의 순단이 발생할 수 있습니다. 계속할까요?`))
      return
    setBusy('failover')
    try {
      const r = await haGroupsApi.failover(group.id, force)
      const to = agentDisplayName(agents.find(a => a.id === r.to_agent_id)?.name || `#${r.to_agent_id}`)
      show(`수동 절체 큐잉 — → ${to} 로 스위치오버`, 'ok')
      await onReload()
    } catch (e) {
      // 사전 점검 — agent 가 구 Active 주소를 보고 있으면 절체 후 fleet 이 단절된다.
      // 막다른 골목으로 두지 않고 전환을 권하거나 강행을 선택하게 한다.
      if (e instanceof ApiError && e.data?.error === 'agents_not_on_vip') {
        const list = (e.data.agents as Array<{ name: string; oam_url: string }> | undefined) || []
        const lines = list.slice(0, 6).map(a => `  · ${a.name} → ${a.oam_url}`).join('\n')
        if (window.confirm(
            `${(e as Error).message}\n\n${lines}\n\n` +
            `지금 전 agent 의 OAM 주소를 VIP 로 바꿀까요? (취소 = 아무것도 하지 않음)\n` +
            `개별 서버만 바꾸려면 [시스템/서버 구성] > 서버 > OAM 접속 주소 를 쓰세요.`)) {
          setBusy(null)
          await doRetargetOamUrl()
          return
        }
        show('절체 취소됨 — 먼저 OAM 주소를 VIP 로 전환하세요', 'err')
        return
      }
      const msg = e instanceof Error ? e.message : String(e)
      show(`수동 절체 실패: ${msg}`, 'err')
    } finally {
      setBusy(null)
    }
  }

  // OAM 주소 VIP 전환 — 전 agent 가 VIP 를 보게 한다. 각 agent 가 새 주소로 /health
  // 도달 확인 후에만 적용하므로 VIP 가 없을 때 눌러도 fleet 이 끊기지 않는다.
  async function doRetargetOamUrl() {
    // VipBinding 의 주소 필드는 `ip` 다(`vip` 아님). 다중 VIP 면 관리 접속용을 고르되,
    // slot 이름에 admin/oam/mgmt 가 들어간 것을 우선하고 없으면 첫 항목. legacy vip 폴백.
    const binds = group.vip_bindings || []
    const admin = binds.find(b => /admin|oam|mgmt/i.test(b.slot || ''))
    const vip = ((admin || binds[0])?.ip || group.vip || '').trim()
    if (!vip) { show('이 그룹에 VIP 가 없습니다', 'err'); return }
    const url = `https://${vip}:4419`
    if (!window.confirm(
        `전 agent 의 OAM 접속 주소를 아래로 전환합니다.\n\n  ${url}\n\n` +
        `각 agent 가 그 주소로 /health 도달을 확인한 뒤에만 적용합니다 — 도달 불가면 ` +
        `주소를 바꾸지 않고 실패로 남습니다(fleet 단절 방지).\n\n진행할까요?`)) return
    setBusy('retarget')
    try {
      const r = await deploymentApi.retargetOamUrl(url)
      show(`OAM 주소 전환 큐잉 — ${r.jobs.length}개 agent (${url})`, 'ok')
      await onReload()
    } catch (e) {
      show(`주소 전환 실패: ${(e as Error).message}`, 'err')
    } finally {
      setBusy(null)
    }
  }

  // 노드 유지보수(EXCLUDE_NODE) — 지정 멤버를 승격 대상에서 제외(on)/복귀(off).
  async function doMaintenance(agentId: number, on: boolean) {
    const nm = agentDisplayName(agents.find(a => a.id === agentId)?.name || `#${agentId}`)
    if (!window.confirm(on
        ? `[${nm}] 를 유지보수(EXCLUDE_NODE)로 전환합니다.\n이 노드는 승격 대상에서 제외되고 모듈이 정지됩니다. 상대 노드가 죽어도 이 노드로 절체되지 않습니다(다운 감수). 계속할까요?`
        : `[${nm}] 유지보수를 해제합니다.\nrole 기반으로 모듈이 재기동되어 standby 로 재합류합니다. 계속할까요?`))
      return
    setBusy(`maint:${agentId}`)
    try {
      await haGroupsApi.maintenance(group.id, agentId, on)
      show(on ? `${nm} 유지보수 진입 (승격 제외)` : `${nm} 유지보수 해제 (재합류)`, 'ok')
      await onReload()
    } catch (e) {
      show(`유지보수 변경 실패: ${e instanceof Error ? e.message : e}`, 'err')
    } finally {
      setBusy(null)
    }
  }

  // 멤버 서버 셀에 붙는 유지보수 토글 (AS 만).
  const maintCtl = (agentId: number) => isAS ? (
    <div style={{ marginTop: 6, display: 'flex', gap: 4 }}>
      <button className="btn btn--sm" style={{ fontSize: 11, padding: '1px 6px' }}
              disabled={!!busy} onClick={() => doMaintenance(agentId, true)}
              title="이 노드를 승격 대상에서 제외(유지보수). 모듈 정지 + 이 노드로 절체 안 됨.">
        🔧 점검
      </button>
      <button className="btn btn--sm" style={{ fontSize: 11, padding: '1px 6px' }}
              disabled={!!busy} onClick={() => doMaintenance(agentId, false)}
              title="유지보수 해제 — role 기반 재기동으로 standby 재합류.">
        복귀
      </button>
    </div>
  ) : null

  return (
    <div style={{ padding: 20, overflow: 'auto' }}>
      <h4 style={{ marginTop: 0 }}>멤버별 프로세스 제어 — {group.name}</h4>
      <p style={{ fontSize: 12, color: 'var(--text-muted)' }}>
        그룹 멤버 전체의 모듈 프로세스 상태를 한눈에 보고 시작/재시작/정지합니다.
        설치/재설치/롤백은 [패키지 설치] 탭에서 수행합니다.
      </p>
      {/* 그룹 일괄 제어 바 — 서비스 의도 전환(무장/비무장) + 수동 절체 */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap',
                    padding: '10px 12px', marginBottom: 12, background: 'var(--bg-soft)',
                    border: '1px solid var(--border)', borderRadius: 4 }}>
        <span style={{ fontSize: 12, fontWeight: 600 }}>그룹 일괄 제어</span>
        <button className="btn btn--sm btn--primary" disabled={!!busy}
                onClick={() => batch('start')}
                title="그룹 서비스 시작 — 서비스 의도를 running 으로 두고 무장(VIP 활성). 기준 멤버가 Active 로 기동.">
          ▶ 일괄 시작
        </button>
        <button className="btn btn--sm" disabled={!!busy}
                onClick={() => batch('restart')}
                title="그룹 전 멤버 재시작 — AS 는 standby 먼저, active 는 유예 하에 재시작(절체 없음, 순단 1회).">
          ⟳ 일괄 재시작
        </button>
        <button className="btn btn--sm btn--danger" disabled={!!busy}
                onClick={() => batch('stop')}
                title="그룹 서비스 중지 — 의도를 stopped 로 두고 비무장(VIP 내려감) + 전 모듈 정지.">
          ■ 일괄 중지
        </button>
        {isAS && (
          <button className="btn btn--sm" disabled={!!busy || group.active_agent_id == null}
                  onClick={() => doFailover()}
                  title={group.active_agent_id == null
                    ? 'Active 판정 불가 — 잠시 후 재시도'
                    : '수동 절체 — 현재 Active 에서 Standby 로 서비스를 넘김(스위치오버).'}>
            ⇄ 수동 절체{activeName ? ` (현재 ${activeName})` : ''}
          </button>
        )}
      </div>
      {/* 절체 래치 — 그 노드는 승격 불가다. 노드 로컬 판정이라 예전에는 콘솔에 아무 표시가
          없어, 래치 걸린 노드로는 절체가 영영 안 되는 것을 운영자가 알 수 없었다(실측). */}
      {isAS && (() => {
        const latched = (group.members || [])
          .map(m => agents.find(a => a.id === m.agent_id))
          .filter((a): a is Agent => !!a)
          .filter(a => Object.values(a.ha_state || {}).some(v => v?.latched))
        if (!latched.length) return null
        return (
          <div role="alert" style={{
            marginBottom: 12, padding: '8px 12px', borderRadius: 4, fontSize: 12,
            background: '#7f1d1d', color: '#fff', lineHeight: 1.6,
          }}>
            <b>절체 래치 — 승격 불가: {latched.map(a => agentDisplayName(a.name)).join(', ')}</b>
            <div style={{ marginTop: 4 }}>
              이 노드는 이전 장애 판정이 걸려 있어 <b>절체 대상이 되지 않습니다.</b> 원인을
              확인한 뒤 해당 모듈을 start/restart 하거나 <b>[홀드 해제]</b> 로 풀어야 합니다.
              {latched.map(a => {
                const rs = Object.values(a.ha_state || {})
                  .flatMap(v => v?.reasons || []).slice(0, 4)
                return rs.length ? ` (${agentDisplayName(a.name)}: ${rs.join(', ')})` : ''
              }).join('')}
            </div>
          </div>
        )
      })()}
      {/* agent 의 OAM 접속 주소 어긋남 배너는 여기 없다 — 이 탭은 프로세스 제어다.
          그리고 개시 전에는 어느 노드도 VIP 를 갖지 않아 전 agent 가 노드 IP 로 보고하는
          것이 정상인데, 그때도 붉게 떠서 상시 경고가 되어 신호가 무의미했다.
          지금은 **VIP 가 실제로 붙은 뒤에만** 판정해 알람(A-PRC-003, mo=`a<id>/agent`)으로
          올린다. 값 확인·변경은 [시스템/서버 구성] > 서버 > OAM 접속 주소. */}
      {group.failover_op && (
        <div style={{ marginBottom: 12, padding: '8px 12px', borderRadius: 4, fontSize: 12,
                      border: '1px solid ' + (group.failover_op.error ? '#e57373' : '#90caf9'),
                      background: group.failover_op.error ? '#ffebee' : '#e3f2fd' }}>
          <b>계획 절체 진행</b> — 상태 <code>{group.failover_op.state}</code>
          {` (${agentDisplayName(agents.find(a => a.id === group.failover_op!.source_agent_id)?.name || '?')}`}
          {` → ${agentDisplayName(agents.find(a => a.id === group.failover_op!.target_agent_id)?.name || '?')})`}
          {group.failover_op.note && <span style={{ color: 'var(--text-muted)' }}> · {group.failover_op.note}</span>}
          {group.failover_op.error && <span style={{ color: '#c62828' }}> · 오류: {group.failover_op.error}</span>}
        </div>
      )}
      <table className="data-table">
        <thead>
          <tr><th>서버</th><th>서버 상태</th><th>모듈 · 버전</th><th>모듈 상태</th><th style={{ width: 220 }}>제어</th></tr>
        </thead>
        <tbody>
          {group.members.map(m => {
            const ag = agents.find(a => a.id === m.agent_id)
            const deps = (depsByAgent.get(m.agent_id) || []).filter(d => d.status !== 'removed')
            const serverCells = (
              <>
                <td style={{ cursor: 'pointer' }} onClick={() => onSelectMember(m.agent_id)}
                    title="클릭 시 해당 서버 선택">
                  <b>{agentDisplayName(ag?.name || `#${m.agent_id}`)}</b>
                  <span onClick={e => e.stopPropagation()}>{maintCtl(m.agent_id)}</span>
                </td>
                <td>
                  <span style={{ color: agentStatusColor(ag?.status || 'offline').bar, fontSize: 12 }}>
                    ● {ag?.status || '—'}
                  </span>
                </td>
              </>
            )
            if (deps.length === 0) {
              return (
                <tr key={m.agent_id}>
                  {serverCells}
                  <td colSpan={3} style={{ color: 'var(--text-muted)' }}>배포된 모듈 없음</td>
                </tr>
              )
            }
            return deps.map((d, i) => (
              <tr key={`${m.agent_id}:${d.id}`}>
                {i === 0 ? (
                  <>
                    <td rowSpan={deps.length} style={{ cursor: 'pointer', verticalAlign: 'top' }}
                        onClick={() => onSelectMember(m.agent_id)} title="클릭 시 해당 서버 선택">
                      <b>{agentDisplayName(ag?.name || `#${m.agent_id}`)}</b>
                      <span onClick={e => e.stopPropagation()}>{maintCtl(m.agent_id)}</span>
                    </td>
                    <td rowSpan={deps.length} style={{ verticalAlign: 'top' }}>
                      <span style={{ color: agentStatusColor(ag?.status || 'offline').bar, fontSize: 12 }}>
                        ● {ag?.status || '—'}
                      </span>
                    </td>
                  </>
                ) : null}
                <td style={{ fontSize: 12 }}>
                  <b>{d.process_name || d.package_name}</b>{' '}
                  <span style={{ color: 'var(--text-muted)' }}>{d.package_name} v{d.package_version}</span>
                </td>
                <td>
                  <DepStatusCell dep={d} />
                </td>
                <td><ProcessControlButtons dep={d} agent={ag} onJob={onJob} /></td>
              </tr>
            ))
          })}
        </tbody>
      </table>
    </div>
  )
}

function NetworkTab({ agent: a, vipIps, mgmtVip }: {
  agent: Agent; vipIps?: Set<string>; mgmtVip?: string | null
}) {
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

  // OAM 접속 주소 — 이 주소는 그 노드 agent 의 설정이다(보내는 주체가 agent).
  // agent 가 새 주소로 /health 도달 확인 후에만 적용하므로, VIP 가 아직 없을 때 눌러도
  // fleet 이 끊기지 않는다(job 이 실패로 남을 뿐). oam_ha.md §9.4.1
  async function onApplyOamUrl(url: string) {
    setApplying(true)
    try {
      const r = await deploymentApi.retargetAgentOamUrl(a.id, url)
      show(`OAM 주소 전환 큐잉 — ${a.name} → ${r.url} (도달 확인 후 적용)`, 'ok')
    } catch (e) { show((e as Error).message, 'err') }
    finally { setApplying(false) }
  }

  async function onApplyOamUrlAll(url: string) {
    setApplying(true)
    try {
      const r = await deploymentApi.retargetOamUrl(url)
      show(`OAM 주소 전환 큐잉 — ${r.jobs.length}개 agent (${r.url})`, 'ok')
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
    <OamUrlPanel
      title={`${a.name} — OAM 접속 주소 (agent → OAM)`}
      current={a.oam_url}
      vipCandidate={mgmtVip}
      applying={applying}
      onApply={onApplyOamUrl}
      onApplyAll={onApplyOamUrlAll}
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

/**
 * [+ 멤버 추가] 선택 단계 — 서버 이름 + **마운트 여부·위치**.
 *
 * 이 경로로 들어오는 서버는 그룹 생성과 며칠 떨어질 수 있다(AA 는 이 경로가 유일하다).
 * 그룹 선언을 조용히 상속하면 운영자는 "이 서버는 마운트가 되는가" 를 알 방법이 없고,
 * 그래서 마운트 없는 노드가 조용히 생긴다(실측: Media(AA) 두 노드가 그렇게 돼 서비스 로그를
 * 쓰지 못했다). 기본값은 그룹 선언 → 없으면 이 설치가 이미 쓰는 마운트로 채우고, **확인은
 * 그 자리에서 받는다.** 체크를 끄면 "마운트하지 않음"으로 명시 저장돼 상속으로 뒤집히지 않는다.
 */
function AddMemberModal({ group, serverName, mountSuggestion, onClose, onSubmit }: {
  group: HaGroup
  serverName: string
  mountSuggestion?: PendingMount | GroupMount | null
  onClose: () => void
  onSubmit: (name: string, mounts: PendingMount[]) => Promise<void> | void
}) {
  const [name, setName] = useState(serverName)
  const sug = mountSuggestion
  const [mountOn, setMountOn] = useState(!!sug)
  const [mnt, setMnt] = useState<PendingMount>(sug
    ? { fstype: sug.fstype, source: sug.source, target: sug.target,
        options: sug.options || 'defaults' }
    : { fstype: 'nfs4', source: '', target: '/mnt/cims', options: 'defaults' })
  const [busy, setBusy] = useState(false)
  const mntValid = !!mnt.source.trim() && mnt.target.trim().startsWith('/') && !!mnt.fstype
  const fromGroup = !!group.mounts?.length

  return (
    <Modal title={`${group.name} — 멤버 추가`} onClose={onClose} width={620}>
      <div className="form-grid">
        <label>서버 이름 *</label>
        <input className="form-input" value={name} disabled={busy}
               onChange={e => setName(e.target.value)} />
        <label style={{ gridColumn: '1 / -1', borderTop: '1px solid var(--border)',
                        paddingTop: 10, marginTop: 4 }}>
          <input type="checkbox" checked={mountOn} disabled={busy}
                 onChange={e => setMountOn(e.target.checked)} />
          {' '}공유 스토리지 마운트를 함께 적용
          <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>
            {' '}— 서버 등록 직후 자동으로 붙습니다 (fstab 영속)
          </span>
        </label>
        {mountOn ? (
          <>
            <label>원본 *</label>
            <input className="form-input" value={mnt.source} disabled={busy}
                   placeholder="예: nas.example:/export/cims"
                   onChange={e => setMnt(m => ({ ...m, source: e.target.value }))} />
            <label>붙일 위치 *</label>
            <input className="form-input" value={mnt.target} disabled={busy}
                   placeholder="/mnt/cims"
                   onChange={e => setMnt(m => ({ ...m, target: e.target.value }))} />
            <label>파일시스템 *</label>
            <select className="form-input" value={mnt.fstype} disabled={busy}
                    onChange={e => setMnt(m => ({ ...m, fstype: e.target.value }))}>
              {['nfs4', 'nfs', 'cifs', 'ext4', 'ext3', 'xfs', 'btrfs'].map(f => (
                <option key={f} value={f}>{f}</option>
              ))}
            </select>
            <label style={{ gridColumn: '1 / -1', fontSize: 11,
                            color: mntValid ? 'var(--text-muted)' : '#c0392b' }}>
              {mntValid
                ? <>{fromGroup && <>기본값은 <b>이 그룹의 마운트 선언</b>입니다. </>}
                   등록 직후 <code>{mnt.source}</code> → <code>{mnt.target}</code> ({mnt.fstype},
                   defaults+_netdev,nofail) 로 마운트되고 [마운트 관리]에 표시됩니다.
                   실패해도 서버 등록은 유지됩니다.</>
                : <>원본과 붙일 위치(절대경로)를 입력하세요.</>}
            </label>
          </>
        ) : (
          <label style={{ gridColumn: '1 / -1', fontSize: 11, color: '#b26a00' }}>
            이 서버는 <b>마운트 없이</b> 등록됩니다 — 공유 store·서비스 로그를 쓰는 모듈이라면
            나중에 [마운트 관리]에서 직접 추가해야 합니다.
          </label>
        )}
      </div>
      <div className="modal-footer" style={{ marginTop: 16 }}>
        <button className="btn" onClick={onClose} disabled={busy}>취소</button>
        <button className="btn btn--primary" disabled={busy || !name.trim() || (mountOn && !mntValid)}
                onClick={async () => {
                  setBusy(true)
                  try {
                    await onSubmit(name.trim(), mountOn
                      ? [{ ...mnt, target: mnt.target.trim().replace(/\/+$/, '') }]
                      : [])   // [] = 마운트하지 않음(명시) — 그룹 선언 상속 안 함
                  } finally { setBusy(false) }
                }}>
          {busy ? '추가 중…' : '추가'}
        </button>
      </div>
    </Modal>
  )
}

function PendingMemberModal({ info, onClose }: {
  info: { groupName: string; serverName: string; enrollment_token: string; install_command: string
          appliedMounts?: PendingMount[] }
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
      {/* 직전 단계에서 확정한 마운트를 되짚어 보여준다 — 설치 명령을 돌리기 전에
          "이 서버는 마운트가 되는가" 가 화면에 남아 있어야 한다. */}
      {info.appliedMounts?.length ? (
        <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 10, lineHeight: 1.6 }}>
          등록 직후 자동 마운트: {info.appliedMounts.map(m =>
            <code key={m.target}>{m.source} → {m.target} ({m.fstype})</code>)
            .reduce((a, b) => <>{a}, {b}</>)}
        </div>
      ) : (
        <div style={{ fontSize: 12, color: '#b26a00', marginBottom: 10, lineHeight: 1.6 }}>
          이 서버는 <b>마운트 없이</b> 등록됩니다 — 필요하면 [마운트 관리]에서 추가하세요.
        </div>
      )}
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

function SystemCreateModal({ onClose, onDone, onCreated, saAgents, mountSuggestion }: {
  onClose: () => void
  onDone: () => Promise<void> | void
  onCreated: (firstAgentId: number | null) => void
  // 그룹 미소속(standalone) 서버 — AS 멤버로 기존 서버 편입 가능 (부트스트랩
  // 호스트처럼 이미 enroll 된 서버를 두 번째 서버와 A/S 로 묶는 시나리오)
  saAgents: Agent[]
  /** 이 설치가 이미 쓰고 있는 공유 마운트 — 기본값 제안용. 없으면 마운트 행을 끈다. */
  mountSuggestion?: PendingMount | null
}) {
  const { show } = useToast()
  const [name, setName] = useState('')
  const [mode, setMode] = useState<SystemMode>('active_standby')
  const [authPass, setAuthPass] = useState('00000000')  // active_standby 만 사용 (VRRP)
  // AS 멤버 슬롯: 0 = 신규 생성, 그 외 = 기존 standalone agent id
  const [memberSel, setMemberSel] = useState<[number, number]>([0, 0])
  const [creating, setCreating] = useState(false)
  // 생성 결과 — Standalone 1건, AS 2건, AA 0건 (이후 그룹에서 추가)
  const [results, setResults] = useState<Array<{ name: string; enrollment_token: string; install_command: string }> | null>(null)
  const [copiedIdx, setCopiedIdx] = useState<number | null>(null)
  // 공유 마운트 자동 적용 — 이 설치가 이미 쓰는 마운트가 있으면 기본 ON.
  //   마운트를 별도 작업으로 두면 운영자가 잊고, 그 노드는 공유 store 를 못 써 승격
  //   부적격이 된다(실측: 계획 절체가 원본을 내려놓은 뒤에야 드러나 관리평면 단절).
  const [mountOn, setMountOn] = useState(!!mountSuggestion)
  const [mnt, setMnt] = useState<PendingMount>(mountSuggestion
    ?? { fstype: 'nfs4', source: '', target: '/mnt/cims', options: 'defaults' })
  const mntValid = !!mnt.source.trim() && mnt.target.trim().startsWith('/') && !!mnt.fstype
  // AA 는 이 모달에서 **서버를 만들지 않는다**(그룹만 생성, 멤버는 이후 [+ 멤버 추가]).
  // 붙일 대상이 없으므로 마운트 입력을 노출하지 않는다 — 채워도 적용될 곳이 없어
  // "등록 직후 붙습니다" 가 거짓이 된다. AA 의 마운트는 [+ 멤버 추가] 단계가 묻는다.
  const showMount = mode !== 'all_active'
  const pendingMounts = showMount && mountOn && mntValid
    ? [{ ...mnt, target: mnt.target.trim().replace(/\/+$/, '') }] : []

  async function create() {
    const base = name.trim()
    if (!base) { show('이름 필수', 'err'); return }
    setCreating(true)
    try {
      let firstAgentId: number | null = null
      if (mode === 'standalone') {
        // standalone 은 그룹이 없어 선언을 둘 곳이 agent 뿐이다.
        const r = await deploymentApi.createAgent(base, '', pendingMounts)
        await deploymentApi.approveAgent(r.id)
        firstAgentId = r.id
        setResults([{ name: base, enrollment_token: r.enrollment_token, install_command: r.install_command }])
      } else {
        // AS = 2 슬롯 (각각 신규 생성 또는 기존 standalone 서버 편입). AA = 0 (이후 추가).
        if (mode === 'active_standby' && memberSel[0] > 0 && memberSel[0] === memberSel[1]) {
          show('멤버 1·2 에 같은 서버를 선택할 수 없습니다', 'err'); setCreating(false); return
        }
        const slots = mode === 'active_standby' ? [memberSel[0], memberSel[1]] : []
        const memberAgents: Array<{ name: string; enrollment_token: string; install_command: string }> = []
        const groupMembers: Array<{ agent_id: number; role: 'master' | 'backup'; priority: number }> = []
        for (let i = 0; i < slots.length; i++) {
          const role: 'master' | 'backup' = i === 0 ? 'master' : 'backup'
          const priority = i === 0 ? 100 : 90
          if (slots[i] > 0) {
            // 기존 서버 편입 — 이미 enroll 됨 → install-command 불필요, addMember 는
            // 그룹 생성 시 members 로 일괄 (백엔드가 update_ha 를 멤버 전체에 큐잉)
            groupMembers.push({ agent_id: slots[i], role, priority })
          } else {
            const nm = `${base}-${String(i + 1).padStart(2, '0')}`
            // 그룹 소속 멤버는 agent 에 복제하지 않는다 — 아래 그룹 생성의 `mounts` 선언을
            // enroll 이 읽는다(단일 SoT). [+ 멤버 추가]로 늘어나는 멤버도 같은 선언을 쓴다.
            const r = await deploymentApi.createAgent(nm, '')
            await deploymentApi.approveAgent(r.id)
            memberAgents.push({ name: nm, enrollment_token: r.enrollment_token, install_command: r.install_command })
            groupMembers.push({ agent_id: r.id, role, priority })
          }
        }
        if (groupMembers.length > 0) firstAgentId = groupMembers[0].agent_id
        await haGroupsApi.create({
          name: base,
          mode,
          vip: '',
          vip_mask: 24,
          // auth_pass — active_standby 만 의미 (VRRP 인증). all_active 는 keepalived 미사용이라 빈값.
          auth_pass: mode === 'active_standby' ? authPass : '',
          members: groupMembers,
          // 그룹에도 선언을 남긴다 — [마운트 (그룹 공통)] 이 '선언 vs 멤버 적용' 을
          // 대조하는 근거다. 선언이 없으면 나중에 편입된 멤버의 미적용을 짚을 수 없다.
          ...(pendingMounts.length ? { mounts: pendingMounts } : {}),
        })
        setResults(memberAgents)
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
              {[0, 1].map(i => (
                <Fragment key={i}>
                  <label>멤버 {i + 1} ({i === 0 ? 'master' : 'backup'})</label>
                  <select className="form-input" value={memberSel[i]} disabled={creating}
                    onChange={e => setMemberSel(prev => {
                      const next: [number, number] = [...prev] as [number, number]
                      next[i] = Number(e.target.value)
                      return next
                    })}>
                    <option value={0}>신규 서버 생성 — {name || '<이름>'}-{String(i + 1).padStart(2, '0')}</option>
                    {saAgents.map(a => (
                      <option key={a.id} value={a.id} disabled={memberSel[1 - i] === a.id}>
                        기존 서버 편입: {agentDisplayName(a.name)} ({a.status}{a.hostname ? ` · ${a.hostname}` : ''})
                      </option>
                    ))}
                  </select>
                </Fragment>
              ))}
            </>
          )}
          {/* 공유 마운트 — 등록 직후 자동 적용. 마운트를 별도 작업으로 두면 운영자가 잊고,
              그 노드는 공유 store 를 못 써 승격 부적격이 된다(실측: 계획 절체가 원본을
              내려놓은 뒤에야 드러나 관리평면이 끊겼다). 기본값은 이 설치가 이미 쓰는 마운트. */}
          {showMount && (
          <label style={{ gridColumn: '1 / -1', borderTop: '1px solid var(--border)', paddingTop: 10, marginTop: 4 }}>
            <input type="checkbox" checked={mountOn} disabled={creating}
                   onChange={e => setMountOn(e.target.checked)} />
            {' '}공유 스토리지 마운트를 함께 적용
            <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>
              {' '}— 서버 등록 직후 자동으로 붙습니다 (fstab 영속)
            </span>
          </label>
          )}
          {showMount && mountOn && (
            <>
              <label>원본 *</label>
              <input className="form-input" value={mnt.source} disabled={creating}
                placeholder="예: nas.example:/export/cims"
                onChange={e => setMnt(m => ({ ...m, source: e.target.value }))} />
              <label>붙일 위치 *</label>
              <input className="form-input" value={mnt.target} disabled={creating}
                placeholder="/mnt/cims"
                onChange={e => setMnt(m => ({ ...m, target: e.target.value }))} />
              <label>파일시스템 *</label>
              <select className="form-input" value={mnt.fstype} disabled={creating}
                onChange={e => setMnt(m => ({ ...m, fstype: e.target.value }))}>
                {['nfs4', 'nfs', 'cifs', 'ext4', 'ext3', 'xfs', 'btrfs'].map(f => (
                  <option key={f} value={f}>{f}</option>
                ))}
              </select>
              <label style={{ gridColumn: '1 / -1', fontSize: 11, color: mntValid ? 'var(--text-muted)' : '#c0392b' }}>
                {mntValid
                  ? <>등록 직후 <code>{mnt.source}</code> → <code>{mnt.target}</code> ({mnt.fstype},
                     defaults+_netdev,nofail) 로 마운트하고 콘솔 [마운트 관리]에 표시됩니다.
                     마운트가 실패해도 서버 등록은 유지됩니다.</>
                  : <>원본과 붙일 위치(절대경로)를 입력하세요 — 비우면 마운트를 적용하지 않습니다.</>}
              </label>
            </>
          )}
          <label style={{ gridColumn: '1 / -1', fontSize: 12, color: 'var(--text-muted)', marginTop: 4 }}>
            선택: <b>{modeLabel}</b>
            {mode === 'active_standby' && memberSel.every(v => v === 0) &&
              <> · 멤버 이름: <code>{name || '<이름>'}-01</code> (master), <code>{name || '<이름>'}-02</code> (backup)</>}
            {mode === 'active_standby' && memberSel.some(v => v > 0) &&
              <> · 기존 서버는 install-command 없이 즉시 편입되고 HA 설정이 자동 재적용됩니다</>}
            {mode === 'all_active' &&
              <> · 서버는 생성되지 않습니다 — 그룹 생성 후 <b>[+ 멤버 추가]</b> 로 한 대씩
                 추가하고, <b>마운트는 그 단계에서</b> 선택합니다</>}
          </label>
        </div>
      ) : results.length === 0 ? (
        <div style={{ color: '#2ecc71' }}>
          {mode === 'active_standby'
            ? <>✓ 기존 서버들로 A/S 시스템 구성 완료 — 트리에서 그룹을 선택해 VIP 를 설정하세요.</>
            : <>✓ AA 그룹 생성됨. 좌측 트리에서 그룹 선택 후 [+ 멤버 추가] 로 서버를 추가하세요.</>}
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

// 모듈 업그레이드 모달 — 등록된 버전 중에서 **골라서** 올린다.
// 실행 중인 모듈은 애초에 열리지 않는다(버튼이 비활성) — 서버도 409 로 거부한다.
function DeploymentUpgradeModal({ dep: d, packages, onClose, onDone }: {
  dep: Deployment
  packages: SipPackage[]
  onClose: () => void
  onDone: () => Promise<void> | void
}) {
  const { show } = useToast()
  // 후보 = 같은 모듈의 다른 버전. 정렬은 [모듈 추가] 와 같은 규칙(최근 업로드순) —
  // 제품 전반의 '최신' 정의와 일치시킨다(semver 비교가 아니다).
  const cands = useMemo(() => packages
    .filter(p => p.name === d.package_name && p.id !== d.package_id)
    .sort((a, b) => {
      const ta = a.uploaded_at ? Date.parse(a.uploaded_at) : 0
      const tb = b.uploaded_at ? Date.parse(b.uploaded_at) : 0
      if (tb !== ta) return tb - ta
      return b.id - a.id
    }), [packages, d.package_name, d.package_id])
  const [pkgId, setPkgId] = useState<number>(cands[0]?.id ?? 0)
  const [busy, setBusy] = useState(false)
  const target = cands.find(p => p.id === pkgId) || null

  async function run(force?: boolean) {
    if (!target) return
    setBusy(true)
    try {
      const r = await deploymentApi.upgradeDeployment(d.id, target.id, force)
      show(`업그레이드 큐 등록 (#${r.job_id}) v${r.from_version} → v${r.to_version}`
           + (force ? ' — 순서 가드 우회' : ''), 'ok')
      await onDone(); onClose()
    } catch (e) {
      // 관리평면 순서(standby 먼저)는 운영자가 사정을 알고 뒤집을 수 있는 **권고**다.
      // 반면 '실행 중'(module_running)은 우회 수단을 주지 않는다 — 정지가 언제나 가능하다.
      if (e instanceof ApiError && e.status === 409 && e.data?.error === 'upgrade_order_active_first') {
        if (confirm(`${(e as Error).message}\n\n그래도 강행할까요? (순서 가드 우회)`)) {
          setBusy(false); return run(true)
        }
        show('취소됨 — 안전한 순서 유지', 'err')
      } else {
        show((e as Error).message, 'err')
      }
    } finally { setBusy(false) }
  }

  return (
    <Modal title={`${d.package_name} 업그레이드`} onClose={onClose} width={520}>
      <div style={{ fontSize: 13, marginBottom: 12 }}>
        <div style={{ color: 'var(--text-muted)' }}>
          {d.process_name} · 현재 <b>v{d.package_version}</b>
        </div>
      </div>
      {cands.length === 0 ? (
        <div className="empty">
          등록된 다른 버전이 없습니다 — [관리 &gt; 릴리스] 에 먼저 업로드하세요.
        </div>
      ) : (
        <>
          <label style={{ display: 'block', fontSize: 12, marginBottom: 4 }}>올릴 버전</label>
          <select className="input" style={{ width: '100%' }} value={pkgId}
                  onChange={e => setPkgId(Number(e.target.value))}>
            {cands.map((p, i) => (
              <option key={p.id} value={p.id}>
                v{p.version}{i === 0 ? '  (최신 업로드)' : ''}
                {p.uploaded_at ? `  — ${fmtRelTime(p.uploaded_at)}` : ''}
              </option>
            ))}
          </select>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 10, lineHeight: 1.7 }}>
            · 파일만 설치되고 <b>자동으로 시작하지 않습니다</b> — 확인 후 [패키지 제어] 에서 시작하세요.<br />
            · 설정은 이관됩니다(collection + 배포 설정). 새 항목은 기본값.<br />
            · 구 버전(v{d.package_version})은 보존되어 곧바로 <b>⤺ 롤백</b> 할 수 있습니다.
          </div>
        </>
      )}
      <div style={{ marginTop: 16, display: 'flex', gap: 8, justifyContent: 'flex-end' }}>
        <button className="btn btn--sm" onClick={onClose}>취소</button>
        <button className="btn btn--sm btn--primary" disabled={!target || busy}
          onClick={() => run()}>
          {busy ? '진행 중…' : target ? `v${target.version} 로 업그레이드` : '업그레이드'}
        </button>
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

  // 모듈 바뀌면 버전/모듈 이름 리셋
  /* eslint-disable react-hooks/set-state-in-effect */
  useEffect(() => {
    if (!moduleName) { setPkgId(0); setProcessName(''); return }
    const latest = (pkgsByModule.get(moduleName) || [])[0]
    setPkgId(latest ? latest.id : 0)
  }, [moduleName, pkgsByModule])

  // 버전 바뀌면 모듈 이름 디폴트 반영
  useEffect(() => {
    if (!selectedPkg) { setProcessName(''); return }
    const procs = selectedPkg.meta?.service?.processes || []
    setProcessName(procs.length > 0 ? procs[0] : (selectedPkg.name || '').toUpperCase())
  }, [selectedPkg])
  /* eslint-enable react-hooks/set-state-in-effect */

  async function create() {
    if (!pkgId) { show('모듈/버전 선택 필요', 'err'); return }
    if (!processName.trim()) { show('모듈 이름 필수', 'err'); return }
    try {
      await deploymentApi.createDeployment({
        agent_id: agent.id,
        package_id: pkgId,
        process_name: processName.trim(),
        service_functions: [],
        note: note || undefined,
      })
      show(`${agent.name} 에 ${processName} 배포 추가 (설치 전)`, 'ok')
      // 이중화 전제(공유 store) 미충족은 여기서 알리지 않는다 — 모듈을 추가할 때마다 뜨면
      // 방해만 된다. 상태는 HA 화면 '공유 store' 패널이 상시 표시한다(응답 `warning` 은
      // API/CLI 용으로 남는다).
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
            <label>3. 모듈 이름 *</label>
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

            <label>4. 설명</label>
            <div style={{
              border: '1px solid var(--border)', borderRadius: 4, padding: 8,
              fontSize: 13, color: 'var(--text)', whiteSpace: 'pre-wrap', minHeight: 36,
            }}>
              {selectedPkg.description
                ? selectedPkg.description
                : <span className="text-muted" style={{ fontSize: 12 }}>(패키지에 설명 없음)</span>}
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

