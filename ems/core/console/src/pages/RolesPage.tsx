import { useConfirm } from '@core/components/custom/confirm'
import { useState, useEffect, useCallback, useMemo } from 'react'
import IconBtn from '@core/components/IconBtn'
import { ChevronDown, ChevronRight, Headphones, Pencil, Plus, Radio as RadioIcon, Trash2, UserMinus, X } from 'lucide-react'
import { rolesApi, ROLE_ERRORS, ROLE_PRESET_LABELS, type RoleDef, type RoleInput, type RolePreset, type RoleAssignment,
  type MonitorCall, type PttListen, type ListenVisibility, type DirectoryScope, type PttGroupManage, type HistoryRead } from '@core/api/roles'
import { phoneGroupsApi, type PhoneGroup } from '@core/api/phoneGroups'
import { groupsApi, type Group } from '@core/api/groups'
import { usersApi, type UserSummary } from '@core/api/users'
import { orgApi, type Organization } from '@core/api/organizations'
import { consoleAccountsApi, type ConsoleAccount } from '@core/api/consoleAccounts'
import { ApiError } from '@core/api/client'
import { DataTable, type Column } from '@core/components/DataTable'
import SubscriberPicker, { buildPickIndex, type PickItem } from '@core/components/SubscriberPicker'
import { useToast } from '@core/components/Toast'
import { useAuth } from '@core/contexts/AuthContext'
import { hasRole, ROLE_LABELS } from '@core/utils/permissions'
import type { Role } from '@core/api/auth'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { NONE, fromSel, toSel } from '@core/components/custom/select-value'
import { Badge } from '@core/components/ui/badge'
import { Checkbox } from '@core/components/ui/checkbox'
import { Radio } from '@core/components/custom/radio'
import { Alert, AlertDescription } from '@core/components/ui/alert'

// ── 역할 (mcptt_authorization.md §2.2·§3 · dispatch_center.md §3.3) ─────────────
//  역할 = 능력 + 범위 한 행. 내장 프리셋 4행(admin/manager/operator/monitor)은 읽기 전용이고, 관제 프리셋
//  (감독/관리/전체)은 생성 시 초깃값이다 — 저장은 개별 필드라 프리셋 밖 조합도 막지 않는다.
//  사람은 역할 하나에 배정된다: 가입자(person)는 PUT /roles/{id}/assignments, 콘솔 계정은 OAM
//  PUT /console-accounts/{login_id} role (= roles.id). 청취 자격(allow_ambient_listening)은 배정의 결과로
//  CSC 가 동기한다(§2.4). 역할·범위·배정 변경은 전부 authz_manage(콘솔 manager 이상) — 커스텀 역할에는
//  authz_manage/audit_read/alarm_ack/mcptt_control 을 켤 수 없다(400 not_delegable).

const ICON = 14

const MONITOR_LABEL: Record<MonitorCall, string> = { none: '없음', own: '자기 전화 그룹', listed: '지정 전화 그룹', all: '전체' }
const MONITOR_HINT: Record<MonitorCall, string> = {
  none: '통화 감청 불가',
  own: '배정자가 속한 전화 그룹원의 통화만 dialog 감시·Join 청취',
  listed: '아래 대상 전화 그룹의 통화를 dialog 감시·Join 청취',
  all: '모든 가입자의 통화를 dialog 감시·Join 청취 (업무망 합법감청 — 운영 규약·감사 전제)',
}
const PTT_LABEL: Record<PttListen, string> = { none: '없음', listed: '지정 그룹', all: '전체' }
const VIS_LABEL: Record<ListenVisibility, string> = { hidden: '은닉', visible: '투명 (청취 중 표시)' }
const DIR_LABEL: Record<DirectoryScope, string> = { none: '없음', own: '소속 조직 하위', all: '전체 조직' }
const PGM_LABEL: Record<PttGroupManage, string> = { none: '없음', own: '본인 소유', scope: '관리 범위 안', all: '전체' }
const HIST_LABEL: Record<HistoryRead, string> = { none: '없음', scope: '범위 안', all: '전체' }

// 관제 프리셋 초깃값 — mcptt_authorization.md §3 표 (감독=감청·청취 / 관리=조직·구성원·PTT 그룹 / 전체=둘 다)
const PRESET_FIELDS: Record<RolePreset, Required<Omit<RoleInput, 'id' | 'name' | 'org_id' | 'preset'>>> = {
  supervisor: { directory_write: 'none', directory_read: 'none', ptt_group_manage: 'none',  monitor_call: 'own',  ptt_listen: 'listed', listen_visibility: 'hidden', history_read: 'scope' },
  admin:      { directory_write: 'own',  directory_read: 'none', ptt_group_manage: 'scope', monitor_call: 'none', ptt_listen: 'none',   listen_visibility: 'hidden', history_read: 'none' },
  full:       { directory_write: 'own',  directory_read: 'none', ptt_group_manage: 'scope', monitor_call: 'own',  ptt_listen: 'listed', listen_visibility: 'hidden', history_read: 'scope' },
}
const PRESET_HINT: Record<RolePreset, string> = {
  supervisor: '감청·PTT 청취·범위 안 이력/녹취 — 조직·번호 관리 없음',
  admin: '조직·구성원·번호·전화 그룹·PTT 그룹 관리(소속 조직 하위) — 감청·청취 없음',
  full: '감독 + 관리',
}

// 오류 토큰 → 문구 (admin_api.md §6.8). 토큰이 아니면 서버 detail/코드 그대로.
function errText(e: unknown): string {
  if (e instanceof ApiError) {
    const code = typeof e.data.error === 'string' ? e.data.error : ''
    if (code && ROLE_ERRORS[code]) return ROLE_ERRORS[code]
  }
  return String(e)
}

// 내장 역할 표시명 = 콘솔 등급 라벨(permissions.ts), 커스텀 = name
function roleName(r: RoleDef): string {
  return r.builtin ? (ROLE_LABELS[r.id as Role] ?? (r.name || r.id)) : (r.name || r.id)
}

function Caret({ open }: { open: boolean }) {
  return <span className="text-muted-foreground inline-flex">
    {open ? <ChevronDown size={15} /> : <ChevronRight size={15} />}
  </span>
}

export default function RolesPage() {
  const { show } = useToast()
  const confirm = useConfirm()
  const { user: me } = useAuth()
  const canManage = hasRole(me, 'manager')   // authz_manage — 내장 admin/manager
  const isAdmin = hasRole(me, 'admin')       // OAM 콘솔 계정 API(/console-accounts)는 admin 게이트

  const [search, setSearch] = useState('')
  const [roles, setRoles] = useState<RoleDef[]>([])
  const [notMigrated, setNotMigrated] = useState(false)
  const [loadErr, setLoadErr] = useState('')
  const [loading, setLoading] = useState(true)
  const [phoneGroups, setPhoneGroups] = useState<PhoneGroup[]>([])
  const [pttGroups, setPttGroups] = useState<Group[]>([])
  const [users, setUsers] = useState<UserSummary[]>([])
  const [orgs, setOrgs] = useState<Organization[]>([])
  const [accounts, setAccounts] = useState<ConsoleAccount[]>([])
  const [openId, setOpenId] = useState<string | null>(null)
  const [adding, setAdding] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const r = await rolesApi.list()
      setRoles(r.roles); setNotMigrated(r.schema === 'not_migrated'); setLoadErr('')
    } catch (e: unknown) { setLoadErr(errText(e)) }
    finally { setLoading(false) }
    // 대상·배정 후보 — 하나가 없어도 화면은 뜬다 (콘솔 계정 API 는 admin 게이트라 manager 는 빈 목록)
    const [pg, g, u, o, a] = await Promise.all([
      phoneGroupsApi.list().then(x => x.groups).catch(() => [] as PhoneGroup[]),
      groupsApi.list().catch(() => [] as Group[]),
      usersApi.list().catch(() => [] as UserSummary[]),
      orgApi.list().catch(() => [] as Organization[]),
      consoleAccountsApi.list().catch(() => [] as ConsoleAccount[]),
    ])
    setPhoneGroups(pg); setPttGroups(g); setUsers(u); setOrgs(o); setAccounts(a)
  }, [])
  useEffect(() => { load() }, [load])

  const rows = useMemo(() => {
    const q = search.trim().toLowerCase()
    return roles.filter(r => !q || r.id.toLowerCase().includes(q) || roleName(r).toLowerCase().includes(q))
  }, [roles, search])

  function toggleOpen(id: string) { setAdding(false); setOpenId(cur => cur === id ? null : id) }

  async function deleteRole(r: RoleDef) {
    if (!await confirm({ title: '역할 삭제', tone: 'danger', confirmLabel: '삭제', body: <>
      역할 "{roleName(r)}" ({r.id}) 삭제?
      <div className="mt-1">배정이 남아 있으면 삭제되지 않습니다 (409) — 배정을 먼저 해제하세요.</div>
    </> })) return
    try { await rolesApi.delete(r.id); show('삭제', 'ok'); if (openId === r.id) setOpenId(null); load() }
    catch (e: unknown) { show(errText(e), 'err') }
  }

  const cols: Column<RoleDef>[] = [
    { key: 'exp', header: '', width: 26, render: r => <Caret open={openId === r.id} /> },
    { key: 'name', header: '역할', sortable: true, sortValue: r => roleName(r), render: r => (
      <span><span className="font-semibold">{roleName(r)}</span>
        {r.builtin && <Badge className="ml-1" variant="neutralSoft" title="내장 프리셋 — 읽기 전용">내장</Badge>}
        {r.authz_manage && <Badge className="ml-0.5" variant="brandSoft" title="역할·배정·범위 관리 (authz_manage)">권한 관리</Badge>}
      </span>
    ) },
    { key: 'id', header: 'ID', width: 130, sortable: true, render: r => <span className="text-sm text-muted-foreground">{r.id}</span> },
    { key: 'monitor', header: '감청', width: 110, render: r => <span className="text-sm text-muted-foreground">{MONITOR_LABEL[r.monitor_call]}</span> },
    { key: 'ptt', header: 'PTT 청취', width: 110, render: r => <span className="text-sm text-muted-foreground">{PTT_LABEL[r.ptt_listen]}{r.ptt_listen !== 'none' ? ` (${r.listen_visibility === 'hidden' ? '은닉' : '투명'})` : ''}</span> },
    { key: 'dir', header: '관리 범위', width: 110, render: r => <span className="text-sm text-muted-foreground">{DIR_LABEL[r.directory_write]}</span> },
    { key: 'hist', header: '이력', width: 80, render: r => <span className="text-sm text-muted-foreground">{HIST_LABEL[r.history_read]}</span> },
    { key: 'org', header: '조직', width: 130, render: r => <span className="text-sm text-muted-foreground">{orgs.find(o => o.id === r.org_id)?.name || '—'}</span> },
    { key: 'assigned', header: '배정', width: 60, align: 'center', render: r => <span className="text-sm text-muted-foreground">{r.assignment_count != null ? `${r.assignment_count}명` : '—'}</span> },
    { key: 'act', header: '', width: 84, align: 'right', render: r => canManage && !r.builtin ? (
      <span className="flex gap-1.5" onClick={e => e.stopPropagation()}>
        <IconBtn title="편집" onClick={() => toggleOpen(r.id)}><Pencil size={ICON} /></IconBtn>
        <IconBtn title="삭제" tone="danger" onClick={() => deleteRole(r)}><Trash2 size={ICON} /></IconBtn>
      </span>
    ) : <span className="text-sm text-muted-foreground">—</span> },
  ]

  const openRole = openId ? roles.find(r => r.id === openId) : undefined
  const drawerCtx = { orgs, phoneGroups, pttGroups, users, accounts, canManage, isAdmin }

  return (
    <div className="flex gap-4 items-stretch flex-1 min-h-0">
      <div className="panel flex flex-1 flex-col overflow-hidden rounded-md border border-border bg-card flex-1 min-w-0">
        <div className="toolbar flex items-center gap-2.5 border-b border-border bg-muted px-4 py-3">
          <span className="font-semibold text-md">역할</span>
          <Input className="flex-1 max-w-[220px]" placeholder="역할명·ID 검색" value={search}
            onChange={e => setSearch(e.target.value)}/>
          {search && <Button variant="ghost" onClick={() => setSearch('')}
        aria-label="검색어 지우기"><X size={13} /></Button>}
          <span className="ml-auto flex gap-1.5">
            {canManage && !notMigrated && !loadErr && (
              <Button variant="default" onClick={() => { setOpenId(null); setAdding(a => !a) }}><Plus size={13} /> 역할</Button>
            )}
          </span>
        </div>

        {loadErr && (
          <Alert variant="danger" className="m-4 mb-0">
            <AlertDescription>역할 목록을 불러오지 못했습니다 — {loadErr}</AlertDescription>
          </Alert>
        )}

        {notMigrated && (
          <div className="py-2.5 px-4 text-sm text-muted-foreground border-b border-border bg-muted">
            DB 에 <code>roles</code> 테이블이 없습니다 — <code>sql/migrate_phone_groups_roles.sql</code> 적용 후 사용할 수 있습니다.
            콘솔 계정은 내장 4등급(admin/manager/operator/monitor)으로 계속 동작합니다.
          </div>
        )}

        {adding && (
          <div className="border-b border-border bg-muted py-3 px-4">
            <div className="font-semibold text-sm text-primary mb-2">새 역할</div>
            <RoleDrawer mode="add" {...drawerCtx}
              onClose={() => setAdding(false)} onSaved={() => { setAdding(false); load() }} reload={load} />
          </div>
        )}

        <DataTable<RoleDef> columns={cols} rows={rows} rowKey={r => r.id} loading={loading}
          onRowClick={r => toggleOpen(r.id)} expandedKey={openId}
          renderExpanded={openRole ? () => (
            <div className="py-3 px-4">
              <RoleDrawer key={openRole.id} mode="view" role={openRole} {...drawerCtx}
                onClose={() => setOpenId(null)} onSaved={() => { setOpenId(null); load() }} reload={load} />
            </div>
          ) : undefined}
          pageSize={50} emptyText={notMigrated ? '마이그레이션 전' : loadErr ? '조회 실패' : '역할 없음'} />
      </div>
    </div>
  )
}

// ════════════════════════════════════════════════════════════
//  드로어 — 능력·범위 + 대상(전화 그룹·PTT 그룹) + 배정(가입자·콘솔 계정)
// ════════════════════════════════════════════════════════════
interface DrawerProps {
  mode: 'view' | 'add'
  role?: RoleDef
  orgs: Organization[]
  phoneGroups: PhoneGroup[]
  pttGroups: Group[]
  users: UserSummary[]
  accounts: ConsoleAccount[]
  canManage: boolean
  isAdmin: boolean
  onClose: () => void
  onSaved: () => void
  reload: () => void
}

type RoleForm = Required<Omit<RoleInput, 'id' | 'preset'>>

function RoleDrawer(p: DrawerProps) {
  const { show } = useToast()
  const existing = p.role
  const isNew = p.mode === 'add'
  const readOnly = !p.canManage || !!existing?.builtin
  const [editing, setEditing] = useState(isNew)
  const [preset, setPreset] = useState<RolePreset>('supervisor')
  const [form, setForm] = useState<RoleForm>(() => existing
    ? { name: existing.name, directory_write: existing.directory_write, directory_read: existing.directory_read,
        ptt_group_manage: existing.ptt_group_manage, monitor_call: existing.monitor_call, ptt_listen: existing.ptt_listen,
        listen_visibility: existing.listen_visibility, history_read: existing.history_read, org_id: existing.org_id }
    : { name: '', org_id: null, ...PRESET_FIELDS.supervisor })

  function applyPreset(k: RolePreset) {
    setPreset(k)
    setForm(f => ({ ...f, ...PRESET_FIELDS[k] }))
  }

  async function save() {
    if (!form.name.trim()) { show('역할명 필수', 'err'); return }
    if (form.directory_write === 'own' && form.org_id == null) { show('관리 범위 "소속 조직 하위"에는 조직이 필요합니다', 'err'); return }
    const body: RoleInput = { ...form, name: form.name.trim() }
    try {
      if (existing) { await rolesApi.update(existing.id, body); show('저장', 'ok'); setEditing(false); p.reload() }
      else { await rolesApi.create({ ...body, preset }); show('생성', 'ok'); p.onSaved() }
    } catch (e: unknown) { show(errText(e), 'err') }
  }
  async function saveMonitorTargets(ids: string[]) {
    if (!existing) return
    try { await rolesApi.setMonitorTargets(existing.id, ids); show('감청 대상 저장', 'ok'); p.reload() }
    catch (e: unknown) { show(errText(e), 'err') }
  }
  async function savePttTargets(ids: string[]) {
    if (!existing) return
    try { await rolesApi.setPttTargets(existing.id, ids); show('청취 대상 저장', 'ok'); p.reload() }
    catch (e: unknown) { show(errText(e), 'err') }
  }

  return (
    <div className="flex flex-col gap-2.5 text-md">
      {editing ? (
        <FieldRow>
          {isNew && (
            <div className="basis-full flex items-center gap-4 flex-wrap text-sm">
              <span className="text-xs text-muted-foreground">프리셋</span>
              {(Object.keys(ROLE_PRESET_LABELS) as RolePreset[]).map(k => (
                <label key={k} className="flex cursor-pointer select-none items-center gap-1.5" title={PRESET_HINT[k]}>
                  <Radio name="role-preset" checked={preset === k} onChange={() => applyPreset(k)} />
                  {ROLE_PRESET_LABELS[k]}
                </label>
              ))}
              <span className="text-xs text-muted-foreground">{PRESET_HINT[preset]} — 아래 필드를 채운다. 저장은 개별 필드</span>
            </div>
          )}
          <Field label="역할명 *" w={170}><Input  autoFocus value={form.name} onChange={e => setForm({ ...form, name: e.target.value })} /></Field>
          <Field label="감청 범위" w={140}>
            <Select value={toSel(form.monitor_call)} onValueChange={(v: string) => setForm({ ...form, monitor_call: fromSel(v) as MonitorCall })}>
              <SelectTrigger title="업무망 합법감청 — dialog 감시·Join 청취·통화 이력/녹취 범위 (전화 그룹 단위)"><SelectValue /></SelectTrigger>
              <SelectContent>
                {(Object.keys(MONITOR_LABEL) as MonitorCall[]).map(k => <SelectItem key={k} value={k}>{MONITOR_LABEL[k]}</SelectItem>)}
              </SelectContent>
            </Select>
          </Field>
          <Field label="PTT 청취" w={110}>
            <Select value={toSel(form.ptt_listen)} onValueChange={(v: string) => setForm({ ...form, ptt_listen: fromSel(v) as PttListen })}>
              <SelectTrigger title="PTT 그룹콜 청취·conference 구독·PTT 이력/녹취 범위 — 배정자의 allow_ambient_listening 을 CSC 가 동기"><SelectValue /></SelectTrigger>
              <SelectContent>
                {(Object.keys(PTT_LABEL) as PttListen[]).map(k => <SelectItem key={k} value={k}>{PTT_LABEL[k]}</SelectItem>)}
              </SelectContent>
            </Select>
          </Field>
          <Field label="청취 노출" w={140}>
            <Select value={toSel(form.listen_visibility)} onValueChange={(v: string) => setForm({ ...form, listen_visibility: fromSel(v) as ListenVisibility })}>
              <SelectTrigger title="PTT 청취 멤버를 로스터에 보이는가"><SelectValue /></SelectTrigger>
              <SelectContent>
                {(Object.keys(VIS_LABEL) as ListenVisibility[]).map(k => <SelectItem key={k} value={k}>{VIS_LABEL[k]}</SelectItem>)}
              </SelectContent>
            </Select>
          </Field>
          <Field label="관리 범위" w={130}>
            <Select value={toSel(form.directory_write)} onValueChange={(v: string) => setForm({ ...form, directory_write: fromSel(v) as DirectoryScope })}>
              <SelectTrigger title="조직·구성원·VoLTE/PTT 번호·전화 그룹 쓰기 범위 (own = 조직과 그 하위)"><SelectValue /></SelectTrigger>
              <SelectContent>
                {(Object.keys(DIR_LABEL) as DirectoryScope[]).map(k => <SelectItem key={k} value={k}>{DIR_LABEL[k]}</SelectItem>)}
              </SelectContent>
            </Select>
          </Field>
          <Field label="조회 범위" w={130}>
            <Select value={toSel(form.directory_read)} onValueChange={(v: string) => setForm({ ...form, directory_read: fromSel(v) as DirectoryScope })}>
              <SelectTrigger title="같은 자원의 콘솔 조회 범위 — 관제 앱 전화번호부는 provisioning scope 라 별개"><SelectValue /></SelectTrigger>
              <SelectContent>
                {(Object.keys(DIR_LABEL) as DirectoryScope[]).map(k => <SelectItem key={k} value={k}>{DIR_LABEL[k]}</SelectItem>)}
              </SelectContent>
            </Select>
          </Field>
          <Field label="PTT 그룹 관리" w={130}>
            <Select value={toSel(form.ptt_group_manage)} onValueChange={(v: string) => setForm({ ...form, ptt_group_manage: fromSel(v) as PttGroupManage })}>
              <SelectTrigger title="PTT 그룹 CRUD — own=본인 소유(가입자만 성립), scope=관리 범위 안 org_code"><SelectValue /></SelectTrigger>
              <SelectContent>
                {(Object.keys(PGM_LABEL) as PttGroupManage[]).map(k => <SelectItem key={k} value={k}>{PGM_LABEL[k]}</SelectItem>)}
              </SelectContent>
            </Select>
          </Field>
          <Field label="이력 열람" w={110}>
            <Select value={toSel(form.history_read)} onValueChange={(v: string) => setForm({ ...form, history_read: fromSel(v) as HistoryRead })}>
              <SelectTrigger title="이력·녹취 열람 — scope=감청/청취 범위 안, all=전역"><SelectValue /></SelectTrigger>
              <SelectContent>
                {(Object.keys(HIST_LABEL) as HistoryRead[]).map(k => <SelectItem key={k} value={k}>{HIST_LABEL[k]}</SelectItem>)}
              </SelectContent>
            </Select>
          </Field>
          <Field label="조직" w={170}>
            <Select value={toSel(form.org_id == null ? '' : String(form.org_id))} onValueChange={(v: string) => setForm({ ...form, org_id: fromSel(v) ? Number(fromSel(v)) : null })}>
              <SelectTrigger title="own 범위의 루트 조직"><SelectValue /></SelectTrigger>
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
          <div className="basis-full text-xs text-muted-foreground">감청 범위: {MONITOR_HINT[form.monitor_call]} · 권한 관리·감사 열람·알람 ack·MCPTT 관제는 내장 역할 전용(커스텀에 켤 수 없음)</div>
        </FieldRow>
      ) : existing && (
        <div className="flex items-center gap-4 flex-wrap text-sm">
          <span className="text-sm text-muted-foreground">ID {existing.id}</span>
          <span className="text-sm text-muted-foreground">감청 {MONITOR_LABEL[existing.monitor_call]}</span>
          <span className="text-sm text-muted-foreground">PTT 청취 {PTT_LABEL[existing.ptt_listen]}{existing.ptt_listen !== 'none' ? ` (${existing.listen_visibility === 'hidden' ? '은닉' : '투명'})` : ''}</span>
          <span className="text-sm text-muted-foreground">관리 {DIR_LABEL[existing.directory_write]} · 조회 {DIR_LABEL[existing.directory_read]}</span>
          <span className="text-sm text-muted-foreground">PTT 그룹 관리 {PGM_LABEL[existing.ptt_group_manage]}</span>
          <span className="text-sm text-muted-foreground">이력 {HIST_LABEL[existing.history_read]}</span>
          <CapBadges role={existing} />
          {existing.builtin
            ? <span className="ml-auto text-xs text-muted-foreground">내장 프리셋 — 읽기 전용</span>
            : p.canManage && <Button className="ml-auto" onClick={() => setEditing(true)}>속성 편집</Button>}
        </div>
      )}

      {existing && (existing.monitor_call === 'listed' || existing.ptt_listen === 'listed') && (
        <div className="flex gap-4 flex-wrap">
          {existing.monitor_call === 'listed' && (
            <TargetPicker key={`m:${(existing.monitor_targets || []).join(',')}`} title="감청 대상 전화 그룹" icon={<Headphones size={12} />} canEdit={!readOnly}
              options={p.phoneGroups.map(g => ({ value: g.id, label: `${g.name} (${g.id})` }))}
              value={existing.monitor_targets || []} onSave={saveMonitorTargets} />
          )}
          {existing.ptt_listen === 'listed' && (
            <TargetPicker key={`p:${(existing.ptt_targets || []).join(',')}`} title="PTT 청취 대상 그룹" icon={<RadioIcon size={12} />} canEdit={!readOnly}
              options={p.pttGroups.map(g => ({ value: g.id, label: `${g.name} (${g.id})` }))}
              value={existing.ptt_targets || []} onSave={savePttTargets} />
          )}
        </div>
      )}

      {existing && (
        <AssignmentsPanel role={existing} users={p.users} accounts={p.accounts} canManage={p.canManage} isAdmin={p.isAdmin} reload={p.reload} />
      )}
    </div>
  )
}

// 내장 역할 전용 능력 4종 — 커스텀에는 켤 수 없어 편집 폼에 없고 읽기 전용으로만 보인다
function CapBadges({ role }: { role: RoleDef }) {
  const caps: Array<[boolean, string, string]> = [
    [role.authz_manage, '권한 관리', 'authz_manage — 역할·배정·범위 관리'],
    [role.audit_read, '감사 열람', 'audit_read — E-AUD 감사 이벤트 열람'],
    [role.alarm_ack, '알람 ack', 'alarm_ack'],
    [role.mcptt_control, 'MCPTT 관제', 'mcptt_control — floor·긴급 조작'],
  ]
  const on = caps.filter(c => c[0])
  if (!on.length) return null
  return <span className="inline-flex items-center gap-1">
    {on.map(([, label, title]) => <Badge key={label} variant="brandSoft" title={title}>{label}</Badge>)}
  </span>
}

// ── 대상 그룹 다중 선택 (listed 범위) ──
function TargetPicker({ title, icon, options, value, canEdit, onSave }: {
  title: string; icon: React.ReactNode; options: Array<{ value: string; label: string }>; value: string[]
  canEdit: boolean; onSave: (ids: string[]) => void
}) {
  // 저장된 대상이 바뀌면 호출부가 key 로 다시 마운트한다 — 효과 안 setState 없이 초기값만 받는다
  const [sel, setSel] = useState<Set<string>>(new Set(value))
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

// ── 배정 — 가입자(person) ↔ 콘솔 계정 한 화면 ──
//  가입자 = PUT/DELETE /roles/{id}/assignments (사람당 역할 하나 — 다른 역할에서 이동).
//  콘솔 계정 = OAM PUT /console-accounts/{login_id} role (admin 게이트). 콘솔 계정은 역할 없이 있을 수 없어
//  「해제」가 없다 — 다른 역할에 배정하는 것이 곧 이동이다.
function AssignmentsPanel({ role, users, accounts, canManage, isAdmin, reload }: {
  role: RoleDef; users: UserSummary[]; accounts: ConsoleAccount[]; canManage: boolean; isAdmin: boolean; reload: () => void
}) {
  const { show } = useToast()
  const [list, setList] = useState<RoleAssignment[]>([])
  const [busy, setBusy] = useState(false)
  const [acctPick, setAcctPick] = useState('')

  const roleId = role.id
  const reloadList = useCallback(() => {
    rolesApi.listAssignments(roleId).then(setList).catch(() => setList([]))
  }, [roleId])
  useEffect(() => { reloadList() }, [reloadList])

  // 콘솔 계정은 CSC 가 OAM console_accounts.role 을 합쳐 준다(§6.8) — 못 합친 배포를 위해 콘솔 계정 API 결과와 합집합
  const merged = useMemo(() => {
    const m = new Map<string, RoleAssignment>()
    for (const a of list) m.set(`${a.principal_type}:${a.principal_id}`, a)
    for (const a of accounts) if (a.role === roleId) {
      const k = `console:${a.login_id}`
      if (!m.has(k)) m.set(k, { principal_type: 'console', principal_id: a.login_id, name: a.name })
    }
    return Array.from(m.values())
  }, [list, accounts, roleId])
  const userRows = merged.filter(a => a.principal_type === 'user')
  const consoleRows = merged.filter(a => a.principal_type === 'console')

  const userIndex = useMemo(() => buildPickIndex(users, 'user'), [users])
  const userName = (id: string) => users.find(u => String(u.id) === id)?.name
  const assignedUsers = useMemo(() => new Set(userRows.map(a => a.principal_id)), [userRows])
  const acctCandidates = accounts.filter(a => a.role !== roleId)

  async function assignUser(it: PickItem) {
    setBusy(true)
    try {
      const r = await rolesApi.assignUser(roleId, it.value)
      show(r.moved_from ? `${it.userName} 배정 (${r.moved_from} 에서 이동)` : `${it.userName} 배정`, 'ok')
      reloadList(); reload()
    } catch (e: unknown) { show(errText(e), 'err') } finally { setBusy(false) }
  }
  async function unassignUser(a: RoleAssignment) {
    setBusy(true)
    try { await rolesApi.unassign(roleId, 'user', a.principal_id); show('배정 해제', 'ok'); reloadList(); reload() }
    catch (e: unknown) { show(errText(e), 'err') } finally { setBusy(false) }
  }
  async function assignAccount() {
    if (!acctPick) return
    setBusy(true)
    try { await consoleAccountsApi.update(acctPick, { role: roleId }); show(`콘솔 계정 ${acctPick} 배정`, 'ok'); setAcctPick(''); reload() }
    catch (e: unknown) { show(String(e), 'err') } finally { setBusy(false) }
  }

  return (
    <div className="mt-2.5 border-t border-border pt-3">
      <div className="flex items-stretch gap-2.5">
        <div className="flex min-w-0 flex-col overflow-hidden rounded-md border border-border bg-card flex-1">
          <div className="flex items-center gap-2 border-b border-border px-2.5 py-2 text-sm font-semibold">가입자 <Badge  variant="brandSoft">{userRows.length}</Badge>
            <span className="ml-auto font-normal text-muted-foreground text-xs">관제 앱(PKCE)에서 쓰는 역할 — 사람당 하나</span></div>
          {canManage && (
            <div className="flex items-center gap-1.5 py-1.5 px-2 border-b border-border">
              <SubscriberPicker kind="user" index={userIndex} exclude={assignedUsers} placeholder="가입자 이름·조직 검색 → 배정" onPick={assignUser} />
            </div>
          )}
          <div className="max-h-[220px] min-h-[60px] overflow-y-auto">
            {userRows.length === 0
              ? <div className="p-3.5 text-sm text-center text-muted-foreground">배정된 가입자 없음</div>
              : userRows.map(a => (
                <div key={a.principal_id} className="flex items-center gap-2 px-2.5 py-1.5 text-sm">
                  <span className="flex flex-col min-w-0 flex-1">
                    <span className="font-semibold whitespace-nowrap overflow-hidden text-ellipsis">{a.name || userName(a.principal_id) || '—'}</span>
                    <span className="text-muted-foreground text-xs">users.id {a.principal_id}</span>
                  </span>
                  {canManage && <IconBtn title="배정 해제" tone="danger" disabled={busy} onClick={() => unassignUser(a)}><UserMinus size={ICON} /></IconBtn>}
                </div>
              ))}
          </div>
        </div>

        <div className="flex min-w-0 flex-col overflow-hidden rounded-md border border-border bg-card flex-1">
          <div className="flex items-center gap-2 border-b border-border px-2.5 py-2 text-sm font-semibold">콘솔 계정 <Badge  variant="neutralSoft">{consoleRows.length}</Badge>
            <span className="ml-auto font-normal text-muted-foreground text-xs">OAM 로그인 계정 — 관리 › 콘솔 계정과 같은 값</span></div>
          {isAdmin && (
            <div className="flex items-center gap-1.5 py-1.5 px-2 border-b border-border">
              <Select value={toSel(acctPick)} onValueChange={(v: string) => setAcctPick(fromSel(v))}>
                <SelectTrigger className="flex-1"><SelectValue placeholder="콘솔 계정 선택" /></SelectTrigger>
                <SelectContent>
                  {acctCandidates.length === 0 && <SelectItem value={NONE} disabled>배정 가능한 계정 없음</SelectItem>}
                  {acctCandidates.map(a => <SelectItem key={a.login_id} value={a.login_id}>{a.login_id} · {a.name} ({a.role})</SelectItem>)}
                </SelectContent>
              </Select>
              <Button disabled={busy || !acctPick} onClick={assignAccount}>배정</Button>
            </div>
          )}
          {!isAdmin && canManage && <div className="py-1.5 px-2.5 text-xs text-muted-foreground border-b border-border">콘솔 계정 배정은 admin (관리 › 콘솔 계정)</div>}
          <div className="max-h-[220px] min-h-[60px] overflow-y-auto">
            {consoleRows.length === 0
              ? <div className="p-3.5 text-sm text-center text-muted-foreground">배정된 콘솔 계정 없음</div>
              : consoleRows.map(a => (
                <div key={a.principal_id} className="flex items-center gap-2 px-2.5 py-1.5 text-sm">
                  <span className="flex flex-col min-w-0 flex-1">
                    <span className="font-semibold whitespace-nowrap overflow-hidden text-ellipsis">{a.principal_id}</span>
                    <span className="text-muted-foreground text-xs">{a.name || '—'}</span>
                  </span>
                </div>
              ))}
          </div>
        </div>
      </div>
      <div className="text-xs text-muted-foreground mt-1.5">PTT 청취 범위가 있는 역할에 배정되면 CSC 가 그 사람의 allow_ambient_listening 을 켜고, 해제·범위 소멸 시 끈다 · 배정·해제는 감사된다(E-AUD-006)</div>
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
