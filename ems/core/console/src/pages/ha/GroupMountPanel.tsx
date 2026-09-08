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
import { X } from 'lucide-react'
import { useState } from 'react'
import type { AgentMount } from '../../api/deployment'
import type { GroupMount, MountOp } from '../../api/ha_groups'
import { ImeSafeInput } from './ImeSafeInput'
import { MOUNT_DEFAULTS } from './helpers'
import { Badge } from '../../components/ui/badge'
import { Button } from '../../components/ui/button'
import { SubSection } from '../../components/custom/collapsible-section'
import { DataTable, Th, Td, orDash } from '../../components/custom/data-table'
import { StatusDot } from '../../components/custom/status-dot'
import { useConfirm } from '../../components/custom/confirm'

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

/**
 * 멤버별 상태 셀 — 시안은 StatusDot + 서버명이다 (Figma G1 187:2936).
 * 미적용은 ES-5 대로 `✕ <서버명>` 인데 글리프 대신 Lucide `X` 를 쓴다.
 */
function StateDot({ state, name, online }: { state: MemberState; name: string; online: boolean }) {
  // 오프라인 멤버는 fan-out 대상에서 빠진다 — 사유를 여기서 알려야 재적용을 무한 반복하지 않는다.
  if (state === 'missing') {
    return (
      <span className="inline-flex items-center gap-1 whitespace-nowrap text-sm text-destructive"
            title={online ? `${name}: 미적용 — [재적용] 필요`
                          : `${name}: 오프라인이라 적용되지 않음 — 노드 복구 후 [재적용]`}>
        <X size={12} /> {name}
      </span>
    )
  }
  return state === 'mounted'
    ? <StatusDot tone="success" label={name} title={`${name}: 마운트됨`} />
    : <StatusDot tone="warning" label={name}
                 title={`${name}: fstab 에는 있으나 지금 마운트 안 됨`} />
}

export function GroupMountPanel({ declared, members, applying, onApply }: {
  declared: GroupMount[]
  members: MountMember[]
  applying?: boolean
  onApply: (ops: MountOp[], label: string) => void
}) {
  const confirm = useConfirm()
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
  // 경로로 붙이므로, 표준 구성이면 [마운트 추가] → [전 멤버에 추가] 두 번이면 끝난다.
  const commitAdd = () => {
    const t = target.trim()  || MOUNT_DEFAULTS.target
    const s = source.trim()  || MOUNT_DEFAULTS.source
    const o = options.trim() || MOUNT_DEFAULTS.options
    onApply([{ op: 'add', fstype: fstype || MOUNT_DEFAULTS.fstype, source: s, target: t, options: o }],
            `그룹 마운트 += ${s} → ${t}`)
    setAddOpen(false)
  }
  const removeMount = async (m: GroupMount) => {
    if (!await confirm({ title: '그룹 마운트 삭제', tone: 'danger', confirmLabel: '삭제', body: <>
      {m.target} 마운트를 <b>전 멤버</b>에서 제거할까요?
      <div className="mt-2">각 노드가 umount + /etc/fstab 의 cims-managed 항목을 삭제합니다.</div>
      <div className="mt-1">대상: {members.map(x => x.name).join(', ') || '(멤버 없음)'}</div>
    </> })) return
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
    // 시안(Figma G1 187:2892~187:2969)은 이 블록을 다른 섹션과 같은 **접힘 섹션** 하나로 둔다 —
    // 구 화면은 회색 패널 안에 별도 제목·우측 [재적용] 을 갖는 딴 살림이었다.
    // 계약상 접힌 섹션에 액션 버튼을 노출하지 않으므로 [재적용] 은 표 아래 액션 줄로 내렸고,
    // 접힌 상태에서도 봐야 하는 `미적용 n건` 만 헤더 우측에 남긴다 (ES-5).
    <SubSection
      title="마운트 (그룹 공통)"
      hint={`멤버 ${members.length}대에 같은 경로로 한 번에 적용 — /etc/fstab 영속`
            + ` · 노드별 예외는 서버 선택 › 네트워크`}
      right={laggingCount > 0
        ? <Badge variant="warningSoft">미적용 {laggingCount}건</Badge>
        : undefined}>
      <DataTable>
        <thead>
          <tr>
            <Th width={150}>마운트 위치(target)</Th>
            <Th width={275}>소스(source)</Th>
            <Th width={60}>유형</Th>
            <Th width={90}>옵션</Th>
            <Th width={175}>멤버별 상태</Th>
            <Th width={72}>액션</Th>
          </tr>
        </thead>
        <tbody>
          {rows.length === 0 && !addOpen && (
            <tr><Td colSpan={6} className="text-muted-foreground">
              그룹 공통 마운트 없음 — 아래 [+ 마운트 추가]
            </Td></tr>
          )}
          {/* 미적용 행을 노란 배경으로 칠하지 않는다 — 배지와 ✕ 로 충분하다 (ES-5). */}
          {rows.map(m => (
            <tr key={m.target}>
              <Td mono>{m.target}</Td>
              <Td mono className="break-all">{orDash(m.source)}</Td>
              <Td>{orDash(m.fstype)}</Td>
              <Td mono className="text-muted-foreground">{orDash(m.options)}</Td>
              <Td>
                {members.length === 0
                  ? <span className="text-muted-foreground">(멤버 없음)</span>
                  : (
                    <div className="flex flex-wrap items-center gap-x-3 gap-y-0.5">
                      {members.map(mem => (
                        <StateDot key={mem.id} name={mem.name} online={mem.online}
                                  state={memberState(mem, m.target)} />
                      ))}
                    </div>
                  )}
              </Td>
              <Td>
                <div className="flex items-center gap-1.5">
                  {lagging(m.target).length > 0 && (
                    <Button variant="outline" onClick={() => applyToLagging(m)} disabled={applying}
                            title={`이 마운트가 없는 멤버에만 적용: ${lagging(m.target).map(x => x.name).join(', ')}`}>
                      없는 멤버에 적용
                    </Button>
                  )}
                  {/* 시안은 여기를 Ghost 로 그렸다 (187:2943) — VIP 행의 Danger 삭제와 다르다. */}
                  <Button variant="ghost" onClick={() => void removeMount(m)} disabled={applying}>삭제</Button>
                </div>
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
                <ImeSafeInput value={options} onCommit={setOptions} placeholder={MOUNT_DEFAULTS.options}
                              className="form-input font-mono" />
              </Td>
              <Td colSpan={2}>
                {/* 빈칸이어도 활성 — 그대로 누르면 위 placeholder 값이 그대로 적용된다. */}
                <div className="flex items-center gap-1.5">
                  <Button variant="default" onClick={commitAdd} disabled={applying}
                          title={(!source.trim() || !target.trim())
                            ? `빈칸은 기본값으로 적용 — ${MOUNT_DEFAULTS.source} → ${MOUNT_DEFAULTS.target}`
                            : '전 멤버에 이 마운트를 추가'}>전 멤버에 추가</Button>
                  <Button variant="ghost" onClick={() => setAddOpen(false)}>취소</Button>
                </div>
              </Td>
            </tr>
          )}
        </tbody>
      </DataTable>

      {/* 액션 줄 — 시안 187:2954: Secondary `+ 마운트 추가` · Ghost `재적용` */}
      <div className="mt-2 flex items-center gap-2">
        <Button variant="outline" onClick={beginAdd}
                disabled={applying || members.length === 0 || addOpen}>
          + 마운트 추가
        </Button>
        <Button variant="ghost" onClick={reapplyAll} disabled={applying || declared.length === 0}
                title="선언된 마운트를 전 멤버에 다시 적용 — 오프라인이었거나 나중에 편입된 멤버 복구">
          재적용
        </Button>
      </div>
      {/* 비활성 사유는 눈에 보이게 (contracts.md §Button) */}
      {members.length === 0 ? (
        <div className="mt-1 text-xs text-muted-foreground">
          [+ 마운트 추가] 는 멤버가 있어야 열립니다 — 좌측 트리에서 서버를 편입하세요.
        </div>
      ) : declared.length === 0 ? (
        <div className="mt-1 text-xs text-muted-foreground">
          [재적용] 은 그룹으로 적용한 마운트가 있을 때 열립니다.
        </div>
      ) : null}
    </SubSection>
  )
}
