import { AlertTriangle, ArrowDown, ArrowRight, ArrowUp, Check, ChevronDown, ChevronRight, Copy, Hourglass, Lock, LockOpen, Pencil, Plus, RefreshCw, RotateCw, ShieldCheck, Stethoscope, Trash2, X } from 'lucide-react'
import { type ReactNode, useCallback, useEffect, useMemo, useState } from 'react'
import { useSearchParams } from 'react-router-dom'
import { Alert } from '../components/ui/alert'
import { Badge } from '../components/ui/badge'
import {
  DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuSeparator, DropdownMenuTrigger,
} from '../components/ui/dropdown-menu'
import { StatusDot, type StatusTone } from '../components/custom/status-dot'
import { Button } from '../components/ui/button'
import { DataTable, Th, Td, orDash } from '../components/custom/data-table'
import { EmptyState } from '../components/custom/empty-state'
import { SubSection } from '../components/custom/collapsible-section'
import { FormField } from '../components/custom/form-field'
import { StickySaveBar } from '../components/custom/sticky-save-bar'
import { useConfirm } from '../components/custom/confirm'
import { Radio } from '../components/custom/radio'
import { Checkbox } from '../components/ui/checkbox'
import {
 deploymentApi,
 type Agent, type SipPackage, type Deployment, type JobType, type AgentMetric, type AgentNetTuning, type ConfigScope } from '../api/deployment'
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
import { depEffectiveStatus, fmtRelTime } from './deploy/deployHelpers'
import ModuleConfigModal, { sectionForScope } from '../components/module/ModuleConfigModal'
import { GroupConfigCompareView } from '../components/group/GroupConfigCompareView'
import HealthCheckModal from '../components/HealthCheckModal'
import MetricTrend from '../components/MetricTrend'
import { agentDisplayName } from '../components/agentDisplay'
import { useAdminCapable } from '../hooks/useAdminCapable'
import { hasRole } from '../utils/permissions'
import AdminElevateDialog from '../components/AdminElevateDialog'
import { clearElevatedToken, elevationActive } from '../api/client'
import { useAuth } from '../contexts/AuthContext'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { NONE, fromSel, toSel } from '@core/components/custom/select-value'
import { usePrompt } from '@core/components/custom/prompt'

type Selection =
  | { kind: 'agent'; id: number }
  | { kind: 'group'; id: number }
  | null

// 상단 페이지 탭 — UX 개편 (2026-06-10): 좌측 선택(서버/그룹) 공유 + 우측 내용 분리.
// infra(시스템/서버 구성)·install(패키지 설치)·control(패키지 제어) = 조회 operator+, 변이 admin/승격.
// config(패키지 설정) = operator+ 편집 가능 (동적 반영 설정).
//  역할 분리: install=파일 배치(설치/재설치/롤백/삭제), control=프로세스(start/stop/restart),
// config=설정 — 한 탭에 섞여 있던 작업을 라이프사이클 단계별로 분리.
type PageTab = 'infra' | 'install' | 'config' | 'control'
// 서버 상태 → StatusDot 톤 / Badge 배리언트 (DESIGN-RULES §2 고정 매핑).
//   Success=online / Info=approved(등록됐으나 heartbeat 없음) / Neutral=pending·미설정
//   Danger=offline·error·revoked
function statusTone(st: string): StatusTone {
 if (st === 'online') return 'success'
 if (st === 'approved') return 'info'
 if (st === 'pending') return 'neutral'
 return 'danger'
}
function statusBadge(st: string) {
 if (st === 'online') return 'successSoft' as const
 if (st === 'approved') return 'infoSoft' as const
 if (st === 'pending') return 'neutralSoft' as const
 return 'dangerSoft' as const
}

// `counts` 는 탭 라벨 뒤 카운트 칩의 출처다 (Figma `Sec/Tabs (신규)` 458:9160 · G1 458:8626).
// 시안은 `패키지 설치 3 · 패키지 설정 9 · 패키지 제어 3` — 그 탭에서 다루는 **큰 항목의 수**다.
// 설치·제어는 모듈, 설정은 설정 필드. 둘 다 **스코프마다 따로 센다** — 서버를 고르면 그 서버
// 것이고 그룹을 고르면 그룹 것이다(같은 모듈이라도 서버 스코프 필드와 그룹 스코프 필드가 다르다).
const PAGE_TABS: Array<{ key: PageTab; label: string; adminGated: boolean
                         counts?: 'modules' | 'configModules' }> = [
  { key: 'infra', label: '시스템/서버 구성', adminGated: true },
  { key: 'install', label: '패키지 설치', adminGated: true, counts: 'modules' },
  { key: 'config', label: '패키지 설정', adminGated: false, counts: 'configModules' },
  { key: 'control', label: '패키지 제어', adminGated: true, counts: 'modules' },
]

/** 탭 카운트 칩 — 시안 실측 15×13 · `neutral-soft` 채움 · `neutral-on-soft` 글자 · 테두리 없음. */
function TabCount({ n }: { n: number }) {
 return (
    <span className="ml-1.5 inline-flex h-[13px] min-w-[15px] items-center justify-center rounded-full bg-neutral-soft px-1 text-xs text-neutral-on">
      {n}
    </span>
  )
}
// fieldset 잠금 래퍼 — 내부 input/button 일괄 disable (조회는 가능)
const LOCK_FIELDSET_STYLE: React.CSSProperties = {
 border: 0, margin: 0, padding: 0, minWidth: 0,
 flex: 1, display: 'flex', flexDirection: 'column', overflow: 'hidden',
}

export default function ServersPage() {
 const { show } = useToast()
  const prompt = usePrompt()
  // 네이티브 window.confirm 대신 시안 Dialog — 아래 호출부는 `await confirm({…})` 다.
 const confirm = useConfirm()
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
  // 탭 카운트용 모듈 수 — 서버는 자기 deployment 수, 그룹은 멤버 전체의 **모듈 종류** 수
  // (같은 모듈이 두 멤버에 깔려 있어도 1로 센다 — 그룹 화면이 모듈 단위로 보이므로).
 const moduleCount = useMemo(() => {
 if (selection?.kind === 'agent') return (depsByAgent.get(selection.id) || []).length
 const g = selection?.kind === 'group' ? haGroups.find(x => x.id === selection.id) : null
 if (!g) return 0
 const names = new Set<string>()
 for (const m of g.members)
 for (const d of depsByAgent.get(m.agent_id) || []) {
 if (d.package_name) names.add(d.package_name)
      }
 return names.size
  }, [selection, haGroups, depsByAgent])

  // [패키지 설정] 탭 카운트 — 그 탭에서 다루는 **큰 항목 = 설정할 모듈 수**.
  // 스코프마다 따로 센다: 서버는 그 서버에 깔린 모듈 중 **서버 스코프 설정이 있는 것**,
  // 그룹은 멤버 전체의 모듈 종류 중 **그룹 공통 설정이 있는 것**(같은 모듈이 두 멤버에 있어도 1).
  //
  // 도안의 `패키지 설정 9` 는 정의가 아니라 **예시 숫자**다 — S3(서버 개별 설정 4필드)와
  // G1·G3-1·G3-3(oam-svc 는 16필드)이 전부 똑같이 9 로 그려져 있어 화면 내용과 무관하다.
  // 모듈 칩의 `oam 9 · oam-svc 6 · csc 4` 도 실제 템플릿(4/9/13)과 다르다(§9).
  const configModuleCount = useMemo(() => {
    const pkgById = new Map(packages.map(p => [p.id, p]))
    const hasFields = (pkgId: number, scope: ConfigScope) => {
      const t = pkgById.get(pkgId)?.config_template
      if (!t) return false
      return t.sections.some(sec => sectionForScope(sec, scope) !== null)
    }
    if (selection?.kind === 'agent') {
      return (depsByAgent.get(selection.id) || [])
        .filter(d => d.status !== 'removed' && hasFields(d.package_id, 'system')).length
    }
    const g = selection?.kind === 'group' ? haGroups.find(x => x.id === selection.id) : null
    if (!g) return 0
    const seen = new Map<string, number>()
    for (const m2 of g.members)
      for (const d of depsByAgent.get(m2.agent_id) || []) {
        if (d.status !== 'removed' && d.package_name && !seen.has(d.package_name)) {
          seen.set(d.package_name, d.package_id)
        }
      }
    return [...seen.values()].filter(id => hasFields(id, 'service')).length
  }, [selection, haGroups, depsByAgent, packages])

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

  // 트리 검색 (시안 TreePanel) — 이름·호스트·IP 부분일치. 그룹은 이름이 맞거나 멤버가
  // 하나라도 맞으면 남는다. 빈 질의면 원본을 그대로 넘겨 불필요한 재생성을 피한다.
 const [treeQuery, setTreeQuery] = useState('')
 const q = treeQuery.trim().toLowerCase()
 const hitAgent = (a: Agent) =>
 [a.name, a.hostname, a.ip_address].some(v => (v ?? '').toLowerCase().includes(q))
 const shownGroups = useMemo(() => !q ? haGroups
    : haGroups.filter(g => g.name.toLowerCase().includes(q)
        || (groupedAgents.get(g.id) || []).some(hitAgent)), [haGroups, groupedAgents, q])
 const shownAgents = useMemo(() => {
 if (!q) return groupedAgents
 const m = new Map<number, Agent[]>()
 for (const [gid, list] of groupedAgents) {
 const g = haGroups.find(x => x.id === gid)
      // 그룹 이름이 맞으면 멤버를 전부 보여 준다 — 그래야 그룹을 찾은 의미가 있다
 const keep = g && g.name.toLowerCase().includes(q) ? list : list.filter(hitAgent)
 if (keep.length) m.set(gid, keep)
    }
 return m
  }, [groupedAgents, haGroups, q])
 const serverCount = agents.length

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
 if (!await confirm({ title: '세션 폐기', tone: 'danger', confirmLabel: '폐기',
 body: `${agentDisplayName(a.name)} 세션을 폐기할까요?` })) return
 try { await deploymentApi.revokeAgent(a.id); show(`${a.name} 폐기`, 'ok'); await load() }
 catch (e) { show((e as Error).message, 'err') }
  }
  // 서버 이름 변경 — **라벨만 바꾼다.** 배포·job·메트릭·알람 식별은 전부 agent_id 라
  //   개명에 딸린 보상 동작이 없다 (identifier_model.md). 노드 로컬의 state.json·systemd
  //   `--name` 은 설치 시점 값이라 옛 이름으로 남지만 인증·보고는 토큰과 id 로 하므로 무해.
 async function renameAgent(a: Agent) {
 const next = await prompt({ title: '서버 이름 변경', body: `서버 이름을 입력하세요 (#${a.id})`,
                             defaultValue: a.name || '' })
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
 if (!await confirm({ title: '서버 삭제', tone: 'danger', confirmLabel: '삭제',
 body: `Agent "${agentDisplayName(a.name)}" 을 삭제할까요? 관련 deployment 도 같이 제거됨` })) return
 try { await deploymentApi.deleteAgent(a.id); show('삭제됨', 'ok'); await load() }
 catch (e) { show((e as Error).message, 'err') }
  }
 async function upgradeAgent(a: Agent) {
 if (!await confirm({ title: 'agent 업그레이드', confirmLabel: '업그레이드', body: <>
      {agentDisplayName(a.name)} 의 agent 바이너리를 최신 버전으로 업그레이드할까요?
      <div className="mt-1">(agent 가 재기동됩니다)</div>
    </> })) return
 try {
 const r = await deploymentApi.upgradeAgent(a.id)
 show(`업그레이드 job 큐잉 (#${r.job_id})`, 'ok')
 await load()
    } catch (e) { show((e as Error).message, 'err') }
  }
 async function restartAgent(a: Agent) {
 if (!await confirm({ title: 'agent 재시작', confirmLabel: '재시작', body: <>
      {agentDisplayName(a.name)} 의 agent 프로세스를 재시작할까요?
      <div className="mt-1">(현재 binary 그대로 self-exec — 약 수 초 끊김)</div>
    </> })) return
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
 const pick = await prompt({ title: 'agent 롤백', defaultValue: others[0],
                             body: <>롤백할 agent 버전을 입력하세요 (현재 v{a.agent_version}).<br />설치됨: {others.join(', ')}</> })
 if (!pick) return
 target = pick.trim()
    } else if (!await confirm({ title: 'agent 롤백', confirmLabel: '롤백',
 body: `${agentDisplayName(a.name)} 의 agent 를 v${target} 로 롤백할까요? (현재 v${a.agent_version} — self-exec 재기동)` })) {
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
 if (!await confirm({ title: `모듈 ${jt}`, confirmLabel: '진행', body: <>
        {d.package_name} 모듈에 [{jt}] 진행할까요?
        <div className="mt-1">{desc}</div>
      </> })) return
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
 if (!await confirm({ title: '안전 가드 우회', tone: 'danger', confirmLabel: '강행', body: <>
          {(e as Error).message}
          <div className="mt-2">그래도 강행할까요? (안전 가드 우회)</div>
        </> })) {
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
 if (!await confirm({ title: '모듈 롤백', confirmLabel: '롤백', body: <>
      {d.package_name} 모듈을 이전 버전으로 롤백할까요?
      <div className="mt-2 font-mono text-xs">
        <div>현재: v{d.package_version} ({d.install_path})</div>
        <div>대상: v{targetVer || '?'} ({target})</div>
      </div>
      <div className="mt-2">collection 재동기 후 재기동됩니다 (단기 다운타임)</div>
    </> })) return
 try {
 const r = await deploymentApi.rollbackDeployment(d.id)
 show(`롤백 큐 등록 (restart #${r.restart_job_id} → ${r.install_path})`, 'ok')
 await load()
    } catch (e) { show((e as Error).message, 'err') }
  }
 async function removeDeployment(d: Deployment) {
 if (!await confirm({ title: '모듈 삭제', tone: 'danger', confirmLabel: '삭제',
 body: `Deployment #${d.id} (${d.package_name}) 을 제거할까요?` })) return
 try { await deploymentApi.deleteDeployment(d.id); show('삭제됨', 'ok'); await load() }
 catch (e) { show((e as Error).message, 'err') }
  }
 async function deleteSystem(g: HaGroup) {
 const memberNames = g.members.map(m => m.agent_name || `#${m.agent_id}`).join(', ')
 if (!await confirm({ title: '시스템 삭제', tone: 'danger', confirmLabel: '삭제', body: <>
      시스템 "{g.name}" 을 삭제합니다.
      <ul className="mt-2 list-disc pl-5">
        <li>HA 그룹 (mode={g.mode}, vrid={g.vrid}) 제거</li>
        <li>멤버 {g.members.length} 개 삭제: {memberNames || '(없음)'}</li>
      </ul>
      <div className="mt-2">계속할까요?</div>
    </> })) return
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
 if (!await confirm({ title: '멤버 제거', tone: 'danger', confirmLabel: '제거', body: <>
      멤버 [{agentDisplayName(a.name)}] 를 그룹 [{g.name}] 에서 제거할까요?
      <div className="mt-2">agent 자체는 삭제되지 않고 standalone (SA) 으로 트리에 남습니다.</div>
    </> })) return
 try {
 await haGroupsApi.removeMember(g.id, a.id)
 show(`${a.name} 그룹에서 제거됨`, 'ok')
 await load()
    } catch (e) { show((e as Error).message, 'err') }
  }

  // ContextBar 가 인스펙터 밖(탭 위)으로 나가면서 [재설치]→설치안내 펼침 연결이 끊긴다.
  // 값이 오를 때마다 인스펙터가 섹션을 펼치고 token 재발급을 건다.
 const [reinstallSignal, setReinstallSignal] = useState(0)

 if (loading) return <div className="flex min-h-0 flex-1 items-center justify-center p-8 text-center text-muted-foreground">로딩 중...</div>

 return (
    <div className="flex flex-col gap-3 flex-1 min-h-0">
      {/* 페이지 탭 — 좌측 선택(서버/그룹) 공유, 우측 내용 전환 */}
      {/* 좌측 트리 + 우측 Inspector */}
      <div className="flex-1 flex gap-3 overflow-hidden">
        {/* 좌측 트리 */}
        {/* 좌측 TreePanel — 정본 = Figma Sec/TreePanel (458:6714).
            폭 300 · 안쪽 여백 10 · 헤더 30 · 검색 34 · 트리 항목 32(간격 2) · 하단 버튼 36 */}
        <div className="flex w-[300px] shrink-0 flex-col overflow-hidden rounded-md border border-border bg-card">
          <div className="flex h-[30px] shrink-0 items-baseline gap-2 px-3.5 pt-3">
            <span className="text-base font-semibold">시스템</span>
            <span className="text-xs text-muted-foreground">
              시스템 {haGroups.length} · 서버 {serverCount}
            </span>
          </div>
          <div className="shrink-0 px-2.5 pt-1">
            {/* 높이 34 는 도안 실측값이다(TreePanel 458:6714) — 계약 기본 33 을 덮는다. */}
            <Input value={treeQuery} onChange={e => setTreeQuery(e.target.value)}
                   placeholder="서버 이름·IP 검색" aria-label="서버 이름·IP 검색"
                   className="h-[34px]" />
          </div>
          <div className="flex-1 overflow-auto px-2.5 pt-2.5">
            <ServerTree
 haGroups={shownGroups}
 groupedAgents={shownAgents}
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
            <div className="shrink-0 p-2.5">
              <Button className="w-full h-[36px]" variant="default" size="default"
 onClick={() => setSystemModalOpen(true)}
 disabled={!canEdit}
 title={canEdit ? 'AS 이중화 (서버 2 자동) / AA 다중화 / SA 단일 서버' : 'admin 권한 필요 (관리자 인증)'}>
                <Plus size={13} /> 시스템 추가
              </Button>
            </div>
          )}
        </div>
        {/* 우측 열 — 시안 RightColumn: ContextBar(60) → Tabs(31) → 본문.
            ContextBar 를 탭 **위**에 두어야 어느 탭에 있든 대상이 계속 보인다
            (decisions.md §3 — 웹은 1탭에서만 보였다). */}
        <div className="flex min-w-0 flex-1 flex-col gap-2 overflow-hidden">
          {selectedGroup && !selectedAgent && (
            <div className="shrink-0 overflow-hidden rounded-md border border-border bg-card">
              <GroupContextBar group={selectedGroup}
 memberCount={(groupedAgents.get(selectedGroup.id) || []).length}
 onOpenConfig={() => setPageTab('config')}
 onDeleteSystem={deleteSystem} />
            </div>
          )}
          {selectedAgent && (
            <div className="shrink-0 overflow-hidden rounded-md border border-border bg-card">
              <ServerContextBar a={selectedAgent}
 onApprove={approveAgent} onRevoke={revokeAgent} onRemove={removeAgent}
 onRename={renameAgent} onUpgrade={upgradeAgent} onRestart={restartAgent}
 onRollbackAgent={rollbackAgent} onMetrics={setMetricsFor}
 onHealthCheck={setHealthCheckFor}
 onClickReinstall={() => setReinstallSignal(v => v + 1)} />
            </div>
          )}
        {/* 4탭 — 정본 = Figma Sec/Tabs (458:8459). 높이 31 · 탭 사이 20 · 활성은 primary 밑줄.
            구 코드는 밑줄색이 `#1976d2` 하드코딩이었다(토큰과 다른 파랑). */}
        <div className="flex items-center gap-5 border-b-2 border-border" role="tablist">
          {PAGE_TABS.map(t => {
 const active = pageTab === t.key
 const locked = t.adminGated && !canEdit
 return (
              <button key={t.key} onClick={() => setPageTab(t.key)}
 role="tab" aria-selected={active}
 className={`-mb-0.5 flex h-[31px] items-center border-b-2 px-1 text-md transition-colors ${
 active ? 'border-primary font-semibold text-primary'
                               : 'border-transparent text-muted-foreground hover:text-foreground'}`}
 title={locked ? '조회 가능 — 변경은 admin 권한 필요 (관리자 인증)' : ''}>
                {t.label}
                {t.counts === 'modules' && <TabCount n={moduleCount} />}
                {t.counts === 'configModules' && <TabCount n={configModuleCount} />}
                {locked && <Lock size={11} className="ml-1.5" />}
              </button>
            )
          })}
          {/* 관리자 승격 — 이모지 두 개(🔓·🔐)를 Lucide 로 바꿨다 (글리프 아이콘 금지). */}
          <div className="ml-auto flex items-center gap-2 pb-1">
            {!hasRole(user, 'admin') && (
 canEdit && elevationActive() ? (
                <span className="flex items-center gap-1.5 text-sm text-success">
                  <LockOpen size={13} /> admin 승격 중
                  <Button variant="outline" onClick={() => clearElevatedToken()}>해제</Button>
                </span>
              ) : (
                <Button variant="outline" onClick={() => setElevateOpen(true)}
 title="admin 패스워드로 30분 승격 — 시스템 구성/패키지 설치 변경 허용">
                  <ShieldCheck /> 관리자 인증
                </Button>
              )
            )}
          </div>
        </div>
        {/* 본문 */}
        <div className="flex-1 overflow-hidden flex flex-col border border-border rounded-sm bg-card">
          {selectedAgent ? (
 pageTab === 'config' ? (
              <AgentConfigTab key={`${selectedAgent.id}:${pkgsReady}`}
 deployments={depsByAgent.get(selectedAgent.id) || []}
 packages={packages}
 onDone={load}
 onOpenGroupConfig={gid => {
 setSelection({ kind: 'group', id: gid })
 setPageTab('config')
                }} />
            ) : (
              // infra/install: 조회는 operator+, 변이는 admin/승격 — fieldset 일괄 잠금
              <fieldset disabled={!canEdit} style={LOCK_FIELDSET_STYLE}>
                <ServerInspector agent={selectedAgent} mode={pageTab} reinstallSignal={reinstallSignal}
 deployments={depsByAgent.get(selectedAgent.id) || []}
 packages={packages}
 vipIps={vipIps}
 mgmtVip={mgmtVip}
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
              <div className="flex flex-col h-full">
                <div className="pt-3 px-3 pb-0">
                  <ModuleSpecSection group={selectedGroup} deployments={deployments} onReload={load} />
                </div>
                <div className="flex-1 min-h-0">
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
                  />
              </fieldset>
            )
          ) : (
            <EmptyState title="왼쪽 트리에서 서버 또는 HA 그룹을 선택하세요" className="p-[40px]" />
          )}
        </div>
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
    <div className="text-md">
      {/* HA groups */}
      {haGroups.map(g => {
 const members = groupedAgents.get(g.id) || []
 const isOpen = expanded.has(g.id)
 const isSelected = selection?.kind === 'group' && selection.id === g.id
 const modeChip = g.mode === 'active_standby' ? 'AS' : 'AA'
 const canAddMember = g.mode === 'all_active'  // AS 는 master/backup 2 fixed
 return (
          <div key={g.id}>
            <div onClick={() => onSelect({ kind: 'group', id: g.id })}
 className={`flex cursor-pointer items-center gap-1.5 border-b border-border px-2.5 py-2 ${
 isSelected ? 'bg-brandsoft' : 'bg-muted'}`}>
              <span className="w-[14px] text-muted-foreground" onClick={e => { e.stopPropagation(); onToggleExpand(g.id) }}>{isOpen ? <ChevronDown size={12} /> : <ChevronRight size={12} />}</span>
              <Badge variant={g.mode === 'active_standby' ? 'infoSolid' : 'successSolid'}>{modeChip}</Badge>
              <b className="flex-1">{g.name}</b>
              {g.vip && (
                <span className="text-xs text-muted-foreground" title={`VIP ${g.vip}/${g.vip_mask}`}>
                  VIP {g.vip}
                </span>
              )}
              <span className="text-xs text-muted-foreground">{members.length}</span>
              {canAddMember && (
                <Button variant="outline" size="iconSm" onClick={e => { e.stopPropagation(); onAddMember(g) }}
 title="새 멤버 자동 생성 (이름 자동, install_command 발급)"><Plus /></Button>
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
 return (
          <div key={`sa-${a.id}`}
 onClick={() => onSelect({ kind: 'agent', id: a.id })}
 className={`flex cursor-pointer items-center gap-1.5 border-b border-border px-2.5 py-2 ${
 isSelected ? 'bg-brandsoft' : 'bg-muted'}`}>
            <span className="w-[14px]"/>  {/* expand 자리 비움 — group 정렬 맞춤 */}
            <Badge variant="neutralSolid">SA</Badge>
            <b className="flex-1">{agentDisplayName(a.name)}</b>
            <span className="text-xs text-muted-foreground">{a.ip_address || '—'}</span>
            <StatusDot status={a.status} label="" className="ml-1" />
            <span className="text-xs text-muted-foreground">
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
 return (
    <div onClick={onClick}
 className={`flex cursor-pointer items-center gap-1.5 border-b border-border py-1.5 pr-2.5 ${
 indent ? 'pl-8' : 'pl-2.5'} ${active ? 'bg-brandsoft' : ''}`}>
      <StatusDot status={a.status} label="" />
      <span className={`flex-1 ${active ? 'font-semibold' : ''}`}>{agentDisplayName(a.name)}</span>
      {role && (
        <Badge variant={role === 'master' ? 'infoSolid' : 'neutralSolid'}
 title={role === 'master' ? 'Master — priority 100 (절체 우선순위)' : 'Backup — priority 90'}>
          {role === 'master' ? 'M' : 'B'}
        </Badge>
      )}
      <span className="text-xs text-muted-foreground">{depCount}m</span>
      {onRemove && (
        // 트리 행의 [멤버 제거] 는 도안(TreePanel 458:6956)이 붉게 그렸다 — §7-14 대로 그림을 따른다
        <Button variant="outline" size="iconSm"
 className="border-destructive text-destructive"
 onClick={e => { e.stopPropagation(); onRemove() }}
 title="그룹에서 멤버 제거 (agent 자체는 standalone 으로 유지)"><X /></Button>
      )}
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  Group Inspector (HA 그룹 선택 시)
// ──────────────────────────────────────────────────────────────

function GroupInspector({ group, agents, onSelectMember, onReload }: {
 group: HaGroup
 agents: Agent[]
 onSelectMember: (aid: number) => void
 onReload: () => Promise<void>
}) {
 const { show } = useToast()
 const confirm = useConfirm()
  // AA(all_active)는 절체 개념이 없어 auth_pass·절체 조건·역할/MASTER/상태 컬럼이 전부 빠진다
  // (`cims-design-handoff/screens/aa-group.md` 대조표).
 const isAS = group.mode === 'active_standby'
 const [editName, setEditName]         = useState(group.name)
  // mode 는 readonly — 생성 후 변경 불가 (변경 원하면 시스템 삭제 후 재생성).
 const [editAuthPass, setEditAuthPass] = useState(group.auth_pass)
 const [editNote, setEditNote]         = useState(group.note || '')
  // 백엔드 vip_bindings 에는 bid 가 없음 → 안정적 bid 부여(누락 시 removeBinding 이 전체 삭제됨).
 const [editBindings, setEditBindings] = useState<VipBinding[]>((group.vip_bindings || []).map((b, i) => ({ ...b, bid: b.bid ?? i + 1 })))
 const [editFailover, setEditFailover] = useState<FailoverOptions>(
    { ...FAILOVER_DEFAULTS, ...(group.failover_options || {}),
 health: { ...FAILOVER_DEFAULTS.health, ...(group.failover_options?.health || {}) } })
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

  // dirty 검출 — **필드 단위**. 하단 통합 저장(StickySaveBar)이 바뀐 필드만 payload 에 담는다.
  // backend `_update_group` 은 `if k in body` 부분 갱신이라 **안 담긴 필드는 손대지 않는다** —
  // 그래서 통합 저장이어도 사정거리가 넓어지지 않는다(정본 문서 §7-10).
 const nameDirty     = editName !== group.name
 const authDirty     = editAuthPass !== group.auth_pass
 const noteDirty     = editNote !== (group.note || '')
 const failoverDirty = JSON.stringify(editFailover) !== JSON.stringify(group.failover_options || FAILOVER_DEFAULTS)
 const vipDirty      = JSON.stringify(editBindings) !== JSON.stringify(group.vip_bindings || [])
 const masterChanged = editMasterAid !== initialMaster

  // 화면이 보여 주는 값의 기준 노드 — 도안·글스펙 모두 「ACTIVE 노드」로 적는다
  // (as-group.md G1 Info Alert). 실제로 VIP 를 들고 있는 노드가 관측되면 그 노드를 말하고,
  // 관측이 없으면 아는 것만 말한다 — 관측 없이 ACTIVE 라고 적으면 거짓이 된다.
  const activeMemberLabel = (() => {
    const obs = memberAgents.find(m => m.vip_observed === true)
    if (obs?.agent) return `ACTIVE 노드 ${agentDisplayName(obs.agent.name)}`
    const top = [...memberAgents].sort((x, y) => (y.priority ?? 0) - (x.priority ?? 0))[0]
    const nm = top?.agent ? agentDisplayName(top.agent.name) : null
    return nm ? `지정 MASTER 노드 ${nm}` : '지정 MASTER 노드'
  })()

  // 저장 전에 **무엇이 바뀌는지** 보여준다 — 배지 숫자와 tooltip 이 같은 목록을 쓴다.
 const changes = [
 nameDirty     && '그룹 이름',
 authDirty     && 'auth_pass',
 noteDirty     && 'note',
 failoverDirty && '절체 조건',
 masterChanged && 'MASTER 지정',
 vipDirty      && 'VIP Bindings',
  ].filter(Boolean) as string[]

  // 영역별 적용 — 그 영역의 변경만 backend 로 push. backend 의 _update_group 은 부분 업데이트
  // 지원 + _enqueue_update_ha_for_members 자동 호출 → 멤버 agent 의 keepalived 즉시 반영.
  /** 통합 저장 — **바뀐 필드만** 한 번에. keepalived 재렌더도 1회로 준다(구: 영역마다 1회). */
 async function saveAll() {
 if (changes.length === 0) return
 const body: Record<string, unknown> = {}
 if (nameDirty) body.name = editName
 if (authDirty) body.auth_pass = group.mode === 'active_standby' ? editAuthPass : ''
 if (noteDirty) body.note = editNote
 if (failoverDirty) body.failover_options = editFailover
 if (vipDirty) body.vip_bindings = editBindings
 try {
 if (Object.keys(body).length) await haGroupsApi.update(group.id, body)
      // MASTER 는 멤버 priority 라 별도 API — 그룹 update 와 한 트랜잭션이 아니다.
 if (masterChanged && editMasterAid !== null) await applyMembers({ silent: true })
 show(`저장됨 — ${changes.join(' · ')} (전 멤버 적용)`, 'ok')
 await onReload()
    } catch (e) { show((e as Error).message, 'err') }
  }
 function revertAll() {
 setEditName(group.name)
 setEditAuthPass(group.auth_pass || '')
 setEditNote(group.note || '')
 setEditFailover(group.failover_options || FAILOVER_DEFAULTS)
 setEditBindings(group.vip_bindings || [])
 setEditMasterAid(initialMaster)
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
 async function applyMembers({ silent = false }: { silent?: boolean } = {}) {
    // Master 선택 변경 → 해당 멤버 priority=100, 나머지=90. AS 에만 의미.
 if (!masterChanged || editMasterAid === null) return
 for (const m of group.members) {
 const newPrio = m.agent_id === editMasterAid ? 100 : 90
 if (newPrio !== m.priority) {
 await haGroupsApi.addMember(group.id, { agent_id: m.agent_id, priority: newPrio })
      }
    }
 if (!silent) { show('멤버 적용됨', 'ok'); await onReload() }
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
 async function removeBinding(bid: number) {
 const b = editBindings.find(x => x.bid === bid)
 if (!b) return
 const desc = `${b.slot || '(slot 미지정)'} — ${b.ip || '(IP 미입력)'}${b.mask ? `/${b.mask}` : ''}`
 if (!await confirm({ title: 'VIP 삭제', tone: 'danger', confirmLabel: '삭제', body: <>
      VIP binding 을 제거할까요?
      <div className="mt-1 font-mono text-xs">{desc}</div>
      <div className="mt-2">저장하기 전 까지는 적용되지 않습니다.</div>
    </> })) return
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
 ip: info.prefix ? info.prefix + oldHost : '',
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
      {/* 정체성(AS 배지·이름·#id·vrid)과 액션은 탭 위 GroupContextBar 로 올라갔다.
          여기는 저장 흐름에 묶인 편집 필드만 — 저장은 하단 StickySaveBar. */}
      <div className="flex-1 overflow-auto p-4">
        {/* 이 화면의 변경 범위를 먼저 알린다 (Figma G1 42:428 상단 SectionMessage).
            표시값 기준 노드도 함께 — 멤버마다 값이 다를 수 있는데 화면은 하나다. */}
        <Alert variant="info" className="mb-4">
          <div className="font-semibold">
            이 화면의 변경은 그룹 멤버 전체({memberAgents.map(m =>
 m.agent ? agentDisplayName(m.agent.name) : `#${m.agent_id}`).join(', ')})에 적용됩니다
          </div>
          {/* 둘째 줄은 스코프마다 다르다 — G1(42:815)은 표시값 기준 노드를,
              A1(188:3049)은 절체가 없다는 사실을 알린다. AA 에는 MASTER 가 없으므로
              같은 문장을 쓰면 사실과 다르다. */}
          <div className="mt-0.5 text-xs opacity-90">
            {isAS
              ? `표시값 기준은 ${activeMemberLabel} 입니다. 개별 서버만 바꾸려면 좌측 트리에서 해당 서버를 선택하세요.`
              : 'all_active 그룹은 절체 개념이 없어 절체 조건·공유 store 섹션이 없습니다.'}
          </div>
        </Alert>
        {/* 그룹 설정 — 세로 라벨 3필드 + 필드별 도움말 (Figma G1 42:428).
            구 화면은 `이름:[ ] auth_pass:[ ] note:[ ]` 한 줄이라 무엇이 필수인지도,
 auth_pass 가 무엇인지도 알 수 없었다. */}
        <SubSection level={1} title="그룹 설정"
 hint={isAS ? 'VRRP 인증·메모 · 저장 시 전 멤버 반영'
                               : '메모 · 저장 시 전 멤버 반영 (AA 는 VRRP 인증 없음)'}>
          <div className="grid grid-cols-1 gap-4 lg:grid-cols-3">
            <FormField label="그룹 이름" required help="트리와 대시보드에 표시되는 이름">
              <Input value={editName}
 onChange={e => setEditName(e.target.value)} />
            </FormField>
            {isAS && (
              <FormField label="auth_pass" required
 help="VRRP 인증 비밀번호 — 멤버 간 동일해야 합니다 (최대 8글자)">
                <Input type="password" maxLength={8}
 value={editAuthPass} onChange={e => setEditAuthPass(e.target.value)} />
              </FormField>
            )}
            <FormField label="note" help="운영 메모 (선택)">
              <Input value={editNote}
 onChange={e => setEditNote(e.target.value)} />
            </FormField>
          </div>
        </SubSection>

        {/* 절체 조건 — 그룹 단위 설정. AS 만 (AA 는 절체 개념이 없다). */}
        {isAS && (
          <div className="mb-5">
            <FailoverSection value={editFailover} onChange={setEditFailover} dirty={failoverDirty} />
          </div>
        )}

        {/* 멤버 — 추가/삭제는 좌측 트리에서 일괄 처리 (트리의 [+] / [×]).
            여기는 표시 + AS 의 Master 선택만 담당.
            AA 는 절체가 없어 역할·MASTER·상태 3컬럼과 [실측] 이 빠진다 (aa-group.md 대조표). */}
        <SubSection level={1} title="멤버" count={memberAgents.length}
 hint={isAS
                      ? '추가·삭제는 좌측 트리에서 · MASTER 는 하나만 지정'
                      : '추가·삭제는 좌측 트리에서'}>
          {/* 컬럼 폭은 Figma G1 멤버 표(43:624) 실측 그대로 — 헤더가 세로로 쪼개지지 않는
              최소폭이다. Agent 는 우측 정렬(TableHeaderCell Align Right). */}
          <DataTable>
            <thead>
              <tr>
                <Th width={isAS ? 150 : 220}>이름</Th>
                {isAS && (
                  <>
                    <Th width={64} title="설정상 역할 (Master/Backup) — Master 선택의 결과(priority).">역할</Th>
                    <Th width={84} title="Master 선택 — 절체 시 우선순위가 가장 높은 노드. 1명만 선택 가능.">MASTER</Th>
                    <Th width={116} title="현재 실제 상태 (Active/Standby). VIP 를 실제로 보유 중인지. 절체 직후엔 설정과 다를 수 있음.">상태</Th>
                  </>
                )}
                <Th width={isAS ? 88 : 160}>접속</Th>
                <Th width={isAS ? 168 : 260}>IP</Th>
                <Th width={isAS ? 152 : 182} align="right">Agent</Th>
              </tr>
            </thead>
            <tbody>
              {memberAgents.map(m => {
 const a = m.agent
 const colCount = isAS ? 7 : 4
 if (!a) return (
                  <tr key={m.agent_id}><Td colSpan={colCount}>(agent #{m.agent_id} not found)</Td></tr>
                )
 const isMasterSel = editMasterAid === a.id
 return (
                  <tr key={a.id}>
                    <Td onClick={() => onSelectMember(a.id)} className="cursor-pointer">
                      {agentDisplayName(a.name)}
                    </Td>
                    {isAS && (
                      <>
                        <Td>
                          <Badge variant={isMasterSel ? 'brandSoft' : 'neutralSoft'}
 title={isMasterSel ? 'Master — 절체 우선순위 100' : 'Backup — 절체 우선순위 90'}>
                            {isMasterSel ? 'M' : 'B'}
                          </Badge>
                        </Td>
                        <Td>
                          <Radio name={`master-${group.id}`} checked={isMasterSel}
 onChange={() => setEditMasterAid(a.id)}
 title="이 멤버를 Master 로 설정 (priority 100, 나머지 90)" />
                        </Td>
                        <Td>
                          {/* A/S = 실제 VIP 보유. 기본은 heartbeat 관측(≤30s 지연, R4) —
 [실측 새로고침] 은 sync health-check 로 즉시 재확인 (관측 override). */}
                          {(() => {
 const o = vipObs[a.id]
 if (o === 'active') return (
                              <StatusDot tone="success" label="Active" title="VIP 실제 보유 — Active (실측)" />)
 if (o === 'standby') return (
                              <StatusDot tone="neutral" label="Standby" title="VIP 미보유 — Standby (실측)" />)
 if (o === 'fail') return (
                              <StatusDot tone="danger" label="점검 실패" title="offline 또는 health-check 오류" />)
 if (vipChecking) return (
                              <span className="text-sm text-muted-foreground">점검 중…</span>)
 const hb = group.members.find(gm => gm.agent_id === a.id)?.vip_observed
 if (hb === true) return (
                              <StatusDot tone="success" label="Active"
 title="VIP 실제 보유 — Active (heartbeat 관측, ≤30s 지연)" />)
 if (hb === false) return (
                              <StatusDot tone="neutral" label="Standby"
 title="VIP 미보유 — Standby (heartbeat 관측, ≤30s 지연)" />)
 return (
                              <span className="text-muted-foreground"
 title="판정 불가 (heartbeat stale·VIP 미설정) — [실측 새로고침] 으로 확인">—</span>)
                          })()}
                        </Td>
                      </>
                    )}
                    {/* 접속은 **살아있는 상태**라 Badge 가 아니라 StatusDot 이다 (DESIGN-RULES §2).
                        G1 그림(43:674)은 Badge 로 그렸지만 A1 그림(189:3238)은 StatusDot 이라
                        두 그림이 갈린다 — 글로 된 규칙이 명시적이라 그쪽을 따랐다. */}
                    <Td><StatusDot status={a.status} /></Td>
                    <Td mono>{orDash(a.ip_address)}</Td>
                    <Td mono align="right" className="text-muted-foreground">
                      {orDash(a.agent_version)}
                    </Td>
                  </tr>
                )
              })}
            </tbody>
          </DataTable>
          {/* 시안은 [실측 새로고침] 을 표 아래 Secondary sm 으로 둔다 (43:705).
              구 화면은 섹션 헤더 우측의 `🔄 실측` 이었다 — 아이콘 글리프도 함께 걷었다. */}
          {isAS && (
            <Button className="mt-2" onClick={checkVipHolders} disabled={vipChecking}
 title="멤버별 health-check 로 실제 VIP 보유(Active) 상태를 관측 (sync REST — 수 초 소요)">
              {vipChecking ? '점검 중…' : '실측 새로고침'}
            </Button>
          )}
        </SubSection>

        {/* VIP Bindings — 정본 Figma G1 44:645. 컬럼 폭 실측:
            용도 100 · VIP 190 · MASK 64 · 멤버 IFACE 180 · 보유 140 · 액션 148.
            액션 줄(수동 입력·+ VIP 추가·재적용)은 시안대로 **표 아래**로 내렸다 (191:3198) —
            구 화면은 섹션 헤더 우측이었다. 0건이면 ES-1 (empty-states.md). */}
        <SubSection level={1} title="VIP Bindings" count={editBindings.length}
 hint="네트워크·마스크는 멤버의 service IP 에서 자동 매핑 — host 옥텟만 입력">
          {editBindings.length === 0 ? (
            <>
              <EmptyState
 title="VIP 없음"
 description="all_active 그룹은 비워둬도 됩니다 (keepalived 안 깔림). active_standby 는 1개 이상 권장." />
              {availableSlots.length === 0 && (
                // 조치 지점이 **다른 화면**이라 해당 멤버로 가는 바로가기를 함께 둔다
                // (empty-states.md ES-1 의 주석). 선택만 바꾸면 그 서버의 네트워크 섹션이 펼쳐진다.
                <Alert variant="warning" className="mt-2">
                  <div className="font-medium">멤버 서버에 용도(service IP) 가 없습니다</div>
                  <div className="mt-0.5 text-xs">
                    멤버의 [네트워크] 탭에서 IP 별 용도를 입력해야 VIP 를 자동 매핑할 수 있습니다.
                  </div>
                  <div className="mt-1.5 flex flex-wrap gap-1.5">
                    {memberAgents.map(m => (
                      <Button key={m.agent_id} variant="outline"
 onClick={() => onSelectMember(m.agent_id)}>
                        {m.agent ? agentDisplayName(m.agent.name) : `#${m.agent_id}`} 네트워크로
                        <ArrowRight />
                      </Button>
                    ))}
                  </div>
                </Alert>
              )}
            </>
          ) : (
            <DataTable>
              <thead>
                <tr>
                  <Th width={100}>용도</Th>
                  <Th width={190}>VIP (네트워크 + HOST)</Th>
                  <Th width={64}>MASK</Th>
                  <Th width={180}>멤버 IFACE</Th>
                  <Th width={140}
 title="이 VIP 를 실제로 들고 있는 멤버 (heartbeat 관측, ≤30s 지연)">보유</Th>
                  <Th width={148}>액션</Th>
                </tr>
              </thead>
              <tbody>
                {editBindings.map(b => {
 const isEditing = bindingEditMode === b.bid
 const info = b.slot ? slotSubnetInfo(b.slot) : null
 const host = splitPrefixHost(b.ip, b.mask ?? 24)?.host ?? ''
 const ifaceStr = Object.entries(b.memberIfaces || {})
                    .map(([sid, iface]) => `#${sid}:${iface}`).join(', ')
 if (!isEditing) {
 return (
                      <tr key={b.bid}>
                        <Td>
                          {b.slot
                            ? <Badge variant="brandSoft">{b.slot}</Badge>
                            : <span className="text-muted-foreground">(미지정)</span>}
                        </Td>
                        <Td mono>{orDash(b.ip)}</Td>
                        <Td>{b.mask || 24}</Td>
                        <Td mono className="text-muted-foreground">{orDash(ifaceStr)}</Td>
                        <Td><VipHolderCell holders={vipHolders(b.ip)} /></Td>
                        <Td>
                          <div className="flex items-center gap-1.5">
                            <Button variant="ghost" disabled={bindingEditMode !== null}
 onClick={() => setBindingEditMode(b.bid)}>
                              <Pencil /> 수정
                            </Button>
                            {/* 행 단위 삭제인데 Danger 다 — 시안이 그렇게 그렸다 (44:696).
 contracts.md 는 행 단위를 Secondary 로 적었지만 그림이 정본이다. */}
                            <Button variant="destructive" disabled={bindingEditMode !== null}
 onClick={() => void removeBinding(b.bid)}>
                              <Trash2 /> 삭제
                            </Button>
                          </div>
                        </Td>
                      </tr>
                    )
                  }
                  // edit mode — 수동 입력이면 IP/mask/멤버 iface 를 직접 지정, 아니면 용도 기반 자동 매핑.
 const manualOk = !!b.ip.trim()
 return (
                    <tr key={b.bid} className="bg-warning-soft">
                      <Td>
                        <Select value={toSel(b.slot)} onValueChange={(v: string) => changeBindingSlot(b.bid, fromSel(v))}>
                          <SelectTrigger className="w-[110px] text-xs p-0.5"><SelectValue /></SelectTrigger>
                          <SelectContent>
                            <SelectItem value={NONE}>{vipManual ? '(용도 없음)' : '(용도 선택)'}</SelectItem>
                            {availableSlots.map(s => <SelectItem key={s} value={s}>{s}</SelectItem>)}
                          </SelectContent>
                        </Select>
                      </Td>
                      <Td>
                        {vipManual ? (
                          <Input className="font-mono w-[130px] text-xs p-0.5" value={b.ip}
 onChange={e => updateBinding(b.bid, { ip: e.target.value })}
 placeholder="121.161.164.140"/>
                        ) : info?.prefix ? (
                          <span className="inline-flex items-center gap-0.5">
                            <span className="font-mono text-muted-foreground">{info.prefix}</span>
                            <Input className="font-mono w-[60px] text-xs p-0.5" value={host}
 onChange={e => changeBindingHost(b.bid, e.target.value)}
 placeholder="host"/>
                          </span>
                        ) : (
                          <span className="text-xs text-warning">
                            {b.slot ? (info?.conflictDetail || '용도의 멤버 IP 가 같은 네트워크 아님')
                                    : '(용도 선택 필요 — 또는 [수동 입력])'}
                          </span>
                        )}
                      </Td>
                      <Td mono>
                        {vipManual ? (
                          <Input className="w-[55px] text-xs p-0.5" type="number" min={8} max={32}
 value={b.mask || 24}
 onChange={e => updateBinding(b.bid, { mask: Number(e.target.value) || 24 })}/>
                        ) : (b.mask || 24)}
                      </Td>
                      <Td className="text-muted-foreground">
                        {vipManual ? (
                          <div className="flex flex-col gap-0.5">
                            {memberAgents.map(m => (
                              <span key={m.agent_id} className="flex items-center gap-1">
                                <span className="w-[54px] overflow-hidden text-ellipsis">
                                  {m.agent ? agentDisplayName(m.agent.name) : `#${m.agent_id}`}
                                </span>
                                <Input className="font-mono w-[60px] text-xs p-0.5"
 value={b.memberIfaces?.[m.agent_id] || ''}
 onChange={e => updateBinding(b.bid, {
 memberIfaces: { ...(b.memberIfaces || {}),
 [m.agent_id]: e.target.value },
                                       })}
 placeholder="ens3"/>
                              </span>
                            ))}
                          </div>
                        ) : <span className="font-mono">{orDash(ifaceStr)}</span>}
                      </Td>
                      <Td><VipHolderCell holders={[]} editing /></Td>
                      <Td>
                        <div className="flex items-center gap-1.5">
                          <Button variant="default"
 disabled={vipManual ? !manualOk : (!b.slot || !host || !info?.prefix)}
 onClick={() => setBindingEditMode(null)}>저장</Button>
                          <Button variant="destructive" onClick={() => void removeBinding(b.bid)}>
                            <Trash2 /> 삭제
                          </Button>
                        </div>
                      </Td>
                    </tr>
                  )
                })}
              </tbody>
            </DataTable>
          )}

          {/* 액션 줄 — 시안 191:3198: Ghost `수동 입력` · Secondary `+ VIP 추가` · Ghost `재적용`.
              `수동 입력` 은 토글이라 켜진 상태를 눈에 보이게 해야 한다 — 시안에 on 상태 그림이
              없어 Secondary + 브랜드 글자로 켜짐을 표시하고 `aria-pressed` 로도 알린다. */}
          <div className="mt-2 flex items-center gap-2">
            <Button variant={vipManual ? 'outline' : 'ghost'} aria-pressed={vipManual}
 className={vipManual ? 'text-primary' : undefined}
 onClick={() => setVipManual(!vipManual)}
 title="용도 자동 매핑 대신 IP·멤버 iface 직접 입력 (다른 서브넷·전용 NIC 구성)">
              수동 입력
            </Button>
            <Button variant="outline" onClick={beginAddBinding}
 disabled={(!vipManual && availableSlots.length === 0) || bindingEditMode !== null}>
              + VIP 추가
            </Button>
            {/* 값 변경 없이 keepalived 만 다시 렌더 — 노드가 재설치·복구된 뒤 VIP 설정을
                따라잡게 하는 통로. update 는 값이 바뀌어야 job 이 나간다. */}
            <Button variant="ghost" onClick={reapplyVip}
 disabled={vipDirty || bindingEditMode !== null}>
              재적용
            </Button>
          </div>
          {/* 비활성 사유는 툴팁이 아니라 눈에 보이게 (contracts.md §Button) */}
          {bindingEditMode !== null ? (
            <div className="mt-1 text-xs text-muted-foreground">
              편집 중인 행을 [저장] 하면 다른 액션이 다시 열립니다.
            </div>
          ) : !vipManual && availableSlots.length === 0 ? (
            <div className="mt-1 text-xs text-muted-foreground">
 [+ VIP 추가] 는 멤버 서버의 [네트워크] 탭에서 IP 용도를 입력한 뒤 열립니다 — 또는 [수동 입력].
            </div>
          ) : vipDirty ? (
            <div className="mt-1 text-xs text-muted-foreground">
 [재적용] 은 변경을 먼저 저장해야 열립니다.
            </div>
          ) : null}
          {/* 시안 hint (44:723). 「상세 편집」은 이 빌드에서 [수동 입력] 이라 이름만 바꿨다. */}
          <div className="mt-2 text-xs text-muted-foreground">
            수동 IP 입력이 필요하면 [수동 입력] 을 쓰세요. 멤버 IFACE 는 각 서버의 용도 슬롯에서 자동 결정됩니다.
          </div>
        </SubSection>

        {/* 그룹 공통 마운트 — 멤버 전체에 같은 경로. 모듈 로그 수집처(NAS) 등.
            섹션 머리·접힘은 패널이 직접 그린다 (다른 섹션과 같은 SubSection). */}
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

        {/* 공유 store 는 이 탭에 없다 — oam/oam-svc 의 [패키지 설정] > 관리 store 로 귀속.
            HA 편입 여부는 그 값에서 유도되고, 미충족 사유는 [패키지 제어] 탭 배너가 알린다. */}
      </div>
      {/* StickySaveBar — 하단 통합 저장 (Figma G1 42:428).
          구 화면은 그룹 설정·절체 조건·멤버·VIP 마다 [적용] 이 따로 있어 keepalived 가
          영역 수만큼 재렌더됐다. 이제 **바뀐 필드만** 한 번에 보낸다(정본 문서 §7-10). */}
      <StickySaveBar
 badge={changes.length > 0
          ? <Badge variant="warningSoft" title={changes.join(' · ')}>변경 {changes.length}건 · 전 멤버 적용</Badge>
          : <Badge variant="neutralSoft">변경 0건</Badge>}
 note={changes.length > 0
          ? changes.join(' · ')
          : '그룹 설정 · 절체 조건 · 멤버 · VIP 를 한 번에 저장 — keepalived 재생성'}
 saveLabel="저장 — 전 멤버 적용"
 disabled={changes.length === 0}
 onRevert={revertAll}
 onSave={saveAll} />
    </>
  )
}

// VIP 보유 멤버 셀 — heartbeat 관측(≤30s 지연). 정확히 1명이 정상, 0명은 이동 중/미적용,
// 2명 이상은 split-brain 의심이라 색으로 구분한다.
/** 보유 셀 — 시안은 StatusDot + 노드명이다 (Figma G1 191:3195). */
function VipHolderCell({ holders, editing }: { holders: string[]; editing?: boolean }) {
 if (editing) return <span className="text-muted-foreground">—</span>
 if (holders.length === 1) {
 return <StatusDot tone="success" label={holders[0]}
 title="이 VIP 를 실제로 보유 (heartbeat 관측)" />
  }
 if (holders.length === 0) {
    // 선언은 됐는데 아무도 안 들고 있다 = 미적용 → Warning (DESIGN-RULES §2 톤 매핑)
 return <StatusDot tone="warning" label="미할당"
 title="어느 멤버도 이 VIP 를 갖고 있지 않음 — 미적용이거나 이동 중" />
  }
 return <StatusDot tone="danger" label={`${holders.length}곳 보유`}
 title={`동시 보유: ${holders.join(', ')} — split-brain 의심`} />
}

// AS 절체 조건 (그룹/시스템 스코프) — keepalived advert_int / vrrp_script health /
// preempt / track_interface / restart_limit. 모듈별 값(프로세스 감시·절체 모드)은
// 패키지 설정의 모듈 운영 명세(ModuleSpecSection)로 이관됨.
function FailoverSection({ value, onChange, dirty }: {
 value: FailoverOptions
 onChange: (v: FailoverOptions) => void
  /** 저장은 하단 StickySaveBar 가 한다 — 여기서는 변경 표시만 */
 dirty: boolean
}) {
 const set = <K extends keyof FailoverOptions>(k: K, v: FailoverOptions[K]) =>
 onChange({ ...value, [k]: v })
 const setHealth = (k: keyof FailoverOptions['health'], v: number) =>
 onChange({ ...value, health: { ...value.health, [k]: v } })
 const rl = value.restart_limit || { max_fails: 3, window_sec: 300 }
 const setRestart = (k: 'max_fails' | 'window_sec', v: number) =>
 set('restart_limit', { ...rl, [k]: v })
 return (
    <SubSection level={1} title="절체 조건 (A/S 전용)"
                hint={`감시주기 ${value.advert_int}s · 장애판정 ${value.health.fall}회 · `
                      + `자동 복귀 ${value.preempt === 'preempt' ? '있음' : '없음'}`}
                right={dirty ? <Badge variant="warningSoft">변경됨</Badge> : undefined}>
      {/* 9필드 3열 그리드 (Figma G1 42:428). 구 화면은 한 줄에 여러 필드를 이어 붙여
           `연속 [3] 회 실패 (윈도우 [300] 초) → 절체` 처럼 문장 속에 입력이 박혀 있었다 —
           라벨과 값의 대응이 흐리고 도움말 자리도 없다. */}
      <div>
          <div className="grid grid-cols-1 gap-x-6 gap-y-4 lg:grid-cols-3">
            <FormField label="감시 주기 (초)" help="기본 1초 · 범위 0.5~5초">
              <Input type="number" min={0.5} max={5} step={0.5}
 value={value.advert_int}
 onChange={e => set('advert_int', Number(e.target.value) || 1)} />
            </FormField>
            <FormField label="점검 주기 (초)" help="health check 실행 간격">
              <Input type="number" min={1} max={60}
 value={value.health.interval}
 onChange={e => setHealth('interval', Number(e.target.value) || 2)} />
            </FormField>
            <FormField label="제한 시간 (초)" help="health check 응답 대기 한도">
              <Input type="number" min={1} max={60}
 value={value.health.timeout}
 onChange={e => setHealth('timeout', Number(e.target.value) || 3)} />
            </FormField>

            <FormField label="장애 판정 (회)" help="연속 실패 N회 → 절체. 절체까지 ≈ 점검주기 × 이 값">
              <Input type="number" min={1} max={60}
 value={value.health.fall}
 onChange={e => setHealth('fall', Number(e.target.value) || 2)} />
            </FormField>
            <FormField label="복귀 판정 (회)" help="연속 성공 N회 → 정상">
              <Input type="number" min={1} max={60}
 value={value.health.rise}
 onChange={e => setHealth('rise', Number(e.target.value) || 2)} />
            </FormField>
            <FormField label="승격 유예 (초)" help="기본 30초 (0 = 유예 없음) — 승격 직후 cold 모듈 기동 시간 흡수">
              <Input type="number" min={0} max={600}
 value={value.health.grace_sec ?? 30}
 onChange={e => setHealth('grace_sec', Number(e.target.value) || 0)} />
            </FormField>

            <FormField label="재기동 임계 (회)" help="윈도우 내 연속 실패 횟수 — 넘으면 로컬 재기동을 포기하고 절체">
              <Input type="number" min={1} max={20}
 value={rl.max_fails}
 onChange={e => setRestart('max_fails', Number(e.target.value) || 3)} />
            </FormField>
            <FormField label="판정 윈도우 (초)" help="기본 300초">
              <Input type="number" min={10} max={3600}
 value={rl.window_sec}
 onChange={e => setRestart('window_sec', Number(e.target.value) || 300)} />
            </FormField>
            <FormField label="권한 복귀 정책" help="자동 복귀 선택 시 복구 노드가 MASTER 를 회수한다">
              <Select value={toSel(value.preempt)} onValueChange={(v: string) => set('preempt', fromSel(v) as 'preempt' | 'nopreempt')}>
                <SelectTrigger><SelectValue /></SelectTrigger>
                <SelectContent>
                  <SelectItem value="nopreempt">복귀 없음 (운영 안정)</SelectItem>
                  <SelectItem value="preempt">자동 복귀 (priority 우선)</SelectItem>
                </SelectContent>
              </Select>
            </FormField>
            {value.preempt === 'preempt' && (
              <FormField label="복귀 지연 (초)" help="옛 MASTER 가 돌아온 뒤 권한 회수 전 안정화 대기">
                <Input type="number" min={0} max={300}
 value={value.preempt_delay}
 onChange={e => set('preempt_delay', Number(e.target.value) || 0)} />
              </FormField>
            )}
          </div>

          {value.preempt === 'preempt' && (
            <Alert variant="warning" className="mt-4">
              자동 복귀 모드는 옛 MASTER 가 살아 돌아올 때 **한 번 더 절체**가 발생합니다(서비스 추가 단절).
 priority 가 의미 있는 비대칭 환경(사양 차이, 주/부 사이트)에서만 권장합니다.
            </Alert>
          )}

          {/* 체크박스 — 시안은 라벨 + 설명 한 줄 (Figma G1) */}
          <label className="mt-4 flex cursor-pointer items-start gap-2.5 rounded-md border border-border p-3">
            <Checkbox  className="mt-0.5" checked={value.track_interface} onCheckedChange={(c) => set('track_interface', (c === true))} />
            <span>
              <span className="text-md font-medium">서비스 NIC 링크 감시</span>
              <span className="mt-0.5 block text-xs text-muted-foreground">
                링크 다운을 즉시 감지해 점검 주기를 기다리지 않고 바로 절체합니다
              </span>
            </span>
          </label>
      </div>
    </SubSection>
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
 failover_mode: s?.ha?.failover_mode ?? MODULE_SPEC_DEFAULT.ha.failover_mode,
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
  // 접힘 상태는 SubSection 이 갖는다 (기본 접힘).
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
    // 시안 G3-1(164:2765~164:2819): 접힘 머리 + 표 + 안내문 + **표 아래** [운영 명세 적용].
    // 구 화면은 머리 우측에 [▶ 적용] 이 있어 접힌 상태에서도 노출됐다 — 계약상 접힌 섹션에
    // 저장/적용 버튼을 두지 않는다.
    // **기본 펼침** — `as-group.md` 는 「접힘」이라 썼지만 도안은 표까지 펼쳐 그렸다(그림이 정본).
    <div className="mb-4">
      <SubSection level={1}
 title="모듈 운영 명세 (감시 · 절체 모드)"
 hint="config.json 과 별개 파일(service.json)로 각 노드에 저장 · agent 가 감시·절체 판정에 사용">
        <DataTable>
          <thead>
            <tr>
              <Th width={140}>모듈</Th>
              <Th width={120} title="프로세스 감시(watchdog) — 죽으면 자동 재기동. 끄면 재기동 안 함(장애 시 즉시 절체 판정).">프로세스 감시</Th>
              <Th width={150} title="Cold(기본): standby 정지 + 승격 시 기동 / Hot: 양쪽 상시 기동(VIP-only). AS 만 적용.">절체 모드</Th>
              <Th width={110} title="이 모듈 실패가 절체 사유가 되는지. 끄면 이 모듈이 죽어도 절체하지 않음(부가 모듈).">절체 관여</Th>
              <Th width={302} title="안전 등급 — shared_writer/unknown 은 자동 래치 해제 금지(수동 확인 필요). VIP 없이 DB/파일에 쓰는 모듈은 fencing/lease 전제.">안전 등급</Th>
            </tr>
          </thead>
          <tbody>
            {modules.map(m => {
 const sp = specs[m] || MODULE_SPEC_DEFAULT
 return (
                <tr key={m}>
                  <Td>{m}</Td>
                  <Td>
                    <Checkbox checked={sp.supervision.watchdog}
 onCheckedChange={v => setSup(m, v === true)} />
                  </Td>
                  <Td>
                    {/* SelectTrigger 가 `w-full` 이라 클래스로는 못 좁힌다 — 감싸서 폭을 준다 */}
                    <span className="inline-block w-[110px]">
                    <Select value={toSel(sp.ha.failover_mode)} onValueChange={(v: string) => setMode(m, fromSel(v) as 'cold' | 'hot')} disabled={group.mode !== 'active_standby'}>
                      <SelectTrigger><SelectValue /></SelectTrigger>
                      <SelectContent>
                        <SelectItem value="cold">cold</SelectItem>
                        <SelectItem value="hot">hot</SelectItem>
                      </SelectContent>
                    </Select>
                    </span>
                  </Td>
                  <Td>
                    <Checkbox checked={sp.ha.failover_relevant}
 onCheckedChange={v => setRelevant(m, v === true)} />
                  </Td>
                  <Td>
                    <span className="inline-block w-[150px]">
                    <Select value={toSel(sp.safety?.class ?? 'unknown')} onValueChange={(v: string) => setSafety(m, fromSel(v) as SafetyClass)}>
                      <SelectTrigger><SelectValue /></SelectTrigger>
                      <SelectContent>
                        <SelectItem value="stateless">stateless</SelectItem>
                        <SelectItem value="read_only">read_only</SelectItem>
                        <SelectItem value="shared_writer">shared_writer</SelectItem>
                        <SelectItem value="unknown">unknown</SelectItem>
                      </SelectContent>
                    </Select>
                    </span>
                  </Td>
                </tr>
              )
            })}
          </tbody>
        </DataTable>
        <div className="mt-2.5 text-xs text-muted-foreground">
          안전 등급 shared_writer / unknown 은 절체 후 자동 복귀(래치 해제)를 하지 않고 운영자
          확인을 요구합니다. 절체 모드 cold / hot · 안전 등급 stateless / read_only /
 shared_writer / unknown.
        </div>
        <Button variant="outline" className="mt-2.5" onClick={save} disabled={!dirty || saving}
 title="모듈 운영 명세 변경을 각 멤버 노드에 반영 (service.json + keepalived 재렌더)">
          {saving ? '적용 중…' : '운영 명세 적용'}
        </Button>
        {!dirty && (
          <div className="mt-1 text-xs text-muted-foreground">
 [운영 명세 적용] 은 값을 바꾸면 열립니다.
          </div>
        )}
      </SubSection>
    </div>
  )
}


// ──────────────────────────────────────────────────────────────
//  Inspector (선택된 서버 상세)
// ──────────────────────────────────────────────────────────────

type InspectorTab = 'install' | 'info' | 'network' | 'modules'

/**
 * ServerContextBar — 탭 위 지속 컨텍스트 (Scope=Server).
 * 정본 = Figma Sec/ContextBar 21:2. **4탭 전부에 유지된다** — 어느 탭에 있든 지금 무엇을
 * 보고 있는지와 그 대상의 액션이 같은 자리에 있어야 한다(decisions.md §3).
 *
 *   ● 이름 ✎ (상태) #id · vX ···· [메트릭] [점검] [더보기 ▾]
 *
 * 구 헤더는 액션 8개를 한 줄에 늘어놓고 [삭제]를 빨간 solid 로 상시 노출했다(decisions.md §4).
 * 자주 쓰고 안전한 둘만 밖에 두고 나머지는 드롭다운으로 넣는다. 파괴적 액션은 **행 단위라
 * outline** — destructive solid 는 그룹/전체 단위에만 쓴다(DESIGN-RULES §2).
 */
function ServerContextBar({ a, onApprove, onRevoke, onRemove, onRename, onUpgrade, onRestart,
 onRollbackAgent, onMetrics, onHealthCheck, onClickReinstall }: {
 a: Agent
 onApprove: (a: Agent) => void
 onRevoke: (a: Agent) => void
 onRemove: (a: Agent) => void
 onRename: (a: Agent) => void
 onUpgrade: (a: Agent) => void
 onRestart: (a: Agent) => void
 onRollbackAgent: (a: Agent) => void
 onMetrics: (a: Agent) => void
 onHealthCheck: (a: Agent) => void
 onClickReinstall: () => void
}) {
 return (
    <div className="flex h-[60px] shrink-0 items-center gap-2 border-b border-border px-3.5">
      <StatusDot tone={statusTone(a.status)} className="[&>span:last-child]:hidden" />
      <b className="text-lg">{agentDisplayName(a.name)}</b>
      {agentDisplayName(a.name) !== a.name && (
        <span className="text-xs text-muted-foreground">{a.name}</span>
      )}
      {/* 이름은 표시 라벨이다 — 시스템은 #id 로 동작하므로 바꿔도 파급이 없다
          (identifier_model.md). 그래서 별도 확인·경고 없이 바로 고친다. */}
      <button className="rounded p-1 text-muted-foreground hover:bg-accent hover:text-foreground"
 title="서버 이름 변경 (표시용 — 시스템은 #id 로 동작)"
 onClick={() => onRename(a)}><Pencil size={14} /></button>
      <Badge variant={statusBadge(a.status)}>{a.status}</Badge>
      <span className="font-mono text-xs text-muted-foreground">
        #{a.id}{a.agent_version ? ` · v${a.agent_version}` : ''}
      </span>

      <div className="ml-auto flex items-center gap-1.5">
        {a.status === 'pending' && (
          <Button variant="default" onClick={() => onApprove(a)}>승인</Button>
        )}
        {(a.status === 'online' || a.status === 'offline') && (
          <>
            <Button onClick={() => onMetrics(a)}>메트릭</Button>
            <Button onClick={() => onHealthCheck(a)}
 disabled={a.status !== 'online'} title="keepalived + 모듈 + VIP 실시간 점검 (sync REST)">
              <Stethoscope size={13} /> 점검
            </Button>
            <DropdownMenu>
              <DropdownMenuTrigger asChild>
                <Button title="그 밖의 서버 액션">더보기 <ChevronDown size={13} /></Button>
              </DropdownMenuTrigger>
              <DropdownMenuContent align="end" className="w-56">
                <DropdownMenuItem disabled={a.status !== 'online'} onSelect={() => onRestart(a)}
 title="agent 프로세스 self-restart (execv)">
                  <RotateCw size={13} /> 재시작
                </DropdownMenuItem>
                <DropdownMenuItem disabled={a.status !== 'online'} onSelect={() => onUpgrade(a)}
 title="agent 바이너리를 최신 버전으로 교체">
                  <ArrowUp size={13} /> 업그레이드
                </DropdownMenuItem>
                {/* 롤백은 이전 버전이 보존된 경우에만 나타난다 (screens/server-scope.md) */}
                {(a.agent_versions || []).filter(v => v && v !== a.agent_version).length > 0 && (
                  <DropdownMenuItem disabled={a.status !== 'online'} onSelect={() => onRollbackAgent(a)}
 title="agent 를 직전(또는 선택) 버전으로 롤백 (current flip + execv)">
                    <ArrowDown size={13} /> 롤백
                  </DropdownMenuItem>
                )}
                {a.status !== 'online' && (
                  <DropdownMenuItem onSelect={onClickReinstall}
 title="물리 서버 교체 / 신규 install — 새 enrollment_token 발급 + 설치 안내 펼침">
                    <RefreshCw size={13} /> 재설치
                  </DropdownMenuItem>
                )}
                <DropdownMenuSeparator />
                <DropdownMenuItem onSelect={() => onRevoke(a)}>폐기</DropdownMenuItem>
                {/* AS 그룹 멤버는 단독 삭제 불가 — 비활성 + **사유 병기**(DESIGN-RULES §2·§3) */}
                <DropdownMenuItem className="text-destructive focus:text-destructive"
 disabled={a.ha_group?.mode === 'active_standby'}
 onSelect={() => onRemove(a)}>
                  <Trash2 size={13} /> 삭제
                  {a.ha_group?.mode === 'active_standby' && (
                    <span className="ml-auto text-xs text-[var(--cims-text-disabled)]">그룹 삭제로만 가능</span>
                  )}
                </DropdownMenuItem>
              </DropdownMenuContent>
            </DropdownMenu>
          </>
        )}
        {a.status !== 'online' && a.status !== 'offline' && a.status !== 'pending' && (
          <Button onClick={() => onRemove(a)}
 disabled={a.ha_group?.mode === 'active_standby'}
 title={a.ha_group?.mode === 'active_standby'
                    ? 'AS 그룹의 멤버는 단독 삭제 불가 — 그룹 삭제로만 가능'
                    : '서버 삭제 (관련 deployment 도 같이 제거)'}>삭제</Button>
        )}
      </div>
    </div>
  )
}

/**
 * GroupContextBar — 탭 위 지속 컨텍스트 (Scope=Group). 정본 = Figma Sec/ContextBar 21:44.
 *
 * [AS] Control (그룹·노드 2) #1 · vrid 51 ···· [설정 비교] [🗑 시스템 삭제]
 *
 * 구 헤더는 AS/AA 칩 색이 `#3498db`/`#27ae60` 하드코딩이었다(hex 금지 위반이자 토큰과 다른 색).
 * 삭제는 **그룹/전체 단위라 destructive solid** 가 맞다 — 행 단위 outline 과 구분된다
 * (DESIGN-RULES §2). 이름 편집은 저장 흐름에 묶여 있어 인스펙터 본문에 남는다.
 */
function GroupContextBar({ group, memberCount, onOpenConfig, onDeleteSystem }: {
 group: HaGroup
 memberCount: number
 onOpenConfig: () => void
 onDeleteSystem: (g: HaGroup) => void
}) {
 const as = group.mode === 'active_standby'
 return (
    <div className="flex h-[60px] items-center gap-2 px-3.5">
      <Badge variant={as ? 'brandSoft' : 'successSoft'}
 title={`mode=${group.mode} (생성 후 변경 불가)`}>{as ? 'AS' : 'AA'}</Badge>
      <b className="text-lg">{group.name}</b>
      <Badge variant="neutralSoft">그룹 · 노드 {memberCount}</Badge>
      <span className="font-mono text-xs text-muted-foreground">#{group.id} · vrid {group.vrid}</span>
      <div className="ml-auto flex items-center gap-1.5">
        <Button onClick={onOpenConfig}
 title="멤버별 설정값 나란히 비교 (읽기 전용) — 편집은 각 멤버 서버의 패키지 설정 탭">
          설정 비교
        </Button>
        <Button variant="destructive" onClick={() => onDeleteSystem(group)}
 title="HA 그룹 + 모든 멤버 일괄 삭제">
          <Trash2 size={13} /> 시스템 삭제
        </Button>
      </div>
    </div>
  )
}

function ServerInspector({ agent: a, mode, deployments, packages, vipIps, mgmtVip, reinstallSignal,
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
  /** ContextBar 의 [재설치] 신호 — 값이 오르면 설치 안내를 펼치고 token 재발급을 건다 */
 reinstallSignal: number
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
  // ContextBar 의 [재설치] — 이제 바가 탭 위로 올라가 인스펙터 밖에 있으므로 시그널로 잇는다.
  // 값이 오르면 설치 안내 섹션을 펼치고 token 재발급을 건다.
 const [autoRegenSignal, setAutoRegenSignal] = useState(0)
 useEffect(() => {
 if (!reinstallSignal) return
 setOpenSections(prev => new Set(prev).add('install'))
 setAutoRegenSignal(v => v + 1)
  }, [reinstallSignal])
 const toggleSection = (s: InspectorTab) => {
 setOpenSections(prev => {
 const next = new Set(prev)
 if (next.has(s)) next.delete(s); else next.add(s)
 return next
    })
  }

 return (
    <>
      {/* 섹션 stack — 페이지 탭에 따라: infra=구성(설치안내/정보/네트워크), install=모듈 */}
      <div className="flex-1 overflow-auto">
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
        {/* 시안 S2(91:1325)는 이 탭을 **접힘 섹션이 아니라 제목 줄**로 연다 —
            탭이 이미 스코프를 갈랐으므로 한 겹 더 접을 이유가 없다. */}
        {mode === 'install' && (
          <ModulesTab agent={a} deployments={deployments} packages={packages} packagesAvailable={packages.length > 0}
 onAddDeploy={onAddDeploy}
 onJob={onJob} onUpgrade={onUpgradeDep} onRollback={onRollback} onRemoveDep={onRemoveDep} />
        )}
        {mode === 'control' && (
          <ControlTab agent={a} deployments={deployments} packages={packages} onJob={onJob} />
        )}
      </div>
    </>
  )
}

// ── [패키지 설정] 탭 — 서버 선택: 모듈별 탭 + 설정 패널 (다이얼로그의 페이지화) ──
function AgentConfigTab({ deployments, packages, onDone, onOpenGroupConfig }: {
 deployments: Deployment[]
 packages: SipPackage[]
 onDone: () => Promise<void> | void
  /** 시안 S3 의 [그룹 공통 설정 편집 →] — 그룹을 선택하고 같은 탭을 연다 */
 onOpenGroupConfig: (groupId: number) => void
}) {
  // 폴링 identity churn 차단 — mount 시 스냅샷 (모듈 전환은 key 리마운트)
  // pending(설치 전) 도 포함 — DB/notify/시크릿을 설치 전에 미리 지정(overlay 저장→설치 시 반영).
 const [deps] = useState(() => deployments.filter(d => d.status !== 'removed'))
 const [selDep, setSelDep] = useState<number>(deps[0]?.id ?? 0)
 const dep = deps.find(d => d.id === selDep)
 const source = useMemo(
    () => dep ? ({ type: 'deployment' as const, deployment: dep }) : null,
 [dep])
  // 칩의 설정 개수 — **이 화면에 나오는 필드 수**(서버 스코프 = system). 패키지에 딸려 온
  // config_template 로 그 자리에서 센다 (모듈마다 config view 를 부르지 않는다).
 const pkgById = useMemo(() => new Map(packages.map(p => [p.id, p])), [packages])
 const countFor = (d: Deployment) => {
 const t = pkgById.get(d.package_id)?.config_template
 if (!t) return null
 let n = 0
 for (const sec of t.sections) {
 const only = sectionForScope(sec, 'system')
 if (only) n += only.fields.length
    }
 return n
  }
 if (deps.length === 0) {
 return (
      <div className="p-4">
        <EmptyState title="설치된 모듈 없음"
 description="[패키지 설치] 탭에서 모듈을 먼저 배포하세요." />
      </div>
    )
  }
 return (
    <div className="flex h-full flex-col">
      {/* 모듈 칩 — 정본 Figma S3 `ModuleTabs`(266:4697): 높이 32 · 사이 6 ·
          이름 + 버전(mono) + 설정 개수 칩. 구 화면은 파일 탭 모양이었다. */}
      <div className="flex shrink-0 flex-wrap items-center gap-1.5 px-4 pb-2 pt-5">
        {deps.map(d => {
 const active = d.id === selDep
 const n = countFor(d)
 return (
            <button key={d.id} onClick={() => setSelDep(d.id)}
 aria-pressed={active}
 className={`flex h-8 items-center gap-[7px] rounded-full border px-3 text-base font-semibold transition-colors ${
 active
                        ? 'border-primary bg-brandsoft text-brandsoft-on'
                        : 'border-border bg-card text-foreground hover:bg-accent'}`}>
              {d.package_name}
              <span className="font-mono text-sm font-normal text-muted-foreground">
 v{d.package_version}
              </span>
              {n !== null && (
                // 도안 `count`(291:5173) 실측 — 20×16 알약. 채움만 칩 배경과 한 단 어긋나게
                // 두고(선택=`--surface` / 비선택=`--bg-soft`) 글자는 양쪽 다 muted 다.
                <span className={`inline-flex h-4 min-w-5 items-center justify-center rounded-full px-1.5 text-sm font-semibold text-muted-foreground ${
 active ? 'bg-card' : 'bg-muted'}`}>
                  {n}
                </span>
              )}
            </button>
          )
        })}
      </div>
      <div className="min-h-0 flex-1">
        {source && (
          <ModuleConfigModal key={selDep} inline source={source}
 onClose={() => { /* inline */ }} onDone={onDone}
 onOpenGroupConfig={onOpenGroupConfig} />
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
 const memberNames = group.members
    .map(m => agentDisplayName(agents.find(a => a.id === m.agent_id)?.name || `#${m.agent_id}`))
  // 드리프트 판정 — 모듈별로 멤버 간 버전이 갈리면 **기준과 다른 쪽**에 배지를 단다.
  // 기준은 G1 안내와 같은 축이다: ACTIVE 노드, 없으면 첫 멤버.
 const baseAid = group.active_agent_id ?? group.members[0]?.agent_id ?? null
 const baseVer = new Map<string, string>()
 for (const d of (baseAid != null ? depsByAgent.get(baseAid) || [] : [])) {
 if (d.package_name && d.package_version) baseVer.set(d.package_name, d.package_version)
  }
 const drifted = (d: Deployment) => {
 const b = d.package_name ? baseVer.get(d.package_name) : undefined
 return !!b && !!d.package_version && b !== d.package_version
  }
 return (
    <div className="overflow-auto px-4 pt-3.5">
      <TitleRow title="멤버별 패키지 배포 현황"
 hint="그룹 멤버에 배포된 모듈과 버전을 한눈에 확인" />
      {/* 이 화면이 **조회 전용**임을 먼저 알린다 (Figma G2 184:2864) — 작업 지점이 다른 화면이라
          여기서 버튼을 찾다 헤매는 자리였다. */}
      <Alert variant="info" className="mt-2.5">
        <div className="font-medium">설치 · 재설치 · 롤백은 서버 스코프에서 수행합니다</div>
        <div className="mt-0.5 text-xs opacity-90">
          좌측 트리에서 멤버({memberNames.join(' / ') || '없음'})를 선택한 뒤 [패키지 설치] 탭에서
          작업하세요. 이 화면은 조회 전용입니다.
        </div>
      </Alert>
      {/* 컬럼 폭은 Figma G2(184:2870) 실측 */}
      <DataTable className="mt-5">
        <thead>
          <tr>
            <Th width={180}>서버</Th>
            <Th width={130}>서버 상태</Th>
            <Th width={512}>배포 모듈</Th>
          </tr>
        </thead>
        <tbody>
          {group.members.map(m => {
 const ag = agents.find(a => a.id === m.agent_id)
 const deps = depsByAgent.get(m.agent_id) || []
 return (
              // 도안(G2 184:2870)은 서버명·상태를 **첫 모듈 줄과 같은 줄**에 둔다 — 모듈이 여러 줄인
              // 행에서 가운데 정렬하면 어느 모듈이 어느 서버인지 눈으로 잇기 어렵다.
              <tr key={m.agent_id} className="cursor-pointer"
 onClick={() => onSelectMember(m.agent_id)}>
                <Td className="py-2.5 align-top">{agentDisplayName(ag?.name || `#${m.agent_id}`)}</Td>
                <Td className="py-2.5 align-top"><StatusDot status={ag?.status || 'offline'} /></Td>
                <Td className="py-2.5 align-top">
                  {deps.length === 0
                    ? <span className="text-muted-foreground">배포된 모듈 없음</span>
                    : (
                      <div className="flex flex-col gap-1">
                        {deps.map(d => (
                          <span key={d.id} className="flex flex-wrap items-center gap-2">
                            <span>{d.package_name}</span>
                            <span className={`font-mono text-sm ${
 drifted(d) ? 'text-destructive' : 'text-muted-foreground'}`}>
 v{d.package_version}
                            </span>
                            {/* 설치·제어 탭과 같은 실측 우선 상태 */}
                            <StatusDot tone={depTone(depEffectiveStatus(d))}
 label={depEffectiveStatus(d)} />
                            {drifted(d) && (
                              <Badge variant="warningSoft"
 title={`기준(${agentDisplayName(agents.find(a => a.id === baseAid)?.name || '')}) `
                                          + `= v${baseVer.get(d.package_name || '')}`}>
                                드리프트
                              </Badge>
                            )}
                          </span>
                        ))}
                      </div>
                    )}
                </Td>
              </tr>
            )
          })}
        </tbody>
      </DataTable>
      <div className="mt-2.5 text-xs text-muted-foreground">
        버전이 어긋난 항목은 드리프트로 표시됩니다. 교정하려면 해당 멤버를 선택해
 [패키지 설치] 에서 재설치하세요.
      </div>
    </div>
  )
}

/** Level 1 섹션 머리 — Level 2 는 `custom/collapsible-section` 의 `SubSection`. */
function InspectorSection({ title, expanded, onToggle, children }: {
 title: string
 expanded: boolean
 onToggle: () => void
 children: React.ReactNode
}) {
  // 머리 모양은 `SubSection level={1}` 과 같다 — 도안(S1 49:1369 · G1 42:428)의 큰 구획 머리는
  // 전부 같은 `CollapsibleSectionHeader` Level 1 이다. 구 화면은 여기만 회색 띠였다.
  // 열림 상태를 바깥이 들고 있어(여러 섹션 동시 제어) SubSection 을 그대로 쓰지는 못한다.
 return (
    <div className="px-4">
      <button onClick={onToggle}
              className="flex h-9 w-full select-none items-center gap-2 border-b border-border text-left">
        <span className="w-4 shrink-0 text-foreground">
          {expanded ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
        </span>
        <span className="text-md font-semibold">{title}</span>
      </button>
      {expanded && <div className="pb-4 pt-3.5">{children}</div>}
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
 const pkgById = new Map(packages.map(p => [p.id, p]))
 return (
    <div className="px-4 pt-3.5">
      <TitleRow title={`모듈 (${deployments.length})`}
 hint="설치·버전 관점 — 시작·정지는 «패키지 제어» 탭" />
      {deployments.length === 0 ? (
        // 시안에 이 화면의 빈 상태 문구가 없다 — 현행 웹 문구를 그대로 쓴다(지어내지 않는다).
        <EmptyState className="mt-2.5" title="배포된 모듈 없음" />
      ) : (
        // 컬럼 폭은 Figma S2(91:1771) 실측. `빌드 · git` 은 시안이 설명 칸에서 떼어낸
        // 별도 컬럼이다 — 웹은 설명 뒤에 원문이 붙어 행 높이가 제각각이었다 (decisions.md §5).
        <DataTable className="mt-2.5">
          <thead>
            <tr>
              <Th width={186}>모듈</Th>
              <Th width={96}>버전</Th>
              <Th width={176}>빌드 · git</Th>
              <Th width={84}>상태</Th>
              <Th width={280} align="right">작업</Th>
            </tr>
          </thead>
          <tbody>
            {deployments.map(d => (
              <DeploymentRow key={d.id} dep={d} agent={a} packages={packages}
 pkg={pkgById.get(d.package_id) ?? null}
 onJob={onJob} onUpgrade={onUpgrade} onRollback={onRollback}
 onRemove={onRemoveDep} />
            ))}
          </tbody>
        </DataTable>
      )}
      {/* 시안은 [+ 모듈 추가] 를 표 아래 왼쪽 Secondary 로 둔다 (185:3125) */}
      <Button variant="outline" className="mt-2.5"
 disabled={a.status !== 'online' || !packagesAvailable}
 onClick={onAddDeploy}>+ 모듈 추가</Button>
      {(a.status !== 'online' || !packagesAvailable) && (
        // 비활성 사유는 눈에 보이게 (contracts.md §Button)
        <div className="mt-1 text-xs text-muted-foreground">
          {a.status !== 'online'
            ? '[+ 모듈 추가] 는 서버가 online 일 때 열립니다.'
            : '[+ 모듈 추가] 는 릴리스에 패키지를 업로드한 뒤 열립니다.'}
        </div>
      )}
      <div className="mt-2.5 text-xs text-muted-foreground">
        롤백은 이전 버전이 보존된 모듈에서만 활성화됩니다. 긴 설명은 모듈명 아래 한 줄 요약하고
        전체 설명은 툴팁으로 제공합니다.
      </div>
    </div>
  )
}

/**
 * 탭 본문 제목 줄 — 접히지 않는 섹션 머리. 정본 = Figma S2 `titleRow` (91:1749) ·
 * S4 도 같은 모양이다. 접히는 섹션은 `SubSection`(CollapsibleSectionHeader) 쪽이다.
 */
function TitleRow({ title, hint }: { title: string; hint?: string }) {
 return (
    <div className="flex h-5 items-center gap-2">
      <span className="text-md font-semibold">{title}</span>
      {hint && <span className="truncate text-xs text-muted-foreground">{hint}</span>}
    </div>
  )
}

/**
 * 패키지 설명에서 백엔드가 붙인 `| build: … | git: … | changelog: …` 꼬리를 뗀다.
 * 조립 지점은 `oam/src/handlers/agents.py` 의 `desc_lines` 세 곳 — 구조화된 값은
 * `meta.build_date`/`git_sha`/`git_branch` 에 그대로 있으므로 표는 그쪽을 쓰고,
 * 설명 칸에는 한 줄 요약만 남긴다 (decisions.md §5).
 */
function pkgSummary(desc: string | null | undefined): string {
 return (desc || '').split(/\s*\|\s*(?:build|git|changelog)\s*:/)[0].trim()
}

/** meta 가 없는 레거시 패키지를 위한 fallback — 설명 꼬리에서 build/git 을 되읽는다. */
function pkgBuildGit(pkg: SipPackage | null): { build: string | null; git: string | null } {
 const m = pkg?.meta
 if (m?.build_date || m?.git_sha) {
 return {
 build: m.build_date || null,
 git: m.git_sha ? m.git_sha + (m.git_branch ? ` (${m.git_branch})` : '') : null,
    }
  }
 const d = pkg?.description || ''
 const b = d.match(/\|\s*build\s*:\s*([^|]+)/)
 const g = d.match(/\|\s*git\s*:\s*([^|]+)/)
 return { build: b ? b[1].trim() : null, git: g ? g[1].trim() : null }
}

// [패키지 설치] 탭 모듈 행 — 파일 배치 작업만 (설치/재설치/업그레이드/롤백/삭제).
// 프로세스 start/stop/restart 는 [패키지 제어] 탭, 설정은 [패키지 설정] 탭.
function DeploymentRow({ dep: d, agent, packages, pkg, onJob, onUpgrade, onRollback, onRemove }: {
 dep: Deployment; agent: Agent
 packages: SipPackage[]
  /** 이 배포가 쓰는 패키지 레코드 — 설명·빌드/git 의 출처 */
 pkg: SipPackage | null
 onJob: (d: Deployment, jt: JobType) => void | Promise<void>
 onUpgrade: (d: Deployment) => void
 onRollback: (d: Deployment) => void | Promise<void>
 onRemove: (d: Deployment) => void | Promise<void>
}) {
  // 진행 중 표시·연타 차단 — [패키지 제어] 탭(ControlTab)과 같은 규약.
  // install 은 queueJob 이 awaitJob 으로 job 완료까지 기다리는데(로컬 패치 06) 그동안
  //   버튼이 아무 변화가 없어 **눌렸는지 알 수 없었다**. 눌린 버튼은 '⏳ 진행 중' 으로
  //   바뀌고 같은 행의 나머지 작업은 잠근다(같은 모듈에 설치·롤백 동시 진행 방지).
  //   업그레이드는 모달만 여는 동기 동작이라 대상에서 뺀다 — 잠금이 깜빡이고 말 뿐이다.
 type RowAction = 'install' | 'rollback' | 'remove'
 const [rowBusy, setRowBusy] = useState<RowAction | null>(null)
 async function runRow(kind: RowAction, fn: () => void | Promise<void>) {
 if (rowBusy) return
 setRowBusy(kind)
 try { await fn() } finally { setRowBusy(null) }
  }
 const rowLbl = (k: RowAction, text: ReactNode): ReactNode =>
    (rowBusy === k ? <><Hourglass size={12} /> 진행 중</> : text)
 const rowTip = (t: string) => (rowBusy ? `${rowBusy} 진행 중 — 완료까지 기다리세요` : t)
  // 상태 배지·색은 실측 우선(depEffectiveStatus) — [패키지 제어] 탭과 동일 기준.
  // 죽어 있으면 마지막 job 결과가 running 이어도 stopped 로 보인다(두 탭 일치).
 const shown = depEffectiveStatus(d)
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
 const summary = pkgSummary(pkg?.description)
 const bg = pkgBuildGit(pkg)
  // 시안에 프로세스 이름 컬럼이 없다. 대개 모듈명과 같지만 다를 때가 있어 툴팁으로 남긴다.
 const procNote = d.process_name && d.process_name !== d.package_name
    ? `\n프로세스: ${d.process_name}` : ''
 return (
    <tr>
      <Td>
        <div className="min-w-0"
 title={`${pkg?.description || summary || d.package_name || ''}`
                    + `\n설치 경로: ${d.install_path || '—'}${procNote}`
                    + `${histTip ? `\n\n설치 이력:\n${histTip}` : ''}`}>
          <div className="truncate">{d.package_name || '—'}</div>
          {/* 긴 설명은 한 줄 요약 + 툴팁 (decisions.md §5) */}
          <div className="truncate text-xs font-normal text-muted-foreground">
            {summary || '—'}
          </div>
        </div>
      </Td>
      <Td mono>v{d.package_version || '?'}</Td>
      <Td className="text-muted-foreground">
        <div className="font-mono text-xs leading-[1.4]">{bg.build || '—'}</div>
        <div className="font-mono text-xs leading-[1.4]">{bg.git || '—'}</div>
      </Td>
      <Td><Badge variant={depBadge(shown)}>{shown}</Badge></Td>
      <Td>
        <div className="flex items-center justify-end gap-1.5">
          <Button variant="outline" disabled={!online || !!rowBusy}
 title={rowTip('install (파일 배치 + 설정 적용)')}
 onClick={() => runRow('install', () => onJob(d, 'install'))}>
            {rowLbl('install', notInstalled ? '설치' : '재설치')}
          </Button>
          <Button variant="outline" disabled={!canUpgrade || !!rowBusy}
 title={rowBusy ? rowTip('') : !online ? 'agent 오프라인'
              : notInstalled ? '아직 설치 전 — [설치] 를 먼저 하세요'
              : isRunning ? '실행 중에는 업그레이드할 수 없습니다 — [패키지 제어] 에서 정지 후 진행하세요'
              : upCands.length === 0 ? `${d.package_name} 의 다른 버전 패키지가 없음 (릴리스에 업로드 필요)`
              : `버전을 골라 업그레이드 (등록됨: ${upCands.map(p => 'v' + p.version).join(', ')})`}
 onClick={() => onUpgrade(d)}>업그레이드</Button>
          {/* 롤백은 이전 버전이 보존된 모듈에서만 활성 — 시안도 대개 비활성 상태로 그렸다.
              Ghost 인 것은 계약이 「되돌리기」를 Ghost 로 못박았기 때문이다. */}
          <Button variant="ghost" disabled={!canRollback || !!rowBusy}
 title={rowBusy ? rowTip('') : canRollback
              ? `이전 버전으로 롤백 (v${d.prev_package_version || '?'} · ${d.prev_install_path})`
              : '롤백 대상 없음 (이전 버전 설치 이력 없음)'}
 onClick={() => runRow('rollback', () => onRollback(d))}>
            {rowLbl('rollback', '롤백')}</Button>
          <Button variant="destructive" disabled={!!rowBusy}
 title={rowTip('이 서버에서 모듈 삭제')}
 onClick={() => runRow('remove', () => onRemove(d))}>
            {rowLbl('remove', <><Trash2 /> 삭제</>)}</Button>
        </div>
      </Td>
    </tr>
  )
}

/**
 * 모듈 상태의 톤. **`stopped` 은 Danger 다** — 시안 네 장(S2 91:1809 · S4 92:2051 ·
 * G2 184:2897 · G4)이 전부 그렇게 그렸다(실측 `#b91c1c`/`#dc2626`). `DESIGN-RULES` §2 의
 * 「Neutral = stopped」와 어긋나는데, 그 표는 **노드/에이전트** 상태의 톤 맵이고 모듈
 * 프로세스가 죽어 있는 것은 서비스 영향이라 그림 쪽이 맞다고 봤다 (정본 문서 §7-16).
 *
 * 모양은 화면마다 다르다 — 서버 화면(S2·S4)은 Badge, 그룹 화면(G2·G4)은 StatusDot 이다.
 * 각 화면 그림 그대로 간다. 톤만 두 모양에서 같게 맞춘다.
 */
function depBadge(st: string) {
 if (st === 'running') return 'successSoft' as const
 if (st === 'pending' || st === 'deploying') return 'infoSoft' as const
 if (st === 'removed') return 'neutralSoft' as const
 return 'dangerSoft' as const   // stopped · failed
}
function depTone(st: string): StatusTone {
 if (st === 'running') return 'success'
 if (st === 'pending' || st === 'deploying') return 'info'
 if (st === 'removed') return 'neutral'
 return 'danger'                // stopped · failed
}

// ── [패키지 제어] 탭 — 서버 선택: 모듈별 프로세스 start/stop/restart ──
function ControlTab({ agent: a, deployments, packages, onJob }: {
 agent: Agent
 deployments: Deployment[]
 packages: SipPackage[]
 onJob: (d: Deployment, jt: JobType) => void
}) {
 const pkgById = new Map(packages.map(p => [p.id, p]))
 return (
    <div className="px-4 pt-3.5">
      {/* 제어 대상이 「서버」가 아니라 「서버의 모듈」임을 먼저 못박는다 (Figma S4 92:1995) —
          ContextBar 의 [더보기 › 재시작] 과 대상이 달라 헷갈리는 자리다. */}
      <Alert variant="info">
        <div className="font-medium">
          여기서 제어하는 대상은 {agentDisplayName(a.name)} 의 모듈입니다
        </div>
        <div className="mt-0.5 text-xs opacity-90">
          서버 자체를 재시작하려면 위 ContextBar 의 «더보기 › 재시작» 을 사용하세요 — 대상이 다릅니다.
        </div>
      </Alert>
      <div className="mt-5">
        <TitleRow title={`모듈 제어 (${deployments.length})`}
 hint="런타임 관점 — 버전·재설치는 «패키지 설치» 탭" />
      </div>
      {deployments.length === 0 ? (
        <EmptyState className="mt-2.5" title="배포된 모듈 없음"
 description="[패키지 설치] 탭에서 모듈을 먼저 배포하세요." />
      ) : (
        // 컬럼 폭은 Figma S4(92:2021) 실측. 설치 탭과 달리 빌드·git 이 없다 — 런타임 관점이다.
        <DataTable className="mt-2.5">
          <thead>
            <tr>
              <Th width={190}>모듈</Th>
              <Th width={110}>버전</Th>
              <Th width={120}>모듈 상태</Th>
              <Th width={402} align="right">제어</Th>
            </tr>
          </thead>
          <tbody>
            {deployments.map(d => {
 const pkg = pkgById.get(d.package_id) ?? null
 const summary = pkgSummary(pkg?.description)
 const shown = depEffectiveStatus(d)
 return (
                <tr key={d.id}>
                  <Td>
                    <div className="min-w-0" title={pkg?.description || summary || ''}>
                      <div className="truncate">{d.package_name || '—'}</div>
                      <div className="truncate text-xs font-normal text-muted-foreground">
                        {summary || '—'}
                      </div>
                    </div>
                  </Td>
                  <Td mono>v{d.package_version || '?'}</Td>
                  <Td><Badge variant={depBadge(shown)}>{shown}</Badge></Td>
                  <Td>
                    <div className="flex items-center justify-end">
                      <ProcessControlButtons dep={d} agent={a} onJob={onJob} />
                    </div>
                  </Td>
                </tr>
              )
            })}
          </tbody>
        </DataTable>
      )}
      {/* 비활성 사유를 눈에 보이게 (contracts.md §Button) — 시안 note(92:2170) 그대로 */}
      <div className="mt-2.5 text-xs text-muted-foreground">
        현재 상태에서 불가능한 동작은 비활성 처리합니다 — running 이면 «시작», stopped 이면
        «재시작·정지». 오클릭으로 서비스를 건드리는 일을 줄입니다.
      </div>
    </div>
  )
}

// 실측 우선 유효 상태 — running/stopped 구간에서는 실측(live_state)이 정본.
// 기록(status)은 운영자 지시 이력일 뿐, HA 절체(notify)가 로컬에서 모듈을 켜고
// 끄면 현실과 어긋난다. pending/deploying/failed/removed 는 lifecycle 상태라 기록 유지.

// 모듈 상태 셀 — 실측(depEffectiveStatus) 단일 표시. running/stopped 는 실제
// 프로세스 상태 그 자체다 (metric 주기상 최대 30초 지연만 존재).
// 프로세스 제어 버튼 3종 — ControlTab(서버)·GroupControlMatrix(그룹) 공용.
// pending(미설치) 은 전부 비활성 — 설치는 [패키지 설치] 탭.
/**
 * 프로세스 제어 3버튼. 정본 = Figma S4(92:2054) — 아이콘 없이 글자만, 우측 정렬.
 *
 * **현재 상태에서 불가능한 동작은 비활성이다** (시안 note 92:2170): running 이면 `시작`,
 * stopped 이면 `재시작`·`정지`. 구 화면은 셋 다 상시 활성이라 오클릭이 곧 서비스 조작이었다.
 * 판정 기준은 배지와 같은 **실측 우선 상태**(`depEffectiveStatus`) — 배지는 stopped 인데
 * 시작이 잠겨 있으면 화면이 스스로 모순되기 때문이다.
 *
 * stopped 일 때 `시작` 이 Primary 인 것도 시안 그대로다 — 그 상태에서 할 일이 하나뿐이다.
 */
function ProcessControlButtons({ dep: d, agent, onJob }: {
 dep: Deployment; agent?: Agent
 onJob: (d: Deployment, jt: JobType) => void
}) {
 const online = agent?.status === 'online'
 const notInstalled = d.status === 'pending'
 const shown = depEffectiveStatus(d)
  // 바깥 게이트는 그대로 — 오프라인·미설치·failed 등에서는 셋 다 잠근다.
 const controllable = online && !notInstalled && (shown === 'running' || shown === 'stopped')
 const isRunning = shown === 'running'
 const tip = (act: string) =>
 notInstalled ? '설치 필요 — [패키지 설치] 탭에서 먼저 설치'
    : !online ? 'agent 오프라인'
    : !controllable ? `${shown} 상태에서는 제어할 수 없습니다`
    : act
 return (
    <div className="flex flex-wrap items-center gap-1.5">
      <Button variant={controllable && !isRunning ? 'default' : 'outline'}
 disabled={!controllable || isRunning}
 title={isRunning ? '이미 running 입니다' : tip('프로세스 시작')}
 onClick={() => onJob(d, 'start')}>시작</Button>
      <Button disabled={!controllable || !isRunning}
 title={!isRunning && controllable ? 'stopped 상태에서는 [시작] 을 쓰세요' : tip('프로세스 재시작')}
 onClick={() => onJob(d, 'restart')}>재시작</Button>
      <Button disabled={!controllable || !isRunning}
 title={!isRunning && controllable ? '이미 stopped 입니다' : tip('프로세스 정지')}
 onClick={() => onJob(d, 'stop')}>정지</Button>
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
 const confirm = useConfirm()
 const [busy, setBusy] = useState<string | null>(null)
 const isAS = group.mode === 'active_standby'
 const activeName = group.active_agent_id != null
    ? agentDisplayName(agents.find(a => a.id === group.active_agent_id)?.name || `#${group.active_agent_id}`)
    : null

 async function batch(action: 'start' | 'stop' | 'restart') {
 const label = action === 'start' ? '일괄 시작' : action === 'stop' ? '일괄 중지' : '일괄 재시작'
 if (action === 'stop' && !await confirm({
 title: '그룹 일괄 중지', tone: 'danger', confirmLabel: '일괄 중지', body: <>
 [{group.name}] 그룹의 서비스를 전부 중지합니다.
          <div className="mt-1">VIP(가상 IP)도 내려가 서비스가 완전히 중단됩니다. 계속할까요?</div>
        </> }))
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
 if (!force && !await confirm({
 title: '수동 절체', confirmLabel: '절체', body: <>
 [{group.name}] 수동 절체 — 현재 Active({activeName || '?'}) 에서 Standby 로 서비스를 넘깁니다.
          <div className="mt-1">절체 중 수 초의 순단이 발생할 수 있습니다. 계속할까요?</div>
        </> }))
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
 const lines = list.slice(0, 6).map(a => `${a.name} → ${a.oam_url}`)
 if (await confirm({ title: 'OAM 주소 전환', confirmLabel: '전환', body: <>
            {(e as Error).message}
            <ul className="mt-2 list-disc pl-5 font-mono text-xs">
              {lines.map(l => <li key={l}>{l}</li>)}
            </ul>
            <div className="mt-2">지금 전 agent 의 OAM 주소를 VIP 로 바꿀까요? (취소 = 아무것도 하지 않음)</div>
            <div className="mt-1">개별 서버만 바꾸려면 [시스템/서버 구성] &gt; 서버 &gt; OAM 접속 주소 를 쓰세요.</div>
          </> })) {
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
 if (!await confirm({ title: 'OAM 주소 전환', confirmLabel: '전환', body: <>
      전 agent 의 OAM 접속 주소를 아래로 전환합니다.
      <div className="mt-1 font-mono text-xs">{url}</div>
      <div className="mt-2">
        각 agent 가 그 주소로 /health 도달을 확인한 뒤에만 적용합니다 — 도달 불가면
        주소를 바꾸지 않고 실패로 남습니다(fleet 단절 방지).
      </div>
      <div className="mt-2">진행할까요?</div>
    </> })) return
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
 if (!await confirm({
 title: on ? '유지보수 진입' : '유지보수 해제',
 tone: on ? 'danger' : 'default',
 confirmLabel: on ? '점검' : '복귀',
 body: on ? <>
 [{nm}] 를 유지보수(EXCLUDE_NODE)로 전환합니다.
          <div className="mt-1">
            이 노드는 승격 대상에서 제외되고 모듈이 정지됩니다. 상대 노드가 죽어도 이 노드로
            절체되지 않습니다(다운 감수). 계속할까요?
          </div>
        </> : <>
 [{nm}] 유지보수를 해제합니다.
          <div className="mt-1">role 기반으로 모듈이 재기동되어 standby 로 재합류합니다. 계속할까요?</div>
        </> }))
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
    <div className="mt-1.5 flex gap-1.5" onClick={e => e.stopPropagation()}>
      <Button disabled={!!busy} onClick={() => doMaintenance(agentId, true)}
 title="이 노드를 승격 대상에서 제외(유지보수). 모듈 정지 + 이 노드로 절체 안 됨.">
        점검
      </Button>
      <Button disabled={!!busy} onClick={() => doMaintenance(agentId, false)}
 title="유지보수 해제 — role 기반 재기동으로 standby 재합류.">
        복귀
      </Button>
    </div>
  ) : null

  // 시안 G4(121:2535)의 상단 Danger 경고 — **VIP 가 있는 그룹에서만** 유효하다
  // (decisions.md §7). 대체 경로가 없다는 사실을 일괄 버튼 바로 위에서 알린다.
 const vipIp = ((group.vip_bindings || []).find(b => /admin|oam|mgmt/i.test(b.slot || ''))
                 || (group.vip_bindings || [])[0])?.ip || group.vip || ''
 return (
    <div className="overflow-auto px-4 pt-3.5">
      {vipIp && (
        <Alert variant="danger" className="mb-5">
          <div className="font-medium">동시 정지·재시작은 VIP 서비스 중단을 유발합니다</div>
          <div className="mt-0.5 text-xs opacity-90">
            {group.name} 그룹은 VRRP vrid {group.vrid} 로 VIP {vipIp} 을 서비스합니다.
            멤버 {group.members.length}대를 동시에 내리면 대체 경로가 없습니다.
          </div>
        </Alert>
      )}
      {/* 그룹 일괄 제어 — 프로세스 제어와 HA 절체는 위험도가 달라 **구분선으로 가른다**
          (decisions.md §6 · 시안 409:5152). */}
      <div className="rounded-lg bg-muted p-3.5">
        <div className="text-md font-semibold">그룹 일괄 제어</div>
        <div className="mt-2.5 flex flex-wrap items-center gap-1.5">
          <Button disabled={!!busy} onClick={() => batch('start')}
 title="그룹 서비스 시작 — 서비스 의도를 running 으로 두고 무장(VIP 활성). 기준 멤버가 Active 로 기동.">
            일괄 시작
          </Button>
          <Button disabled={!!busy} onClick={() => batch('restart')}
 title="그룹 전 멤버 재시작 — AS 는 standby 먼저, active 는 유예 하에 재시작(절체 없음, 순단 1회).">
            일괄 재시작
          </Button>
          <Button variant="destructive" disabled={!!busy} onClick={() => batch('stop')}
 title="그룹 서비스 중지 — 의도를 stopped 로 두고 비무장(VIP 내려감) + 전 모듈 정지.">
            일괄 중지
          </Button>
          {isAS && (
            <>
              <span className="mx-1.5 h-5 w-px shrink-0 bg-border-strong" aria-hidden />
              <Button disabled={!!busy || group.active_agent_id == null}
 onClick={() => doFailover()}
 title={group.active_agent_id == null
                        ? 'Active 판정 불가 — 잠시 후 재시도'
                        : '수동 절체 — 현재 Active 에서 Standby 로 서비스를 넘김(스위치오버).'}>
                수동 절체{activeName ? ` (현재 ${activeName})` : ''}
              </Button>
            </>
          )}
        </div>
        {isAS && group.active_agent_id == null && (
          // 비활성 사유는 눈에 보이게 (contracts.md §Button)
          <div className="mt-1.5 text-xs text-muted-foreground">
 [수동 절체] 는 Active 노드가 판정된 뒤 열립니다.
          </div>
        )}
      </div>
      <div className="mt-5">
        <TitleRow title="멤버별 프로세스 제어"
 hint="설치 · 재설치 · 롤백은 [패키지 설치] 탭에서 수행합니다" />
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
 background: 'var(--destructive)', color: 'var(--destructive-foreground)', lineHeight: 1.6,
          }}>
            <b>절체 래치 — 승격 불가: {latched.map(a => agentDisplayName(a.name)).join(', ')}</b>
            <div className="mt-1">
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
 border: '1px solid ' + (group.failover_op.error ? 'var(--border)' : 'var(--cims-info-soft)'),
 background: group.failover_op.error ? 'var(--cims-danger-soft)' : 'var(--cims-brand-soft)' }}>
          <b>계획 절체 진행</b> — 상태 <code>{group.failover_op.state}</code>
          {` (${agentDisplayName(agents.find(a => a.id === group.failover_op!.source_agent_id)?.name || '?')}`}
          {` → ${agentDisplayName(agents.find(a => a.id === group.failover_op!.target_agent_id)?.name || '?')})`}
          {group.failover_op.note && <span className="text-muted-foreground"> · {group.failover_op.note}</span>}
          {group.failover_op.error && <span className="text-destructive"> · 오류: {group.failover_op.error}</span>}
        </div>
      )}
      {/* 컬럼 폭은 Figma G4(185:2874) 실측. **행 = 멤버 하나**이고 모듈은 셀 안에서 쌓인다 —
          모듈마다 행을 나누면 같은 멤버 안에도 가로줄이 생겨 "멤버 간 비교" 라는 이 표의
          목적이 흐려진다. */}
      <DataTable className="mt-2.5">
        <thead>
          <tr>
            <Th width={170}>서버</Th>
            <Th width={120}>서버 상태</Th>
            <Th width={200}>모듈 · 버전</Th>
            <Th width={110}>모듈 상태</Th>
            <Th width={222}>제어</Th>
          </tr>
        </thead>
        <tbody>
          {group.members.map(m => {
 const ag = agents.find(a => a.id === m.agent_id)
 const deps = (depsByAgent.get(m.agent_id) || []).filter(d => d.status !== 'removed')
 return (
              <tr key={m.agent_id}>
                <Td className="cursor-pointer align-top"
 onClick={() => onSelectMember(m.agent_id)}
 title="클릭 시 해당 서버 선택">
                  {agentDisplayName(ag?.name || `#${m.agent_id}`)}
                  {maintCtl(m.agent_id)}
                </Td>
                <Td className="align-top">
                  <StatusDot status={ag?.status || 'offline'} />
                </Td>
                {deps.length === 0 ? (
                  // ES-3 — 행은 남기고 이 셀만 문구로, 제어 버튼은 내지 않는다
                  <Td colSpan={3} className="align-top text-muted-foreground">배포된 모듈 없음</Td>
                ) : (
                  <>
                    <Td className="align-top">
                      <div className="flex flex-col gap-2.5">
                        {deps.map(d => (
                          <span key={d.id} className="flex h-[26px] items-center gap-1.5"
 title={d.process_name && d.process_name !== d.package_name
                                  ? `프로세스: ${d.process_name}` : undefined}>
                            {d.package_name}
                            <span className="font-mono text-sm text-muted-foreground">
 v{d.package_version}
                            </span>
                          </span>
                        ))}
                      </div>
                    </Td>
                    <Td className="align-top">
                      <div className="flex flex-col gap-2.5">
                        {deps.map(d => (
                          <span key={d.id} className="flex h-[26px] items-center">
                            <StatusDot tone={depTone(depEffectiveStatus(d))}
 label={depEffectiveStatus(d)} />
                          </span>
                        ))}
                      </div>
                    </Td>
                    <Td className="align-top">
                      <div className="flex flex-col gap-2.5">
                        {deps.map(d => (
                          <ProcessControlButtons key={d.id} dep={d} agent={ag} onJob={onJob} />
                        ))}
                      </div>
                    </Td>
                  </>
                )}
              </tr>
            )
          })}
        </tbody>
      </DataTable>
      <div className="mt-2.5 text-xs text-muted-foreground">
 uptime · PID · 메모리는 멤버 상세(트리에서 서버 선택)에서 확인합니다.
        그룹 화면은 멤버 간 상태 일치 여부에 집중합니다.
      </div>
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
 else show(`${label} — rc=${r.rc} ${r.stderr || r.stdout}`, 'err')
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
 else show(`${label} — rc=${r.rc} ${r.stderr || r.stdout}`, 'err')
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
    <div className="flex flex-col">
    {/* Level 2 접힘 섹션 5개 (Figma S1 49:1369) — IP/Routing · 라우팅 · 마운트 ·
        OAM 접속 주소 · 네트워크 튜닝. 제목에서 서버명을 뺐다: ContextBar 가 바로 위에서
        대상을 말하고 있어 매 섹션마다 반복할 이유가 없다.
        (IP/Routing 과 라우팅은 ServiceIpPanel 이 한 컴포넌트로 렌더한다 — 시안처럼 둘로
         쪼개려면 그 컴포넌트를 갈라야 해서 별도 단계로 둔다) */}
    <SubSection title="IP / Routing" count={(a.interfaces || []).length}
 hint="cims-managed 만 변경 가능 — 외부 IP / mgmt NIC 은 보호">
    <ServiceIpPanel
 title="" section="ip"
 interfaces={a.interfaces || []}
 storedRows={(a.service_ip_rows || []).map(r => ({ ...r }))}
 storedRoutes={a.routes || []}
 slots={[]}
 applying={applying}
 onApply={onApply}
 onUpdateSlot={onUpdateSlot}
 vipIps={vipIps}
    />
    </SubSection>
    <SubSection title="라우팅" count={(a.routes || []).length}
 hint="subnet 자동(kernel) 외 모두 변경 가능 — default gateway 포함">
    <ServiceIpPanel
 title="" section="routes"
 interfaces={a.interfaces || []}
 storedRows={(a.service_ip_rows || []).map(r => ({ ...r }))}
 storedRoutes={a.routes || []}
 slots={[]}
 applying={applying}
 onApply={onApply}
 onUpdateSlot={onUpdateSlot}
 vipIps={vipIps}
    />
    </SubSection>
    <SubSection title="마운트" count={(a.mounts || []).length}
 hint="콘솔에서 추가하면 /etc/fstab 에 기록되어 재부팅에도 유지 · 네트워크 FS 는 _netdev,nofail 자동">
    <MountPanel
 title=""
 mounts={a.mounts || []}
 applying={applying}
 onApply={onApplyMounts}
    />
    </SubSection>
    <SubSection title="OAM 접속 주소"
 hint="agent 가 heartbeat·job 결과를 보내는 주소 — 관리평면이 이중화면 VIP 여야 한다">
    <OamUrlPanel
 title=""
 current={a.oam_url}
 vipCandidate={mgmtVip}
 applying={applying}
 onApply={onApplyOamUrl}
 onApplyAll={onApplyOamUrlAll}
    />
    </SubSection>
    <SubSection title="네트워크 튜닝" hint="RPS / sysctl — 적용 후 부팅 시 재적용">
    <NetTuningPanel
 title=""
 agent={a}
 applying={applying}
 onApply={onApplyNetTuning}
    />
    </SubSection>
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
      <div className="text-md text-muted-foreground mb-2">
        대상 서버에서 다음 명령 실행 (ssh 1회) — systemd --user + linger 자동 (die 시 자동 재기동).
      </div>
      {loading && <div className="flex min-h-0 flex-1 items-center justify-center text-center text-muted-foreground p-[8px]">불러오는 중...</div>}
      {err && <div className="text-destructive mb-2">※ {err}</div>}
      {data && (
        <>
          <div className="relative">
            <pre style={{
 background: 'var(--muted)', color: 'var(--foreground)', padding: 12, paddingRight: 88,
 borderRadius: 4, fontSize: 12, whiteSpace: 'pre-wrap', margin: 0,
 opacity: expired ? 0.5 : 1,
            }}>{data.install_command}</pre>
            <Button className="absolute t-[8px] r-[8px]"
 onClick={copy} disabled={expired}>{copied ? <Check size={12} /> : <Copy size={12} />} 복사</Button>
          </div>
          <div className="flex items-center gap-3 mt-2">
            <div style={{ fontSize: 12, color: expired ? 'var(--destructive)' : 'var(--foreground)' }}>
              {expiresAt
                ? expired
                  ? <><AlertTriangle size={12} className="inline align-[-2px]" /> token 만료됨 ({expiresAt}) — 재발급 필요</>
                  : <>token 만료까지 약 <b>{minsLeft}분</b> (만료 시각: {expiresAt})</>
                : <>token 만료 시각 미상</>}
            </div>
            <Button className="ml-auto" onClick={regenerate} disabled={regenerating}>
              {regenerating ? '재발급 중...' : <><RotateCw size={13} /> 재발급</>}
            </Button>
          </div>
          <div className="text-xs text-muted-foreground mt-1.5">
            실행 후 <code>./init.sh</code> 로 sudoers + enrollment + systemd unit 일괄 설정 (sudo 비번 1회).
          </div>
        </>
      )}
    </div>
  )
}

/**
 * 정보 — **2열 그리드 12필드** (Figma S1 infoGrid 49:1477).
 * 열 393 · 열 사이 36 · 라벨 118 · 값 130 부터. 세로로 12줄을 늘어놓으면 아래 네트워크
 * 섹션이 한참 밀려 한 화면에 안 들어온다.
 *
 * IP · 버전 · 시각은 mono (DESIGN-RULES §1-5). 빈 값은 `—` + muted (§1-4).
 */
function InfoTab({ agent: a }: { agent: Agent }) {
 const left: [string, string, boolean?][] = [
 ['이름', a.name],
 ['호스트', a.hostname || '—', true],
 ['IP', a.ip_address || '—', true],
 ['OS', a.os_info || '—'],
 ['CPU 코어', a.cpu_cores ? `${a.cpu_cores}` : '—'],
 ['메모리', a.memory_mb ? `${Math.round(a.memory_mb / 1024)} GB` : '—'],
  ]
 const right: [string, string, boolean?][] = [
 ['디스크', a.disk_gb ? `${a.disk_gb} GB` : '—'],
 ['Agent 버전', a.agent_version || '—', true],
 ['등록 시각', a.enrolled_at || '—', true],
 ['승인 시각', a.approved_at || '—', true],
 ['마지막 heartbeat', a.last_heartbeat ? `${a.last_heartbeat} (${fmtRelTime(a.last_heartbeat)})` : '—', true],
 ['메모', a.note || '—'],
  ]
 return (
    <div className="grid grid-cols-1 gap-x-9 xl:grid-cols-2">
      {[left, right].map((col, i) => (
        <div key={i} className="flex flex-col">
          {col.map(([label, value, mono]) => (
            <Field key={label} label={label} value={value} mono={mono} />
          ))}
        </div>
      ))}
    </div>
  )
}

function Field({ label, value, mono }: { label: string; value: string; mono?: boolean }) {
 const empty = value === '—'
 return (
    <div className="flex min-h-8 items-baseline gap-3 py-1">
      <span className="w-[118px] shrink-0 text-md text-muted-foreground">{label}</span>
      <span className={`min-w-0 break-all text-md ${mono ? 'font-mono' : ''} ${
 empty ? 'text-muted-foreground' : ''}`}>{value}</span>
    </div>
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
        <Input value={name} disabled={busy}
 onChange={e => setName(e.target.value)} />
        <label style={{ gridColumn: '1 / -1', borderTop: '1px solid var(--border)',
 paddingTop: 10, marginTop: 4 }}>
          <Checkbox  checked={mountOn} disabled={busy} onCheckedChange={(c) => setMountOn((c === true))} />
          {' '}공유 스토리지 마운트를 함께 적용
          <span className="text-xs text-muted-foreground">
            {' '}— 서버 등록 직후 자동으로 붙습니다 (fstab 영속)
          </span>
        </label>
        {mountOn ? (
          <>
            <label>원본 *</label>
            <Input value={mnt.source} disabled={busy}
 placeholder="예: nas.example:/export/cims"
 onChange={e => setMnt(m => ({ ...m, source: e.target.value }))} />
            <label>붙일 위치 *</label>
            <Input value={mnt.target} disabled={busy}
 placeholder="/mnt/cims"
 onChange={e => setMnt(m => ({ ...m, target: e.target.value }))} />
            <label>파일시스템 *</label>
            <Select value={toSel(mnt.fstype)} onValueChange={(v: string) => setMnt(m => ({ ...m, fstype: fromSel(v) }))} disabled={busy}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                {['nfs4', 'nfs', 'cifs', 'ext4', 'ext3', 'xfs', 'btrfs'].map(f => (
                  <SelectItem key={f} value={f}>{f}</SelectItem>
                ))}
              </SelectContent>
            </Select>
            <label style={{ gridColumn: '1 / -1', fontSize: 11,
 color: mntValid ? 'var(--muted-foreground)' : 'var(--destructive)' }}>
              {mntValid
                ? <>{fromGroup && <>기본값은 <b>이 그룹의 마운트 선언</b>입니다. </>}
                   등록 직후 <code>{mnt.source}</code> → <code>{mnt.target}</code> ({mnt.fstype},
 defaults+_netdev,nofail) 로 마운트되고 [마운트 관리]에 표시됩니다.
                   실패해도 서버 등록은 유지됩니다.</>
                : <>원본과 붙일 위치(절대경로)를 입력하세요.</>}
            </label>
          </>
        ) : (
          <label style={{ gridColumn: '1 / -1', fontSize: 11, color: 'var(--cims-warning)' }}>
            이 서버는 <b>마운트 없이</b> 등록됩니다 — 공유 store·서비스 로그를 쓰는 모듈이라면
            나중에 [마운트 관리]에서 직접 추가해야 합니다.
          </label>
        )}
      </div>
      <div className="flex justify-end gap-2.5 pt-5 mt-4">
        <Button size="default" onClick={onClose} disabled={busy}>취소</Button>
        <Button variant="default" size="default" disabled={busy || !name.trim() || (mountOn && !mntValid)}
 onClick={async () => {
 setBusy(true)
 try {
 await onSubmit(name.trim(), mountOn
                      ? [{ ...mnt, target: mnt.target.trim().replace(/\/+$/, '') }]
                      : []) // [] = 마운트하지 않음(명시) — 그룹 선언 상속 안 함
                  } finally { setBusy(false) }
                }}>
          {busy ? '추가 중…' : '추가'}
        </Button>
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
      <div className="mb-2.5 text-success">
        <Check size={13} className="inline align-[-2px]" /> <b>{info.serverName}</b> 그룹 멤버로 등록됨. 다음 명령을 대상 서버에서 실행:
      </div>
      {/* 직전 단계에서 확정한 마운트를 되짚어 보여준다 — 설치 명령을 돌리기 전에
          "이 서버는 마운트가 되는가" 가 화면에 남아 있어야 한다. */}
      {info.appliedMounts?.length ? (
        <div className="text-sm text-muted-foreground mb-2.5 leading-[1.6]">
          등록 직후 자동 마운트: {info.appliedMounts.map(m =>
            <code key={m.target}>{m.source} → {m.target} ({m.fstype})</code>)
            .reduce((a, b) => <>{a}, {b}</>)}
        </div>
      ) : (
        <div className="text-sm text-warning mb-2.5 leading-[1.6]">
          이 서버는 <b>마운트 없이</b> 등록됩니다 — 필요하면 [마운트 관리]에서 추가하세요.
        </div>
      )}
      <div className="relative">
        <pre className="bg-muted text-foreground p-3 pr-[88px] rounded-sm text-sm whitespace-pre-wrap m-0">{info.install_command}</pre>
        <Button className="absolute t-[8px] r-[8px]"
 onClick={copy}>{copied ? <Check size={12} /> : <Copy size={12} />} 복사</Button>
      </div>
      <div className="text-xs text-muted-foreground mt-1.5">
 token: <code>{info.enrollment_token}</code>
      </div>
      <div className="flex justify-end gap-2.5 pt-5 mt-4">
        <Button variant="default" size="default" onClick={onClose}>닫기</Button>
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
    // 시안 M3(460:7421) — 세로 폼(라벨 위·헬프 아래), 마운트 블록은 구분선 아래 조건부.
    <Modal title="시스템 추가" onClose={onClose} width={660}>
      {!results ? (
        <div className="flex flex-col gap-3.5">
          <FormField label="이름" required help="트리와 대시보드에 표시되는 이름 (예: Control-Server)">
            <Input value={name} placeholder="예: Control-Server"
 onChange={e => setName(e.target.value)} disabled={creating} />
          </FormField>
          <FormField label="유형" required>
            <Select value={toSel(mode)} onValueChange={(v: string) => setMode(fromSel(v) as SystemMode)} disabled={creating}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="active_standby">AS — Active/Standby (master + backup 2서버 자동)</SelectItem>
                <SelectItem value="all_active">AA — All Active (다중화, 그룹만 생성 + 이후 멤버 추가)</SelectItem>
                <SelectItem value="standalone">Standalone — 단일 서버 (HA 그룹 없음)</SelectItem>
              </SelectContent>
            </Select>
          </FormField>
          {mode === 'active_standby' && (
            <>
              <FormField label="auth_pass" required help="VRRP 인증 비밀번호 — 멤버 간 동일 (최대 8글자)">
                <Input value={authPass} type="password"
 onChange={e => setAuthPass(e.target.value)} disabled={creating} maxLength={8} />
              </FormField>
              {[0, 1].map(i => (
                <FormField key={i} label={`멤버 ${i + 1} (${i === 0 ? 'master' : 'backup'})`}>
                  <Select value={String(memberSel[i])} onValueChange={(v: string) => setMemberSel(prev => {
 const next: [number, number] = [...prev] as [number, number]
 next[i] = Number(v)
 return next
                    })} disabled={creating}>
                    <SelectTrigger><SelectValue /></SelectTrigger>
                    <SelectContent>
                      <SelectItem value="0">신규 서버 생성 — {name || '<이름>'}-{String(i + 1).padStart(2, '0')}</SelectItem>
                      {saAgents.map(a => (
                        <SelectItem key={a.id} value={String(a.id)} disabled={memberSel[1 - i] === a.id}>
                          기존 서버 편입: {agentDisplayName(a.name)} ({a.status}{a.hostname ? ` · ${a.hostname}` : ''})
                        </SelectItem>
                      ))}
                    </SelectContent>
                  </Select>
                </FormField>
              ))}
            </>
          )}
          {/* 공유 마운트 — 등록 직후 자동 적용. 마운트를 별도 작업으로 두면 운영자가 잊고,
              그 노드는 공유 store 를 못 써 승격 부적격이 된다(실측: 계획 절체가 원본을
              내려놓은 뒤에야 드러나 관리평면이 끊겼다). 기본값은 이 설치가 이미 쓰는 마운트. */}
          {showMount && (
            <label className="mt-1 flex cursor-pointer select-none items-start gap-2 border-t border-border pt-3.5">
              <Checkbox checked={mountOn} disabled={creating}
 onCheckedChange={v => setMountOn(v === true)} className="mt-0.5" />
              <span className="text-md">
                공유 스토리지 마운트를 함께 적용
                <span className="text-xs text-muted-foreground">
                  {' '}— 서버 등록 직후 자동으로 붙입니다 (fstab 영속)
                </span>
              </span>
            </label>
          )}
          {showMount && mountOn && (
            <>
              <FormField label="원본" required>
                <Input className="font-mono" value={mnt.source} disabled={creating}
 placeholder="예: nas.example:/export/cims"
 onChange={e => setMnt(m => ({ ...m, source: e.target.value }))} />
              </FormField>
              <FormField label="붙일 위치" required>
                <Input className="font-mono" value={mnt.target} disabled={creating}
 placeholder="/mnt/cims"
 onChange={e => setMnt(m => ({ ...m, target: e.target.value }))} />
              </FormField>
              <FormField label="파일시스템" required
 help={mntValid
                           ? `등록 직후 ${mnt.source} → ${mnt.target} (${mnt.fstype}, defaults,_netdev,nofail) 로 마운트하고 콘솔 [마운트 관리]에 표시됩니다. 마운트가 실패해도 서버 등록은 유지됩니다.`
                           : undefined}
 error={mntValid ? undefined
                           : '원본과 붙일 위치(절대경로)를 입력하세요 — 비우면 마운트를 적용하지 않습니다.'}>
                <Select value={toSel(mnt.fstype)} onValueChange={(v: string) => setMnt(m => ({ ...m, fstype: fromSel(v) }))} disabled={creating}>
                  <SelectTrigger><SelectValue /></SelectTrigger>
                  <SelectContent>
                    {['nfs4', 'nfs', 'cifs', 'ext4', 'ext3', 'xfs', 'btrfs'].map(f => (
                      <SelectItem key={f} value={f}>{f}</SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </FormField>
            </>
          )}
          {/* 선택 요약 — 시안은 회색 박스 한 줄로 두고, 고른 값에 따라 실시간으로 바뀐다 */}
          <div className="rounded-md bg-muted px-3 py-2 text-xs text-muted-foreground">
            선택: <b>{modeLabel}</b>
            {mode === 'active_standby' && memberSel.every(v => v === 0) &&
              <> · 멤버 이름: <code className="font-mono">{name || '<이름>'}-01</code> (master),{' '}
                 <code className="font-mono">{name || '<이름>'}-02</code> (backup)</>}
            {mode === 'active_standby' && memberSel.some(v => v > 0) &&
              <> · 기존 서버는 install-command 없이 즉시 편입되고 HA 설정이 자동 재적용됩니다</>}
            {mode === 'all_active' &&
              <> · 서버는 생성되지 않습니다 — 그룹 생성 후 <b>[+ 멤버 추가]</b> 로 한 대씩
                 추가하고, <b>마운트는 그 단계에서</b> 선택합니다</>}
          </div>
        </div>
      ) : results.length === 0 ? (
        <Alert variant="success">
          {mode === 'active_standby'
            ? '기존 서버들로 A/S 시스템 구성 완료 — 트리에서 그룹을 선택해 VIP 를 설정하세요.'
            : 'AA 그룹 생성됨. 좌측 트리에서 그룹 선택 후 [+ 멤버 추가] 로 서버를 추가하세요.'}
        </Alert>
      ) : (
        <div>
          <Alert variant="success" className="mb-2.5">
            {results.length} 서버 등록됨. 각 서버에서 다음 명령 실행:
          </Alert>
          {results.map((r, i) => (
            <div key={i} className="mb-3">
              <div className="mb-1 text-md font-semibold">{r.name}</div>
              <div className="relative">
                <pre className="m-0 whitespace-pre-wrap rounded-sm border border-border bg-muted p-3 pr-24 font-mono text-sm">{r.install_command}</pre>
                <Button variant="outline" className="absolute right-2 top-2" onClick={() => copyCmd(i)}>
                  {copiedIdx === i ? <Check /> : <Copy />} 복사
                </Button>
              </div>
              <div className="mt-1 text-xs text-muted-foreground">
 token: <code className="font-mono">{r.enrollment_token}</code>
              </div>
            </div>
          ))}
        </div>
      )}
      <div className="mt-4 flex justify-end gap-1.5">
        {!results ? (
          <>
            <Button variant="outline" onClick={onClose} disabled={creating}>취소</Button>
            <Button variant="default" onClick={create} disabled={creating || !name.trim()}>
              {creating ? '생성 중…' : '생성'}
            </Button>
          </>
        ) : (
          <Button variant="default" onClick={onClose}>닫기</Button>
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
 const confirm = useConfirm()
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
 if (await confirm({ title: '순서 가드 우회', tone: 'danger', confirmLabel: '강행', body: <>
          {(e as Error).message}
          <div className="mt-2">그래도 강행할까요? (순서 가드 우회)</div>
        </> })) {
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
      <div className="text-md mb-3">
        <div className="text-muted-foreground">
          {d.process_name} · 현재 <b>v{d.package_version}</b>
        </div>
      </div>
      {cands.length === 0 ? (
        <EmptyState title="등록된 다른 버전이 없습니다 — [관리 &gt; 릴리스] 에 먼저 업로드하세요." />
      ) : (
        <>
          <label className="block text-sm mb-1">올릴 버전</label>
          <Select value={String(pkgId)} onValueChange={(v: string) => setPkgId(Number(v))}>
            <SelectTrigger className="input w-full"><SelectValue /></SelectTrigger>
            <SelectContent>
              {cands.map((p, i) => (
                <SelectItem key={p.id} value={String(p.id)}>
 v{p.version}{i === 0 ? '  (최신 업로드)' : ''}
                  {p.uploaded_at ? `  — ${fmtRelTime(p.uploaded_at)}` : ''}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          <div className="text-sm text-muted-foreground mt-2.5 leading-[1.7]">
            · 파일만 설치되고 <b>자동으로 시작하지 않습니다</b> — 확인 후 [패키지 제어] 에서 시작하세요.<br />
            · 설정은 이관됩니다(collection + 배포 설정). 새 항목은 기본값.<br />
            · 구 버전(v{d.package_version})은 보존되어 곧바로 <b>롤백</b> 할 수 있습니다.
          </div>
        </>
      )}
      <div className="mt-4 flex gap-2 justify-end">
        <Button onClick={onClose}>취소</Button>
        <Button variant="default" disabled={!target || busy}
 onClick={() => run()}>
          {busy ? '진행 중…' : target ? `v${target.version} 로 업그레이드` : '업그레이드'}
        </Button>
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
        <Alert variant="info" className="mb-2">
          이 agent 는 HA 그룹 <b>{agent.ha_group.name}</b> (mode={agent.ha_group.mode}, role={agent.ha_group.role}) 소속 —
          {' '}<b>{agent.ha_group.mode}</b> 가능 모듈 + standalone 모듈만 install 가능
        </Alert>
      )}
      <div className="form-grid">
        <label>1. 모듈 *</label>
        <Select value={toSel(moduleName)} onValueChange={(v: string) => setModuleName(fromSel(v))}>
          <SelectTrigger><SelectValue /></SelectTrigger>
          <SelectContent>
            <SelectItem value={NONE}>(선택)</SelectItem>
            {moduleNames.map(m => {
 const mm = moduleMismatch(m)
 return (
                <SelectItem key={m} value={m} disabled={!!mm}>
                  {m} ({pkgsByModule.get(m)!.length}개 버전){mm ? ` — ${mm}` : ''}
                </SelectItem>
              )
            })}
          </SelectContent>
        </Select>

        <label>2. 버전 *</label>
        <Select value={String(pkgId)} onValueChange={(v: string) => setPkgId(Number(v))} disabled={!moduleName}>
          <SelectTrigger><SelectValue /></SelectTrigger>
          <SelectContent>
            <SelectItem value="0">(선택)</SelectItem>
            {versions.map((p, i) => (
              <SelectItem key={p.id} value={String(p.id)}>
 v{p.version}{i === 0 ? '  (최신)' : ''}
              </SelectItem>
            ))}
          </SelectContent>
        </Select>

        {selectedPkg && (
          <>
            <label>3. 모듈 이름 *</label>
            {processOptions.length > 1 ? (
              <Select value={toSel(processName)} onValueChange={(v: string) => setProcessName(fromSel(v))}>
                <SelectTrigger><SelectValue /></SelectTrigger>
                <SelectContent>
                  {processOptions.map(p => <SelectItem key={p} value={p}>{p}</SelectItem>)}
                </SelectContent>
              </Select>
            ) : (
              <Input value={processName}
 onChange={e => setProcessName(e.target.value)}
 placeholder={selectedPkg.name.toUpperCase()} />
            )}

            <label>4. 설명</label>
            <div className="border border-border rounded-sm p-2 text-md text-foreground whitespace-pre-wrap min-h-[36px]">
              {selectedPkg.description
                ? selectedPkg.description
                : <span className="text-muted-foreground text-sm">(패키지에 설명 없음)</span>}
            </div>
          </>
        )}

        <label>메모</label>
        <Input value={note} onChange={e => setNote(e.target.value)} />
      </div>
      <div className="mt-3 text-sm text-muted-foreground">
        ℹ 추가 후 <b>pending</b> 상태로 생성됩니다. 설정을 확인한 뒤
        <b>설치</b> → <b>Start</b> 순으로 진행하세요.
      </div>
      {selectedMismatch && (
        <div style={{ marginTop: 8, fontSize: 12, color: 'var(--destructive)',
 padding: '6px 10px', background: 'var(--cims-danger-soft)', border: '1px solid var(--cims-danger-soft)',
 borderRadius: 4 }}>
          <AlertTriangle size={13} className="inline align-[-2px]" /> {selectedMismatch} — install 시 backend 400 reject
        </div>
      )}
      <div className="flex justify-end gap-2.5 pt-5 mt-4">
        <Button size="default" onClick={onClose}>취소</Button>
        <Button variant="default" size="default" onClick={create}
 disabled={!!selectedMismatch}>추가</Button>
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
    // 시안 M1(193:3205) — 카드 3장 + 표 + 하단 안내문. **푸터 버튼이 없다**(머리의 ✕ 로 닫는다).
    <Modal title={`${agentDisplayName(agent.name)} — 메트릭 (최근 ${metrics.length}건)`}
 onClose={onClose} width={1000}>
      {chrono.length >= 2 && (
        <div className="mb-4 flex gap-2.5">
          <MetricTrend label="CPU" values={chrono.map(m => m.cpu_pct)}
 color="var(--cims-info)" warn={85} />
          <MetricTrend label="MEM" values={chrono.map(m => m.mem_pct)}
 color="var(--cims-success)" warn={90} />
          <MetricTrend label="Disk" values={chrono.map(m => m.disk_pct)}
 color="var(--primary)" warn={90} />
        </div>
      )}
      <DataTable>
        <thead>
          <tr>
            <Th width={230}>시각</Th>
            <Th width={110}>CPU%</Th>
            <Th width={110}>MEM%</Th>
            <Th width={110}>DISK%</Th>
            <Th width={200}>LOAD</Th>
            <Th>CIMS 프로세스</Th>
          </tr>
        </thead>
        <tbody>
          {metrics.length === 0 && (
            <tr><Td colSpan={6} className="text-muted-foreground">메트릭 없음 — heartbeat 대기</Td></tr>
          )}
          {metrics.map(m => (
            <tr key={m.ts}>
              <Td mono>{m.ts}</Td>
              <Td>{orDash(m.cpu_pct)}</Td>
              <Td>{orDash(m.mem_pct)}</Td>
              <Td>{orDash(m.disk_pct)}</Td>
              <Td mono>{orDash(m.load_avg)}</Td>
              <Td mono className="text-muted-foreground">
                {m.processes.length === 0 ? '—'
                  : m.processes.map(p => `${p.name}(${p.pid})`).join(', ')}
              </Td>
            </tr>
          ))}
        </tbody>
      </DataTable>
      <div className="mt-2.5 text-xs text-muted-foreground">
        세로 스크롤 · 최근 {metrics.length}건 · 상단 카드의 붉은 점선은 임계치
      </div>
    </Modal>
  )
}

