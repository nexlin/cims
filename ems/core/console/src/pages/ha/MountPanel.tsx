// ──────────────────────────────────────────────────────────────
//  MountPanel — agent 가 관리하는 마운트(fstab 영속) 추가/삭제.
//  콘솔에서 추가하면 agent 가 fstab 에 기록 → 재부팅 시 OS 가 자동 마운트.
//  네트워크 FS(nfs/cifs)는 agent(cims-priv)가 _netdev,nofail 강제(부팅 hang/실패 차단 방지).
// ──────────────────────────────────────────────────────────────
import { Trash2 } from 'lucide-react'
import { useState } from 'react'
import type { AgentMount } from '../../api/deployment'
import { ImeSafeInput } from './ImeSafeInput'
import { MOUNT_DEFAULTS } from './helpers'
import { Button } from '../../components/ui/button'
import { DataTable, Th, Td, orDash } from '../../components/custom/data-table'
import { StatusDot } from '../../components/custom/status-dot'

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
    // 상위 SubSection 이 제목·힌트·들여쓰기를 그린다 — 여기서 또 회색 패널을 두르지 않는다
    // (시안 S1 은 섹션 본문에 표만 있다).
    <div>
      {/* 다른 화면에서 단독으로 쓸 때만 자체 제목을 낸다 */}
      {title && (
        <div className="mb-2 text-sm font-semibold text-muted-foreground">
          {title}
          <span className="ml-2 text-xs font-normal">
            (콘솔 추가 시 /etc/fstab 에 기록 — 재부팅에도 유지. 네트워크 FS 는 _netdev,nofail 자동)
          </span>
        </div>
      )}
      {/* 컬럼 폭은 Figma S1 실측 (그룹 공통 마운트 표와 같은 축) */}
      <DataTable>
        <thead>
          <tr>
            <Th width={150}>마운트 위치(target)</Th>
            <Th width={275}>소스(source)</Th>
            <Th width={60}>유형</Th>
            <Th width={150}>옵션</Th>
            <Th width={90}>상태</Th>
            <Th width={72}>액션</Th>
          </tr>
        </thead>
        <tbody>
          {mounts.length === 0 && !addOpen && (
            <tr><Td colSpan={6} className="text-muted-foreground">
              마운트 없음 — 아래 [+ 마운트 추가]
            </Td></tr>
          )}
          {mounts.map((m) => (
            <tr key={m.target}>
              <Td mono>{m.target}</Td>
              <Td mono className="break-all">{orDash(m.source)}</Td>
              <Td>{orDash(m.fstype)}</Td>
              <Td mono className="text-muted-foreground">{orDash(m.options)}</Td>
              <Td>
                <StatusDot tone={m.mounted ? 'success' : 'danger'}
                           label={m.mounted ? 'mounted' : 'unmounted'} />
              </Td>
              <Td>
                <Button variant="destructive" onClick={() => deleteMount(m)} disabled={applying}>
                  <Trash2 /> 삭제
                </Button>
              </Td>
            </tr>
          ))}
          {addOpen && (
            <tr className="bg-warning-soft">
              <Td>
                <ImeSafeInput value={target} onCommit={setTarget} placeholder={MOUNT_DEFAULTS.target}
                              className="form-input font-mono" />
              </Td>
              <Td>
                <ImeSafeInput value={source} onCommit={setSource} placeholder={MOUNT_DEFAULTS.source}
                              className="form-input font-mono" />
              </Td>
              <Td>
                <select value={fstype} onChange={e => setFstype(e.target.value)} className="form-input">
                  {FSTYPES.map(t => <option key={t} value={t}>{t}</option>)}
                </select>
              </Td>
              <Td>
                <ImeSafeInput value={options} onCommit={setOptions} placeholder="defaults"
                              className="form-input font-mono" />
              </Td>
              <Td colSpan={2}>
                {/* 빈칸이어도 활성 — 그대로 누르면 위 placeholder 값이 그대로 적용된다. */}
                <div className="flex items-center gap-1.5">
                  <Button variant="default" onClick={commitAdd} disabled={applying}
                          title={(!source.trim() || !target.trim())
                            ? `빈칸은 기본값으로 적용 — ${MOUNT_DEFAULTS.source} → ${MOUNT_DEFAULTS.target}`
                            : '이 서버에 마운트 추가'}>추가</Button>
                  <Button variant="ghost" onClick={() => setAddOpen(false)}>취소</Button>
                </div>
              </Td>
            </tr>
          )}
        </tbody>
      </DataTable>
      <Button variant="outline" className="mt-2.5" onClick={beginAdd} disabled={applying || addOpen}>
        + 마운트 추가
      </Button>
    </div>
  )
}
