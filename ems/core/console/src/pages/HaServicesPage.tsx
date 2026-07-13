/**
 * HaServicesPage — "서비스(=HA 그룹/단독) → 서버" 트리 list.
 *
 * 한 페이지에서 그룹(서비스) 정의 + 서버 자동 발급 + 패키지 일괄 설치 모두 inline 편집.
 * 팝업 없음 — 모든 추가/편집은 행 안에서 expand.
 *
 * 데이터 모델 매핑:
 *   ServiceRow ↔ HaGroup (mode=active_standby|all_active) — 또는 standalone agent (id=-agent.id)
 *   ServerRow  ↔ Agent
 *   PkgDef     ↔ SipPackage (config_template ip_slots — 현재 패키지 이름 기반 hardcoded 매핑)
 *   NetIface[] ← Agent.interfaces (heartbeat 보고)
 *   ServiceIpRow[]  ← Agent.service_ip_rows (운영자 설정, PUT /agents/{id})
 *   VipBinding[]    ← HaGroup.vip_bindings   (운영자 설정, PUT /ha-groups/{id})
 *   packageIds      ← deployments.filter(...).map(d => d.package_id) (실배포 = 의도)
 */
import { useCallback, useEffect, useMemo, useState } from 'react'
import { Link, useNavigate, useSearchParams } from 'react-router-dom'
import { haGroupsApi, type HaGroup } from '../api/ha_groups'
import { deploymentApi, type Agent, type SipPackage, type Deployment, type AgentMetric,
         type ServiceIpRow as ApiServiceIpRow } from '../api/deployment'
import { GroupConfigCompareView } from '../components/group/GroupConfigCompareView'
import Modal from '../components/Modal'
import HealthCheckModal from '../components/HealthCheckModal'
import MetricTrend from '../components/MetricTrend'
import { summarizeApplyResult } from './ha/helpers'
import { ServiceIpPanel } from './ha/ServiceIpPanel'
import { VipPanel } from './ha/VipPanel'
import { btnSmall, btnPrimary, btnSecondary, btnDanger, btnAdd } from './ha/styles'
import type {
  Mode, Role, ServerStatus, IpSlot, ServiceIpRow, ServiceRow, ServerRow,
} from './ha/types'

// ──────────────────────────────────────────────────────────────
//  Types (page-specific — 공유 type/패널/스타일 은 ./ha/*)
// ──────────────────────────────────────────────────────────────

type Capability = Mode

interface PkgDef {
  id: number
  name: string
  version: string
  description: string
  capability: Capability
  ipSlots: IpSlot[]
}

// 패키지 이름 → IP slot 매핑 (config_template ip_slot 메타데이터 도입 전 임시 hardcoded).
// 실제 운영자는 ServiceIpPanel 에서 자유 입력 가능 (이 매핑은 hint 용).
const SLOT_MAP: Record<string, IpSlot[]> = {
  csc:     [
    { scope: 'service', name: 'Admin', port: 4421, proto: 'tcp' },
    { scope: 'service', name: 'McPTT', port: 4430, proto: 'tcp' },
    { scope: 'vip',     name: 'Admin', port: 4421, proto: 'tcp' },
  ],
  csp:     [
    { scope: 'service', name: 'SIP',   port: 5060, proto: 'udp' },
    { scope: 'service', name: 'Admin', port: 4421, proto: 'tcp' },
    { scope: 'service', name: 'Stats', port: 9000, proto: 'udp' },
    { scope: 'vip',     name: 'SIP',   port: 5060, proto: 'udp' },
  ],
  psp:     [
    { scope: 'service', name: 'SIP',   port: 5060, proto: 'udp' },
    { scope: 'vip',     name: 'SIP',   port: 5060, proto: 'udp' },
  ],
  cmp:     [
    { scope: 'service', name: 'RTP',     port: 50000, proto: 'udp' },
    { scope: 'service', name: 'Control', port: 9000,  proto: 'udp' },
  ],
  pmp:     [
    { scope: 'service', name: 'RTP',     port: 52000, proto: 'udp' },
    { scope: 'service', name: 'Floor',   port: 54000, proto: 'udp' },
    { scope: 'service', name: 'Control', port: 9001,  proto: 'udp' },
  ],
  cwrtc:   [{ scope: 'service', name: 'WS',    port: 8443, proto: 'tcp' }],
  console: [{ scope: 'service', name: 'HTTPS', port: 8081, proto: 'tcp' }],
  phone:   [{ scope: 'service', name: 'HTTPS', port: 3002, proto: 'tcp' }],
}

// config_template.json 에 ip_scope/ip_slot 메타가 있으면 우선, 없으면 hardcoded SLOT_MAP fallback
function extractIpSlotsFromTemplate(p: SipPackage): IpSlot[] {
  const tpl = p.config_template
  if (!tpl?.sections) return []
  const slots: IpSlot[] = []
  const seen = new Set<string>()
  for (const sec of tpl.sections) {
    for (const f of sec.fields ?? []) {
      if (!f.ip_scope || !f.ip_slot) continue
      const key = `${f.ip_scope}:${f.ip_slot}:${f.ip_proto ?? ''}:${f.ip_port ?? ''}`
      if (seen.has(key)) continue
      seen.add(key)
      slots.push({
        scope: f.ip_scope,
        name:  f.ip_slot,
        port:  f.ip_port,
        proto: f.ip_proto,
      })
    }
  }
  return slots
}

function pkgToDef(p: SipPackage): PkgDef {
  const fromTemplate = extractIpSlotsFromTemplate(p)
  return {
    id: p.id,
    name: p.name,
    version: p.version,
    description: p.description ?? '',
    capability: (p.meta?.ha_capability as Capability) ?? 'standalone',
    ipSlots: fromTemplate.length > 0 ? fromTemplate : (SLOT_MAP[p.name] ?? []),
  }
}

// ──────────────────────────────────────────────────────────────
//  Helpers
// ──────────────────────────────────────────────────────────────

const MODE_LABEL: Record<Mode, string> = {
  active_standby: 'AS',
  all_active:     'AA',
  standalone:     'SA',
}

const MODE_TOOLTIP: Record<Mode, string> = {
  active_standby: 'Active/Standby — primary 1명 + standby 1명. VIP fail-over 자동 전환.',
  all_active:     'All-Active — N개 동등 멤버 동시 처리 (load balancing 분배).',
  standalone:     'Standalone — 단일 서버 (이중화 없음).',
}

const MODE_COLOR: Record<Mode, string> = {
  active_standby: '#3498db',
  all_active:     '#27ae60',
  standalone:     '#95a5a6',
}

const STATUS_COLOR: Record<ServerStatus, string> = {
  pending: '#f39c12',
  online:  '#27ae60',
  offline: '#c0392b',
}

const STATUS_ICON: Record<ServerStatus, string> = {
  pending: '⏳',
  online:  '●',
  offline: '○',
}

function pad2(n: number): string {
  return n.toString().padStart(2, '0')
}

function agentToServer(a: Agent, role: Role, vipObserved?: boolean | null): ServerRow {
  const status: ServerStatus =
    a.status === 'online' ? 'online'
    : a.status === 'offline' || a.status === 'error' ? 'offline'
    : 'pending'
  return {
    id: a.id,
    name: a.name,
    role,
    vipObserved,
    ip: a.ip_address,
    status,
    agent_version: a.agent_version,
    token: '',                                          // pending 만 set, AgentCreateResult.enrollment_token
    expiresAt: a.enrollment_token_expires_at,
    interfaces: a.interfaces ?? [],
    serviceIpRows: (a.service_ip_rows ?? []) as ServiceIpRow[],
    routes: a.routes ?? [],
  }
}

// 실측 VIP 보유 뱃지 (R4, AS 만) — 정적 role(우선순위 설정)과 달리 절체를 반영
// (agent heartbeat interfaces[] 관측, ≤30s 지연). null(판정 불가)은 미표시.
function VipObservedBadge({ v, size = 9 }: { v?: boolean | null; size?: number }) {
  if (v === true) return (
    <span title="실측 ACTIVE — 현재 VIP 보유 (heartbeat 관측)"
          style={{ fontSize: size, padding: '0 4px', borderRadius: 2,
                   background: '#27ae60', color: '#fff', fontWeight: 700 }}>
      ● ACT
    </span>
  )
  if (v === false) return (
    <span title="실측 STANDBY — VIP 미보유 (heartbeat 관측)"
          style={{ fontSize: size, padding: '0 4px', borderRadius: 2,
                   border: '1px solid #95a5a6', color: '#7f8c8d' }}>
      ○ SBY
    </span>
  )
  return null
}

// ── 토큰 만료 헬퍼 ──
function isTokenValid(expiresAt: string | null): boolean {
  if (!expiresAt) return false
  return new Date(expiresAt).getTime() > Date.now()
}

function minutesLeft(expiresAt: string | null): number {
  if (!expiresAt) return 0
  return Math.max(0, Math.ceil((new Date(expiresAt).getTime() - Date.now()) / 60000))
}

function buildInstallCommand(token: string, name: string, role: Role): string {
  const r = role ? ` --role ${role}` : ''
  // name 에 space/특수문자 포함 가능 → 큰따옴표 quote
  const quotedName = `"${name.replace(/(["\\$`])/g, '\\$1')}"`
  // 서버 발급(install_command) fallback — install-agent.sh 는 OAM(4419) 이 서빙.
  return `curl -fsSLk https://OAM:4419/install-agent.sh | bash -s -- \\
  --oam-url https://OAM:4419 \\
  --enrollment-token ${token} \\
  --name ${quotedName}${r}`
}

// navigator.clipboard 는 secure context (HTTPS / localhost) 에서만 동작.
// HTTP dev 환경 (예: http://192.168.x.x:3000) 에서는 execCommand fallback 사용.
async function copyToClipboard(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text)
      return true
    }
  } catch {
    /* fall through */
  }
  try {
    const ta = document.createElement('textarea')
    ta.value = text
    ta.setAttribute('readonly', '')
    ta.style.position = 'fixed'
    ta.style.left = '-9999px'
    document.body.appendChild(ta)
    ta.select()
    const ok = document.execCommand('copy')
    document.body.removeChild(ta)
    return ok
  } catch {
    return false
  }
}

// ──────────────────────────────────────────────────────────────
//  Page
// ──────────────────────────────────────────────────────────────

export default function HaServicesPage() {
  const [searchParams] = useSearchParams()
  const navigate = useNavigate()
  // ServersPage 의 [📋 상세 편집] 진입점 — ?group=<id> 로 자동 시스템 선택.
  const initialGroupId = (() => {
    const q = searchParams.get('group')
    const n = q ? Number(q) : NaN
    return Number.isFinite(n) && n > 0 ? n : null
  })()

  const [haGroups, setHaGroups] = useState<HaGroup[]>([])
  const [agents, setAgents] = useState<Agent[]>([])
  const [packages, setPackages] = useState<SipPackage[]>([])
  const [deployments, setDeployments] = useState<Deployment[]>([])
  const [loading, setLoading] = useState(true)
  const [err, setErr] = useState<string>('')

  // 새 Master-Detail layout — 선택된 (시스템, 멤버) id. 멤버 id null = 시스템 view, 값 = 멤버 view.
  const [selectedSvcId, setSelectedSvcId] = useState<number | null>(initialGroupId)
  const [selectedSrvId, setSelectedSrvId] = useState<number | null>(null)
  // 좌측 트리에서 펼쳐진 시스템 id set. AS/AA 만 의미 (Standalone 은 멤버 sub-node 없음).
  const [treeExpanded, setTreeExpanded] = useState<Set<number>>(new Set())
  const [adding, setAdding] = useState<{ name: string; mode: Mode | '' } | null>(null)
  const [editingName, setEditingName] = useState<{ kind: 'service' | 'server'; id: number; value: string } | null>(null)
  const [pkgPickerFor, setPkgPickerFor] = useState<number | null>(null)
  const [configModalFor, setConfigModalFor] = useState<ServiceRow | null>(null)
  const [healthCheckGroup, setHealthCheckGroup] = useState<ServiceRow | null>(null)
  const [toast, setToast] = useState<string | null>(null)
  // pending agent 신규 생성 직후 1회용 enrollment_token + install command
  const [pendingTokens, setPendingTokens] = useState<Map<number, { token: string; cmd: string }>>(new Map())
  // applyServiceIp 진행 중인 agent.id 집합 — ServiceIpPanel row 가 'applying' 표시
  const [applyingAgents, setApplyingAgents] = useState<Set<number>>(new Set())
  // 1분마다 re-render 강제 — 만료시간 카운트다운 갱신용
  const [, setMinuteTick] = useState(0)

  const flash = (msg: string) => { setToast(msg); window.setTimeout(() => setToast(null), 2000) }

  const load = useCallback(async () => {
    setErr('')
    try {
      const [g, a, p, d] = await Promise.all([
        haGroupsApi.list(),
        deploymentApi.listAgents(),
        deploymentApi.listPackages(),
        deploymentApi.listDeployments(),
      ])
      setHaGroups(g); setAgents(a); setPackages(p); setDeployments(d)
    } catch (e) {
      setErr(String((e as Error).message ?? e))
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => { void load() }, [load])
  useEffect(() => {
    const iv = setInterval(() => void load(), 10_000)
    return () => clearInterval(iv)
  }, [load])
  // 토큰 카운트다운 1분 단위 re-render
  useEffect(() => {
    const iv = setInterval(() => setMinuteTick(t => t + 1), 60_000)
    return () => clearInterval(iv)
  }, [])

  const packageMap = useMemo(() => new Map(packages.map(p => [p.id, pkgToDef(p)])), [packages])
  const agentMap = useMemo(() => new Map(agents.map(a => [a.id, a])), [agents])

  // 서비스(=HA 그룹 + standalone agents) 빌드
  const services: ServiceRow[] = useMemo(() => {
    const out: ServiceRow[] = []
    // HA 그룹 → service
    for (const g of haGroups) {
      const members = g.members.slice().sort((a, b) => b.priority - a.priority)
      const servers = members.map(m => {
        const a = agentMap.get(m.agent_id)
        if (!a) return null
        return agentToServer(a, g.mode === 'active_standby' ? m.role : null,
                             g.mode === 'active_standby' ? (m.vip_observed ?? null) : undefined)
      }).filter((s): s is ServerRow => s !== null)
      const agentIds = new Set(members.map(m => m.agent_id))
      const groupDeps = deployments.filter(d => agentIds.has(d.agent_id))
      const pkgIds = Array.from(new Set(groupDeps.map(d => d.package_id)))
      out.push({
        id: g.id,
        name: g.name,
        mode: g.mode,
        vrid: g.vrid,
        vip: g.vip ?? '',
        vipMask: g.vip_mask,
        authPass: g.auth_pass,
        servers,
        packageIds: pkgIds,
        // 백엔드 vip_bindings 에는 bid 가 없음(slot/ip/mask 만) → 로딩 시 안정적 bid 부여.
        // 누락 시 모든 row 의 bid 가 undefined 가 되어 removeRow(filter by bid) 가 전체를 지움.
        vipBindings: (g.vip_bindings ?? []).map((b, i) => ({ ...b, bid: b.bid ?? i + 1 })),
      })
    }
    // standalone agents (ha_group 미배정)
    for (const a of agents) {
      if (a.ha_group) continue
      const deps = deployments.filter(d => d.agent_id === a.id)
      const pkgIds = Array.from(new Set(deps.map(d => d.package_id)))
      out.push({
        id: -a.id,                          // 음수 = standalone marker
        name: a.name,
        mode: 'standalone',
        vrid: null,
        vip: '',
        vipMask: 24,
        authPass: '',
        servers: [agentToServer(a, null)],
        packageIds: pkgIds,
        vipBindings: [],
      })
    }
    return out
  }, [haGroups, agents, deployments, agentMap])

  // 선택된 시스템 객체 — services 변경 후에도 같은 id 유지. 없거나 services 로드 직후면 첫 항목.
  const selectedSvc = useMemo(() => {
    if (services.length === 0) return null
    return services.find(s => s.id === selectedSvcId) ?? services[0]
  }, [services, selectedSvcId])

  // services 가 처음 로드되면 자동 선택 + tree expand.
  // initialGroupId (URL ?group=<id>) 가 있으면 해당 시스템 선택, 없으면 첫 항목.
  useEffect(() => {
    if (services.length === 0) return
    // 선택된 id 가 services 에 존재하지 않으면 reset (predicate 별 fallback).
    const wanted = selectedSvcId
    const found = wanted != null ? services.find(s => s.id === wanted) : null
    if (!found) {
      const target = services[0]
      setSelectedSvcId(target.id)
      if (target.mode !== 'standalone') {
        setTreeExpanded(prev => new Set(prev).add(target.id))
      }
    } else if (found.mode !== 'standalone') {
      // 직접 진입한 group 도 펼침.
      setTreeExpanded(prev => prev.has(found.id) ? prev : new Set(prev).add(found.id))
    }
  }, [services, selectedSvcId])

  // 선택된 멤버 객체 — selectedSrvId 가 set 이면 그 멤버 (현재 시스템 안에서만 유효).
  const selectedSrv = useMemo(() => {
    if (!selectedSvc || selectedSrvId === null) return null
    return selectedSvc.servers.find(s => s.id === selectedSrvId) ?? null
  }, [selectedSvc, selectedSrvId])

  const toggleTree = (svcId: number) =>
    setTreeExpanded(prev => {
      const n = new Set(prev)
      if (n.has(svcId)) n.delete(svcId); else n.add(svcId)
      return n
    })

  // 패키지 IP slot 합집합 — service 의 packageIds 에 등록된 패키지 기준
  function slotsForService(svc: ServiceRow, scope: 'service' | 'vip'): IpSlot[] {
    const slots: IpSlot[] = []
    const seen = new Set<string>()
    for (const pkgId of svc.packageIds) {
      const pkg = packageMap.get(pkgId)
      if (!pkg) continue
      for (const slot of pkg.ipSlots) {
        if (slot.scope !== scope) continue
        const key = `${slot.name}:${slot.proto ?? ''}:${slot.port ?? ''}`
        if (seen.has(key)) continue
        seen.add(key); slots.push(slot)
      }
    }
    return slots
  }

  // ── 서비스 생성 ──
  const createService = async () => {
    if (!adding || !adding.name.trim() || !adding.mode) { flash('이름 + 유형 필요'); return }
    const baseName = adding.name.trim()
    const mode = adding.mode as Mode

    try {
      if (mode === 'standalone') {
        // standalone = agent 1개 생성 (HA group 없음)
        const r = await deploymentApi.createAgent(baseName, '')
        await deploymentApi.approveAgent(r.id)
        setPendingTokens(prev => new Map(prev).set(r.id, { token: r.enrollment_token, cmd: r.install_command }))
      } else {
        // A/S = 멤버 2개 자동, AA = 멤버 0개 (운영자가 [+서버 추가] 로 채움)
        const memberCount = mode === 'active_standby' ? 2 : 0
        const memberAgents: Array<{ agent: Agent; token: string; cmd: string }> = []
        for (let i = 1; i <= memberCount; i++) {
          const r = await deploymentApi.createAgent(`${baseName}-${pad2(i)}`, '')
          await deploymentApi.approveAgent(r.id)
          memberAgents.push({ agent: r, token: r.enrollment_token, cmd: r.install_command })
        }
        // ha_group 생성 — vip 는 비움 (운영자가 VipPanel 에서 vip_bindings 추가)
        const gres = await haGroupsApi.create({
          name: baseName,
          mode,
          vip: '',                                     // nullable — vip_bindings 가 대체
          vip_mask: 24,
          auth_pass: '00000000',                       // TODO: 운영자가 입력하도록 UI 추가
          members: memberAgents.map((m, i) => ({
            agent_id: m.agent.id,
            role: i === 0 && mode === 'active_standby' ? 'master' : 'backup',
            priority: i === 0 ? 100 : 90,
          })),
        })
        setPendingTokens(prev => {
          const next = new Map(prev)
          for (const m of memberAgents) next.set(m.agent.id, { token: m.token, cmd: m.cmd })
          return next
        })
        setTreeExpanded(prev => new Set([...prev, gres.id]))
        setSelectedSvcId(gres.id)
        setSelectedSrvId(null)
      }
      flash(`서비스 "${baseName}" 추가 (${MODE_LABEL[mode]})`)
      setAdding(null)
      await load()
    } catch (e) {
      flash(`서비스 생성 실패: ${(e as Error).message}`)
    }
  }

  // ── 서버 추가 (AA 만 — Standalone 은 시스템 == 단일 agent, A/S 는 자식 2 고정) ──
  const addServer = async (svc: ServiceRow) => {
    if (svc.mode === 'active_standby' || svc.mode === 'standalone') return
    const idx = svc.servers.length + 1
    try {
      const r = await deploymentApi.createAgent(`${svc.name}-${pad2(idx)}`, '')
      await deploymentApi.approveAgent(r.id)
      setPendingTokens(prev => new Map(prev).set(r.id, { token: r.enrollment_token, cmd: r.install_command }))
      if (svc.id > 0) {
        // AA HA group 멤버로 등록
        await haGroupsApi.addMember(svc.id, { agent_id: r.id, role: 'backup', priority: 90 })
      }
      flash(`서버 "${r.name}" 추가 — install command 생성됨`)
      await load()
    } catch (e) {
      flash(`서버 추가 실패: ${(e as Error).message}`)
    }
  }

  // ── 토큰 재발행 — enrollment_token 만 갱신 (id / agent_token / HA membership 보존) ──
  // 기존 토큰이 미만료면 backend 가 409 still_valid 반환 → 사용자는 기존 토큰 그대로 복사 가능
  const regenerateToken = async (_svc: ServiceRow, srv: ServerRow) => {
    if (srv.status === 'online') {
      if (!confirm(`${srv.name} 은 이미 online — 새 install 명령은 같은 호스트 재설치 용도. 진행?`)) return
    }
    try {
      const r = await deploymentApi.regenerateToken(srv.id)
      setPendingTokens(prev => new Map(prev).set(srv.id, { token: r.enrollment_token, cmd: r.install_command }))
      const ttlMin = r.enrollment_token_ttl_sec
        ? Math.round(r.enrollment_token_ttl_sec / 60)
        : null
      const expireHint = ttlMin ? ` · ${ttlMin}분 내 사용` : ''
      if (await copyToClipboard(r.install_command)) {
        flash(`${srv.name} 설치명령 발급 + 복사됨${expireHint}`)
      } else {
        flash(`${srv.name} 설치명령 발급 (복사 실패 — 화면에서 마우스 선택)${expireHint}`)
      }
      await load()
    } catch (e) {
      const msg = (e as Error).message
      if (msg.includes('still_valid') || msg.includes('409')) {
        flash(`${srv.name} 기존 토큰 아직 유효 — 📋 복사 버튼 사용`)
      } else {
        flash(`토큰 재발행 실패: ${msg}`)
      }
    }
  }

  // ── 기존 토큰의 install command 복사 (재발행 없이) ──
  const copyExistingInstallCmd = async (srv: ServerRow) => {
    const pt = pendingTokens.get(srv.id)
    if (pt) {
      if (await copyToClipboard(pt.cmd)) {
        flash(`${srv.name} install command 복사됨 (${minutesLeft(srv.expiresAt)}분 남음)`)
        return
      }
    }
    try {
      const r = await deploymentApi.getInstallCommand(srv.id)
      if (await copyToClipboard(r.install_command)) {
        flash(`${srv.name} install command 복사됨 (${minutesLeft(r.enrollment_token_expires_at)}분 남음)`)
      } else {
        flash(`${srv.name} install command 조회 — 복사 실패 (화면 선택)`)
      }
    } catch (e) {
      flash(`install command 조회 실패: ${(e as Error).message}`)
    }
  }

  // ── 통합 버튼 핸들러 — 토큰 유효 시 복사, 만료 시 재발행 ──
  const handleInstallCmdClick = (svc: ServiceRow, srv: ServerRow) => {
    if (isTokenValid(srv.expiresAt)) return copyExistingInstallCmd(srv)
    return regenerateToken(svc, srv)
  }

  // ── install command 복사 ──
  const copyInstallCmd = async (srv: ServerRow) => {
    const pt = pendingTokens.get(srv.id)
    const cmd = pt ? pt.cmd : buildInstallCommand(srv.token || '(토큰 만료)', srv.name, srv.role)
    if (await copyToClipboard(cmd)) {
      flash(`${srv.name} install command 복사됨`)
    } else {
      flash('clipboard 복사 실패 — 텍스트를 마우스로 선택해 복사하세요')
    }
  }

  // ── 삭제 ──
  const deleteService = async (svc: ServiceRow) => {
    if (!confirm(`서비스 "${svc.name}" 과 서버 ${svc.servers.length} 개를 모두 삭제하시겠습니까?`)) return
    try {
      if (svc.id > 0) {
        // HA 그룹 + 멤버 cascade
        for (const s of svc.servers) {
          await deploymentApi.deleteAgent(s.id)
        }
        await haGroupsApi.delete(svc.id)
      } else {
        // standalone — agent 1개만 삭제
        const agentId = -svc.id
        await deploymentApi.deleteAgent(agentId)
      }
      flash(`"${svc.name}" 삭제`)
      await load()
    } catch (e) {
      flash(`삭제 실패: ${(e as Error).message}`)
    }
  }

  // ── 서비스 update (이름 등) ──
  const updateService = async (sid: number, patch: Partial<ServiceRow>) => {
    if (sid > 0) {
      // HA group update
      const body: Record<string, unknown> = {}
      if (patch.name !== undefined) body.name = patch.name
      if (patch.vipBindings !== undefined) body.vip_bindings = patch.vipBindings
      if (patch.vip !== undefined) body.vip = patch.vip
      if (patch.authPass !== undefined) body.auth_pass = patch.authPass
      try {
        await haGroupsApi.update(sid, body)
        await load()
      } catch (e) { flash(`업데이트 실패: ${(e as Error).message}`) }
    } else {
      // standalone — agent 이름만 의미 있음
      if (patch.name !== undefined) {
        try {
          await deploymentApi.updateAgent(-sid, { name: patch.name })
          await load()
        } catch (e) { flash(`업데이트 실패: ${(e as Error).message}`) }
      }
    }
  }

  // ── 서버 update (이름 / service_ip_rows) ──
  const updateServer = async (_sid: number, srvId: number, patch: Partial<ServerRow>) => {
    const body: Parameters<typeof deploymentApi.updateAgent>[1] = {}
    if (patch.name !== undefined) body.name = patch.name
    if (patch.serviceIpRows !== undefined) {
      body.service_ip_rows = patch.serviceIpRows as ApiServiceIpRow[]
    }
    if (!Object.keys(body).length) return
    try {
      await deploymentApi.updateAgent(srvId, body)
      await load()
    } catch (e) { flash(`서버 업데이트 실패: ${(e as Error).message}`) }
  }

  // ── VipPanel "[적용]" — update_ha job 강제 큐잉 (keepalived reload) ──
  const applyVip = async (svc: ServiceRow) => {
    if (svc.id <= 0) { flash('standalone 서비스는 VIP 없음'); return }
    try {
      const r = await haGroupsApi.apply(svc.id)
      flash(`VIP 적용 — ${r.jobs_queued} 멤버에 update_ha 큐잉`)
    } catch (e) { flash(`VIP 적용 실패: ${(e as Error).message}`) }
  }

  // ── ServiceIpPanel / RoutePanel 의 추가/삭제 진입점 — agent sync REST 동기 호출 ──
  // 옛 흐름(전체 desired state 일괄 add) 제거. 각 액션이 명시적 op 보냄.
  // 진행 중에는 해당 server 의 모든 row 가 status='applying' 으로 표시 (set 으로 추적).
  const applyServiceIp = async (
    srv: ServerRow,
    ops?: {
      service_ip_rows?: Array<{ op: 'add'|'del'; iface: string; ip: string; mask: number; slot?: string }>
      routes?:          Array<{ op: 'add'|'del'; dst: string; via: string; dev: string }>
    },
    label?: string,
  ) => {
    setApplyingAgents(s => new Set(s).add(srv.id))
    try {
      const r = await deploymentApi.applyIpConfig(srv.id, ops)
      const c = summarizeApplyResult(r.stdout)
      const parts = [
        c.ok   && `${c.ok} OK`,
        c.skip && `${c.skip} SKIP`,
        c.deny && `${c.deny} DENY`,
        c.fail && `${c.fail} FAIL`,
      ].filter(Boolean).join(', ')
      const what = label ?? `${r.rows} rows`
      if (r.ok && c.fail === 0 && c.deny === 0) {
        flash(`✓ ${srv.name} — ${what} ${parts ? `(${parts})` : ''}`)
      } else if (c.ok > 0 || c.skip > 0) {
        flash(`⚠ ${srv.name} 부분 적용 — ${what} (${parts})`)
      } else {
        flash(`❌ ${srv.name} 실패 — ${what} ${parts ? `(${parts})` : ''}`)
      }
      await load()
    } catch (e) { flash(`${srv.name} 적용 실패: ${(e as Error).message}`) }
    finally {
      setApplyingAgents(s => { const n = new Set(s); n.delete(srv.id); return n })
    }
  }

  // ── ServiceIpPanel 의 slot 편집 (외부 IP 포함) — PUT 으로 stored service_ip_rows 갱신 ──
  // ip addr 변경은 안 함. VIP / module config 매핑용 라벨 보존만.
  const updateSlot = async (srv: ServerRow, iface: string, ip: string, mask: number, slot: string) => {
    if (srv.id <= 0) return                                                     // pending placeholder 제외
    // (iface, ip) 키 기준 upsert. slot 비우면 row 제거.
    const next = srv.serviceIpRows.filter(r => !(r.iface === iface && r.ip === ip))
    if (slot.trim()) next.push({ iface, ip, mask, slot: slot.trim() })
    try {
      await deploymentApi.updateAgent(srv.id, { service_ip_rows: next })
      await load()
    } catch (e) { flash(`${srv.name} 용도 저장 실패: ${(e as Error).message}`) }
  }

  // ── 패키지 추가/제거 → deployments insert/delete per member ──
  const updatePackageIds = async (svc: ServiceRow, ids: number[]) => {
    const current = new Set(svc.packageIds)
    const next = new Set(ids)
    const added = ids.filter(id => !current.has(id))
    const removed = svc.packageIds.filter(id => !next.has(id))
    try {
      for (const pkgId of added) {
        const pkg = packageMap.get(pkgId)
        if (!pkg) continue
        for (const srv of svc.servers) {
          await deploymentApi.createDeployment({
            agent_id: srv.id,
            package_id: pkgId,
            process_name: pkg.name.toUpperCase(),
            service_functions: [],
          })
        }
      }
      for (const pkgId of removed) {
        const target = deployments.filter(d =>
          d.package_id === pkgId && svc.servers.some(s => s.id === d.agent_id)
        )
        for (const d of target) await deploymentApi.deleteDeployment(d.id)
      }
      flash(`패키지 ${added.length} 추가 / ${removed.length} 제거`)
      await load()
    } catch (e) {
      flash(`패키지 변경 실패: ${(e as Error).message}`)
    }
  }

  if (loading) return <div style={{ padding: 24 }}>로딩 중...</div>

  return (
    <div style={{ padding: 16, width: '100%', height: 'calc(100vh - 120px)',
                  display: 'flex', flexDirection: 'column', boxSizing: 'border-box' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', marginBottom: 12 }}>
        <h1 style={{ margin: 0 }}>서버 + HA 관리</h1>
        <button onClick={() => void load()} style={{ fontSize: 12, padding: '4px 12px',
                                                     background: 'var(--surface)', border: '1px solid var(--border)',
                                                     borderRadius: 3, cursor: 'pointer' }}>↻ 새로고침</button>
      </div>
      <div style={{ color: 'var(--text-muted)', fontSize: 13, marginBottom: 16 }}>
        서비스(=HA 그룹/단독) 단위로 서버를 묶어 관리. 유형 선택 시 자동으로 서버 발급(A/S=2, AA/Standalone=1).
        모든 추가/편집은 list 행 안 inline (팝업 없음).
      </div>
      {err && <div style={{ color: '#c00', marginBottom: 12, fontSize: 12 }}>오류: {err}</div>}

      <div style={{ flex: 1, display: 'flex', minHeight: 0,
                    border: '1px solid #e0e0e0', borderRadius: 6, background: 'var(--surface)', overflow: 'hidden' }}>
        {/* 좌측 SystemList */}
        <div style={{ width: 280, borderRight: '1px solid #e0e0e0', display: 'flex', flexDirection: 'column' }}>
          <div style={{ padding: '8px 12px', borderBottom: '1px solid #e0e0e0',
                        fontSize: 12, fontWeight: 'bold', color: 'var(--text-muted)', background: '#f7f8fa' }}>
            시스템 ({services.length})
          </div>
          <div style={{ flex: 1, overflowY: 'auto' }}>
            {services.length === 0 && !adding && (
              <div style={{ padding: 16, color: 'var(--text-muted)', fontSize: 12, textAlign: 'center' }}>
                (등록된 시스템 없음)<br />아래 [＋ 시스템 추가]
              </div>
            )}
            {services.map(svc => (
              <SystemListItem key={svc.id} svc={svc}
                              selected={svc.id === selectedSvcId && selectedSrvId === null}
                              expanded={treeExpanded.has(svc.id)}
                              onClickSystem={() => { setSelectedSvcId(svc.id); setSelectedSrvId(null) }}
                              onToggleExpand={() => toggleTree(svc.id)}
                              selectedSrvId={svc.id === selectedSvcId ? selectedSrvId : null}
                              onClickMember={(srv) => { setSelectedSvcId(svc.id); setSelectedSrvId(srv.id) }}
                              onAddMember={() => addServer(svc)} />
            ))}
          </div>
          {/* 시스템 추가 폼 — 좌측 list 아래 */}
          {adding ? (
            <div style={{ padding: 10, borderTop: '1px solid #e0e0e0', background: '#f0f8ff' }}>
              <div style={{ fontSize: 11, color: 'var(--text-muted)', marginBottom: 6 }}>새 시스템</div>
              <input value={adding.name} onChange={e => setAdding({ ...adding, name: e.target.value })}
                     placeholder="이름 (예: VoLTE SIP Server)"
                     style={{ width: '100%', padding: '4px 8px', boxSizing: 'border-box', marginBottom: 6 }}
                     autoFocus />
              <select value={adding.mode} onChange={e => setAdding({ ...adding, mode: e.target.value as Mode })}
                      style={{ width: '100%', padding: '4px', marginBottom: 6 }}>
                <option value="">유형 선택</option>
                <option value="active_standby">AS (자식 2)</option>
                <option value="all_active">AA (자식 N)</option>
                <option value="standalone">Standalone (자식 N)</option>
              </select>
              <div style={{ display: 'flex', gap: 4 }}>
                <button onClick={() => void createService()} style={{ ...btnPrimary(), flex: 1 }}>생성</button>
                <button onClick={() => setAdding(null)} style={btnSecondary()}>취소</button>
              </div>
            </div>
          ) : (
            <div style={{ padding: 8, borderTop: '1px solid #e0e0e0' }}>
              <button onClick={() => setAdding({ name: '', mode: '' })}
                      style={{ ...btnAdd(), width: '100%' }}>
                ＋ 시스템 추가
              </button>
            </div>
          )}
        </div>

        {/* 우측 Detail — 시스템 view (selectedSrv=null) 또는 멤버 view (selectedSrv!=null) */}
        <div style={{ flex: 1, overflowY: 'auto', padding: 16, background: '#fafbfc' }}>
          {selectedSvc && selectedSrv ? (
            <ServerDetail
              svc={selectedSvc}
              srv={selectedSrv}
              editingName={editingName}
              setEditingName={setEditingName}
              serviceSlots={slotsForService(selectedSvc, 'service')}
              updateServer={updateServer}
              applyServiceIp={applyServiceIp}
              updateSlot={updateSlot}
              applyingAgents={applyingAgents}
              regenerateToken={(srv) => handleInstallCmdClick(selectedSvc, srv)}
            />
          ) : selectedSvc ? (
            <SystemDetail
              svc={selectedSvc}
              editingName={editingName}
              setEditingName={setEditingName}
              pkgPickerOpen={pkgPickerFor === selectedSvc.id}
              setPkgPicker={(open) => setPkgPickerFor(open ? selectedSvc.id : null)}
              vipSlots={slotsForService(selectedSvc, 'vip')}
              serviceSlots={slotsForService(selectedSvc, 'service')}
              packageMap={packageMap}
              pendingTokens={pendingTokens}
              updateService={updateService}
              updateServer={updateServer}
              updatePackageIds={updatePackageIds}
              applyVip={applyVip}
              applyServiceIp={applyServiceIp}
              updateSlot={updateSlot}
              applyingAgents={applyingAgents}
              addServer={() => addServer(selectedSvc)}
              regenerateToken={(srv) => handleInstallCmdClick(selectedSvc, srv)}
              copyCmd={(srv) => copyInstallCmd(srv)}
              onDelete={() => deleteService(selectedSvc)}
              onOpenConfig={() => setConfigModalFor(selectedSvc)}
              onHealthCheck={() => setHealthCheckGroup(selectedSvc)}
              canHealthCheck={selectedSvc.servers.some(s => s.status === 'online')}
            />
          ) : (
            <div style={{ color: 'var(--text-muted)', fontSize: 13, padding: 32, textAlign: 'center' }}>
              ← 좌측에서 시스템 선택
            </div>
          )}
        </div>
      </div>

      {/* 토스트 */}
      {toast && (
        <div style={{
          position: 'fixed', bottom: 24, left: '50%', transform: 'translateX(-50%)',
          background: '#333', color: '#fff', padding: '8px 16px', borderRadius: 4,
          fontSize: 13, zIndex: 1000,
        }}>{toast}</div>
      )}

      {/* 그룹 설정 비교 (R2) — 멤버별 설정값 읽기 전용 비교. 편집은 시스템/인프라 >
          패키지 설정 탭 (셀 클릭 시 해당 서버로 딥링크). */}
      {configModalFor && configModalFor.mode !== 'standalone' && (() => {
        const g = haGroups.find(x => x.id === configModalFor.id)
        if (!g) return null
        return (
          <Modal title={`${configModalFor.name} — 설정 비교`} onClose={() => setConfigModalFor(null)} fullscreen>
            <GroupConfigCompareView
              group={g}
              members={configModalFor.servers.map(s => ({ id: s.id, name: s.name }))}
              deployments={deployments}
              packages={packages}
              onSelectMember={(aid) => navigate(`/deploy/servers?agent=${aid}&t=config`)} />
          </Modal>
        )
      })()}

      {/* 그룹 단위 실시간 점검 — 온라인 멤버를 한 모달에 동시 점검 */}
      {healthCheckGroup && (() => {
        const members = healthCheckGroup.servers
          .filter(s => s.status === 'online')
          .map(s => agentMap.get(s.id))
          .filter((a): a is Agent => a != null)
        if (members.length === 0) return null
        return <HealthCheckModal agents={members} onClose={() => setHealthCheckGroup(null)} />
      })()}
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  새 Master-Detail layout — SystemList / SystemDetail / AccordionSection
//  (옛 ServiceTreeRows/StandaloneRow/ServerRows 는 점진 마이그레이션 후 제거)
// ──────────────────────────────────────────────────────────────

function AccordionSection({ title, defaultOpen, right, children }: {
  title: string
  defaultOpen?: boolean
  right?: React.ReactNode                                                       // header 우측 슬롯 (선택)
  children: React.ReactNode
}) {
  const [open, setOpen] = useState(defaultOpen ?? true)
  return (
    <div style={{ marginBottom: 10, border: '1px solid #e0e0e0', borderRadius: 4, background: 'var(--surface)' }}>
      <div style={{ padding: '8px 12px', cursor: 'pointer', background: '#f7f8fa',
                    borderBottom: open ? '1px solid #e0e0e0' : 'none',
                    fontWeight: 'bold', fontSize: 13,
                    display: 'flex', alignItems: 'center', gap: 6 }}
           onClick={() => setOpen(!open)}>
        <span style={{ width: 12, color: 'var(--text-muted)' }}>{open ? '▾' : '▸'}</span>
        <span style={{ flex: 1 }}>{title}</span>
        {right && <span onClick={e => e.stopPropagation()}>{right}</span>}
      </div>
      {open && <div style={{ padding: 12 }}>{children}</div>}
    </div>
  )
}

// 상태 → 점 + 색상 (시스템 요약 / 멤버 row 공용).
function statusDot(statuses: ServerStatus[]): { dot: string; color: string } {
  const ss = statuses as readonly string[]   // agent 원본 status 는 revoked/error 도 가능 (ServerStatus 로 좁히기 전 방어)
  const hasOffline = ss.includes('offline') || ss.includes('revoked') || ss.includes('error')
  const hasPending = statuses.includes('pending')
  const allOnline = statuses.length > 0 && statuses.every(s => s === 'online')
  return {
    dot:   allOnline ? '●' : hasOffline ? '◐' : hasPending ? '⏳' : '○',
    color: allOnline ? '#27ae60' : hasOffline ? '#c0392b' : hasPending ? '#f39c12' : '#888',
  }
}

function SystemListItem({ svc, selected, expanded, onClickSystem, onToggleExpand,
                         selectedSrvId, onClickMember, onAddMember }: {
  svc: ServiceRow
  selected: boolean                                                             // 시스템 자체가 선택 (srvId null)
  expanded: boolean
  onClickSystem: () => void
  onToggleExpand: () => void
  selectedSrvId: number | null                                                  // 멤버 선택 시 그 멤버 id
  onClickMember: (srv: ServerRow) => void
  onAddMember: () => void                                                       // AA 의 멤버 추가 (좌측 트리 끝)
}) {
  const isStandalone = svc.mode === 'standalone'
  const { dot, color } = statusDot(svc.servers.map(s => s.status))
  return (
    <>
      <div onClick={() => { if (!isStandalone) onToggleExpand(); onClickSystem() }}
           style={{
             padding: '8px 12px', cursor: 'pointer',
             background: selected ? '#e8f0fe' : 'transparent',
             borderLeft: selected ? '3px solid #4285f4' : '3px solid transparent',
             fontSize: 13, userSelect: 'none',
           }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
          {!isStandalone && (
            <span style={{ fontSize: 10, color: 'var(--text-muted)', width: 10, flexShrink: 0 }}>
              {expanded ? '▾' : '▸'}
            </span>
          )}
          {isStandalone && <span style={{ width: 10, flexShrink: 0 }} />}
          <span style={{ color, fontSize: 11 }}>{dot}</span>
          <span style={{
            fontWeight: selected ? 'bold' : 'normal',
            flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
          }}>
            {svc.name}
          </span>
          {/* AA 만 — 모드 chip 앞에 [+] 아이콘. row 클릭 이벤트와 분리 (stopPropagation). */}
          {svc.mode === 'all_active' && (
            <span onClick={(e) => { e.stopPropagation(); onAddMember() }}
                  title="멤버 서버 추가"
                  style={{
                    fontSize: 14, color: '#4285f4', cursor: 'pointer',
                    padding: '0 4px', borderRadius: 3, lineHeight: 1,
                  }}>
              ＋
            </span>
          )}
          <ModeBadge mode={svc.mode} />
        </div>
      </div>
      {!isStandalone && expanded && (
        <>
          {svc.servers.length === 0 && (
            <div style={{ padding: '6px 12px 6px 36px', fontSize: 11, color: 'var(--text-muted)', fontStyle: 'italic' }}>
              (멤버 없음)
            </div>
          )}
          {svc.servers.map(srv => {
            const isSelected = selectedSrvId === srv.id
            const { dot: sd, color: sc } = statusDot([srv.status])
            return (
              <div key={srv.id} onClick={() => onClickMember(srv)}
                   style={{
                     padding: '6px 12px 6px 36px', cursor: 'pointer',
                     background: isSelected ? '#e8f0fe' : 'transparent',
                     borderLeft: isSelected ? '3px solid #4285f4' : '3px solid transparent',
                     fontSize: 12, userSelect: 'none',
                   }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                  <span style={{ color: sc, fontSize: 10 }}>{sd}</span>
                  <span style={{
                    fontWeight: isSelected ? 'bold' : 'normal',
                    flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
                  }}>
                    {srv.name}
                  </span>
                  <VipObservedBadge v={srv.vipObserved} />
                  {srv.role && (
                    <span style={{ fontSize: 9, padding: '0 4px', borderRadius: 2,
                                   background: srv.role === 'master' ? '#e67e22' : '#7f8c8d', color: '#fff' }}>
                      {srv.role}
                    </span>
                  )}
                </div>
              </div>
            )
          })}
        </>
      )}
    </>
  )
}

interface SystemDetailProps {
  svc: ServiceRow
  editingName: { kind: 'service' | 'server'; id: number; value: string } | null
  setEditingName: (v: { kind: 'service' | 'server'; id: number; value: string } | null) => void
  pkgPickerOpen: boolean
  setPkgPicker: (open: boolean) => void
  vipSlots: IpSlot[]
  serviceSlots: IpSlot[]
  packageMap: Map<number, PkgDef>
  pendingTokens: Map<number, { token: string; cmd: string }>
  updateService: (sid: number, patch: Partial<ServiceRow>) => void
  updateServer: (sid: number, srvId: number, patch: Partial<ServerRow>) => void
  updatePackageIds: (svc: ServiceRow, ids: number[]) => void
  applyVip: (svc: ServiceRow) => void
  applyServiceIp: (
    srv: ServerRow,
    ops?: {
      service_ip_rows?: Array<{ op: 'add'|'del'; iface: string; ip: string; mask: number; slot?: string }>
      routes?:          Array<{ op: 'add'|'del'; dst: string; via: string; dev: string }>
    },
    label?: string,
  ) => void
  updateSlot: (srv: ServerRow, iface: string, ip: string, mask: number, slot: string) => void
  applyingAgents: Set<number>
  addServer: () => void
  regenerateToken: (srv: ServerRow) => void
  copyCmd: (srv: ServerRow) => void
  onDelete: () => void
  onOpenConfig: () => void
  onHealthCheck: () => void
  canHealthCheck: boolean
}

function SystemDetail(p: SystemDetailProps) {
  const { svc } = p
  const isStandalone = svc.mode === 'standalone'
  const needsVip = svc.mode === 'active_standby'
  const canAddServer = !isStandalone && svc.mode !== 'active_standby'           // AA 만 임의 추가
  const statuses = svc.servers.map(s => s.status)
  const allOnline = svc.servers.length > 0 && statuses.every(s => s === 'online')

  return (
    <div>
      {/* 헤더 — 이름 + 모드 + 액션 */}
      <div style={{
        display: 'flex', alignItems: 'center', gap: 12, padding: '8px 0 12px',
        borderBottom: '1px solid #e0e0e0', marginBottom: 12,
      }}>
        <span style={{ fontSize: 18, fontWeight: 'bold', flex: '0 0 auto' }}>
          <InlineNameEdit kind="service" id={svc.id} value={svc.name}
                          editing={p.editingName}
                          onStart={(v) => p.setEditingName({ kind: 'service', id: svc.id, value: v })}
                          onChange={(v) => p.setEditingName(p.editingName ? { ...p.editingName, value: v } : null)}
                          onSave={(v) => { p.updateService(svc.id, { name: v }); p.setEditingName(null) }}
                          onCancel={() => p.setEditingName(null)}
                          bold />
        </span>
        <ModeBadge mode={svc.mode} />
        <StatusSummary servers={svc.servers} mode={svc.mode} />
        <span style={{ flex: 1 }} />
        <button onClick={p.onHealthCheck} style={btnSecondary()} disabled={!p.canHealthCheck}
                title={p.canHealthCheck ? '온라인 멤버 동시 실시간 점검' : '온라인 멤버 없음'}>🩺 점검</button>
        {!isStandalone && (
          <button onClick={p.onOpenConfig} style={btnSecondary()}
                  title="멤버별 설정값 비교 (읽기 전용) — 편집은 시스템/인프라 > 패키지 설정 탭">🔍 설정 비교</button>
        )}
        <button onClick={p.onDelete} style={btnDanger()}>삭제</button>
      </div>

      {/* 일반 정보 */}
      <AccordionSection title="일반 정보" defaultOpen>
        <table style={{ fontSize: 12 }}>
          <tbody>
            <tr><td style={{ padding: '4px 12px 4px 0', color: 'var(--text-muted)' }}>모드</td>
                <td style={{ padding: '4px 0' }}>{MODE_LABEL[svc.mode]}</td></tr>
            <tr><td style={{ padding: '4px 12px 4px 0', color: 'var(--text-muted)' }}>멤버 수</td>
                <td style={{ padding: '4px 0' }}>{svc.servers.length}</td></tr>
            <tr><td style={{ padding: '4px 12px 4px 0', color: 'var(--text-muted)' }}>상태</td>
                <td style={{ padding: '4px 0' }}>{allOnline ? '전체 online' : '일부 비-online'}</td></tr>
            {needsVip && (
              <tr><td style={{ padding: '4px 12px 4px 0', color: 'var(--text-muted)' }}>VRID</td>
                  <td style={{ padding: '4px 0' }}>{svc.vrid ?? '—'}</td></tr>
            )}
          </tbody>
        </table>
      </AccordionSection>

      {/* 멤버 서버 */}
      <AccordionSection title={`멤버 서버 (${svc.servers.length})`} defaultOpen>
        <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
          <thead>
            <tr style={{ background: 'var(--surface-2)', color: 'var(--text-muted)' }}>
              <th style={{ padding: '4px 8px', textAlign: 'left' }}>이름</th>
              <th style={{ padding: '4px 8px', textAlign: 'left', width: 140 }}>mgmt IP</th>
              <th style={{ padding: '4px 8px', textAlign: 'left', width: 120 }}>상태</th>
              <th style={{ padding: '4px 8px', textAlign: 'left', width: 220 }}>액션</th>
            </tr>
          </thead>
          <tbody>
            {svc.servers.map(srv => (
              <tr key={srv.id}>
                <td style={{ padding: '4px 8px' }}>
                  <InlineNameEdit kind="server" id={srv.id} value={srv.name}
                                  editing={p.editingName}
                                  onStart={(v) => p.setEditingName({ kind: 'server', id: srv.id, value: v })}
                                  onChange={(v) => p.setEditingName(p.editingName ? { ...p.editingName, value: v } : null)}
                                  onSave={(v) => { p.updateServer(svc.id, srv.id, { name: v }); p.setEditingName(null) }}
                                  onCancel={() => p.setEditingName(null)} />
                  {srv.role && (
                    <span style={{ marginLeft: 6, fontSize: 10, padding: '1px 5px', borderRadius: 3,
                                   background: srv.role === 'master' ? '#e67e22' : '#7f8c8d', color: '#fff' }}>
                      {srv.role}
                    </span>
                  )}
                  <span style={{ marginLeft: 4 }}>
                    <VipObservedBadge v={srv.vipObserved} size={10} />
                  </span>
                </td>
                <td style={{ padding: '4px 8px', color: srv.ip ? '#333' : '#aaa' }}>
                  {srv.ip ?? '— (enroll 후 자동)'}
                </td>
                <td style={{ padding: '4px 8px' }}>
                  <span style={{ color: STATUS_COLOR[srv.status], fontWeight: 'bold' }}>
                    {STATUS_ICON[srv.status]} {srv.status}
                  </span>
                  {srv.agent_version && <span style={{ marginLeft: 6, fontSize: 10, color: 'var(--text-muted)' }}>v{srv.agent_version}</span>}
                </td>
                <td style={{ padding: '4px 8px' }}>
                  {srv.status !== 'online' && (
                    isTokenValid(srv.expiresAt) ? (
                      <button onClick={() => p.regenerateToken(srv)} style={btnSmall()}
                              title={`기존 install command 복사 — 토큰 ${minutesLeft(srv.expiresAt)}분 남음`}>
                        📋 복사 ({minutesLeft(srv.expiresAt)}m)
                      </button>
                    ) : (
                      <button onClick={() => p.regenerateToken(srv)} style={btnSmall()}>
                        🔧 설치명령
                      </button>
                    )
                  )}
                  {srv.id > 0 && (
                    <Link to={`/deploy/servers?agent=${srv.id}`}
                          title="Server Inspector"
                          style={{ ...btnSmall(), textDecoration: 'none', display: 'inline-block' }}>
                      🔍
                    </Link>
                  )}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        {canAddServer && (
          <div style={{ marginTop: 8 }}>
            <button onClick={p.addServer} style={btnAdd(true)}>
              ＋ 서버 추가 ({MODE_LABEL[svc.mode]} — 신규 토큰 발행)
            </button>
          </div>
        )}
      </AccordionSection>

      {/* 패키지 */}
      <AccordionSection title="패키지" defaultOpen>
        <PackagesArea svc={svc}
                      packageMap={p.packageMap}
                      pickerOpen={p.pkgPickerOpen}
                      setPickerOpen={p.setPkgPicker}
                      onChange={(ids) => p.updatePackageIds(svc, ids)} />
      </AccordionSection>

      {/* 서비스 IP / 라우팅 — Standalone 만 시스템 view 에 표시 (시스템 = 멤버).
          AS/AA 는 좌측에서 개별 멤버 선택 → 멤버 view 에 표시. */}
      {isStandalone && svc.servers[0] && (
        <AccordionSection title="서비스 IP / 라우팅" defaultOpen>
          {(() => {
            const srv = svc.servers[0]
            const enrollDone = srv.status !== 'pending'
            if (!enrollDone) return <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>⏳ enroll 대기</div>
            if (srv.interfaces.length === 0) return <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>— (NIC 정보 대기)</div>
            return (
              <ServiceIpPanel
                title="인터페이스 IP / 라우팅"
                interfaces={srv.interfaces}
                storedRows={srv.serviceIpRows}
                storedRoutes={srv.routes}
                slots={p.serviceSlots}
                applying={p.applyingAgents.has(srv.id)}
                onApply={(ops, label) => p.applyServiceIp(srv, ops, label)}
                onUpdateSlot={(iface, ip, mask, slot) => p.updateSlot(srv, iface, ip, mask, slot)}
                vipIps={new Set((svc.vipBindings || []).map(b => b.ip).filter(Boolean))}
              />
            )
          })()}
        </AccordionSection>
      )}
      {!isStandalone && (
        <AccordionSection title="서비스 IP / 라우팅 — 멤버별" defaultOpen={false}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>
            좌측 트리에서 멤버 서버를 선택하면 해당 멤버의 서비스 IP / 라우팅 이 우측에 표시됩니다.
          </div>
        </AccordionSection>
      )}

      {/* VIP — AS 만 */}
      {needsVip && (
        <AccordionSection title="VIP (A/S fail-over)" defaultOpen>
          <VipPanel
            title=""
            svc={svc}
            vrid={svc.vrid}
            onChange={(bindings) => p.updateService(svc.id, { vipBindings: bindings })}
            onApply={() => p.applyVip(svc)}
          />
        </AccordionSection>
      )}
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  ServerDetail — 멤버 서버 한 명 focus view (AS/AA 의 좌측 멤버 선택 시)
// ──────────────────────────────────────────────────────────────

interface ServerDetailProps {
  svc: ServiceRow
  srv: ServerRow
  editingName: { kind: 'service' | 'server'; id: number; value: string } | null
  setEditingName: (v: { kind: 'service' | 'server'; id: number; value: string } | null) => void
  serviceSlots: IpSlot[]
  updateServer: (sid: number, srvId: number, patch: Partial<ServerRow>) => void
  applyServiceIp: (
    srv: ServerRow,
    ops?: {
      service_ip_rows?: Array<{ op: 'add'|'del'; iface: string; ip: string; mask: number; slot?: string }>
      routes?:          Array<{ op: 'add'|'del'; dst: string; via: string; dev: string }>
    },
    label?: string,
  ) => void
  updateSlot: (srv: ServerRow, iface: string, ip: string, mask: number, slot: string) => void
  applyingAgents: Set<number>
  regenerateToken: (srv: ServerRow) => void
}

// 멤버별 리소스 mini-chart — heartbeat metric JSONL(2s) 시계열을 cpu/mem/disk sparkline 으로.
function MemberMetricTrends({ agentId }: { agentId: number }) {
  const [metrics, setMetrics] = useState<AgentMetric[]>([])
  const [err, setErr] = useState('')
  useEffect(() => {
    let alive = true
    deploymentApi.agentMetrics(agentId)
      .then(r => { if (alive) setMetrics(r.items) })
      .catch(e => { if (alive) setErr((e as Error).message) })
    return () => { alive = false }
  }, [agentId])
  if (err) return <div style={{ fontSize: 12, color: '#e74c3c' }}>※ {err}</div>
  const chrono = [...metrics].sort((a, b) => (a.ts || '').localeCompare(b.ts || ''))
  if (chrono.length < 2) return <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>데이터 부족 (metric 수집 대기)</div>
  return (
    <div style={{ display: 'flex', gap: 10 }}>
      <MetricTrend label="CPU"  values={chrono.map(m => m.cpu_pct)}  color="#3498db" warn={85} width={150} />
      <MetricTrend label="MEM"  values={chrono.map(m => m.mem_pct)}  color="#27ae60" warn={90} width={150} />
      <MetricTrend label="Disk" values={chrono.map(m => m.disk_pct)} color="#9b59b6" warn={90} width={150} />
    </div>
  )
}

function ServerDetail(p: ServerDetailProps) {
  const { svc, srv } = p
  const enrollDone = srv.status !== 'pending'
  return (
    <div>
      {/* 헤더 */}
      <div style={{
        display: 'flex', alignItems: 'center', gap: 12, padding: '8px 0 12px',
        borderBottom: '1px solid #e0e0e0', marginBottom: 12,
      }}>
        <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>{svc.name} /</span>
        <span style={{ fontSize: 18, fontWeight: 'bold' }}>
          <InlineNameEdit kind="server" id={srv.id} value={srv.name}
                          editing={p.editingName}
                          onStart={(v) => p.setEditingName({ kind: 'server', id: srv.id, value: v })}
                          onChange={(v) => p.setEditingName(p.editingName ? { ...p.editingName, value: v } : null)}
                          onSave={(v) => { p.updateServer(svc.id, srv.id, { name: v }); p.setEditingName(null) }}
                          onCancel={() => p.setEditingName(null)} bold />
        </span>
        {srv.role && (
          <span style={{ fontSize: 11, padding: '2px 6px', borderRadius: 3,
                         background: srv.role === 'master' ? '#e67e22' : '#7f8c8d', color: '#fff' }}>
            {srv.role}
          </span>
        )}
        <VipObservedBadge v={srv.vipObserved} size={11} />
        <span style={{ color: STATUS_COLOR[srv.status], fontWeight: 'bold' }}>
          {STATUS_ICON[srv.status]} {srv.status}
        </span>
        <span style={{ flex: 1 }} />
        {srv.status !== 'online' && (
          isTokenValid(srv.expiresAt) ? (
            <button onClick={() => p.regenerateToken(srv)} style={btnSmall()}
                    title={`기존 install command 복사 — 토큰 ${minutesLeft(srv.expiresAt)}분 남음`}>
              📋 복사 ({minutesLeft(srv.expiresAt)}m)
            </button>
          ) : (
            <button onClick={() => p.regenerateToken(srv)} style={btnSmall()}>
              🔧 설치명령
            </button>
          )
        )}
        {srv.id > 0 && (
          <Link to={`/deploy/servers?agent=${srv.id}`}
                title="Server Inspector"
                style={{ ...btnSmall(), textDecoration: 'none', display: 'inline-block' }}>
            🔍 Inspector
          </Link>
        )}
      </div>

      <AccordionSection title="일반 정보" defaultOpen>
        <table style={{ fontSize: 12 }}>
          <tbody>
            <tr><td style={{ padding: '4px 12px 4px 0', color: 'var(--text-muted)' }}>mgmt IP</td>
                <td style={{ padding: '4px 0', fontFamily: 'monospace' }}>{srv.ip ?? '— (enroll 후 자동)'}</td></tr>
            <tr><td style={{ padding: '4px 12px 4px 0', color: 'var(--text-muted)' }}>상태</td>
                <td style={{ padding: '4px 0' }}>
                  <span style={{ color: STATUS_COLOR[srv.status], fontWeight: 'bold' }}>
                    {STATUS_ICON[srv.status]} {srv.status}
                  </span>
                  {srv.agent_version && <span style={{ marginLeft: 8, color: 'var(--text-muted)' }}>v{srv.agent_version}</span>}
                </td></tr>
            <tr><td style={{ padding: '4px 12px 4px 0', color: 'var(--text-muted)' }}>role</td>
                <td style={{ padding: '4px 0' }}>{srv.role || '—'}</td></tr>
            {srv.expiresAt && srv.status === 'pending' && (
              <tr><td style={{ padding: '4px 12px 4px 0', color: 'var(--text-muted)' }}>토큰 만료</td>
                  <td style={{ padding: '4px 0', color: 'var(--text-muted)' }}>{minutesLeft(srv.expiresAt)}분 남음</td></tr>
            )}
          </tbody>
        </table>
      </AccordionSection>

      {srv.status === 'online' && srv.id > 0 && (
        <AccordionSection title="리소스 추세" defaultOpen>
          <MemberMetricTrends agentId={srv.id} />
        </AccordionSection>
      )}

      <AccordionSection title="서비스 IP / 라우팅" defaultOpen>
        {!enrollDone ? (
          <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>⏳ enroll 대기</div>
        ) : srv.interfaces.length === 0 ? (
          <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>— (NIC 정보 대기)</div>
        ) : (
          <ServiceIpPanel
            title="인터페이스 IP / 라우팅"
            interfaces={srv.interfaces}
            storedRows={srv.serviceIpRows}
            storedRoutes={srv.routes}
            slots={p.serviceSlots}
            applying={p.applyingAgents.has(srv.id)}
            onApply={(ops, label) => p.applyServiceIp(srv, ops, label)}
            onUpdateSlot={(iface, ip, mask, slot) => p.updateSlot(srv, iface, ip, mask, slot)}
            vipIps={new Set((svc.vipBindings || []).map(b => b.ip).filter(Boolean))}
          />
        )}
      </AccordionSection>
    </div>
  )
}

function ModeBadge({ mode }: { mode: Mode }) {
  return (
    <span title={MODE_TOOLTIP[mode]} style={{
      fontSize: 11, padding: '2px 6px', borderRadius: 3,
      background: MODE_COLOR[mode], color: '#fff', cursor: 'help',
    }}>{MODE_LABEL[mode]}</span>
  )
}

function StatusSummary({ servers, mode }: { servers: ServerRow[]; mode: Mode }) {
  const online = servers.filter(s => s.status === 'online').length
  const total = servers.length
  const cap = mode === 'active_standby' ? 2 : null
  const color = online === total && total > 0 ? STATUS_COLOR.online : STATUS_COLOR.pending
  return (
    <span style={{ fontSize: 12, color }}>
      {online === total && total > 0 ? '●' : '◐'} {online}/{cap ?? total}
      {cap && online < cap && <span style={{ marginLeft: 4, color: 'var(--text-muted)' }}>(pending {cap - online})</span>}
    </span>
  )
}

function InlineNameEdit({ kind, id, value, editing, onStart, onChange, onSave, onCancel, bold }: {
  kind: 'service' | 'server'; id: number; value: string
  editing: { kind: 'service' | 'server'; id: number; value: string } | null
  onStart: (v: string) => void
  onChange: (v: string) => void
  onSave: (v: string) => void
  onCancel: () => void
  bold?: boolean
}) {
  const isEditing = editing && editing.kind === kind && editing.id === id
  if (isEditing) {
    return (
      <input value={editing.value}
             onChange={e => onChange(e.target.value)}
             onKeyDown={e => {
               if (e.key === 'Enter') onSave(editing.value.trim() || value)
               if (e.key === 'Escape') onCancel()
             }}
             onBlur={() => onSave(editing.value.trim() || value)}
             autoFocus
             style={{ width: '85%', padding: '2px 6px', fontWeight: bold ? 'bold' : 'normal' }} />
    )
  }
  return (
    <span onClick={() => onStart(value)}
          style={{ cursor: 'pointer', fontWeight: bold ? 'bold' : 'normal' }}
          title="클릭 시 이름 편집">
      {value}
    </span>
  )
}

function PackagesArea({ svc, packageMap, pickerOpen, setPickerOpen, onChange }: {
  svc: ServiceRow
  packageMap: Map<number, PkgDef>
  pickerOpen: boolean
  setPickerOpen: (open: boolean) => void
  onChange: (ids: number[]) => void
}) {
  const allPackages = useMemo(() => Array.from(packageMap.values()), [packageMap])
  const installed = useMemo(
    () => allPackages.filter(p => svc.packageIds.includes(p.id)),
    [allPackages, svc.packageIds]
  )
  const available = useMemo(
    () => allPackages.filter(p => !svc.packageIds.includes(p.id)),
    [allPackages, svc.packageIds]
  )
  const targetMode: Capability = svc.mode

  const togglePkg = (pkgId: number) => {
    onChange(svc.packageIds.includes(pkgId)
      ? svc.packageIds.filter(x => x !== pkgId)
      : [...svc.packageIds, pkgId])
  }
  const removePkg = (pkgId: number) => onChange(svc.packageIds.filter(x => x !== pkgId))

  return (
    <div>
      <div style={{ fontSize: 12, color: 'var(--text-muted)', display: 'flex', alignItems: 'center', gap: 6, flexWrap: 'wrap' }}>
        <span>▸ 패키지 ({MODE_LABEL[svc.mode]} 가능):</span>
        {installed.length === 0 && <span style={{ color: 'var(--text-muted)' }}>(없음)</span>}
        {installed.map(p => (
          <span key={p.id} style={{
            fontSize: 11, padding: '2px 6px', borderRadius: 3,
            background: '#e8f5e9', border: '1px solid #c8e6c9', color: '#2e7d32',
            display: 'inline-flex', alignItems: 'center', gap: 4,
          }}>
            {p.name} {p.version}
            <button onClick={() => removePkg(p.id)}
                    style={{ background: 'none', border: 'none', cursor: 'pointer', color: 'var(--text-muted)' }}>×</button>
          </span>
        ))}
        <button onClick={() => setPickerOpen(!pickerOpen)} style={btnSmall()}>
          {pickerOpen ? '✕ 닫기' : '＋ 패키지 추가'}
        </button>
      </div>

      {pickerOpen && (
        <div style={{ marginTop: 8, padding: 10, background: 'var(--surface)',
                      border: '1px dashed #c0c0c0', borderRadius: 4 }}>
          <div style={{ fontSize: 11, color: 'var(--text-muted)', marginBottom: 6 }}>
            ℹ {MODE_LABEL[targetMode]} 가능 또는 standalone 모듈만 선택 가능 — 서비스 모든 서버에 일괄 설치
          </div>
          {available.length === 0 && <div style={{ color: 'var(--text-muted)' }}>(추가 가능한 패키지 없음)</div>}
          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(200px, 1fr))', gap: 6 }}>
            {available.map(p => {
              const ok = p.capability === targetMode || p.capability === 'standalone'
              return (
                <label key={p.id} style={{
                  display: 'flex', alignItems: 'center', gap: 6,
                  padding: 6, border: '1px solid var(--border)', borderRadius: 3,
                  background: !ok ? '#f7f7f7' : '#fff',
                  cursor: ok ? 'pointer' : 'not-allowed',
                  opacity: ok ? 1 : 0.5, fontSize: 12,
                }}>
                  <input type="checkbox" disabled={!ok} onChange={() => ok && togglePkg(p.id)} checked={false} />
                  <span><b>{p.name}</b> {p.version}</span>
                  <span style={{
                    marginLeft: 'auto', fontSize: 9, padding: '0 4px', borderRadius: 2,
                    background: MODE_COLOR[p.capability], color: '#fff',
                  }}>{MODE_LABEL[p.capability]}</span>
                </label>
              )
            })}
          </div>
        </div>
      )}
    </div>
  )
}
