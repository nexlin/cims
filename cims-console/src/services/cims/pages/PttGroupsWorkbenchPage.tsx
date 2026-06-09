import { useState, useEffect, useCallback, useMemo } from 'react'
import { Pencil, Trash2 } from 'lucide-react'
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
//  좌: 조직트리(공유 스코프) | 그룹 DataTable | 행 확장: 속성 편집 + 멤버(가입자 피커).
//  그룹 = 사용자/번호를 가로지르는 N:M 집합 → 사용자 워크벤치와 분리된 독립 메뉴.

type GroupExt = Group

const ICON = 14

function IconBtn({ title, onClick, tone, children }: { title: string; onClick: () => void; tone?: 'primary' | 'danger' | 'default'; children: React.ReactNode }) {
  const cls = tone === 'danger' ? 'btn--danger' : tone === 'primary' ? 'btn--primary' : 'btn--ghost'
  return (
    <button title={title} aria-label={title} onClick={onClick}
      className={`btn btn--sm ${cls}`}
      style={{ padding: '3px 6px', display: 'inline-flex', alignItems: 'center', lineHeight: 0 }}>
      {children}
    </button>
  )
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

  // 행 확장(상세/편집) + 신규 추가 블록
  const [openId, setOpenId] = useState<string | null>(null)   // 펼친 그룹
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

  // 멤버 가입자 피커 인덱스 (1회 구성 → 공유)
  const pttIndex = useMemo(() => buildPickIndex(users, 'ptt'), [users])

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
          <div style={{ borderBottom: '1px solid var(--border)', background: 'var(--bg-soft)', padding: '10px 16px' }}>
            <GroupDrawer mode="add" orgs={orgs} me={me} canGroupCreate={canGroupCreate}
              pttIndex={pttIndex} orgScope={orgScope} orgPathOf={orgPathOf}
              onClose={() => setAdding(false)} onSaved={() => { setAdding(false); load() }} reload={load} />
          </div>
        )}

        <DataTable<GroupExt> columns={groupCols} rows={groupRows} rowKey={g => g.id} loading={loading}
          onRowClick={g => toggleOpen(g.id)}
          expandedKey={openId}
          renderExpanded={openGroup ? () => (
            <div style={{ padding: '10px 16px' }}>
              <GroupDrawer mode="view" group={openGroup} orgs={orgs} me={me} canGroupCreate={canGroupCreate}
                pttIndex={pttIndex} orgScope={orgScope} orgPathOf={orgPathOf}
                onClose={() => setOpenId(null)} onSaved={() => { setOpenId(null); load() }} reload={load} />
            </div>
          ) : undefined}
          pageSize={50} emptyText="그룹 없음" />
      </div>
    </div>
  )
}

// ── 그룹 드로어 (속성 편집 + 멤버 피커) ──
interface GroupDrawerProps {
  mode: 'view' | 'add'
  group?: GroupExt
  orgs: Organization[]
  me: ReturnType<typeof useAuth>['user']
  canGroupCreate: boolean
  pttIndex: PickItem[]
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
    ? { name: existing.name, priority: existing.priority ?? 5, encryption: existing.encryption, emergency_call: existing.emergency_call, video_enabled: existing.video_enabled, org_code: existing.org_code || '', session_start: existing.session_start || '', session_end: existing.session_end || '', authorized_user_id: existing.authorized_user_id ?? null, group_type: existing.group_type }
    : { id: '', name: '', priority: 5, encryption: false, emergency_call: false, video_enabled: false, org_code: '', group_type: 'prearranged', authorized_user_id: null })

  const [members, setMembers] = useState<Member[]>([])
  const existingId = existing?.id
  useEffect(() => {
    if (existingId) groupsApi.listMembers(existingId).then(setMembers).catch(() => setMembers([]))
  }, [existingId])

  async function save() {
    if (!form.name || (isNew && !form.id)) { show('ID/이름 필수', 'err'); return }
    const body = { ...form }
    if (body.session_start === '') body.session_start = null
    if (body.session_end === '') body.session_end = null
    try {
      if (existing) { await groupsApi.update(existing.id, body as Partial<GroupInput>); show('저장', 'ok'); setEditing(false); p.reload() }
      else { await groupsApi.create(body as GroupInput); show('저장', 'ok'); p.onSaved() }
    } catch (e: unknown) { show(String(e), 'err') }
  }
  async function addMember(it: PickItem) {
    if (!existing) return
    try { await groupsApi.addMember(existing.id, { user_id: it.value, priority: 5 }); show('멤버 추가', 'ok'); groupsApi.listMembers(existing.id).then(setMembers); p.reload() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function removeMember(uid: string) {
    if (!existing) return
    try { await groupsApi.removeMember(existing.id, uid); show('삭제', 'ok'); groupsApi.listMembers(existing.id).then(setMembers); p.reload() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  const memberIds = useMemo(() => new Set(members.map(m => m.user_id)), [members])

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10, fontSize: 13 }}>
      {editing ? (
        <FieldRow>
          {isNew && <Field label="그룹 ID *" w={130}><input className="form-input" autoFocus value={form.id || ''} onChange={e => setForm({ ...form, id: e.target.value })} /></Field>}
          <Field label="그룹명 *" w={150}><input className="form-input" value={form.name || ''} onChange={e => setForm({ ...form, name: e.target.value })} /></Field>
          <Field label="타입" w={120}>
            <select className="form-input" value={form.group_type || 'prearranged'} onChange={e => setForm({ ...form, group_type: e.target.value as GroupExt['group_type'] })}>
              <option value="prearranged">prearranged</option>
              <option value="chat">chat</option>
              <option value="broadcast">broadcast</option>
            </select>
          </Field>
          <Field label="우선순위" w={80}><input className="form-input" type="number" value={form.priority ?? 5} onChange={e => setForm({ ...form, priority: Number(e.target.value) })} /></Field>
          {allowOwner && <Field label="소유자 ID" w={110}><input className="form-input" type="number" placeholder={isNew && !hasRole(p.me, 'manager') ? '본인' : ''} value={form.authorized_user_id ?? ''} onChange={e => setForm({ ...form, authorized_user_id: e.target.value === '' ? null : Number(e.target.value) })} /></Field>}
          <Field label="조직 코드" w={150}>
            <select className="form-input" value={form.org_code || ''} onChange={e => setForm({ ...form, org_code: e.target.value })}>
              <option value="">없음</option>
              {p.orgs.map(o => <option key={o.id} value={o.code}>{o.name} ({o.code})</option>)}
            </select>
          </Field>
          <div style={{ display: 'flex', gap: 12, alignItems: 'center', alignSelf: 'center' }}>
            <label style={{ display: 'flex', alignItems: 'center', gap: 4 }}><input type="checkbox" checked={form.encryption || false} onChange={e => setForm({ ...form, encryption: e.target.checked })} />암호</label>
            <label style={{ display: 'flex', alignItems: 'center', gap: 4 }}><input type="checkbox" checked={form.emergency_call || false} onChange={e => setForm({ ...form, emergency_call: e.target.checked })} />긴급</label>
            <label style={{ display: 'flex', alignItems: 'center', gap: 4 }}><input type="checkbox" checked={form.video_enabled || false} onChange={e => setForm({ ...form, video_enabled: e.target.checked })} />영상</label>
          </div>
          <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
            <button className="btn btn--sm btn--primary" onClick={save}>저장</button>
            <button className="btn btn--sm btn--ghost" onClick={() => isNew ? p.onClose() : setEditing(false)}>취소</button>
          </div>
        </FieldRow>
      ) : canManage && existing && (
        <div style={{ display: 'flex', justifyContent: 'flex-end' }}>
          <button className="btn btn--sm btn--outline" onClick={() => setEditing(true)}>그룹 속성 편집</button>
        </div>
      )}

      {/* 멤버 — 가입자 피커 */}
      {existing && <div style={{ marginTop: 8, borderTop: '1px solid var(--border)', paddingTop: 8 }}>
        <div style={{ fontWeight: 600, fontSize: 12, color: 'var(--text-muted)', marginBottom: 6 }}>멤버 ({members.length})</div>
        {members.map(m => (
          <div key={m.user_id} style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '3px 0', fontSize: 12 }}>
            <span className="ts">{m.user_id} <span style={{ color: 'var(--text-muted)' }}>(P:{m.priority}{m.role === 'chair' ? ', chair' : ''})</span></span>
            {canManage && <button className="btn btn--sm btn--danger" style={{ padding: '0 6px', fontSize: 10 }} onClick={() => removeMember(m.user_id)}>×</button>}
          </div>
        ))}
        {canManage && (
          <div style={{ marginTop: 6 }}>
            <SubscriberPicker kind="ptt" index={p.pttIndex} orgScope={p.orgScope} orgPathOf={p.orgPathOf}
              exclude={memberIds} onPick={addMember} placeholder="멤버 가입자 검색·선택" />
          </div>
        )}
      </div>}
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
