import { useConfirm } from '@core/components/custom/confirm'
import { useState, useEffect, useCallback, useMemo } from 'react'
import IconBtn from '@core/components/IconBtn'
import { ArrowLeft, ArrowRight, ChevronDown, ChevronRight, Headphones, Pencil, Plus, Radio, Trash2, X } from 'lucide-react'
import { dispatchApi, type DispatchGroup, type DispatchGroupInput, type DispatchMember,
  type MonitorScope, type PttListen } from '@core/api/dispatch'
import { usersApi, type UserSummary } from '@core/api/users'
import { groupsApi, type Group } from '@core/api/groups'
import { orgApi, type Organization } from '@core/api/organizations'
import OrgTreePanel from '@core/components/OrgTreePanel'
import { DataTable, type Column } from '@core/components/DataTable'
import { buildPickIndex, type PickItem } from '@core/components/SubscriberPicker'
import { useToast } from '@core/components/Toast'
import { useAuth } from '@core/contexts/AuthContext'
import { hasRole } from '@core/utils/permissions'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { NONE, fromSel, toSel } from '@core/components/custom/select-value'
import { Badge } from '@core/components/ui/badge'
import { Checkbox } from '@core/components/ui/checkbox'

// ── 관제 그룹 (dispatch_center.md §3) ─────────────────────────
//  관제 그룹 = 픽업 그룹 + (선택) 대표번호 + (선택) 감청 범위. id 가 곧 가입자 pickup_group 값이라
//  당겨받기·BLF·대표번호 병렬 호출·감청 범위가 한 축을 공유한다. 멤버십이 SoT — 가입자 편집의
//  pickup_group 은 여기서 파생된다(직접 편집 409). 가입자당 그룹 하나.
//  감청 범위(monitor_scope/ptt_listen)와 감청 그룹 편입은 manager 승인 사항(§5.8).

const ICON = 14

const SCOPE_LABEL: Record<MonitorScope, string> = { none: '없음', own: '자기 그룹', listed: '지정 그룹', all: '전체' }
const SCOPE_HINT: Record<MonitorScope, string> = {
  none: '감청 불가 — 순수 당겨받기·대표번호 그룹',
  own: '자기 그룹원의 통화만 dialog 감시·Join 청취',
  listed: '아래 대상 그룹의 통화를 dialog 감시·Join 청취',
  all: '모든 가입자의 통화를 dialog 감시·Join 청취 (업무망 합법감청 — 운영 규약·감사 전제)',
}
const PTT_LABEL: Record<PttListen, string> = { none: '없음', listed: '지정 그룹', all: '전체' }

function Caret({ open }: { open: boolean }) {
  return <span className="text-muted-foreground inline-flex">
    {open ? <ChevronDown size={15} /> : <ChevronRight size={15} />}
  </span>
}

export default function DispatchGroupsPage() {
  const { show } = useToast()
  const confirm = useConfirm()
  const { user: me } = useAuth()
  const canWrite = hasRole(me, 'operator')

  const [orgScope, setOrgScope] = useState<string | null>(null)
  const [orgName, setOrgName] = useState('전체')
  const [search, setSearch] = useState('')
  const [users, setUsers] = useState<UserSummary[]>([])
  const [orgs, setOrgs] = useState<Organization[]>([])
  const [pttGroups, setPttGroups] = useState<Group[]>([])
  const [groups, setGroups] = useState<DispatchGroup[]>([])
  const [notMigrated, setNotMigrated] = useState(false)
  const [loading, setLoading] = useState(true)
  const [openId, setOpenId] = useState<string | null>(null)
  const [adding, setAdding] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [d, u, o, g] = await Promise.all([dispatchApi.list(), usersApi.list(), orgApi.list(), groupsApi.list().catch(() => [] as Group[])])
      setGroups(d.groups); setNotMigrated(d.schema === 'not_migrated'); setUsers(u); setOrgs(o); setPttGroups(g)
    } catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [show])
  useEffect(() => { load() }, [load])

  const orgPathOf = useCallback((code: string) => orgs.find(o => o.code === code)?.code_path || code, [orgs])
  const orgCodeOfId = useCallback((id: number | null) => orgs.find(o => o.id === id)?.code || '', [orgs])
  const inScope = useCallback((orgCode: string) => !orgScope || (orgPathOf(orgCode) || '').startsWith(orgScope), [orgScope, orgPathOf])

  // 멤버 = VoLTE 가입 번호(관제 소프트폰 축) — 이름 매핑 공유
  const callIndex = useMemo(() => buildPickIndex(users, 'call'), [users])
  const nameOf = useMemo(() => {
    const m = new Map<string, string>()
    for (const it of callIndex) m.set(it.value, it.userName)
    return m
  }, [callIndex])
  // 가입자 → 소속 관제 그룹 (가입자당 하나) — 후보 목록에서 타 그룹 소속 표시
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

  async function deleteGroup(g: DispatchGroup) {
    if (!await confirm({ title: '관제 그룹 삭제', tone: 'danger', confirmLabel: '삭제', body: <>
      관제 그룹 "{g.name}" ({g.id}) 삭제?
      <div className="mt-1">멤버 {g.members.length}명의 픽업 그룹이 해제됩니다.</div>
    </> })) return
    try { await dispatchApi.delete(g.id); show('삭제', 'ok'); if (openId === g.id) setOpenId(null); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  const cols: Column<DispatchGroup>[] = [
    { key: 'exp', header: '', width: 26, render: g => <Caret open={openId === g.id} /> },
    { key: 'name', header: '그룹명', sortable: true, render: g => (
      <span><span className="font-semibold">{g.name}</span>
        {g.monitor_scope !== 'none' && <Badge className="ml-1" variant="dangerSoft" title={`감청 범위: ${SCOPE_LABEL[g.monitor_scope]}`}>감청</Badge>}
        {g.ptt_listen !== 'none' && <Badge className="ml-0.5" variant="warningSoft" title={`PTT 청취: ${PTT_LABEL[g.ptt_listen]}`}>PTT청취</Badge>}
      </span>
    ) },
    { key: 'id', header: 'ID', width: 120, sortable: true, render: g => <span className="text-sm text-muted-foreground">{g.id}</span> },
    { key: 'pilot', header: '대표번호', width: 110, sortable: true, sortValue: g => g.pilot_id || '', render: g => g.pilot_id
      ? <span className="inline-flex items-center gap-1"><Radio className="text-primary" size={12}/><span className="text-sm text-muted-foreground">{g.pilot_id}</span></span>
      : <span className="text-sm text-muted-foreground">—</span> },
    { key: 'alert', header: '호출', width: 90, render: g => <span className="text-sm text-muted-foreground">{g.pilot_id ? (g.alert_mode === 'parallel' ? `병렬 ${g.no_answer_sec}s` : `순차 ${g.no_answer_sec}s`) : '—'}</span> },
    { key: 'overflow', header: '넘김', width: 110, render: g => <span className="text-sm text-muted-foreground">{g.overflow_target || '—'}</span> },
    { key: 'scope', header: '감청', width: 90, render: g => <span className="text-sm text-muted-foreground">{SCOPE_LABEL[g.monitor_scope]}</span> },
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
              <Button variant="default" onClick={() => { setOpenId(null); setAdding(a => !a) }}><Plus size={13} /> 관제 그룹</Button>
            )}
          </span>
        </div>

        {notMigrated && (
          <div className="py-2.5 px-4 text-sm text-muted-foreground border-b border-border bg-muted">
            DB 에 <code>dispatch_groups</code> 테이블이 없습니다 — <code>sql/migrate_dispatch_groups.sql</code> 적용 후 사용할 수 있습니다.
            당겨받기는 가입자 <code>pickup_group</code> 축으로 계속 동작합니다.
          </div>
        )}

        {adding && (
          <div className="border-b border-border bg-muted py-3 px-4">
            <div className="font-semibold text-sm text-primary mb-2">새 관제 그룹</div>
            <GroupDrawer mode="add" orgs={orgs} isManager={hasRole(me, 'manager')} canWrite={canWrite} allGroups={groups} pttGroups={pttGroups}
              callIndex={callIndex} nameOf={nameOf} groupOfUser={groupOfUser} orgScope={orgScope} orgPathOf={orgPathOf}
              onClose={() => setAdding(false)} onSaved={() => { setAdding(false); load() }} reload={load} />
          </div>
        )}

        <DataTable<DispatchGroup> columns={cols} rows={rows} rowKey={g => g.id} loading={loading}
          onRowClick={g => toggleOpen(g.id)} expandedKey={openId}
          renderExpanded={openGroup ? () => (
            <div className="py-3 px-4">
              <GroupDrawer key={openGroup.id} mode="view" group={openGroup} orgs={orgs} isManager={hasRole(me, 'manager')} canWrite={canWrite}
                allGroups={groups} pttGroups={pttGroups} callIndex={callIndex} nameOf={nameOf} groupOfUser={groupOfUser}
                orgScope={orgScope} orgPathOf={orgPathOf}
                onClose={() => setOpenId(null)} onSaved={() => { setOpenId(null); load() }} reload={load} />
            </div>
          ) : undefined}
          pageSize={50} emptyText={notMigrated ? '마이그레이션 전' : '관제 그룹 없음'} />
      </div>
    </div>
  )
}

// ════════════════════════════════════════════════════════════
//  드로어 — 속성 + 멤버 + 감청/청취 대상
// ════════════════════════════════════════════════════════════
interface DrawerProps {
  mode: 'view' | 'add'
  group?: DispatchGroup
  orgs: Organization[]
  isManager: boolean
  canWrite: boolean
  allGroups: DispatchGroup[]
  pttGroups: Group[]
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
  const [form, setForm] = useState<DispatchGroupInput>(() => existing
    ? { name: existing.name, pilot_id: existing.pilot_id || '', service_ref: existing.service_ref || 'volte', alert_mode: existing.alert_mode,
        no_answer_sec: existing.no_answer_sec, busy_members: existing.busy_members, overflow_target: existing.overflow_target || '',
        monitor_scope: existing.monitor_scope, ptt_listen: existing.ptt_listen, listen_visibility: existing.listen_visibility, org_id: existing.org_id }
    : { name: '', pilot_id: '', service_ref: 'volte', alert_mode: 'parallel', no_answer_sec: 30, busy_members: 'skip', overflow_target: '',
        monitor_scope: 'none', ptt_listen: 'none', listen_visibility: 'hidden', org_id: null })
  const [members, setMembers] = useState<DispatchMember[]>(existing?.members || [])
  const existingId = existing?.id
  const reloadMembers = useCallback(() => {
    if (existingId) dispatchApi.listMembers(existingId).then(setMembers).catch(() => setMembers([]))
  }, [existingId])
  useEffect(() => { reloadMembers() }, [reloadMembers])

  // 감청 범위·청취 범위는 manager 만 바꿀 수 있다 (§5.8) — operator 화면에서는 읽기 전용
  const scopeLocked = !p.isManager

  async function save() {
    if (!form.name) { show('그룹명 필수', 'err'); return }
    if (form.pilot_id && !form.service_ref) { show('대표번호에는 접속서비스가 필요합니다', 'err'); return }
    const body: DispatchGroupInput = { ...form, pilot_id: form.pilot_id || null, overflow_target: form.overflow_target || null,
      service_ref: form.service_ref || null }
    if (scopeLocked && existing) { delete body.monitor_scope; delete body.ptt_listen }
    try {
      if (existing) { await dispatchApi.update(existing.id, body); show('저장', 'ok'); setEditing(false); p.reload() }
      else { await dispatchApi.create(body); show('생성', 'ok'); p.onSaved() }
    } catch (e: unknown) { show(String(e), 'err') }
  }
  async function addMembers(ids: string[]) {
    if (!existing || !ids.length) return
    const results = await Promise.allSettled(ids.map((uid, i) => dispatchApi.addMember(existing.id, { user_id: uid, alert_order: members.length + i })))
    const ok = results.filter(r => r.status === 'fulfilled').length
    const fail = results.length - ok
    const firstErr = results.find(r => r.status === 'rejected') as PromiseRejectedResult | undefined
    show(fail ? `${ok}명 추가, ${fail}명 실패${firstErr ? ` — ${String(firstErr.reason)}` : ''}` : `${ok}명 추가`, fail ? 'err' : 'ok')
    reloadMembers(); p.reload()
  }
  async function removeMembers(ids: string[]) {
    if (!existing || !ids.length) return
    await Promise.allSettled(ids.map(uid => dispatchApi.removeMember(existing.id, uid)))
    show(`${ids.length}명 제거`, 'ok'); reloadMembers(); p.reload()
  }
  async function saveOrder(uid: string, order: number) {
    if (!existing) return
    try { await dispatchApi.addMember(existing.id, { user_id: uid, alert_order: order }); reloadMembers(); p.reload() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function saveMonitorTargets(ids: string[]) {
    if (!existing) return
    try { await dispatchApi.setMonitorTargets(existing.id, ids); show('감청 대상 저장', 'ok'); p.reload() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function savePttTargets(ids: string[]) {
    if (!existing) return
    try { await dispatchApi.setPttTargets(existing.id, ids); show('청취 대상 저장', 'ok'); p.reload() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  const memberIds = useMemo(() => new Set(members.map(m => m.user_id)), [members])
  const monitoring = (existing?.monitor_scope || 'none') !== 'none' || (existing?.ptt_listen || 'none') !== 'none'

  return (
    <div className="flex flex-col gap-2.5 text-md">
      {editing ? (
        <FieldRow>
          <Field label="그룹명 *" w={170}><Input  autoFocus value={form.name || ''} onChange={e => setForm({ ...form, name: e.target.value })} /></Field>
          <Field label="대표번호" w={120}><Input  placeholder="예: 7000" title="다이얼 가능한 주소 — 가입 번호와 겹치면 409" value={form.pilot_id || ''} onChange={e => setForm({ ...form, pilot_id: e.target.value.trim() })} /></Field>
          <Field label="접속서비스" w={110}><Input  placeholder="volte" title="대표번호가 속한 접속서비스 name — 도메인·SRTP 정책" value={form.service_ref || ''} onChange={e => setForm({ ...form, service_ref: e.target.value.trim() })} /></Field>
          <Field label="호출 방식" w={120}>
            <Select value={toSel(form.alert_mode || 'parallel')} onValueChange={(v: string) => setForm({ ...form, alert_mode: fromSel(v) as DispatchGroup['alert_mode'] })}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="parallel">병렬 (전원 동시)</SelectItem>
                <SelectItem value="sequential">순차 (후속)</SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <Field label="무응답(초)" w={80}><Input  type="number" min={5} value={form.no_answer_sec ?? 30} onChange={e => setForm({ ...form, no_answer_sec: Number(e.target.value) })} /></Field>
          <Field label="통화 중 그룹원" w={120}>
            <Select value={toSel(form.busy_members || 'skip')} onValueChange={(v: string) => setForm({ ...form, busy_members: fromSel(v) as DispatchGroup['busy_members'] })}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="skip">호출 안 함</SelectItem>
                <SelectItem value="alert">호출 (통화대기)</SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <Field label="무응답 넘김" w={130}><Input  placeholder="대표번호/내선" value={form.overflow_target || ''} onChange={e => setForm({ ...form, overflow_target: e.target.value.trim() })} /></Field>
          <Field label="조직" w={170}>
            <Select value={toSel(form.org_id == null ? '' : String(form.org_id))} onValueChange={(v: string) => setForm({ ...form, org_id: fromSel(v) ? Number(fromSel(v)) : null })}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value={NONE}>없음</SelectItem>
                {p.orgs.map(o => <SelectItem key={o.id} value={String(o.id)}>{o.name} ({o.code})</SelectItem>)}
              </SelectContent>
            </Select>
          </Field>
          <Field label={`감청 범위${scopeLocked ? ' (manager)' : ''}`} w={130}>
            <Select value={toSel(form.monitor_scope || 'none')} onValueChange={(v: string) => setForm({ ...form, monitor_scope: fromSel(v) as MonitorScope })} disabled={scopeLocked}>
              <SelectTrigger title="업무망 합법감청 — dialog 감시·Join 청취 범위. manager 만 변경"><SelectValue /></SelectTrigger>
              <SelectContent>
                {(Object.keys(SCOPE_LABEL) as MonitorScope[]).map(k => <SelectItem key={k} value={k}>{SCOPE_LABEL[k]}</SelectItem>)}
              </SelectContent>
            </Select>
          </Field>
          <Field label={`PTT 청취${scopeLocked ? ' (manager)' : ''}`} w={110}>
            <Select value={toSel(form.ptt_listen || 'none')} onValueChange={(v: string) => setForm({ ...form, ptt_listen: fromSel(v) as PttListen })} disabled={scopeLocked}>
              <SelectTrigger title="PTT 그룹콜 청취 범위 — 멤버는 allow_ambient_listening 자격도 필요"><SelectValue /></SelectTrigger>
              <SelectContent>
                {(Object.keys(PTT_LABEL) as PttListen[]).map(k => <SelectItem key={k} value={k}>{PTT_LABEL[k]}</SelectItem>)}
              </SelectContent>
            </Select>
          </Field>
          <Field label="청취 노출" w={110}>
            <Select value={toSel(form.listen_visibility || 'hidden')} onValueChange={(v: string) => setForm({ ...form, listen_visibility: fromSel(v) as DispatchGroup['listen_visibility'] })}>
              <SelectTrigger title="PTT 청취 멤버를 로스터에 보이는가"><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="hidden">은닉</SelectItem>
                <SelectItem value="visible">투명 (청취 중 표시)</SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <div className="flex gap-1.5 items-center">
            <Button variant="default" onClick={save}>저장</Button>
            <Button variant="ghost" onClick={() => isNew ? p.onClose() : setEditing(false)}>취소</Button>
          </div>
          <div className="basis-full text-xs text-muted-foreground">감청 범위: {SCOPE_HINT[form.monitor_scope || 'none']}</div>
        </FieldRow>
      ) : existing && (
        <div className="flex items-center gap-4 flex-wrap text-sm">
          <span className="text-sm text-muted-foreground">ID {existing.id}</span>
          <span className="text-sm text-muted-foreground">대표번호 {existing.pilot_id ? `${existing.pilot_id} (${existing.service_ref || '—'}, ${existing.alert_mode === 'parallel' ? '병렬' : '순차'} ${existing.no_answer_sec}s, 통화중 ${existing.busy_members === 'skip' ? '제외' : '호출'})` : '없음'}</span>
          <span className="text-sm text-muted-foreground">넘김 {existing.overflow_target || '—'}</span>
          <span className="text-sm text-muted-foreground">감청 {SCOPE_LABEL[existing.monitor_scope]}</span>
          <span className="text-sm text-muted-foreground">PTT 청취 {PTT_LABEL[existing.ptt_listen]}{existing.ptt_listen !== 'none' ? ` (${existing.listen_visibility === 'hidden' ? '은닉' : '투명'})` : ''}</span>
          {p.canWrite && <Button className="ml-auto" onClick={() => setEditing(true)}>속성 편집</Button>}
        </div>
      )}

      {existing && (existing.monitor_scope === 'listed' || existing.ptt_listen === 'listed') && (
        <div className="flex gap-4 flex-wrap">
          {existing.monitor_scope === 'listed' && (
            <TargetPicker title="감청 대상 그룹" icon={<Headphones size={12} />} canEdit={p.isManager}
              options={p.allGroups.filter(g => g.id !== existing.id).map(g => ({ value: g.id, label: `${g.name} (${g.id})` }))}
              value={existing.monitor_targets} onSave={saveMonitorTargets} />
          )}
          {existing.ptt_listen === 'listed' && (
            <TargetPicker title="PTT 청취 대상 그룹" icon={<Radio size={12} />} canEdit={p.isManager}
              options={p.pttGroups.map(g => ({ value: g.id, label: `${g.name} (${g.id})` }))}
              value={existing.ptt_targets} onSave={savePttTargets} />
          )}
        </div>
      )}

      {existing && (
        <MemberTransfer members={members} memberIds={memberIds} callIndex={p.callIndex} nameOf={p.nameOf} groupOfUser={p.groupOfUser}
          selfId={existing.id} canManage={p.canWrite && (!monitoring || p.isManager)} lockedReason={monitoring && !p.isManager ? '감청/청취 그룹 편입은 manager 승인 사항' : ''}
          orgScope={p.orgScope} orgPathOf={p.orgPathOf} onAdd={addMembers} onRemove={removeMembers} onSaveOrder={saveOrder} />
      )}
    </div>
  )
}

// ── 대상 그룹 다중 선택 (listed 범위) ──
function TargetPicker({ title, icon, options, value, canEdit, onSave }: {
  title: string; icon: React.ReactNode; options: Array<{ value: string; label: string }>; value: string[]
  canEdit: boolean; onSave: (ids: string[]) => void
}) {
  const [sel, setSel] = useState<Set<string>>(new Set(value))
  useEffect(() => { setSel(new Set(value)) }, [value])
  const dirty = sel.size !== value.length || value.some(v => !sel.has(v))
  return (
    <div className="flex-[1_1_280px] border border-border rounded-md py-2 px-2.5 bg-card">
      <div className="flex items-center gap-1.5 text-sm font-semibold mb-1.5">
        {icon} {title} <Badge  variant="neutralSoft">{sel.size}</Badge>
        {canEdit && dirty && <Button className="ml-auto" variant="default" onClick={() => onSave(Array.from(sel))}>저장</Button>}
      </div>
      <div className="flex max-h-[120px] flex-wrap gap-x-3 gap-y-1 overflow-y-auto text-sm">
        {options.length === 0 && <span className="text-sm text-muted-foreground">선택 가능한 그룹 없음</span>}
        {options.map(o => (
          <label className="inline-flex items-center gap-1" key={o.value}>
            <Checkbox  disabled={!canEdit} checked={sel.has(o.value)} onCheckedChange={() => setSel(s => { const n = new Set(s); if (n.has(o.value)) n.delete(o.value); else n.add(o.value); return n })} />
            {o.label}
          </label>
        ))}
      </div>
    </div>
  )
}

// ── 멤버 transfer (좌: 멤버(alert_order) ↔ 우: 조직트리 + VoLTE 가입자) ──
function MemberTransfer({ members, memberIds, callIndex, nameOf, groupOfUser, selfId, canManage, lockedReason, orgScope, orgPathOf, onAdd, onRemove, onSaveOrder }: {
  members: DispatchMember[]; memberIds: Set<string>; callIndex: PickItem[]; nameOf: Map<string, string>
  groupOfUser: Map<string, string>; selfId: string; canManage: boolean; lockedReason: string
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
      {lockedReason && <div className="text-xs text-muted-foreground mb-1.5">{lockedReason}</div>}
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
          <div className="flex items-center gap-2 border-b border-border px-2.5 py-2 text-sm font-semibold">VoLTE 가입자 <Badge  variant="neutralSoft">{candidates.length}</Badge></div>
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
                        {other && other !== selfId && <Badge  variant="warningSoft" title="다른 관제 그룹 소속 — 추가하면 이동(가입자당 그룹 하나)">{other}</Badge>}
                      </div>
                    )
                  })}
              </div>
            </div>
          </div>
        </div>
      </div>
      {canManage && <div className="text-xs text-muted-foreground mt-1.5">가입자 더블클릭 = 바로 추가 · 다른 그룹 소속 가입자는 추가 시 이동한다 · 반영은 다음 REGISTER 갱신부터</div>}
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
