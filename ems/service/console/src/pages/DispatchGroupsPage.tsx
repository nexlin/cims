import { useState, useEffect, useCallback, useMemo } from 'react'
import IconBtn from '@core/components/IconBtn'
import { Pencil, Trash2, ChevronRight, ChevronDown, ArrowLeft, ArrowRight, Radio, Headphones } from 'lucide-react'
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
  return <span style={{ color: 'var(--text-muted)', display: 'inline-flex' }}>
    {open ? <ChevronDown size={15} /> : <ChevronRight size={15} />}
  </span>
}

export default function DispatchGroupsPage() {
  const { show } = useToast()
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
    if (!confirm(`관제 그룹 "${g.name}" (${g.id}) 삭제?\n멤버 ${g.members.length}명의 픽업 그룹이 해제됩니다.`)) return
    try { await dispatchApi.delete(g.id); show('삭제', 'ok'); if (openId === g.id) setOpenId(null); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  const cols: Column<DispatchGroup>[] = [
    { key: 'exp', header: '', width: 26, render: g => <Caret open={openId === g.id} /> },
    { key: 'name', header: '그룹명', sortable: true, render: g => (
      <span><span style={{ fontWeight: 600 }}>{g.name}</span>
        {g.monitor_scope !== 'none' && <span className="badge badge--red" style={{ fontSize: 9, marginLeft: 4 }} title={`감청 범위: ${SCOPE_LABEL[g.monitor_scope]}`}>감청</span>}
        {g.ptt_listen !== 'none' && <span className="badge badge--yellow" style={{ fontSize: 9, marginLeft: 2 }} title={`PTT 청취: ${PTT_LABEL[g.ptt_listen]}`}>PTT청취</span>}
      </span>
    ) },
    { key: 'id', header: 'ID', width: 120, sortable: true, render: g => <span className="ts">{g.id}</span> },
    { key: 'pilot', header: '대표번호', width: 110, sortable: true, sortValue: g => g.pilot_id || '', render: g => g.pilot_id
      ? <span style={{ display: 'inline-flex', alignItems: 'center', gap: 4 }}><Radio size={12} style={{ color: 'var(--primary)' }} /><span className="ts">{g.pilot_id}</span></span>
      : <span className="ts" style={{ color: 'var(--text-muted)' }}>—</span> },
    { key: 'alert', header: '호출', width: 90, render: g => <span className="ts">{g.pilot_id ? (g.alert_mode === 'parallel' ? `병렬 ${g.no_answer_sec}s` : `순차 ${g.no_answer_sec}s`) : '—'}</span> },
    { key: 'overflow', header: '넘김', width: 110, render: g => <span className="ts">{g.overflow_target || '—'}</span> },
    { key: 'scope', header: '감청', width: 90, render: g => <span className="ts">{SCOPE_LABEL[g.monitor_scope]}</span> },
    { key: 'org', header: '조직', width: 130, render: g => <span className="ts">{orgs.find(o => o.id === g.org_id)?.name || '—'}</span> },
    { key: 'members', header: '멤버', width: 60, align: 'center', render: g => <span className="ts">{g.members.length}명</span> },
    { key: 'act', header: '', width: 84, align: 'right', render: g => canWrite ? (
      <span className="actions" onClick={e => e.stopPropagation()}>
        <IconBtn title="편집" onClick={() => toggleOpen(g.id)}><Pencil size={ICON} /></IconBtn>
        <IconBtn title="삭제" tone="danger" onClick={() => deleteGroup(g)}><Trash2 size={ICON} /></IconBtn>
      </span>
    ) : <span className="ts">—</span> },
  ]

  const openGroup = openId ? groups.find(g => g.id === openId) : undefined

  return (
    <div style={{ display: 'flex', gap: 16, alignItems: 'stretch', height: 'calc(100vh - 92px)' }}>
      <OrgTreePanel fill selectedPath={orgScope} onSelect={(p, n) => { setOrgScope(p); setOrgName(n) }}
        style={{ flex: '0 0 200px', width: 200, maxWidth: 200 }} />

      <div className="panel" style={{ flex: 1, minWidth: 0 }}>
        <div className="toolbar">
          <span style={{ fontWeight: 600, fontSize: 13 }}>{orgName}</span>
          <input className="search-input" placeholder="그룹명·ID·대표번호 검색" value={search}
            onChange={e => setSearch(e.target.value)} style={{ maxWidth: 220 }} />
          {search && <button className="btn btn--ghost btn--sm" onClick={() => setSearch('')}>✕</button>}
          <span style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
            {canWrite && !notMigrated && (
              <button className="btn btn--primary btn--sm" onClick={() => { setOpenId(null); setAdding(a => !a) }}>＋ 관제 그룹</button>
            )}
          </span>
        </div>

        {notMigrated && (
          <div style={{ padding: '10px 16px', fontSize: 12, color: 'var(--text-muted)', borderBottom: '1px solid var(--border)', background: 'var(--bg-soft)' }}>
            DB 에 <code>dispatch_groups</code> 테이블이 없습니다 — <code>sql/migrate_dispatch_groups.sql</code> 적용 후 사용할 수 있습니다.
            당겨받기는 가입자 <code>pickup_group</code> 축으로 계속 동작합니다.
          </div>
        )}

        {adding && (
          <div style={{ borderBottom: '1px solid var(--border)', background: 'var(--bg-soft)', padding: '12px 16px' }}>
            <div style={{ fontWeight: 600, fontSize: 12, color: 'var(--primary)', marginBottom: 8 }}>새 관제 그룹</div>
            <GroupDrawer mode="add" orgs={orgs} isManager={hasRole(me, 'manager')} canWrite={canWrite} allGroups={groups} pttGroups={pttGroups}
              callIndex={callIndex} nameOf={nameOf} groupOfUser={groupOfUser} orgScope={orgScope} orgPathOf={orgPathOf}
              onClose={() => setAdding(false)} onSaved={() => { setAdding(false); load() }} reload={load} />
          </div>
        )}

        <DataTable<DispatchGroup> columns={cols} rows={rows} rowKey={g => g.id} loading={loading}
          onRowClick={g => toggleOpen(g.id)} expandedKey={openId}
          renderExpanded={openGroup ? () => (
            <div style={{ padding: '12px 16px' }}>
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
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10, fontSize: 13 }}>
      {editing ? (
        <FieldRow>
          <Field label="그룹명 *" w={170}><input className="form-input" autoFocus value={form.name || ''} onChange={e => setForm({ ...form, name: e.target.value })} /></Field>
          <Field label="대표번호" w={120}><input className="form-input" placeholder="예: 7000" title="다이얼 가능한 주소 — 가입 번호와 겹치면 409" value={form.pilot_id || ''} onChange={e => setForm({ ...form, pilot_id: e.target.value.trim() })} /></Field>
          <Field label="접속서비스" w={110}><input className="form-input" placeholder="volte" title="대표번호가 속한 접속서비스 name — 도메인·SRTP 정책" value={form.service_ref || ''} onChange={e => setForm({ ...form, service_ref: e.target.value.trim() })} /></Field>
          <Field label="호출 방식" w={120}>
            <select className="form-input" value={form.alert_mode || 'parallel'} onChange={e => setForm({ ...form, alert_mode: e.target.value as DispatchGroup['alert_mode'] })}>
              <option value="parallel">병렬 (전원 동시)</option>
              <option value="sequential">순차 (후속)</option>
            </select>
          </Field>
          <Field label="무응답(초)" w={80}><input className="form-input" type="number" min={5} value={form.no_answer_sec ?? 30} onChange={e => setForm({ ...form, no_answer_sec: Number(e.target.value) })} /></Field>
          <Field label="통화 중 그룹원" w={120}>
            <select className="form-input" value={form.busy_members || 'skip'} onChange={e => setForm({ ...form, busy_members: e.target.value as DispatchGroup['busy_members'] })}>
              <option value="skip">호출 안 함</option>
              <option value="alert">호출 (통화대기)</option>
            </select>
          </Field>
          <Field label="무응답 넘김" w={130}><input className="form-input" placeholder="대표번호/내선" value={form.overflow_target || ''} onChange={e => setForm({ ...form, overflow_target: e.target.value.trim() })} /></Field>
          <Field label="조직" w={170}>
            <select className="form-input" value={form.org_id ?? ''} onChange={e => setForm({ ...form, org_id: e.target.value ? Number(e.target.value) : null })}>
              <option value="">없음</option>
              {p.orgs.map(o => <option key={o.id} value={o.id}>{o.name} ({o.code})</option>)}
            </select>
          </Field>
          <Field label={`감청 범위${scopeLocked ? ' (manager)' : ''}`} w={130}>
            <select className="form-input" disabled={scopeLocked} title="업무망 합법감청 — dialog 감시·Join 청취 범위. manager 만 변경" value={form.monitor_scope || 'none'} onChange={e => setForm({ ...form, monitor_scope: e.target.value as MonitorScope })}>
              {(Object.keys(SCOPE_LABEL) as MonitorScope[]).map(k => <option key={k} value={k}>{SCOPE_LABEL[k]}</option>)}
            </select>
          </Field>
          <Field label={`PTT 청취${scopeLocked ? ' (manager)' : ''}`} w={110}>
            <select className="form-input" disabled={scopeLocked} title="PTT 그룹콜 청취 범위 — 멤버는 allow_ambient_listening 자격도 필요" value={form.ptt_listen || 'none'} onChange={e => setForm({ ...form, ptt_listen: e.target.value as PttListen })}>
              {(Object.keys(PTT_LABEL) as PttListen[]).map(k => <option key={k} value={k}>{PTT_LABEL[k]}</option>)}
            </select>
          </Field>
          <Field label="청취 노출" w={110}>
            <select className="form-input" title="PTT 청취 멤버를 로스터에 보이는가" value={form.listen_visibility || 'hidden'} onChange={e => setForm({ ...form, listen_visibility: e.target.value as DispatchGroup['listen_visibility'] })}>
              <option value="hidden">은닉</option>
              <option value="visible">투명 (청취 중 표시)</option>
            </select>
          </Field>
          <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
            <button className="btn btn--sm btn--primary" onClick={save}>저장</button>
            <button className="btn btn--sm btn--ghost" onClick={() => isNew ? p.onClose() : setEditing(false)}>취소</button>
          </div>
          <div style={{ flexBasis: '100%', fontSize: 11, color: 'var(--text-muted)' }}>감청 범위: {SCOPE_HINT[form.monitor_scope || 'none']}</div>
        </FieldRow>
      ) : existing && (
        <div style={{ display: 'flex', alignItems: 'center', gap: 16, flexWrap: 'wrap', fontSize: 12 }}>
          <span className="ts">ID {existing.id}</span>
          <span className="ts">대표번호 {existing.pilot_id ? `${existing.pilot_id} (${existing.service_ref || '—'}, ${existing.alert_mode === 'parallel' ? '병렬' : '순차'} ${existing.no_answer_sec}s, 통화중 ${existing.busy_members === 'skip' ? '제외' : '호출'})` : '없음'}</span>
          <span className="ts">넘김 {existing.overflow_target || '—'}</span>
          <span className="ts">감청 {SCOPE_LABEL[existing.monitor_scope]}</span>
          <span className="ts">PTT 청취 {PTT_LABEL[existing.ptt_listen]}{existing.ptt_listen !== 'none' ? ` (${existing.listen_visibility === 'hidden' ? '은닉' : '투명'})` : ''}</span>
          {p.canWrite && <button className="btn btn--sm btn--outline" style={{ marginLeft: 'auto' }} onClick={() => setEditing(true)}>속성 편집</button>}
        </div>
      )}

      {existing && (existing.monitor_scope === 'listed' || existing.ptt_listen === 'listed') && (
        <div style={{ display: 'flex', gap: 16, flexWrap: 'wrap' }}>
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
    <div style={{ flex: '1 1 280px', border: '1px solid var(--border)', borderRadius: 10, padding: '8px 10px', background: 'var(--surface)' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 12, fontWeight: 600, marginBottom: 6 }}>
        {icon} {title} <span className="badge badge--gray" style={{ fontSize: 10 }}>{sel.size}</span>
        {canEdit && dirty && <button className="btn btn--sm btn--primary" style={{ marginLeft: 'auto' }} onClick={() => onSave(Array.from(sel))}>저장</button>}
      </div>
      <div style={{ display: 'flex', flexWrap: 'wrap', gap: '4px 12px', fontSize: 12, maxHeight: 120, overflowY: 'auto' }}>
        {options.length === 0 && <span className="ts" style={{ color: 'var(--text-muted)' }}>선택 가능한 그룹 없음</span>}
        {options.map(o => (
          <label key={o.value} style={{ display: 'inline-flex', alignItems: 'center', gap: 4 }}>
            <input type="checkbox" disabled={!canEdit} checked={sel.has(o.value)}
              onChange={() => setSel(s => { const n = new Set(s); if (n.has(o.value)) n.delete(o.value); else n.add(o.value); return n })} />
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

  const panelHead: React.CSSProperties = { display: 'flex', alignItems: 'center', gap: 8, padding: '8px 10px', borderBottom: '1px solid var(--border)', fontSize: 12, fontWeight: 600 }
  const panel: React.CSSProperties = { display: 'flex', flexDirection: 'column', minWidth: 0, border: '1px solid var(--border)', borderRadius: 10, overflow: 'hidden', background: 'var(--surface)' }

  return (
    <div style={{ marginTop: 10, borderTop: '1px solid var(--border)', paddingTop: 12 }}>
      {lockedReason && <div style={{ fontSize: 11, color: 'var(--text-muted)', marginBottom: 6 }}>{lockedReason}</div>}
      <div style={{ display: 'flex', alignItems: 'stretch', gap: 10, height: 320 }}>
        <div style={{ ...panel, flex: 1 }}>
          <div style={panelHead}>멤버 <span className="badge badge--blue" style={{ fontSize: 10 }}>{members.length}</span>
            <span style={{ marginLeft: 'auto', fontWeight: 400, color: 'var(--text-muted)', fontSize: 11 }}>순서 = 순차 호출·포크 상한 절삭 순</span></div>
          <div style={{ flex: 1, minHeight: 0, overflowY: 'auto' }}>
            {members.length === 0
              ? <div className="ts" style={{ padding: 14, fontSize: 12, textAlign: 'center', color: 'var(--text-muted)' }}>멤버 없음<br />우측에서 가입자를 선택해 <ArrowLeft size={11} style={{ verticalAlign: '-1px' }} /> 추가</div>
              : members.map(m => (
                <div key={m.user_id} style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '6px 10px', fontSize: 12,
                  borderLeft: selMembers.has(m.user_id) ? '3px solid var(--primary)' : '3px solid transparent' }}>
                  {canManage && <input type="checkbox" checked={selMembers.has(m.user_id)} onChange={() => toggle(setSelMembers, m.user_id)} />}
                  <span style={{ display: 'flex', flexDirection: 'column', minWidth: 0, flex: 1 }}>
                    <span style={{ fontWeight: 600, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{nameOf.get(m.user_id) || '—'}</span>
                    <span className="ts" style={{ color: 'var(--text-muted)', fontSize: 11 }}>{m.user_id}</span>
                  </span>
                  <input className="form-input" type="number" title="alert_order" disabled={!canManage} value={m.alert_order} style={{ width: 54 }}
                    onChange={e => onSaveOrder(m.user_id, Number(e.target.value))} />
                  {canManage && <IconBtn title="제거" tone="danger" onClick={() => doRemove([m.user_id])}><ArrowRight size={ICON} /></IconBtn>}
                </div>
              ))}
          </div>
        </div>

        {canManage && (
          <div style={{ display: 'flex', flexDirection: 'column', justifyContent: 'center', gap: 10, alignSelf: 'center' }}>
            <button className="btn btn--primary btn--sm" disabled={busy || picked.size === 0} onClick={() => doAdd(Array.from(picked))}
              style={{ display: 'inline-flex', alignItems: 'center', gap: 4, whiteSpace: 'nowrap' }}><ArrowLeft size={14} /> 추가{picked.size ? ` ${picked.size}` : ''}</button>
            <button className="btn btn--outline btn--sm" disabled={busy || selMembers.size === 0} onClick={() => doRemove(Array.from(selMembers))}
              style={{ display: 'inline-flex', alignItems: 'center', gap: 4, whiteSpace: 'nowrap' }}>제거{selMembers.size ? ` ${selMembers.size}` : ''} <ArrowRight size={14} /></button>
          </div>
        )}

        <div style={{ ...panel, flex: 1.3 }}>
          <div style={panelHead}>VoLTE 가입자 <span className="badge badge--gray" style={{ fontSize: 10 }}>{candidates.length}</span></div>
          <div style={{ display: 'flex', flex: 1, minHeight: 0 }}>
            <OrgTreePanel fill selectedPath={treeScope} onSelect={(pth, n) => { setTreeScope(pth); setTreeName(n) }}
              style={{ flex: '0 0 150px', width: 150, maxWidth: 150, border: 'none', borderRight: '1px solid var(--border)', borderRadius: 0 }} />
            <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}>
              <div style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '6px 8px', borderBottom: '1px solid var(--border)' }}>
                <input className="search-input" placeholder={`${treeName} 내 검색`} value={q} onChange={e => setQ(e.target.value)} style={{ flex: 1, fontSize: 12 }} />
              </div>
              <div style={{ flex: 1, minHeight: 0, overflowY: 'auto' }}>
                {candidates.length === 0
                  ? <div className="ts" style={{ padding: 14, fontSize: 12, textAlign: 'center', color: 'var(--text-muted)' }}>{callIndex.length ? `${treeName}에 추가할 가입자 없음` : '불러오는 중...'}</div>
                  : candidates.map(c => {
                    const other = groupOfUser.get(c.value)
                    return (
                      <div key={c.value} onDoubleClick={() => canManage && doAdd([c.value])} onClick={() => canManage && toggle(setPicked, c.value)}
                        style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '6px 10px', fontSize: 12, cursor: 'pointer',
                          borderLeft: picked.has(c.value) ? '3px solid var(--primary)' : '3px solid transparent' }}>
                        <input type="checkbox" checked={picked.has(c.value)} readOnly tabIndex={-1} />
                        <span style={{ display: 'flex', flexDirection: 'column', minWidth: 0, flex: 1 }}>
                          <span style={{ fontWeight: 600, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{c.userName}</span>
                          <span className="ts" style={{ color: 'var(--text-muted)', fontSize: 11 }}>{c.value}{c.orgCode ? ` · ${c.orgCode}` : ''}</span>
                        </span>
                        {other && other !== selfId && <span className="badge badge--yellow" style={{ fontSize: 9 }} title="다른 관제 그룹 소속 — 추가하면 이동(가입자당 그룹 하나)">{other}</span>}
                      </div>
                    )
                  })}
              </div>
            </div>
          </div>
        </div>
      </div>
      {canManage && <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 6 }}>가입자 더블클릭 = 바로 추가 · 다른 그룹 소속 가입자는 추가 시 이동한다 · 반영은 다음 REGISTER 갱신부터</div>}
    </div>
  )
}

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
