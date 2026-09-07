// ──────────────────────────────────────────────────────────────
//  MountPanel — agent 가 관리하는 마운트(fstab 영속) 추가/삭제.
//  콘솔에서 추가하면 agent 가 fstab 에 기록 → 재부팅 시 OS 가 자동 마운트.
//  네트워크 FS(nfs/cifs)는 agent(cims-priv)가 _netdev,nofail 강제(부팅 hang/실패 차단 방지).
// ──────────────────────────────────────────────────────────────
import { useState } from 'react'
import type { AgentMount } from '../../api/deployment'
import { ImeSafeInput } from './ImeSafeInput'
import { MOUNT_DEFAULTS } from './helpers'
import { btnSmall, btnDanger } from './styles'

const FSTYPES = ['nfs', 'nfs4', 'cifs', 'ext4', 'ext3', 'xfs', 'btrfs']

export function MountPanel({ title, mounts, applying, onApply }: {
  title: string
  mounts: AgentMount[]
  applying?: boolean
  onApply: (
    ops: Array<{ op: 'add'|'del'; fstype?: string; source?: string; target: string; options?: string }>,
    label: string,
  ) => void
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
  // 빈칸은 placeholder 로 보여준 기본값으로 채운다 (그룹 공통 패널과 동일 규칙).
  const commitAdd = () => {
    const t = target.trim()  || MOUNT_DEFAULTS.target
    const s = source.trim()  || MOUNT_DEFAULTS.source
    const o = options.trim() || MOUNT_DEFAULTS.options
    onApply([{ op: 'add', fstype: fstype || MOUNT_DEFAULTS.fstype, source: s, target: t, options: o }],
            `mount += ${s} → ${t}`)
    setAddOpen(false)
  }
  const deleteMount = (m: AgentMount) => {
    if (!confirm(`${m.target} 마운트를 제거할까요?\n(agent 가 umount + /etc/fstab 의 cims-managed 항목 삭제)`)) return
    onApply([{ op: 'del', target: m.target }], `mount -= ${m.target}`)
  }

  return (
    <div style={{ borderLeft: '3px solid var(--border)', borderRadius: 4, padding: '10px 12px',
                  background: 'var(--muted)' }}>
      <div style={{ fontSize: 12, fontWeight: 'bold', color: 'var(--muted-foreground)', marginBottom: 8 }}>
        {title}
        <span style={{ marginLeft: 8, fontSize: 11, color: 'var(--muted-foreground)', fontWeight: 'normal' }}>
          (콘솔 추가 시 /etc/fstab 에 기록 — 재부팅에도 유지. 네트워크 FS 는 _netdev,nofail 자동)
        </span>
      </div>

      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ background: 'var(--secondary)', color: 'var(--muted-foreground)' }}>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 150 }}>마운트 위치(target)</th>
            <th style={{ padding: '4px 8px', textAlign: 'left' }}>소스(source)</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 70 }}>유형</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 150 }}>옵션</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 70 }}>상태</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 70 }}>액션</th>
          </tr>
        </thead>
        <tbody>
          {mounts.length === 0 && !addOpen && (
            <tr><td colSpan={6} style={{ padding: '8px', color: 'var(--muted-foreground)' }}>
              (마운트 없음 — 아래 [＋ 마운트 추가])
            </td></tr>
          )}
          {mounts.map((m) => (
            <tr key={m.target}>
              <td style={{ padding: '4px 8px', fontFamily: 'monospace' }}>{m.target}</td>
              <td style={{ padding: '4px 8px', fontFamily: 'monospace', wordBreak: 'break-all' }}>{m.source}</td>
              <td style={{ padding: '4px 8px' }}>{m.fstype}</td>
              <td style={{ padding: '4px 8px', fontFamily: 'monospace', fontSize: 11, color: 'var(--muted-foreground)' }}>{m.options || '-'}</td>
              <td style={{ padding: '4px 8px', fontSize: 11 }}>
                {m.mounted
                  ? <span style={{ color: 'var(--cims-success)', fontWeight: 'bold' }}>● mounted</span>
                  : <span style={{ color: 'var(--destructive)' }}>○ unmounted</span>}
              </td>
              <td style={{ padding: '4px 8px' }}>
                <button onClick={() => deleteMount(m)} style={btnDanger()} disabled={applying}>삭제</button>
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
                <ImeSafeInput value={source} onCommit={setSource} placeholder={MOUNT_DEFAULTS.source}
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
                {/* 빈칸이어도 활성 — 그대로 누르면 위 placeholder 값이 그대로 적용된다. */}
                <button onClick={commitAdd} style={btnSmall()} disabled={applying}
                        title={(!source.trim() || !target.trim())
                          ? `빈칸은 기본값으로 적용 — ${MOUNT_DEFAULTS.source} → ${MOUNT_DEFAULTS.target}`
                          : '이 서버에 마운트 추가'}>추가</button>
                <button onClick={() => setAddOpen(false)} style={btnSmall()}>취소</button>
              </td>
            </tr>
          ) : (
            <tr>
              <td colSpan={6} style={{ padding: '4px 8px' }}>
                <button onClick={beginAdd} style={btnSmall()} disabled={applying}>＋ 마운트 추가</button>
              </td>
            </tr>
          )}
        </tbody>
      </table>
    </div>
  )
}
