import { useState, useEffect, useCallback, useMemo } from 'react'
import { Pencil, Trash2, Check, X } from 'lucide-react'
import { usersApi, type UserSummary, type Subscription, type UserInput } from '../../../api/users'
import { orgApi, type Organization } from '../../../api/organizations'
import { groupsApi, type Group, type Member } from '../../../api/groups'
import OrgTreePanel from '../../../components/OrgTreePanel'
import { DataTable, type Column } from '../../../components/DataTable'
import SubscriberPicker, { buildPickIndex, type PickItem } from '../../../components/SubscriberPicker'
import { useToast } from '../../../components/Toast'
import { useAuth } from '../../../contexts/AuthContext'
import {
  canWriteConfig, canAssignRole, canCreateGroup, canManageGroup, hasRole,
  ROLE_LABELS, ASSIGNABLE_ROLES,
} from '../../../utils/permissions'

// ── 통합 프로비저닝 워크벤치 ──────────────────────────────────
//  좌: 조직트리(공유 스코프) | 상단 탭: 사용자/번호/PTT그룹 | 우: 상세·편집 드로어.
//  조직 1회 선택 → 세 탭 모두 그 스코프로 필터. 페이지 횡단 이동 제거.

type Tab = 'users' | 'volte' | 'ptt' | 'groups'

interface GroupExt extends Group { priority?: number; encryption?: boolean; emergency_call?: boolean; video_enabled?: boolean; org_code?: string; session_start?: string | null; session_end?: string | null }

// 번호 탭의 평탄화 행
interface NumberRow { msisdn: string; svc: 'call' | 'ptt'; user: UserSummary; sub: Subscription }

// 우측 드로어 상태
type Drawer =
  | { kind: 'user'; mode: 'view' | 'edit' | 'add'; id?: number; rowKey?: string | number }
  | { kind: 'group'; mode: 'view' | 'edit' | 'add'; id?: string; rowKey?: string | number }
  | null

// 작은 아이콘 액션 버튼 (편집/삭제/저장/취소 공용)
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
const ICON = 14

// 조직 code → 전체 경로 (예: "CIMS > 제1본부 > 팀01")
function buildOrgPath(orgs: Organization[], code?: string): string {
  if (!code) return '—'
  const byId = new Map(orgs.map(o => [o.id, o]))
  const byCode = new Map(orgs.map(o => [o.code, o]))
  const names: string[] = []
  let cur = byCode.get(code)
  let guard = 0
  while (cur && guard++ < 30) {
    names.unshift(cur.name)
    cur = cur.parent_id != null ? byId.get(cur.parent_id) : undefined
  }
  return names.length ? names.join(' > ') : (code || '—')
}

// 조직 트리 선택용 들여쓰기 옵션 목록
function orgIndentedOptions(orgs: Organization[]): Array<{ code: string; label: string }> {
  const byParent = new Map<number | null, Organization[]>()
  for (const o of orgs) {
    const k = o.parent_id ?? null
    if (!byParent.has(k)) byParent.set(k, [])
    byParent.get(k)!.push(o)
  }
  for (const arr of byParent.values()) arr.sort((a, b) => a.sort_order - b.sort_order || a.name.localeCompare(b.name))
  const out: Array<{ code: string; label: string }> = []
  const walk = (parent: number | null, depth: number) => {
    for (const o of byParent.get(parent) || []) {
      out.push({ code: o.code, label: '　'.repeat(depth) + o.name })
      walk(o.id, depth + 1)
    }
  }
  walk(null, 0)
  return out
}

export default function ProvisioningWorkbenchPage() {
  const { show } = useToast()
  const { user: me } = useAuth()
  const canWrite = canWriteConfig(me)
  const canRole = canAssignRole(me)
  const canGroupCreate = canCreateGroup(me)

  const [tab, setTab] = useState<Tab>('users')
  const [orgScope, setOrgScope] = useState<string | null>(null)
  const [orgName, setOrgName] = useState('전체')
  const [search, setSearch] = useState('')

  const [users, setUsers] = useState<UserSummary[]>([])
  const [orgs, setOrgs] = useState<Organization[]>([])
  const [groups, setGroups] = useState<GroupExt[]>([])
  const [loading, setLoading] = useState(true)

  const [selected, setSelected] = useState<Set<string | number>>(new Set())
  const [drawer, setDrawer] = useState<Drawer>(null)
  const [importOpen, setImportOpen] = useState(false)

  // 사용자 인라인 셀 편집
  const [editUserId, setEditUserId] = useState<number | null>(null)
  const [editUser, setEditUser] = useState<UserInput>({ name: '', login_id: '', org_id: '', details: '', role: 'user' })
  const orgOpts = useMemo(() => orgIndentedOptions(orgs), [orgs])
  function startEditUser(u: UserSummary) {
    setEditUserId(u.id)
    setEditUser({ name: u.name, login_id: u.login_id, org_id: u.org_id, details: u.details || '', role: u.role || 'user' })
  }
  async function saveEditUser() {
    if (!editUserId) return
    if (!editUser.name) { show('이름 필수', 'err'); return }
    try { await usersApi.update(editUserId, editUser); show('저장', 'ok'); setEditUserId(null); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  // 사용자 인라인 추가
  const [addingUser, setAddingUser] = useState(false)
  const [addUserForm, setAddUserForm] = useState<UserInput>({ name: '', login_id: '', org_id: '', details: '', role: 'user', password: '123456' })
  function startAddUser() {
    setEditUserId(null); setAddingUser(true)
    setAddUserForm({ name: '', login_id: '', org_id: orgScope ? (orgScope.split('/').pop() || '') : '', details: '', role: 'user', password: '123456' })
  }
  async function saveAddUser() {
    if (!addUserForm.name) { show('이름 필수', 'err'); return }
    try { await usersApi.create(addUserForm); show('생성', 'ok'); setAddingUser(false); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  // 행 클릭 토글 — 같은 행 재클릭/다른 행 클릭 시 닫힘
  function toggleDrawer(next: NonNullable<Drawer>) {
    setDrawer(cur => (cur && cur.kind === next.kind && (cur.rowKey ?? cur.id) === (next.rowKey ?? next.id)) ? null : next)
  }

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [u, o, g] = await Promise.all([usersApi.list(), orgApi.list(), groupsApi.list()])
      setUsers(u); setOrgs(o); setGroups(g as GroupExt[])
    } catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [show])
  useEffect(() => { load() }, [load])

  // 조직 code → code_path (스코프 startsWith 비교용)
  const orgPathOf = useCallback((code: string) => orgs.find(o => o.code === code)?.code_path || code, [orgs])
  const inScope = useCallback((orgCode: string) => {
    if (!orgScope) return true
    return (orgPathOf(orgCode) || '').startsWith(orgScope)
  }, [orgScope, orgPathOf])

  // 가입자 피커 인덱스 (워크벤치 1회 구성 → 공유)
  const pttIndex = useMemo(() => buildPickIndex(users, 'ptt'), [users])
  const userIndex = useMemo(() => buildPickIndex(users, 'user'), [users])

  // 탭 전환·검색 시 선택 초기화
  useEffect(() => { setSelected(new Set()) }, [tab])

  function clearSearch() { setSearch('') }

  // ── 사용자 탭 데이터 ──
  const userRows = useMemo(() => {
    const s = search.trim().toLowerCase()
    return users.filter(u => inScope(u.org_id || '') &&
      (!s || u.name.toLowerCase().includes(s) || (u.login_id || '').toLowerCase().includes(s)))
  }, [users, inScope, search])

  // ── 번호 탭 데이터 (VoLTE / PTT 분리) ──
  const buildNumberRows = useCallback((svc: 'call' | 'ptt'): NumberRow[] => {
    const s = search.trim().toLowerCase()
    const out: NumberRow[] = []
    for (const u of users) {
      if (!inScope(u.org_id || '')) continue
      const subs = svc === 'call' ? u.call_subscriptions : u.ptt_subscriptions
      for (const sub of subs) out.push({ msisdn: sub.id, svc, user: u, sub })
    }
    return out.filter(r => !s || r.msisdn.toLowerCase().includes(s) || r.user.name.toLowerCase().includes(s))
  }, [users, inScope, search])
  const volteRows = useMemo(() => buildNumberRows('call'), [buildNumberRows])
  const pttRows = useMemo(() => buildNumberRows('ptt'), [buildNumberRows])

  // ── 그룹 탭 데이터 ──
  const groupRows = useMemo(() => {
    const s = search.trim().toLowerCase()
    return groups.filter(g => (!orgScope || inScope(g.org_code || '')) &&
      (!s || g.id.toLowerCase().includes(s) || g.name.toLowerCase().includes(s)))
  }, [groups, orgScope, inScope, search])

  // ── 삭제 ──
  async function batchDeleteUsers() {
    const ids = Array.from(selected).map(Number)
    if (!ids.length || !confirm(`${ids.length}명을 삭제합니다. 연결된 번호도 삭제됩니다.`)) return
    try { await usersApi.batchDelete(ids); show('삭제 완료', 'ok'); setSelected(new Set()); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function deleteNumber(r: NumberRow) {
    if (!confirm(`${r.msisdn} 삭제?`)) return
    try { await usersApi.deleteSub(r.user.id, r.svc, r.msisdn); show('삭제', 'ok'); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function deleteGroup(id: string) {
    if (!confirm(`그룹 ${id} 삭제?`)) return
    try { await groupsApi.delete(id); show('삭제', 'ok'); load(); if (drawer?.kind === 'group' && drawer.id === id) setDrawer(null) }
    catch (e: unknown) { show(String(e), 'err') }
  }

  // ── 컬럼 정의 ──
  const stop = (e: React.MouseEvent) => e.stopPropagation()
  const ie = (u: UserSummary) => editUserId === u.id   // inline editing this row?
  const userCols: Column<UserSummary>[] = [
    { key: 'name', header: '이름', sortable: true, width: 120, render: u => ie(u)
      ? <input className="form-input" autoFocus value={editUser.name} onClick={stop} onChange={e => setEditUser({ ...editUser, name: e.target.value })} />
      : <span style={{ fontWeight: 500 }}>{u.name}</span> },
    { key: 'login_id', header: '로그인ID / 암호', sortable: true, width: 210, render: u => ie(u)
      ? <span style={{ display: 'flex', gap: 4 }} onClick={stop}>
          <input className="form-input" style={{ flex: 1, minWidth: 0 }} placeholder="로그인ID" value={editUser.login_id || ''} onChange={e => setEditUser({ ...editUser, login_id: e.target.value })} />
          <input className="form-input" style={{ width: 84 }} type="text" placeholder="새 암호" value={editUser.password || ''} onChange={e => setEditUser({ ...editUser, password: e.target.value })} />
        </span>
      : <span className="ts">{u.login_id || '—'}</span> },
    { key: 'role', header: '권한', width: 110, render: u => ie(u)
      ? (canRole
          ? <select className="form-input" onClick={stop} value={editUser.role || 'user'} onChange={e => setEditUser({ ...editUser, role: e.target.value as UserInput['role'] })}>
              {ASSIGNABLE_ROLES.map(r => <option key={r} value={r}>{ROLE_LABELS[r]}</option>)}
            </select>
          : <span className="badge">{ROLE_LABELS[editUser.role || 'user']}</span>)
      : <span className="badge">{ROLE_LABELS[u.role || 'user']}</span> },
    { key: 'org', header: '조직', width: 220, sortValue: u => buildOrgPath(orgs, u.org_id), render: u => ie(u)
      ? <select className="form-input" onClick={stop} value={editUser.org_id} onChange={e => setEditUser({ ...editUser, org_id: e.target.value })}>
          <option value="">없음</option>
          {orgOpts.map(o => <option key={o.code} value={o.code}>{o.label}</option>)}
        </select>
      : <span className="ts" title={buildOrgPath(orgs, u.org_id)}>{buildOrgPath(orgs, u.org_id)}</span> },
    { key: 'details', header: '설명', render: u => ie(u)
      ? <input className="form-input" onClick={stop} value={editUser.details || ''} onChange={e => setEditUser({ ...editUser, details: e.target.value })} />
      : <span className="ts">{u.details || '—'}</span> },
    { key: 'nums', header: '번호', width: 220, render: u => {
      const all = [
        ...u.call_subscriptions.map(s => ({ svc: 'call' as const, id: s.id })),
        ...u.ptt_subscriptions.map(s => ({ svc: 'ptt' as const, id: s.id })),
      ]
      if (all.length === 0) return <span className="ts">—</span>
      return <span style={{ display: 'flex', flexWrap: 'wrap', gap: 3 }}>
        {all.map(n => <span key={`${n.svc}:${n.id}`} className={`badge ${n.svc === 'call' ? 'badge--blue' : 'badge--green'}`} style={{ fontSize: 10 }} title={n.svc === 'call' ? 'VoLTE' : 'McPTT'}>{n.id}</span>)}
      </span>
    } },
    { key: 'act', header: '', width: 84, align: 'right', render: u => ie(u)
      ? <span className="actions" onClick={stop}>
          <IconBtn title="저장" tone="primary" onClick={saveEditUser}><Check size={ICON} /></IconBtn>
          <IconBtn title="취소" onClick={() => setEditUserId(null)}><X size={ICON} /></IconBtn>
        </span>
      : canWrite ? (
        <span className="actions" onClick={stop}>
          <IconBtn title="편집" onClick={() => startEditUser(u)}><Pencil size={ICON} /></IconBtn>
          <IconBtn title="삭제" tone="danger" onClick={() => { if (confirm(`${u.name} 삭제?`)) usersApi.delete(u.id).then(() => { show('삭제', 'ok'); load() }).catch(e => show(String(e), 'err')) }}><Trash2 size={ICON} /></IconBtn>
        </span>
      ) : <span className="ts">—</span> },
  ]

  const numberActCol: Column<NumberRow> = { key: 'act', header: '', width: 120, align: 'right', render: r => canWrite ? (
    <span className="actions" onClick={e => e.stopPropagation()}>
      <button className="btn btn--sm btn--outline" onClick={() => setDrawer({ kind: 'user', mode: 'edit', id: r.user.id })}>가입자</button>
      <button className="btn btn--sm btn--danger" onClick={() => deleteNumber(r)}>삭제</button>
    </span>
  ) : <span className="ts">—</span> }
  const numberBaseCols: Column<NumberRow>[] = [
    { key: 'msisdn', header: 'MSISDN', sortable: true, render: r => <span style={{ fontWeight: 600 }}>{r.msisdn}</span> },
    { key: 'imsi', header: 'IMSI', width: 150, sortable: true, sortValue: r => r.sub.imsi || '', render: r => <span className="ts">{r.sub.imsi || '—'}</span> },
    { key: 'svc_ref', header: '서비스', width: 90, render: r => <span className="ts">{r.sub.service_ref || '—'}</span> },
    { key: 'user', header: '가입자', sortable: true, sortValue: r => r.user.name, render: r => r.user.name },
    { key: 'org', header: '조직', width: 130, render: r => <span className="ts">{orgs.find(o => o.code === r.user.org_id)?.name || r.user.org_id || '—'}</span> },
  ]
  const volteCols: Column<NumberRow>[] = [
    ...numberBaseCols,
    { key: 'dnd', header: 'DND', width: 70, align: 'center', render: r => <span className={`badge ${r.sub.dnd ? 'badge--red' : 'badge--gray'}`} style={{ fontSize: 10 }}>{r.sub.dnd ? 'ON' : 'OFF'}</span> },
    { key: 'fwd', header: '착신전환', width: 120, render: r => <span className="ts">{r.sub.forward_id || '—'}</span> },
    numberActCol,
  ]
  const pttCols: Column<NumberRow>[] = [...numberBaseCols, numberActCol]

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
    { key: 'members', header: '멤버', width: 64, align: 'center', render: g => <span className="ts">{g.members?.length ?? 0}명</span> },
    { key: 'act', header: '', width: 84, align: 'right', render: g => canManageGroup(me, g.authorized_user_id) ? (
      <span className="actions" onClick={e => e.stopPropagation()}>
        <IconBtn title="편집" onClick={() => setDrawer({ kind: 'group', mode: 'edit', id: g.id, rowKey: g.id })}><Pencil size={ICON} /></IconBtn>
        <IconBtn title="삭제" tone="danger" onClick={() => deleteGroup(g.id)}><Trash2 size={ICON} /></IconBtn>
      </span>
    ) : <span className="ts">—</span> },
  ]

  const TABS: Array<{ k: Tab; label: string; count: number }> = [
    { k: 'users', label: '사용자', count: userRows.length },
    { k: 'volte', label: 'VoLTE 번호', count: volteRows.length },
    { k: 'ptt', label: 'PTT 번호', count: pttRows.length },
    { k: 'groups', label: 'PTT 그룹', count: groupRows.length },
  ]

  // 사용자 인라인 추가 행 (DataTable footer 로 렌더, 체크박스 컬럼 포함 8셀)
  const userAddRow = addingUser ? (
    <tr style={{ background: 'rgba(74,144,217,0.06)' }}>
      <td></td>
      <td><input className="form-input" placeholder="이름 *" autoFocus value={addUserForm.name} onChange={e => setAddUserForm({ ...addUserForm, name: e.target.value })} /></td>
      <td><span style={{ display: 'flex', gap: 4 }}>
        <input className="form-input" style={{ flex: 1, minWidth: 0 }} placeholder="로그인ID" value={addUserForm.login_id || ''} onChange={e => setAddUserForm({ ...addUserForm, login_id: e.target.value })} />
        <input className="form-input" style={{ width: 84 }} type="text" placeholder="암호" value={addUserForm.password || ''} onChange={e => setAddUserForm({ ...addUserForm, password: e.target.value })} />
      </span></td>
      <td>{canRole
        ? <select className="form-input" value={addUserForm.role || 'user'} onChange={e => setAddUserForm({ ...addUserForm, role: e.target.value as UserInput['role'] })}>
            {ASSIGNABLE_ROLES.map(r => <option key={r} value={r}>{ROLE_LABELS[r]}</option>)}
          </select>
        : <span className="badge">{ROLE_LABELS['user']}</span>}</td>
      <td><select className="form-input" value={addUserForm.org_id} onChange={e => setAddUserForm({ ...addUserForm, org_id: e.target.value })}>
        <option value="">없음</option>
        {orgOpts.map(o => <option key={o.code} value={o.code}>{o.label}</option>)}
      </select></td>
      <td><input className="form-input" placeholder="설명" value={addUserForm.details || ''} onChange={e => setAddUserForm({ ...addUserForm, details: e.target.value })} /></td>
      <td></td>
      <td className="actions">
        <button className="btn btn--sm btn--primary" onClick={saveAddUser}>저장</button>
        <button className="btn btn--sm btn--ghost" onClick={() => setAddingUser(false)}>취소</button>
      </td>
    </tr>
  ) : undefined

  // 상세/편집 블록 공용 props (drawer 가 있을 때만 — 행 확장/추가 블록에서 사용)
  const detailProps: DrawerProps | null = drawer ? {
    drawer, onClose: () => setDrawer(null), onSaved: () => { setDrawer(null); load() }, reload: load,
    users, orgs, groups, me, canWrite, canRole, canGroupCreate,
    pttIndex, userIndex, orgPathOf, orgScope,
  } : null

  return (
    <div style={{ display: 'flex', gap: 16, alignItems: 'stretch', height: 'calc(100vh - 92px)' }}>
      {/* 좌: 조직 트리 (full-height 패널, 공유 스코프) */}
      <OrgTreePanel fill selectedPath={orgScope} onSelect={(p, n) => { setOrgScope(p); setOrgName(n) }}
        style={{ flex: '0 0 200px', width: 200, maxWidth: 200 }} />

      {/* 중: 패널 = 탭 헤더 + 툴바 + 테이블 */}
      <div className="panel" style={{ flex: 1, minWidth: 0 }}>
        {/* 탭 헤더 */}
        <div className="panel-header" style={{ display: 'flex', gap: 2, padding: '0 8px', alignItems: 'stretch' }}>
          {TABS.map(t => (
            <button key={t.k} onClick={() => setTab(t.k)}
              style={{
                padding: '12px 14px', border: 'none', background: 'none', cursor: 'pointer', fontSize: 13,
                fontWeight: tab === t.k ? 700 : 500, color: tab === t.k ? 'var(--primary)' : 'var(--text-muted)',
                borderBottom: tab === t.k ? '2px solid var(--primary)' : '2px solid transparent', marginBottom: -1,
              }}>
              {t.label} <span className="badge badge--gray" style={{ fontSize: 10, marginLeft: 2 }}>{t.count}</span>
            </button>
          ))}
        </div>

        {/* 툴바 */}
        <div className="toolbar">
          <span style={{ fontWeight: 600, fontSize: 13 }}>{orgName}</span>
          <input className="search-input" placeholder="이름·번호·ID 검색" value={search}
            onChange={e => setSearch(e.target.value)} style={{ maxWidth: 220 }} />
          {search && <button className="btn btn--ghost btn--sm" onClick={clearSearch}>✕</button>}
          <span style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
            {tab === 'users' && canWrite && <>
              <button className="btn btn--outline btn--sm" onClick={() => setImportOpen(true)}>Excel 가져오기</button>
              {selected.size > 0 && <button className="btn btn--danger btn--sm" onClick={batchDeleteUsers}>선택 삭제 ({selected.size})</button>}
              <button className="btn btn--primary btn--sm" onClick={startAddUser}>＋ 사용자</button>
            </>}
            {(tab === 'volte' || tab === 'ptt') && canWrite && (
              <button className="btn btn--primary btn--sm" onClick={() => setDrawer({ kind: 'user', mode: 'add' })}>＋ 사용자(번호)</button>
            )}
            {tab === 'groups' && canGroupCreate && (
              <button className="btn btn--primary btn--sm" onClick={() => setDrawer({ kind: 'group', mode: 'add' })}>＋ 그룹</button>
            )}
          </span>
        </div>

        {/* 추가(add)는 펼칠 행이 없으므로 테이블 위 블록으로 */}
        {detailProps && detailProps.drawer.mode === 'add' && (
          <div style={{ borderBottom: '1px solid var(--border)', background: 'var(--bg-soft)' }}>
            <DetailBlock {...detailProps} />
          </div>
        )}

        {/* 테이블 (패널 잔여 높이 채움 + 자체 스크롤). 선택 행 바로 아래 상세/편집 인라인 확장 */}
        {tab === 'users' && (
          <DataTable<UserSummary> columns={userCols} rows={userRows} rowKey={u => u.id} loading={loading}
            selectable={canWrite} selected={selected} onSelectChange={setSelected}
            onRowClick={u => { if (editUserId !== u.id) toggleDrawer({ kind: 'user', mode: 'view', id: u.id, rowKey: u.id }) }}
            expandedKey={drawer?.kind === 'user' && drawer.mode !== 'add' ? (drawer.rowKey ?? drawer.id ?? null) : null}
            renderExpanded={detailProps ? () => <DetailBlock {...detailProps} /> : undefined}
            footer={userAddRow}
            pageSize={50} emptyText="사용자 없음" />
        )}
        {tab === 'volte' && (
          <DataTable<NumberRow> columns={volteCols} rows={volteRows} rowKey={r => r.msisdn} loading={loading}
            onRowClick={r => toggleDrawer({ kind: 'user', mode: 'view', id: r.user.id, rowKey: r.msisdn })}
            expandedKey={drawer?.kind === 'user' && drawer.mode !== 'add' ? (drawer.rowKey ?? null) : null}
            renderExpanded={detailProps ? () => <DetailBlock {...detailProps} /> : undefined}
            pageSize={50} emptyText="VoLTE 번호 없음" />
        )}
        {tab === 'ptt' && (
          <DataTable<NumberRow> columns={pttCols} rows={pttRows} rowKey={r => r.msisdn} loading={loading}
            onRowClick={r => toggleDrawer({ kind: 'user', mode: 'view', id: r.user.id, rowKey: r.msisdn })}
            expandedKey={drawer?.kind === 'user' && drawer.mode !== 'add' ? (drawer.rowKey ?? null) : null}
            renderExpanded={detailProps ? () => <DetailBlock {...detailProps} /> : undefined}
            pageSize={50} emptyText="PTT 번호 없음" />
        )}
        {tab === 'groups' && (
          <DataTable<GroupExt> columns={groupCols} rows={groupRows} rowKey={g => g.id} loading={loading}
            onRowClick={g => toggleDrawer({ kind: 'group', mode: 'view', id: g.id, rowKey: g.id })}
            expandedKey={drawer?.kind === 'group' && drawer.mode !== 'add' ? (drawer.rowKey ?? drawer.id ?? null) : null}
            renderExpanded={detailProps ? () => <DetailBlock {...detailProps} /> : undefined}
            pageSize={50} emptyText="그룹 없음" />
        )}
      </div>

      {/* Excel import (사용자+번호 통합) */}
      {importOpen && <ImportModal onClose={() => setImportOpen(false)} onDone={load} />}
    </div>
  )
}

// ════════════════════════════════════════════════════════════
//  우측 드로어 — 사용자 / 그룹 편집·추가
// ════════════════════════════════════════════════════════════
interface DrawerProps {
  drawer: NonNullable<Drawer>
  onClose: () => void
  onSaved: () => void
  reload: () => void
  users: UserSummary[]
  orgs: Organization[]
  groups: GroupExt[]
  me: ReturnType<typeof useAuth>['user']
  canWrite: boolean
  canRole: boolean
  canGroupCreate: boolean
  pttIndex: PickItem[]
  userIndex: PickItem[]
  orgPathOf: (code: string) => string
  orgScope: string | null
}

// 선택 행 바로 아래(또는 추가 시 테이블 위)에 인라인 렌더되는 상세/편집 블록 (헤더·닫기 없음 — 행 재클릭으로 토글)
function DetailBlock(p: DrawerProps) {
  return (
    <div style={{ padding: '10px 16px' }}>
      {p.drawer.kind === 'user'
        ? <UserDrawer key={`u${p.drawer.id ?? 'new'}`} {...p} />
        : <GroupDrawer key={`g${p.drawer.id ?? 'new'}`} {...p} />}
    </div>
  )
}

// 서비스 카탈로그 항목 — ref(access_services 이름) + 그 서비스의 svc(call=VoLTE / ptt=PTT)
interface ServiceCat { svc: 'call' | 'ptt'; ref: string }
function buildServiceCatalog(users: UserSummary[]): ServiceCat[] {
  const seen = new Set<string>()
  const cat: ServiceCat[] = []
  const add = (svc: 'call' | 'ptt', ref: string) => { const k = `${svc}:${ref}`; if (ref && !seen.has(k)) { seen.add(k); cat.push({ svc, ref }) } }
  for (const u of users) {
    for (const s of u.call_subscriptions) if (s.service_ref) add('call', s.service_ref)
    for (const s of u.ptt_subscriptions) if (s.service_ref) add('ptt', s.service_ref)
  }
  if (!cat.some(c => c.svc === 'call')) add('call', 'volte')
  if (!cat.some(c => c.svc === 'ptt')) add('ptt', 'mcptt')
  return cat
}

// ── 사용자 확장 영역 = 단일 번호 테이블 (서비스 선택이 VoLTE/PTT 유형을 결정) ──
function UserDrawer(p: DrawerProps) {
  const existing = p.drawer.kind === 'user' && p.drawer.id != null
    ? p.users.find(u => u.id === p.drawer.id) : undefined
  const catalog = useMemo(() => buildServiceCatalog(p.users), [p.users])
  if (!existing) return null
  return <NumbersTable user={existing} catalog={catalog} onReload={p.reload} />
}

// VoLTE/PTT 유형 배지 (모듈 레벨 — 렌더 중 컴포넌트 생성 회피)
function SvcBadge({ svc }: { svc: 'call' | 'ptt' }) {
  return <span className={`badge ${svc === 'call' ? 'badge--blue' : 'badge--green'}`} style={{ fontSize: 9 }}>{svc === 'call' ? 'VoLTE' : 'McPTT'}</span>
}

interface AddNum { id: string; imsi: string; svcCat: string; passwd: string; dnd: boolean; forward_id: string }

// ── 단일 번호 테이블 (VoLTE+PTT 통합) ──
//  유형은 '서비스' 선택으로 결정(서비스 ref → svc=call/ptt 매핑). 백엔드는 volte/ptt 테이블 분리.
//  SIM 정보 = IMSI(인증 username, 필수) + 서비스(도메인) + 비밀번호, VoLTE 는 DND·착신전환.
function NumbersTable({ user, catalog, onReload }: { user: UserSummary; catalog: ServiceCat[]; onReload: () => void }) {
  const { show } = useToast()
  const rows: Array<{ svc: 'call' | 'ptt'; sub: Subscription }> = [
    ...user.call_subscriptions.map(s => ({ svc: 'call' as const, sub: s })),
    ...user.ptt_subscriptions.map(s => ({ svc: 'ptt' as const, sub: s })),
  ]
  const svcVal = (c: ServiceCat) => `${c.svc}:${c.ref}`
  const rk = (svc: 'call' | 'ptt', msisdn: string) => `${svc}:${msisdn}`
  const newAdd = (): AddNum => ({ id: '', imsi: '', svcCat: catalog[0] ? svcVal(catalog[0]) : 'call:volte', passwd: '123456', dnd: false, forward_id: '' })

  const [editKey, setEditKey] = useState<string | null>(null)
  const [editForm, setEditForm] = useState<Partial<Subscription>>({})
  const [adding, setAdding] = useState(false)
  const [addForm, setAddForm] = useState<AddNum>(newAdd())

  function startEdit(r: { svc: 'call' | 'ptt'; sub: Subscription }) {
    setAdding(false); setEditKey(rk(r.svc, r.sub.id))
    setEditForm({ imsi: r.sub.imsi || '', service_ref: r.sub.service_ref || '', passwd: '', dnd: r.sub.dnd, forward_id: r.sub.forward_id })
  }
  async function saveEdit(r: { svc: 'call' | 'ptt'; sub: Subscription }) {
    const d: Partial<Subscription> = { ...editForm }; if (!d.passwd) delete d.passwd
    try { await usersApi.updateSub(user.id, r.svc, r.sub.id, d); show('수정', 'ok'); setEditKey(null); onReload() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function del(r: { svc: 'call' | 'ptt'; sub: Subscription }) {
    if (!confirm(`${r.sub.id} 삭제?`)) return
    try { await usersApi.deleteSub(user.id, r.svc, r.sub.id); show('삭제', 'ok'); onReload() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function add() {
    if (!addForm.id) { show('MSISDN 필수', 'err'); return }
    if (!addForm.imsi) { show('IMSI 필수', 'err'); return }
    const [svc, ref] = addForm.svcCat.split(':') as ['call' | 'ptt', string]
    const body: Partial<Subscription> = { id: addForm.id, imsi: addForm.imsi, service_ref: ref, passwd: addForm.passwd, dnd: addForm.dnd, forward_id: addForm.forward_id }
    try { await usersApi.addSub(user.id, svc, body); show('추가', 'ok'); setAdding(false); setAddForm(newAdd()); onReload() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  const addSvc = addForm.svcCat.split(':')[0] as 'call' | 'ptt'
  const addIsCall = addSvc === 'call'

  return (
    <div style={{ marginTop: 4 }}>
      <div className="table-wrap">
      <table className="data-table" style={{ fontSize: 12 }}>
        <thead>
          <tr>
            <th style={{ width: 150 }}>서비스</th>
            <th style={{ width: 140 }}>MSISDN</th>
            <th style={{ width: 100 }}>비밀번호</th>
            <th>IMSI</th>
            <th style={{ width: 56, textAlign: 'center' }}>DND</th>
            <th style={{ width: 110 }}>착신전환</th>
            <th style={{ width: 130 }}></th>
          </tr>
        </thead>
        <tbody>
          {rows.length === 0 && !adding && <tr><td colSpan={7} className="empty-cell" style={{ padding: 12 }}>번호 없음</td></tr>}
          {rows.map(r => {
            const ed = editKey === rk(r.svc, r.sub.id)
            const isCall = r.svc === 'call'
            return (
              <tr key={rk(r.svc, r.sub.id)}>
                <td>{ed
                  ? <select className="form-input" value={editForm.service_ref || ''} onChange={e => setEditForm({ ...editForm, service_ref: e.target.value })}>
                      {catalog.filter(c => c.svc === r.svc).map(c => <option key={c.ref} value={c.ref}>{c.ref}</option>)}
                    </select>
                  : <SvcBadge svc={r.svc} />}</td>
                <td><strong>{r.sub.id}</strong></td>
                <td>{ed ? <input className="form-input" placeholder="변경 시 입력" value={editForm.passwd || ''} onChange={e => setEditForm({ ...editForm, passwd: e.target.value })} /> : <span className="ts">••••</span>}</td>
                <td>{ed ? <input className="form-input" placeholder="SIM IMSI" value={editForm.imsi || ''} onChange={e => setEditForm({ ...editForm, imsi: e.target.value })} /> : <span className="ts">{r.sub.imsi || '—'}</span>}</td>
                <td style={{ textAlign: 'center' }}>{!isCall ? <span className="ts">—</span> : ed ? <input type="checkbox" checked={editForm.dnd || false} onChange={e => setEditForm({ ...editForm, dnd: e.target.checked })} /> : (r.sub.dnd ? <span className="badge badge--red" style={{ fontSize: 9 }}>ON</span> : <span className="ts">—</span>)}</td>
                <td>{!isCall ? <span className="ts">—</span> : ed ? <input className="form-input" placeholder="대상" value={editForm.forward_id || ''} onChange={e => setEditForm({ ...editForm, forward_id: e.target.value })} /> : <span className="ts">{r.sub.forward_id || '—'}</span>}</td>
                <td className="actions">
                  {ed ? <>
                    <IconBtn title="저장" tone="primary" onClick={() => saveEdit(r)}><Check size={ICON} /></IconBtn>
                    <IconBtn title="취소" onClick={() => setEditKey(null)}><X size={ICON} /></IconBtn>
                  </> : <>
                    <IconBtn title="편집" onClick={() => startEdit(r)}><Pencil size={ICON} /></IconBtn>
                    <IconBtn title="삭제" tone="danger" onClick={() => del(r)}><Trash2 size={ICON} /></IconBtn>
                  </>}
                </td>
              </tr>
            )
          })}
          {adding && (
            <tr style={{ background: 'rgba(74,144,217,0.06)' }}>
              <td><select className="form-input" value={addForm.svcCat} onChange={e => setAddForm({ ...addForm, svcCat: e.target.value })}>
                {catalog.map(c => <option key={svcVal(c)} value={svcVal(c)}>{c.ref} ({c.svc === 'call' ? 'VoLTE' : 'McPTT'})</option>)}
              </select></td>
              <td><input className="form-input" placeholder={addIsCall ? '+8213…' : '+825…'} autoFocus value={addForm.id} onChange={e => setAddForm({ ...addForm, id: e.target.value })} /></td>
              <td><input className="form-input" placeholder="암호" value={addForm.passwd} onChange={e => setAddForm({ ...addForm, passwd: e.target.value })} /></td>
              <td><input className="form-input" placeholder="SIM IMSI *" value={addForm.imsi} onChange={e => setAddForm({ ...addForm, imsi: e.target.value })} /></td>
              <td style={{ textAlign: 'center' }}>{addIsCall ? <input type="checkbox" checked={addForm.dnd} onChange={e => setAddForm({ ...addForm, dnd: e.target.checked })} /> : <span className="ts">—</span>}</td>
              <td>{addIsCall ? <input className="form-input" placeholder="대상" value={addForm.forward_id} onChange={e => setAddForm({ ...addForm, forward_id: e.target.value })} /> : <span className="ts">—</span>}</td>
              <td className="actions">
                <button className="btn btn--sm btn--primary" onClick={add}>추가</button>
                <button className="btn btn--sm btn--ghost" onClick={() => { setAdding(false); setAddForm(newAdd()) }}>취소</button>
              </td>
            </tr>
          )}
        </tbody>
      </table>
      </div>
      {!adding && (
        <button className="btn btn--ghost btn--sm" style={{ color: 'var(--primary)', fontSize: 12, marginTop: 4 }} onClick={() => { setAdding(true); setEditKey(null) }}>＋ 번호 추가</button>
      )}
    </div>
  )
}

// ── 그룹 드로어 (속성 + 멤버 피커) ──
function GroupDrawer(p: DrawerProps) {
  const { show } = useToast()
  const existing = p.drawer.kind === 'group' && p.drawer.id != null
    ? p.groups.find(g => g.id === p.drawer.id) : undefined
  const canManage = canManageGroup(p.me, existing?.authorized_user_id)
  const isNew = p.drawer.mode === 'add'
  const allowOwner = hasRole(p.me, 'manager') || (isNew && p.canGroupCreate)
  const [editing, setEditing] = useState(p.drawer.mode !== 'view')

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
      if (existing) { await groupsApi.update(existing.id, body as Partial<Group>); show('저장', 'ok'); setEditing(false); p.reload() }
      else { await groupsApi.create(body as Group); show('저장', 'ok'); p.onSaved() }
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
      {/* 그룹 속성은 테이블 컬럼에 이미 표시 — 보기 땐 [편집] 버튼만, 편집 땐 컴팩트 필드 노출 */}
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

      {/* 멤버 — 가입자 피커 (항상 그 자리에서 관리) */}
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
// 필드들을 한 줄(넘치면 다음 줄)로 흐르게 하는 컨테이너
function FieldRow({ children }: { children: React.ReactNode }) {
  return <div style={{ display: 'flex', flexWrap: 'wrap', gap: '8px 12px', alignItems: 'flex-end' }}>{children}</div>
}

// ── 통합 Excel import 모달 (사용자+VoLTE+PTT) ──
function ImportModal({ onClose, onDone }: { onClose: () => void; onDone: () => void }) {
  const { show } = useToast()
  const [result, setResult] = useState<{ total: number; created_users: number; created_voip: number; created_ptt: number; errors: Array<{ row: number; sheet: string; error: string }> } | null>(null)
  const [busy, setBusy] = useState(false)
  async function onFile(e: React.ChangeEvent<HTMLInputElement>) {
    const f = e.target.files?.[0]; if (!f) return
    setBusy(true); setResult(null)
    try {
      const buf = await f.arrayBuffer()
      const b64 = btoa(String.fromCharCode(...new Uint8Array(buf)))
      const r = await usersApi.importExcel(b64)
      setResult(r)
      if (r.created_users + r.created_voip + r.created_ptt > 0) onDone()
    } catch (err: unknown) { show(String(err), 'err') }
    finally { setBusy(false); e.target.value = '' }
  }
  return (
    <div className="modal-overlay" onClick={onClose}>
      <div className="modal-box" onClick={e => e.stopPropagation()}>
        <div className="modal-header">
          <span className="modal-title">사용자·번호 Excel 가져오기</span>
          <button className="modal-close" onClick={onClose}>✕</button>
        </div>
        <div className="modal-body">
          <p style={{ marginBottom: 12 }}>사용자 + VoLTE/PTT 번호를 한 Excel(.xlsx)로 일괄 등록합니다.</p>
          <div style={{ display: 'flex', gap: 12, alignItems: 'center', marginBottom: 16 }}>
            <label className="btn btn--primary" style={{ cursor: 'pointer' }}>파일 선택<input type="file" accept=".xlsx" onChange={onFile} style={{ display: 'none' }} /></label>
            <a href={usersApi.templateUrl} className="btn btn--outline" download>템플릿 다운로드</a>
            {busy && <span className="ts">처리 중...</span>}
          </div>
          {result && (
            <div style={{ background: 'var(--surface)', borderRadius: 8, padding: 16, fontSize: 13 }}>
              <div style={{ fontWeight: 600, marginBottom: 8 }}>결과</div>
              <div>사용자 <strong>{result.created_users}</strong> · VoLTE <strong>{result.created_voip}</strong> · PTT <strong>{result.created_ptt}</strong></div>
              {result.errors.length > 0 && (
                <div style={{ marginTop: 8, color: 'var(--danger)', fontSize: 12 }}>
                  {result.errors.map((er, i) => <div key={i}>[{er.sheet}] 행 {er.row}: {er.error}</div>)}
                </div>
              )}
            </div>
          )}
        </div>
        <div className="modal-footer"><button className="btn btn--ghost" onClick={onClose}>닫기</button></div>
      </div>
    </div>
  )
}
