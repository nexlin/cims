// ──────────────────────────────────────────────────────────────
//  GroupMountPanel — 그룹 공통 마운트 (전 멤버에 같은 경로로 일괄 적용).
//
//  모듈 로그를 한곳(NAS)에 모으려면 모든 멤버가 같은 마운트를 가져야 하는데, 서버마다
//  같은 값을 반복 입력하는 것이 실제 운영의 부담이었다. 그래서 **그룹이 선언을 갖고**
//  적용은 fan-out 한다. 노드별 예외는 서버 인스펙터의 [마운트 관리]가 계속 담당한다.
//
//  선언(group.mounts) 과 멤버 실제 상태(agent.mounts + heartbeat mounted) 를 대조해
//  멤버별 ●/◐/✕ 를 표시한다 — 오프라인이라 빠진 멤버, 나중에 편입된 멤버가 드러난다.
// ──────────────────────────────────────────────────────────────
import { useState } from 'react'
import type { AgentMount } from '../../api/deployment'
import type { GroupMount, MountOp } from '../../api/ha_groups'
import { ImeSafeInput } from './ImeSafeInput'
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
    mounted:  { mark: '●', color: '#27ae60', title: `${name}: 마운트됨` },
    declared: { mark: '◐', color: '#e67e22', title: `${name}: fstab 에는 있으나 지금 마운트 안 됨` },
    missing:  { mark: '✕', color: '#c0392b',
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
    setAddOpen(true); setFstype('nfs'); setSource(''); setTarget(''); setOptions('defaults')
  }
  const commitAdd = () => {
    const t = target.trim(), s = source.trim()
    if (!t || !s) return
    onApply([{ op: 'add', fstype, source: s, target: t, options: options.trim() || 'defaults' }],
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

  const laggingCount = declared.reduce(
    (n, d) => n + members.filter(m => memberState(m, d.target) !== 'mounted').length, 0)

  return (
    <div style={{ borderLeft: '3px solid var(--border)', borderRadius: 4, padding: '10px 12px',
                  background: 'var(--bg-soft)' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 8 }}>
        <div style={{ fontSize: 12, fontWeight: 'bold', color: 'var(--text-muted)' }}>
          마운트 (그룹 공통)
        </div>
        <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>
          멤버 {members.length}대에 같은 경로로 한 번에 적용 — /etc/fstab 영속.
          노드별 예외는 좌측 트리에서 서버 선택 &gt; [네트워크] 탭.
        </span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 6, alignItems: 'center' }}>
          {laggingCount > 0 && (
            <span style={{ fontSize: 11, color: '#c0392b' }}>미적용 {laggingCount}건</span>
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
          <tr style={{ background: 'var(--surface-2)', color: 'var(--text-muted)' }}>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 150 }}>마운트 위치(target)</th>
            <th style={{ padding: '4px 8px', textAlign: 'left' }}>소스(source)</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 70 }}>유형</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 130 }}>옵션</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 200 }}>멤버별 상태</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 60 }}>액션</th>
          </tr>
        </thead>
        <tbody>
          {declared.length === 0 && !addOpen && (
            <tr><td colSpan={6} style={{ padding: 8, color: 'var(--text-muted)' }}>
              (그룹 공통 마운트 없음 — 아래 [＋ 마운트 추가])
            </td></tr>
          )}
          {declared.map(m => (
            <tr key={m.target}>
              <td style={{ padding: '4px 8px', fontFamily: 'monospace' }}>{m.target}</td>
              <td style={{ padding: '4px 8px', fontFamily: 'monospace', wordBreak: 'break-all' }}>{m.source}</td>
              <td style={{ padding: '4px 8px' }}>{m.fstype}</td>
              <td style={{ padding: '4px 8px', fontFamily: 'monospace', fontSize: 11,
                           color: 'var(--text-muted)' }}>{m.options || '-'}</td>
              <td style={{ padding: '4px 8px' }}>
                {members.length === 0
                  ? <span style={{ color: 'var(--text-muted)' }}>(멤버 없음)</span>
                  : members.map(mem => (
                      <StateDot key={mem.id} name={mem.name} online={mem.online}
                                state={memberState(mem, m.target)} />
                    ))}
              </td>
              <td style={{ padding: '4px 8px' }}>
                <button onClick={() => removeMount(m)} style={btnDanger()} disabled={applying}>삭제</button>
              </td>
            </tr>
          ))}
          {addOpen ? (
            <tr style={{ background: 'var(--warn-soft)' }}>
              <td style={{ padding: '4px 8px' }}>
                <ImeSafeInput value={target} onCommit={setTarget} placeholder="/mnt/cims-log"
                              style={{ width: '95%', padding: '2px 6px', fontSize: 12,
                                       border: '1px solid #e67e22', borderRadius: 3 }} />
              </td>
              <td style={{ padding: '4px 8px' }}>
                <ImeSafeInput value={source} onCommit={setSource}
                              placeholder="121.161.164.105:/home/cbm/NAS/log"
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
                <ImeSafeInput value={options} onCommit={setOptions} placeholder="defaults"
                              style={{ width: '95%', padding: '2px 6px', fontSize: 12,
                                       border: '1px solid var(--border)', borderRadius: 3 }} />
              </td>
              <td colSpan={2} style={{ padding: '4px 8px' }}>
                <button onClick={commitAdd} style={btnSmall()}
                        disabled={!source.trim() || !target.trim() || applying}>전 멤버에 추가</button>
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
