/**
 * HaServerWizardPage — "서버 등록 + HA 구성 + 모듈 설치" 통합 wizard (mock-up prototype).
 *
 * 본 페이지는 **React prototype** — mock data 사용, 실제 API 호출 없음.
 * 운영자 흐름 (Step 1 서버 → Step 2 HA → Step 3 모듈 → Step 4 review) 의 UX 검증용.
 *
 * 후속 라운드: 실제 wiring (createAgent / ha-groups CRUD / createDeployment),
 * 기존 ServersPage / HaGroupsPage 통합/폐기 결정.
 */
import { useMemo, useState } from 'react'

// ──────────────────────────────────────────────────────────────
//  Mock data (prototype only — wiring 시 실제 API 호출로 교체)
// ──────────────────────────────────────────────────────────────

type AgentStatus = 'pending' | 'approved' | 'online' | 'offline'
type HaMode = 'active_standby' | 'all_active'
type HaCapability = HaMode | 'standalone'

interface MockAgent {
  id: number
  name: string
  hostname: string
  ip: string
  status: AgentStatus
  ha_group_id: number | null  // 이미 그룹 소속이면 select 제한
}

interface MockPackage {
  id: number
  name: string
  version: string
  description: string
  ha_capability: HaCapability
}

interface MockHaGroup {
  id: number
  name: string
  mode: HaMode
  vip: string
  member_count: number
  capacity: number | null  // null = unlimited (AA)
}

const MOCK_AGENTS: MockAgent[] = [
  { id: 11, name: 'srv-mgmt-a', hostname: 'cims-mgmt-01', ip: '10.0.0.11', status: 'pending', ha_group_id: null },
  { id: 12, name: 'srv-mgmt-b', hostname: 'cims-mgmt-02', ip: '10.0.0.12', status: 'pending', ha_group_id: null },
  { id: 21, name: 'srv-volte-a', hostname: 'cims-volte-01', ip: '10.0.0.21', status: 'online', ha_group_id: 100 },
  { id: 22, name: 'srv-volte-b', hostname: 'cims-volte-02', ip: '10.0.0.22', status: 'online', ha_group_id: 100 },
  { id: 31, name: 'srv-media-a', hostname: 'cims-media-01', ip: '10.0.0.31', status: 'online', ha_group_id: 200 },
]

const MOCK_PACKAGES: MockPackage[] = [
  { id: 1, name: 'csc',     version: '0.1.0', description: '관리 API 서버',    ha_capability: 'active_standby' },
  { id: 2, name: 'csp',     version: '0.1.0', description: 'VoLTE SIP 서버',  ha_capability: 'active_standby' },
  { id: 3, name: 'psp',     version: '0.1.0', description: 'PTT SIP 서버',    ha_capability: 'active_standby' },
  { id: 4, name: 'cmp',     version: '0.1.0', description: 'VoLTE Media',     ha_capability: 'all_active'    },
  { id: 5, name: 'pmp',     version: '0.1.0', description: 'PTT Media',       ha_capability: 'all_active'    },
  { id: 6, name: 'cwrtc',   version: '0.1.0', description: 'WebRTC 게이트웨이', ha_capability: 'standalone'    },
  { id: 7, name: 'console', version: '0.1.0', description: '관리 콘솔',       ha_capability: 'standalone'    },
  { id: 8, name: 'phone',   version: '0.1.0', description: 'PTT Web UE',      ha_capability: 'standalone'    },
]

const MOCK_HA_GROUPS: MockHaGroup[] = [
  { id: 100, name: 'volte-as',  mode: 'active_standby', vip: '10.0.0.100', member_count: 2, capacity: 2 },
  { id: 200, name: 'media-aa',  mode: 'all_active',     vip: '10.0.0.200', member_count: 1, capacity: null },
]

// ──────────────────────────────────────────────────────────────
//  Wizard state
// ──────────────────────────────────────────────────────────────

type StepNo = 1 | 2 | 3 | 4

type ServerStep =
  | { mode: 'select'; agentId: number }
  | { mode: 'create'; name: string; note: string; submitted: boolean; mockToken?: string }
  | null

type HaStep =
  | { kind: 'standalone' }
  | { kind: 'join'; groupId: number; role: 'master' | 'backup' }
  | { kind: 'new'; name: string; mode: HaMode; vip: string; vipMask: number; authPass: string; peerAgentId: number | null }
  | null

interface WizardState {
  step: StepNo
  server: ServerStep
  ha: HaStep
  modules: number[]
}

const INITIAL: WizardState = {
  step: 1,
  server: null,
  ha: null,
  modules: [],
}

// ──────────────────────────────────────────────────────────────
//  Validation
// ──────────────────────────────────────────────────────────────

function canProceed(s: WizardState): boolean {
  if (s.step === 1) {
    if (!s.server) return false
    if (s.server.mode === 'select') return !!s.server.agentId
    if (s.server.mode === 'create') return !!s.server.name.trim() && !!s.server.submitted
  }
  if (s.step === 2) {
    if (!s.ha) return false
    if (s.ha.kind === 'standalone') return true
    if (s.ha.kind === 'join') return !!s.ha.groupId
    if (s.ha.kind === 'new') {
      return !!s.ha.name.trim() && !!s.ha.vip.trim() && !!s.ha.authPass.trim()
        && (s.ha.mode === 'all_active' || !!s.ha.peerAgentId)
    }
  }
  if (s.step === 3) return true  // 0개 선택도 OK
  if (s.step === 4) return true
  return false
}

function effectiveHaMode(s: WizardState): HaMode | 'standalone' {
  if (!s.ha) return 'standalone'
  if (s.ha.kind === 'standalone') return 'standalone'
  if (s.ha.kind === 'join') {
    const g = MOCK_HA_GROUPS.find(x => x.id === s.ha!.groupId)
    return g?.mode ?? 'standalone'
  }
  return s.ha.mode  // kind === 'new'
}

function moduleAllowed(pkg: MockPackage, mode: HaMode | 'standalone'): boolean {
  if (pkg.ha_capability === 'standalone') return true
  if (mode === 'standalone') return false  // standalone 노드 = standalone 모듈만
  return pkg.ha_capability === mode
}

// ──────────────────────────────────────────────────────────────
//  Component
// ──────────────────────────────────────────────────────────────

export default function HaServerWizardPage() {
  const [state, setState] = useState<WizardState>(INITIAL)
  const set = (patch: Partial<WizardState>) => setState(prev => ({ ...prev, ...patch }))
  const updateServer = (patch: Partial<Exclude<ServerStep, null>>) =>
    setState(prev => ({ ...prev, server: prev.server ? ({ ...prev.server, ...patch } as ServerStep) : prev.server }))

  const proceed = canProceed(state)
  const haMode = effectiveHaMode(state)

  const onNext = () => {
    if (!proceed) return
    if (state.step === 4) {
      // mock submit
      console.log('[wizard-prototype] submit:', state)
      const summary = renderSummary(state)
      alert(`prototype — 실제 실행 안 됨\n\n${summary}`)
      return
    }
    setState(prev => ({ ...prev, step: (prev.step + 1) as StepNo }))
  }
  const onPrev = () => {
    if (state.step === 1) return
    setState(prev => ({ ...prev, step: (prev.step - 1) as StepNo }))
  }
  const onReset = () => setState(INITIAL)

  return (
    <div style={{ padding: 24, maxWidth: 1100, margin: '0 auto' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', marginBottom: 16 }}>
        <h1 style={{ margin: 0 }}>서버 + HA 설정 마법사</h1>
        <span style={{ fontSize: 12, padding: '2px 8px', borderRadius: 3,
                       background: '#fff8db', border: '1px solid #f0c75e', color: '#876200' }}>
          prototype (mock data)
        </span>
      </div>
      <div style={{ color: '#666', fontSize: 13, marginBottom: 24 }}>
        서버 등록과 HA 구성을 한 번에 — 모듈 설치까지 일련의 흐름.
      </div>

      <Stepper current={state.step} />

      <div style={{ marginTop: 24, padding: 24, border: '1px solid #e5e5e5',
                    borderRadius: 8, background: '#fff', minHeight: 360 }}>
        {state.step === 1 && <Step1Server state={state} setState={setState}
                                          updateServer={updateServer} />}
        {state.step === 2 && <Step2Ha state={state} setState={setState} />}
        {state.step === 3 && <Step3Modules state={state} setState={setState} mode={haMode} />}
        {state.step === 4 && <Step4Review state={state} />}
      </div>

      <div style={{ marginTop: 16, display: 'flex', gap: 8, justifyContent: 'flex-end' }}>
        <button onClick={onReset} style={{ marginRight: 'auto', fontSize: 12, color: '#888' }}>
          처음부터
        </button>
        <button onClick={onPrev} disabled={state.step === 1}>← 이전</button>
        <button onClick={onNext} disabled={!proceed}
                style={{ background: proceed ? '#3498db' : '#ccc', color: '#fff',
                         padding: '6px 16px', border: 'none', borderRadius: 4,
                         cursor: proceed ? 'pointer' : 'not-allowed' }}>
          {state.step === 4 ? '완료 ✓' : '다음 →'}
        </button>
      </div>
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  Stepper
// ──────────────────────────────────────────────────────────────

const STEPS: { no: StepNo; label: string }[] = [
  { no: 1, label: '서버 선택/등록' },
  { no: 2, label: 'HA 구성' },
  { no: 3, label: '모듈 선택' },
  { no: 4, label: '확인 + 실행' },
]

function Stepper({ current }: { current: StepNo }) {
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
      {STEPS.map((s, i) => {
        const isCurrent = s.no === current
        const isDone = s.no < current
        const color = isDone ? '#27ae60' : isCurrent ? '#3498db' : '#bbb'
        return (
          <div key={s.no} style={{ display: 'flex', alignItems: 'center', flex: i === STEPS.length - 1 ? 0 : 1 }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
              <div style={{
                width: 32, height: 32, borderRadius: 16,
                background: color, color: '#fff',
                display: 'flex', alignItems: 'center', justifyContent: 'center',
                fontWeight: 'bold', fontSize: 14,
              }}>
                {isDone ? '✓' : s.no}
              </div>
              <span style={{ fontSize: 13, fontWeight: isCurrent ? 'bold' : 'normal',
                             color: isCurrent ? '#333' : '#666' }}>
                {s.label}
              </span>
            </div>
            {i < STEPS.length - 1 && (
              <div style={{ flex: 1, height: 2, background: s.no < current ? '#27ae60' : '#ddd',
                            margin: '0 12px' }} />
            )}
          </div>
        )
      })}
    </div>
  )
}

// ──────────────────────────────────────────────────────────────
//  Step 1 — 서버 선택 / 등록
// ──────────────────────────────────────────────────────────────

function Step1Server({ state, setState, updateServer }: {
  state: WizardState
  setState: React.Dispatch<React.SetStateAction<WizardState>>
  updateServer: (patch: Partial<Exclude<ServerStep, null>>) => void
}) {
  const tab = state.server?.mode ?? 'select'
  const setTab = (mode: 'select' | 'create') => {
    setState(prev => ({
      ...prev,
      server: mode === 'select'
        ? { mode: 'select', agentId: 0 }
        : { mode: 'create', name: '', note: '', submitted: false },
    }))
  }

  const pendingAgents = MOCK_AGENTS.filter(a => a.status === 'pending')

  return (
    <div>
      <h2 style={{ marginTop: 0 }}>Step 1 — 서버 선택 또는 등록</h2>
      <div style={{ display: 'flex', gap: 8, marginBottom: 16 }}>
        <TabButton active={tab === 'select'} onClick={() => setTab('select')}
                   label={`기존 pending 서버 선택 (${pendingAgents.length})`} />
        <TabButton active={tab === 'create'} onClick={() => setTab('create')}
                   label="새 서버 등록" />
      </div>

      {tab === 'select' && (
        <div>
          {pendingAgents.length === 0 && (
            <div style={{ color: '#888', padding: 16 }}>pending 상태 서버가 없습니다.</div>
          )}
          {pendingAgents.map(a => {
            const selected = state.server?.mode === 'select' && state.server.agentId === a.id
            return (
              <label key={a.id} style={{
                display: 'block', padding: 10, marginBottom: 6,
                border: `2px solid ${selected ? '#3498db' : '#e5e5e5'}`,
                borderRadius: 6, cursor: 'pointer',
                background: selected ? '#f0f8ff' : '#fff',
              }}>
                <input type="radio" checked={selected} onChange={() =>
                  setState(prev => ({ ...prev, server: { mode: 'select', agentId: a.id } }))
                } />
                {' '}<b>{a.name}</b> <span style={{ color: '#888', fontSize: 12 }}>
                  ({a.hostname} · {a.ip}) · status={a.status}
                </span>
              </label>
            )
          })}
        </div>
      )}

      {tab === 'create' && state.server?.mode === 'create' && (
        <div>
          {!state.server.submitted ? (
            <div style={{ display: 'grid', gridTemplateColumns: '120px 1fr', gap: 10 }}>
              <label>이름 *</label>
              <input value={state.server.name}
                     onChange={e => updateServer({ name: e.target.value })}
                     placeholder="srv-mgmt-c" />
              <label>메모</label>
              <input value={state.server.note}
                     onChange={e => updateServer({ note: e.target.value })}
                     placeholder="새 mgmt 노드 (KR-IDC)" />
              <div></div>
              <button onClick={() => {
                if (!state.server || state.server.mode !== 'create') return
                if (!state.server.name.trim()) { alert('이름 필요'); return }
                const tok = 'mock-token-' + Math.random().toString(36).slice(2, 10)
                updateServer({ submitted: true, mockToken: tok })
              }} style={{ width: 'fit-content', padding: '6px 16px' }}>
                등록 (mock)
              </button>
            </div>
          ) : (
            <div style={{ padding: 16, background: '#f5f9ff', border: '1px solid #d0e3ff', borderRadius: 6 }}>
              <div style={{ fontSize: 13, marginBottom: 8 }}>
                ✓ 서버 등록 완료 (mock) — <b>{state.server.name}</b>
              </div>
              <div style={{ fontSize: 12, color: '#555', marginBottom: 8 }}>
                대상 서버에서 다음 명령으로 install-agent.sh 실행:
              </div>
              <pre style={{ background: '#fff', padding: 10, fontSize: 11, overflow: 'auto', margin: 0,
                            border: '1px solid #ddd', borderRadius: 4 }}>
{`curl -k https://CSC_HOST:4420/install-agent.sh | \\
  bash -s -- --csc-url https://CSC_HOST:4420 \\
             --enrollment-token ${state.server.mockToken} \\
             --name ${state.server.name}`}
              </pre>
              <div style={{ marginTop: 8, fontSize: 12, color: '#888' }}>
                ℹ prototype: 실제로는 위 명령 실행 + heartbeat 도달 + 승인 후 다음 진행
              </div>
            </div>
          )}
        </div>
      )}
    </div>
  )
}

function TabButton({ active, onClick, label }: { active: boolean; onClick: () => void; label: string }) {
  return (
    <button onClick={onClick} style={{
      padding: '6px 14px', border: 'none',
      borderBottom: `3px solid ${active ? '#3498db' : 'transparent'}`,
      background: 'transparent', cursor: 'pointer',
      fontWeight: active ? 'bold' : 'normal', color: active ? '#333' : '#888',
    }}>{label}</button>
  )
}

// ──────────────────────────────────────────────────────────────
//  Step 2 — HA 구성
// ──────────────────────────────────────────────────────────────

function Step2Ha({ state, setState }: {
  state: WizardState
  setState: React.Dispatch<React.SetStateAction<WizardState>>
}) {
  const ha = state.ha
  const setHaKind = (kind: 'standalone' | 'join' | 'new') => {
    setState(prev => ({
      ...prev,
      ha: kind === 'standalone' ? { kind: 'standalone' }
        : kind === 'join' ? { kind: 'join', groupId: 0, role: 'backup' }
        : { kind: 'new', name: '', mode: 'active_standby', vip: '', vipMask: 24, authPass: '', peerAgentId: null },
    }))
  }
  const updateHa = (patch: Partial<Exclude<HaStep, null>>) =>
    setState(prev => ({ ...prev, ha: prev.ha ? ({ ...prev.ha, ...patch } as HaStep) : prev.ha }))

  // join 모드: 가입 가능 그룹 (capacity 여유)
  const joinableGroups = MOCK_HA_GROUPS.filter(g => g.capacity === null || g.member_count < g.capacity)

  // new 모드 A/S: peer 후보 (자기 자신 제외 + 그룹 미정의 agent)
  const myAgentId = state.server?.mode === 'select' ? state.server.agentId : -1
  const peerCandidates = MOCK_AGENTS.filter(a => a.ha_group_id === null && a.id !== myAgentId)

  return (
    <div>
      <h2 style={{ marginTop: 0 }}>Step 2 — HA 구성</h2>
      <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>

        <RadioCard checked={ha?.kind === 'standalone'} onClick={() => setHaKind('standalone')}
                   title="단독 (HA 미사용)"
                   desc="HA 그룹 없음. standalone 가능 모듈만 install 가능 (cwrtc / console / phone 등)."
                   color="#95a5a6" />

        <RadioCard checked={ha?.kind === 'join'} onClick={() => setHaKind('join')}
                   title="기존 HA 그룹 편입"
                   desc="이미 정의된 HA 그룹에 새 노드로 합류."
                   color="#3498db">
          {ha?.kind === 'join' && (
            <div style={{ marginTop: 10 }}>
              <div style={{ fontSize: 13, marginBottom: 6 }}>가입 가능 그룹:</div>
              {joinableGroups.length === 0 && <div style={{ color: '#888' }}>(가입 가능 그룹 없음 — 새 그룹 생성 옵션 사용)</div>}
              {joinableGroups.map(g => (
                <label key={g.id} style={{ display: 'block', padding: 6, fontSize: 13 }}>
                  <input type="radio" checked={ha.groupId === g.id}
                         onChange={() => updateHa({ groupId: g.id })} />
                  {' '}<b>{g.name}</b> ({modeLabel(g.mode)}, VIP={g.vip}, {g.member_count}/{g.capacity ?? '∞'} 멤버)
                </label>
              ))}
              {ha.groupId > 0 && (
                <div style={{ marginTop: 10, paddingLeft: 20 }}>
                  <label style={{ fontSize: 13 }}>
                    Role:
                    <select value={ha.role} onChange={e => updateHa({ role: e.target.value as 'master' | 'backup' })}
                            style={{ marginLeft: 8 }}>
                      <option value="backup">backup</option>
                      <option value="master">master</option>
                    </select>
                  </label>
                </div>
              )}
            </div>
          )}
        </RadioCard>

        <RadioCard checked={ha?.kind === 'new'} onClick={() => setHaKind('new')}
                   title="새 HA 그룹 생성"
                   desc="새 그룹을 만들고 이 서버를 첫 멤버로."
                   color="#27ae60">
          {ha?.kind === 'new' && (
            <div style={{ marginTop: 10, display: 'grid', gridTemplateColumns: '120px 1fr', gap: 8, fontSize: 13 }}>
              <label>그룹명 *</label>
              <input value={ha.name} onChange={e => updateHa({ name: e.target.value })} placeholder="mgmt-as" />
              <label>모드 *</label>
              <select value={ha.mode} onChange={e => updateHa({ mode: e.target.value as HaMode, peerAgentId: null })}>
                <option value="active_standby">Active/Standby (2 노드)</option>
                <option value="all_active">All Active (N 노드)</option>
              </select>
              <label>VIP *</label>
              <input value={ha.vip} onChange={e => updateHa({ vip: e.target.value })} placeholder="10.0.0.100" />
              <label>VIP mask</label>
              <input type="number" value={ha.vipMask}
                     onChange={e => updateHa({ vipMask: parseInt(e.target.value) || 24 })} />
              <label>auth_pass *</label>
              <input value={ha.authPass} onChange={e => updateHa({ authPass: e.target.value })}
                     maxLength={8} placeholder="8 chars max" />
              {ha.mode === 'active_standby' && (
                <>
                  <label>Peer 노드 *</label>
                  <select value={ha.peerAgentId ?? 0}
                          onChange={e => updateHa({ peerAgentId: parseInt(e.target.value) })}>
                    <option value={0}>-- peer 선택 (A/S 는 2 노드) --</option>
                    {peerCandidates.map(a => (
                      <option key={a.id} value={a.id}>{a.name} ({a.ip}, {a.status})</option>
                    ))}
                  </select>
                </>
              )}
              <div style={{ gridColumn: '1 / -1', fontSize: 12, color: '#888', marginTop: 6 }}>
                ℹ VRID 는 자동 할당 (51-255 range). VIP 는 네트워크 대역 의존이라 수동.
              </div>
            </div>
          )}
        </RadioCard>
      </div>
    </div>
  )
}

function RadioCard({ checked, onClick, title, desc, color, children }: {
  checked: boolean
  onClick: () => void
  title: string
  desc: string
  color: string
  children?: React.ReactNode
}) {
  return (
    <div onClick={onClick} style={{
      padding: 12, border: `2px solid ${checked ? color : '#e5e5e5'}`,
      borderRadius: 6, cursor: 'pointer',
      background: checked ? '#fafcfe' : '#fff',
    }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <input type="radio" checked={checked} onChange={onClick} />
        <b>{title}</b>
      </div>
      <div style={{ fontSize: 12, color: '#666', marginTop: 4, marginLeft: 24 }}>{desc}</div>
      {children}
    </div>
  )
}

function modeLabel(m: HaMode): string {
  return m === 'active_standby' ? 'A/S' : 'AA'
}

// ──────────────────────────────────────────────────────────────
//  Step 3 — 모듈 선택
// ──────────────────────────────────────────────────────────────

function Step3Modules({ state, setState, mode }: {
  state: WizardState
  setState: React.Dispatch<React.SetStateAction<WizardState>>
  mode: HaMode | 'standalone'
}) {
  const toggle = (id: number) => {
    setState(prev => ({
      ...prev,
      modules: prev.modules.includes(id) ? prev.modules.filter(x => x !== id) : [...prev.modules, id],
    }))
  }

  const allowedCount = useMemo(
    () => MOCK_PACKAGES.filter(p => moduleAllowed(p, mode)).length,
    [mode]
  )

  return (
    <div>
      <h2 style={{ marginTop: 0 }}>Step 3 — 모듈 선택</h2>
      <div style={{ fontSize: 13, color: '#666', marginBottom: 12 }}>
        이 노드의 HA 모드 <b>{mode}</b> 와 호환되는 모듈만 선택 가능 ({allowedCount}/{MOCK_PACKAGES.length} 개).
        선택 안 한 모듈은 나중에 추가 가능.
      </div>
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(220px, 1fr))', gap: 10 }}>
        {MOCK_PACKAGES.map(p => {
          const ok = moduleAllowed(p, mode)
          const sel = state.modules.includes(p.id)
          return (
            <label key={p.id} style={{
              padding: 10, borderRadius: 6,
              border: `2px solid ${sel && ok ? '#27ae60' : '#e5e5e5'}`,
              background: !ok ? '#f5f5f5' : sel ? '#f0fff4' : '#fff',
              opacity: ok ? 1 : 0.5,
              cursor: ok ? 'pointer' : 'not-allowed',
              display: 'flex', flexDirection: 'column', gap: 4,
            }}>
              <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                <input type="checkbox" disabled={!ok} checked={sel && ok}
                       onChange={() => ok && toggle(p.id)} />
                <b>{p.name}</b>
                <span style={{
                  marginLeft: 'auto', fontSize: 10, padding: '1px 5px', borderRadius: 3,
                  background: capColor(p.ha_capability), color: '#fff',
                }}>{capLabel(p.ha_capability)}</span>
              </div>
              <div style={{ fontSize: 12, color: '#666' }}>{p.description}</div>
              <div style={{ fontSize: 11, color: '#888' }}>v{p.version}</div>
              {!ok && (
                <div style={{ fontSize: 11, color: '#c00' }}>
                  ⚠ {mode} 노드 — {capLabel(p.ha_capability)} 모듈 install 불가
                </div>
              )}
            </label>
          )
        })}
      </div>
      <div style={{ marginTop: 12, fontSize: 13 }}>
        선택: <b>{state.modules.length}</b> 개
      </div>
    </div>
  )
}

function capLabel(c: HaCapability): string {
  return c === 'active_standby' ? 'A/S' : c === 'all_active' ? 'AA' : 'standalone'
}
function capColor(c: HaCapability): string {
  return c === 'active_standby' ? '#3498db' : c === 'all_active' ? '#27ae60' : '#95a5a6'
}

// ──────────────────────────────────────────────────────────────
//  Step 4 — Review + 실행
// ──────────────────────────────────────────────────────────────

function Step4Review({ state }: { state: WizardState }) {
  return (
    <div>
      <h2 style={{ marginTop: 0 }}>Step 4 — 확인 및 실행</h2>
      <pre style={{ background: '#f8f8f8', padding: 16, borderRadius: 6, fontSize: 12, overflow: 'auto' }}>
{renderSummary(state)}
      </pre>
      <div style={{ marginTop: 16, padding: 12, background: '#fff8db',
                    border: '1px solid #f0c75e', borderRadius: 4, fontSize: 13 }}>
        ⚠ <b>prototype</b> — "완료" 클릭 시 alert 만 표시 (실제 createAgent /
        ha-groups CRUD / createDeployment 호출 없음). 실제 wiring 은 후속 라운드.
      </div>
    </div>
  )
}

function renderSummary(s: WizardState): string {
  const lines: string[] = []
  lines.push('── 서버 ──')
  if (!s.server) {
    lines.push('  (미선택)')
  } else if (s.server.mode === 'select') {
    const target = MOCK_AGENTS.find(a => a.id === s.server!.agentId)
    lines.push(`  선택: ${target?.name ?? '?'} (${target?.ip ?? '?'}, ${target?.status ?? '?'})`)
  } else {
    lines.push(`  새 등록: ${s.server.name} — token=${s.server.mockToken ?? '(미발급)'}`)
  }

  lines.push('── HA 구성 ──')
  if (!s.ha) {
    lines.push('  (미설정)')
  } else if (s.ha.kind === 'standalone') {
    lines.push('  단독 (HA 미사용)')
  } else if (s.ha.kind === 'join') {
    const g = MOCK_HA_GROUPS.find(x => x.id === s.ha!.groupId)
    lines.push(`  기존 그룹 편입: ${g?.name ?? '?'} (${modeLabel(g?.mode ?? 'active_standby')}, role=${s.ha.role})`)
  } else {
    lines.push(`  새 그룹: ${s.ha.name} (${modeLabel(s.ha.mode)}, VIP=${s.ha.vip}/${s.ha.vipMask})`)
    if (s.ha.mode === 'active_standby' && s.ha.peerAgentId) {
      const peer = MOCK_AGENTS.find(a => a.id === s.ha!.peerAgentId)
      lines.push(`  peer: ${peer?.name ?? '?'} (${peer?.ip ?? '?'})`)
    }
  }

  lines.push('── 모듈 ──')
  if (s.modules.length === 0) {
    lines.push('  (선택 안 함 — 나중에 추가)')
  } else {
    for (const id of s.modules) {
      const p = MOCK_PACKAGES.find(x => x.id === id)
      lines.push(`  • ${p?.name} v${p?.version} (${capLabel(p?.ha_capability ?? 'standalone')})`)
    }
  }
  return lines.join('\n')
}
