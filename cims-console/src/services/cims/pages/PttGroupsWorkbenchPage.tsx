import { useState, useEffect, useCallback, useMemo } from 'react'
import IconBtn from '../../../components/IconBtn'
import { Pencil, Trash2, Check, X, ChevronRight, ChevronDown, ArrowLeft, ArrowRight, Crown } from 'lucide-react'
import { groupsApi, type Group, type GroupInput, type Member } from '../../../api/groups'
import { usersApi, type UserSummary } from '../../../api/users'
import { orgApi, type Organization } from '../../../api/organizations'
import OrgTreePanel from '../../../components/OrgTreePanel'
import { DataTable, type Column } from '../../../components/DataTable'
import SubscriberPicker, { buildPickIndex, type PickItem } from '../../../components/SubscriberPicker'
import { useToast } from '../../../components/Toast'
import { useAuth } from '../../../contexts/AuthContext'
import { canCreateGroup, canManageGroup, hasRole } from '../../../utils/permissions'

// ── PTT 그룹 워크벤치 ─────────────────────────────────────────
//  좌: 조직트리(공유 스코프) | 그룹 DataTable | 행 확장: 속성 편집 + 멤버(다중선택 추가).
//  그룹 = 사용자/번호를 가로지르는 N:M 집합 → 사용자 워크벤치와 분리된 독립 메뉴.

type GroupExt = Group

const ICON = 14


function Caret({ open }: { open: boolean }) {
  return <span style={{ color: 'var(--text-muted)', display: 'inline-flex' }}>
    {open ? <ChevronDown size={15} /> : <ChevronRight size={15} />}
  </span>
}

export default function PttGroupsWorkbenchPage() {
  const { show } = useToast()
  const { user: me } = useAuth()
  const canGroupCreate = canCreateGroup(me)

  const [orgScope, setOrgScope] = useState<string | null>(null)
  const [orgName, setOrgName] = useState('전체')
  const [search, setSearch] = useState('')

  const [users, setUsers] = useState<UserSummary[]>([])
  const [orgs, setOrgs] = useState<Organization[]>([])
  const [groups, setGroups] = useState<GroupExt[]>([])
  const [loading, setLoading] = useState(true)

  const [openId, setOpenId] = useState<string | null>(null)
  const [adding, setAdding] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [g, u, o] = await Promise.all([groupsApi.list(), usersApi.list(), orgApi.list()])
      setGroups(g as GroupExt[]); setUsers(u); setOrgs(o)
    } catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [show])
  useEffect(() => { load() }, [load])

  const orgPathOf = useCallback((code: string) => orgs.find(o => o.code === code)?.code_path || code, [orgs])
  const inScope = useCallback((orgCode: string) => {
    if (!orgScope) return true
    return (orgPathOf(orgCode) || '').startsWith(orgScope)
  }, [orgScope, orgPathOf])

  // 멤버=PTT 번호(검색·선택), 소유자=사용자(person) — 두 인덱스 1회 구성 후 공유
  const pttIndex = useMemo(() => buildPickIndex(users, 'ptt'), [users])
  const userIndex = useMemo(() => buildPickIndex(users, 'user'), [users])
  // PTT MSISDN → 표시명 매핑 (멤버 행에 이름 노출용)
  const pttName = useMemo(() => {
    const m = new Map<string, string>()
    for (const it of pttIndex) m.set(it.value, it.userName)
    return m
  }, [pttIndex])

  const groupRows = useMemo(() => {
    const q = search.trim().toLowerCase()
    return groups.filter(g => (!orgScope || inScope(g.org_code || '')) &&
      (!q || g.id.toLowerCase().includes(q) || g.name.toLowerCase().includes(q)))
  }, [groups, orgScope, inScope, search])

  function toggleOpen(id: string) {
    setAdding(false)
    setOpenId(cur => cur === id ? null : id)
  }

  async function deleteGroup(id: string) {
    if (!confirm(`그룹 ${id} 삭제?`)) return
    try { await groupsApi.delete(id); show('삭제', 'ok'); load(); if (openId === id) setOpenId(null) }
    catch (e: unknown) { show(String(e), 'err') }
  }

  const groupCols: Column<GroupExt>[] = [
    { key: 'exp', header: '', width: 26, render: g => <Caret open={openId === g.id} /> },
    { key: 'name', header: '그룹명', sortable: true, render: g => (
      <span><span style={{ fontWeight: 600 }}>{g.name}</span>
        {g.encryption && <span className="badge badge--green" style={{ fontSize: 9, marginLeft: 4 }}>암호</span>}
        {g.emergency_call && <span className="badge badge--red" style={{ fontSize: 9, marginLeft: 2 }}>긴급</span>}
        {g.video_enabled && <span className="badge badge--blue" style={{ fontSize: 9, marginLeft: 2 }}>영상</span>}
      </span>
    ) },
    { key: 'id', header: 'ID', width: 130, sortable: true, render: g => <span className="ts">{g.id}</span> },
    { key: 'type', header: '타입', width: 90, render: g => <span className="ts">{g.group_type || 'prearranged'}</span> },
    { key: 'priority', header: '우선', width: 56, align: 'center', sortable: true, sortValue: g => g.priority ?? 5, render: g => g.priority ?? 5 },
    { key: 'owner', header: '소유자', width: 110, render: g => <span className="ts">{g.authorized_user_name || g.authorized_user || '—'}</span> },
    { key: 'org', header: '조직', width: 130, render: g => <span className="ts">{orgs.find(o => o.code === g.org_code)?.name || g.org_code || '—'}</span> },
    { key: 'members', header: '멤버', width: 64, align: 'center', render: g => <span className="ts">{g.members?.length ?? 0}명</span> },
    { key: 'act', header: '', width: 84, align: 'right', render: g => canManageGroup(me, g.authorized_user_id) ? (
      <span className="actions" onClick={e => e.stopPropagation()}>
        <IconBtn title="편집" onClick={() => toggleOpen(g.id)}><Pencil size={ICON} /></IconBtn>
        <IconBtn title="삭제" tone="danger" onClick={() => deleteGroup(g.id)}><Trash2 size={ICON} /></IconBtn>
      </span>
    ) : <span className="ts">—</span> },
  ]

  const openGroup = openId ? groups.find(g => g.id === openId) : undefined

  return (
    <div style={{ display: 'flex', gap: 16, alignItems: 'stretch', height: 'calc(100vh - 92px)' }}>
      {/* 좌: 조직 트리 (공유 스코프) */}
      <OrgTreePanel fill selectedPath={orgScope} onSelect={(p, n) => { setOrgScope(p); setOrgName(n) }}
        style={{ flex: '0 0 200px', width: 200, maxWidth: 200 }} />

      {/* 중: 패널 = 툴바 + 테이블 */}
      <div className="panel" style={{ flex: 1, minWidth: 0 }}>
        <div className="toolbar">
          <span style={{ fontWeight: 600, fontSize: 13 }}>{orgName}</span>
          <input className="search-input" placeholder="그룹명·ID 검색" value={search}
            onChange={e => setSearch(e.target.value)} style={{ maxWidth: 220 }} />
          {search && <button className="btn btn--ghost btn--sm" onClick={() => setSearch('')}>✕</button>}
          <span style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
            {canGroupCreate && (
              <button className="btn btn--primary btn--sm" onClick={() => { setOpenId(null); setAdding(a => !a) }}>＋ 그룹</button>
            )}
          </span>
        </div>

        {/* 신규 그룹 추가 (테이블 위 블록) */}
        {adding && (
          <div style={{ borderBottom: '1px solid var(--border)', background: 'var(--bg-soft)', padding: '12px 16px' }}>
            <div style={{ fontWeight: 600, fontSize: 12, color: 'var(--primary)', marginBottom: 8 }}>새 PTT 그룹</div>
            <GroupDrawer mode="add" orgs={orgs} me={me} canGroupCreate={canGroupCreate}
              pttIndex={pttIndex} userIndex={userIndex} pttName={pttName} orgScope={orgScope} orgPathOf={orgPathOf}
              onClose={() => setAdding(false)} onSaved={() => { setAdding(false); load() }} reload={load} />
          </div>
        )}

        <DataTable<GroupExt> columns={groupCols} rows={groupRows} rowKey={g => g.id} loading={loading}
          onRowClick={g => toggleOpen(g.id)}
          expandedKey={openId}
          renderExpanded={openGroup ? () => (
            <div style={{ padding: '12px 16px' }}>
              <GroupDrawer key={openGroup.id} mode="view" group={openGroup} orgs={orgs} me={me} canGroupCreate={canGroupCreate}
                pttIndex={pttIndex} userIndex={userIndex} pttName={pttName} orgScope={orgScope} orgPathOf={orgPathOf}
                onClose={() => setOpenId(null)} onSaved={() => { setOpenId(null); load() }} reload={load} />
            </div>
          ) : undefined}
          pageSize={50} emptyText="그룹 없음" />
      </div>
    </div>
  )
}

// ════════════════════════════════════════════════════════════
//  그룹 드로어 (속성 편집 + 멤버 관리)
// ════════════════════════════════════════════════════════════
interface GroupDrawerProps {
  mode: 'view' | 'add'
  group?: GroupExt
  orgs: Organization[]
  me: ReturnType<typeof useAuth>['user']
  canGroupCreate: boolean
  pttIndex: PickItem[]
  userIndex: PickItem[]
  pttName: Map<string, string>
  orgScope: string | null
  orgPathOf: (code: string) => string
  onClose: () => void
  onSaved: () => void
  reload: () => void
}

function GroupDrawer(p: GroupDrawerProps) {
  const { show } = useToast()
  const existing = p.group
  const canManage = canManageGroup(p.me, existing?.authorized_user_id)
  const isNew = p.mode === 'add'
  const allowOwner = hasRole(p.me, 'manager') || (isNew && p.canGroupCreate)
  const [editing, setEditing] = useState(isNew)

  const [form, setForm] = useState<Partial<GroupExt>>(() => existing
    ? { name: existing.name, priority: existing.priority ?? 5, encryption: existing.encryption, emergency_call: existing.emergency_call, imminent_peril_call: existing.imminent_peril_call ?? true, emergency_alert: existing.emergency_alert ?? true, adhoc_enabled: existing.adhoc_enabled ?? false, video_enabled: existing.video_enabled, org_code: existing.org_code || '', authorized_user_id: existing.authorized_user_id ?? null, group_type: existing.group_type }
    : { id: '', name: '', priority: 5, encryption: false, emergency_call: false, imminent_peril_call: true, emergency_alert: true, adhoc_enabled: false, video_enabled: false, org_code: '', group_type: 'prearranged', authorized_user_id: null })
  // 소유자 표시명 (피커 선택 결과 보존)
  const [ownerName, setOwnerName] = useState<string>(existing?.authorized_user_name || '')

  const [members, setMembers] = useState<Member[]>([])
  const existingId = existing?.id
  const reloadMembers = useCallback(() => {
    if (existingId) groupsApi.listMembers(existingId).then(setMembers).catch(() => setMembers([]))
  }, [existingId])
  useEffect(() => { reloadMembers() }, [reloadMembers])

  const groupTypeHint: Record<string, string> = {
    prearranged: '미리 구성된 그룹콜 (키업 시 on-demand 개설)',
    chat: '상시 유지 채팅형 그룹',
    broadcast: '개시자만 발언, 나머지는 수신 전용',
  }

  async function save() {
    if (!form.name || (isNew && !form.id)) { show('ID/이름 필수', 'err'); return }
    const body = { ...form }
    try {
      if (existing) { await groupsApi.update(existing.id, body as Partial<GroupInput>); show('저장', 'ok'); setEditing(false); p.reload() }
      else { await groupsApi.create(body as GroupInput); show('저장', 'ok'); p.onSaved() }
    } catch (e: unknown) { show(String(e), 'err') }
  }
  // 우→좌 이동(추가): 다중 addMember 병렬 (백엔드 upsert)
  async function addMembers(ids: string[], priority: number, role: 'chair' | 'participant') {
    if (!existing || !ids.length) return
    const results = await Promise.allSettled(ids.map(uid => groupsApi.addMember(existing.id, { user_id: uid, priority, role })))
    const ok = results.filter(r => r.status === 'fulfilled').length
    const fail = results.length - ok
    show(fail ? `${ok}명 추가, ${fail}명 실패` : `${ok}명 추가`, fail ? 'err' : 'ok')
    reloadMembers(); p.reload()
  }
  // 좌→우 이동(제거): 다중 removeMember 병렬
  async function removeMembers(ids: string[]) {
    if (!existing || !ids.length) return
    await Promise.allSettled(ids.map(uid => groupsApi.removeMember(existing.id, uid)))
    show(`${ids.length}명 제거`, 'ok')
    reloadMembers(); p.reload()
  }
  // priority/role 변경 = addMember upsert (백엔드 ON DUPLICATE KEY UPDATE)
  async function saveMember(uid: string, priority: number, role: 'chair' | 'participant') {
    if (!existing) return
    try { await groupsApi.addMember(existing.id, { user_id: uid, priority, role }); show('수정', 'ok'); reloadMembers(); p.reload() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  const memberIds = useMemo(() => new Set(members.map(m => m.user_id)), [members])

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10, fontSize: 13 }}>
      {/* ── 속성 ── */}
      {editing ? (
        <FieldRow>
          {isNew && <Field label="그룹 ID *" w={130}><input className="form-input" autoFocus value={form.id || ''} onChange={e => setForm({ ...form, id: e.target.value })} /></Field>}
          <Field label="그룹명 *" w={160}><input className="form-input" value={form.name || ''} onChange={e => setForm({ ...form, name: e.target.value })} /></Field>
          <Field label="타입" w={150}>
            <select className="form-input" value={form.group_type || 'prearranged'} onChange={e => setForm({ ...form, group_type: e.target.value as GroupExt['group_type'] })}>
              <option value="prearranged">prearranged</option>
              <option value="chat">chat</option>
              <option value="broadcast">broadcast</option>
            </select>
          </Field>
          <Field label="우선순위" w={80}><input className="form-input" type="number" value={form.priority ?? 5} onChange={e => setForm({ ...form, priority: Number(e.target.value) })} /></Field>
          {allowOwner && <Field label="소유자 (가입자 검색)" w={230}>
            {form.authorized_user_id != null
              ? <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                  <span className="badge badge--blue" style={{ fontSize: 11 }}>{ownerName || `user#${form.authorized_user_id}`}</span>
                  <button className="btn btn--ghost btn--sm" onClick={() => { setForm({ ...form, authorized_user_id: null }); setOwnerName('') }}>변경</button>
                </div>
              : <SubscriberPicker kind="user" index={p.userIndex} orgScope={p.orgScope} orgPathOf={p.orgPathOf}
                  placeholder={isNew && !hasRole(p.me, 'manager') ? '비우면 본인' : '소유자 이름 검색'}
                  onPick={it => { setForm({ ...form, authorized_user_id: Number(it.value) }); setOwnerName(it.label) }} />}
          </Field>}
          <Field label="조직 코드" w={170}>
            <select className="form-input" value={form.org_code || ''} onChange={e => setForm({ ...form, org_code: e.target.value })}>
              <option value="">없음</option>
              {p.orgs.map(o => <option key={o.id} value={o.code}>{o.name} ({o.code})</option>)}
            </select>
          </Field>
          <div style={{ display: 'flex', gap: 12, alignItems: 'center', alignSelf: 'center', flexWrap: 'wrap' }}>
            <label style={{ display: 'flex', alignItems: 'center', gap: 4 }}><input type="checkbox" checked={form.encryption || false} onChange={e => setForm({ ...form, encryption: e.target.checked })} />암호</label>
            <label style={{ display: 'flex', alignItems: 'center', gap: 4 }}><input type="checkbox" checked={form.video_enabled || false} onChange={e => setForm({ ...form, video_enabled: e.target.checked })} />영상</label>
            <label title="allow-MCPTT-emergency-call" style={{ display: 'flex', alignItems: 'center', gap: 4 }}><input type="checkbox" checked={form.emergency_call || false} onChange={e => setForm({ ...form, emergency_call: e.target.checked })} />긴급콜</label>
            <label title="allow-imminent-peril-call" style={{ display: 'flex', alignItems: 'center', gap: 4 }}><input type="checkbox" checked={form.imminent_peril_call ?? true} onChange={e => setForm({ ...form, imminent_peril_call: e.target.checked })} />임박위험</label>
            <label title="allow-MCPTT-emergency-alert" style={{ display: 'flex', alignItems: 'center', gap: 4 }}><input type="checkbox" checked={form.emergency_alert ?? true} onChange={e => setForm({ ...form, emergency_alert: e.target.checked })} />긴급경보</label>
            <label title="ad hoc 그룹콜 허용 (Rel-18)" style={{ display: 'flex', alignItems: 'center', gap: 4 }}><input type="checkbox" checked={form.adhoc_enabled || false} onChange={e => setForm({ ...form, adhoc_enabled: e.target.checked })} />애드혹</label>
          </div>
          <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
            <button className="btn btn--sm btn--primary" onClick={save}>저장</button>
            <button className="btn btn--sm btn--ghost" onClick={() => isNew ? p.onClose() : setEditing(false)}>취소</button>
          </div>
          <div style={{ flexBasis: '100%', fontSize: 11, color: 'var(--text-muted)' }}>타입: {groupTypeHint[form.group_type || 'prearranged']}</div>
        </FieldRow>
      ) : existing && (
        <div style={{ display: 'flex', alignItems: 'center', gap: 16, flexWrap: 'wrap', fontSize: 12 }}>
          <span className="ts">ID {existing.id}</span>
          <span className="ts">타입 {existing.group_type || 'prearranged'}</span>
          <span className="ts">우선순위 {existing.priority ?? 5}</span>
          <span className="ts">소유자 {existing.authorized_user_name || existing.authorized_user || '—'}</span>
          <span className="ts">조직 {p.orgs.find(o => o.code === existing.org_code)?.name || existing.org_code || '—'}</span>
          {canManage && <button className="btn btn--sm btn--outline" style={{ marginLeft: 'auto' }} onClick={() => setEditing(true)}>그룹 속성 편집</button>}
        </div>
      )}

      {/* ── 멤버 (좌:등록 ↔ 우:조직트리+가입자, 좌우 이동) ── */}
      {existing && (
        <MemberTransfer members={members} memberIds={memberIds} pttIndex={p.pttIndex} pttName={p.pttName}
          canManage={canManage} orgScope={p.orgScope} orgPathOf={p.orgPathOf}
          onAdd={addMembers} onRemove={removeMembers} onSaveMember={saveMember} />
      )}
    </div>
  )
}

// 멤버/후보 공용 우선순위 칩
function PriChip({ n }: { n: number }) {
  return <span style={{ fontSize: 10, fontWeight: 600, color: 'var(--text-muted)', background: 'var(--bg-soft)', border: '1px solid var(--border)', borderRadius: 999, padding: '1px 7px' }}>P{n}</span>
}

// ── 멤버 행 (좌측 패널) — 선택 + 우선순위/역할 인라인 편집 ──
function MemberRow({ m, name, selected, canManage, onToggle, onSave, onRemove }: {
  m: Member; name?: string; selected: boolean; canManage: boolean
  onToggle: (uid: string) => void
  onSave: (uid: string, priority: number, role: 'chair' | 'participant') => void
  onRemove: (uid: string) => void
}) {
  const [editing, setEditing] = useState(false)
  const [pri, setPri] = useState(m.priority)
  const [role, setRole] = useState<'chair' | 'participant'>(m.role === 'chair' ? 'chair' : 'participant')

  return (
    <div style={{
      display: 'flex', alignItems: 'center', gap: 8, padding: '6px 10px', fontSize: 12,
      borderLeft: selected ? '3px solid var(--primary)' : '3px solid transparent',
      background: editing ? 'rgba(74,144,217,0.08)' : selected ? 'rgba(74,144,217,0.06)' : undefined,
    }}>
      {canManage && <input type="checkbox" checked={selected} onChange={() => onToggle(m.user_id)} />}
      <span style={{ display: 'flex', flexDirection: 'column', minWidth: 0, flex: 1 }}>
        <span style={{ fontWeight: 600, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
          {name || '—'}
          {!editing && m.role === 'chair' && <Crown size={11} style={{ marginLeft: 4, verticalAlign: '-1px', color: '#d9a400' }} />}
        </span>
        <span className="ts" style={{ color: 'var(--text-muted)', fontSize: 11 }}>{m.user_id}</span>
      </span>
      {editing ? (
        <>
          <input className="form-input" type="number" value={pri} title="우선순위" onChange={e => setPri(Number(e.target.value))} style={{ width: 52 }} />
          <select className="form-input" value={role} onChange={e => setRole(e.target.value as 'chair' | 'participant')} style={{ width: 104 }}>
            <option value="participant">participant</option>
            <option value="chair">chair (의장)</option>
          </select>
          <IconBtn title="저장" tone="primary" onClick={() => { onSave(m.user_id, pri, role); setEditing(false) }}><Check size={ICON} /></IconBtn>
          <IconBtn title="취소" onClick={() => { setPri(m.priority); setRole(m.role === 'chair' ? 'chair' : 'participant'); setEditing(false) }}><X size={ICON} /></IconBtn>
        </>
      ) : (
        <>
          <PriChip n={m.priority} />
          {canManage && <>
            <IconBtn title="우선순위·역할 편집" onClick={() => setEditing(true)}><Pencil size={ICON} /></IconBtn>
            <IconBtn title="제거" tone="danger" onClick={() => onRemove(m.user_id)}><ArrowRight size={ICON} /></IconBtn>
          </>}
        </>
      )}
    </div>
  )
}

// ════════════════════════════════════════════════════════════
//  멤버 transfer (좌: 등록 멤버 ↔ 우: 조직트리 + PTT 가입자)
// ════════════════════════════════════════════════════════════
function MemberTransfer({ members, memberIds, pttIndex, pttName, canManage, orgScope, orgPathOf, onAdd, onRemove, onSaveMember }: {
  members: Member[]
  memberIds: Set<string>
  pttIndex: PickItem[]
  pttName: Map<string, string>
  canManage: boolean
  orgScope: string | null
  orgPathOf: (code: string) => string
  onAdd: (ids: string[], priority: number, role: 'chair' | 'participant') => Promise<void>
  onRemove: (ids: string[]) => Promise<void>
  onSaveMember: (uid: string, priority: number, role: 'chair' | 'participant') => void
}) {
  // 좌측 선택(제거 대상)
  const [selMembers, setSelMembers] = useState<Set<string>>(new Set())
  // 우측 후보 탐색/선택
  const [treeScope, setTreeScope] = useState<string | null>(orgScope)
  const [treeName, setTreeName] = useState('전체')
  const [q, setQ] = useState('')
  const [picked, setPicked] = useState<Set<string>>(new Set())
  const [bulkPri, setBulkPri] = useState(5)
  const [bulkRole, setBulkRole] = useState<'chair' | 'participant'>('participant')
  const [busy, setBusy] = useState(false)

  const candidates = useMemo(() => {
    const s = q.trim().toLowerCase()
    const inScope = (it: PickItem) => !treeScope || (orgPathOf(it.orgCode) || '').startsWith(treeScope)
    return pttIndex
      .filter(it => !memberIds.has(it.value))
      .filter(inScope)
      .filter(it => !s || it.userName.toLowerCase().includes(s) || it.value.toLowerCase().includes(s) || it.orgCode.toLowerCase().includes(s))
      .slice(0, 500)
  }, [pttIndex, memberIds, treeScope, orgPathOf, q])

  const allCandPicked = candidates.length > 0 && candidates.every(c => picked.has(c.value))
  const allMemSel = members.length > 0 && members.every(m => selMembers.has(m.user_id))

  function toggleCand(v: string) { setPicked(p => { const n = new Set(p); if (n.has(v)) n.delete(v); else n.add(v); return n }) }
  function toggleMem(v: string) { setSelMembers(p => { const n = new Set(p); if (n.has(v)) n.delete(v); else n.add(v); return n }) }
  function toggleAllCand() { setPicked(p => { const n = new Set(p); if (allCandPicked) candidates.forEach(c => n.delete(c.value)); else candidates.forEach(c => n.add(c.value)); return n }) }
  function toggleAllMem() { setSelMembers(p => { const n = new Set(p); if (allMemSel) members.forEach(m => n.delete(m.user_id)); else members.forEach(m => n.add(m.user_id)); return n }) }

  async function doAdd(ids: string[]) {
    if (!ids.length) return
    setBusy(true); await onAdd(ids, bulkPri, bulkRole); setBusy(false); setPicked(new Set())
  }
  async function doRemove(ids: string[]) {
    if (!ids.length) return
    setBusy(true); await onRemove(ids); setBusy(false); setSelMembers(new Set())
  }

  const panelHead: React.CSSProperties = { display: 'flex', alignItems: 'center', gap: 8, padding: '8px 10px', borderBottom: '1px solid var(--border)', fontSize: 12, fontWeight: 600 }
  const countChip = (n: number, tone?: 'primary') => <span className={`badge ${tone === 'primary' ? 'badge--blue' : 'badge--gray'}`} style={{ fontSize: 10 }}>{n}</span>
  const panel: React.CSSProperties = { display: 'flex', flexDirection: 'column', minWidth: 0, border: '1px solid var(--border)', borderRadius: 10, overflow: 'hidden', background: 'var(--bg-card, #fff)' }

  return (
    <div style={{ marginTop: 10, borderTop: '1px solid var(--border)', paddingTop: 12 }}>
      <div style={{ display: 'flex', alignItems: 'stretch', gap: 10, height: 360 }}>
        {/* ── 좌: 등록된 멤버 ── */}
        <div style={{ ...panel, flex: 1 }}>
          <div style={panelHead}>
            등록된 멤버 {countChip(members.length, 'primary')}
            {canManage && members.length > 0 && (
              <label style={{ display: 'inline-flex', alignItems: 'center', gap: 4, marginLeft: 'auto', fontWeight: 400, color: 'var(--text-muted)' }}>
                <input type="checkbox" checked={allMemSel} onChange={toggleAllMem} /> 전체
              </label>
            )}
          </div>
          <div style={{ flex: 1, minHeight: 0, overflowY: 'auto' }}>
            {members.length === 0
              ? <div className="ts" style={{ padding: 14, fontSize: 12, textAlign: 'center', color: 'var(--text-muted)' }}>멤버 없음<br />우측에서 가입자를 선택해 <ArrowLeft size={11} style={{ verticalAlign: '-1px' }} /> 추가</div>
              : members.map(m => (
                <MemberRow key={m.user_id} m={m} name={pttName.get(m.user_id)} selected={selMembers.has(m.user_id)}
                  canManage={canManage} onToggle={toggleMem} onSave={onSaveMember} onRemove={uid => doRemove([uid])} />
              ))}
          </div>
        </div>

        {/* ── 중앙: 이동 버튼 ── */}
        {canManage && (
          <div style={{ display: 'flex', flexDirection: 'column', justifyContent: 'center', gap: 10, alignSelf: 'center' }}>
            <button className="btn btn--primary btn--sm" disabled={busy || picked.size === 0} title="선택 가입자 추가"
              onClick={() => doAdd(Array.from(picked))} style={{ display: 'inline-flex', alignItems: 'center', gap: 4, whiteSpace: 'nowrap' }}>
              <ArrowLeft size={14} /> 추가{picked.size ? ` ${picked.size}` : ''}
            </button>
            <button className="btn btn--outline btn--sm" disabled={busy || selMembers.size === 0} title="선택 멤버 제거"
              onClick={() => doRemove(Array.from(selMembers))} style={{ display: 'inline-flex', alignItems: 'center', gap: 4, whiteSpace: 'nowrap' }}>
              제거{selMembers.size ? ` ${selMembers.size}` : ''} <ArrowRight size={14} />
            </button>
          </div>
        )}

        {/* ── 우: 조직트리 + PTT 가입자 ── */}
        <div style={{ ...panel, flex: 1.3 }}>
          <div style={panelHead}>
            가입자 추가 {countChip(candidates.length)}
            <span style={{ marginLeft: 'auto', display: 'inline-flex', alignItems: 'center', gap: 8, fontWeight: 400, color: 'var(--text-muted)' }}>
              <label style={{ display: 'inline-flex', alignItems: 'center', gap: 3 }}>P
                <input className="form-input" type="number" value={bulkPri} title="추가 시 적용할 우선순위" onChange={e => setBulkPri(Number(e.target.value))} style={{ width: 46 }} /></label>
              <select className="form-input" value={bulkRole} title="추가 시 적용할 역할" onChange={e => setBulkRole(e.target.value as 'chair' | 'participant')} style={{ width: 116 }}>
                <option value="participant">participant</option>
                <option value="chair">chair (의장)</option>
              </select>
            </span>
          </div>
          <div style={{ display: 'flex', flex: 1, minHeight: 0 }}>
            {/* 조직 트리 */}
            <OrgTreePanel fill selectedPath={treeScope} onSelect={(pth, n) => { setTreeScope(pth); setTreeName(n) }}
              style={{ flex: '0 0 150px', width: 150, maxWidth: 150, border: 'none', borderRight: '1px solid var(--border)', borderRadius: 0 }} />
            {/* 후보 리스트 */}
            <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}>
              <div style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '6px 8px', borderBottom: '1px solid var(--border)' }}>
                <input className="search-input" autoFocus placeholder={`${treeName} 내 검색`} value={q} onChange={e => setQ(e.target.value)} style={{ flex: 1, fontSize: 12 }} />
              </div>
              <div style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '4px 8px', fontSize: 11, color: 'var(--text-muted)', borderBottom: '1px solid var(--border)' }}>
                <label style={{ display: 'inline-flex', alignItems: 'center', gap: 4 }}>
                  <input type="checkbox" checked={allCandPicked} onChange={toggleAllCand} disabled={candidates.length === 0} /> 전체 선택
                </label>
                <span style={{ marginLeft: 'auto', color: 'var(--primary)', fontWeight: 600 }}>선택 {picked.size}</span>
              </div>
              <div style={{ flex: 1, minHeight: 0, overflowY: 'auto' }}>
                {candidates.length === 0
                  ? <div className="ts" style={{ padding: 14, fontSize: 12, textAlign: 'center', color: 'var(--text-muted)' }}>{pttIndex.length ? `${treeName}에 추가할 가입자 없음` : '불러오는 중...'}</div>
                  : candidates.map(c => (
                    <div key={c.value} onDoubleClick={() => canManage && doAdd([c.value])}
                      style={{
                        display: 'flex', alignItems: 'center', gap: 8, padding: '6px 10px', fontSize: 12, cursor: 'pointer',
                        borderLeft: picked.has(c.value) ? '3px solid var(--primary)' : '3px solid transparent',
                        background: picked.has(c.value) ? 'rgba(74,144,217,0.06)' : undefined,
                      }}
                      onClick={() => canManage && toggleCand(c.value)}>
                      <input type="checkbox" checked={picked.has(c.value)} readOnly tabIndex={-1} />
                      <span style={{ display: 'flex', flexDirection: 'column', minWidth: 0, flex: 1 }}>
                        <span style={{ fontWeight: 600, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{c.userName}</span>
                        <span className="ts" style={{ color: 'var(--text-muted)', fontSize: 11 }}>{c.value}{c.orgCode ? ` · ${c.orgCode}` : ''}</span>
                      </span>
                    </div>
                  ))}
              </div>
            </div>
          </div>
        </div>
      </div>
      {canManage && <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 6 }}>가입자 더블클릭 = 바로 추가 · 체크 후 <ArrowLeft size={10} style={{ verticalAlign: '-1px' }} /> 추가 = 일괄 추가(우측 P·역할 적용)</div>}
    </div>
  )
}

// 가로 wrap 레이아웃용 컴팩트 필드. w 지정 없으면 flex-grow.
function Field({ label, children, w }: { label: string; children: React.ReactNode; w?: number | string }) {
  return (
    <label style={{ display: 'flex', flexDirection: 'column', gap: 2, width: w, flex: w ? undefined : '1 1 150px', minWidth: 110 }}>
      <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>{label}</span>
      {children}
    </label>
  )
}
function FieldRow({ children }: { children: React.ReactNode }) {
  return <div style={{ display: 'flex', flexWrap: 'wrap', gap: '8px 12px', alignItems: 'flex-end' }}>{children}</div>
}
