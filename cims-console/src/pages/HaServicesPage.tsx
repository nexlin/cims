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
import { haGroupsApi, type HaGroup, type VipBinding as ApiVipBinding } from '../api/ha_groups'
import { deploymentApi, type Agent, type SipPackage, type Deployment,
         type NetIface as ApiNetIface, type ServiceIpRow as ApiServiceIpRow } from '../api/deployment'

// ──────────────────────────────────────────────────────────────
//  Types
// ──────────────────────────────────────────────────────────────

type Mode = 'active_standby' | 'all_active' | 'standalone'
type Capability = Mode
type Role = 'master' | 'backup' | null
type ServerStatus = 'pending' | 'online' | 'offline'

/** IP slot — 패키지가 요구하는 IP 필드. scope='vip' 그룹 단위 / 'service' 서버 단위. */
interface IpSlot {
  scope: 'service' | 'vip'
  name: string
  port?: number
  proto?: 'tcp' | 'udp'
}

interface PkgDef {
  id: number
  name: string
  version: string
  description: string
  capability: Capability
  ipSlots: IpSlot[]
}

type NetIface = ApiNetIface
type BindingStatus = 'up' | 'down' | 'unknown'
type ServiceIpRow = ApiServiceIpRow & { status?: BindingStatus }
type VipBinding = ApiVipBinding

interface ServerRow {
  id: number                    // = Agent.id (음수 = pending placeholder)
  name: string
  role: Role
  ip: string | null             // = Agent.ip_address (mgmt)
  status: ServerStatus
  agent_version: string | null
  token: string                 // enrollment_token (one-time install command 용)
  interfaces: NetIface[]
  serviceIpRows: ServiceIpRow[]
}

interface ServiceRow {
  id: number                    // HaGroup.id (양수) 또는 -agent.id (standalone)
  name: string
  mode: Mode
  vrid: number | null
  vip: string                   // primary VIP (legacy field, vipBindings 와 별도)
  vipMask: number
  authPass: string
  servers: ServerRow[]
  packageIds: number[]          // derived from deployments
  vipBindings: VipBinding[]
}

// 패키지 이름 → IP slot 매핑 (config_template ip_slot 메타데이터 도입 전 임시 hardcoded).
// 실제 운영자는 ServiceIpPanel 에서 자유 입력 가능 (이 매핑은 hint 용).
const SLOT_MAP: Record<string, IpSlot[]> = {
  csc:     [
    { scope: 'service', name: 'Admin', port: 4420, proto: 'tcp' },
    { scope: 'service', name: 'McPTT', port: 4430, proto: 'tcp' },
    { scope: 'vip',     name: 'Admin', port: 4420, proto: 'tcp' },
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

function pkgToDef(p: SipPackage): PkgDef {
  return {
    id: p.id,
    name: p.name,
    version: p.version,
    description: p.description ?? '',
    capability: (p.meta?.ha_capability as Capability) ?? 'standalone',
    ipSlots: SLOT_MAP[p.name] ?? [],
  }
}

// ──────────────────────────────────────────────────────────────
//  Helpers
// ──────────────────────────────────────────────────────────────

const MODE_LABEL: Record<Mode, string> = {
  active_standby: 'A/S',
  all_active:     'AA',
  standalone:     'Standalone',
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

function agentToServer(a: Agent, role: Role): ServerRow {
  const status: ServerStatus =
    a.status === 'online' ? 'online'
    : a.status === 'offline' || a.status === 'error' ? 'offline'
    : 'pending'
  return {
    id: a.id,
    name: a.name,
    role,
    ip: a.ip_address,
    status,
    agent_version: a.agent_version,
    token: '',                                          // pending 만 set, AgentCreateResult.enrollment_token
    interfaces: a.interfaces ?? [],
    serviceIpRows: (a.service_ip_rows ?? []) as ServiceIpRow[],
  }
}

function buildInstallCommand(token: string, name: string, role: Role): string {
  const r = role ? ` --role ${role}` : ''
  return `curl -k https://CSC:4420/install-agent.sh | bash -s -- \\
  --csc-url https://CSC:4420 \\
  --enrollment-token ${token} \\
  --name ${name}${r}`
}

// ──────────────────────────────────────────────────────────────
//  Page
// ──────────────────────────────────────────────────────────────

export default function HaServicesPage() {
  const [haGroups, setHaGroups] = useState<HaGroup[]>([])
  const [agents, setAgents] = useState<Agent[]>([])
  const [packages, setPackages] = useState<SipPackage[]>([])
  const [deployments, setDeployments] = useState<Deployment[]>([])
  const [loading, setLoading] = useState(true)
  const [err, setErr] = useState<string>('')

  const [expanded, setExpanded] = useState<Set<number>>(new Set())
  const [adding, setAdding] = useState<{ name: string; mode: Mode | '' } | null>(null)
  const [editingName, setEditingName] = useState<{ kind: 'service' | 'server'; id: number; value: string } | null>(null)
  const [pkgPickerFor, setPkgPickerFor] = useState<number | null>(null)
  const [vipExpandFor, setVipExpandFor] = useState<number | null>(null)
  const [svcIpExpandFor, setSvcIpExpandFor] = useState<number | null>(null)
  const [toast, setToast] = useState<string | null>(null)
  // pending agent 신규 생성 직후 1회용 enrollment_token + install command
  const [pendingTokens, setPendingTokens] = useState<Map<number, { token: string; cmd: string }>>(new Map())

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
        return agentToServer(a, g.mode === 'active_standby' ? m.role : null)
      }).filter((s): s is ServerRow => s !== null)
      const agentIds = new Set(members.map(m => m.agent_id))
      const groupDeps = deployments.filter(d => agentIds.has(d.agent_id))
      const pkgIds = Array.from(new Set(groupDeps.map(d => d.package_id)))
      out.push({
        id: g.id,
        name: g.name,
        mode: g.mode,
        vrid: g.vrid,
        vip: g.vip,
        vipMask: g.vip_mask,
        authPass: g.auth_pass,
        servers,
        packageIds: pkgIds,
        vipBindings: g.vip_bindings ?? [],
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

  const toggleExpand = (id: number) => setExpanded(prev => {
    const n = new Set(prev)
    if (n.has(id)) n.delete(id); else n.add(id)
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
        setPendingTokens(prev => new Map(prev).set(r.id, { token: r.enrollment_token, cmd: r.install_command }))
      } else {
        // A/S = 멤버 2개, AA = 멤버 1개 (운영자가 추가 가능)
        const memberCount = mode === 'active_standby' ? 2 : 1
        const memberAgents: Array<{ agent: Agent; token: string; cmd: string }> = []
        for (let i = 1; i <= memberCount; i++) {
          const r = await deploymentApi.createAgent(`${baseName}-${pad2(i)}`, '')
          memberAgents.push({ agent: r, token: r.enrollment_token, cmd: r.install_command })
        }
        // ha_group 생성 — vip 는 빈 값 (운영자가 VIP panel 에서 설정)
        const gres = await haGroupsApi.create({
          name: baseName,
          mode,
          vip: '0.0.0.0',                              // 임시 placeholder — 추후 VipPanel 에서 update
          vip_mask: 24,
          auth_pass: '00000000',                       // 임시 placeholder
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
        setExpanded(prev => new Set([...prev, gres.id]))
      }
      flash(`서비스 "${baseName}" 추가 (${MODE_LABEL[mode]})`)
      setAdding(null)
      await load()
    } catch (e) {
      flash(`서비스 생성 실패: ${(e as Error).message}`)
    }
  }

  // ── 서버 추가 (AA/Standalone 만) ──
  const addServer = async (svc: ServiceRow) => {
    if (svc.mode === 'active_standby') return
    const idx = svc.servers.length + 1
    try {
      const r = await deploymentApi.createAgent(`${svc.name}-${pad2(idx)}`, '')
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

  // ── 토큰 재발행 — 현 agent 삭제 + 신규 agent 생성 (같은 이름) ──
  const regenerateToken = async (svc: ServiceRow, srv: ServerRow) => {
    if (srv.status === 'online') {
      if (!confirm(`${srv.name} 은 이미 online — 재발행 시 기존 agent 인증 무효. 진행?`)) return
    }
    try {
      // 기존 agent 삭제 (HA member 도 자동 cascade)
      await deploymentApi.deleteAgent(srv.id)
      // 신규 생성
      const r = await deploymentApi.createAgent(srv.name, '')
      setPendingTokens(prev => new Map(prev).set(r.id, { token: r.enrollment_token, cmd: r.install_command }))
      // HA group 이면 멤버로 재등록
      if (svc.id > 0) {
        await haGroupsApi.addMember(svc.id, {
          agent_id: r.id,
          role: srv.role ?? 'backup',
          priority: srv.role === 'master' ? 100 : 90,
        })
      }
      try {
        await navigator.clipboard.writeText(r.install_command)
        flash(`${srv.name} 토큰 재발행 + clipboard 복사`)
      } catch {
        flash(`${srv.name} 토큰 재발행 (clipboard 권한 없음)`)
      }
      await load()
    } catch (e) {
      flash(`토큰 재발행 실패: ${(e as Error).message}`)
    }
  }

  // ── install command 복사 ──
  const copyInstallCmd = async (srv: ServerRow) => {
    const pt = pendingTokens.get(srv.id)
    const cmd = pt ? pt.cmd : buildInstallCommand(srv.token || '(토큰 만료)', srv.name, srv.role)
    try {
      await navigator.clipboard.writeText(cmd)
      flash(`${srv.name} install command 복사됨`)
    } catch {
      flash('clipboard 권한 없음')
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
                                                     background: '#fff', border: '1px solid #ccc',
                                                     borderRadius: 3, cursor: 'pointer' }}>↻ 새로고침</button>
      </div>
      <div style={{ color: '#666', fontSize: 13, marginBottom: 16 }}>
        서비스(=HA 그룹/단독) 단위로 서버를 묶어 관리. 유형 선택 시 자동으로 서버 발급(A/S=2, AA/Standalone=1).
        모든 추가/편집은 list 행 안 inline (팝업 없음).
      </div>
      {err && <div style={{ color: '#c00', marginBottom: 12, fontSize: 12 }}>오류: {err}</div>}

      <div style={{ flex: 1, overflow: 'auto', minHeight: 0,
                    border: '1px solid #e0e0e0', borderRadius: 6, background: '#fff' }}>
      <table style={{ width: '100%', borderCollapse: 'collapse', background: '#fff' }}>
        <thead style={{ position: 'sticky', top: 0, zIndex: 1 }}>
          <tr style={{ background: '#f7f8fa', fontSize: 12, color: '#666' }}>
            <th style={th(60)}>#</th>
            <th style={thLeft(240)}>이름</th>
            <th style={th(110)}>유형</th>
            <th style={thLeft(140)}>mgmt IP</th>
            <th style={thLeft(220)}>IP (서비스 / VIP·VRID)</th>
            <th style={th(120)}>상태</th>
            <th style={th(150)}>액션</th>
          </tr>
        </thead>
        <tbody>
          {services.length === 0 && (
            <tr>
              <td colSpan={7} style={{ padding: 24, color: '#888', textAlign: 'center' }}>
                등록된 서비스/서버 없음 — 아래 [＋ 시스템 추가] 로 생성
              </td>
            </tr>
          )}
          {services.map((svc, sIdx) => (
            <ServiceTreeRows
              key={svc.id}
              svc={svc} idx={sIdx + 1}
              expanded={expanded.has(svc.id)}
              onToggle={() => toggleExpand(svc.id)}
              editingName={editingName}
              setEditingName={setEditingName}
              pkgPickerOpen={pkgPickerFor === svc.id}
              setPkgPicker={(open) => setPkgPickerFor(open ? svc.id : null)}
              vipExpanded={vipExpandFor === svc.id}
              setVipExpand={(open) => setVipExpandFor(open ? svc.id : null)}
              svcIpExpandFor={svcIpExpandFor}
              setSvcIpExpand={(srvId, open) => setSvcIpExpandFor(open ? srvId : null)}
              vipSlots={slotsForService(svc, 'vip')}
              serviceSlots={slotsForService(svc, 'service')}
              packageMap={packageMap}
              pendingTokens={pendingTokens}
              updateService={updateService}
              updateServer={updateServer}
              updatePackageIds={updatePackageIds}
              addServer={() => addServer(svc)}
              regenerateToken={(srv) => regenerateToken(svc, srv)}
              copyCmd={(srv) => copyInstallCmd(srv)}
              onDelete={() => deleteService(svc)}
            />
          ))}

          {/* 인라인 시스템 추가 행 */}
          {adding && (
            <tr style={{ background: '#f0f8ff' }}>
              <td style={td(60)}>{services.length + 1}</td>
              <td style={tdLeft()}>
                <input value={adding.name} onChange={e => setAdding({ ...adding, name: e.target.value })}
                       placeholder="예: VoLTE SIP Server"
                       style={{ width: '95%', padding: '4px 8px' }} autoFocus />
              </td>
              <td style={td(110)}>
                <select value={adding.mode} onChange={e => setAdding({ ...adding, mode: e.target.value as Mode })}
                        style={{ width: '95%' }}>
                  <option value="">유형 선택</option>
                  <option value="active_standby">A/S (자식 2)</option>
                  <option value="all_active">AA (자식 N)</option>
                  <option value="standalone">Standalone (자식 N)</option>
                </select>
              </td>
              <td colSpan={3} style={tdLeft()}>
                <span style={{ fontSize: 11, color: '#888' }}>
                  생성 후 mgmt IP / 서비스 IP / VIP / auth_pass 편집 + 서버 토큰 자동 발급
                </span>
              </td>
              <td style={td(150)}>
                <button onClick={() => void createService()} style={btnPrimary()}>생성</button>
                <button onClick={() => setAdding(null)} style={btnSecondary()}>취소</button>
              </td>
            </tr>
          )}
        </tbody>
      </table>
      </div>

      <div style={{ marginTop: 12, flexShrink: 0 }}>
        {!adding && (
          <button onClick={() => setAdding({ name: '', mode: '' })} style={btnAdd()}>
            ＋ 시스템 추가
          </button>
        )}
      </div>

      {/* 토스트 */}
      {toast && (
        <div style={{
          position: 'fixed', bottom: 24, left: '50%', transform: 'translateX(-50%)',
          background: '#333', color: '#fff', padding: '8px 16px', borderRadius: 4,
          fontSize: 13, zIndex: 1000,
        }}>{toast}</div>
      )}
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  ServiceTreeRows — service row + server rows + [＋ 서버 추가] row + packages row
// ──────────────────────────────────────────────────────────────

interface ServiceTreeProps {
  svc: ServiceRow
  idx: number
  expanded: boolean
  onToggle: () => void
  editingName: { kind: 'service' | 'server'; id: number; value: string } | null
  setEditingName: (v: { kind: 'service' | 'server'; id: number; value: string } | null) => void
  pkgPickerOpen: boolean
  setPkgPicker: (open: boolean) => void
  vipExpanded: boolean
  setVipExpand: (open: boolean) => void
  svcIpExpandFor: number | null
  setSvcIpExpand: (srvId: number, open: boolean) => void
  vipSlots: IpSlot[]
  serviceSlots: IpSlot[]
  packageMap: Map<number, PkgDef>
  pendingTokens: Map<number, { token: string; cmd: string }>
  updateService: (sid: number, patch: Partial<ServiceRow>) => void
  updateServer: (sid: number, srvId: number, patch: Partial<ServerRow>) => void
  updatePackageIds: (svc: ServiceRow, ids: number[]) => void
  addServer: () => void
  regenerateToken: (srv: ServerRow) => void
  copyCmd: (srv: ServerRow) => void
  onDelete: () => void
}

function ServiceTreeRows(p: ServiceTreeProps) {
  const { svc, idx, expanded, onToggle } = p
  const isStandalone = svc.mode === 'standalone'
  const canAddServer = svc.mode !== 'active_standby'

  return (
    <>
      <tr style={{ borderTop: '2px solid #e0e0e0', background: '#fafbfc' }}>
        <td style={td(60)}>
          <button onClick={onToggle} style={{ background: 'none', border: 'none', cursor: 'pointer', fontSize: 11 }}>
            {expanded ? '▼' : '▶'} {idx}
          </button>
        </td>
        <td style={tdLeft()}>
          <InlineNameEdit kind="service" id={svc.id} value={svc.name}
                          editing={p.editingName}
                          onStart={(v) => p.setEditingName({ kind: 'service', id: svc.id, value: v })}
                          onChange={(v) => p.setEditingName(p.editingName ? { ...p.editingName, value: v } : null)}
                          onSave={(v) => { p.updateService(svc.id, { name: v }); p.setEditingName(null) }}
                          onCancel={() => p.setEditingName(null)}
                          bold />
        </td>
        <td style={td(110)}>
          <ModeBadge mode={svc.mode} />
        </td>
        <td style={tdLeft(140)}>
          <span style={{ color: '#aaa', fontSize: 12 }}>—</span>
        </td>
        <td style={tdLeft(220)}>
          {isStandalone || p.vipSlots.length === 0 ? (
            <span style={{ color: '#aaa', fontSize: 12 }}>—</span>
          ) : (
            <button onClick={() => p.setVipExpand(!p.vipExpanded)} style={chipBtn(p.vipExpanded)}>
              📡 VIP {svc.vipBindings.length}/{p.vipSlots.length} (VRID {svc.vrid}) {p.vipExpanded ? '▲' : '▼'}
            </button>
          )}
        </td>
        <td style={td(120)}>
          <StatusSummary servers={svc.servers} mode={svc.mode} />
        </td>
        <td style={td(150)}>
          <button onClick={p.onDelete} style={btnDanger()}>삭제</button>
        </td>
      </tr>

      {p.vipExpanded && p.vipSlots.length > 0 && (
        <tr>
          <td colSpan={7} style={{ padding: '8px 16px 12px 60px' }}>
            <VipPanel
              title="VIP (서비스 단위 — A/S fail-over 공유. 멤버 별 iface 분리 매핑)"
              svc={svc}
              vrid={svc.vrid}
              onChange={(bindings) => p.updateService(svc.id, { vipBindings: bindings })}
            />
          </td>
        </tr>
      )}

      {expanded && svc.servers.map((srv, srvIdx) => (
        <ServerRows
          key={srv.id}
          svc={svc} srv={srv} idx={idx} srvIdx={srvIdx}
          serviceSlots={p.serviceSlots}
          editingName={p.editingName}
          setEditingName={p.setEditingName}
          svcIpExpanded={p.svcIpExpandFor === srv.id}
          setSvcIpExpand={(open) => p.setSvcIpExpand(srv.id, open)}
          pendingToken={p.pendingTokens.get(srv.id)}
          updateServer={p.updateServer}
          regenerateToken={p.regenerateToken}
          copyCmd={p.copyCmd}
        />
      ))}

      {expanded && canAddServer && (
        <tr style={{ background: '#fcfdfe' }}>
          <td style={td(60)}></td>
          <td colSpan={6} style={{ padding: '6px 12px' }}>
            <button onClick={p.addServer} style={btnAdd(true)}>
              ＋ 서버 추가 ({MODE_LABEL[svc.mode]} — 신규 토큰 발행)
            </button>
          </td>
        </tr>
      )}

      {expanded && (
        <tr style={{ background: '#fcfdfe' }}>
          <td style={td(60)}></td>
          <td colSpan={6} style={{ padding: '6px 12px' }}>
            <PackagesArea svc={svc}
                          packageMap={p.packageMap}
                          pickerOpen={p.pkgPickerOpen}
                          setPickerOpen={p.setPkgPicker}
                          onChange={(ids) => p.updatePackageIds(svc, ids)} />
          </td>
        </tr>
      )}
    </>
  )
}

// ──────────────────────────────────────────────────────────────
//  ServerRows
// ──────────────────────────────────────────────────────────────

interface ServerRowsProps {
  svc: ServiceRow
  srv: ServerRow
  idx: number
  srvIdx: number
  serviceSlots: IpSlot[]
  editingName: { kind: 'service' | 'server'; id: number; value: string } | null
  setEditingName: (v: { kind: 'service' | 'server'; id: number; value: string } | null) => void
  svcIpExpanded: boolean
  setSvcIpExpand: (open: boolean) => void
  pendingToken?: { token: string; cmd: string }
  updateServer: (sid: number, srvId: number, patch: Partial<ServerRow>) => void
  regenerateToken: (srv: ServerRow) => void
  copyCmd: (srv: ServerRow) => void
}

function ServerRows(p: ServerRowsProps) {
  const { svc, srv, idx, srvIdx } = p
  const enrollDone = srv.status !== 'pending'
  return (
    <>
      <tr style={{ background: '#fff' }}>
        <td style={td(60)}>
          <span style={{ color: '#888', fontSize: 12, paddingLeft: 16 }}>{idx}.{srvIdx + 1}</span>
        </td>
        <td style={tdLeft()}>
          <InlineNameEdit kind="server" id={srv.id} value={srv.name}
                          editing={p.editingName}
                          onStart={(v) => p.setEditingName({ kind: 'server', id: srv.id, value: v })}
                          onChange={(v) => p.setEditingName(p.editingName ? { ...p.editingName, value: v } : null)}
                          onSave={(v) => { p.updateServer(svc.id, srv.id, { name: v }); p.setEditingName(null) }}
                          onCancel={() => p.setEditingName(null)} />
          {srv.role && (
            <span style={{ marginLeft: 8, fontSize: 10, padding: '1px 5px', borderRadius: 3,
                           background: srv.role === 'master' ? '#e67e22' : '#7f8c8d', color: '#fff' }}>
              {srv.role}
            </span>
          )}
        </td>
        <td style={td(110)}></td>
        <td style={tdLeft(140)}>
          <span style={{ fontSize: 12, color: srv.ip ? '#333' : '#aaa' }}>
            {srv.ip ?? '— (enroll 후 자동)'}
          </span>
        </td>
        <td style={tdLeft(220)}>
          {p.serviceSlots.length === 0 ? (
            <span style={{ color: '#aaa', fontSize: 12 }}>— (패키지 없음)</span>
          ) : !enrollDone ? (
            <span style={{ color: '#aaa', fontSize: 12 }} title="enroll 전 — 인터페이스 정보 없음">
              ⏳ enroll 대기
            </span>
          ) : (
            <button onClick={() => p.setSvcIpExpand(!p.svcIpExpanded)} style={chipBtn(p.svcIpExpanded)}>
              📡 인터페이스 {srv.interfaces.length}개 / 용도 {srv.serviceIpRows.filter(r => r.slot).length}/{p.serviceSlots.length} {p.svcIpExpanded ? '▲' : '▼'}
            </button>
          )}
        </td>
        <td style={td(120)}>
          <span style={{ color: STATUS_COLOR[srv.status], fontWeight: 'bold' }}>
            {STATUS_ICON[srv.status]} {srv.status}
          </span>
          {srv.agent_version && <span style={{ marginLeft: 6, fontSize: 10, color: '#888' }}>v{srv.agent_version}</span>}
        </td>
        <td style={td(150)}>
          <button onClick={() => p.copyCmd(srv)} style={btnSmall()}
                  disabled={!p.pendingToken && srv.status !== 'pending'}
                  title={p.pendingToken ? '신규 발급 토큰' : (srv.status === 'pending' ? '대기' : '이미 enroll 완료')}>
            📋 복사
          </button>
          {srv.status !== 'online' && (
            <button onClick={() => p.regenerateToken(srv)} style={btnSmall()}>↻ 토큰</button>
          )}
        </td>
      </tr>

      {p.svcIpExpanded && enrollDone && p.serviceSlots.length > 0 && (
        <tr>
          <td colSpan={7} style={{ padding: '8px 16px 12px 60px' }}>
            <ServiceIpPanel
              title={`${srv.name} 의 인터페이스 IP 매핑`}
              interfaces={srv.interfaces}
              rows={srv.serviceIpRows}
              slots={p.serviceSlots}
              onChange={(rows) => p.updateServer(svc.id, srv.id, { serviceIpRows: rows })}
            />
          </td>
        </tr>
      )}
    </>
  )
}

// ──────────────────────────────────────────────────────────────
//  ServiceIpPanel — 인터페이스 단위 row
// ──────────────────────────────────────────────────────────────

function ServiceIpPanel({ title, interfaces, rows, slots, onChange }: {
  title: string
  interfaces: NetIface[]
  rows: ServiceIpRow[]
  slots: IpSlot[]
  onChange: (rows: ServiceIpRow[]) => void
}) {
  const ifaceRows: ServiceIpRow[] = interfaces.map(iface => {
    const existing = rows.find(r => r.iface === iface.name)
    return existing ?? {
      iface: iface.name, ip: iface.ip, mask: iface.mask, slot: '', status: 'unknown',
    }
  })
  const slotHints = slots.map(s => s.name).join(' / ')

  const updateRow = (iface: string, patch: Partial<ServiceIpRow>) => {
    const next = ifaceRows.map(r => r.iface === iface ? { ...r, ...patch } : r)
    onChange(next)
  }

  const applyRow = (iface: string) => {
    updateRow(iface, { status: 'unknown' })
    // TODO: agent 에 실제 ip 적용 API (별도 endpoint 필요). 현재는 변경값만 저장.
  }

  const resetRow = (iface: string) => {
    const initial = interfaces.find(x => x.name === iface)
    if (!initial) return
    updateRow(iface, { ip: initial.ip, mask: initial.mask, status: 'unknown' })
  }

  const isChanged = (r: ServiceIpRow): boolean => {
    const initial = interfaces.find(x => x.name === r.iface)
    return !!initial && (initial.ip !== r.ip || initial.mask !== r.mask)
  }

  return (
    <div style={{
      borderLeft: '3px solid #b8d4f5', borderRadius: 4, padding: '10px 12px',
      background: '#fafcfe',
    }}>
      <div style={{ fontSize: 12, fontWeight: 'bold', color: '#555', marginBottom: 8 }}>
        {title}
        <span style={{ marginLeft: 8, fontSize: 11, color: '#888', fontWeight: 'normal' }}>
          (행 = 인터페이스 — agent 보고. IP / 용도 만 편집 가능)
        </span>
      </div>

      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ background: '#f5f5f5', color: '#666' }}>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 40 }}>#</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 90 }}>인터페이스</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 180 }}>IP / mask</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 140 }}>용도</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 100 }}>상태</th>
            <th style={{ padding: '4px 8px', textAlign: 'left' }}>액션</th>
          </tr>
        </thead>
        <tbody>
          {ifaceRows.length === 0 && (
            <tr>
              <td colSpan={6} style={{ padding: '8px', color: '#aaa' }}>
                (인터페이스 없음 — agent 보고 대기)
              </td>
            </tr>
          )}
          {ifaceRows.map((r, i) => {
            const changed = isChanged(r)
            return (
              <tr key={r.iface}>
                <td style={{ padding: '4px 8px', color: '#888' }}>{i + 1}</td>
                <td style={{ padding: '4px 8px', fontFamily: 'monospace' }}>
                  <b>{r.iface}</b>
                </td>
                <td style={{ padding: '4px 8px' }}>
                  <span style={{ display: 'inline-flex', gap: 2, alignItems: 'center' }}>
                    <input value={r.ip}
                           onChange={e => updateRow(r.iface, { ip: e.target.value, status: 'unknown' })}
                           style={{ width: 110, padding: '2px 6px', fontSize: 12,
                                    border: `1px solid ${changed ? '#e67e22' : '#ddd'}`,
                                    borderRadius: 3 }} />
                    <span>/</span>
                    <input type="number" value={r.mask}
                           onChange={e => updateRow(r.iface, { mask: parseInt(e.target.value) || 24, status: 'unknown' })}
                           style={{ width: 40, padding: '2px 6px', fontSize: 12,
                                    border: `1px solid ${changed ? '#e67e22' : '#ddd'}`,
                                    borderRadius: 3 }} />
                    {changed && (
                      <span style={{ marginLeft: 4, fontSize: 10, color: '#e67e22' }}
                            title="agent 보고 IP 와 다름 — [적용] 또는 [초기화]">변경됨</span>
                    )}
                  </span>
                </td>
                <td style={{ padding: '4px 8px' }}>
                  <input value={r.slot}
                         onChange={e => updateRow(r.iface, { slot: e.target.value })}
                         placeholder="(용도 입력)"
                         style={{ width: '95%', padding: '2px 6px', fontSize: 12,
                                  border: '1px solid #ddd', borderRadius: 3 }} />
                </td>
                <td style={{ padding: '4px 8px' }}><StatusBadge status={r.status} /></td>
                <td style={{ padding: '4px 8px' }}>
                  <button onClick={() => applyRow(r.iface)} style={btnSmall()}
                          title="변경된 IP 를 저장 (실제 agent 적용 API 추후)">
                    적용
                  </button>
                  <button onClick={() => resetRow(r.iface)} style={btnSmall()}
                          disabled={!changed}
                          title={changed ? 'agent 가 보고한 initial IP 로 되돌림' : '변경 없음'}>
                    초기화
                  </button>
                </td>
              </tr>
            )
          })}
        </tbody>
      </table>
      {slots.length > 0 && (
        <div style={{ marginTop: 8, fontSize: 11, color: '#888' }}>
          ℹ 참고 — 설치된 패키지의 권장 용도: <code>{slotHints}</code> (자유 입력 가능)
        </div>
      )}
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  VipPanel — VIP slot 단위 row
// ──────────────────────────────────────────────────────────────

function VipPanel({ title, svc, vrid, onChange }: {
  title: string
  svc: ServiceRow
  vrid?: number | null
  onChange: (bindings: VipBinding[]) => void
}) {
  const bindings = svc.vipBindings
  const servers = svc.servers

  const slotMap = new Map<string, Map<number, string>>()
  for (const srv of servers) {
    for (const r of srv.serviceIpRows) {
      if (!r.slot) continue
      if (!slotMap.has(r.slot)) slotMap.set(r.slot, new Map())
      slotMap.get(r.slot)!.set(srv.id, r.iface)
    }
  }
  const availableSlots = Array.from(slotMap.keys()).sort()

  const autoMapMemberIfaces = (slot: string): { [id: number]: string } => {
    const result: { [id: number]: string } = {}
    const ifaceMap = slotMap.get(slot)
    if (ifaceMap) for (const [sid, iface] of ifaceMap) result[sid] = iface
    return result
  }

  const addRow = () => {
    const newId = Math.max(0, ...bindings.map(b => b.bid)) + 1
    onChange([...bindings, {
      bid: newId, slot: '', ip: '', mask: 24, status: 'unknown', memberIfaces: {},
    }])
  }
  const updateRow = (bid: number, patch: Partial<VipBinding>) =>
    onChange(bindings.map(b => b.bid === bid ? { ...b, ...patch } : b))
  const removeRow = (bid: number) => onChange(bindings.filter(b => b.bid !== bid))

  const onSlotChange = (bid: number, newSlot: string) => {
    updateRow(bid, {
      slot: newSlot,
      memberIfaces: newSlot ? autoMapMemberIfaces(newSlot) : {},
      status: 'unknown',
    })
  }

  const applyRow = (bid: number) => {
    updateRow(bid, { status: 'unknown' })
    // TODO: keepalived config render + reload API. 현재는 변경값 저장만.
  }

  return (
    <div style={{
      borderLeft: '3px solid #b8d4f5', borderRadius: 4, padding: '10px 12px',
      background: '#fafcfe',
    }}>
      <div style={{ fontSize: 12, fontWeight: 'bold', color: '#555', marginBottom: 10 }}>
        {title}
        <span style={{ marginLeft: 8, fontSize: 11, color: '#888', fontWeight: 'normal' }}>
          (용도 선택 시 멤버별 iface 자동 매핑 — 운영자 수동 override 가능)
        </span>
      </div>

      {availableSlots.length === 0 && (
        <div style={{ padding: 10, background: '#fff8db', border: '1px solid #f0c75e',
                      borderRadius: 4, fontSize: 12, color: '#876200', marginBottom: 8 }}>
          ⚠ 멤버 서버의 ServiceIpRow 에 "용도" 가 설정된 항목이 없습니다. 먼저 각 서버의
          인터페이스에 용도를 입력해야 VIP 의 용도 select 에 옵션이 표시됩니다.
        </div>
      )}

      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ background: '#f5f5f5', color: '#666' }}>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 40 }}>#</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 140 }}>용도</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 170 }}>VIP / mask</th>
            {servers.map(s => (
              <th key={s.id} style={{ padding: '4px 8px', textAlign: 'left' }}>
                {s.name} {s.role && <span style={{ fontSize: 10, color: '#888' }}>({s.role})</span>}
              </th>
            ))}
            {vrid != null && <th style={{ padding: '4px 8px', textAlign: 'left', width: 60 }}>VRID</th>}
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 90 }}>상태</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 130 }}>액션</th>
          </tr>
        </thead>
        <tbody>
          {bindings.length === 0 && (
            <tr>
              <td colSpan={4 + servers.length + (vrid != null ? 1 : 0) + 1} style={{ padding: '8px', color: '#aaa' }}>
                (VIP 없음 — 아래 [＋ VIP 추가])
              </td>
            </tr>
          )}
          {bindings.map((b, i) => {
            const usedSlots = new Set(bindings.map(x => x.slot).filter(Boolean))
            return (
              <tr key={b.bid}>
                <td style={{ padding: '4px 8px', color: '#888' }}>{i + 1}</td>
                <td style={{ padding: '4px 8px' }}>
                  <select value={b.slot} onChange={e => onSlotChange(b.bid, e.target.value)}
                          style={{ width: '95%', padding: '2px 4px', fontSize: 12,
                                   color: b.slot ? '#333' : '#c00' }}>
                    <option value="">(용도 선택)</option>
                    {availableSlots.map(name => (
                      <option key={name} value={name}
                              disabled={usedSlots.has(name) && b.slot !== name}>
                        {name}{usedSlots.has(name) && b.slot !== name ? ' (사용중)' : ''}
                      </option>
                    ))}
                  </select>
                </td>
                <td style={{ padding: '4px 8px' }}>
                  <span style={{ display: 'inline-flex', gap: 2, alignItems: 'center' }}>
                    <input value={b.ip}
                           onChange={e => updateRow(b.bid, { ip: e.target.value, status: 'unknown' })}
                           placeholder="(VIP)"
                           style={{ width: 110, padding: '2px 6px', fontSize: 12,
                                    border: '1px solid #ddd', borderRadius: 3 }} />
                    <span>/</span>
                    <input type="number" value={b.mask ?? 24}
                           onChange={e => updateRow(b.bid, { mask: parseInt(e.target.value) || 24 })}
                           style={{ width: 40, padding: '2px 6px', fontSize: 12,
                                    border: '1px solid #ddd', borderRadius: 3 }} />
                  </span>
                </td>
                {servers.map(s => {
                  const autoIface = slotMap.get(b.slot)?.get(s.id)
                  const currentIface = b.memberIfaces?.[s.id] ?? ''
                  const overridden = autoIface && currentIface !== autoIface
                  return (
                    <td key={s.id} style={{ padding: '4px 8px' }}>
                      <select value={currentIface}
                              onChange={e => updateRow(b.bid, {
                                memberIfaces: { ...(b.memberIfaces ?? {}), [s.id]: e.target.value },
                                status: 'unknown',
                              })}
                              style={{ width: '95%', padding: '2px 4px', fontSize: 12,
                                       color: overridden ? '#e67e22' : (currentIface ? '#333' : '#aaa') }}>
                        <option value="">{s.interfaces.length === 0 ? '(enroll 대기)' : '(iface)'}</option>
                        {s.interfaces.map(iface => (
                          <option key={iface.name} value={iface.name}>
                            {iface.name} ({iface.ip})
                          </option>
                        ))}
                      </select>
                      {!currentIface && b.slot && (
                        <div style={{ fontSize: 10, color: '#c0392b', marginTop: 2 }}>
                          ⚠ "{b.slot}" 미설정
                        </div>
                      )}
                    </td>
                  )
                })}
                {vrid != null && (
                  <td style={{ padding: '4px 8px', color: '#666' }}>{vrid}</td>
                )}
                <td style={{ padding: '4px 8px' }}><StatusBadge status={b.status} /></td>
                <td style={{ padding: '4px 8px' }}>
                  <button onClick={() => applyRow(b.bid)} style={btnSmall()} title="VIP 적용 + up 확인">적용</button>
                  <button onClick={() => removeRow(b.bid)} style={btnSmall()} title="row 제거">삭제</button>
                </td>
              </tr>
            )
          })}
        </tbody>
      </table>

      <div style={{ marginTop: 8 }}>
        <button onClick={addRow} style={btnAdd(true)}
                disabled={availableSlots.length === 0}>
          ＋ VIP 추가 {availableSlots.length === 0 && '(서버 용도 선설정 필요)'}
        </button>
      </div>
    </div>
  )
}

function StatusBadge({ status }: { status?: BindingStatus }) {
  const s = status ?? 'unknown'
  const map: Record<BindingStatus, { icon: string; color: string; label: string }> = {
    up:      { icon: '●', color: '#27ae60', label: 'up' },
    down:    { icon: '◐', color: '#c0392b', label: 'down' },
    unknown: { icon: '○', color: '#888',    label: '미확인' },
  }
  const m = map[s]
  return (
    <span style={{ fontSize: 12, color: m.color, fontWeight: 'bold' }}>
      {m.icon} {m.label}
    </span>
  )
}

function ModeBadge({ mode }: { mode: Mode }) {
  return (
    <span style={{
      fontSize: 11, padding: '2px 6px', borderRadius: 3,
      background: MODE_COLOR[mode], color: '#fff',
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
      {cap && online < cap && <span style={{ marginLeft: 4, color: '#888' }}>(pending {cap - online})</span>}
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
      <div style={{ fontSize: 12, color: '#666', display: 'flex', alignItems: 'center', gap: 6, flexWrap: 'wrap' }}>
        <span>▸ 패키지 ({MODE_LABEL[svc.mode]} 가능):</span>
        {installed.length === 0 && <span style={{ color: '#aaa' }}>(없음)</span>}
        {installed.map(p => (
          <span key={p.id} style={{
            fontSize: 11, padding: '2px 6px', borderRadius: 3,
            background: '#e8f5e9', border: '1px solid #c8e6c9', color: '#2e7d32',
            display: 'inline-flex', alignItems: 'center', gap: 4,
          }}>
            {p.name} {p.version}
            <button onClick={() => removePkg(p.id)}
                    style={{ background: 'none', border: 'none', cursor: 'pointer', color: '#777' }}>×</button>
          </span>
        ))}
        <button onClick={() => setPickerOpen(!pickerOpen)} style={btnSmall()}>
          {pickerOpen ? '✕ 닫기' : '＋ 패키지 추가'}
        </button>
      </div>

      {pickerOpen && (
        <div style={{ marginTop: 8, padding: 10, background: '#fff',
                      border: '1px dashed #c0c0c0', borderRadius: 4 }}>
          <div style={{ fontSize: 11, color: '#888', marginBottom: 6 }}>
            ℹ {MODE_LABEL[targetMode]} 가능 또는 standalone 모듈만 선택 가능 — 서비스 모든 서버에 일괄 설치
          </div>
          {available.length === 0 && <div style={{ color: '#aaa' }}>(추가 가능한 패키지 없음)</div>}
          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(200px, 1fr))', gap: 6 }}>
            {available.map(p => {
              const ok = p.capability === targetMode || p.capability === 'standalone'
              return (
                <label key={p.id} style={{
                  display: 'flex', alignItems: 'center', gap: 6,
                  padding: 6, border: '1px solid #e5e5e5', borderRadius: 3,
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

// ──────────────────────────────────────────────────────────────
//  Style helpers
// ──────────────────────────────────────────────────────────────

function th(width: number): React.CSSProperties {
  return { padding: '8px 10px', textAlign: 'center', width, fontWeight: 'normal',
           borderBottom: '1px solid #e0e0e0' }
}
function thLeft(width?: number): React.CSSProperties {
  return { padding: '8px 10px', textAlign: 'left', width, fontWeight: 'normal',
           borderBottom: '1px solid #e0e0e0' }
}
function td(width: number): React.CSSProperties {
  return { padding: '6px 10px', textAlign: 'center', width, fontSize: 13,
           borderBottom: '1px solid #f0f0f0' }
}
function tdLeft(width?: number): React.CSSProperties {
  return { padding: '6px 10px', textAlign: 'left', width, fontSize: 13,
           borderBottom: '1px solid #f0f0f0' }
}
function btnSmall(): React.CSSProperties {
  return { fontSize: 11, padding: '2px 8px', marginLeft: 4, cursor: 'pointer',
           background: '#fff', border: '1px solid #ccc', borderRadius: 3 }
}
function btnPrimary(): React.CSSProperties {
  return { fontSize: 12, padding: '4px 12px', marginRight: 4, cursor: 'pointer',
           background: '#3498db', color: '#fff', border: 'none', borderRadius: 3 }
}
function btnSecondary(): React.CSSProperties {
  return { fontSize: 12, padding: '4px 12px', cursor: 'pointer',
           background: '#fff', border: '1px solid #ccc', borderRadius: 3 }
}
function btnDanger(): React.CSSProperties {
  return { fontSize: 11, padding: '2px 8px', cursor: 'pointer',
           background: '#fff', border: '1px solid #c0392b', color: '#c0392b', borderRadius: 3 }
}
function btnAdd(small = false): React.CSSProperties {
  return { fontSize: small ? 11 : 13, padding: small ? '3px 10px' : '6px 16px',
           cursor: 'pointer', background: '#f5f9ff', border: '1px dashed #3498db',
           color: '#3498db', borderRadius: 3 }
}
function chipBtn(open: boolean): React.CSSProperties {
  return { fontSize: 12, padding: '2px 8px', cursor: 'pointer',
           background: open ? '#e8f0fe' : '#fff',
           border: '1px solid #b8d4f5', borderRadius: 12, color: '#1a73e8' }
}
