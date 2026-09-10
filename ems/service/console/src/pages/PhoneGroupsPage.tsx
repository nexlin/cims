import { useConfirm } from '@core/components/custom/confirm'
import { useState, useEffect, useCallback, useMemo } from 'react'
import IconBtn from '@core/components/IconBtn'
import { ArrowLeft, ArrowRight, ChevronDown, ChevronRight, Pencil, Plus, Radio, Trash2, X } from 'lucide-react'
import { phoneGroupsApi, PHONE_GROUP_ERRORS, type PhoneGroup, type PhoneGroupInput, type PhoneGroupMember } from '@core/api/phoneGroups'
import { ApiError } from '@core/api/client'
import { usersApi, type UserSummary } from '@core/api/users'
import { orgApi, type Organization } from '@core/api/organizations'
import OrgTreePanel from '@core/components/OrgTreePanel'
import { DataTable, type Column } from '@core/components/DataTable'
import { buildPickIndex, type PickItem } from '@core/components/SubscriberPicker'
import { useToast } from '@core/components/Toast'
import { useAuth } from '@core/contexts/AuthContext'
import { canWriteConfig } from '@core/utils/permissions'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { NONE, fromSel, toSel } from '@core/components/custom/select-value'
import { Badge } from '@core/components/ui/badge'
import { Checkbox } from '@core/components/ui/checkbox'

// ── 전화 그룹 (dispatch_center.md §3.1·§3.2) ─────────────────────
//  전화 그룹 = 픽업 그룹 + (선택) 대표번호. 유선 전화의 일반 기능이며 관제 권한과 무관하다 —
//  감청·청취·관리 범위는 관리 › 역할(RolesPage)에 있다. id 가 곧 가입자 pickup_group 값이라
//  당겨받기·BLF·대표번호 병렬 호출이 한 축을 공유한다. 멤버십이 SoT — 가입자 편집의 pickup_group 은
//  여기서 파생된다(직접 편집 409 derived_from_phone_group). 가입자당 그룹 하나.
//  쓰기 = directory_write(콘솔 manager 전역, admin_api.md §6.7).

const ICON = 14

// 오류 토큰 → 문구 (admin_api.md §6.7). 토큰이 아니면 서버 detail/코드 그대로.
function errText(e: unknown): string {
  if (e instanceof ApiError) {
    const code = typeof e.data.error === 'string' ? e.data.error : ''
    if (code && PHONE_GROUP_ERRORS[code]) return PHONE_GROUP_ERRORS[code]
  }
  return String(e)
}

function Caret({ open }: { open: boolean }) {
  return <span className="text-muted-foreground inline-flex">
    {open ? <ChevronDown size={15} /> : <ChevronRight size={15} />}
  </span>
}

export default function PhoneGroupsPage() {
  const { show } = useToast()
  const confirm = useConfirm()
  const { user: me } = useAuth()
  const canWrite = canWriteConfig(me)

  const [orgScope, setOrgScope] = useState<string | null>(null)
  const [orgName, setOrgName] = useState('전체')
  const [search, setSearch] = useState('')
  const [users, setUsers] = useState<UserSummary[]>([])
  const [orgs, setOrgs] = useState<Organization[]>([])
  const [groups, setGroups] = useState<PhoneGroup[]>([])
  const [notMigrated, setNotMigrated] = useState(false)
  const [loading, setLoading] = useState(true)
  const [openId, setOpenId] = useState<string | null>(null)
  const [adding, setAdding] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [d, u, o] = await Promise.all([phoneGroupsApi.list(), usersApi.list(), orgApi.list()])
      setGroups(d.groups); setNotMigrated(d.schema === 'not_migrated'); setUsers(u); setOrgs(o)
    } catch (e: unknown) { show(errText(e), 'err') }
    finally { setLoading(false) }
  }, [show])
  useEffect(() => { load() }, [load])

  const orgPathOf = useCallback((code: string) => orgs.find(o => o.code === code)?.code_path || code, [orgs])
  const orgCodeOfId = useCallback((id: number | null) => orgs.find(o => o.id === id)?.code || '', [orgs])
  const inScope = useCallback((orgCode: string) => !orgScope || (orgPathOf(orgCode) || '').startsWith(orgScope), [orgScope, orgPathOf])

  // 멤버 = 유선(VoLTE/VoIP) 가입 번호 — 대표번호 포크·BLF 대상 회선. PTT 회선은 넣지 않는다(§3.2)
  const callIndex = useMemo(() => buildPickIndex(users, 'call'), [users])
  const nameOf = useMemo(() => {
    const m = new Map<string, string>()
    for (const it of callIndex) m.set(it.value, it.userName)
    return m
  }, [callIndex])
  // 가입자 → 소속 전화 그룹 (가입자당 하나) — 후보 목록에서 타 그룹 소속 표시
  const groupOfUser = useMemo(() => {
    const m = new Map<string, string>()
    for (const g of groups) for (const mb of g.members) m.set(mb.user_id, g.id)
    return m
  }, [groups])

  const rows = useMemo(() => {
    const q = search.trim().toLowerCase()
    return groups.filter(g => inScope(orgCodeOfId(g.org_id)) &&
      (!q || g.id.toLowerCase().includes(q) || g.name.toLowerCase().includes(q) || (g.pilot_id || '').includes(q)))
  }, [groups, inScope, orgCodeOfId, search])

  function toggleOpen(id: string) { setAdding(false); setOpenId(cur => cur === id ? null : id) }

  async function deleteGroup(g: PhoneGroup) {
    if (!await confirm({ title: '전화 그룹 삭제', tone: 'danger', confirmLabel: '삭제', body: <>
      전화 그룹 "{g.name}" ({g.id}) 삭제?
      <div className="mt-1">멤버 {g.members.length}명의 픽업 그룹이 해제됩니다.</div>
    </> })) return
    try { await phoneGroupsApi.delete(g.id); show('삭제', 'ok'); if (openId === g.id) setOpenId(null); load() }
    catch (e: unknown) { show(errText(e), 'err') }
  }

  const cols: Column<PhoneGroup>[] = [
    { key: 'exp', header: '', width: 26, render: g => <Caret open={openId === g.id} /> },
    { key: 'name', header: '그룹명', sortable: true, render: g => <span className="font-semibold">{g.name}</span> },
    { key: 'id', header: 'ID', width: 120, sortable: true, render: g => <span className="text-sm text-muted-foreground">{g.id}</span> },
    { key: 'pilot', header: '대표번호', width: 110, sortable: true, sortValue: g => g.pilot_id || '', render: g => g.pilot_id
      ? <span className="inline-flex items-center gap-1"><Radio className="text-primary" size={12}/><span className="text-sm text-muted-foreground">{g.pilot_id}</span></span>
      : <span className="text-sm text-muted-foreground">—</span> },
    { key: 'alert', header: '호출', width: 90, render: g => <span className="text-sm text-muted-foreground">{g.pilot_id ? (g.alert_mode === 'parallel' ? `병렬 ${g.no_answer_sec}s` : `순차 ${g.no_answer_sec}s`) : '—'}</span> },
    { key: 'overflow', header: '넘김', width: 110, render: g => <span className="text-sm text-muted-foreground">{g.overflow_target || '—'}</span> },
    { key: 'org', header: '조직', width: 130, render: g => <span className="text-sm text-muted-foreground">{orgs.find(o => o.id === g.org_id)?.name || '—'}</span> },
    { key: 'members', header: '멤버', width: 60, align: 'center', render: g => <span className="text-sm text-muted-foreground">{g.members.length}명</span> },
    { key: 'act', header: '', width: 84, align: 'right', render: g => canWrite ? (
      <span className="flex gap-1.5" onClick={e => e.stopPropagation()}>
        <IconBtn title="편집" onClick={() => toggleOpen(g.id)}><Pencil size={ICON} /></IconBtn>
        <IconBtn title="삭제" tone="danger" onClick={() => deleteGroup(g)}><Trash2 size={ICON} /></IconBtn>
      </span>
    ) : <span className="text-sm text-muted-foreground">—</span> },
  ]

  const openGroup = openId ? groups.find(g => g.id === openId) : undefined

  return (
    <div className="flex gap-4 items-stretch flex-1 min-h-0">
      <OrgTreePanel className="flex-[0_0_200px] w-[200px] max-w-[200px]" fill selectedPath={orgScope} onSelect={(p, n) => { setOrgScope(p); setOrgName(n) }}/>

      <div className="panel flex flex-1 flex-col overflow-hidden rounded-md border border-border bg-card flex-1 min-w-0">
        <div className="toolbar flex items-center gap-2.5 border-b border-border bg-muted px-4 py-3">
          <span className="font-semibold text-md">{orgName}</span>
          <Input className="flex-1 max-w-[220px]" placeholder="그룹명·ID·대표번호 검색" value={search}
            onChange={e => setSearch(e.target.value)}/>
          {search && <Button variant="ghost" onClick={() => setSearch('')}
        aria-label="검색어 지우기"><X size={13} /></Button>}
          <span className="ml-auto flex gap-1.5">
            {canWrite && !notMigrated && (
              <Button variant="default" onClick={() => { setOpenId(null); setAdding(a => !a) }}><Plus size={13} /> 전화 그룹</Button>
            )}
          </span>
        </div>

        {notMigrated && (
          <div className="py-2.5 px-4 text-sm text-muted-foreground border-b border-border bg-muted">
            DB 에 <code>phone_groups</code> 테이블이 없습니다 — <code>sql/migrate_phone_groups_roles.sql</code> 적용 후 사용할 수 있습니다.
            당겨받기는 가입자 <code>pickup_group</code> 축으로 계속 동작합니다.
          </div>
        )}

        {adding && (
          <div className="border-b border-border bg-muted py-3 px-4">
            <div className="font-semibold text-sm text-primary mb-2">새 전화 그룹</div>
            <GroupDrawer mode="add" orgs={orgs} canWrite={canWrite}
              callIndex={callIndex} nameOf={nameOf} groupOfUser={groupOfUser} orgScope={orgScope} orgPathOf={orgPathOf}
              onClose={() => setAdding(false)} onSaved={() => { setAdding(false); load() }} reload={load} />
          </div>
        )}

        <DataTable<PhoneGroup> columns={cols} rows={rows} rowKey={g => g.id} loading={loading}
          onRowClick={g => toggleOpen(g.id)} expandedKey={openId}
          renderExpanded={openGroup ? () => (
            <div className="py-3 px-4">
              <GroupDrawer key={openGroup.id} mode="view" group={openGroup} orgs={orgs} canWrite={canWrite}
                callIndex={callIndex} nameOf={nameOf} groupOfUser={groupOfUser}
                orgScope={orgScope} orgPathOf={orgPathOf}
                onClose={() => setOpenId(null)} onSaved={() => { setOpenId(null); load() }} reload={load} />
            </div>
          ) : undefined}
          pageSize={50} emptyText={notMigrated ? '마이그레이션 전' : '전화 그룹 없음'} />
      </div>
    </div>
  )
}

// ════════════════════════════════════════════════════════════
//  드로어 — 속성(대표번호·호출 방식) + 멤버(alert_order)
// ════════════════════════════════════════════════════════════
interface DrawerProps {
  mode: 'view' | 'add'
  group?: PhoneGroup
  orgs: Organization[]
  canWrite: boolean
  callIndex: PickItem[]
  nameOf: Map<string, string>
  groupOfUser: Map<string, string>
  orgScope: string | null
  orgPathOf: (code: string) => string
  onClose: () => void
  onSaved: () => void
  reload: () => void
}

function GroupDrawer(p: DrawerProps) {
  const { show } = useToast()
  const existing = p.group
  const isNew = p.mode === 'add'
  const [editing, setEditing] = useState(isNew)
  const [form, setForm] = useState<PhoneGroupInput>(() => existing
    ? { name: existing.name, pilot_id: existing.pilot_id || '', service_ref: existing.service_ref || '', alert_mode: existing.alert_mode,
        no_answer_sec: existing.no_answer_sec, busy_members: existing.busy_members, overflow_target: existing.overflow_target || '',
        org_id: existing.org_id }
    : { name: '', pilot_id: '', service_ref: '', alert_mode: 'parallel', no_answer_sec: 30, busy_members: 'skip', overflow_target: '', org_id: null })
  const [members, setMembers] = useState<PhoneGroupMember[]>(existing?.members || [])
  const existingId = existing?.id
  const reloadMembers = useCallback(() => {
    if (existingId) phoneGroupsApi.listMembers(existingId).then(setMembers).catch(() => setMembers([]))
  }, [existingId])
  useEffect(() => { reloadMembers() }, [reloadMembers])

  async function save() {
    if (!form.name) { show('그룹명 필수', 'err'); return }
    if (form.pilot_id && !form.service_ref) { show('대표번호에는 접속서비스(유선 VoIP)가 필요합니다', 'err'); return }
    const body: PhoneGroupInput = { ...form, pilot_id: form.pilot_id || null, overflow_target: form.overflow_target || null,
      service_ref: form.service_ref || null }
    try {
      if (existing) { await phoneGroupsApi.update(existing.id, body); show('저장', 'ok'); setEditing(false); p.reload() }
      else { await phoneGroupsApi.create(body); show('생성', 'ok'); p.onSaved() }
    } catch (e: unknown) { show(errText(e), 'err') }
  }
  async function addMembers(ids: string[]) {
    if (!existing || !ids.length) return
    const results = await Promise.allSettled(ids.map((uid, i) => phoneGroupsApi.addMember(existing.id, { user_id: uid, alert_order: members.length + i })))
    const ok = results.filter(r => r.status === 'fulfilled').length
    const fail = results.length - ok
    const firstErr = results.find(r => r.status === 'rejected') as PromiseRejectedResult | undefined
    show(fail ? `${ok}명 추가, ${fail}명 실패${firstErr ? ` — ${errText(firstErr.reason)}` : ''}` : `${ok}명 추가`, fail ? 'err' : 'ok')
    reloadMembers(); p.reload()
  }
  async function removeMembers(ids: string[]) {
    if (!existing || !ids.length) return
    await Promise.allSettled(ids.map(uid => phoneGroupsApi.removeMember(existing.id, uid)))
    show(`${ids.length}명 제거`, 'ok'); reloadMembers(); p.reload()
  }
  async function saveOrder(uid: string, order: number) {
    if (!existing) return
    try { await phoneGroupsApi.addMember(existing.id, { user_id: uid, alert_order: order }); reloadMembers(); p.reload() }
    catch (e: unknown) { show(errText(e), 'err') }
  }

  const memberIds = useMemo(() => new Set(members.map(m => m.user_id)), [members])

  return (
    <div className="flex flex-col gap-2.5 text-md">
      {editing ? (
        <FieldRow>
          <Field label="그룹명 *" w={170}><Input  autoFocus value={form.name || ''} onChange={e => setForm({ ...form, name: e.target.value })} /></Field>
          <Field label="대표번호" w={120}><Input  placeholder="예: 7000" title="다이얼 가능한 주소 — 가입 번호·다른 대표번호와 겹치면 409" value={form.pilot_id || ''} onChange={e => setForm({ ...form, pilot_id: e.target.value.trim() })} /></Field>
          <Field label="접속서비스" w={110}><Input  placeholder="voip" title="대표번호가 속한 유선 VoIP 접속서비스 name — 도메인·SRTP 정책·피처코드" value={form.service_ref || ''} onChange={e => setForm({ ...form, service_ref: e.target.value.trim() })} /></Field>
          <Field label="호출 방식" w={120}>
            <Select value={toSel(form.alert_mode || 'parallel')} onValueChange={(v: string) => setForm({ ...form, alert_mode: fromSel(v) as PhoneGroup['alert_mode'] })}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="parallel">병렬 (전원 동시)</SelectItem>
                <SelectItem value="sequential">순차 (후속)</SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <Field label="무응답(초)" w={80}><Input  type="number" min={5} value={form.no_answer_sec ?? 30} onChange={e => setForm({ ...form, no_answer_sec: Number(e.target.value) })} /></Field>
          <Field label="통화 중 그룹원" w={120}>
            <Select value={toSel(form.busy_members || 'skip')} onValueChange={(v: string) => setForm({ ...form, busy_members: fromSel(v) as PhoneGroup['busy_members'] })}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="skip">호출 안 함</SelectItem>
                <SelectItem value="alert">호출 (통화대기)</SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <Field label="무응답 넘김" w={130}><Input  placeholder="대표번호/가입 번호" value={form.overflow_target || ''} onChange={e => setForm({ ...form, overflow_target: e.target.value.trim() })} /></Field>
          <Field label="조직" w={170}>
            <Select value={toSel(form.org_id == null ? '' : String(form.org_id))} onValueChange={(v: string) => setForm({ ...form, org_id: fromSel(v) ? Number(fromSel(v)) : null })}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value={NONE}>없음</SelectItem>
                {p.orgs.map(o => <SelectItem key={o.id} value={String(o.id)}>{o.name} ({o.code})</SelectItem>)}
              </SelectContent>
            </Select>
          </Field>
          <div className="flex gap-1.5 items-center">
            <Button variant="default" onClick={save}>저장</Button>
            <Button variant="ghost" onClick={() => isNew ? p.onClose() : setEditing(false)}>취소</Button>
          </div>
        </FieldRow>
      ) : existing && (
        <div className="flex items-center gap-4 flex-wrap text-sm">
          <span className="text-sm text-muted-foreground">ID {existing.id}</span>
          <span className="text-sm text-muted-foreground">대표번호 {existing.pilot_id ? `${existing.pilot_id} (${existing.service_ref || '—'}, ${existing.alert_mode === 'parallel' ? '병렬' : '순차'} ${existing.no_answer_sec}s, 통화중 ${existing.busy_members === 'skip' ? '제외' : '호출'})` : '없음'}</span>
          <span className="text-sm text-muted-foreground">넘김 {existing.overflow_target || '—'}</span>
          {p.canWrite && <Button className="ml-auto" onClick={() => setEditing(true)}>속성 편집</Button>}
        </div>
      )}

      {existing && (
        <MemberTransfer members={members} memberIds={memberIds} callIndex={p.callIndex} nameOf={p.nameOf} groupOfUser={p.groupOfUser}
          selfId={existing.id} canManage={p.canWrite}
          orgScope={p.orgScope} orgPathOf={p.orgPathOf} onAdd={addMembers} onRemove={removeMembers} onSaveOrder={saveOrder} />
      )}
    </div>
  )
}

// ── 멤버 transfer (좌: 멤버(alert_order) ↔ 우: 조직트리 + 유선 가입자) ──
function MemberTransfer({ members, memberIds, callIndex, nameOf, groupOfUser, selfId, canManage, orgScope, orgPathOf, onAdd, onRemove, onSaveOrder }: {
  members: PhoneGroupMember[]; memberIds: Set<string>; callIndex: PickItem[]; nameOf: Map<string, string>
  groupOfUser: Map<string, string>; selfId: string; canManage: boolean
  orgScope: string | null; orgPathOf: (code: string) => string
  onAdd: (ids: string[]) => Promise<void>; onRemove: (ids: string[]) => Promise<void>; onSaveOrder: (uid: string, order: number) => void
}) {
  const [selMembers, setSelMembers] = useState<Set<string>>(new Set())
  const [treeScope, setTreeScope] = useState<string | null>(orgScope)
  const [treeName, setTreeName] = useState('전체')
  const [q, setQ] = useState('')
  const [picked, setPicked] = useState<Set<string>>(new Set())
  const [busy, setBusy] = useState(false)

  const candidates = useMemo(() => {
    const s = q.trim().toLowerCase()
    const inScope = (it: PickItem) => !treeScope || (orgPathOf(it.orgCode) || '').startsWith(treeScope)
    return callIndex.filter(it => !memberIds.has(it.value)).filter(inScope)
      .filter(it => !s || it.userName.toLowerCase().includes(s) || it.value.toLowerCase().includes(s) || it.orgCode.toLowerCase().includes(s))
      .slice(0, 500)
  }, [callIndex, memberIds, treeScope, orgPathOf, q])

  function toggle(set: React.Dispatch<React.SetStateAction<Set<string>>>, v: string) {
    set(p => { const n = new Set(p); if (n.has(v)) n.delete(v); else n.add(v); return n })
  }
  async function doAdd(ids: string[]) { if (!ids.length) return; setBusy(true); await onAdd(ids); setBusy(false); setPicked(new Set()) }
  async function doRemove(ids: string[]) { if (!ids.length) return; setBusy(true); await onRemove(ids); setBusy(false); setSelMembers(new Set()) }

  return (
    <div className="mt-2.5 border-t border-border pt-3">
      <div className="flex items-stretch gap-2.5 h-[320px]">
        <div className="flex min-w-0 flex-col overflow-hidden rounded-md border border-border bg-card flex-1">
          <div className="flex items-center gap-2 border-b border-border px-2.5 py-2 text-sm font-semibold">멤버 <Badge  variant="brandSoft">{members.length}</Badge>
            <span className="ml-auto font-normal text-muted-foreground text-xs">순서 = 순차 호출·포크 상한 절삭 순</span></div>
          <div className="flex-1 min-h-0 overflow-y-auto">
            {members.length === 0
              ? <div className="p-3.5 text-sm text-center text-muted-foreground">멤버 없음<br />우측에서 가입자를 선택해 <ArrowLeft size={11} className="inline align-[-1px]" /> 추가</div>
              : members.map(m => (
                <div key={m.user_id} className="flex items-center gap-2 px-2.5 py-1.5 text-sm" style={{
                  borderLeft: selMembers.has(m.user_id) ? '3px solid var(--primary)' : '3px solid transparent' }}>
                  {canManage && <Checkbox  checked={selMembers.has(m.user_id)} onCheckedChange={() => toggle(setSelMembers, m.user_id)} />}
                  <span className="flex flex-col min-w-0 flex-1">
                    <span className="font-semibold whitespace-nowrap overflow-hidden text-ellipsis">{nameOf.get(m.user_id) || '—'}</span>
                    <span className="text-muted-foreground text-xs">{m.user_id}</span>
                  </span>
                  <Input className="w-[54px]" type="number" title="alert_order" disabled={!canManage} value={m.alert_order}
                    onChange={e => onSaveOrder(m.user_id, Number(e.target.value))}/>
                  {canManage && <IconBtn title="제거" tone="danger" onClick={() => doRemove([m.user_id])}><ArrowRight size={ICON} /></IconBtn>}
                </div>
              ))}
          </div>
        </div>

        {canManage && (
          <div className="flex flex-col justify-center gap-2.5 self-center">
            <Button className="inline-flex items-center gap-1 whitespace-nowrap" variant="default" disabled={busy || picked.size === 0} onClick={() => doAdd(Array.from(picked))}><ArrowLeft size={14} /> 추가{picked.size ? ` ${picked.size}` : ''}</Button>
            <Button className="inline-flex items-center gap-1 whitespace-nowrap" disabled={busy || selMembers.size === 0} onClick={() => doRemove(Array.from(selMembers))}>제거{selMembers.size ? ` ${selMembers.size}` : ''} <ArrowRight size={14} /></Button>
          </div>
        )}

        <div className="flex min-w-0 flex-col overflow-hidden rounded-md border border-border bg-card flex-[1.3]">
          <div className="flex items-center gap-2 border-b border-border px-2.5 py-2 text-sm font-semibold">유선 가입자 <Badge  variant="neutralSoft">{candidates.length}</Badge></div>
          <div className="flex flex-1 min-h-0">
            <OrgTreePanel className="flex-[0_0_150px] w-[150px] max-w-[150px] border-0 border-r border-border rounded-none" fill selectedPath={treeScope} onSelect={(pth, n) => { setTreeScope(pth); setTreeName(n) }}/>
            <div className="flex-1 min-w-0 flex flex-col">
              <div className="flex items-center gap-1.5 py-1.5 px-2 border-b border-border">
                <Input className="flex-1 text-sm" placeholder={`${treeName} 내 검색`} value={q} onChange={e => setQ(e.target.value)}/>
              </div>
              <div className="flex-1 min-h-0 overflow-y-auto">
                {candidates.length === 0
                  ? <div className="p-3.5 text-sm text-center text-muted-foreground">{callIndex.length ? `${treeName}에 추가할 가입자 없음` : '불러오는 중...'}</div>
                  : candidates.map(c => {
                    const other = groupOfUser.get(c.value)
                    return (
                      <div key={c.value} onDoubleClick={() => canManage && doAdd([c.value])} onClick={() => canManage && toggle(setPicked, c.value)}
                        className="flex cursor-pointer items-center gap-2 px-2.5 py-1.5 text-sm" style={{
                          borderLeft: picked.has(c.value) ? '3px solid var(--primary)' : '3px solid transparent' }}>
                        <Checkbox checked={picked.has(c.value)} tabIndex={-1} className="pointer-events-none" />
                        <span className="flex flex-col min-w-0 flex-1">
                          <span className="font-semibold whitespace-nowrap overflow-hidden text-ellipsis">{c.userName}</span>
                          <span className="text-muted-foreground text-xs">{c.value}{c.orgCode ? ` · ${c.orgCode}` : ''}</span>
                        </span>
                        {other && other !== selfId && <Badge  variant="warningSoft" title="다른 전화 그룹 소속 — 추가하면 이동(가입자당 그룹 하나)">{other}</Badge>}
                      </div>
                    )
                  })}
              </div>
            </div>
          </div>
        </div>
      </div>
      {canManage && <div className="text-xs text-muted-foreground mt-1.5">가입자 더블클릭 = 바로 추가 · 다른 그룹 소속 가입자는 추가 시 이동한다 · 같은 사람의 PTT 회선도 pickup_group 을 물려받는다 · 반영은 다음 REGISTER 갱신부터</div>}
    </div>
  )
}

function Field({ label, children, w }: { label: string; children: React.ReactNode; w?: number | string }) {
  return (
    <label className="flex min-w-[110px] flex-col gap-0.5" style={{ width: w, flex: w ? undefined : '1 1 150px' }}>
      <span className="text-xs text-muted-foreground">{label}</span>
      {children}
    </label>
  )
}
function FieldRow({ children }: { children: React.ReactNode }) {
  return <div className="flex flex-wrap items-end gap-x-3 gap-y-2">{children}</div>
}
