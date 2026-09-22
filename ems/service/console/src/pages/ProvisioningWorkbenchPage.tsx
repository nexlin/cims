import { useConfirm } from '@core/components/custom/confirm'
import { Fragment, useState, useEffect, useCallback, useMemo, useRef } from 'react'
import IconBtn from '@core/components/IconBtn'
import { AlertTriangle, ChevronRight, Pencil, Plus, Trash2, Upload, X } from 'lucide-react'
import { usersApi, type UserSummary, type Subscription, type UserInput, type McpttProfile, type SipTransport, type AuthScheme, type ImportResult, type LineSvc } from '@core/api/users'
import { groupsApi, type Group } from '@core/api/groups'
import { phoneGroupsApi, type PhoneGroup } from '@core/api/phoneGroups'
import { rolesApi, type RoleDef } from '@core/api/roles'
import { orgApi, type Organization } from '@core/api/organizations'
import OrgTreePanel from '@core/components/OrgTreePanel'
import { DataTable, type Column } from '@core/components/DataTable'
import { useToast } from '@core/components/Toast'
import { useAuth } from '@core/contexts/AuthContext'
import { canWriteConfig } from '@core/utils/permissions'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { NONE, fromSel, toSel } from '@core/components/custom/select-value'
import { DataTable as TableFrame, Th, Td } from '@core/components/custom/data-table'
import { Badge } from '@core/components/ui/badge'
import { Tabs, TabsList, TabsTrigger } from '@core/components/ui/tabs'
import { ToggleGroup, ToggleGroupItem } from '@core/components/ui/toggle-group'
import { StatusDot } from '@core/components/custom/status-dot'
import { EmptyState } from '@core/components/custom/empty-state'
import Modal from '@core/components/Modal'
import { Checkbox } from '@core/components/ui/checkbox'
import { announcementsApi } from '../api/announcements'

// ── 가입자 관리 (사용자 = 가입, 회선 등록이 가입 행위) ─────────────────────────────
//  좌: 조직 트리(범위 필터 — 구조 편집은 구성 › 조직) | 중: 한 표를 두 뷰로 본다 — 가입자(사람 행 + 회선 칩) /
//  회선(번호 행 — 종류 필터에 따라 열이 바뀐다) | 우: 행을 고르면 **드로어**가 열린다(상세 + 편집).
//  기본정보는 드로어 헤더 자리에서 바로 편집하고, 회선은 종류별 카드에서 편집한다 — PTT 카드의 긴급(SOS) 섹션이
//  MCPTT 사용자 프로파일(TS 24.484, PTT 번호 = MCPTT ID 단위)을 품는다. 회선 추가도 드로어 안(누구의 회선인지 먼저).

type View = 'users' | 'lines'
type KindFilter = 'all' | LineSvc

// 회선 뷰의 평탄화 행
interface NumberRow { msisdn: string; svc: LineSvc; user: UserSummary; sub: Subscription }

// ── 회선 종류(접속환경 kind)별 스펙 — 종류마다 다른 것만 여기 선언하고 폼·표·카드는 이 스펙 하나로 그린다
//    (sip_service_model.md §2-9, volte_supplementary_services.md §3 유선 규약). 같은 것은 스펙에 두지 않는다.
interface LineSpec {
  label: string                     // 배지 라벨
  short: string                     // 버튼·제목 라벨
  badge: 'brandSoft' | 'infoSoft' | 'successSoft'
  subsOf: (u: UserSummary) => Subscription[]
  defaultRef: string                // 접속서비스 카탈로그가 비었을 때 기본 name
  numLabel: string                  // 번호 라벨(MSISDN / 번호 / MCPTT ID)
  msisdnPlaceholder: string
  imsiAuto: boolean                 // IMSI 를 비우면 MSISDN 숫자로 채운다(USIM 없는 유선 규약 — Digest username 의 user 파트)
  authSchemes: AuthScheme[]         // 고를 수 있는 인증 체계 — 하나뿐이면 선택 UI 를 숨기고 그 값으로 보낸다
  defaultTransport: SipTransport | ''   // 새 회선 기본 채널 정책 ('' = ANY 단말 선택)
  fixedTransport: boolean           // 채널 정책을 고정 표시(유선 = TLS 규약)
  showDnd: boolean                  // DND·착신전환·링백은 전화 회선만
  showExtension: boolean            // 내선 라벨(끝 자리, 표시 전용 — 망 주소는 E.164)
  addHint: string                   // 회선 추가 폼 아래 안내
}
const LINE: Record<LineSvc, LineSpec> = {
  call: { label: 'VoLTE', short: 'VoLTE', badge: 'brandSoft', subsOf: u => u.call_subscriptions || [], defaultRef: 'volte', numLabel: 'MSISDN',
          msisdnPlaceholder: '+8210…', imsiAuto: false, authSchemes: ['digest', 'aka'], defaultTransport: '', fixedTransport: false, showDnd: true, showExtension: false,
          addHint: 'USIM 가입자 — IMSI 가 Digest username 의 user 파트. AKA 를 고르면 K/OPc 를 함께 입력하고 채널은 TLS 로 강제된다.' },
  voip: { label: 'VoIP', short: 'VoIP', badge: 'infoSoft', subsOf: u => u.voip_subscriptions || [], defaultRef: 'voip', numLabel: '번호',
          msisdnPlaceholder: '+8221…', imsiAuto: true, authSchemes: ['digest'], defaultTransport: 'TLS', fixedTransport: true, showDnd: true, showExtension: true,
          addHint: 'USIM 없는 유선 가입자 — IMSI 는 비우면 번호 숫자열, Digest+TLS 규약. 픽업 그룹은 전화 그룹 멤버십에서 파생되므로 여기서 묻지 않는다.' },
  ptt:  { label: 'McPTT', short: 'PTT', badge: 'successSoft', subsOf: u => u.ptt_subscriptions || [], defaultRef: 'mcptt', numLabel: 'MCPTT ID',
          msisdnPlaceholder: '+825…', imsiAuto: false, authSchemes: ['digest', 'aka'], defaultTransport: 'TLS', fixedTransport: false, showDnd: false, showExtension: false,
          addHint: '만든 뒤 카드의 [편집]에서 소속 그룹 확인 · 긴급(SOS) 설정. 기본값 = 긴급 그룹콜·경보 허용, 대상은 단말이 선택한 그룹.' },
}
const LINE_SVCS: LineSvc[] = ['call', 'voip', 'ptt']
// 내선 라벨 자릿수 — 서버 Provisioning.ExtensionDigits 기본값과 같다(표시 전용)
const EXT_DIGITS = 4
const digitsOf = (msisdn: string) => msisdn.replace(/\D/g, '')
const extensionOf = (msisdn: string) => digitsOf(msisdn).slice(-EXT_DIGITS)
// 등록 상태 — 마지막 REGISTER 가 마지막 해제보다 뒤면 등록 중(ISO8601 문자열 비교)
const isRegistered = (s: Subscription) => !!s.register_time && (!s.logout_time || s.logout_time < s.register_time)
// 착신전환 대상 형식(CSC `_FORWARD_RE`) — 숫자열, 선행 + 허용
const FORWARD_RE = /^\+?[0-9*#]{1,32}$/
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
type OrgOpt = { code: string; label: string }

// 착신전환 요약 — CFU/CFB/CFNR(+시한)/CFNL (volte_supplementary_services.md §6A)
function forwardSummary(s: Subscription): string {
  const p: string[] = []
  if (s.forward_id) p.push(`CFU→${s.forward_id.slice(-4)}`)
  if (s.forward_busy_id) p.push(`통화중→${s.forward_busy_id.slice(-4)}`)
  if (s.forward_no_reply_id) p.push(`무응답→${s.forward_no_reply_id.slice(-4)}${s.forward_no_reply_sec ? ` ${s.forward_no_reply_sec}s` : ''}`)
  if (s.forward_not_logged_in_id) p.push(`미등록→${s.forward_not_logged_in_id.slice(-4)}`)
  if (s.forward_not_reachable_id) p.push(`도달불가→${s.forward_not_reachable_id.slice(-4)}`)
  return p.join(' · ')
}
const hasForward = (s: Subscription) => !!(s.forward_id || s.forward_busy_id || s.forward_no_reply_id || s.forward_not_logged_in_id || s.forward_not_reachable_id)
// 조건부 전환 컬럼 키 — 판정 컬럼은 마지막에 더해진 forward_not_reachable_id(CSC 와 같은 규칙: 부분 적용 DB 는 미적용)
const COND_KEYS = ['forward_busy_id', 'forward_no_reply_id', 'forward_not_logged_in_id', 'forward_not_reachable_id'] as const
type CondKey = typeof COND_KEYS[number]
const COND_LABEL: Record<CondKey, string> = { forward_busy_id: '통화중 (CFB)', forward_no_reply_id: '무응답 (CFNR)', forward_not_logged_in_id: '미등록 (CFNL)', forward_not_reachable_id: '도달불가 (CFNRc)' }

// PTT 그룹 멤버십 — 멤버 user_id 는 PTT 회선 번호(MSISDN), mcptt_id 는 tel:URI 파생
function pttGroupsOf(groups: Group[], msisdn: string): Group[] {
  const d = digitsOf(msisdn)
  return groups.filter(g => (g.members || []).some(m => m.user_id === msisdn || digitsOf(m.user_id || '') === d || digitsOf(m.mcptt_id || '') === d))
}
function phoneGroupOf(groups: PhoneGroup[], msisdn: string): PhoneGroup | undefined {
  const d = digitsOf(msisdn)
  return groups.find(g => (g.members || []).some(m => m.user_id === msisdn || digitsOf(m.user_id) === d))
}

export default function ProvisioningWorkbenchPage() {
  const { show } = useToast()
  const confirm = useConfirm()
  const { user: me } = useAuth()
  const canWrite = canWriteConfig(me)

  const [view, setView] = useState<View>('users')
  const [kind, setKind] = useState<KindFilter>('all')
  const [orgScope, setOrgScope] = useState<string | null>(null)
  const [orgName, setOrgName] = useState('전체')
  const [search, setSearch] = useState('')

  const [users, setUsers] = useState<UserSummary[]>([])
  const [orgs, setOrgs] = useState<Organization[]>([])
  const [pttGroups, setPttGroups] = useState<Group[]>([])
  const [phoneGroups, setPhoneGroups] = useState<PhoneGroup[]>([])
  const [loading, setLoading] = useState(true)

  const [selected, setSelected] = useState<Set<string | number>>(new Set())
  const [bulkOrg, setBulkOrg] = useState('')
  const [importOpen, setImportOpen] = useState(false)

  // 드로어 — 선택 가입자(id) 또는 새 가입자('new'), 강조할 회선(svc:msisdn)
  const [sel, setSel] = useState<number | 'new' | null>(null)
  const [hi, setHi] = useState<string | null>(null)

  const orgOpts = useMemo(() => orgIndentedOptions(orgs), [orgs])

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [u, o, g, pg] = await Promise.all([
        usersApi.list(), orgApi.list(),
        groupsApi.list().catch(() => [] as Group[]),
        phoneGroupsApi.list().then(r => r.groups || []).catch(() => [] as PhoneGroup[]),
      ])
      setUsers(u); setOrgs(o); setPttGroups(g); setPhoneGroups(pg)
    } catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [show])
  useEffect(() => { load() }, [load])

  const orgPathOf = useCallback((code: string) => orgs.find(o => o.code === code)?.code_path || code, [orgs])
  const inScope = useCallback((orgCode: string) => {
    if (!orgScope) return true
    return (orgPathOf(orgCode) || '').startsWith(orgScope)
  }, [orgScope, orgPathOf])

  // 뷰·범위 전환 시 선택 초기화
  useEffect(() => { setSelected(new Set()) }, [view, orgScope])

  const userHasNumber = useCallback((u: UserSummary, q: string) =>
    LINE_SVCS.some(svc => LINE[svc].subsOf(u).some(s => s.id.toLowerCase().includes(q) || extensionOf(s.id) === q)), [])

  // ── 뷰 데이터 ──
  const userRows = useMemo(() => {
    const q = search.trim().toLowerCase()
    return users.filter(u => inScope(u.org_id || '') &&
      (!q || u.name.toLowerCase().includes(q) || (u.title || '').toLowerCase().includes(q) || (u.login_id || '').toLowerCase().includes(q) || userHasNumber(u, q)))
  }, [users, inScope, search, userHasNumber])

  const lineRows = useMemo((): NumberRow[] => {
    const q = search.trim().toLowerCase()
    const out: NumberRow[] = []
    for (const u of userRows) for (const svc of LINE_SVCS) for (const sub of LINE[svc].subsOf(u)) out.push({ msisdn: sub.id, svc, user: u, sub })
    return out.filter(r => (kind === 'all' || r.svc === kind) &&
      (!q || r.msisdn.toLowerCase().includes(q) || extensionOf(r.msisdn) === q || r.user.name.toLowerCase().includes(q) || (r.user.login_id || '').toLowerCase().includes(q)))
  }, [userRows, kind, search])
  const allLineCount = useMemo(() => userRows.reduce((n, u) => n + LINE_SVCS.reduce((m, svc) => m + LINE[svc].subsOf(u).length, 0), 0), [userRows])
  const kindCount = useCallback((svc: LineSvc) => userRows.reduce((n, u) => n + LINE[svc].subsOf(u).length, 0), [userRows])

  // ── 드로어 열기/닫기 ──
  const openUser = useCallback((id: number, hiKey?: string) => { setSel(id); setHi(hiKey || null) }, [])
  const closeDrawer = useCallback(() => { setSel(null); setHi(null) }, [])

  // ── 일괄 ──
  async function batchDeleteUsers() {
    const ids = Array.from(selected).map(Number)
    if (!ids.length) return
    if (!await confirm({ title: '가입자 일괄 삭제', tone: 'danger', confirmLabel: '삭제',
      body: `${ids.length}명을 삭제합니다. 연결된 회선도 삭제됩니다.` })) return
    try { await usersApi.batchDelete(ids); show('삭제 완료', 'ok'); setSelected(new Set()); if (typeof sel === 'number' && ids.includes(sel)) closeDrawer(); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function batchMoveOrg() {
    const ids = Array.from(selected).map(Number)
    if (!ids.length || !bulkOrg) { show('이동할 조직을 고르세요', 'err'); return }
    const results = await Promise.allSettled(ids.map(id => usersApi.update(id, { org_id: bulkOrg })))
    const fail = results.filter(r => r.status === 'rejected').length
    show(fail ? `${ids.length - fail}명 이동, ${fail}명 실패` : `${ids.length}명을 ${buildOrgPath(orgs, bulkOrg)} 로 이동`, fail ? 'err' : 'ok')
    setSelected(new Set()); load()
  }

  // ── 가입자 뷰 컬럼 ──
  const userCols: Column<UserSummary>[] = [
    { key: 'name', header: '이름 · 직함', sortable: true, width: 150, sortValue: u => u.name, render: u => (
      <div className="flex flex-col leading-tight">
        <span className="font-semibold">{u.name}</span>
        <span className="text-xs text-muted-foreground">{u.title || '—'}</span>
      </div>) },
    { key: 'org', header: '조직', width: 200, sortValue: u => buildOrgPath(orgs, u.org_id), render: u => <span className="text-sm text-muted-foreground" title={buildOrgPath(orgs, u.org_id)}>{buildOrgPath(orgs, u.org_id)}</span> },
    { key: 'login_id', header: '로그인ID', sortable: true, width: 110, sortValue: u => u.login_id || '', render: u => <span className="font-mono text-xs text-muted-foreground" title="단말 로그인 ID">{u.login_id || '—'}</span> },
    { key: 'nums', header: '회선', render: u => {
      const all = LINE_SVCS.flatMap(svc => LINE[svc].subsOf(u).map(s => ({ svc, s })))
      if (all.length === 0) return <span className="text-sm text-muted-foreground">회선 없음</span>
      return <span className="flex flex-wrap gap-1">
        {all.map(({ svc, s }) => (
          <button key={`${svc}:${s.id}`} type="button" className="rounded-md" title={`${LINE[svc].label} ${s.id} — 이 회선 카드로 열기`}
            onClick={e => { e.stopPropagation(); openUser(u.id, `${svc}:${s.id}`) }}>
            <Badge variant={LINE[svc].badge} className="gap-1">
              <StatusDot tone={isRegistered(s) ? 'success' : 'neutral'} />
              {LINE[svc].label} {svc === 'voip' ? extensionOf(s.id) : s.id.slice(-8)}
            </Badge>
          </button>
        ))}
      </span>
    } },
    { key: 'svc', header: '부가서비스', width: 190, render: u => {
      const phones = [...LINE.call.subsOf(u), ...LINE.voip.subsOf(u)]
      const p: string[] = []
      if (phones.some(s => s.dnd)) p.push('DND')
      if (phones.some(hasForward)) p.push('착신전환')
      if (phones.some(s => s.ringback_media)) p.push('링백')
      return <span className="text-sm text-muted-foreground">{p.length ? p.join(' · ') : '—'}</span>
    } },
    { key: 'reg', header: '등록', width: 70, render: u => {
      const all = LINE_SVCS.flatMap(svc => LINE[svc].subsOf(u))
      return <span className="text-sm text-muted-foreground tabular-nums">{all.filter(isRegistered).length}/{all.length}</span>
    } },
    { key: 'go', header: '', width: 30, align: 'right', render: () => <ChevronRight size={ICON} className="text-muted-foreground" /> },
  ]

  // ── 회선 뷰 컬럼 — 종류 필터에 따라 열이 바뀐다 ──
  const numCol: Column<NumberRow> = { key: 'msisdn', header: kind === 'all' ? '번호' : LINE[kind].numLabel, sortable: true, width: 160, render: r => <strong className="font-mono">{r.msisdn}</strong> }
  const userCol: Column<NumberRow> = { key: 'user', header: '가입자', sortable: true, width: 130, sortValue: r => r.user.name, render: r => r.user.name }
  const regCol: Column<NumberRow> = { key: 'reg', header: '등록', width: 80, sortValue: r => isRegistered(r.sub) ? 1 : 0, render: r => <StatusDot tone={isRegistered(r.sub) ? 'success' : 'neutral'} label={isRegistered(r.sub) ? '등록' : '미등록'} /> }
  const dndCol: Column<NumberRow> = { key: 'dnd', header: 'DND', width: 64, align: 'center', render: r => r.sub.dnd ? <Badge variant="dangerSoft">ON</Badge> : <span className="text-sm text-muted-foreground">—</span> }
  const fwdCol: Column<NumberRow> = { key: 'fwd', header: '착신전환', render: r => <span className="text-sm text-muted-foreground">{forwardSummary(r.sub) || '—'}</span> }
  const rbCol: Column<NumberRow> = { key: 'rb', header: '링백', width: 150, render: r => <span className="font-mono text-xs text-muted-foreground">{r.sub.ringback_media || '—'}</span> }
  const transportCol: Column<NumberRow> = { key: 'tr', header: '채널', width: 96, render: r => <TransportBadge v={r.sub.sip_transport} aka={r.sub.auth_scheme === 'aka'} /> }
  const lineColsOf: Record<KindFilter, Column<NumberRow>[]> = {
    all: [
      { key: 'svc', header: '종류', width: 76, sortValue: r => r.svc, render: r => <SvcBadge svc={r.svc} /> },
      numCol, userCol,
      { key: 'org', header: '조직', width: 170, render: r => <span className="text-sm text-muted-foreground">{buildOrgPath(orgs, r.user.org_id)}</span> },
      { key: 'ch', header: '채널 · 인증', width: 130, render: r => <span className="text-sm text-muted-foreground">{r.sub.sip_transport || 'ANY'} · {r.sub.auth_scheme === 'aka' ? 'AKA' : 'Digest'}</span> },
      { key: 'sum', header: '요약', render: r => <span className="text-sm text-muted-foreground">{r.svc === 'ptt'
        ? `그룹 ${pttGroupsOf(pttGroups, r.msisdn).map(g => g.name || g.id).join(', ') || '—'}`
        : [r.sub.dnd ? 'DND' : '', forwardSummary(r.sub), r.sub.ringback_media ? `링백 ${r.sub.ringback_media}` : ''].filter(Boolean).join(' · ') || '—'}</span> },
      regCol,
    ],
    call: [
      numCol, userCol,
      { key: 'imsi', header: 'IMSI', width: 150, render: r => <span className="font-mono text-xs text-muted-foreground">{r.sub.imsi || '—'}</span> },
      { key: 'auth', header: '인증', width: 80, render: r => <AuthBadge sub={r.sub} /> },
      transportCol, dndCol, fwdCol, rbCol, regCol,
    ],
    voip: [
      numCol,
      { key: 'ext', header: '내선', width: 64, render: r => <strong className="font-mono text-info-on">{extensionOf(r.msisdn)}</strong> },
      userCol,
      { key: 'pg', header: '전화 그룹 (픽업)', width: 170, render: r => { const g = phoneGroupOf(phoneGroups, r.msisdn); return <span className="text-sm text-muted-foreground" title={PICKUP_TITLE}>{g ? `${g.name} (${g.id})` : (r.sub.pickup_group || '—')}</span> } },
      transportCol, dndCol, fwdCol, rbCol, regCol,
    ],
    ptt: [
      numCol, userCol,
      { key: 'groups', header: '소속 그룹', render: r => <span className="text-sm text-muted-foreground">{pttGroupsOf(pttGroups, r.msisdn).map(g => g.name || g.id).join(', ') || '—'}</span> },
      { key: 'auth', header: '인증', width: 80, render: r => <AuthBadge sub={r.sub} /> },
      transportCol, regCol,
    ],
  }

  const catalog = useMemo(() => buildServiceCatalog(users), [users])
  const selUser = typeof sel === 'number' ? users.find(u => u.id === sel) : undefined
  const activeKey = view === 'users' ? (typeof sel === 'number' ? sel : null) : (hi ? hi.split(':').slice(1).join(':') : null)

  return (
    <div className="flex gap-4 items-stretch flex-1 min-h-0">
      {/* 좌: 조직 트리 = 범위 필터. 구조 편집은 구성 › 조직 */}
      <OrgTreePanel className="flex-[0_0_200px] w-[200px] max-w-[200px]" fill selectedPath={orgScope} onSelect={(p, n) => { setOrgScope(p); setOrgName(n) }}/>

      {/* 중: 패널 = 툴바 + (일괄 바) + 표 */}
      <div className="panel flex flex-1 flex-col overflow-hidden rounded-md border border-border bg-card min-w-0">
        <div className="toolbar flex items-center gap-2.5 border-b border-border bg-muted px-4 py-3 flex-wrap">
          <ToggleGroup type="single" value={view} onValueChange={(v: string) => v && setView(v as View)} className="shrink-0 justify-start rounded-md bg-background p-[3px]" aria-label="보기 기준">
            <ToggleGroupItem value="users">가입자 <Badge className="ml-1" variant="neutralSoft">{userRows.length}</Badge></ToggleGroupItem>
            <ToggleGroupItem value="lines">회선 <Badge className="ml-1" variant="neutralSoft">{allLineCount}</Badge></ToggleGroupItem>
          </ToggleGroup>
          {view === 'lines' && (
            <ToggleGroup type="single" value={kind} onValueChange={(v: string) => v && setKind(v as KindFilter)} className="shrink-0 justify-start rounded-md bg-background p-[3px]" aria-label="회선 종류">
              <ToggleGroupItem value="all">전체</ToggleGroupItem>
              {LINE_SVCS.map(svc => <ToggleGroupItem key={svc} value={svc}>{LINE[svc].short} <Badge className="ml-1" variant="neutralSoft">{kindCount(svc)}</Badge></ToggleGroupItem>)}
            </ToggleGroup>
          )}
          <span className="font-semibold text-md">{orgName}</span>
          <Input className="flex-1 max-w-[240px]" placeholder="이름 · 번호 · 내선 · 로그인ID" value={search} onChange={e => setSearch(e.target.value)} aria-label="검색"/>
          {search && <Button variant="ghost" onClick={() => setSearch('')} aria-label="검색어 지우기"><X size={13} /></Button>}
          {canWrite && <span className="ml-auto flex gap-1.5">
            <Button onClick={() => setImportOpen(true)}><Upload size={13} /> Excel 가져오기</Button>
            <Button variant="default" onClick={() => { setSel('new'); setHi(null) }}><Plus size={13} /> 가입자</Button>
          </span>}
        </div>

        {/* 선택 일괄 바 — 조직 이동 · 삭제 */}
        {canWrite && view === 'users' && selected.size > 0 && (
          <div className="flex items-center gap-2 border-b border-border bg-brandsoft px-4 py-2 text-sm">
            <b>{selected.size}명 선택</b>
            <span className="ml-auto flex items-center gap-1.5">
              <Select value={toSel(bulkOrg)} onValueChange={(v: string) => setBulkOrg(fromSel(v))}>
                <SelectTrigger className="w-[220px]"><SelectValue placeholder="이동할 조직" /></SelectTrigger>
                <SelectContent>{orgOpts.map(o => <SelectItem key={o.code} value={o.code}>{o.label}</SelectItem>)}</SelectContent>
              </Select>
              <Button onClick={batchMoveOrg}>조직 이동</Button>
              <Button variant="destructive" onClick={batchDeleteUsers}>삭제</Button>
              <Button variant="ghost" onClick={() => setSelected(new Set())}>선택 해제</Button>
            </span>
          </div>
        )}

        {view === 'users' ? (
          <DataTable<UserSummary> columns={userCols} rows={userRows} rowKey={u => u.id} loading={loading}
            selectable={canWrite} selected={selected} onSelectChange={setSelected}
            onRowClick={u => openUser(u.id)} activeRowKey={activeKey}
            pageSize={50} emptyText="가입자 없음" />
        ) : (
          <DataTable<NumberRow> key={kind} columns={lineColsOf[kind]} rows={lineRows} rowKey={r => r.msisdn} loading={loading}
            onRowClick={r => openUser(r.user.id, `${r.svc}:${r.msisdn}`)} activeRowKey={activeKey}
            pageSize={50} emptyText={kind === 'all' ? '회선 없음' : `${LINE[kind].short} 회선 없음 — 가입자 드로어의 [회선 추가]`} />
        )}
      </div>

      {/* 우: 드로어 */}
      {(sel === 'new' || selUser) && (
        <UserDrawer key={sel === 'new' ? 'new' : selUser!.id} user={selUser} orgs={orgs} orgOpts={orgOpts} catalog={catalog}
          pttGroups={pttGroups} phoneGroups={phoneGroups} canWrite={canWrite} highlight={hi}
          defaultOrg={orgScope ? (orgScope.split('/').pop() || '') : ''}
          onCreated={id => { setSel(id); load() }} onReload={load} onClose={closeDrawer} />
      )}

      {/* Excel import (사용자+회선 통합) */}
      {importOpen && <ImportModal onClose={() => setImportOpen(false)} onDone={load} />}
    </div>
  )
}

// ════════════════════════════════════════════════════════════
//  드로어 — 헤더(기본정보 보기↔인라인 편집) + 탭(회선 / 역할·그룹)
//  시트 2 에 Drawer 가 없어 알람 드로어(AlarmIndicator)와 같은 패턴 — 차단층 + 우측 고정 패널.
//  차단층은 Radix 팝오버(z-50)보다 아래(z-40)에 둬서 드로어 안의 Select 가 열린 채로 동작한다.
// ════════════════════════════════════════════════════════════
function UserDrawer({ user, orgs, orgOpts, catalog, pttGroups, phoneGroups, canWrite, highlight, defaultOrg, onCreated, onReload, onClose }: {
  user?: UserSummary; orgs: Organization[]; orgOpts: OrgOpt[]; catalog: ServiceCat[]
  pttGroups: Group[]; phoneGroups: PhoneGroup[]; canWrite: boolean; highlight: string | null; defaultOrg: string
  onCreated: (id: number) => void; onReload: () => void; onClose: () => void
}) {
  const { show } = useToast()
  const confirm = useConfirm()
  const isNew = !user
  const [editBasic, setEditBasic] = useState(false)
  const [tab, setTab] = useState<'lines' | 'roles'>('lines')
  const [addSvc, setAddSvc] = useState<LineSvc | null>(null)
  const [editKey, setEditKey] = useState<string | null>(null)

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') onClose() }
    document.addEventListener('keydown', onKey)
    return () => document.removeEventListener('keydown', onKey)
  }, [onClose])

  async function deleteUser() {
    if (!user) return
    const n = LINE_SVCS.reduce((m, svc) => m + LINE[svc].subsOf(user).length, 0)
    if (!await confirm({ title: '가입자 삭제', tone: 'danger', confirmLabel: '삭제',
      body: `${user.name} 을(를) 삭제합니다. 회선 ${n}개도 함께 삭제됩니다.` })) return
    try { await usersApi.delete(user.id); show('삭제', 'ok'); onClose(); onReload() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  const rows: LineRow[] = user ? LINE_SVCS.flatMap(svc => LINE[svc].subsOf(user).map(sub => ({ svc, sub }))) : []

  return (
    <>
      {/* 바깥 클릭을 삼키는 층 — 닫히면서 누른 것이 실행되지 않게 mousedown 에서 끊는다 */}
      <div className="fixed inset-0 z-[40] bg-black/20" onMouseDown={e => { e.preventDefault(); onClose() }} />
      <aside role="dialog" aria-label="가입자 상세"
        className="fixed bottom-0 right-0 top-[58px] z-[45] flex w-[640px] max-w-[95vw] flex-col border-l border-border bg-card shadow-lg">
        {/* 헤더 — 보기 / 인라인 편집 / 새 가입자 */}
        {isNew || editBasic ? (
          <BasicHeaderForm user={user} orgOpts={orgOpts} defaultOrg={defaultOrg}
            onCancel={() => isNew ? onClose() : setEditBasic(false)}
            onSaved={id => { setEditBasic(false); if (isNew) onCreated(id); else onReload() }} />
        ) : (
          <div className="flex items-start justify-between gap-3 px-5 pt-4">
            <div className="flex min-w-0 items-center gap-3">
              <div className="flex h-10 w-10 shrink-0 items-center justify-center rounded-md bg-brandsoft text-brandsoft-on text-md font-bold">{user!.name.slice(0, 1)}</div>
              <div className="min-w-0">
                <div className="flex flex-wrap items-baseline gap-2"><span className="text-lg font-bold">{user!.name}</span><span className="text-sm text-muted-foreground">{user!.title || ''}</span></div>
                <div className="text-xs text-muted-foreground">{buildOrgPath(orgs, user!.org_id)} · 로그인 <span className="font-mono">{user!.login_id || '—'}</span></div>
                {user!.details && <div className="text-xs text-muted-foreground">{user!.details}</div>}
              </div>
            </div>
            <div className="flex shrink-0 gap-1">
              {canWrite && <Button variant="ghost" onClick={() => setEditBasic(true)}><Pencil size={13} /> 기본정보</Button>}
              {canWrite && <Button variant="ghost" className="text-destructive" onClick={deleteUser}>삭제</Button>}
              <Button variant="ghost" onClick={onClose} aria-label="닫기"><X size={16} /></Button>
            </div>
          </div>
        )}

        <Tabs value={tab} onValueChange={v => setTab(v as 'lines' | 'roles')} className="px-5 pt-2">
          <TabsList>
            <TabsTrigger value="lines" disabled={isNew}>회선 <Badge className="ml-1" variant="neutralSoft">{rows.length}</Badge></TabsTrigger>
            <TabsTrigger value="roles" disabled={isNew}>역할 · 그룹</TabsTrigger>
          </TabsList>
        </Tabs>

        <div className="flex min-h-0 flex-1 flex-col gap-3 overflow-auto px-5 pb-6 pt-3">
          {isNew && <EmptyState title="기본정보를 저장하면 여기서 바로 회선을 추가합니다" description="VoLTE · VoIP · PTT 회선은 종류별로 묻는 항목이 다르다" />}
          {!isNew && tab === 'lines' && (
            <>
              {rows.map(r => {
                const k = `${r.svc}:${r.sub.id}`
                return <LineCard key={k} user={user!} row={r} catalog={catalog} pttGroups={pttGroups} phoneGroups={phoneGroups}
                  canWrite={canWrite} highlight={highlight === k && editKey !== k} editing={editKey === k}
                  onEdit={() => { setEditKey(k); setAddSvc(null) }} onDone={() => { setEditKey(null); onReload() }} onCancel={() => setEditKey(null)} />
              })}
              {rows.length === 0 && !addSvc && <EmptyState title="아직 회선이 없습니다" description="아래에서 종류를 골라 추가하세요" />}
              {canWrite && (addSvc
                ? <AddLineCard user={user!} svc={addSvc} catalog={catalog} onCancel={() => setAddSvc(null)} onAdded={() => { setAddSvc(null); onReload() }} />
                : <div className="flex flex-wrap items-center gap-2 pt-1">
                    <span className="flex-1 text-xs text-muted-foreground">회선 추가 — 종류를 고르면 그 종류에 필요한 항목만 묻습니다</span>
                    {LINE_SVCS.map(svc => <Button key={svc} onClick={() => { setAddSvc(svc); setEditKey(null) }}><Plus size={13} /> {LINE[svc].short}</Button>)}
                  </div>)}
            </>
          )}
          {!isNew && tab === 'roles' && <RolesTab user={user!} pttGroups={pttGroups} phoneGroups={phoneGroups} />}
        </div>
      </aside>
    </>
  )
}

// ── 헤더 자리 기본정보 폼 (새 가입자 + 편집 공용) — 아래 회선 카드는 그대로 둔 채 헤더만 폼으로 바뀐다 ──
function BasicHeaderForm({ user, orgOpts, defaultOrg, onCancel, onSaved }: {
  user?: UserSummary; orgOpts: OrgOpt[]; defaultOrg: string; onCancel: () => void; onSaved: (id: number) => void
}) {
  const { show } = useToast()
  const isNew = !user
  // 가입자(person). login_id/passwd = 단말(IdMS) 로그인 자격(MCPTT ID 와 별개). passwd 는 입력 시에만 전송(편집 시 빈칸=유지).
  const [form, setForm] = useState<UserInput>(() => user
    ? { name: user.name, title: user.title || '', org_id: user.org_id, details: user.details || '', login_id: user.login_id || '' }
    : { name: '', title: '', org_id: defaultOrg || '', details: '', login_id: '' })
  const [busy, setBusy] = useState(false)

  async function submit() {
    if (!form.name) { show('이름 필수', 'err'); return }
    const payload: UserInput = { ...form }
    if (!payload.passwd) delete payload.passwd
    setBusy(true)
    try {
      if (isNew) { const r = await usersApi.create(payload); show(`${form.name} 생성 — 회선을 추가하세요`, 'ok'); onSaved(r.id) }
      else { await usersApi.update(user!.id, payload); show(form.org_id !== user!.org_id ? '저장 — 소속 조직 변경' : '저장', 'ok'); onSaved(user!.id) }
    } catch (e: unknown) { show(String(e), 'err') } finally { setBusy(false) }
  }

  return (
    <div className="flex flex-col gap-2.5 border-b border-border bg-brandsoft px-5 pb-3.5 pt-4">
      <div className="flex items-center gap-3">
        <div className="min-w-0 flex-1">
          <div className="text-md font-bold">{isNew ? '가입자 추가' : '기본정보 편집'}</div>
          <div className="text-xs text-muted-foreground">{isNew ? '사람을 먼저 만들고, 아래에서 회선을 종류별로 추가합니다.' : '소속 조직도 여기서 바꿉니다 — 아래 회선 카드는 그대로 둔 채 저장됩니다.'}</div>
        </div>
        <Button variant="ghost" onClick={onCancel}>취소</Button>
        <Button variant="default" disabled={busy} onClick={submit}>{isNew ? '만들기' : '저장'}</Button>
      </div>
      <div className="grid grid-cols-3 gap-2.5">
        <Field label="이름 *"><Input autoFocus value={form.name} onChange={e => setForm({ ...form, name: e.target.value })} /></Field>
        <Field label="직함"><Input placeholder="예: 팀장" value={form.title || ''} onChange={e => setForm({ ...form, title: e.target.value })} /></Field>
        <Field label="로그인 ID"><Input className="font-mono" placeholder="예: test001" value={form.login_id || ''} onChange={e => setForm({ ...form, login_id: e.target.value })} /></Field>
        <Field label="조직" className="col-span-2">
          <Select value={toSel(form.org_id)} onValueChange={(v: string) => setForm({ ...form, org_id: fromSel(v) })}>
            <SelectTrigger><SelectValue /></SelectTrigger>
            <SelectContent>
              <SelectItem value={NONE}>없음</SelectItem>
              {orgOpts.map(o => <SelectItem key={o.code} value={o.code}>{o.label}</SelectItem>)}
            </SelectContent>
          </Select>
        </Field>
        <Field label={isNew ? '비밀번호' : '비밀번호 (변경 시)'}><Input type="password" placeholder={isNew ? '' : '미변경'} value={form.passwd || ''} onChange={e => setForm({ ...form, passwd: e.target.value })} /></Field>
        <Field label="설명" className="col-span-3"><Input value={form.details || ''} onChange={e => setForm({ ...form, details: e.target.value })} /></Field>
      </div>
      <div className="text-xs text-muted-foreground">조직 구조(추가·이름·상위 이동)는 구성 › 조직에서. 여기서는 이 사람의 소속만 바꿉니다.</div>
    </div>
  )
}

// ── 폼 필드 (라벨 위, 입력 아래) ──
function Field({ label, children, className, title }: { label: string; children: React.ReactNode; className?: string; title?: string }) {
  return (
    <label className={`flex min-w-0 flex-col gap-0.5 ${className || ''}`} title={title}>
      <span className="text-xs text-muted-foreground">{label}</span>
      {children}
    </label>
  )
}
function Section({ title, aside, children }: { title: string; aside?: React.ReactNode; children: React.ReactNode }) {
  return (
    <div className="flex flex-col gap-2">
      <div className="flex items-center justify-between">
        <span className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">{title}</span>
        {aside}
      </div>
      {children}
    </div>
  )
}
function KV({ rows }: { rows: Array<[string, React.ReactNode]> }) {
  return (
    <div className="grid grid-cols-[96px_minmax(0,1fr)] gap-x-3 gap-y-1.5 text-sm">
      {rows.map(([k, v], i) => <Fragment key={`${k}-${i}`}><span className="text-muted-foreground">{k}</span><span className="min-w-0 break-words">{v}</span></Fragment>)}
    </div>
  )
}

// ════════════════════════════════════════════════════════════
//  회선 카드 — 종류별 보기/편집. PTT 카드는 MCPTT 사용자 프로파일(긴급 SOS)을 품는다.
// ════════════════════════════════════════════════════════════
type LineRow = { svc: LineSvc; sub: Subscription }
interface EditLine {
  service_ref: string; imsi: string; passwd: string; sip_transport: SipTransport | ''; auth_scheme: AuthScheme; k: string; opc: string
  dnd: boolean; forward_id: string; forward_busy_id: string; forward_no_reply_id: string; forward_no_reply_sec: string; forward_not_logged_in_id: string; forward_not_reachable_id: string
  // 착신전환 서비스 폼 — 번호 하나 + 조건 선택(TS 22.082 `004` all-conditional 과 같은 표현). perCond = 조건별 번호 따로(규격이 허용하는 rule 별 target)
  fwdNumber: string; fwdCfu: boolean; fwdCond: Record<CondKey, boolean>; perCond: boolean
  ringback_media: string
}
// 저장값 → 착신전환 서비스 폼. 비어 있지 않은 번호가 하나(또는 없음)면 '번호 하나 + 조건' 모드, 둘 이상이면 조건별 모드.
function forwardFormOf(sub: Subscription): Pick<EditLine, 'fwdNumber' | 'fwdCfu' | 'fwdCond' | 'perCond'> {
  const cond = Object.fromEntries(COND_KEYS.map(k => [k, !!(sub[k] || '').trim()])) as Record<CondKey, boolean>
  const nums = new Set([sub.forward_id, ...COND_KEYS.map(k => sub[k])].map(v => (v || '').trim()).filter(Boolean))
  return { fwdNumber: nums.size ? [...nums][0] : '', fwdCfu: !!(sub.forward_id || '').trim(), fwdCond: cond, perCond: nums.size > 1 }
}
const PRIV_LABEL: Record<McpttProfile['private_emergency_mode'], string> = { LocallyDetermined: '허용 — 단말이 고른 상대', UsePreConfigured: '허용 — 사전 지정 수신자' }

function LineCard({ user, row, catalog, pttGroups, phoneGroups, canWrite, highlight, editing, onEdit, onDone, onCancel }: {
  user: UserSummary; row: LineRow; catalog: ServiceCat[]; pttGroups: Group[]; phoneGroups: PhoneGroup[]
  canWrite: boolean; highlight: boolean; editing: boolean; onEdit: () => void; onDone: () => void; onCancel: () => void
}) {
  const { show } = useToast()
  const confirm = useConfirm()
  const { svc, sub } = row
  const spec = LINE[svc]
  const reg = isRegistered(sub)
  const memberGroups = svc === 'ptt' ? pttGroupsOf(pttGroups, sub.id) : []
  const phoneGroup = svc === 'voip' ? phoneGroupOf(phoneGroups, sub.id) : undefined
  // 선택 컬럼(마이그레이션 의존) — 응답에 키가 없으면 그 DB 에 컬럼이 없다(보내면 400 schema_not_migrated)
  const hasCdiv = sub.forward_not_reachable_id !== undefined
  const hasRingback = sub.ringback_media !== undefined

  // PTT 프로파일 — 카드가 열릴 때 읽는다(목록 API 에는 없다)
  const [prof, setProf] = useState<(McpttProfile & { exists?: boolean }) | null | undefined>(undefined)
  useEffect(() => {
    if (svc !== 'ptt') return
    usersApi.getPttProfile(user.id, sub.id).then(setProf).catch(() => setProf(null))
  }, [svc, user.id, sub.id])

  const [form, setForm] = useState<EditLine | null>(null)
  const [pform, setPform] = useState<McpttProfile | null>(null)
  const [media, setMedia] = useState<string[] | null>(null)
  useEffect(() => {
    if (!editing) { setForm(null); setPform(null); return }
    setForm({ service_ref: sub.service_ref || '', imsi: sub.imsi || '', passwd: '', sip_transport: sub.sip_transport || '', auth_scheme: sub.auth_scheme || 'digest', k: '', opc: '',
      dnd: !!sub.dnd, forward_id: sub.forward_id || '', forward_busy_id: sub.forward_busy_id || '', forward_no_reply_id: sub.forward_no_reply_id || '',
      forward_no_reply_sec: sub.forward_no_reply_sec ? String(sub.forward_no_reply_sec) : '', forward_not_logged_in_id: sub.forward_not_logged_in_id || '', forward_not_reachable_id: sub.forward_not_reachable_id || '',
      ringback_media: sub.ringback_media || '', ...forwardFormOf(sub) })
    if (svc === 'ptt' && prof) setPform({ allow_emergency_call: prof.allow_emergency_call, allow_emergency_alert: prof.allow_emergency_alert, allow_adhoc_call: prof.allow_adhoc_call,
      emergency_group_mode: prof.emergency_group_mode, emergency_group_id: prof.emergency_group_id, allow_emergency_private_call: prof.allow_emergency_private_call,
      private_emergency_mode: prof.private_emergency_mode, emergency_private_recipient: prof.emergency_private_recipient })
    // 링백 음원 후보 — 서비스 음원 라이브러리(announcements.md §7). 못 읽으면 직접 입력만
    if (spec.showDnd && hasRingback && media === null) announcementsApi.list().then(r => setMedia(r.media.map(m => m.id))).catch(() => setMedia([]))
  }, [editing, sub, svc, prof, spec.showDnd, hasRingback, media])

  async function save() {
    if (!form) return
    // passwd 는 변경 시에만 전송. imsi/service_ref 가 바뀌면 서버가 passwd 를 요구한다(H(A1) 결박).
    const d: Partial<Subscription> = { service_ref: form.service_ref, imsi: form.imsi, sip_transport: form.sip_transport || null }
    if (form.passwd) d.passwd = form.passwd
    if (spec.imsiAuto && !(d.imsi || '').trim()) d.imsi = digitsOf(sub.id)
    if (!d.passwd && ((d.imsi || '') !== (sub.imsi || '') || (d.service_ref || '') !== (sub.service_ref || ''))) { show('IMSI/서비스 변경 시 비밀번호를 함께 입력해야 합니다 (H(A1) 재결박)', 'err'); return }
    const scheme: AuthScheme = spec.authSchemes.length === 1 ? spec.authSchemes[0] : form.auth_scheme
    const aka = akaBody(scheme, form.k, form.opc, !!sub.aka_provisioned)
    if (aka.err) { show(aka.err, 'err'); return }
    if ((sub.auth_scheme || 'digest') === 'aka' && (aka.fields.auth_scheme || 'digest') === 'digest' && !d.passwd) { show('AKA→Digest 전환 시 비밀번호를 함께 입력해야 합니다 (H(A1) 생성)', 'err'); return }
    if ((aka.fields.auth_scheme || 'digest') !== (sub.auth_scheme || 'digest') || aka.fields.k) Object.assign(d, aka.fields)
    if (spec.showDnd) {
      // 착신전환 서비스 — 번호 하나 모드: CFU 면 forward_id 만(조건부 컬럼은 비운다 — CFU 가 평가 순서상 앞이라 조건부는 의미가 없다),
      //   아니면 고른 조건 컬럼에 같은 번호. 조건별 모드: 입력한 그대로.
      let cols: Record<'forward_id' | CondKey, string>
      if (form.perCond) {
        cols = { forward_id: form.forward_id, forward_busy_id: form.forward_busy_id, forward_no_reply_id: form.forward_no_reply_id, forward_not_logged_in_id: form.forward_not_logged_in_id, forward_not_reachable_id: form.forward_not_reachable_id }
      } else {
        const n = form.fwdNumber.trim()
        const anyCond = COND_KEYS.some(k => form.fwdCond[k])
        if (n && !form.fwdCfu && !anyCond) { show('전환 조건을 하나 이상 고르세요 (무조건 또는 통화중·무응답·미등록·도달불가)', 'err'); return }
        if (!n && (form.fwdCfu || anyCond)) { show('전환 번호를 입력하세요', 'err'); return }
        cols = { forward_id: form.fwdCfu ? n : '', forward_busy_id: '', forward_no_reply_id: '', forward_not_logged_in_id: '', forward_not_reachable_id: '' }
        if (!form.fwdCfu) for (const k of COND_KEYS) cols[k] = form.fwdCond[k] ? n : ''
      }
      const labels: Record<'forward_id' | CondKey, string> = { forward_id: '무조건 (CFU)', ...COND_LABEL }
      for (const key of ['forward_id', ...COND_KEYS] as const) { const v = cols[key].trim(); if (v && !FORWARD_RE.test(v)) { show(`${labels[key]} 대상은 번호(숫자열, 선행 + 허용)여야 합니다`, 'err'); return } }
      d.dnd = form.dnd; d.forward_id = cols.forward_id.trim()
      if (hasCdiv) {
        const sec = Number(form.forward_no_reply_sec || 0)
        if (!Number.isInteger(sec) || sec < 0 || sec > 120) { show('무응답 시한은 0~120초 (0 = 서버 기본)', 'err'); return }
        for (const k of COND_KEYS) d[k] = cols[k].trim()
        d.forward_no_reply_sec = sec
      }
      if (hasRingback) d.ringback_media = form.ringback_media.trim() || null
    }
    if (svc === 'ptt' && pform) {
      if (pform.emergency_group_mode === 'DedicatedGroup' && !pform.emergency_group_id) { show('긴급 그룹을 고르세요 — 미지정이면 SOS 가 불발됩니다', 'err'); return }
      if (pform.allow_emergency_private_call && pform.private_emergency_mode === 'UsePreConfigured' && !pform.emergency_private_recipient?.trim()) { show('사전 지정 수신자가 필요합니다 — 미지정이면 긴급 사설콜이 불발됩니다', 'err'); return }
    }
    try {
      await usersApi.updateSub(user.id, svc, sub.id, d)
      if (svc === 'ptt' && pform) await usersApi.updatePttProfile(user.id, sub.id, { ...pform, emergency_private_recipient: pform.emergency_private_recipient?.trim() || null })
      show(`${spec.label} ${sub.id} 저장 — 다음 REGISTER·착신부터 적용`, 'ok'); onDone()
    } catch (e: unknown) { show(String(e), 'err') }
  }
  async function del() {
    if (!await confirm({ title: '회선 삭제', tone: 'danger', confirmLabel: '삭제', body: `${spec.label} ${sub.id} 을(를) 삭제합니다.` })) return
    try { await usersApi.deleteSub(user.id, svc, sub.id); show('삭제', 'ok'); onDone() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  const isAka = (form ? (spec.authSchemes.length === 1 ? spec.authSchemes[0] : form.auth_scheme) : sub.auth_scheme) === 'aka'
  const sosGroupName = (id: string | null) => id ? (pttGroups.find(g => g.id === id)?.name || id) : null

  return (
    <article className={`rounded-md border bg-card ${editing ? 'border-primary' : highlight ? 'border-primary shadow-[0_0_0_3px_var(--cims-brand-soft)]' : 'border-border'}`}>
      {/* 카드 헤더 */}
      <div className="flex flex-wrap items-center gap-2.5 border-b border-border px-3.5 py-2.5">
        <SvcBadge svc={svc} />
        <strong className="font-mono text-md">{sub.id}</strong>
        {spec.showExtension && <Badge variant="neutralSoft" title="내선 라벨(끝자리, 표시 전용) — 망 주소는 E.164">내선 {extensionOf(sub.id)}</Badge>}
        <StatusDot tone={reg ? 'success' : 'neutral'} label={reg ? '등록' : '미등록'} title={reg ? `REGISTER ${sub.register_time}` : (sub.logout_time ? `해제 ${sub.logout_time}` : '등록 이력 없음')} />
        <span className="ml-auto flex gap-1">
          {editing ? <>
            <Button variant="ghost" onClick={onCancel}>취소</Button>
            <Button variant="default" onClick={save}>저장</Button>
          </> : canWrite && <>
            <Button onClick={onEdit}><Pencil size={13} /> 편집</Button>
            <IconBtn title="회선 삭제" tone="danger" onClick={del}><Trash2 size={ICON} /></IconBtn>
          </>}
        </span>
      </div>

      {/* 보기 */}
      {!editing && (
        <div className="flex flex-col gap-3 px-3.5 py-3">
          <KV rows={[
            ...(svc === 'voip'
              ? [['전화 그룹', <span title={PICKUP_TITLE}>{phoneGroup ? `${phoneGroup.name} (${phoneGroup.id}) — 픽업·BLF 축` : (sub.pickup_group ? <PickupCell value={sub.pickup_group} /> : '없음 — 픽업 불가')}</span>] as [string, React.ReactNode],
                 ['서비스 · 채널', <span>{sub.service_ref || '—'} · <TransportBadge v={sub.sip_transport} aka={false} /> (Digest)</span>] as [string, React.ReactNode]]
              : [['IMSI', <span className="font-mono">{sub.imsi || '—'}</span>] as [string, React.ReactNode],
                 ['서비스 · 채널', <span>{sub.service_ref || '—'} · <TransportBadge v={sub.sip_transport} aka={sub.auth_scheme === 'aka'} /> · <AuthBadge sub={sub} /></span>] as [string, React.ReactNode]]),
            ...(spec.showDnd ? [
              ['착신 처리', <span>{sub.dnd && <Badge variant="dangerSoft" className="mr-1">DND</Badge>}{forwardSummary(sub) || (sub.dnd ? '' : '—')}</span>] as [string, React.ReactNode],
              ['링백', hasRingback ? <span className="font-mono">{sub.ringback_media || '서비스 프로파일 그대로'}</span> : <span className="text-muted-foreground">DB 마이그레이션 전</span>] as [string, React.ReactNode],
            ] : [
              ['소속 그룹', memberGroups.length ? <span className="flex flex-wrap gap-1">{memberGroups.map(g => <Badge key={g.id} variant="successSoft">{g.name || g.id}</Badge>)}</span> : '없음'] as [string, React.ReactNode],
            ]),
          ]} />
          {svc === 'ptt' && (
            <div className="flex flex-col gap-2 border-t border-dashed border-border pt-2.5">
              <Section title="긴급 (SOS)">
                {prof === undefined ? <span className="text-xs text-muted-foreground">프로파일 읽는 중…</span>
                : prof === null ? <span className="text-xs text-muted-foreground">프로파일 조회 실패(서버 구버전?)</span>
                : <KV rows={[
                    ['긴급 그룹', <span className="flex flex-wrap items-center gap-1.5">
                      {prof.emergency_group_mode === 'DedicatedGroup'
                        ? (prof.emergency_group_id ? <Badge variant="dangerSoft">{sosGroupName(prof.emergency_group_id)}</Badge> : <Badge variant="dangerSoft"><AlertTriangle size={10} className="mr-0.5" /> 미지정 — SOS 불발</Badge>)
                        : <Badge variant="neutralSoft">현재 선택 그룹 (단말)</Badge>}
                      <span className="text-xs text-muted-foreground">소속 그룹 중 하나 · 미지정이면 단말이 선택한 그룹</span></span>],
                    ['개시 허용', [prof.allow_emergency_call ? '긴급 그룹콜' : '', prof.allow_emergency_alert ? '긴급 경보' : '', prof.allow_adhoc_call ? '애드혹' : ''].filter(Boolean).join(' · ') || '전부 차단'],
                    ['긴급 사설콜', prof.allow_emergency_private_call
                      ? `${PRIV_LABEL[prof.private_emergency_mode]}${prof.private_emergency_mode === 'UsePreConfigured' ? ` → ${prof.emergency_private_recipient || '(수신자 미지정 — 불발)'}` : ''}`
                      : '차단'],
                    ['청취 자격', <span>{prof.allow_ambient_listening ? '허용' : '없음'} <span className="text-xs text-muted-foreground">— 역할 배정의 결과 (역할 · 그룹 탭)</span></span>],
                    ...(!prof.exists ? [['', <span className="text-xs text-muted-foreground">(저장된 프로파일 없음 — 서버 기본값)</span>] as [string, React.ReactNode]] : []),
                  ]} />}
              </Section>
            </div>
          )}
        </div>
      )}

      {/* 편집 */}
      {editing && form && (
        <div className="flex flex-col gap-4 px-3.5 py-3">
          <div className="grid grid-cols-3 gap-2.5">
            <Field label="접속서비스">
              <Select value={toSel(form.service_ref)} onValueChange={(v: string) => setForm({ ...form, service_ref: fromSel(v) })}>
                <SelectTrigger><SelectValue /></SelectTrigger>
                <SelectContent>{catalog.filter(c => c.svc === svc).map(c => <SelectItem key={c.ref} value={c.ref}>{c.ref}</SelectItem>)}</SelectContent>
              </Select>
            </Field>
            {!spec.imsiAuto && <Field label="IMSI"><Input className="font-mono" placeholder="SIM IMSI" value={form.imsi} onChange={e => setForm({ ...form, imsi: e.target.value })} /></Field>}
            <Field label="비밀번호 (변경 시)"><Input type="password" placeholder="••••" value={form.passwd} onChange={e => setForm({ ...form, passwd: e.target.value })} /></Field>
            {spec.authSchemes.length > 1 && <Field label="인증 체계"><AuthSelect value={form.auth_scheme} onChange={v => setForm({ ...form, auth_scheme: v })} /></Field>}
            <Field label="채널 정책" title={TRANSPORT_TITLE}>
              {isAka ? <div className="pt-1.5"><TransportFixedAka /></div>
               : spec.fixedTransport ? <div className="pt-1.5"><Badge variant="dangerSoft" title="유선 규약 — Digest+TLS">TLS (유선 규약)</Badge></div>
               : <TransportSelect value={form.sip_transport} onChange={v => setForm({ ...form, sip_transport: v })} />}
            </Field>
            {isAka && spec.authSchemes.includes('aka') && <Field label="K / OPc" className="col-span-2"><AkaKeyInputs k={form.k} opc={form.opc} keep={!!sub.aka_provisioned} onChange={(k, opc) => setForm({ ...form, k, opc })} /></Field>}
            {svc === 'voip' && <Field label="전화 그룹 (픽업)" title={PICKUP_TITLE}><div className="pt-1.5 text-sm text-muted-foreground">{phoneGroup ? phoneGroup.name : '없음'} <span className="text-xs">— 구성 › 전화 그룹에서</span></div></Field>}
          </div>

          {spec.showDnd && (
            <>
              <Section title="착신전환 서비스" aside={hasCdiv ? <button type="button" className="text-xs text-primary hover:underline" onClick={() => setForm({ ...form, perCond: !form.perCond })}>{form.perCond ? '번호 하나로 묶기' : '조건별 번호 따로…'}</button> : undefined}>
                {!form.perCond ? (
                  <>
                    <div className="grid grid-cols-[minmax(0,1fr)_96px] gap-2.5">
                      <Field label="전환 번호"><Input className="font-mono" placeholder="번호 · 비우면 전환 없음" value={form.fwdNumber} onChange={e => setForm({ ...form, fwdNumber: e.target.value })} /></Field>
                      <Field label="무응답 시한 (s)" title="CFNR 무응답 시한 — 비우면 CSP 기본(Setup.Sip.Cdiv.NoReplySec)"><Input type="number" min={0} max={120} placeholder="기본" disabled={!hasCdiv || form.fwdCfu || !form.fwdCond.forward_no_reply_id} value={form.forward_no_reply_sec} onChange={e => setForm({ ...form, forward_no_reply_sec: e.target.value })} /></Field>
                    </div>
                    <div className="flex flex-wrap items-center gap-x-4 gap-y-1.5 text-sm">
                      <label className="flex items-center gap-2 font-medium"><Checkbox checked={form.fwdCfu} onCheckedChange={c => setForm({ ...form, fwdCfu: c === true })} /> 무조건 (CFU)</label>
                      <span className="text-muted-foreground">|</span>
                      {COND_KEYS.map(k => (
                        <label key={k} className={`flex items-center gap-2 ${form.fwdCfu || !hasCdiv ? 'opacity-50' : ''}`} title={!hasCdiv ? '조건부 전환은 DB 마이그레이션(migrate_subscription_cdiv.sql) 뒤에 열린다' : form.fwdCfu ? '무조건 전환이 우선이라 조건부는 평가되지 않는다' : undefined}>
                          <Checkbox disabled={form.fwdCfu || !hasCdiv} checked={form.fwdCond[k]} onCheckedChange={c => setForm({ ...form, fwdCond: { ...form.fwdCond, [k]: c === true } })} /> {COND_LABEL[k]}
                        </label>
                      ))}
                    </div>
                  </>
                ) : (
                  <div className="grid grid-cols-2 gap-2.5">
                    <Field label="무조건 (CFU)"><Input className="font-mono" placeholder="번호 · 비우면 없음" value={form.forward_id} onChange={e => setForm({ ...form, forward_id: e.target.value })} /></Field>
                    <Field label="통화중 (CFB)"><Input className="font-mono" placeholder="번호" value={form.forward_busy_id} onChange={e => setForm({ ...form, forward_busy_id: e.target.value })} /></Field>
                    <div className="grid grid-cols-[minmax(0,1fr)_84px] gap-2">
                      <Field label="무응답 (CFNR)"><Input className="font-mono" placeholder="번호" value={form.forward_no_reply_id} onChange={e => setForm({ ...form, forward_no_reply_id: e.target.value })} /></Field>
                      <Field label="시한 (s)"><Input type="number" min={0} max={120} placeholder="기본" value={form.forward_no_reply_sec} onChange={e => setForm({ ...form, forward_no_reply_sec: e.target.value })} /></Field>
                    </div>
                    <Field label="미등록 (CFNL)"><Input className="font-mono" placeholder="번호" value={form.forward_not_logged_in_id} onChange={e => setForm({ ...form, forward_not_logged_in_id: e.target.value })} /></Field>
                    <Field label="도달불가 (CFNRc)"><Input className="font-mono" placeholder="번호" value={form.forward_not_reachable_id} onChange={e => setForm({ ...form, forward_not_reachable_id: e.target.value })} /></Field>
                  </div>
                )}
                <div className="text-xs text-muted-foreground">번호는 숫자열(선행 + 허용). 무조건(CFU)을 고르면 조건부는 평가되지 않으며 저장 시 비워진다. 조건부는 다중 선택 — 통화중 486 · 링잉 뒤 무응답(시한) · 미등록 · 도달불가(링잉 없는 480/408). 전환 대상은 등록 가입자만, 전환 상한·기본 시한은 CSP 설정 [착신전환].</div>
              </Section>
              <Section title="착신 거부">
                <label className="flex items-center gap-2 text-sm"><Checkbox checked={form.dnd} onCheckedChange={c => setForm({ ...form, dnd: c === true })} /> DND — 모든 착신을 603 으로 거절 (착신전환보다 우선)</label>
              </Section>
              {hasRingback && (
                <Section title="링백 (컬러링)">
                  <div className="grid grid-cols-[minmax(0,1fr)_auto] items-end gap-2">
                    <Field label="음원">
                      {media && media.length ? (
                        <Select value={toSel(form.ringback_media)} onValueChange={(v: string) => setForm({ ...form, ringback_media: fromSel(v) })}>
                          <SelectTrigger className="font-mono"><SelectValue /></SelectTrigger>
                          <SelectContent>
                            <SelectItem value={NONE}>(서비스 프로파일 그대로)</SelectItem>
                            {[...new Set([...media, ...(form.ringback_media ? [form.ringback_media] : [])])].map(m => <SelectItem key={m} value={m} className="font-mono">{m}</SelectItem>)}
                          </SelectContent>
                        </Select>
                      ) : <Input className="font-mono" placeholder="sys:<name> | op:<name> | sub:<번호> — 비우면 프로파일 그대로" value={form.ringback_media} onChange={e => setForm({ ...form, ringback_media: e.target.value })} />}
                    </Field>
                    <Button asChild><a href="/service/announcements" title="서비스 › 안내음성 — [가입자 링백…] 으로 WAV 를 올리면 이 회선 링백으로 지정된다"><Upload size={13} /> WAV 업로드 (sub:)</a></Button>
                  </div>
                  <div className="text-xs text-muted-foreground">발신자가 이 음원을 들으려면 발신자 접속서비스의 안내 프로파일이 서버 링백(ringback)이어야 한다 (announcements.md §6.3).</div>
                </Section>
              )}
            </>
          )}

          {svc === 'ptt' && (
            <>
              <Section title="소속 그룹">
                <div className="flex flex-wrap items-center gap-1.5">
                  {memberGroups.map(g => <Badge key={g.id} variant="successSoft">{g.name || g.id}</Badge>)}
                  {!memberGroups.length && <span className="text-xs text-muted-foreground">없음</span>}
                  <Button asChild variant="ghost" className="text-primary"><a href="/subscribers/ptt-groups">PTT 그룹에서 관리</a></Button>
                </div>
              </Section>
              <Section title="긴급 (SOS)">
                {!pform ? <span className="text-xs text-muted-foreground">프로파일을 읽지 못해 긴급 설정은 편집할 수 없습니다</span> : (
                  <div className="grid grid-cols-2 gap-2.5">
                    <Field label="긴급 그룹콜 대상">
                      <Select value={toSel(pform.emergency_group_mode === 'DedicatedGroup' ? (pform.emergency_group_id || '') : '')}
                        onValueChange={(v: string) => { const id = fromSel(v); setPform({ ...pform, emergency_group_mode: id ? 'DedicatedGroup' : 'UseCurrentlySelectedGroup', emergency_group_id: id || null }) }}>
                        <SelectTrigger><SelectValue /></SelectTrigger>
                        <SelectContent>
                          <SelectItem value={NONE}>현재 선택 그룹 (단말)</SelectItem>
                          {memberGroups.map(g => <SelectItem key={g.id} value={g.id}>{g.name || g.id}</SelectItem>)}
                          {pform.emergency_group_id && !memberGroups.some(g => g.id === pform.emergency_group_id) &&
                            <SelectItem value={pform.emergency_group_id}>{sosGroupName(pform.emergency_group_id)} (미소속)</SelectItem>}
                        </SelectContent>
                      </Select>
                    </Field>
                    <div className="flex flex-col gap-1.5 pt-4">
                      <label className="flex items-center gap-2 text-sm"><Checkbox checked={pform.allow_emergency_call} onCheckedChange={c => setPform({ ...pform, allow_emergency_call: c === true })} /> 긴급 그룹콜 개시</label>
                      <label className="flex items-center gap-2 text-sm"><Checkbox checked={pform.allow_emergency_alert} onCheckedChange={c => setPform({ ...pform, allow_emergency_alert: c === true })} /> 긴급 경보 개시</label>
                      <label className="flex items-center gap-2 text-sm"><Checkbox checked={pform.allow_adhoc_call} onCheckedChange={c => setPform({ ...pform, allow_adhoc_call: c === true })} /> 애드혹 개시</label>
                    </div>
                    <Field label="긴급 사설콜 (1:1)">
                      <Select value={toSel(pform.allow_emergency_private_call ? pform.private_emergency_mode : 'off')}
                        onValueChange={(v: string) => { const m = fromSel(v); setPform(m === 'off' ? { ...pform, allow_emergency_private_call: false } : { ...pform, allow_emergency_private_call: true, private_emergency_mode: m as McpttProfile['private_emergency_mode'] }) }}>
                        <SelectTrigger><SelectValue /></SelectTrigger>
                        <SelectContent>
                          <SelectItem value="LocallyDetermined">{PRIV_LABEL.LocallyDetermined}</SelectItem>
                          <SelectItem value="UsePreConfigured">{PRIV_LABEL.UsePreConfigured}</SelectItem>
                          <SelectItem value="off">차단</SelectItem>
                        </SelectContent>
                      </Select>
                    </Field>
                    {pform.allow_emergency_private_call && pform.private_emergency_mode === 'UsePreConfigured' && (
                      <Field label="사전 지정 수신자"><Input className="font-mono" placeholder="+825… (PTT 번호 — 서버가 존재 검증)" value={pform.emergency_private_recipient || ''} onChange={e => setPform({ ...pform, emergency_private_recipient: e.target.value || null })} /></Field>
                    )}
                  </div>
                )}
                <div className="text-xs text-muted-foreground">긴급 그룹은 소속 그룹 중에서 고른다(TS 24.484 entry-info). 청취 자격(allow-ambient-listening)은 역할 배정으로 정해지며 여기서 편집하지 않는다.</div>
              </Section>
            </>
          )}
        </div>
      )}
    </article>
  )
}

// ── 회선 추가 카드 — 종류를 정한 뒤 그 종류에 필요한 항목만 ──
interface AddNum { id: string; imsi: string; ref: string; passwd: string; sip_transport: SipTransport | ''; auth_scheme: AuthScheme; k: string; opc: string }
function AddLineCard({ user, svc, catalog, onCancel, onAdded }: { user: UserSummary; svc: LineSvc; catalog: ServiceCat[]; onCancel: () => void; onAdded: () => void }) {
  const { show } = useToast()
  const spec = LINE[svc]
  const refs = catalog.filter(c => c.svc === svc)
  const [f, setF] = useState<AddNum>({ id: '', imsi: '', ref: refs[0]?.ref || spec.defaultRef, passwd: '', sip_transport: spec.defaultTransport, auth_scheme: 'digest', k: '', opc: '' })
  const [busy, setBusy] = useState(false)
  const scheme: AuthScheme = spec.authSchemes.length === 1 ? spec.authSchemes[0] : f.auth_scheme
  const isAka = scheme === 'aka'

  async function add() {
    if (!f.id.trim()) { show(`${spec.numLabel} 필수`, 'err'); return }
    const imsi = f.imsi.trim() || (spec.imsiAuto ? digitsOf(f.id) : '')
    if (!imsi) { show('IMSI 필수', 'err'); return }
    if (!f.passwd && !isAka) { show('비밀번호 필수', 'err'); return }
    const aka = akaBody(scheme, f.k, f.opc, false)
    if (aka.err) { show(aka.err, 'err'); return }
    const body: Partial<Subscription> = { id: f.id.trim(), imsi, service_ref: f.ref, sip_transport: isAka ? 'TLS' : (f.sip_transport || null), dnd: false, forward_id: '', ...aka.fields }
    if (f.passwd) body.passwd = f.passwd
    setBusy(true)
    try { await usersApi.addSub(user.id, svc, body); show(`${spec.label} ${body.id} 추가 — 단말이 등록하면 '등록'으로 바뀝니다`, 'ok'); onAdded() }
    catch (e: unknown) { show(String(e), 'err') } finally { setBusy(false) }
  }

  return (
    <article className="rounded-md border border-dashed border-primary bg-card">
      <div className="flex items-center gap-2.5 border-b border-border px-3.5 py-2.5">
        <SvcBadge svc={svc} /><span className="font-semibold">새 회선 — {user.name}</span>
        <span className="ml-auto flex gap-1"><Button variant="ghost" onClick={onCancel}>취소</Button><Button variant="default" disabled={busy} onClick={add}>추가</Button></span>
      </div>
      <div className="flex flex-col gap-3 px-3.5 py-3">
        <div className="grid grid-cols-3 gap-2.5">
          <Field label={`${spec.numLabel} *`}><Input autoFocus className="font-mono" placeholder={spec.msisdnPlaceholder} value={f.id} onChange={e => setF({ ...f, id: e.target.value })} />
            {spec.showExtension && f.id && <span className="text-xs text-muted-foreground">내선 {extensionOf(f.id)}</span>}</Field>
          <Field label="접속서비스">
            <Select value={toSel(f.ref)} onValueChange={(v: string) => setF({ ...f, ref: fromSel(v) })}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>{(refs.length ? refs : [{ svc, ref: f.ref }]).map(c => <SelectItem key={c.ref} value={c.ref}>{c.ref}</SelectItem>)}</SelectContent>
            </Select>
          </Field>
          <Field label={spec.imsiAuto ? 'IMSI' : 'IMSI *'}><Input className="font-mono" placeholder={spec.imsiAuto ? '비우면 번호 숫자' : 'SIM IMSI'} value={f.imsi} onChange={e => setF({ ...f, imsi: e.target.value })} /></Field>
          {spec.authSchemes.length > 1 && <Field label="인증 체계"><AuthSelect value={f.auth_scheme} onChange={v => setF({ ...f, auth_scheme: v })} /></Field>}
          <Field label={isAka ? '비밀번호' : '비밀번호 *'}><Input type="password" placeholder={isAka ? '선택' : 'H(A1) 생성'} value={f.passwd} onChange={e => setF({ ...f, passwd: e.target.value })} /></Field>
          <Field label="채널 정책" title={TRANSPORT_TITLE}>
            {isAka ? <div className="pt-1.5"><TransportFixedAka /></div>
             : spec.fixedTransport ? <div className="pt-1.5"><Badge variant="dangerSoft" title="유선 규약 — Digest+TLS">TLS (유선 규약)</Badge></div>
             : <TransportSelect value={f.sip_transport} onChange={v => setF({ ...f, sip_transport: v })} />}
          </Field>
          {isAka && <Field label="K / OPc *" className="col-span-3"><AkaKeyInputs k={f.k} opc={f.opc} onChange={(k, opc) => setF({ ...f, k, opc })} /></Field>}
        </div>
        <div className="text-xs text-muted-foreground">{spec.addHint}</div>
      </div>
    </article>
  )
}

// ── 역할 · 그룹 탭 — 읽기 전용 요약 + 관리 화면 링크 ──
function RolesTab({ user, pttGroups, phoneGroups }: { user: UserSummary; pttGroups: Group[]; phoneGroups: PhoneGroup[] }) {
  const [roles, setRoles] = useState<RoleDef[] | null | undefined>(undefined)
  const loaded = useRef(false)
  useEffect(() => {
    if (loaded.current) return
    loaded.current = true
    // 사람당 역할 하나 — 역할별 배정 목록에서 이 사람을 찾는다(mcptt_authorization.md §2)
    rolesApi.list().then(async r => {
      const found: RoleDef[] = []
      await Promise.all(r.roles.map(async role => {
        try { const a = await rolesApi.listAssignments(role.id); if (a.some(x => x.principal_type === 'user' && String(x.principal_id) === String(user.id))) found.push(role) } catch { /* ignore */ }
      }))
      setRoles(found)
    }).catch(() => setRoles(null))
  }, [user.id])
  const pttNums = LINE.ptt.subsOf(user).map(s => s.id)
  const voipNums = LINE.voip.subsOf(user).map(s => s.id)
  const pgs = phoneGroups.filter(g => voipNums.some(n => phoneGroupOf([g], n)))
  const pgroups = pttGroups.filter(g => pttNums.some(n => pttGroupsOf([g], n).length))
  return (
    <>
      <div className="flex flex-col gap-2 rounded-md border border-border p-3.5">
        <Section title="역할">
          <div className="flex flex-wrap items-center gap-1.5">
            {roles === undefined && <span className="text-xs text-muted-foreground">읽는 중…</span>}
            {roles === null && <span className="text-xs text-muted-foreground">역할 목록을 읽지 못했습니다</span>}
            {roles && roles.length === 0 && <span className="text-xs text-muted-foreground">배정된 역할 없음</span>}
            {roles?.map(r => <Badge key={r.id} variant="brandSoft">{r.name || r.id}</Badge>)}
            <Button asChild variant="ghost" className="text-primary"><a href="/mcptt/roles">역할 · 권한에서 배정</a></Button>
          </div>
          <div className="text-xs text-muted-foreground">청취 자격(allow-ambient-listening)·감청·이력 열람 범위는 역할 배정의 결과. 여기서는 읽기만.</div>
        </Section>
      </div>
      <div className="flex flex-col gap-2 rounded-md border border-border p-3.5">
        <Section title="전화 그룹 · PTT 그룹">
          <KV rows={[
            ['전화 그룹', <span className="flex flex-wrap items-center gap-1.5">{pgs.length ? pgs.map(g => <span key={g.id}>{g.name} ({g.id}){g.pilot_id ? ` · 대표번호 ${extensionOf(g.pilot_id)}` : ''}</span>) : '—'}<Button asChild variant="ghost" className="text-primary"><a href="/subscribers/phone-groups">관리</a></Button></span>],
            ['PTT 그룹', <span className="flex flex-wrap items-center gap-1.5">{pgroups.length ? pgroups.map(g => <Badge key={g.id} variant="successSoft">{g.name || g.id}</Badge>) : '—'}<Button asChild variant="ghost" className="text-primary"><a href="/subscribers/ptt-groups">관리</a></Button></span>],
          ]} />
        </Section>
      </div>
    </>
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
  return <Badge variant={LINE[svc].badge}>{LINE[svc].label}</Badge>
}

// 픽업그룹 칸 — 값은 전화 그룹 멤버십에서 파생된다(SoT = 멤버십, 직접 편집 409). 읽기 전용.
const PICKUP_TITLE = '전화 그룹 멤버십에서 파생 — 구성 › 전화 그룹. 빈 값 = 어떤 픽업·BLF 축에도 속하지 않음. 반영은 다음 등록 갱신부터'
function PickupCell({ value }: { value?: string | null }) {
  if (isPhoneGroupId(value)) return <Badge variant="brandSoft" title={PICKUP_TITLE}>{value}</Badge>
  return <span className="text-sm text-muted-foreground" title={PICKUP_TITLE}>{value || '—'}</span>
}
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
  return <Badge variant={sub.aka_provisioned ? 'successSoft' : 'dangerSoft'}
    title={sub.aka_provisioned ? 'IMS AKA — K/OPc 보관됨, 보호 채널(TLS/IPsec) 강제' : 'IMS AKA — K/OPc 미보관(등록 불가)'}>AKA{sub.aka_provisioned ? '' : <AlertTriangle size={10} className="ml-0.5 inline align-[-1px]" />}</Badge>
}
// K/OPc 입력 — 편집 시 비우면 보관 키 유지(aka_provisioned 일 때). 둘 다 hex32.
function AkaKeyInputs({ k, opc, keep, onChange }: { k: string; opc: string; keep?: boolean; onChange: (k: string, opc: string) => void }) {
  return <div className="grid grid-cols-2 gap-2">
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
  return <Badge variant={v === 'TLS' ? 'dangerSoft' : 'brandSoft'} title={v === 'TLS' ? '서버 집행 — 비-TLS 채널 요청 403' : '프로비저닝 힌트'}>{v}</Badge>
}
// AKA 가입자는 채널 정책(sip_transport) 값과 무관하게 보호 채널이 강제된다(requiresTls = TLS ∨ aka,
//   sip_access_security.md §8.2) — 선택이 무의미하므로 고정 표시한다. 프로비저닝도 목록을 TLS 로 좁힌다.
function TransportFixedAka() {
  return <Badge variant="dangerSoft"
    title="AKA — 보호 채널(TLS) 강제. sip_transport 값과 무관하게 비-TLS 요청은 403이며, 단말 프로비저닝 목록도 TLS 하나로 좁혀진다. 접속서비스에 TLS 접속점(tls_port)이 없으면 등록 불가">TLS (AKA 강제)</Badge>
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
