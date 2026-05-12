/**
 * HaServicesPage — "서비스(=HA 그룹/단독) → 서버" 트리 list (mock-up prototype).
 *
 * 한 페이지에서 그룹(서비스) 정의 + 서버 자동 발급 + 패키지 일괄 설치 모두 inline 편집.
 * 팝업 없음 — 모든 추가/편집은 행 안에서 expand.
 *
 * 본 페이지는 **prototype** — mock data, 실제 API 호출 없음. 운영자 UX 검증용.
 * 후속: 실제 wiring + 기존 ServersPage/HaGroupsPage 통합/폐기.
 */
import { useMemo, useState } from 'react'

// ──────────────────────────────────────────────────────────────
//  Types + mock data
// ──────────────────────────────────────────────────────────────

type Mode = 'active_standby' | 'all_active' | 'standalone'
type Capability = Mode
type Role = 'master' | 'backup' | null
type ServerStatus = 'pending' | 'online' | 'offline'

/** IP slot — 패키지 config 가 요구하는 IP 필드. scope 가 'vip' 면 그룹 단위,
 *  'service' 면 서버 단위. 실제 backend 에서는 config_template.json 의 attribute
 *  로 정의됨 (예: {"key": "Setup.Sip.LocalIp", "ip_scope": "service", "ip_slot": "SIP"}).
 */
interface IpSlot {
  scope: 'service' | 'vip'
  name: string            // 'SIP' / 'Admin' / 'Stats' / 'RTP' 등
  port?: number
  proto?: 'tcp' | 'udp'
}

interface PkgDef {
  id: number
  name: string
  version: string
  description: string
  capability: Capability
  ipSlots: IpSlot[]       // 이 패키지가 요구하는 IP slot 들
}

/** 인터페이스 — agent 가 ip -a 로 보고. mock data 로 시뮬레이션. */
interface NetIface {
  name: string            // 'eth0' / 'eth1' / 'lo'
  ip: string              // '192.168.10.21'
  mask: number            // 24
  hint?: string           // 'mgmt 사용중' / 'service 권장'
}

/** IP binding — 운영자가 인터페이스 + IP 선택 + 용도(slot) 매핑 */
type BindingStatus = 'up' | 'down' | 'unknown'

interface IpBinding {
  bid: number               // 행 단위 id (운영자가 row 추가/삭제 — slot 중복 가능)
  slot: string              // 용도 — 빈 문자열 ('') 도 허용 (운영자가 아직 미선택)
  iface?: string            // NetIface.name
  ip: string                // 선택한 IP (iface 의 IP default, 운영자 override)
  mask?: number             // default 24
  status?: BindingStatus    // 'up' / 'down' / 'unknown'. [확인] 후 갱신
}

interface ServerRow {
  id: number
  name: string
  role: Role
  ip: string | null            // mgmt IP — agent enroll 후 자동 (운영자 편집 X)
  status: ServerStatus
  agent_version: string | null
  token: string
  interfaces: NetIface[]       // agent enroll 후 cache (mock)
  serviceIpBindings: IpBinding[]  // slot 별 운영자 입력
}

interface ServiceRow {
  id: number
  name: string
  mode: Mode
  vrid: number | null     // standalone 은 null
  vipMask: number
  authPass: string
  servers: ServerRow[]
  packageIds: number[]
  vipBindings: IpBinding[]  // VIP slot 별 입력
}

const MOCK_PACKAGES: PkgDef[] = [
  { id: 1, name: 'csc',     version: '0.1.0', description: '관리 API 서버',     capability: 'active_standby',
    ipSlots: [
      { scope: 'service', name: 'Admin', port: 4420, proto: 'tcp' },
      { scope: 'service', name: 'McPTT', port: 4430, proto: 'tcp' },
      { scope: 'vip',     name: 'Admin', port: 4420, proto: 'tcp' },
    ] },
  { id: 2, name: 'csp',     version: '0.1.0', description: 'VoLTE SIP 서버',   capability: 'active_standby',
    ipSlots: [
      { scope: 'service', name: 'SIP',   port: 5060, proto: 'udp' },
      { scope: 'service', name: 'Admin', port: 4421, proto: 'tcp' },
      { scope: 'service', name: 'Stats', port: 9000, proto: 'udp' },
      { scope: 'vip',     name: 'SIP',   port: 5060, proto: 'udp' },
    ] },
  { id: 3, name: 'psp',     version: '0.1.0', description: 'PTT SIP 서버',     capability: 'active_standby',
    ipSlots: [
      { scope: 'service', name: 'SIP',   port: 5060, proto: 'udp' },
      { scope: 'vip',     name: 'SIP',   port: 5060, proto: 'udp' },
    ] },
  { id: 4, name: 'cmp',     version: '0.1.0', description: 'VoLTE Media',      capability: 'all_active',
    ipSlots: [
      { scope: 'service', name: 'RTP',     port: 50000, proto: 'udp' },
      { scope: 'service', name: 'Control', port: 9000,  proto: 'udp' },
    ] },
  { id: 5, name: 'pmp',     version: '0.1.0', description: 'PTT Media',        capability: 'all_active',
    ipSlots: [
      { scope: 'service', name: 'RTP',     port: 52000, proto: 'udp' },
      { scope: 'service', name: 'Floor',   port: 54000, proto: 'udp' },
      { scope: 'service', name: 'Control', port: 9001,  proto: 'udp' },
    ] },
  { id: 6, name: 'cwrtc',   version: '0.1.0', description: 'WebRTC 게이트웨이', capability: 'standalone',
    ipSlots: [{ scope: 'service', name: 'WS', port: 8443, proto: 'tcp' }] },
  { id: 7, name: 'console', version: '0.1.0', description: '관리 콘솔',        capability: 'standalone',
    ipSlots: [{ scope: 'service', name: 'HTTPS', port: 8081, proto: 'tcp' }] },
  { id: 8, name: 'phone',   version: '0.1.0', description: 'PTT Web UE',       capability: 'standalone',
    ipSlots: [{ scope: 'service', name: 'HTTPS', port: 3002, proto: 'tcp' }] },
]

const DEMO_IFACES: NetIface[] = [
  { name: 'eth0', ip: '192.168.10.21', mask: 24, hint: 'mgmt 사용중' },
  { name: 'eth1', ip: '10.0.0.21',     mask: 24, hint: 'service 권장' },
  { name: 'lo',   ip: '127.0.0.1',     mask: 8  },
]
const DEMO_IFACES_22: NetIface[] = [
  { name: 'eth0', ip: '192.168.10.22', mask: 24, hint: 'mgmt 사용중' },
  { name: 'eth1', ip: '10.0.0.22',     mask: 24, hint: 'service 권장' },
  { name: 'lo',   ip: '127.0.0.1',     mask: 8  },
]
const DEMO_IFACES_31: NetIface[] = [
  { name: 'eth0', ip: '192.168.10.31', mask: 24, hint: 'mgmt 사용중' },
  { name: 'eth1', ip: '10.0.0.31',     mask: 24 },
  { name: 'lo',   ip: '127.0.0.1',     mask: 8  },
]
const DEMO_IFACES_32: NetIface[] = [
  { name: 'eth0', ip: '192.168.10.32', mask: 24, hint: 'mgmt 사용중' },
  { name: 'eth1', ip: '10.0.0.32',     mask: 24 },
  { name: 'lo',   ip: '127.0.0.1',     mask: 8  },
]

// 데모용 초기 서비스 1-2개 — 운영자가 추가하는 흐름도 함께
const INITIAL_SERVICES: ServiceRow[] = [
  {
    id: 1, name: 'VoLTE SIP Server', mode: 'active_standby',
    vrid: 52, vipMask: 24, authPass: 'secret01',
    packageIds: [2],
    vipBindings: [
      { bid: 1001, slot: 'SIP', iface: 'eth1', ip: '10.0.0.100', mask: 24, status: 'up' },
    ],
    servers: [
      { id: 11, name: 'VoLTE SIP Server-01', role: 'master',
        ip: '192.168.10.21', status: 'online', agent_version: '0.0.1', token: 'tok-a1b2c3d4',
        interfaces: DEMO_IFACES,
        serviceIpBindings: [
          { bid: 101, slot: 'SIP',   iface: 'eth1', ip: '10.0.0.21',     mask: 24, status: 'up'   },
          { bid: 102, slot: 'Admin', iface: 'eth0', ip: '192.168.10.21', mask: 24, status: 'up'   },
          { bid: 103, slot: 'Stats', iface: 'eth1', ip: '10.0.0.21',     mask: 24, status: 'unknown' },
        ] },
      { id: 12, name: 'VoLTE SIP Server-02', role: 'backup',
        ip: null, status: 'pending', agent_version: null, token: 'tok-e5f6g7h8',
        interfaces: [], serviceIpBindings: [] },
    ],
  },
  {
    id: 2, name: 'VoLTE Media', mode: 'all_active',
    vrid: 53, vipMask: 24, authPass: 'secret02',
    packageIds: [4],
    vipBindings: [],   // AA media — cmp 는 vip slot 정의 안 함
    servers: [
      { id: 21, name: 'VoLTE Media-01', role: null,
        ip: '192.168.10.31', status: 'online', agent_version: '0.0.1', token: 'tok-aa01',
        interfaces: DEMO_IFACES_31,
        serviceIpBindings: [
          { bid: 201, slot: 'RTP',     iface: 'eth1', ip: '10.0.0.31', mask: 24, status: 'up'   },
          { bid: 202, slot: 'Control', iface: 'eth1', ip: '10.0.0.31', mask: 24, status: 'down' },
        ] },
      { id: 22, name: 'VoLTE Media-02', role: null,
        ip: '192.168.10.32', status: 'online', agent_version: '0.0.1', token: 'tok-aa02',
        interfaces: DEMO_IFACES_32,
        serviceIpBindings: [
          { bid: 203, slot: 'RTP',     iface: 'eth1', ip: '10.0.0.32', mask: 24, status: 'up' },
          { bid: 204, slot: 'Control', iface: 'eth1', ip: '10.0.0.32', mask: 24, status: 'up' },
        ] },
    ],
  },
]

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

function genToken(): string {
  return 'tok-' + Math.random().toString(36).slice(2, 10)
}

function pad2(n: number): string {
  return n.toString().padStart(2, '0')
}

function buildInstallCommand(srv: ServerRow, svc: ServiceRow): string {
  const role = srv.role ? ` --role ${srv.role}` : ''
  return `curl -k https://CSC:4420/install-agent.sh | bash -s -- \\
  --csc-url https://CSC:4420 \\
  --enrollment-token ${srv.token} \\
  --name ${srv.name} --service ${svc.name}${role}`
}

// ──────────────────────────────────────────────────────────────
//  Page
// ──────────────────────────────────────────────────────────────

export default function HaServicesPage() {
  const [services, setServices] = useState<ServiceRow[]>(INITIAL_SERVICES)
  const [expanded, setExpanded] = useState<Set<number>>(new Set([1, 2]))
  const [adding, setAdding] = useState<{ name: string; mode: Mode | '' } | null>(null)
  const [editingName, setEditingName] = useState<{ kind: 'service' | 'server'; id: number; value: string } | null>(null)
  const [pkgPickerFor, setPkgPickerFor] = useState<number | null>(null)  // service id
  const [vipExpandFor, setVipExpandFor] = useState<number | null>(null)  // service id
  const [svcIpExpandFor, setSvcIpExpandFor] = useState<number | null>(null) // server id
  const [toast, setToast] = useState<string | null>(null)

  // ── helpers ──
  const flash = (msg: string) => { setToast(msg); window.setTimeout(() => setToast(null), 2000) }
  const toggleExpand = (id: number) => setExpanded(prev => {
    const n = new Set(prev); n.has(id) ? n.delete(id) : n.add(id); return n
  })
  const nextServiceId = () => Math.max(0, ...services.map(s => s.id)) + 1
  const nextServerId = () => {
    const all = services.flatMap(s => s.servers.map(x => x.id))
    return Math.max(99, ...all) + 1
  }
  const nextVrid = () => {
    const used = new Set(services.map(s => s.vrid).filter((v): v is number => v !== null))
    for (let v = 51; v <= 255; v++) if (!used.has(v)) return v
    return 51
  }

  // 패키지가 정의한 IP slot 들 (서비스 전체에서 합집합) — 그룹 단위 (vip) / 서버 단위 (service)
  function slotsForService(svc: ServiceRow, scope: 'service' | 'vip'): IpSlot[] {
    const slots: IpSlot[] = []
    const seen = new Set<string>()
    for (const pkgId of svc.packageIds) {
      const pkg = MOCK_PACKAGES.find(p => p.id === pkgId)
      if (!pkg) continue
      for (const slot of pkg.ipSlots) {
        if (slot.scope !== scope) continue
        const key = `${slot.name}:${slot.proto}:${slot.port}`
        if (seen.has(key)) continue
        seen.add(key); slots.push(slot)
      }
    }
    return slots
  }
  const updateService = (sid: number, patch: Partial<ServiceRow>) =>
    setServices(prev => prev.map(s => s.id === sid ? { ...s, ...patch } : s))
  const updateServer = (sid: number, srvId: number, patch: Partial<ServerRow>) =>
    setServices(prev => prev.map(s => s.id !== sid ? s :
      { ...s, servers: s.servers.map(x => x.id === srvId ? { ...x, ...patch } : x) }))

  // ── 서비스 추가 ──
  const createService = () => {
    if (!adding || !adding.name.trim() || !adding.mode) { flash('이름 + 유형 필요'); return }
    const baseName = adding.name.trim()
    const mode = adding.mode as Mode
    const sid = nextServiceId()
    let sIdSeq = nextServerId()
    let servers: ServerRow[] = []
    const blankSrv = (i: number, role: Role): ServerRow => ({
      id: sIdSeq + i - 1,
      name: `${baseName}-${pad2(i)}`,
      role,
      ip: null,
      status: 'pending',
      agent_version: null,
      token: genToken(),
      interfaces: [],
      serviceIpBindings: [],
    })
    if (mode === 'active_standby') {
      servers = [blankSrv(1, 'master'), blankSrv(2, 'backup')]
    } else {
      servers = [blankSrv(1, null)]
    }
    setServices([...services, {
      id: sid, name: baseName, mode,
      vrid: mode === 'standalone' ? null : nextVrid(),
      vipMask: 24,
      authPass: '',
      packageIds: [],
      vipBindings: [],
      servers,
    }])
    setExpanded(prev => new Set([...prev, sid]))
    setAdding(null)
    flash(`서비스 "${baseName}" 추가 (${MODE_LABEL[mode]})`)
  }

  // ── 서버 추가 (AA/Standalone 만) ──
  const addServer = (svc: ServiceRow) => {
    const idx = svc.servers.length + 1
    const newSrv: ServerRow = {
      id: nextServerId(),
      name: `${svc.name}-${pad2(idx)}`,
      role: null,
      ip: null,
      status: 'pending',
      agent_version: null,
      token: genToken(),
      interfaces: [],
      serviceIpBindings: [],
    }
    updateService(svc.id, { servers: [...svc.servers, newSrv] })
    flash(`서버 "${newSrv.name}" 추가 — 신규 토큰 발행`)
  }

  // ── 토큰 재발행 ──
  const regenerateToken = (svc: ServiceRow, srv: ServerRow) => {
    if (srv.status === 'online') {
      if (!confirm(`${srv.name} 은 이미 online — 재발행 시 기존 agent 인증 무효. 진행?`)) return
    }
    const newTok = genToken()
    updateServer(svc.id, srv.id, { token: newTok, status: 'pending', ip: null, agent_version: null, interfaces: [] })
    copyInstallCmd({ ...srv, token: newTok }, svc, true)
    flash(`${srv.name} 토큰 재발행 + clipboard 복사`)
  }

  // ── install command 복사 ──
  const copyInstallCmd = (srv: ServerRow, svc: ServiceRow, silent = false) => {
    const cmd = buildInstallCommand(srv, svc)
    navigator.clipboard?.writeText(cmd).then(() => {
      if (!silent) flash(`${srv.name} install command 복사됨`)
    }).catch(() => flash('clipboard 권한 없음'))
  }

  // ── 삭제 ──
  const deleteService = (svc: ServiceRow) => {
    if (!confirm(`서비스 "${svc.name}" 과 서버 ${svc.servers.length} 개를 모두 삭제하시겠습니까?`)) return
    setServices(prev => prev.filter(s => s.id !== svc.id))
    flash(`"${svc.name}" 삭제`)
  }

  return (
    <div style={{ padding: 16, width: '100%' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', marginBottom: 12 }}>
        <h1 style={{ margin: 0 }}>서버 + HA 관리</h1>
        <span style={{ fontSize: 11, padding: '2px 8px', borderRadius: 3,
                       background: '#fff8db', border: '1px solid #f0c75e', color: '#876200' }}>
          prototype (mock data)
        </span>
      </div>
      <div style={{ color: '#666', fontSize: 13, marginBottom: 16 }}>
        서비스(=HA 그룹/단독) 단위로 서버를 묶어 관리. 유형 선택 시 자동으로 서버 발급(A/S=2, AA/Standalone=1).
        모든 추가/편집은 list 행 안 inline (팝업 없음).
      </div>

      <table style={{ width: '100%', borderCollapse: 'collapse', background: '#fff',
                      border: '1px solid #e0e0e0', borderRadius: 6, overflow: 'hidden' }}>
        <thead>
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
              updateService={updateService}
              updateServer={updateServer}
              addServer={() => addServer(svc)}
              regenerateToken={(srv) => regenerateToken(svc, srv)}
              copyCmd={(srv) => copyInstallCmd(srv, svc)}
              onDelete={() => deleteService(svc)}
            />
          ))}

          {/* 인라인 서비스 추가 행 */}
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
                <button onClick={createService} style={btnPrimary()}>생성</button>
                <button onClick={() => setAdding(null)} style={btnSecondary()}>취소</button>
              </td>
            </tr>
          )}
        </tbody>
      </table>

      <div style={{ marginTop: 12 }}>
        {!adding && (
          <button onClick={() => setAdding({ name: '', mode: '' })} style={btnAdd()}>
            ＋ 서비스 추가
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
  updateService: (sid: number, patch: Partial<ServiceRow>) => void
  updateServer: (sid: number, srvId: number, patch: Partial<ServerRow>) => void
  addServer: () => void
  regenerateToken: (srv: ServerRow) => void
  copyCmd: (srv: ServerRow) => void
  onDelete: () => void
}

function ServiceTreeRows(p: ServiceTreeProps) {
  const { svc, idx, expanded, onToggle } = p
  const isStandalone = svc.mode === 'standalone'
  const canAddServer = svc.mode !== 'active_standby'  // A/S 는 2 고정

  return (
    <>
      {/* 서비스 행 */}
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

      {/* VIP expand panel (서비스 행 다음) */}
      {p.vipExpanded && p.vipSlots.length > 0 && (
        <tr style={{ background: '#f8f9fb' }}>
          <td colSpan={7} style={{ padding: '10px 16px 12px 60px' }}>
            <IpSlotPanel
              title="VIP slots (서비스 단위 — A/S fail-over 공유, AA 는 사용 안 함)"
              slots={p.vipSlots}
              bindings={svc.vipBindings}
              interfaces={svc.servers[0]?.interfaces ?? []}
              vrid={svc.vrid}
              onChange={(bindings) => p.updateService(svc.id, { vipBindings: bindings })}
            />
          </td>
        </tr>
      )}

      {/* 서버 자식 행들 (펼침 시) */}
      {expanded && svc.servers.map((srv, srvIdx) => (
        <ServerRows
          key={srv.id}
          svc={svc} srv={srv} idx={idx} srvIdx={srvIdx}
          serviceSlots={p.serviceSlots}
          editingName={p.editingName}
          setEditingName={p.setEditingName}
          svcIpExpanded={p.svcIpExpandFor === srv.id}
          setSvcIpExpand={(open) => p.setSvcIpExpand(srv.id, open)}
          updateServer={p.updateServer}
          regenerateToken={p.regenerateToken}
          copyCmd={p.copyCmd}
        />
      ))}

      {/* [＋ 서버 추가] 행 — AA/Standalone 만 */}
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

      {/* 패키지 행 */}
      {expanded && (
        <tr style={{ background: '#fcfdfe' }}>
          <td style={td(60)}></td>
          <td colSpan={6} style={{ padding: '6px 12px' }}>
            <PackagesArea svc={svc}
                          pickerOpen={p.pkgPickerOpen}
                          setPickerOpen={p.setPkgPicker}
                          onChange={(ids) => p.updateService(svc.id, { packageIds: ids })} />
          </td>
        </tr>
      )}
    </>
  )
}

// ──────────────────────────────────────────────────────────────
//  ServerRows — 서버 행 + (선택적) 서비스 IP expand panel
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
              📡 서비스 IP {srv.serviceIpBindings.length}/{p.serviceSlots.length} {p.svcIpExpanded ? '▲' : '▼'}
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
          <button onClick={() => p.copyCmd(srv)} style={btnSmall()}>📋 복사</button>
          {srv.status !== 'online' && (
            <button onClick={() => p.regenerateToken(srv)} style={btnSmall()}>↻ 토큰</button>
          )}
        </td>
      </tr>

      {p.svcIpExpanded && enrollDone && p.serviceSlots.length > 0 && (
        <tr style={{ background: '#f8f9fb' }}>
          <td colSpan={7} style={{ padding: '10px 16px 12px 76px' }}>
            <IpSlotPanel
              title={`서비스 IP slots (${srv.name} — 패키지 ${svc.packageIds.length}개에서 요구)`}
              slots={p.serviceSlots}
              bindings={srv.serviceIpBindings}
              interfaces={srv.interfaces}
              onChange={(bindings) => p.updateServer(svc.id, srv.id, { serviceIpBindings: bindings })}
            />
          </td>
        </tr>
      )}
    </>
  )
}

// ──────────────────────────────────────────────────────────────
//  IpSlotPanel — 인터페이스 list + slot 별 매핑 (VIP / 서비스 IP 공용)
// ──────────────────────────────────────────────────────────────

function IpSlotPanel({ title, slots, bindings, interfaces, vrid, onChange }: {
  title: string
  slots: IpSlot[]            // 운영자가 "용도" select 에 사용 (slot.name 만 list)
  bindings: IpBinding[]      // 행 단위 (bid)
  interfaces: NetIface[]
  vrid?: number | null
  onChange: (bindings: IpBinding[]) => void
}) {
  // 행 추가 — 새 bid 생성, default iface = 첫 인터페이스
  const addRow = () => {
    const newId = Math.max(0, ...bindings.map(b => b.bid)) + 1
    const firstIface = interfaces[0]
    onChange([...bindings, {
      bid: newId,
      slot: '',
      iface: firstIface?.name,
      ip: firstIface?.ip ?? '',
      mask: firstIface?.mask ?? 24,
      status: 'unknown',
    }])
  }
  const updateRow = (bid: number, patch: Partial<IpBinding>) =>
    onChange(bindings.map(b => b.bid === bid ? { ...b, ...patch } : b))
  const removeRow = (bid: number) => onChange(bindings.filter(b => b.bid !== bid))

  // mock IP up 확인 — 실제 wiring 시 agent 에 check 명령
  const checkRow = (bid: number) => {
    updateRow(bid, { status: 'unknown' })
    window.setTimeout(() => {
      // mock: 70% up, 30% down
      const ok = Math.random() < 0.7
      updateRow(bid, { status: ok ? 'up' : 'down' })
    }, 600)
  }

  const slotNames = slots.map(s => s.name)
  const usedSlots = new Set(bindings.map(b => b.slot).filter(Boolean))

  return (
    <div style={{ border: '1px solid #d8e0ea', borderRadius: 6, padding: 12, background: '#fff' }}>
      <div style={{ fontSize: 12, fontWeight: 'bold', color: '#555', marginBottom: 10 }}>
        {title}
      </div>

      {/* 1. 인터페이스 — 정보 표시 (먼저) */}
      <div style={{ marginBottom: 12 }}>
        <div style={{ fontSize: 11, color: '#666', marginBottom: 4,
                      display: 'flex', alignItems: 'center', gap: 8 }}>
          <b>인터페이스 (agent 보고):</b>
          <button style={btnSmall()} disabled>↻ 새로고침</button>
          <span style={{ color: '#aaa', fontSize: 10 }}>(prototype — 실제 wiring 시 agent metric)</span>
        </div>
        {interfaces.length === 0 ? (
          <div style={{ color: '#aaa', fontSize: 12 }}>(없음 — enroll 미완료)</div>
        ) : (
          <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap' }}>
            {interfaces.map(iface => (
              <span key={iface.name} style={{
                padding: '3px 8px', background: '#eef4fa',
                border: '1px solid #c5d8eb', borderRadius: 3,
                fontFamily: 'monospace', fontSize: 11,
              }}>
                <b>{iface.name}</b> {iface.ip}/{iface.mask}
                {iface.hint && <span style={{ color: '#888', marginLeft: 4 }}>· {iface.hint}</span>}
              </span>
            ))}
          </div>
        )}
      </div>

      {/* 2. 용도 매핑 — 행 단위 */}
      <div style={{ fontSize: 11, color: '#666', marginBottom: 4 }}>
        <b>IP 매핑</b> — 인터페이스/IP 선택 후 "용도" 지정. 같은 인터페이스를 여러 용도에 매핑 가능.
      </div>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ background: '#f5f5f5', color: '#666' }}>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 40 }}>#</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 140 }}>인터페이스</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 180 }}>IP / mask</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 140 }}>용도</th>
            {vrid != null && <th style={{ padding: '4px 8px', textAlign: 'left', width: 60 }}>VRID</th>}
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 100 }}>상태</th>
            <th style={{ padding: '4px 8px', textAlign: 'left' }}>액션</th>
          </tr>
        </thead>
        <tbody>
          {bindings.length === 0 && (
            <tr>
              <td colSpan={vrid != null ? 7 : 6} style={{ padding: '8px', color: '#aaa' }}>
                (매핑 없음 — 아래 [＋ 행 추가] 로 시작)
              </td>
            </tr>
          )}
          {bindings.map((b, i) => (
            <tr key={b.bid}>
              <td style={{ padding: '4px 8px', color: '#888' }}>{i + 1}</td>
              <td style={{ padding: '4px 8px' }}>
                <select value={b.iface ?? ''}
                        onChange={e => {
                          const iface = e.target.value || undefined
                          const found = interfaces.find(x => x.name === iface)
                          updateRow(b.bid, {
                            iface,
                            ip: found?.ip ?? b.ip,
                            mask: found?.mask ?? b.mask ?? 24,
                            status: 'unknown',
                          })
                        }}
                        style={{ width: '95%', padding: '2px 4px', fontSize: 12 }}>
                  <option value="">(선택)</option>
                  {interfaces.map(iface => (
                    <option key={iface.name} value={iface.name}>{iface.name}</option>
                  ))}
                </select>
              </td>
              <td style={{ padding: '4px 8px' }}>
                <span style={{ display: 'inline-flex', gap: 2, alignItems: 'center' }}>
                  <input value={b.ip}
                         onChange={e => updateRow(b.bid, { ip: e.target.value, status: 'unknown' })}
                         placeholder="(IP)"
                         style={{ width: 110, padding: '2px 6px', fontSize: 12,
                                  border: '1px solid #ddd', borderRadius: 3 }} />
                  <span>/</span>
                  <input type="number" value={b.mask ?? 24}
                         onChange={e => updateRow(b.bid, { mask: parseInt(e.target.value) || 24 })}
                         style={{ width: 40, padding: '2px 6px', fontSize: 12,
                                  border: '1px solid #ddd', borderRadius: 3 }} />
                </span>
              </td>
              <td style={{ padding: '4px 8px' }}>
                <select value={b.slot} onChange={e => updateRow(b.bid, { slot: e.target.value })}
                        style={{ width: '95%', padding: '2px 4px', fontSize: 12,
                                 color: b.slot ? '#333' : '#c00' }}>
                  <option value="">(용도 선택)</option>
                  {slotNames.map(name => (
                    <option key={name} value={name}
                            disabled={usedSlots.has(name) && b.slot !== name}>
                      {name}{usedSlots.has(name) && b.slot !== name ? ' (사용중)' : ''}
                    </option>
                  ))}
                </select>
              </td>
              {vrid != null && (
                <td style={{ padding: '4px 8px', color: '#666' }}>{vrid}</td>
              )}
              <td style={{ padding: '4px 8px' }}><StatusBadge status={b.status} /></td>
              <td style={{ padding: '4px 8px' }}>
                <button onClick={() => checkRow(b.bid)} style={btnSmall()} title="IP up 확인">확인</button>
                <button onClick={() => removeRow(b.bid)} style={btnSmall()} title="행 삭제">삭제</button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>

      <div style={{ marginTop: 8 }}>
        <button onClick={addRow}
                style={btnAdd(true)}
                disabled={interfaces.length === 0}>
          ＋ 행 추가 {interfaces.length === 0 && '(인터페이스 정보 필요)'}
        </button>
        {slots.length === 0 && (
          <span style={{ marginLeft: 12, color: '#aaa', fontSize: 11 }}>
            (패키지 미설치 — 용도 select 비어있음)
          </span>
        )}
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

// ──────────────────────────────────────────────────────────────
//  Sub-components
// ──────────────────────────────────────────────────────────────

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
  const color = online === total ? STATUS_COLOR.online : STATUS_COLOR.pending
  return (
    <span style={{ fontSize: 12, color }}>
      {online === total ? '●' : '◐'} {online}/{cap ?? total}
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

function PackagesArea({ svc, pickerOpen, setPickerOpen, onChange }: {
  svc: ServiceRow
  pickerOpen: boolean
  setPickerOpen: (open: boolean) => void
  onChange: (ids: number[]) => void
}) {
  const installed = useMemo(
    () => MOCK_PACKAGES.filter(p => svc.packageIds.includes(p.id)),
    [svc.packageIds]
  )
  const available = useMemo(
    () => MOCK_PACKAGES.filter(p => !svc.packageIds.includes(p.id)),
    [svc.packageIds]
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
