// ──────────────────────────────────────────────────────────────
//  GroupMountPanel — 그룹 공통 마운트 (전 멤버에 같은 경로로 일괄 적용).
//
//  모듈 로그를 한곳(NAS)에 모으려면 모든 멤버가 같은 마운트를 가져야 하는데, 서버마다
//  같은 값을 반복 입력하는 것이 실제 운영의 부담이었다. 그래서 **그룹이 선언을 갖고**
//  적용은 fan-out 한다. 노드별 예외는 서버 인스펙터의 [마운트 관리]가 계속 담당한다.
//
//  **표시는 멤버 실측의 집계 하나다.** 그룹에 별도 선언을 두고 그것을 따로 그리지 않는다 —
//  같은 사실이 두 곳에 생기고 화면에도 두 기능처럼 보인다. 행 = 멤버들이 실제로 가진 마운트
//  (agent.mounts, fstab `# cims-managed` + heartbeat `mounted`)의 합집합이고, 어떤 멤버에
//  없으면 그 자리가 ✕ 로 드러난다(오프라인이라 빠진 멤버·나중에 편입된 멤버).
//  버튼은 **작업**이다: 전 멤버에 추가 / 없는 멤버에만 적용 / 전 멤버에서 제거.
// ──────────────────────────────────────────────────────────────
import { useState } from 'react'
import type { AgentMount } from '../../api/deployment'
import type { GroupMount, MountOp } from '../../api/ha_groups'
import { ImeSafeInput } from './ImeSafeInput'
import { MOUNT_DEFAULTS } from './helpers'
import { btnSmall, btnDanger } from './styles'

const FSTYPES = ['nfs', 'nfs4', 'cifs', 'ext4', 'ext3', 'xfs', 'btrfs']

export interface MountMember {
  id: number
  name: string
  online: boolean
  mounts: AgentMount[]
}

/** 멤버 하나의 선언 대비 상태. */
type MemberState = 'mounted' | 'declared' | 'missing'

function memberState(m: MountMember, target: string): MemberState {
  const hit = (m.mounts || []).find(x => x.target === target)
  if (!hit) return 'missing'
  return hit.mounted ? 'mounted' : 'declared'
}

function StateDot({ state, name, online }: { state: MemberState; name: string; online: boolean }) {
  const view = {
    mounted:  { mark: '●', color: 'var(--cims-success)', title: `${name}: 마운트됨` },
    declared: { mark: '◐', color: '#e67e22', title: `${name}: fstab 에는 있으나 지금 마운트 안 됨` },
    missing:  { mark: '✕', color: 'var(--destructive)',
                // 오프라인 멤버는 fan-out 대상에서 빠진다 — 사유를 여기서 알려야 재적용을
                // 무한 반복하지 않는다.
                title: online ? `${name}: 미적용 — [↻ 재적용] 필요`
                              : `${name}: 오프라인이라 적용되지 않음 — 노드 복구 후 [↻ 재적용]` },
  }[state]
  return (
    <span title={view.title} style={{ color: view.color, fontSize: 12, marginRight: 8 }}>
      {view.mark} {name}
    </span>
  )
}

export function GroupMountPanel({ declared, members, applying, onApply }: {
  declared: GroupMount[]
  members: MountMember[]
  applying?: boolean
  onApply: (ops: MountOp[], label: string) => void
}) {
  const [addOpen, setAddOpen] = useState(false)
  const [fstype, setFstype]   = useState('nfs')
  const [source, setSource]   = useState('')
  const [target, setTarget]   = useState('')
  const [options, setOptions] = useState('defaults')

  const beginAdd = () => {
    setAddOpen(true); setFstype(MOUNT_DEFAULTS.fstype)
    setSource(''); setTarget(''); setOptions(MOUNT_DEFAULTS.options)
  }
  // 빈칸은 placeholder 로 보여준 기본값으로 채운다 — 대부분의 노드가 같은 NAS 를 같은
  // 경로로 붙이므로, 표준 구성이면 [＋ 마운트 추가] → [전 멤버에 추가] 두 번이면 끝난다.
  const commitAdd = () => {
    const t = target.trim()  || MOUNT_DEFAULTS.target
    const s = source.trim()  || MOUNT_DEFAULTS.source
    const o = options.trim() || MOUNT_DEFAULTS.options
    onApply([{ op: 'add', fstype: fstype || MOUNT_DEFAULTS.fstype, source: s, target: t, options: o }],
            `그룹 마운트 += ${s} → ${t}`)
    setAddOpen(false)
  }
  const removeMount = (m: GroupMount) => {
    if (!confirm(
      `${m.target} 마운트를 **전 멤버**에서 제거할까요?\n\n` +
      `각 노드가 umount + /etc/fstab 의 cims-managed 항목을 삭제합니다.\n` +
      `대상: ${members.map(x => x.name).join(', ') || '(멤버 없음)'}`)) return
    onApply([{ op: 'del', target: m.target }], `그룹 마운트 -= ${m.target}`)
  }
  // 선언 전체를 다시 내린다 — 오프라인이었거나 나중에 편입된 멤버를 따라잡게 하는 통로.
  const reapplyAll = () => {
    if (declared.length === 0) return
    onApply(declared.map(m => ({
      op: 'add' as const, target: m.target, source: m.source,
      fstype: m.fstype, options: m.options || 'defaults',
    })), `그룹 마운트 재적용 (${declared.length}건)`)
  }

  // 행 = **멤버 실측의 합집합**. 그룹이 따로 선언을 갖지 않는다 — 서버 탭에 붙을 수 있는
  // 것을 여기서 모아 보여주는 것이 전부다(같은 사실을 두 곳에 두면 어긋나고, 화면에도
  // 두 기능처럼 보인다). `declared` 는 이전 그룹 적용 작업의 기록이라 target 만 합쳐
  // "전에 적용했는데 지금 아무 멤버에도 없는" 항목이 사라지지 않게 한다.
  const rows: GroupMount[] = []
  const seen = new Set<string>()
  const push = (target: string, source: string, fstype: string, options?: string) => {
    if (!target || seen.has(target)) return
    seen.add(target)
    rows.push({ target, source, fstype, options: options || 'defaults' })
  }
  for (const mem of members) {
    for (const am of mem.mounts || []) push(am.target, am.source || '', am.fstype || '', am.options)
  }
  for (const d of declared) push(d.target, d.source, d.fstype, d.options)

  // 어떤 멤버에 없으면 미적용 — 판정 기준이 '선언' 이 아니라 '동료 멤버가 가졌는지' 다.
  const lagging = (target: string) =>
    members.filter(m => memberState(m, target) !== 'mounted')
  const laggingCount = rows.reduce((n, r) => n + lagging(r.target).length, 0)

  // 없는 멤버에만 적용 — 전 멤버 재적용보다 좁은 작업(이미 붙은 노드는 건드리지 않는다).
  const applyToLagging = (m: GroupMount) => {
    const miss = lagging(m.target)
    if (!miss.length) return
    if (!m.source || !m.fstype) {
      alert(`${m.target}: 멤버 보고에 source/유형이 없어 적용할 수 없습니다.`); return
    }
    onApply([{ op: 'add', target: m.target, source: m.source,
               fstype: m.fstype, options: m.options || 'defaults' }],
            `미적용 멤버에 적용 (${miss.map(x => x.name).join(', ')}) = ${m.source} → ${m.target}`)
  }

  return (
    <div style={{ borderLeft: '3px solid var(--border)', borderRadius: 4, padding: '10px 12px',
                  background: 'var(--muted)' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 8 }}>
        <div style={{ fontSize: 12, fontWeight: 'bold', color: 'var(--muted-foreground)' }}>
          마운트 (그룹 공통)
        </div>
        <span style={{ fontSize: 11, color: 'var(--muted-foreground)' }}>
          멤버 {members.length}대에 같은 경로로 한 번에 적용 — /etc/fstab 영속.
          노드별 예외는 좌측 트리에서 서버 선택 &gt; [네트워크] 탭.
        </span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 6, alignItems: 'center' }}>
          {laggingCount > 0 && (
            <span style={{ fontSize: 11, color: 'var(--destructive)' }}>미적용 {laggingCount}건</span>
          )}
          <button onClick={reapplyAll} style={btnSmall()}
                  disabled={applying || declared.length === 0}
                  title="선언된 마운트를 전 멤버에 다시 적용 — 오프라인이었거나 나중에 편입된 멤버 복구">
            ↻ 재적용
          </button>
        </div>
      </div>

      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ background: 'var(--secondary)', color: 'var(--muted-foreground)' }}>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 150 }}>마운트 위치(target)</th>
            <th style={{ padding: '4px 8px', textAlign: 'left' }}>소스(source)</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 70 }}>유형</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 130 }}>옵션</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 200 }}>멤버별 상태</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 60 }}>액션</th>
          </tr>
        </thead>
        <tbody>
          {rows.length === 0 && !addOpen && (
            <tr><td colSpan={6} style={{ padding: 8, color: 'var(--muted-foreground)' }}>
              (그룹 공통 마운트 없음 — 아래 [＋ 마운트 추가])
            </td></tr>
          )}
          {rows.map(m => (
            <tr key={m.target}
                style={lagging(m.target).length ? { background: 'var(--cims-warning-soft)' } : undefined}>
              <td style={{ padding: '4px 8px', fontFamily: 'monospace' }}>{m.target}</td>
              <td style={{ padding: '4px 8px', fontFamily: 'monospace', wordBreak: 'break-all' }}>{m.source}</td>
              <td style={{ padding: '4px 8px' }}>{m.fstype}</td>
              <td style={{ padding: '4px 8px', fontFamily: 'monospace', fontSize: 11,
                           color: 'var(--muted-foreground)' }}>{m.options || '-'}</td>
              <td style={{ padding: '4px 8px' }}>
                {members.length === 0
                  ? <span style={{ color: 'var(--muted-foreground)' }}>(멤버 없음)</span>
                  : members.map(mem => (
                      <StateDot key={mem.id} name={mem.name} online={mem.online}
                                state={memberState(mem, m.target)} />
                    ))}
              </td>
              <td style={{ padding: '4px 8px', whiteSpace: 'nowrap' }}>
                {lagging(m.target).length > 0 && (
                  <button onClick={() => applyToLagging(m)} style={btnSmall()} disabled={applying}
                          title={`이 마운트가 없는 멤버에만 적용: ${lagging(m.target).map(x => x.name).join(', ')}`}>
                    없는 멤버에 적용
                  </button>
                )}
                <button onClick={() => removeMount(m)} style={btnDanger()} disabled={applying}>삭제</button>
              </td>
            </tr>
          ))}
          {addOpen ? (
            <tr style={{ background: 'var(--cims-warning-soft)' }}>
              <td style={{ padding: '4px 8px' }}>
                <ImeSafeInput value={target} onCommit={setTarget} placeholder={MOUNT_DEFAULTS.target}
                              style={{ width: '95%', padding: '2px 6px', fontSize: 12,
                                       border: '1px solid #e67e22', borderRadius: 3 }} />
              </td>
              <td style={{ padding: '4px 8px' }}>
                <ImeSafeInput value={source} onCommit={setSource}
                              placeholder={MOUNT_DEFAULTS.source}
                              style={{ width: '95%', padding: '2px 6px', fontSize: 12,
                                       border: '1px solid #e67e22', borderRadius: 3 }} />
              </td>
              <td style={{ padding: '4px 8px' }}>
                <select value={fstype} onChange={e => setFstype(e.target.value)}
                        style={{ width: '95%', padding: '2px 4px', fontSize: 12,
                                 border: '1px solid #e67e22', borderRadius: 3 }}>
                  {FSTYPES.map(t => <option key={t} value={t}>{t}</option>)}
                </select>
              </td>
              <td style={{ padding: '4px 8px' }}>
                <ImeSafeInput value={options} onCommit={setOptions} placeholder={MOUNT_DEFAULTS.options}
                              style={{ width: '95%', padding: '2px 6px', fontSize: 12,
                                       border: '1px solid var(--border)', borderRadius: 3 }} />
              </td>
              <td colSpan={2} style={{ padding: '4px 8px' }}>
                {/* 빈칸이어도 활성 — 그대로 누르면 위 placeholder 값이 그대로 적용된다. */}
                <button onClick={commitAdd} style={btnSmall()} disabled={applying}
                        title={(!source.trim() || !target.trim())
                          ? `빈칸은 기본값으로 적용 — ${MOUNT_DEFAULTS.source} → ${MOUNT_DEFAULTS.target}`
                          : '전 멤버에 이 마운트를 추가'}>전 멤버에 추가</button>
                <button onClick={() => setAddOpen(false)} style={btnSmall()}>취소</button>
              </td>
            </tr>
          ) : (
            <tr>
              <td colSpan={6} style={{ padding: '4px 8px' }}>
                <button onClick={beginAdd} style={btnSmall()} disabled={applying || members.length === 0}
                        title={members.length === 0 ? '멤버가 없습니다 — 좌측 트리에서 서버를 편입하세요' : ''}>
                  ＋ 마운트 추가
                </button>
              </td>
            </tr>
          )}
        </tbody>
      </table>
    </div>
  )
}
