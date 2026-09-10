import { useConfirm } from '@core/components/custom/confirm'
import { useState, useEffect, useCallback, useMemo } from 'react'
import IconBtn from '@core/components/IconBtn'
import { AlertTriangle, Check, ChevronDown, ChevronRight, Pencil, Plus, Trash2, X } from 'lucide-react'
import { usersApi, type UserSummary, type Subscription, type UserInput, type McpttProfile, type SipTransport, type AuthScheme, type ImportResult, type LineSvc } from '@core/api/users'
import { groupsApi, type Group } from '@core/api/groups'
import { orgApi, type Organization } from '@core/api/organizations'
import OrgTreePanel from '@core/components/OrgTreePanel'
import { DataTable, type Column } from '@core/components/DataTable'
import SubscriberPicker, { buildPickIndex, type PickItem } from '@core/components/SubscriberPicker'
import { useToast } from '@core/components/Toast'
import { useAuth } from '@core/contexts/AuthContext'
import { canWriteConfig } from '@core/utils/permissions'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { NONE, fromSel, toSel } from '@core/components/custom/select-value'
import { DataTable as TableFrame, Th, Td } from '@core/components/custom/data-table'
import { Badge } from '@core/components/ui/badge'
import Modal from '@core/components/Modal'
import { Checkbox } from '@core/components/ui/checkbox'

// ── 사용자 프로비저닝 워크벤치 (사용자 = 가입, 번호 등록이 가입 행위) ──────────
//  좌: 조직트리(공유 스코프) | 상단 탭: 사용자/VoLTE 번호/VoIP 번호/PTT 번호 (번호 탭 = 가입 테이블 = 접속환경 kind).
//  편집은 '행 펼침 상세' 단일 패러다임으로 통일 — 행 클릭 → 상세(기본정보 편집 + 번호 서브테이블).
//  번호는 사용자 종속(child) — 별도 메뉴 없이 사용자 하위로 관리. PTT 그룹은 별도 메뉴.

type Tab = 'users' | 'volte' | 'voip' | 'ptt'
const TAB_SVC: Record<Exclude<Tab, 'users'>, LineSvc> = { volte: 'call', voip: 'voip', ptt: 'ptt' }

// 번호 탭의 평탄화 행
interface NumberRow { msisdn: string; svc: LineSvc; user: UserSummary; sub: Subscription }

// ── 회선 종류(접속환경 kind)별 폼 스펙 — 종류마다 다른 것만 여기 선언하고 폼·표는 이 스펙 하나로 그린다
//    (sip_service_model.md §2-9, volte_supplementary_services.md §3 유선 규약). 같은 것은 스펙에 두지 않는다.
interface LineSpec {
  label: string                     // 배지 라벨
  short: string                     // 버튼·제목 라벨
  badge: 'brandSoft' | 'infoSoft' | 'successSoft'
  subsOf: (u: UserSummary) => Subscription[]
  defaultRef: string                // 접속서비스 카탈로그가 비었을 때 기본 name
  msisdnPlaceholder: string
  imsiAuto: boolean                 // IMSI 를 비우면 MSISDN 숫자로 채운다(USIM 없는 유선 규약 — Digest username 의 user 파트)
  authSchemes: AuthScheme[]         // 고를 수 있는 인증 체계 — 하나뿐이면 선택 UI 를 숨기고 그 값으로 보낸다
  defaultTransport: SipTransport | ''   // 새 회선 기본 채널 정책 ('' = ANY 단말 선택)
  showDnd: boolean                  // DND·착신전환은 전화 회선만
  showExtension: boolean            // 내선 라벨(끝 자리, 표시 전용 — 망 주소는 E.164)
}
const LINE: Record<LineSvc, LineSpec> = {
  call: { label: 'VoLTE', short: 'VoLTE', badge: 'brandSoft', subsOf: u => u.call_subscriptions || [], defaultRef: 'volte',
          msisdnPlaceholder: '+8213…', imsiAuto: false, authSchemes: ['digest', 'aka'], defaultTransport: '', showDnd: true, showExtension: false },
  voip: { label: 'VoIP', short: 'VoIP', badge: 'infoSoft', subsOf: u => u.voip_subscriptions || [], defaultRef: 'voip',
          msisdnPlaceholder: '+8221…', imsiAuto: true, authSchemes: ['digest'], defaultTransport: 'TLS', showDnd: true, showExtension: true },
  ptt:  { label: 'McPTT', short: 'PTT', badge: 'successSoft', subsOf: u => u.ptt_subscriptions || [], defaultRef: 'mcptt',
          msisdnPlaceholder: '+825…', imsiAuto: false, authSchemes: ['digest', 'aka'], defaultTransport: 'TLS', showDnd: false, showExtension: false },
}
const LINE_SVCS: LineSvc[] = ['call', 'voip', 'ptt']
// 내선 라벨 자릿수 — 서버 Provisioning.ExtensionDigits 기본값과 같다(표시 전용)
const EXT_DIGITS = 4
const digitsOf = (msisdn: string) => msisdn.replace(/\D/g, '')
const extensionOf = (msisdn: string) => digitsOf(msisdn).slice(-EXT_DIGITS)

// 펼침 상태 — 어느 행이 펼쳐졌는지(key) + 그 사용자(userId) + 초기 편집모드 + 강조할 번호
type Expand = { key: string | number; userId: number; edit: boolean; hi?: string } | null

const ICON = 14

// 작은 아이콘 액션 버튼

// 펼침 표시 caret (열림/닫힘)
function Caret({ open }: { open: boolean }) {
  return <span className="text-muted-foreground inline-flex">
    {open ? <ChevronDown size={15} /> : <ChevronRight size={15} />}
  </span>
}

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

type OrgOpt = { code: string; label: string }

export default function ProvisioningWorkbenchPage() {
  const { show } = useToast()
  const confirm = useConfirm()
  const { user: me } = useAuth()
  const canWrite = canWriteConfig(me)

  const [tab, setTab] = useState<Tab>('users')
  const [orgScope, setOrgScope] = useState<string | null>(null)
  const [orgName, setOrgName] = useState('전체')
  const [search, setSearch] = useState('')

  const [users, setUsers] = useState<UserSummary[]>([])
  const [orgs, setOrgs] = useState<Organization[]>([])
  const [loading, setLoading] = useState(true)

  const [selected, setSelected] = useState<Set<string | number>>(new Set())
  const [importOpen, setImportOpen] = useState(false)

  // 단일 편집 패러다임: 행 펼침 상세
  const [exp, setExp] = useState<Expand>(null)
  // 추가 폼 (테이블 위 블록)
  const [addUserOpen, setAddUserOpen] = useState(false)
  const [addNumSvc, setAddNumSvc] = useState<LineSvc | null>(null)

  const orgOpts = useMemo(() => orgIndentedOptions(orgs), [orgs])
  const userIndex = useMemo(() => buildPickIndex(users, 'user'), [users])

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [u, o] = await Promise.all([usersApi.list(), orgApi.list()])
      setUsers(u); setOrgs(o)
    } catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [show])
  useEffect(() => { load() }, [load])

  const orgPathOf = useCallback((code: string) => orgs.find(o => o.code === code)?.code_path || code, [orgs])
  const inScope = useCallback((orgCode: string) => {
    if (!orgScope) return true
    return (orgPathOf(orgCode) || '').startsWith(orgScope)
  }, [orgScope, orgPathOf])

  // 탭 전환 시 임시상태 초기화
  useEffect(() => { setSelected(new Set()); setExp(null); setAddUserOpen(false); setAddNumSvc(null) }, [tab])

  // 행 펼침 토글 (같은 행 재클릭 → 닫힘)
  const toggleExpand = useCallback((key: string | number, userId: number, hi?: string) => {
    setExp(cur => (cur && cur.key === key) ? null : { key, userId, edit: false, hi })
  }, [])
  const openEdit = useCallback((key: string | number, userId: number) => {
    setExp({ key, userId, edit: true })
  }, [])

  const userHasNumber = useCallback((u: UserSummary, q: string) =>
    LINE_SVCS.some(svc => LINE[svc].subsOf(u).some(s => s.id.toLowerCase().includes(q))), [])

  // ── 탭 데이터 ──
  const userRows = useMemo(() => {
    const q = search.trim().toLowerCase()
    return users.filter(u => inScope(u.org_id || '') &&
      (!q || u.name.toLowerCase().includes(q) || (u.title || '').toLowerCase().includes(q) || userHasNumber(u, q)))
  }, [users, inScope, search, userHasNumber])

  const buildNumberRows = useCallback((svc: LineSvc): NumberRow[] => {
    const q = search.trim().toLowerCase()
    const out: NumberRow[] = []
    for (const u of users) {
      if (!inScope(u.org_id || '')) continue
      for (const sub of LINE[svc].subsOf(u)) out.push({ msisdn: sub.id, svc, user: u, sub })
    }
    return out.filter(r => !q || r.msisdn.toLowerCase().includes(q) || r.user.name.toLowerCase().includes(q))
  }, [users, inScope, search])
  const volteRows = useMemo(() => buildNumberRows('call'), [buildNumberRows])
  const voipRows = useMemo(() => buildNumberRows('voip'), [buildNumberRows])
  const pttRows = useMemo(() => buildNumberRows('ptt'), [buildNumberRows])
  const rowsOf: Record<Exclude<Tab, 'users'>, NumberRow[]> = { volte: volteRows, voip: voipRows, ptt: pttRows }

  // ── 삭제 ──
  async function batchDeleteUsers() {
    const ids = Array.from(selected).map(Number)
    if (!ids.length) return
    if (!await confirm({ title: '가입자 일괄 삭제', tone: 'danger', confirmLabel: '삭제',
      body: `${ids.length}명을 삭제합니다. 연결된 번호도 삭제됩니다.` })) return
    try { await usersApi.batchDelete(ids); show('삭제 완료', 'ok'); setSelected(new Set()); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function deleteUser(u: UserSummary) {
    if (!await confirm({ title: '가입자 삭제', tone: 'danger', confirmLabel: '삭제',
      body: `${u.name} 삭제? 연결된 번호도 삭제됩니다.` })) return
    try { await usersApi.delete(u.id); show('삭제', 'ok'); if (exp?.userId === u.id) setExp(null); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function deleteNumber(r: NumberRow) {
    if (!await confirm({ title: '번호 삭제', tone: 'danger', confirmLabel: '삭제',
      body: `${r.msisdn} 삭제?` })) return
    try { await usersApi.deleteSub(r.user.id, r.svc, r.msisdn); show('삭제', 'ok'); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  // ── 컬럼 정의 ──
  const userCols: Column<UserSummary>[] = [
    { key: 'exp', header: '', width: 26, render: u => <Caret open={exp?.key === u.id} /> },
    { key: 'name', header: '이름', sortable: true, width: 130, render: u => <span className="font-medium">{u.name}</span> },
    { key: 'title', header: '직함', sortable: true, width: 90, sortValue: u => u.title || '', render: u => <span className="text-sm text-muted-foreground">{u.title || '—'}</span> },
    { key: 'login_id', header: '로그인ID', sortable: true, width: 110, sortValue: u => u.login_id || '', render: u => <span className="text-sm text-muted-foreground" title="단말 로그인 ID">{u.login_id || '—'}</span> },
    { key: 'org', header: '조직', width: 220, sortValue: u => buildOrgPath(orgs, u.org_id), render: u => <span className="text-sm text-muted-foreground" title={buildOrgPath(orgs, u.org_id)}>{buildOrgPath(orgs, u.org_id)}</span> },
    { key: 'details', header: '설명', render: u => <span className="text-sm text-muted-foreground">{u.details || '—'}</span> },
    { key: 'nums', header: '번호', width: 220, render: u => {
      const all = LINE_SVCS.flatMap(svc => LINE[svc].subsOf(u).map(s => ({ svc, id: s.id })))
      if (all.length === 0) return <span className="text-sm text-muted-foreground">—</span>
      return <span className="flex flex-wrap gap-[3px]">
        {all.map(n => <Badge  variant={LINE[n.svc].badge} key={`${n.svc}:${n.id}`} title={LINE[n.svc].label}>{n.id}</Badge>)}
      </span>
    } },
    { key: 'act', header: '', width: 84, align: 'right', render: u => canWrite ? (
      <span className="flex gap-1.5" onClick={e => e.stopPropagation()}>
        <IconBtn title="편집" onClick={() => openEdit(u.id, u.id)}><Pencil size={ICON} /></IconBtn>
        <IconBtn title="삭제" tone="danger" onClick={() => deleteUser(u)}><Trash2 size={ICON} /></IconBtn>
      </span>
    ) : <span className="text-sm text-muted-foreground">—</span> },
  ]

  const numberActCol: Column<NumberRow> = { key: 'act', header: '', width: 64, align: 'right', render: r => canWrite ? (
    <span className="flex gap-1.5" onClick={e => e.stopPropagation()}>
      <IconBtn title="삭제" tone="danger" onClick={() => deleteNumber(r)}><Trash2 size={ICON} /></IconBtn>
    </span>
  ) : <span className="text-sm text-muted-foreground">—</span> }
  const numberBaseCols: Column<NumberRow>[] = [
    { key: 'exp', header: '', width: 26, render: r => <Caret open={exp?.key === r.msisdn} /> },
    { key: 'msisdn', header: 'MSISDN', sortable: true, render: r => <span className="font-semibold">{r.msisdn}</span> },
    { key: 'imsi', header: 'IMSI', width: 150, sortable: true, sortValue: r => r.sub.imsi || '', render: r => <span className="text-sm text-muted-foreground">{r.sub.imsi || '—'}</span> },
    { key: 'svc_ref', header: '서비스', width: 90, render: r => <span className="text-sm text-muted-foreground">{r.sub.service_ref || '—'}</span> },
    { key: 'user', header: '가입자', sortable: true, sortValue: r => r.user.name, render: r => r.user.name },
    { key: 'org', header: '조직', width: 130, render: r => <span className="text-sm text-muted-foreground">{orgs.find(o => o.code === r.user.org_id)?.name || r.user.org_id || '—'}</span> },
  ]
  // 전화 회선(VoLTE·VoIP) 표 = 기본 열 + DND/착신전환, PTT 표 = 기본 열
  const phoneCols: Column<NumberRow>[] = [
    ...numberBaseCols,
    { key: 'dnd', header: 'DND', width: 70, align: 'center', render: r => <Badge  variant={r.sub.dnd ? 'dangerSoft' : 'neutralSoft'}>{r.sub.dnd ? 'ON' : 'OFF'}</Badge> },
    { key: 'fwd', header: '착신전환', width: 120, render: r => <span className="text-sm text-muted-foreground">{r.sub.forward_id || '—'}</span> },
    numberActCol,
  ]
  const pttCols: Column<NumberRow>[] = [...numberBaseCols, numberActCol]
  const colsOf: Record<Exclude<Tab, 'users'>, Column<NumberRow>[]> = { volte: phoneCols, voip: phoneCols, ptt: pttCols }

  const TABS: Array<{ k: Tab; label: string; count: number }> = [
    { k: 'users', label: '사용자', count: userRows.length },
    { k: 'volte', label: 'VoLTE 번호', count: volteRows.length },
    { k: 'voip', label: 'VoIP 번호', count: voipRows.length },
    { k: 'ptt', label: 'PTT 번호', count: pttRows.length },
  ]

  const expUser = exp ? users.find(u => u.id === exp.userId) : undefined
  const catalog = useMemo(() => buildServiceCatalog(users), [users])

  // 행 확장 렌더 (사용자 상세 = 기본정보 편집 + 번호 서브테이블) — 모드 전환 시 remount
  const renderDetail = () => exp && expUser
    ? <UserDetail key={`${expUser.id}:${exp.edit}`} user={expUser} catalog={catalog}
        orgOpts={orgOpts} canWrite={canWrite} initialEdit={exp.edit}
        highlight={exp.hi} onReload={load} />
    : null

  return (
    <div className="flex gap-4 items-stretch flex-1 min-h-0">
      {/* 좌: 조직 트리 (공유 스코프) */}
      <OrgTreePanel className="flex-[0_0_200px] w-[200px] max-w-[200px]" fill selectedPath={orgScope} onSelect={(p, n) => { setOrgScope(p); setOrgName(n) }}/>

      {/* 중: 패널 = 탭 헤더 + 툴바 + 테이블 */}
      <div className="panel flex flex-1 flex-col overflow-hidden rounded-md border border-border bg-card flex-1 min-w-0">
        {/* 탭 헤더 */}
        <div className="flex items-stretch gap-0.5 border-b border-border bg-muted px-2">
          {TABS.map(t => (
            <button key={t.k} onClick={() => setTab(t.k)}
              style={{
                padding: '12px 14px', border: 'none', background: 'none', cursor: 'pointer', fontSize: 13,
                fontWeight: tab === t.k ? 700 : 500, color: tab === t.k ? 'var(--primary)' : 'var(--muted-foreground)',
                borderBottom: tab === t.k ? '2px solid var(--primary)' : '2px solid transparent', marginBottom: -1,
              }}>
              {t.label} <Badge className="ml-0.5" variant="neutralSoft">{t.count}</Badge>
            </button>
          ))}
        </div>

        {/* 툴바 */}
        <div className="toolbar flex items-center gap-2.5 border-b border-border bg-muted px-4 py-3">
          <span className="font-semibold text-md">{orgName}</span>
          <Input className="flex-1 max-w-[220px]" placeholder="이름·번호·ID 검색" value={search}
            onChange={e => setSearch(e.target.value)}/>
          {search && <Button variant="ghost" onClick={() => setSearch('')}
        aria-label="검색어 지우기"><X size={13} /></Button>}
          <span className="ml-auto flex gap-1.5">
            {tab === 'users' && canWrite && <>
              <Button onClick={() => setImportOpen(true)}>Excel 가져오기</Button>
              {selected.size > 0 && <Button variant="destructive" onClick={batchDeleteUsers}>선택 삭제 ({selected.size})</Button>}
              <Button variant="default" onClick={() => { setAddUserOpen(v => !v); setExp(null) }}><Plus size={13} /> 사용자</Button>
            </>}
            {tab !== 'users' && canWrite && (
              <Button variant="default" onClick={() => { setAddNumSvc(TAB_SVC[tab]); setExp(null) }}>
                <Plus size={13} /> {LINE[TAB_SVC[tab]].short} 번호
              </Button>
            )}
          </span>
        </div>

        {/* 추가 폼 블록 (테이블 위) */}
        {tab === 'users' && addUserOpen && (
          <div className="border-b border-border bg-muted py-2.5 px-4">
            <div className="font-semibold text-sm text-primary mb-2">새 사용자</div>
            <UserBasicForm mode="add" orgOpts={orgOpts}
              defaultOrg={orgScope ? (orgScope.split('/').pop() || '') : ''}
              onSubmit={async (input) => { await usersApi.create(input); show('생성', 'ok'); setAddUserOpen(false); load() }}
              onCancel={() => setAddUserOpen(false)} />
          </div>
        )}
        {tab !== 'users' && addNumSvc && (
          <div className="border-b border-border bg-muted py-2.5 px-4">
            <div className="font-semibold text-sm text-primary mb-2">새 {LINE[addNumSvc].short} 번호</div>
            <NumberAddForm svc={addNumSvc} catalog={catalog} userIndex={userIndex} orgScope={orgScope} orgPathOf={orgPathOf}
              onAdded={() => { setAddNumSvc(null); load() }} onCancel={() => setAddNumSvc(null)} />
          </div>
        )}

        {/* 테이블 — 행 클릭 시 바로 아래 사용자 상세(기본정보 편집 + 번호) 인라인 확장 */}
        {tab === 'users' && (
          <DataTable<UserSummary> columns={userCols} rows={userRows} rowKey={u => u.id} loading={loading}
            selectable={canWrite} selected={selected} onSelectChange={setSelected}
            onRowClick={u => toggleExpand(u.id, u.id)}
            expandedKey={exp?.key ?? null}
            renderExpanded={exp && expUser ? renderDetail : undefined}
            pageSize={50} emptyText="사용자 없음" />
        )}
        {tab !== 'users' && (
          <DataTable<NumberRow> key={tab} columns={colsOf[tab]} rows={rowsOf[tab]} rowKey={r => r.msisdn} loading={loading}
            onRowClick={r => toggleExpand(r.msisdn, r.user.id, r.msisdn)}
            expandedKey={exp?.key ?? null}
            renderExpanded={exp && expUser ? renderDetail : undefined}
            pageSize={50} emptyText={`${LINE[TAB_SVC[tab]].short} 번호 없음`} />
        )}
      </div>

      {/* Excel import (사용자+번호 통합) */}
      {importOpen && <ImportModal onClose={() => setImportOpen(false)} onDone={load} />}
    </div>
  )
}

// ════════════════════════════════════════════════════════════
//  공용 컴팩트 폼 위젯
// ════════════════════════════════════════════════════════════
function Field({ label, children, w }: { label: string; children: React.ReactNode; w?: number | string }) {
  return (
    <label style={{ display: 'flex', flexDirection: 'column', gap: 2, width: w, flex: w ? undefined : '1 1 160px', minWidth: 120 }}>
      <span className="text-xs text-muted-foreground">{label}</span>
      {children}
    </label>
  )
}
function FieldRow({ children }: { children: React.ReactNode }) {
  return <div className="flex flex-wrap items-end gap-x-3 gap-y-2">{children}</div>
}

// ── 사용자 기본정보 폼 (추가 + 편집 공용) ──
function UserBasicForm({ mode, initial, orgOpts, defaultOrg, onSubmit, onCancel }: {
  mode: 'add' | 'edit'
  initial?: UserSummary
  orgOpts: OrgOpt[]
  defaultOrg?: string
  onSubmit: (input: UserInput) => Promise<void> | void
  onCancel: () => void
}) {
  const { show } = useToast()
  // 가입자(person). login_id/passwd = 단말(IdMS) 로그인 자격(MCPTT ID 와 별개).
  //   콘솔 admin 계정은 '콘솔 계정' 메뉴에서 별도 관리. passwd 는 입력 시에만 전송(편집 시 빈칸=유지).
  const [form, setForm] = useState<UserInput>(() => initial
    ? { name: initial.name, title: initial.title || '', org_id: initial.org_id, details: initial.details || '', login_id: initial.login_id || '' }
    : { name: '', title: '', org_id: defaultOrg || '', details: '', login_id: '' })
  const [busy, setBusy] = useState(false)

  async function submit() {
    if (!form.name) { show('이름 필수', 'err'); return }
    setBusy(true)
    // passwd 빈칸이면 전송하지 않음(기존 비번 유지). 추가 모드에선 빈칸이면 미설정.
    const payload: UserInput = { ...form }
    if (!payload.passwd) delete payload.passwd
    try { await onSubmit(payload) } catch (e: unknown) { show(String(e), 'err') } finally { setBusy(false) }
  }

  return (
    <FieldRow>
      <Field label="이름 *" w={150}><Input  autoFocus value={form.name} onChange={e => setForm({ ...form, name: e.target.value })} /></Field>
      <Field label="직함" w={110}><Input  placeholder="예: 팀장" value={form.title || ''} onChange={e => setForm({ ...form, title: e.target.value })} /></Field>
      <Field label="로그인 ID" w={130}><Input  placeholder="예: test001" value={form.login_id || ''} onChange={e => setForm({ ...form, login_id: e.target.value })} /></Field>
      <Field label={mode === 'add' ? '비밀번호' : '비밀번호(변경 시)'} w={140}><Input  type="password" placeholder={mode === 'add' ? '' : '미변경'} value={form.passwd || ''} onChange={e => setForm({ ...form, passwd: e.target.value })} /></Field>
      <Field label="조직" w={200}>
        <Select value={toSel(form.org_id)} onValueChange={(v: string) => setForm({ ...form, org_id: fromSel(v) })}>
          <SelectTrigger><SelectValue /></SelectTrigger>
          <SelectContent>
            <SelectItem value={NONE}>없음</SelectItem>
            {orgOpts.map(o => <SelectItem key={o.code} value={o.code}>{o.label}</SelectItem>)}
          </SelectContent>
        </Select>
      </Field>
      <Field label="설명"><Input  value={form.details || ''} onChange={e => setForm({ ...form, details: e.target.value })} /></Field>
      <div className="flex gap-1.5 items-center">
        <Button variant="default" disabled={busy} onClick={submit}>{mode === 'add' ? '생성' : '저장'}</Button>
        <Button variant="ghost" onClick={onCancel}>취소</Button>
      </div>
    </FieldRow>
  )
}

// ════════════════════════════════════════════════════════════
//  사용자 상세 (행 확장) — 기본정보(보기↔편집) + 번호 서브테이블
// ════════════════════════════════════════════════════════════
function UserDetail({ user, catalog, orgOpts, canWrite, initialEdit, highlight, onReload }: {
  user: UserSummary; catalog: ServiceCat[]; orgOpts: OrgOpt[]; canWrite: boolean
  initialEdit: boolean; highlight?: string; onReload: () => void
}) {
  const { show } = useToast()
  const [editing, setEditing] = useState(initialEdit)
  const orgPath = useMemo(() => {
    // 조직 표시는 코드만 보유 → orgOpts 라벨(들여쓰기 제거) 매칭
    const o = orgOpts.find(o => o.code === user.org_id)
    return o ? o.label.replace(/^[\u3000]+/, '') : (user.org_id || '—')
  }, [orgOpts, user.org_id])

  return (
    <div className="py-3 px-4">
      {/* 기본정보 */}
      {editing ? (
        <UserBasicForm mode="edit" initial={user} orgOpts={orgOpts}
          onSubmit={async (input) => { await usersApi.update(user.id, input); show('저장', 'ok'); setEditing(false); onReload() }}
          onCancel={() => setEditing(false)} />
      ) : (
        <div className="flex items-center gap-4 flex-wrap text-sm">
          <span><b className="text-md">{user.name}</b>{user.title && <span className="text-sm text-muted-foreground ml-1.5">{user.title}</span>}</span>
          <span className="text-sm text-muted-foreground">조직 {orgPath}</span>
          {user.details && <span className="text-sm text-muted-foreground">{user.details}</span>}
          {canWrite && <Button className="ml-auto" onClick={() => setEditing(true)}>기본정보 편집</Button>}
        </div>
      )}

      {/* 번호 */}
      <div className="mt-3 border-t border-border pt-2.5">
        <div className="font-semibold text-sm text-muted-foreground mb-1.5">번호</div>
        <NumbersTable user={user} catalog={catalog} canWrite={canWrite} highlight={highlight} onReload={onReload} />
      </div>

      {/* MCPTT 프로파일 — SOS 대상 결정(TS 24.484 entry-info)·사용자 단위 개시 인가 */}
      {user.ptt_subscriptions.length > 0 && (
        <div className="mt-3 border-t border-border pt-2.5">
          <div className="font-semibold text-sm text-muted-foreground mb-1.5">MCPTT 프로파일 (SOS 대상·개시 인가)</div>
          {user.ptt_subscriptions.map(s => (
            <PttProfileRow key={s.id} pid={user.id} msisdn={s.id} canWrite={canWrite} />
          ))}
        </div>
      )}
    </div>
  )
}

// ── 사용자 MCPTT 프로파일 행 (PTT 번호당 1개) — DedicatedGroup=전용 긴급그룹으로 SOS,
//    UseCurrentlySelectedGroup=단말 선택 그룹(주채널)으로 SOS. 미저장 시 서버 기본값 표시. ──
const MODE_LABEL: Record<McpttProfile['emergency_group_mode'], string> = {
  DedicatedGroup: '전용 긴급그룹',
  UseCurrentlySelectedGroup: '선택 그룹(주채널)',
}

// 긴급 사설콜(1:1) 대상 결정 — LocallyDetermined=단말이 고른 상대, UsePreConfigured=사전지정 수신자.
const PRIV_MODE_LABEL: Record<McpttProfile['private_emergency_mode'], string> = {
  LocallyDetermined: '단말 선택 상대',
  UsePreConfigured: '사전지정 수신자',
}

function PttProfileRow({ pid, msisdn, canWrite }: { pid: number; msisdn: string; canWrite: boolean }) {
  const { show } = useToast()
  const [prof, setProf] = useState<(McpttProfile & { exists?: boolean }) | null>(null)
  const [groups, setGroups] = useState<Group[]>([])
  const [editing, setEditing] = useState(false)
  const [form, setForm] = useState<McpttProfile | null>(null)

  const load = useCallback(() => {
    usersApi.getPttProfile(pid, msisdn).then(setProf).catch(() => setProf(null))
  }, [pid, msisdn])
  useEffect(() => { load() }, [load])
  useEffect(() => {
    if (editing && groups.length === 0) groupsApi.list().then(setGroups).catch(() => {})
  }, [editing, groups.length])

  async function save() {
    if (!form) return
    if (form.emergency_group_mode === 'DedicatedGroup' && !form.emergency_group_id) {
      show('전용 긴급그룹 지정이 필요합니다 — 미지정이면 SOS 가 불발됩니다', 'err'); return
    }
    if (form.private_emergency_mode === 'UsePreConfigured' && !form.emergency_private_recipient?.trim()) {
      show('사전지정 수신자가 필요합니다 — 미지정이면 긴급 사설콜이 불발됩니다', 'err'); return
    }
    const body = { ...form, emergency_private_recipient: form.emergency_private_recipient?.trim() || null }
    try { await usersApi.updatePttProfile(pid, msisdn, body); show('저장', 'ok'); setEditing(false); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  if (!prof) return <div className="text-muted-foreground text-sm">{msisdn} — 프로파일 조회 실패(서버 구버전?)</div>

  if (editing && form) {
    return (
      <div className="flex items-center gap-2.5 flex-wrap text-sm py-1 px-0">
        <strong>{msisdn}</strong>
        <label className="text-sm text-muted-foreground">SOS 대상
          <Select value={toSel(form.emergency_group_mode)} onValueChange={(v: string) => setForm({ ...form, emergency_group_mode: fromSel(v) as McpttProfile['emergency_group_mode'] })}>
            <SelectTrigger className="ml-1"><SelectValue /></SelectTrigger>
            <SelectContent>
              <SelectItem value="DedicatedGroup">{MODE_LABEL.DedicatedGroup}</SelectItem>
              <SelectItem value="UseCurrentlySelectedGroup">{MODE_LABEL.UseCurrentlySelectedGroup}</SelectItem>
            </SelectContent>
          </Select>
        </label>
        {form.emergency_group_mode === 'DedicatedGroup' && (
          <label className="text-sm text-muted-foreground">긴급그룹
            <Select value={toSel(form.emergency_group_id || '')} onValueChange={(v: string) => setForm({ ...form, emergency_group_id: fromSel(v) || null })}>
              <SelectTrigger className="ml-1"><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value={NONE}>(미지정)</SelectItem>
                {groups.map(g => <SelectItem key={g.id} value={g.id}>{g.name || g.id}</SelectItem>)}
              </SelectContent>
            </Select>
          </label>
        )}
        <label className="text-sm text-muted-foreground"><Checkbox  checked={form.allow_emergency_call} onCheckedChange={(c) => setForm({ ...form, allow_emergency_call: (c === true) })} /> 긴급콜</label>
        <label className="text-sm text-muted-foreground"><Checkbox  checked={form.allow_emergency_alert} onCheckedChange={(c) => setForm({ ...form, allow_emergency_alert: (c === true) })} /> 긴급경보</label>
        <label className="text-sm text-muted-foreground"><Checkbox  checked={form.allow_adhoc_call} onCheckedChange={(c) => setForm({ ...form, allow_adhoc_call: (c === true) })} /> 애드혹</label>
        <label className="text-sm text-muted-foreground"><Checkbox  checked={form.allow_emergency_private_call} onCheckedChange={(c) => setForm({ ...form, allow_emergency_private_call: (c === true) })} /> 긴급 사설콜</label>
        {form.allow_emergency_private_call && (
          <label className="text-sm text-muted-foreground">사설 대상
            <Select value={toSel(form.private_emergency_mode)} onValueChange={(v: string) => setForm({ ...form, private_emergency_mode: fromSel(v) as McpttProfile['private_emergency_mode'] })}>
              <SelectTrigger className="ml-1"><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="LocallyDetermined">{PRIV_MODE_LABEL.LocallyDetermined}</SelectItem>
                <SelectItem value="UsePreConfigured">{PRIV_MODE_LABEL.UsePreConfigured}</SelectItem>
              </SelectContent>
            </Select>
          </label>
        )}
        {form.allow_emergency_private_call && form.private_emergency_mode === 'UsePreConfigured' && (
          <label className="text-sm text-muted-foreground">수신자
            <Input className="ml-1 w-[140px]" placeholder="+82500000001"
              title="지정 수신자의 PTT 번호 — 저장 시 서버가 존재검증(미존재 400)"
              value={form.emergency_private_recipient || ''}
              onChange={e => setForm({ ...form, emergency_private_recipient: e.target.value || null })}/>
          </label>
        )}
        <IconBtn title="저장" tone="primary" onClick={save}><Check size={ICON} /></IconBtn>
        <IconBtn title="취소" onClick={() => setEditing(false)}><X size={ICON} /></IconBtn>
      </div>
    )
  }

  const noDedicated = prof.emergency_group_mode === 'DedicatedGroup' && !prof.emergency_group_id
  return (
    <div className="flex items-center gap-2.5 flex-wrap text-sm py-1 px-0">
      <strong>{msisdn}</strong>
      <Badge  variant="brandSoft">{MODE_LABEL[prof.emergency_group_mode]}</Badge>
      {prof.emergency_group_mode === 'DedicatedGroup' && (
        noDedicated
          ? <Badge  variant="dangerSoft">긴급그룹 미지정 — SOS 불발</Badge>
          : <span className="text-sm text-muted-foreground">긴급그룹 <b>{prof.emergency_group_id}</b></span>
      )}
      {!prof.allow_emergency_call && <Badge  variant="dangerSoft">긴급콜 차단</Badge>}
      {!prof.allow_emergency_alert && <Badge  variant="dangerSoft">경보 차단</Badge>}
      {!prof.allow_adhoc_call && <Badge  variant="dangerSoft">애드혹 차단</Badge>}
      {!prof.allow_emergency_private_call && <Badge  variant="dangerSoft">긴급 사설콜 차단</Badge>}
      {prof.allow_emergency_private_call && prof.private_emergency_mode === 'UsePreConfigured' && (
        prof.emergency_private_recipient
          ? <span className="text-sm text-muted-foreground">사설수신자 <b>{prof.emergency_private_recipient}</b></span>
          : <Badge  variant="dangerSoft">사설수신자 미지정 — 긴급 사설콜 불발</Badge>
      )}
      {!prof.exists && <span className="text-sm text-muted-foreground">(기본값)</span>}
      {canWrite && (
        <IconBtn title="편집" onClick={() => { setForm({
          allow_emergency_call: prof.allow_emergency_call,
          allow_emergency_alert: prof.allow_emergency_alert,
          allow_adhoc_call: prof.allow_adhoc_call,
          emergency_group_mode: prof.emergency_group_mode,
          emergency_group_id: prof.emergency_group_id,
          allow_emergency_private_call: prof.allow_emergency_private_call,
          private_emergency_mode: prof.private_emergency_mode,
          emergency_private_recipient: prof.emergency_private_recipient,
        }); setEditing(true) }}><Pencil size={ICON} /></IconBtn>
      )}
    </div>
  )
}

// 서비스 카탈로그 항목 — ref(access_services 이름) + 그 서비스의 회선 종류(svc). 같은 종류의 회선이 쓰는 이름을 모으고,
//   종류마다 하나도 없으면 패키지 기본 이름(volte/voip/mcptt)을 둔다. 회선의 service_ref 는 자기 종류(kind)의 서비스만 가리킬 수 있다.
interface ServiceCat { svc: LineSvc; ref: string }
function buildServiceCatalog(users: UserSummary[]): ServiceCat[] {
  const seen = new Set<string>()
  const cat: ServiceCat[] = []
  const add = (svc: LineSvc, ref: string) => { const k = `${svc}:${ref}`; if (ref && !seen.has(k)) { seen.add(k); cat.push({ svc, ref }) } }
  for (const u of users) for (const svc of LINE_SVCS) for (const s of LINE[svc].subsOf(u)) if (s.service_ref) add(svc, s.service_ref)
  for (const svc of LINE_SVCS) if (!cat.some(c => c.svc === svc)) add(svc, LINE[svc].defaultRef)
  return cat
}

// 회선 종류 배지 (VoLTE / VoIP / McPTT)
function SvcBadge({ svc }: { svc: LineSvc }) {
  return <Badge  variant={LINE[svc].badge}>{LINE[svc].label}</Badge>
}

// 인증 체계 칸 — 종류가 허용하는 체계가 하나면(유선 voip = digest) 선택 UI 없이 고정 표시
function AuthCell({ spec, value, onChange }: { spec: LineSpec; value: AuthScheme | undefined; onChange: (v: AuthScheme) => void }) {
  if (spec.authSchemes.length === 1) return <span className="text-sm text-muted-foreground" title="USIM 없는 유선 회선 — SIP Digest 만">Digest</span>
  return <AuthSelect value={value} onChange={onChange} />
}

// 픽업그룹 칸 — 값은 전화 그룹 멤버십에서 파생된다(SoT = 멤버십, 직접 편집 409). 읽기 전용.
const PICKUP_TITLE = '전화 그룹 멤버십에서 파생 — 구성 › 전화 그룹. 빈 값 = 어떤 픽업·BLF 축에도 속하지 않음. 반영은 다음 등록 갱신부터'
function PickupCell({ value }: { value?: string | null }) {
  if (isPhoneGroupId(value)) return <Badge  variant="brandSoft" title={PICKUP_TITLE}>{value}</Badge>
  return <span className="text-sm text-muted-foreground" title={PICKUP_TITLE}>{value || '—'}</span>
}

// 새 회선 입력 상태 — IMSI 는 종류 규약(imsiAuto)이면 비워도 된다(전송 시 MSISDN 숫자로 채움)
interface AddNum { id: string; imsi: string; svcCat: string; passwd: string; sip_transport: SipTransport | ''; auth_scheme: AuthScheme; k: string; opc: string; dnd: boolean; forward_id: string }
// 종류 규약에 맞춘 IMSI 값 — 비면 imsiAuto 종류만 MSISDN 숫자로 채운다
const effectiveImsi = (spec: LineSpec, imsi: string, msisdn: string) => imsi.trim() || (spec.imsiAuto ? digitsOf(msisdn) : '')

// 전화 그룹 id(pg-…, 전환 전 발급 dg-… 유지 — dispatch_center.md §3.1) 에서 파생된 pickup_group 은 직접 편집 409(derived_from_phone_group)
const isPhoneGroupId = (v: string | null | undefined) => /^(pg|dg)-/.test(v || '')

// 인증 체계 (sip_access_security.md §8.2) — aka 는 K/OPc(hex32) 를 CSC AuC 가 암호화 보관, 보호 채널(TLS/IPsec) 강제.
//   K/OPc 는 응답에 오지 않는다(aka_provisioned 로 보관 여부만) — 입력 시에만 전송, 전송하면 SQN 0 리셋.
const HEX32 = /^[0-9a-fA-F]{32}$/
function AuthSelect({ value, onChange }: { value: AuthScheme | undefined; onChange: (v: AuthScheme) => void }) {
  return <Select value={toSel(value || 'digest')} onValueChange={(v: string) => onChange(fromSel(v) as AuthScheme)}>
   <SelectTrigger title="digest=SIP Digest(H(A1)) / aka=IMS AKA(K/OPc — 보호 채널 강제)"><SelectValue /></SelectTrigger>
   <SelectContent>
      <SelectItem value="digest">Digest</SelectItem><SelectItem value="aka">AKA</SelectItem>
   </SelectContent>
 </Select>
}
function AuthBadge({ sub }: { sub: Subscription }) {
  if (sub.auth_scheme !== 'aka') return <span className="text-sm text-muted-foreground">Digest</span>
  return <Badge  variant={sub.aka_provisioned ? 'successSoft' : 'dangerSoft'}
    title={sub.aka_provisioned ? 'IMS AKA — K/OPc 보관됨, 보호 채널(TLS/IPsec) 강제' : 'IMS AKA — K/OPc 미보관(등록 불가)'}>AKA{sub.aka_provisioned ? '' : <AlertTriangle size={10} className="ml-0.5 inline align-[-1px]" />}</Badge>
}
// K/OPc 입력 — 편집 시 비우면 보관 키 유지(aka_provisioned 일 때). 둘 다 hex32.
function AkaKeyInputs({ k, opc, keep, onChange }: { k: string; opc: string; keep?: boolean; onChange: (k: string, opc: string) => void }) {
  return <div className="flex flex-col gap-[3px] mt-[3px]">
    <Input className="font-mono text-xs" placeholder={keep ? 'K (미변경)' : 'K hex32 *'} value={k} onChange={e => onChange(e.target.value.trim(), opc)}/>
    <Input className="font-mono text-xs" placeholder={keep ? 'OPc (미변경)' : 'OPc hex32 *'} value={opc} onChange={e => onChange(k, e.target.value.trim())}/>
  </div>
}
// 입력 검증 + 전송 본문의 AKA 필드. 오류면 문자열 반환.
function akaBody(scheme: AuthScheme, k: string, opc: string, stored: boolean): { err?: string; fields: Partial<Subscription> } {
  const fields: Partial<Subscription> = { auth_scheme: scheme }
  if (scheme !== 'aka') return { fields }
  if (!k && !opc) return stored ? { fields } : { err: 'AKA 는 K/OPc(hex32) 입력이 필요합니다', fields }
  if (!HEX32.test(k) || !HEX32.test(opc)) return { err: 'K/OPc 는 32자리 16진수여야 합니다', fields }
  return { fields: { ...fields, k, opc } }
}

// 채널 정책 선택 — 값의 의미는 Subscription.sip_transport 주석 참조. ANY(null) = 서버 정책 없음, 단말이 고른다.
const TRANSPORT_TITLE = 'ANY=단말 선택(서버 정책 없음) / UDP·TCP=단말 힌트 / TLS=서버 집행(비-TLS 요청 403)'
const TRANSPORT_OPTS: Array<{ v: SipTransport | ''; label: string }> = [
  { v: '', label: 'ANY' }, { v: 'UDP', label: 'UDP' }, { v: 'TCP', label: 'TCP' }, { v: 'TLS', label: 'TLS (강제)' },
]
function TransportSelect({ value, onChange }: { value: SipTransport | '' | null | undefined; onChange: (v: SipTransport | '') => void }) {
  return <Select value={toSel(value || '')} onValueChange={(v: string) => onChange(fromSel(v) as SipTransport | '')}>
   <SelectTrigger title={TRANSPORT_TITLE}><SelectValue /></SelectTrigger>
   <SelectContent>
      {TRANSPORT_OPTS.map(o => <SelectItem key={o.v} value={o.v}>{o.label}</SelectItem>)}
   </SelectContent>
 </Select>
}
function TransportBadge({ v, aka }: { v?: SipTransport | null; aka?: boolean }) {
  if (aka) return <TransportFixedAka />
  if (!v) return <span className="text-sm text-muted-foreground" title="단말 선택 — 서버 정책 없음">ANY</span>
  return <Badge  variant={v === 'TLS' ? 'dangerSoft' : 'brandSoft'} title={v === 'TLS' ? '서버 집행 — 비-TLS 채널 요청 403' : '프로비저닝 힌트'}>{v}</Badge>
}
// AKA 가입자는 채널 정책(sip_transport) 값과 무관하게 보호 채널이 강제된다(requiresTls = TLS ∨ aka,
//   sip_access_security.md §8.2) — 선택이 무의미하므로 고정 표시한다. 프로비저닝도 목록을 TLS 로 좁힌다.
function TransportFixedAka() {
  return <Badge  variant="dangerSoft"
    title="AKA — 보호 채널(TLS) 강제. sip_transport 값과 무관하게 비-TLS 요청은 403이며, 단말 프로비저닝 목록도 TLS 하나로 좁혀진다. 접속서비스에 TLS 접속점(tls_port)이 없으면 등록 불가">TLS (AKA 강제)</Badge>
}

// ── 단일 번호 테이블 (사용자 상세 내부, VoLTE+VoIP+PTT 통합) — 종류별 차이는 LINE 스펙 하나로 그린다 ──
type LineRow = { svc: LineSvc; sub: Subscription }
function NumbersTable({ user, catalog, canWrite, highlight, onReload }: { user: UserSummary; catalog: ServiceCat[]; canWrite: boolean; highlight?: string; onReload: () => void }) {
  const { show } = useToast()
  const confirm = useConfirm()
  const rows: LineRow[] = LINE_SVCS.flatMap(svc => LINE[svc].subsOf(user).map(sub => ({ svc, sub })))
  const svcVal = (c: ServiceCat) => `${c.svc}:${c.ref}`
  const rk = (svc: LineSvc, msisdn: string) => `${svc}:${msisdn}`
  const catOf = (svcCat: string) => svcCat.split(':')[0] as LineSvc
  // 새 회선 초기값 — 채널 기본값은 종류 스펙(유선·PTT = TLS, 이동 = ANY)
  const newAdd = (svcCat?: string): AddNum => {
    const cat = svcCat || (catalog[0] ? svcVal(catalog[0]) : 'call:volte')
    return { id: '', imsi: '', svcCat: cat, passwd: '', sip_transport: LINE[catOf(cat)].defaultTransport, auth_scheme: 'digest', k: '', opc: '', dnd: false, forward_id: '' }
  }

  const [editKey, setEditKey] = useState<string | null>(null)
  const [editForm, setEditForm] = useState<Partial<Subscription>>({})
  const [adding, setAdding] = useState(false)
  const [addForm, setAddForm] = useState<AddNum>(newAdd())

  function startEdit(r: LineRow) {
    setAdding(false); setEditKey(rk(r.svc, r.sub.id))
    setEditForm({ imsi: r.sub.imsi || '', service_ref: r.sub.service_ref || '', passwd: '', sip_transport: r.sub.sip_transport || null, auth_scheme: r.sub.auth_scheme || 'digest', k: '', opc: '', dnd: r.sub.dnd, forward_id: r.sub.forward_id })
  }
  async function saveEdit(r: LineRow) {
    const spec = LINE[r.svc]
    // passwd 는 변경 시에만 전송. imsi/service_ref 가 바뀌면 서버가 passwd 를 요구한다(H(A1) 결박).
    const d: Partial<Subscription> = { ...editForm }; if (!d.passwd) delete d.passwd
    // IMSI 를 비웠으면 종류 규약으로 채운다 — 유선 회선은 번호 숫자(USIM 없음)
    if (spec.imsiAuto && !(d.imsi || '').trim()) d.imsi = digitsOf(r.sub.id)
    if (!d.passwd && ((d.imsi || '') !== (r.sub.imsi || '') || (d.service_ref || '') !== (r.sub.service_ref || ''))) { show('IMSI/서비스 변경 시 비밀번호를 함께 입력해야 합니다 (H(A1) 재결박)', 'err'); return }
    // 인증 체계 — 종류가 하나만 허용하면 그 값(유선 = digest). AKA: 체계 변경/키 갱신만 전송 (키는 입력했을 때만 — 비우면 보관 키 유지)
    const scheme: AuthScheme = spec.authSchemes.length === 1 ? spec.authSchemes[0] : (d.auth_scheme || 'digest')
    const aka = akaBody(scheme, d.k || '', d.opc || '', !!r.sub.aka_provisioned)
    if (aka.err) { show(aka.err, 'err'); return }
    // aka→digest 전환은 Digest 자격(H(A1)) 생성이 필요하다 — 서버는 저장 ha1 이 없을 때 400 (§8.2)
    if ((r.sub.auth_scheme || 'digest') === 'aka' && (aka.fields.auth_scheme || 'digest') === 'digest' && !d.passwd) { show('AKA→Digest 전환 시 비밀번호를 함께 입력해야 합니다 (H(A1) 생성)', 'err'); return }
    delete d.k; delete d.opc; delete d.auth_scheme
    if ((aka.fields.auth_scheme || 'digest') !== (r.sub.auth_scheme || 'digest') || aka.fields.k) Object.assign(d, aka.fields)
    if (!spec.showDnd) { delete d.dnd; delete d.forward_id }
    try { await usersApi.updateSub(user.id, r.svc, r.sub.id, d); show('수정', 'ok'); setEditKey(null); onReload() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function del(r: LineRow) {
    if (!await confirm({ title: '가입 삭제', tone: 'danger', confirmLabel: '삭제',
      body: `${r.sub.id} 삭제?` })) return
    try { await usersApi.deleteSub(user.id, r.svc, r.sub.id); show('삭제', 'ok'); onReload() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function add() {
    const svc = catOf(addForm.svcCat)
    const spec = LINE[svc]
    const ref = addForm.svcCat.split(':')[1] || spec.defaultRef
    if (!addForm.id) { show('MSISDN 필수', 'err'); return }
    const imsi = effectiveImsi(spec, addForm.imsi, addForm.id)
    if (!imsi) { show('IMSI 필수', 'err'); return }
    const scheme: AuthScheme = spec.authSchemes.length === 1 ? spec.authSchemes[0] : addForm.auth_scheme
    if (!addForm.passwd && scheme !== 'aka') { show('비밀번호 필수', 'err'); return }
    const aka = akaBody(scheme, addForm.k, addForm.opc, false)
    if (aka.err) { show(aka.err, 'err'); return }
    const body: Partial<Subscription> = { id: addForm.id, imsi, service_ref: ref, sip_transport: addForm.sip_transport || null,
      dnd: spec.showDnd ? addForm.dnd : false, forward_id: spec.showDnd ? addForm.forward_id : '', ...aka.fields }
    if (addForm.passwd) body.passwd = addForm.passwd
    try { await usersApi.addSub(user.id, svc, body); show('추가', 'ok'); setAdding(false); setAddForm(newAdd()); onReload() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  const addSpec = LINE[catOf(addForm.svcCat)]
  // 종류를 바꾸면 채널 기본값·인증 체계도 그 종류의 것으로 되돌린다(새 행이라 잃는 입력 없음)
  const changeAddCat = (v: string) => setAddForm({ ...addForm, svcCat: v, sip_transport: LINE[catOf(v)].defaultTransport, auth_scheme: 'digest', k: '', opc: '' })

  return (
    <div>
      <div className="flex-1 overflow-x-auto">
      <TableFrame sticky className="[&_td]:text-sm">
        <thead>
          <tr>
            <Th className="w-[150px]">서비스</Th>
            <Th className="w-[140px]">MSISDN</Th>
            <Th className="w-[100px]">비밀번호</Th>
            <Th>IMSI</Th>
            <Th className="w-[96px]" title={TRANSPORT_TITLE}>채널</Th>
            <Th className="w-[150px]" title="Digest=SIP Digest(H(A1)) / AKA=IMS AKA(K/OPc — CSC AuC 암호화 보관, 보호 채널 강제). 유선 VoIP 는 Digest 만">인증</Th>
            <Th className="w-[56px] text-center">DND</Th>
            <Th className="w-[110px]">착신전환</Th>
            <Th className="w-[100px]" title={PICKUP_TITLE}>픽업그룹</Th>
            <Th className="w-[110px]"></Th>
          </tr>
        </thead>
        <tbody>
          {rows.length === 0 && !adding && <tr><Td colSpan={10} className="py-8 text-center text-muted-foreground p-3">번호 없음 — 아래 [번호 추가]</Td></tr>}
          {rows.map(r => {
            const ed = editKey === rk(r.svc, r.sub.id)
            const spec = LINE[r.svc]
            const hi = highlight && r.sub.id === highlight
            return (
              <tr key={rk(r.svc, r.sub.id)} style={{ background: hi && !ed ? 'var(--cims-brand-soft)' : undefined }}>
                <Td>{ed
                  ? <Select value={toSel(editForm.service_ref || '')} onValueChange={(v: string) => setEditForm({ ...editForm, service_ref: fromSel(v) })}>
   <SelectTrigger><SelectValue /></SelectTrigger>
   <SelectContent>
                        {catalog.filter(c => c.svc === r.svc).map(c => <SelectItem key={c.ref} value={c.ref}>{c.ref}</SelectItem>)}
   </SelectContent>
 </Select>
                  : <SvcBadge svc={r.svc} />}</Td>
                <Td><strong>{r.sub.id}</strong>{spec.showExtension && <span className="ml-1.5 text-xs text-muted-foreground" title="내선 라벨(끝자리, 표시 전용) — 망 주소는 E.164">내선 {extensionOf(r.sub.id)}</span>}</Td>
                <Td>{ed ? <Input  type="password" placeholder="변경 시 입력" value={editForm.passwd || ''} onChange={e => setEditForm({ ...editForm, passwd: e.target.value })} /> : <span className="text-sm text-muted-foreground">••••</span>}</Td>
                <Td>{ed ? <Input  placeholder={spec.imsiAuto ? '비우면 번호 숫자' : 'SIM IMSI'} value={editForm.imsi || ''} onChange={e => setEditForm({ ...editForm, imsi: e.target.value })} /> : <span className="text-sm text-muted-foreground">{r.sub.imsi || '—'}</span>}</Td>
                <Td>{ed ? (editForm.auth_scheme === 'aka' ? <TransportFixedAka /> : <TransportSelect value={editForm.sip_transport} onChange={v => setEditForm({ ...editForm, sip_transport: v || null })} />) : <TransportBadge v={r.sub.sip_transport} aka={r.sub.auth_scheme === 'aka'} />}</Td>
                <Td>{ed ? <>
                  <AuthCell spec={spec} value={editForm.auth_scheme} onChange={v => setEditForm({ ...editForm, auth_scheme: v })} />
                  {editForm.auth_scheme === 'aka' && spec.authSchemes.includes('aka') && <AkaKeyInputs k={editForm.k || ''} opc={editForm.opc || ''} keep={!!r.sub.aka_provisioned} onChange={(k, opc) => setEditForm({ ...editForm, k, opc })} />}
                </> : <AuthBadge sub={r.sub} />}</Td>
                <Td className="text-center">{!spec.showDnd ? <span className="text-sm text-muted-foreground">—</span> : ed ? <Checkbox  checked={editForm.dnd || false} onCheckedChange={(c) => setEditForm({ ...editForm, dnd: (c === true) })} /> : (r.sub.dnd ? <Badge  variant="dangerSoft">ON</Badge> : <span className="text-sm text-muted-foreground">—</span>)}</Td>
                <Td>{!spec.showDnd ? <span className="text-sm text-muted-foreground">—</span> : ed ? <Input  placeholder="대상" value={editForm.forward_id || ''} onChange={e => setEditForm({ ...editForm, forward_id: e.target.value })} /> : <span className="text-sm text-muted-foreground">{r.sub.forward_id || '—'}</span>}</Td>
                <Td><PickupCell value={r.sub.pickup_group} /></Td>
                <Td className="flex gap-1.5">
                  {!canWrite ? <span className="text-sm text-muted-foreground">—</span> : ed ? <>
                    <IconBtn title="저장" tone="primary" onClick={() => saveEdit(r)}><Check size={ICON} /></IconBtn>
                    <IconBtn title="취소" onClick={() => setEditKey(null)}><X size={ICON} /></IconBtn>
                  </> : <>
                    <IconBtn title="편집" onClick={() => startEdit(r)}><Pencil size={ICON} /></IconBtn>
                    <IconBtn title="삭제" tone="danger" onClick={() => del(r)}><Trash2 size={ICON} /></IconBtn>
                  </>}
                </Td>
              </tr>
            )
          })}
          {adding && (
            <tr className="bg-brandsoft">
              <Td><Select value={toSel(addForm.svcCat)} onValueChange={(v: string) => changeAddCat(fromSel(v))}>
  <SelectTrigger><SelectValue /></SelectTrigger>
  <SelectContent>
                  {catalog.map(c => <SelectItem key={svcVal(c)} value={svcVal(c)}>{c.ref} ({LINE[c.svc].label})</SelectItem>)}
  </SelectContent>
</Select></Td>
              <Td><Input  placeholder={addSpec.msisdnPlaceholder} autoFocus value={addForm.id} onChange={e => setAddForm({ ...addForm, id: e.target.value })} />
                {addSpec.showExtension && addForm.id && <span className="ml-1.5 text-xs text-muted-foreground">내선 {extensionOf(addForm.id)}</span>}</Td>
              <Td><Input  type="password" placeholder={addForm.auth_scheme === 'aka' && addSpec.authSchemes.includes('aka') ? '암호(선택)' : '암호 *'} value={addForm.passwd} onChange={e => setAddForm({ ...addForm, passwd: e.target.value })} /></Td>
              <Td><Input  placeholder={addSpec.imsiAuto ? '비우면 번호 숫자' : 'SIM IMSI *'} value={addForm.imsi} onChange={e => setAddForm({ ...addForm, imsi: e.target.value })} /></Td>
              <Td>{addForm.auth_scheme === 'aka' && addSpec.authSchemes.includes('aka') ? <TransportFixedAka /> : <TransportSelect value={addForm.sip_transport} onChange={v => setAddForm({ ...addForm, sip_transport: v })} />}</Td>
              <Td>
                <AuthCell spec={addSpec} value={addForm.auth_scheme} onChange={v => setAddForm({ ...addForm, auth_scheme: v })} />
                {addForm.auth_scheme === 'aka' && addSpec.authSchemes.includes('aka') && <AkaKeyInputs k={addForm.k} opc={addForm.opc} onChange={(k, opc) => setAddForm({ ...addForm, k, opc })} />}
              </Td>
              <Td className="text-center">{addSpec.showDnd ? <Checkbox  checked={addForm.dnd} onCheckedChange={(c) => setAddForm({ ...addForm, dnd: (c === true) })} /> : <span className="text-sm text-muted-foreground">—</span>}</Td>
              <Td>{addSpec.showDnd ? <Input  placeholder="대상" value={addForm.forward_id} onChange={e => setAddForm({ ...addForm, forward_id: e.target.value })} /> : <span className="text-sm text-muted-foreground">—</span>}</Td>
              <Td><PickupCell value={null} /></Td>
              <Td className="flex gap-1.5">
                <Button variant="default" onClick={add}>추가</Button>
                <Button variant="ghost" onClick={() => { setAdding(false); setAddForm(newAdd()) }}>취소</Button>
              </Td>
            </tr>
          )}
        </tbody>
      </TableFrame>
      </div>
      {canWrite && !adding && (
        <Button className="text-primary text-sm mt-1" variant="ghost" onClick={() => { setAdding(true); setEditKey(null) }}><Plus size={13} /> 번호 추가</Button>
      )}
    </div>
  )
}

// ── 번호 탭 직접 추가 폼 (가입자 피커 + 번호 입력) — 필드 가감은 LINE 스펙이 정한다 ──
function NumberAddForm({ svc, catalog, userIndex, orgScope, orgPathOf, onAdded, onCancel }: {
  svc: LineSvc
  catalog: ServiceCat[]
  userIndex: PickItem[]
  orgScope: string | null
  orgPathOf: (code: string) => string
  onAdded: () => void
  onCancel: () => void
}) {
  const { show } = useToast()
  const spec = LINE[svc]
  const svcCatalog = catalog.filter(c => c.svc === svc)
  const [pick, setPick] = useState<PickItem | null>(null)
  const [serviceRef, setServiceRef] = useState(svcCatalog[0]?.ref || spec.defaultRef)
  const [msisdn, setMsisdn] = useState('')
  const [imsi, setImsi] = useState('')
  const [passwd, setPasswd] = useState('')
  const [sipTransport, setSipTransport] = useState<SipTransport | ''>(spec.defaultTransport)
  const [authScheme, setAuthScheme] = useState<AuthScheme>('digest')
  const [akaK, setAkaK] = useState('')
  const [akaOpc, setAkaOpc] = useState('')
  const [dnd, setDnd] = useState(false)
  const [forwardId, setForwardId] = useState('')
  const [busy, setBusy] = useState(false)
  const scheme: AuthScheme = spec.authSchemes.length === 1 ? spec.authSchemes[0] : authScheme
  const isAka = scheme === 'aka'

  async function add() {
    if (!pick) { show('가입자 선택 필수', 'err'); return }
    if (!msisdn) { show('MSISDN 필수', 'err'); return }
    const imsiVal = effectiveImsi(spec, imsi, msisdn)
    if (!imsiVal) { show('IMSI 필수', 'err'); return }
    if (!passwd && !isAka) { show('비밀번호 필수', 'err'); return }
    const aka = akaBody(scheme, akaK, akaOpc, false)
    if (aka.err) { show(aka.err, 'err'); return }
    const body: Partial<Subscription> = { id: msisdn, imsi: imsiVal, service_ref: serviceRef, sip_transport: sipTransport || null,
      dnd: spec.showDnd ? dnd : false, forward_id: spec.showDnd ? forwardId : '', ...aka.fields }
    if (passwd) body.passwd = passwd
    setBusy(true)
    try { await usersApi.addSub(Number(pick.value), svc, body); show('번호 추가', 'ok'); onAdded() }
    catch (e: unknown) { show(String(e), 'err') } finally { setBusy(false) }
  }

  return (
    <div className="flex flex-col gap-2.5">
      {/* 가입자 선택 */}
      <FieldRow>
        <Field label="가입자 *" w={280}>
          {pick
            ? <div className="flex items-center gap-2">
                <Badge className="text-xs" variant="brandSoft">{pick.label}</Badge>
                <Button variant="ghost" onClick={() => setPick(null)}>변경</Button>
              </div>
            : <SubscriberPicker kind="user" index={userIndex} orgScope={orgScope} orgPathOf={orgPathOf}
                onPick={setPick} placeholder="가입자 이름·로그인ID 검색·선택" autoFocus />}
        </Field>
      </FieldRow>
      {/* 번호 정보 */}
      <FieldRow>
        <Field label="서비스" w={150}>
          <Select value={toSel(serviceRef)} onValueChange={(v: string) => setServiceRef(fromSel(v))}>
            <SelectTrigger><SelectValue /></SelectTrigger>
            <SelectContent>
              {(svcCatalog.length ? svcCatalog : [{ svc, ref: serviceRef }]).map(c => <SelectItem key={c.ref} value={c.ref}>{c.ref}</SelectItem>)}
            </SelectContent>
          </Select>
        </Field>
        <Field label="MSISDN *" w={150}><Input  placeholder={spec.msisdnPlaceholder} value={msisdn} onChange={e => setMsisdn(e.target.value)} /></Field>
        {spec.showExtension && <Field label="내선" w={70}><span className="text-sm text-muted-foreground py-1.5" title="내선 라벨(끝자리, 표시 전용) — 망 주소는 E.164">{msisdn ? extensionOf(msisdn) : '—'}</span></Field>}
        <Field label={spec.imsiAuto ? 'IMSI' : 'IMSI *'} w={170}><Input  placeholder={spec.imsiAuto ? '비우면 번호 숫자' : 'SIM IMSI'} value={imsi} onChange={e => setImsi(e.target.value)} /></Field>
        <Field label={isAka ? '암호' : '암호 *'} w={120}><Input  type="password" value={passwd} onChange={e => setPasswd(e.target.value)} /></Field>
        <Field label="채널" w={110}>{isAka ? <TransportFixedAka /> : <TransportSelect value={sipTransport} onChange={setSipTransport} />}</Field>
        <Field label="인증" w={100}><AuthCell spec={spec} value={authScheme} onChange={setAuthScheme} /></Field>
        {isAka && <Field label="K / OPc *" w={300}><AkaKeyInputs k={akaK} opc={akaOpc} onChange={(k, opc) => { setAkaK(k); setAkaOpc(opc) }} /></Field>}
        {spec.showDnd && <Field label="DND" w={56}><Checkbox className="mt-1.5" checked={dnd} onCheckedChange={(c) => setDnd((c === true))} /></Field>}
        {spec.showDnd && <Field label="착신전환" w={130}><Input  placeholder="대상" value={forwardId} onChange={e => setForwardId(e.target.value)} /></Field>}
        <Field label="픽업그룹" w={110}><PickupCell value={null} /></Field>
        <div className="flex gap-1.5 items-center">
          <Button variant="default" disabled={busy} onClick={add}>추가</Button>
          <Button variant="ghost" onClick={onCancel}>취소</Button>
        </div>
      </FieldRow>
    </div>
  )
}

// ── 통합 Excel import 모달 (사용자+VoLTE+VoIP+PTT) ──
function ImportModal({ onClose, onDone }: { onClose: () => void; onDone: () => void }) {
  const { show } = useToast()
  const [result, setResult] = useState<ImportResult | null>(null)
  const [busy, setBusy] = useState(false)
  async function onFile(e: React.ChangeEvent<HTMLInputElement>) {
    const f = e.target.files?.[0]; if (!f) return
    setBusy(true); setResult(null)
    try {
      const buf = await f.arrayBuffer()
      const b64 = btoa(String.fromCharCode(...new Uint8Array(buf)))
      const r = await usersApi.importExcel(b64)
      setResult(r)
      if (r.created_users + (r.created_volte || 0) + r.created_voip + r.created_ptt > 0) onDone()
    } catch (err: unknown) { show(String(err), 'err') }
    finally { setBusy(false); e.target.value = '' }
  }
  return (
    <Modal title="사용자·번호 Excel 가져오기" onClose={onClose}>
        <div>
          <p className="mb-3">사용자 + VoLTE/VoIP/PTT 번호를 한 Excel(.xlsx)로 일괄 등록합니다 (시트 = 회선 종류).</p>
          <div className="flex gap-3 items-center mb-4">
            <Button asChild variant="default" size="default">
              <label className="cursor-pointer">파일 선택<input className="hidden" type="file" accept=".xlsx" onChange={onFile}/></label>
            </Button>
            <Button asChild size="default"><a href={usersApi.templateUrl} download>템플릿 다운로드</a></Button>
            {busy && <span className="text-sm text-muted-foreground">처리 중...</span>}
          </div>
          {result && (
            <div className="bg-card rounded-md p-4 text-md">
              <div className="font-semibold mb-2">결과</div>
              <div>사용자 <strong>{result.created_users}</strong> · VoLTE <strong>{result.created_volte || 0}</strong> · VoIP <strong>{result.created_voip}</strong> · PTT <strong>{result.created_ptt}</strong></div>
              {result.errors.length > 0 && (
                <div className="mt-2 text-destructive text-sm">
                  {result.errors.map((er, i) => <div key={i}>[{er.sheet}] 행 {er.row}: {er.error}</div>)}
                </div>
              )}
              {(result.credentials?.length ?? 0) > 0 && (
                <div className="mt-2.5 text-sm">
                  <div className="font-semibold mb-1">생성된 비밀번호 <span className="text-sm text-muted-foreground font-normal">— 서버는 H(A1) 만 저장하므로 지금 기록하지 않으면 복구할 수 없습니다</span></div>
                  <div className="flex-1 overflow-x-auto max-h-[220px] overflow-auto">
                    <TableFrame sticky className="[&_td]:text-sm">
                      <thead><tr><Th>시트</Th><Th>행</Th><Th>MSISDN</Th><Th>비밀번호</Th></tr></thead>
                      <tbody>{result.credentials!.map((c, i) => <tr key={i}><Td>{c.sheet}</Td><Td>{c.row}</Td><Td>{c.msisdn}</Td><Td><code>{c.password}</code></Td></tr>)}</tbody>
                    </TableFrame>
                  </div>
                </div>
              )}
            </div>
          )}
        </div>
        <div className="flex justify-end gap-2.5 pt-5"><Button variant="ghost" size="default" onClick={onClose}>닫기</Button></div>
    </Modal>
  )
}
