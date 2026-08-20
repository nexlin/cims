import { useState, useEffect, useCallback, useMemo } from 'react'
import IconBtn from '@core/components/IconBtn'
import { Pencil, Trash2, Check, X, ChevronRight, ChevronDown } from 'lucide-react'
import { usersApi, type UserSummary, type Subscription, type UserInput, type McpttProfile, type SipTransport, type ImportResult } from '@core/api/users'
import { groupsApi, type Group } from '@core/api/groups'
import { orgApi, type Organization } from '@core/api/organizations'
import OrgTreePanel from '@core/components/OrgTreePanel'
import { DataTable, type Column } from '@core/components/DataTable'
import SubscriberPicker, { buildPickIndex, type PickItem } from '@core/components/SubscriberPicker'
import { useToast } from '@core/components/Toast'
import { useAuth } from '@core/contexts/AuthContext'
import { canWriteConfig } from '@core/utils/permissions'

// ── 사용자 프로비저닝 워크벤치 (사용자 = 가입, 번호 등록이 가입 행위) ──────────
//  좌: 조직트리(공유 스코프) | 상단 탭: 사용자/VoLTE 번호/PTT 번호.
//  편집은 '행 펼침 상세' 단일 패러다임으로 통일 — 행 클릭 → 상세(기본정보 편집 + 번호 서브테이블).
//  번호는 사용자 종속(child) — 별도 메뉴 없이 사용자 하위로 관리. PTT 그룹은 별도 메뉴.

type Tab = 'users' | 'volte' | 'ptt'

// 번호 탭의 평탄화 행
interface NumberRow { msisdn: string; svc: 'call' | 'ptt'; user: UserSummary; sub: Subscription }

// 펼침 상태 — 어느 행이 펼쳐졌는지(key) + 그 사용자(userId) + 초기 편집모드 + 강조할 번호
type Expand = { key: string | number; userId: number; edit: boolean; hi?: string } | null

const ICON = 14

// 작은 아이콘 액션 버튼

// 펼침 표시 caret (열림/닫힘)
function Caret({ open }: { open: boolean }) {
  return <span style={{ color: 'var(--text-muted)', display: 'inline-flex' }}>
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
  const [addNumSvc, setAddNumSvc] = useState<'call' | 'ptt' | null>(null)

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
    u.call_subscriptions.some(s => s.id.toLowerCase().includes(q)) ||
    u.ptt_subscriptions.some(s => s.id.toLowerCase().includes(q)), [])

  // ── 탭 데이터 ──
  const userRows = useMemo(() => {
    const q = search.trim().toLowerCase()
    return users.filter(u => inScope(u.org_id || '') &&
      (!q || u.name.toLowerCase().includes(q) || (u.title || '').toLowerCase().includes(q) || userHasNumber(u, q)))
  }, [users, inScope, search, userHasNumber])

  const buildNumberRows = useCallback((svc: 'call' | 'ptt'): NumberRow[] => {
    const q = search.trim().toLowerCase()
    const out: NumberRow[] = []
    for (const u of users) {
      if (!inScope(u.org_id || '')) continue
      const subs = svc === 'call' ? u.call_subscriptions : u.ptt_subscriptions
      for (const sub of subs) out.push({ msisdn: sub.id, svc, user: u, sub })
    }
    return out.filter(r => !q || r.msisdn.toLowerCase().includes(q) || r.user.name.toLowerCase().includes(q))
  }, [users, inScope, search])
  const volteRows = useMemo(() => buildNumberRows('call'), [buildNumberRows])
  const pttRows = useMemo(() => buildNumberRows('ptt'), [buildNumberRows])

  // ── 삭제 ──
  async function batchDeleteUsers() {
    const ids = Array.from(selected).map(Number)
    if (!ids.length || !confirm(`${ids.length}명을 삭제합니다. 연결된 번호도 삭제됩니다.`)) return
    try { await usersApi.batchDelete(ids); show('삭제 완료', 'ok'); setSelected(new Set()); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function deleteUser(u: UserSummary) {
    if (!confirm(`${u.name} 삭제? 연결된 번호도 삭제됩니다.`)) return
    try { await usersApi.delete(u.id); show('삭제', 'ok'); if (exp?.userId === u.id) setExp(null); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function deleteNumber(r: NumberRow) {
    if (!confirm(`${r.msisdn} 삭제?`)) return
    try { await usersApi.deleteSub(r.user.id, r.svc, r.msisdn); show('삭제', 'ok'); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  // ── 컬럼 정의 ──
  const userCols: Column<UserSummary>[] = [
    { key: 'exp', header: '', width: 26, render: u => <Caret open={exp?.key === u.id} /> },
    { key: 'name', header: '이름', sortable: true, width: 130, render: u => <span style={{ fontWeight: 500 }}>{u.name}</span> },
    { key: 'title', header: '직함', sortable: true, width: 90, sortValue: u => u.title || '', render: u => <span className="ts">{u.title || '—'}</span> },
    { key: 'login_id', header: '로그인ID', sortable: true, width: 110, sortValue: u => u.login_id || '', render: u => <span className="ts" title="단말 로그인 ID">{u.login_id || '—'}</span> },
    { key: 'org', header: '조직', width: 220, sortValue: u => buildOrgPath(orgs, u.org_id), render: u => <span className="ts" title={buildOrgPath(orgs, u.org_id)}>{buildOrgPath(orgs, u.org_id)}</span> },
    { key: 'details', header: '설명', render: u => <span className="ts">{u.details || '—'}</span> },
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
    { key: 'act', header: '', width: 84, align: 'right', render: u => canWrite ? (
      <span className="actions" onClick={e => e.stopPropagation()}>
        <IconBtn title="편집" onClick={() => openEdit(u.id, u.id)}><Pencil size={ICON} /></IconBtn>
        <IconBtn title="삭제" tone="danger" onClick={() => deleteUser(u)}><Trash2 size={ICON} /></IconBtn>
      </span>
    ) : <span className="ts">—</span> },
  ]

  const numberActCol: Column<NumberRow> = { key: 'act', header: '', width: 64, align: 'right', render: r => canWrite ? (
    <span className="actions" onClick={e => e.stopPropagation()}>
      <IconBtn title="삭제" tone="danger" onClick={() => deleteNumber(r)}><Trash2 size={ICON} /></IconBtn>
    </span>
  ) : <span className="ts">—</span> }
  const numberBaseCols: Column<NumberRow>[] = [
    { key: 'exp', header: '', width: 26, render: r => <Caret open={exp?.key === r.msisdn} /> },
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

  const TABS: Array<{ k: Tab; label: string; count: number }> = [
    { k: 'users', label: '사용자', count: userRows.length },
    { k: 'volte', label: 'VoLTE 번호', count: volteRows.length },
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
    <div style={{ display: 'flex', gap: 16, alignItems: 'stretch', height: 'calc(100vh - 92px)' }}>
      {/* 좌: 조직 트리 (공유 스코프) */}
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
          {search && <button className="btn btn--ghost btn--sm" onClick={() => setSearch('')}>✕</button>}
          <span style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
            {tab === 'users' && canWrite && <>
              <button className="btn btn--outline btn--sm" onClick={() => setImportOpen(true)}>Excel 가져오기</button>
              {selected.size > 0 && <button className="btn btn--danger btn--sm" onClick={batchDeleteUsers}>선택 삭제 ({selected.size})</button>}
              <button className="btn btn--primary btn--sm" onClick={() => { setAddUserOpen(v => !v); setExp(null) }}>＋ 사용자</button>
            </>}
            {(tab === 'volte' || tab === 'ptt') && canWrite && (
              <button className="btn btn--primary btn--sm" onClick={() => { setAddNumSvc(tab === 'volte' ? 'call' : 'ptt'); setExp(null) }}>
                ＋ {tab === 'volte' ? 'VoLTE' : 'PTT'} 번호
              </button>
            )}
          </span>
        </div>

        {/* 추가 폼 블록 (테이블 위) */}
        {tab === 'users' && addUserOpen && (
          <div style={{ borderBottom: '1px solid var(--border)', background: 'var(--bg-soft)', padding: '10px 16px' }}>
            <div style={{ fontWeight: 600, fontSize: 12, color: 'var(--primary)', marginBottom: 8 }}>새 사용자</div>
            <UserBasicForm mode="add" orgOpts={orgOpts}
              defaultOrg={orgScope ? (orgScope.split('/').pop() || '') : ''}
              onSubmit={async (input) => { await usersApi.create(input); show('생성', 'ok'); setAddUserOpen(false); load() }}
              onCancel={() => setAddUserOpen(false)} />
          </div>
        )}
        {(tab === 'volte' || tab === 'ptt') && addNumSvc && (
          <div style={{ borderBottom: '1px solid var(--border)', background: 'var(--bg-soft)', padding: '10px 16px' }}>
            <div style={{ fontWeight: 600, fontSize: 12, color: 'var(--primary)', marginBottom: 8 }}>새 {addNumSvc === 'call' ? 'VoLTE' : 'PTT'} 번호</div>
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
        {tab === 'volte' && (
          <DataTable<NumberRow> columns={volteCols} rows={volteRows} rowKey={r => r.msisdn} loading={loading}
            onRowClick={r => toggleExpand(r.msisdn, r.user.id, r.msisdn)}
            expandedKey={exp?.key ?? null}
            renderExpanded={exp && expUser ? renderDetail : undefined}
            pageSize={50} emptyText="VoLTE 번호 없음" />
        )}
        {tab === 'ptt' && (
          <DataTable<NumberRow> columns={pttCols} rows={pttRows} rowKey={r => r.msisdn} loading={loading}
            onRowClick={r => toggleExpand(r.msisdn, r.user.id, r.msisdn)}
            expandedKey={exp?.key ?? null}
            renderExpanded={exp && expUser ? renderDetail : undefined}
            pageSize={50} emptyText="PTT 번호 없음" />
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
      <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>{label}</span>
      {children}
    </label>
  )
}
function FieldRow({ children }: { children: React.ReactNode }) {
  return <div style={{ display: 'flex', flexWrap: 'wrap', gap: '8px 12px', alignItems: 'flex-end' }}>{children}</div>
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
      <Field label="이름 *" w={150}><input className="form-input" autoFocus value={form.name} onChange={e => setForm({ ...form, name: e.target.value })} /></Field>
      <Field label="직함" w={110}><input className="form-input" placeholder="예: 팀장" value={form.title || ''} onChange={e => setForm({ ...form, title: e.target.value })} /></Field>
      <Field label="로그인 ID" w={130}><input className="form-input" placeholder="예: test001" value={form.login_id || ''} onChange={e => setForm({ ...form, login_id: e.target.value })} /></Field>
      <Field label={mode === 'add' ? '비밀번호' : '비밀번호(변경 시)'} w={140}><input className="form-input" type="password" placeholder={mode === 'add' ? '' : '미변경'} value={form.passwd || ''} onChange={e => setForm({ ...form, passwd: e.target.value })} /></Field>
      <Field label="조직" w={200}>
        <select className="form-input" value={form.org_id} onChange={e => setForm({ ...form, org_id: e.target.value })}>
          <option value="">없음</option>
          {orgOpts.map(o => <option key={o.code} value={o.code}>{o.label}</option>)}
        </select>
      </Field>
      <Field label="설명"><input className="form-input" value={form.details || ''} onChange={e => setForm({ ...form, details: e.target.value })} /></Field>
      <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
        <button className="btn btn--sm btn--primary" disabled={busy} onClick={submit}>{mode === 'add' ? '생성' : '저장'}</button>
        <button className="btn btn--sm btn--ghost" onClick={onCancel}>취소</button>
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
    <div style={{ padding: '12px 16px' }}>
      {/* 기본정보 */}
      {editing ? (
        <UserBasicForm mode="edit" initial={user} orgOpts={orgOpts}
          onSubmit={async (input) => { await usersApi.update(user.id, input); show('저장', 'ok'); setEditing(false); onReload() }}
          onCancel={() => setEditing(false)} />
      ) : (
        <div style={{ display: 'flex', alignItems: 'center', gap: 16, flexWrap: 'wrap', fontSize: 12 }}>
          <span><b style={{ fontSize: 13 }}>{user.name}</b>{user.title && <span className="ts" style={{ marginLeft: 6 }}>{user.title}</span>}</span>
          <span className="ts">조직 {orgPath}</span>
          {user.details && <span className="ts">{user.details}</span>}
          {canWrite && <button className="btn btn--sm btn--outline" style={{ marginLeft: 'auto' }} onClick={() => setEditing(true)}>기본정보 편집</button>}
        </div>
      )}

      {/* 번호 */}
      <div style={{ marginTop: 12, borderTop: '1px solid var(--border)', paddingTop: 10 }}>
        <div style={{ fontWeight: 600, fontSize: 12, color: 'var(--text-muted)', marginBottom: 6 }}>번호</div>
        <NumbersTable user={user} catalog={catalog} canWrite={canWrite} highlight={highlight} onReload={onReload} />
      </div>

      {/* MCPTT 프로파일 — SOS 대상 결정(TS 24.484 entry-info)·사용자 단위 개시 인가 */}
      {user.ptt_subscriptions.length > 0 && (
        <div style={{ marginTop: 12, borderTop: '1px solid var(--border)', paddingTop: 10 }}>
          <div style={{ fontWeight: 600, fontSize: 12, color: 'var(--text-muted)', marginBottom: 6 }}>MCPTT 프로파일 (SOS 대상·개시 인가)</div>
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
    try { await usersApi.updatePttProfile(pid, msisdn, form); show('저장', 'ok'); setEditing(false); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  if (!prof) return <div className="ts" style={{ fontSize: 12 }}>{msisdn} — 프로파일 조회 실패(서버 구버전?)</div>

  if (editing && form) {
    return (
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap', fontSize: 12, padding: '4px 0' }}>
        <strong>{msisdn}</strong>
        <label className="ts">SOS 대상
          <select className="form-input" style={{ marginLeft: 4 }} value={form.emergency_group_mode}
            onChange={e => setForm({ ...form, emergency_group_mode: e.target.value as McpttProfile['emergency_group_mode'] })}>
            <option value="DedicatedGroup">{MODE_LABEL.DedicatedGroup}</option>
            <option value="UseCurrentlySelectedGroup">{MODE_LABEL.UseCurrentlySelectedGroup}</option>
          </select>
        </label>
        {form.emergency_group_mode === 'DedicatedGroup' && (
          <label className="ts">긴급그룹
            <select className="form-input" style={{ marginLeft: 4 }} value={form.emergency_group_id || ''}
              onChange={e => setForm({ ...form, emergency_group_id: e.target.value || null })}>
              <option value="">(미지정)</option>
              {groups.map(g => <option key={g.id} value={g.id}>{g.name || g.id}</option>)}
            </select>
          </label>
        )}
        <label className="ts"><input type="checkbox" checked={form.allow_emergency_call}
          onChange={e => setForm({ ...form, allow_emergency_call: e.target.checked })} /> 긴급콜</label>
        <label className="ts"><input type="checkbox" checked={form.allow_emergency_alert}
          onChange={e => setForm({ ...form, allow_emergency_alert: e.target.checked })} /> 긴급경보</label>
        <label className="ts"><input type="checkbox" checked={form.allow_adhoc_call}
          onChange={e => setForm({ ...form, allow_adhoc_call: e.target.checked })} /> 애드혹</label>
        <IconBtn title="저장" tone="primary" onClick={save}><Check size={ICON} /></IconBtn>
        <IconBtn title="취소" onClick={() => setEditing(false)}><X size={ICON} /></IconBtn>
      </div>
    )
  }

  const noDedicated = prof.emergency_group_mode === 'DedicatedGroup' && !prof.emergency_group_id
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap', fontSize: 12, padding: '4px 0' }}>
      <strong>{msisdn}</strong>
      <span className="badge badge--blue" style={{ fontSize: 9 }}>{MODE_LABEL[prof.emergency_group_mode]}</span>
      {prof.emergency_group_mode === 'DedicatedGroup' && (
        noDedicated
          ? <span className="badge badge--red" style={{ fontSize: 9 }}>긴급그룹 미지정 — SOS 불발</span>
          : <span className="ts">긴급그룹 <b>{prof.emergency_group_id}</b></span>
      )}
      {!prof.allow_emergency_call && <span className="badge badge--red" style={{ fontSize: 9 }}>긴급콜 차단</span>}
      {!prof.allow_emergency_alert && <span className="badge badge--red" style={{ fontSize: 9 }}>경보 차단</span>}
      {!prof.allow_adhoc_call && <span className="badge badge--red" style={{ fontSize: 9 }}>애드혹 차단</span>}
      {!prof.exists && <span className="ts">(기본값)</span>}
      {canWrite && (
        <IconBtn title="편집" onClick={() => { setForm({
          allow_emergency_call: prof.allow_emergency_call,
          allow_emergency_alert: prof.allow_emergency_alert,
          allow_adhoc_call: prof.allow_adhoc_call,
          emergency_group_mode: prof.emergency_group_mode,
          emergency_group_id: prof.emergency_group_id,
        }); setEditing(true) }}><Pencil size={ICON} /></IconBtn>
      )}
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

// VoLTE/PTT 유형 배지
function SvcBadge({ svc }: { svc: 'call' | 'ptt' }) {
  return <span className={`badge ${svc === 'call' ? 'badge--blue' : 'badge--green'}`} style={{ fontSize: 9 }}>{svc === 'call' ? 'VoLTE' : 'McPTT'}</span>
}

interface AddNum { id: string; imsi: string; svcCat: string; passwd: string; sip_transport: SipTransport | ''; dnd: boolean; forward_id: string }

// 채널 정책 선택 — 값의 의미는 Subscription.sip_transport 주석 참조
const TRANSPORT_OPTS: Array<{ v: SipTransport | ''; label: string }> = [
  { v: '', label: '자유' }, { v: 'UDP', label: 'UDP' }, { v: 'TCP', label: 'TCP' }, { v: 'TLS', label: 'TLS (강제)' },
]
function TransportSelect({ value, onChange }: { value: SipTransport | '' | null | undefined; onChange: (v: SipTransport | '') => void }) {
  return <select className="form-input" title="TLS=서버 집행(비-TLS 요청 403) / UDP·TCP=단말 힌트 / 자유=단말 선택" value={value || ''} onChange={e => onChange(e.target.value as SipTransport | '')}>
    {TRANSPORT_OPTS.map(o => <option key={o.v} value={o.v}>{o.label}</option>)}
  </select>
}
function TransportBadge({ v }: { v?: SipTransport | null }) {
  if (!v) return <span className="ts">자유</span>
  return <span className={`badge ${v === 'TLS' ? 'badge--red' : 'badge--blue'}`} style={{ fontSize: 9 }} title={v === 'TLS' ? '서버 집행 — 비-TLS 채널 요청 403' : '프로비저닝 힌트'}>{v}</span>
}

// ── 단일 번호 테이블 (사용자 상세 내부, VoLTE+PTT 통합) ──
function NumbersTable({ user, catalog, canWrite, highlight, onReload }: { user: UserSummary; catalog: ServiceCat[]; canWrite: boolean; highlight?: string; onReload: () => void }) {
  const { show } = useToast()
  const rows: Array<{ svc: 'call' | 'ptt'; sub: Subscription }> = [
    ...user.call_subscriptions.map(s => ({ svc: 'call' as const, sub: s })),
    ...user.ptt_subscriptions.map(s => ({ svc: 'ptt' as const, sub: s })),
  ]
  const svcVal = (c: ServiceCat) => `${c.svc}:${c.ref}`
  const rk = (svc: 'call' | 'ptt', msisdn: string) => `${svc}:${msisdn}`
  const newAdd = (): AddNum => ({ id: '', imsi: '', svcCat: catalog[0] ? svcVal(catalog[0]) : 'call:volte', passwd: '', sip_transport: '', dnd: false, forward_id: '' })

  const [editKey, setEditKey] = useState<string | null>(null)
  const [editForm, setEditForm] = useState<Partial<Subscription>>({})
  const [adding, setAdding] = useState(false)
  const [addForm, setAddForm] = useState<AddNum>(newAdd())

  function startEdit(r: { svc: 'call' | 'ptt'; sub: Subscription }) {
    setAdding(false); setEditKey(rk(r.svc, r.sub.id))
    setEditForm({ imsi: r.sub.imsi || '', service_ref: r.sub.service_ref || '', passwd: '', sip_transport: r.sub.sip_transport || null, dnd: r.sub.dnd, forward_id: r.sub.forward_id })
  }
  async function saveEdit(r: { svc: 'call' | 'ptt'; sub: Subscription }) {
    // passwd 는 변경 시에만 전송. imsi/service_ref 가 바뀌면 서버가 passwd 를 요구한다(H(A1) 결박).
    const d: Partial<Subscription> = { ...editForm }; if (!d.passwd) delete d.passwd
    if (!d.passwd && ((d.imsi || '') !== (r.sub.imsi || '') || (d.service_ref || '') !== (r.sub.service_ref || ''))) { show('IMSI/서비스 변경 시 비밀번호를 함께 입력해야 합니다 (H(A1) 재결박)', 'err'); return }
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
    if (!addForm.passwd) { show('비밀번호 필수', 'err'); return }
    const [svc, ref] = addForm.svcCat.split(':') as ['call' | 'ptt', string]
    const body: Partial<Subscription> = { id: addForm.id, imsi: addForm.imsi, service_ref: ref, passwd: addForm.passwd, sip_transport: addForm.sip_transport || null, dnd: addForm.dnd, forward_id: addForm.forward_id }
    try { await usersApi.addSub(user.id, svc, body); show('추가', 'ok'); setAdding(false); setAddForm(newAdd()); onReload() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  const addSvc = addForm.svcCat.split(':')[0] as 'call' | 'ptt'
  const addIsCall = addSvc === 'call'

  return (
    <div>
      <div className="table-wrap">
      <table className="data-table" style={{ fontSize: 12 }}>
        <thead>
          <tr>
            <th style={{ width: 150 }}>서비스</th>
            <th style={{ width: 140 }}>MSISDN</th>
            <th style={{ width: 100 }}>비밀번호</th>
            <th>IMSI</th>
            <th style={{ width: 96 }} title="TLS=서버 집행(비-TLS 채널 요청 403) / UDP·TCP=단말 힌트 / 자유=단말 선택">채널</th>
            <th style={{ width: 56, textAlign: 'center' }}>DND</th>
            <th style={{ width: 110 }}>착신전환</th>
            <th style={{ width: 110 }}></th>
          </tr>
        </thead>
        <tbody>
          {rows.length === 0 && !adding && <tr><td colSpan={8} className="empty-cell" style={{ padding: 12 }}>번호 없음 — 아래 ＋ 번호 추가</td></tr>}
          {rows.map(r => {
            const ed = editKey === rk(r.svc, r.sub.id)
            const isCall = r.svc === 'call'
            const hi = highlight && r.sub.id === highlight
            return (
              <tr key={rk(r.svc, r.sub.id)} style={{ background: hi && !ed ? 'rgba(74,144,217,0.10)' : undefined }}>
                <td>{ed
                  ? <select className="form-input" value={editForm.service_ref || ''} onChange={e => setEditForm({ ...editForm, service_ref: e.target.value })}>
                      {catalog.filter(c => c.svc === r.svc).map(c => <option key={c.ref} value={c.ref}>{c.ref}</option>)}
                    </select>
                  : <SvcBadge svc={r.svc} />}</td>
                <td><strong>{r.sub.id}</strong></td>
                <td>{ed ? <input className="form-input" type="password" placeholder="변경 시 입력" value={editForm.passwd || ''} onChange={e => setEditForm({ ...editForm, passwd: e.target.value })} /> : <span className="ts">••••</span>}</td>
                <td>{ed ? <input className="form-input" placeholder="SIM IMSI" value={editForm.imsi || ''} onChange={e => setEditForm({ ...editForm, imsi: e.target.value })} /> : <span className="ts">{r.sub.imsi || '—'}</span>}</td>
                <td>{ed ? <TransportSelect value={editForm.sip_transport} onChange={v => setEditForm({ ...editForm, sip_transport: v || null })} /> : <TransportBadge v={r.sub.sip_transport} />}</td>
                <td style={{ textAlign: 'center' }}>{!isCall ? <span className="ts">—</span> : ed ? <input type="checkbox" checked={editForm.dnd || false} onChange={e => setEditForm({ ...editForm, dnd: e.target.checked })} /> : (r.sub.dnd ? <span className="badge badge--red" style={{ fontSize: 9 }}>ON</span> : <span className="ts">—</span>)}</td>
                <td>{!isCall ? <span className="ts">—</span> : ed ? <input className="form-input" placeholder="대상" value={editForm.forward_id || ''} onChange={e => setEditForm({ ...editForm, forward_id: e.target.value })} /> : <span className="ts">{r.sub.forward_id || '—'}</span>}</td>
                <td className="actions">
                  {!canWrite ? <span className="ts">—</span> : ed ? <>
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
              <td><input className="form-input" type="password" placeholder="암호 *" value={addForm.passwd} onChange={e => setAddForm({ ...addForm, passwd: e.target.value })} /></td>
              <td><input className="form-input" placeholder="SIM IMSI *" value={addForm.imsi} onChange={e => setAddForm({ ...addForm, imsi: e.target.value })} /></td>
              <td><TransportSelect value={addForm.sip_transport} onChange={v => setAddForm({ ...addForm, sip_transport: v })} /></td>
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
      {canWrite && !adding && (
        <button className="btn btn--ghost btn--sm" style={{ color: 'var(--primary)', fontSize: 12, marginTop: 4 }} onClick={() => { setAdding(true); setEditKey(null) }}>＋ 번호 추가</button>
      )}
    </div>
  )
}

// ── 번호 탭 직접 추가 폼 (가입자 피커 + 번호 입력) ──
function NumberAddForm({ svc, catalog, userIndex, orgScope, orgPathOf, onAdded, onCancel }: {
  svc: 'call' | 'ptt'
  catalog: ServiceCat[]
  userIndex: PickItem[]
  orgScope: string | null
  orgPathOf: (code: string) => string
  onAdded: () => void
  onCancel: () => void
}) {
  const { show } = useToast()
  const svcCatalog = catalog.filter(c => c.svc === svc)
  const [pick, setPick] = useState<PickItem | null>(null)
  const [serviceRef, setServiceRef] = useState(svcCatalog[0]?.ref || (svc === 'call' ? 'volte' : 'mcptt'))
  const [msisdn, setMsisdn] = useState('')
  const [imsi, setImsi] = useState('')
  const [passwd, setPasswd] = useState('')
  const [sipTransport, setSipTransport] = useState<SipTransport | ''>('')
  const [dnd, setDnd] = useState(false)
  const [forwardId, setForwardId] = useState('')
  const [busy, setBusy] = useState(false)
  const isCall = svc === 'call'

  async function add() {
    if (!pick) { show('가입자 선택 필수', 'err'); return }
    if (!msisdn) { show('MSISDN 필수', 'err'); return }
    if (!imsi) { show('IMSI 필수', 'err'); return }
    if (!passwd) { show('비밀번호 필수', 'err'); return }
    const body: Partial<Subscription> = { id: msisdn, imsi, service_ref: serviceRef, passwd, sip_transport: sipTransport || null, dnd, forward_id: forwardId }
    setBusy(true)
    try { await usersApi.addSub(Number(pick.value), svc, body); show('번호 추가', 'ok'); onAdded() }
    catch (e: unknown) { show(String(e), 'err') } finally { setBusy(false) }
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
      {/* 가입자 선택 */}
      <FieldRow>
        <Field label="가입자 *" w={280}>
          {pick
            ? <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                <span className="badge badge--blue" style={{ fontSize: 11 }}>{pick.label}</span>
                <button className="btn btn--ghost btn--sm" onClick={() => setPick(null)}>변경</button>
              </div>
            : <SubscriberPicker kind="user" index={userIndex} orgScope={orgScope} orgPathOf={orgPathOf}
                onPick={setPick} placeholder="가입자 이름·로그인ID 검색·선택" autoFocus />}
        </Field>
      </FieldRow>
      {/* 번호 정보 */}
      <FieldRow>
        <Field label="서비스" w={150}>
          <select className="form-input" value={serviceRef} onChange={e => setServiceRef(e.target.value)}>
            {(svcCatalog.length ? svcCatalog : [{ svc, ref: serviceRef }]).map(c => <option key={c.ref} value={c.ref}>{c.ref}</option>)}
          </select>
        </Field>
        <Field label="MSISDN *" w={150}><input className="form-input" placeholder={isCall ? '+8213…' : '+825…'} value={msisdn} onChange={e => setMsisdn(e.target.value)} /></Field>
        <Field label="IMSI *" w={170}><input className="form-input" placeholder="SIM IMSI" value={imsi} onChange={e => setImsi(e.target.value)} /></Field>
        <Field label="암호 *" w={120}><input className="form-input" type="password" value={passwd} onChange={e => setPasswd(e.target.value)} /></Field>
        <Field label="채널" w={110}><TransportSelect value={sipTransport} onChange={setSipTransport} /></Field>
        {isCall && <Field label="DND" w={56}><input type="checkbox" checked={dnd} onChange={e => setDnd(e.target.checked)} style={{ marginTop: 6 }} /></Field>}
        {isCall && <Field label="착신전환" w={130}><input className="form-input" placeholder="대상" value={forwardId} onChange={e => setForwardId(e.target.value)} /></Field>}
        <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
          <button className="btn btn--sm btn--primary" disabled={busy} onClick={add}>추가</button>
          <button className="btn btn--sm btn--ghost" onClick={onCancel}>취소</button>
        </div>
      </FieldRow>
    </div>
  )
}

// ── 통합 Excel import 모달 (사용자+VoLTE+PTT) ──
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
              {(result.credentials?.length ?? 0) > 0 && (
                <div style={{ marginTop: 10, fontSize: 12 }}>
                  <div style={{ fontWeight: 600, marginBottom: 4 }}>생성된 비밀번호 <span className="ts" style={{ fontWeight: 400 }}>— 서버는 H(A1) 만 저장하므로 지금 기록하지 않으면 복구할 수 없습니다</span></div>
                  <div className="table-wrap" style={{ maxHeight: 220, overflow: 'auto' }}>
                    <table className="data-table" style={{ fontSize: 12 }}>
                      <thead><tr><th>시트</th><th>행</th><th>MSISDN</th><th>비밀번호</th></tr></thead>
                      <tbody>{result.credentials!.map((c, i) => <tr key={i}><td>{c.sheet}</td><td>{c.row}</td><td>{c.msisdn}</td><td><code>{c.password}</code></td></tr>)}</tbody>
                    </table>
                  </div>
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
