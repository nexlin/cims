// 토폴로지 캔버스 편집기 — 팔레트(왼쪽) · 캔버스(호스트 영역 안에 워커·대상 노드 카드, 워커 안에 풀, 선은 모델에서 파생) · 속성 패널(오른쪽) ·
// 하단 드로어(레코드 JSON · 검증 · 연결 검사). 빈 곳에 놓으면 상자 자동 생성, 카드 위치·영역 크기는 레코드 layout 에 저장
// (test_instrument.md §7 토폴로지 캔버스 사양 ①~⑥). 문서는 부모(페이지)가 소유 — 이 컴포넌트는 doc 을 받아 바뀐 사본을 onChange 로 준다.
import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState, type ReactNode } from 'react'
import { Server, Cpu, Radio, Waves, Users, Activity, Database, Smartphone, Globe, Phone, Router, Terminal, ChevronDown, ChevronRight, Trash2 } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Input } from '@core/components/ui/input'
import { Checkbox } from '@core/components/ui/checkbox'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { DataTable, Th, Td } from '@core/components/custom/data-table'
import { useToast } from '@core/components/Toast'
import { useConfirm } from '@core/components/custom/confirm'
import type { TopologyDoc, TopoNode, PoolDoc, PeerPoolDoc, UePoolDoc, CheckItem, NodeRole, Transport, WorkerRow } from '@tester/api/tester'
import * as M from '@tester/lib/topology-model'
import type { Focus, Issue, PaletteKind, Pos } from '@tester/lib/topology-model'

const HC = (i: number) => `var(--chart-${i})`
const ROLE_ICON: Record<NodeRole, ReactNode> = { sip: <Radio size={12} />, tas: <Cpu size={12} />, media: <Waves size={12} />, subscriber: <Users size={12} />, oam: <Activity size={12} />, db: <Database size={12} /> }
const PALETTE: { group: string; items: { kind: PaletteKind; label: string; hint: string; icon: ReactNode }[] }[] = [
  { group: '서버', items: [{ kind: 'host', label: '호스트', hint: '서버 한 대 — 주소·SSH 자격', icon: <Server size={13} /> }, { kind: 'obs_ssh', label: 'SSH 관측', hint: '호스트 위에 놓기 — 프로세스 CPU/메모리', icon: <Terminal size={13} /> }] },
  { group: '계측기 워커 (호스트 위에)', items: [{ kind: 'worker', label: '워커', hint: 'cims-tester-worker 프로세스', icon: <Cpu size={13} /> }] },
  { group: '풀 (워커 위에)', items: [
    { kind: 'ue', label: 'UE 풀', hint: '가상 단말 · DB/creds', icon: <Smartphone size={13} /> }, { kind: 'real-ue', label: '실단말 풀', hint: 'cimsue-cli', icon: <Smartphone size={13} /> },
    { kind: 'ibcf', label: 'IBCF 피어', hint: '타 사업자 IMS', icon: <Globe size={13} /> }, { kind: 'pbx', label: 'PBX 트렁크', hint: 'REGISTER · DID', icon: <Phone size={13} /> }, { kind: 'mgcf', label: 'MGCF', hint: 'PSTN 게이트웨이', icon: <Router size={13} /> }] },
  { group: '시험 대상 노드 (호스트 위에)', items: [
    { kind: 'n_sip', label: 'SIP 서버', hint: 'CSP · P/I/S-CSCF · SBC · IBCF', icon: ROLE_ICON.sip }, { kind: 'n_tas', label: 'TAS', hint: '보조 서비스 · 그룹콜 AS', icon: ROLE_ICON.tas },
    { kind: 'n_media', label: '미디어 서버', hint: 'CMP · MRF · TrGW — RTP 관측', icon: ROLE_ICON.media }, { kind: 'n_subscriber', label: '가입자 서버', hint: 'CSC · HSS — 프로비저닝 API', icon: ROLE_ICON.subscriber },
    { kind: 'n_oam', label: 'OAM', hint: '통계 · 알람 · 컬렉션 시드 (CIMS)', icon: ROLE_ICON.oam }, { kind: 'n_db', label: '가입자 DB', hint: 'MariaDB — H(A1) 원천', icon: ROLE_ICON.db }] },
]

type Drag =
  | { type: 'new'; kind: PaletteKind; x: number; y: number }
  | { type: 'pool'; id: string; x: number; y: number; sx: number; sy: number; moved: boolean }
  | { type: 'card'; kind: 'worker' | 'node'; id: string; x: number; y: number; ox: number; oy: number; moved: boolean }
  | { type: 'region'; id: string; ox: number; oy: number }
  | { type: 'resize'; id: string; ox: number; oy: number }

interface Edge { d: string; cls: 'udp' | 'tcp' | 'tls' | 'peer' | 'reg' | 'rtp' | 'db'; label: string | null; lx: number; ly: number; hi: boolean; dim: boolean; fail: boolean }
const EDGE_COLOR: Record<Edge['cls'], string> = { udp: 'var(--chart-1)', tcp: 'var(--chart-9)', tls: 'var(--chart-3)', peer: 'var(--chart-2)', reg: 'var(--chart-8)', rtp: 'var(--chart-11)', db: 'var(--chart-6)' }

const snap = (v: number) => Math.round(v / 10) * 10

export default function TopologyCanvas({ doc, onChange, check, workers, canWrite, onFocusCheck }: {
  doc: TopologyDoc
  onChange: (next: TopologyDoc) => void
  check: { items: CheckItem[]; at: string } | null
  workers: WorkerRow[]
  canWrite: boolean
  onFocusCheck?: () => void
}) {
  const { show } = useToast()
  const confirm = useConfirm()
  const [sel, setSel] = useState<Focus | null>(null)
  const [tab, setTab] = useState<'json' | 'issues' | 'check'>('issues')
  const [drawerOpen, setDrawerOpen] = useState(true)
  const [drag, setDrag] = useState<Drag | null>(null)
  const [hot, setHot] = useState<Focus | null>(null)
  const [edges, setEdges] = useState<Edge[]>([])
  const [collapsed, setCollapsed] = useState<Record<string, boolean>>(() => { try { return JSON.parse(localStorage.getItem('tester-topo-palette') || '{}') } catch { return {} } })
  const stageRef = useRef<HTMLDivElement>(null)
  const wrapRef = useRef<HTMLDivElement>(null)
  const dragRef = useRef<Drag | null>(null)
  dragRef.current = drag

  const issues = useMemo(() => M.validate(doc), [doc])
  const errsFor = useCallback((f: Focus) => issues.filter(i => i.focus && i.focus.kind === f.kind && i.focus.id === f.id), [issues])
  const mutate = useCallback((fn: (d: TopologyDoc) => void) => { const d = M.deep(doc); M.ensureLayout(d); fn(d); onChange(d) }, [doc, onChange])
  const L = doc.layout ?? { regions: {}, items: {} }
  const checkOf = useCallback((name: string) => check?.items.find(i => i.name === name), [check])
  const healthOf = useCallback((wname: string) => workers.find(w => w.name === wname), [workers])

  // ── 선 파생 + 영역 자동 확장 (렌더 뒤 DOM 측정) ──────────────────────────
  useLayoutEffect(() => {
    const st = stageRef.current; if (!st) return
    const sr = st.getBoundingClientRect()
    const ctr = (el: Element | null) => { if (!el) return null; const r = el.getBoundingClientRect(); return { x: r.left - sr.left + r.width / 2, y: r.top - sr.top + r.height / 2 } }
    const portA = (id: string) => ctr(st.querySelector(`[data-port="${id}"] [data-pa]`))
    const failing = (id: string) => { const c = checkOf(id); return !!c && !c.ok && !c.info }
    const out: Edge[] = []
    const edge = (a: Pos | null, b: Pos | null, cls: Edge['cls'], label: string | null, hi: boolean, fail: boolean) => {
      if (!a || !b) return
      const dx = Math.max(60, Math.abs(b.x - a.x) * 0.5)
      out.push({ d: `M${a.x},${a.y} C${a.x + dx},${a.y} ${b.x - dx},${b.y} ${b.x},${b.y}`, cls, label, lx: 0.125 * a.x + 0.375 * (a.x + dx) + 0.375 * (b.x - dx) + 0.125 * b.x, ly: (a.y + b.y) / 2, hi, dim: !!sel && !hi, fail })
    }
    for (const [n, p] of Object.entries(doc.pools ?? {})) {
      const hi = sel?.kind === 'pool' && sel.id === n
      const a = ctr(st.querySelector(`[data-pool-anchor="${n}"]`)); if (!a) continue
      if (M.isPeer(p)) {
        const seed = p.seed ?? {}
        const lbl = (doc.target.kind ?? 'cims') === 'cims' ? (seed.acl ? `ACL ${seed.acl}` : seed.route_set ? `${seed.route_set} · p${seed.priority ?? 100}${seed.distribution && seed.distribution !== 'failover' ? ` · ${seed.distribution}` : ''}` : `rs ${n}`) : `→ ${p.peering}`
        edge(a, portA(`${p.peering}:peering`), 'peer', lbl, hi, failing(`${p.peering}:peering`))
        if (p.register) edge({ x: a.x, y: a.y + 6 }, portA(`${p.peering}:${p.bind.protocol ?? 'udp'}`) ?? portA(`${p.peering}:peering`), 'reg', `REGISTER ${p.register.user}`, hi, false)
      } else {
        const tr = p.transport ?? 'udp'; const pid = `${p.access}:${tr}`
        edge(a, portA(pid), tr, `${tr}${p.srtp && p.srtp !== 'off' ? ` · srtp ${p.srtp}` : ''}`, hi, failing(pid))
        for (const m of M.mediaNodes(doc)) edge({ x: a.x, y: a.y + 6 }, portA(`${m}:rtp`), 'rtp', hi ? 'RTP' : null, hi, false)
        if (M.isUe(p) && 'db' in p.source) { const d = doc.target.nodes[p.source.db]; edge({ x: a.x, y: a.y + 10 }, portA(`${p.source.db}:${d?.role === 'db' ? 'db' : 'api'}`), 'db', `${p.source.table} ×${p.source.count}`, hi, false) }
      }
    }
    setEdges(prev => JSON.stringify(prev) === JSON.stringify(out) ? prev : out)
    // 영역은 내용물보다 작아지지 않는다
    let grew = false
    const d = M.deep(doc); const LL = M.ensureLayout(d)
    for (const [hid] of M.hosts(doc)) {
      const body = st.querySelector(`[data-region="${hid}"] [data-body]`); if (!body) continue
      let right = 0, bottom = 0
      body.querySelectorAll<HTMLElement>('[data-card]').forEach(c => { right = Math.max(right, c.offsetLeft + c.offsetWidth); bottom = Math.max(bottom, c.offsetTop + c.offsetHeight) })
      const need = { w: right ? right + 18 : 240, h: bottom ? bottom + 30 + 14 : 100 }
      const r = (LL.regions[hid] ||= { x: 40, y: 40, w: 300, h: 120 })
      if (r.w < need.w) { r.w = Math.ceil(need.w / 10) * 10; grew = true }
      if (r.h < need.h) { r.h = Math.ceil(need.h / 10) * 10; grew = true }
    }
    if (grew && !drag) onChange(d)
  })

  // ── 포인터 DnD ───────────────────────────────────────────────────────────
  const stagePt = (e: { clientX: number; clientY: number }): Pos => { const r = stageRef.current!.getBoundingClientRect(); return { x: e.clientX - r.left, y: e.clientY - r.top } }
  const under = (e: { clientX: number; clientY: number }) => document.elementFromPoint(e.clientX, e.clientY)
  const ctxOf = (el: Element | null) => {
    const h = el?.closest('[data-region]') as HTMLElement | null, w = el?.closest('[data-worker]') as HTMLElement | null, n = el?.closest('[data-node]') as HTMLElement | null
    return { host: h?.dataset.region ?? null, worker: w?.dataset.worker ?? null, node: n?.dataset.node ?? null, inCanvas: !!el && !!wrapRef.current?.contains(el) }
  }
  const wantNew = (kind: PaletteKind, ctx: ReturnType<typeof ctxOf>): Focus | null => {
    if (M.POOL_KINDS.includes(kind) && ctx.worker) return { kind: 'worker', id: ctx.worker }
    if ((kind === 'worker' || kind === 'obs_ssh' || kind.startsWith('n_')) && ctx.host) return { kind: 'host', id: ctx.host }
    return null
  }
  const wantPool = (id: string, ctx: ReturnType<typeof ctxOf>): Focus | null => {
    const p = doc.pools[id]; if (!p) return null
    if (ctx.worker && ctx.worker !== p.worker) return { kind: 'worker', id: ctx.worker }
    if (ctx.node) { const ok = M.isPeer(p) ? M.peeringNodes(doc).includes(ctx.node) : M.accessNodes(doc).includes(ctx.node); if (ok) return { kind: 'node', id: ctx.node } }
    return null
  }

  useEffect(() => {
    if (!drag) return
    const move = (e: PointerEvent) => {
      const d = dragRef.current; if (!d) return
      if (d.type === 'new' || d.type === 'pool') {
        const ctx = ctxOf(under(e))
        setHot(d.type === 'new' ? wantNew(d.kind, ctx) : wantPool(d.id, ctx))
        setDrag({ ...d, x: e.clientX, y: e.clientY, ...(d.type === 'pool' ? { moved: d.moved || Math.hypot(e.clientX - d.sx, e.clientY - d.sy) > 4 } : {}) } as Drag)
      } else if (d.type === 'card') {
        const p = stagePt(e)
        setDrag({ ...d, x: snap(p.x - d.ox), y: snap(p.y - d.oy), moved: true })
        const el = under(e); const rg = (el?.closest('[data-region]') as HTMLElement | null)?.dataset.region ?? null
        setHot(rg ? { kind: 'host', id: rg } : null)
      } else if (d.type === 'region') {
        mutate(x => { const r = M.ensureLayout(x).regions[d.id]; if (!r) return; r.x = snap(Math.max(10, e.clientX - d.ox)); r.y = snap(Math.max(10, e.clientY - d.oy)) })
      } else if (d.type === 'resize') {
        mutate(x => { const r = M.ensureLayout(x).regions[d.id]; if (!r) return; r.w = snap(Math.max(200, e.clientX - d.ox)); r.h = snap(Math.max(90, e.clientY - d.oy)) })
      }
    }
    const up = (e: PointerEvent) => {
      const d = dragRef.current; setDrag(null); setHot(null); if (!d) return
      if (d.type === 'new') {
        const el = under(e); const ctx = ctxOf(el); if (!ctx.inCanvas) return
        const pos = stagePt(e)
        mutate(x => { const r = M.createFromPalette(x, d.kind, pos, { host: ctx.host, worker: ctx.worker }); if (r.made.length) show(`${r.made.join(' · ')} 를 만들어 그 위에 놓았습니다 — 주소·포트를 확인하십시오`, 'ok'); if (r.sel) setSel(r.sel) })
      } else if (d.type === 'pool') {
        const ctx = ctxOf(under(e)); const t = wantPool(d.id, ctx)
        if (d.moved && t) mutate(x => {
          const p = x.pools[d.id]
          if (t.kind === 'worker') { p.worker = t.id; show(`${d.id} → ${t.id}`, 'ok') }
          else if (M.isPeer(p)) { p.peering = t.id; show(`${d.id} 다음 홉 → ${t.id}`, 'ok') }
          else { p.access = t.id; const a = x.target.nodes[t.id]?.sip?.access; if (a && !a[p.transport ?? 'udp']) p.transport = (['udp', 'tcp', 'tls'] as Transport[]).find(k => a[k]) ?? p.transport; show(`${d.id} 접속점 → ${t.id}`, 'ok') }
        })
        else if (d.moved && ctx.node) show(`${ctx.node} 에는 ${M.isPeer(doc.pools[d.id]) ? '피어링' : '접속'} 수신점이 없습니다 — 노드 속성에서 켜십시오`, 'err')
      } else if (d.type === 'card') {
        if (!d.moved) return
        const el = under(e); const rid = (el?.closest('[data-region]') as HTMLElement | null)?.dataset.region ?? null
        mutate(x => {
          const LL = M.ensureLayout(x); const obj = d.kind === 'worker' ? M.worker(x, d.id) : x.target.nodes[d.id]; if (!obj) return
          if (rid) { const r = LL.regions[rid]; LL.items[d.id] = { x: Math.max(4, d.x - r.x), y: Math.max(4, d.y - r.y - 30) }; if (obj.host !== rid) { obj.host = rid; show(`${d.id} → ${x.hosts[rid]?.name ?? rid}`, 'ok') } }
          else { LL.items[d.id] = { x: d.x, y: d.y }; if (obj.host) { obj.host = ''; show(`${d.id} 호스트 없음 — 영역 안으로 끌어 놓으십시오`, 'err') } }
        })
      }
    }
    window.addEventListener('pointermove', move); window.addEventListener('pointerup', up)
    return () => { window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up) }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [!!drag, doc])

  const startNew = (kind: PaletteKind) => (e: React.PointerEvent) => { if (!canWrite) return; e.preventDefault(); setDrag({ type: 'new', kind, x: e.clientX, y: e.clientY }) }
  const startPool = (id: string) => (e: React.PointerEvent) => { e.stopPropagation(); e.preventDefault(); setSel({ kind: 'pool', id }); if (canWrite) setDrag({ type: 'pool', id, x: e.clientX, y: e.clientY, sx: e.clientX, sy: e.clientY, moved: false }) }
  const startCard = (kind: 'worker' | 'node', id: string) => (e: React.PointerEvent) => {
    if ((e.target as HTMLElement).closest('input,select,button')) return
    e.stopPropagation(); e.preventDefault(); setSel({ kind, id }); if (!canWrite) return
    const r = (e.currentTarget as HTMLElement).getBoundingClientRect(); const sr = stageRef.current!.getBoundingClientRect()
    setDrag({ type: 'card', kind, id, x: r.left - sr.left, y: r.top - sr.top, ox: e.clientX - r.left, oy: e.clientY - r.top, moved: false })
  }
  const startRegion = (id: string) => (e: React.PointerEvent) => {
    if ((e.target as HTMLElement).closest('input,select,button,[data-card]')) return
    e.preventDefault(); setSel({ kind: 'host', id }); if (!canWrite) return
    const r = L.regions[id]; if (!r) return
    if ((e.target as HTMLElement).closest('[data-resize]')) { setDrag({ type: 'resize', id, ox: e.clientX - r.w, oy: e.clientY - r.h }); return }
    setDrag({ type: 'region', id, ox: e.clientX - r.x, oy: e.clientY - r.y })
  }

  const del = async (f: Focus) => {
    const refs = M.refsOf(doc, f)
    if (refs.length && !await confirm({ title: `${f.id} 삭제`, body: `${f.id} 위/참조: ${refs.join(', ')}. 지우면 그것들은 놓일 곳이 없어집니다. 지울까요?`, confirmLabel: '삭제', tone: 'danger' })) return
    mutate(x => M.deleteFocus(x, f)); setSel(null)
  }
  useEffect(() => {
    const kd = (e: KeyboardEvent) => { if ((e.target as HTMLElement).closest('input,select,textarea')) return; if (e.key === 'Escape') setSel(null); if ((e.key === 'Delete') && sel && canWrite) del(sel) }
    window.addEventListener('keydown', kd); return () => window.removeEventListener('keydown', kd)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [sel, doc, canWrite])

  const focusIssue = (f: Focus | null) => { if (f) setSel(f) }
  const errBadge = (f: Focus) => errsFor(f).some(i => i.level === 'err') ? <Badge variant="dangerSolid" className="h-4 px-1 text-[10px]">!</Badge> : null
  const resBadge = (name: string) => { const r = checkOf(name); if (!r) return null; return r.ok ? <Badge variant="successSoft" className="h-4 px-1 text-[10px]">OK {r.ms}ms</Badge> : r.info ? <Badge variant="neutralSoft" className="h-4 px-1 text-[10px]">참고</Badge> : <Badge variant="dangerSoft" className="h-4 px-1 text-[10px]">실패</Badge> }
  const isSel = (k: Focus['kind'], id: string) => sel?.kind === k && sel.id === id
  const isHot = (k: Focus['kind'], id: string) => hot?.kind === k && hot.id === id

  // ── 카드 ────────────────────────────────────────────────────────────────
  const PortRow = ({ id, cls, name, val }: { id: string; cls: Edge['cls'] | 'rtp' | 'db'; name: string; val: string }) => (
    <div data-port={id} className="flex items-center gap-1.5 border-t border-border px-2 py-0.5 text-[11px]">
      <span data-pa className="inline-block h-2 w-2 rounded-full" style={{ background: EDGE_COLOR[cls] }} />
      <span className="w-14 shrink-0 text-muted-foreground">{name}</span><span className="truncate font-mono" title={val}>{val}</span><span className="ml-auto">{resBadge(id)}</span>
    </div>
  )
  const PlainRow = ({ name, val }: { name: string; val: string }) => <div className="flex items-center gap-1.5 border-t border-border px-2 py-0.5 text-[11px]"><span className="w-14 shrink-0 text-muted-foreground">{name}</span><span className="truncate font-mono" title={val}>{val}</span></div>

  const NodeCard = ({ id, n, abs }: { id: string; n: TopoNode; abs?: Pos }) => {
    const it = L.items[id] ?? { x: 14, y: 12 }
    const pos = abs ?? it
    const hc = doc.hosts[n.host] ? HC(M.colorOf(doc, n.host)) : 'var(--destructive)'
    const a = n.sip?.access, pg = n.sip?.peering
    return (
      <div data-card data-node={id} onPointerDown={startCard('node', id)}
           className={`absolute w-[250px] cursor-grab select-none rounded-md border bg-card text-xs shadow-sm ${isSel('node', id) ? 'ring-2 ring-primary' : ''} ${isHot('node', id) ? 'ring-2 ring-success' : ''} ${doc.hosts[n.host] ? 'border-border' : 'border-dashed border-destructive'}`}
           style={{ left: pos.x, top: pos.y, borderTopColor: hc, borderTopWidth: 3, zIndex: drag?.type === 'card' && drag.id === id ? 20 : undefined }}>
        <div className="flex items-center gap-1.5 px-2 py-1">
          <span className="text-muted-foreground">{ROLE_ICON[n.role]}</span><b className="truncate">{n.label ?? id}</b><span className="text-muted-foreground">{n.fn}</span>{errBadge({ kind: 'node', id })}
          <span className="ml-auto truncate font-mono text-[10px] text-muted-foreground">{(n.procs ?? []).join(',') || '—'}</span>
        </div>
        {n.role === 'sip' && <>
          {a && (['udp', 'tcp', 'tls'] as const).map(t => a[t] ? <PortRow key={t} id={`${id}:${t}`} cls={t} name={`SIP ${t.toUpperCase()}`} val={`:${a[t]}`} /> : null)}
          {a && <PlainRow name="도메인" val={(a.domains ?? []).join(' · ') || '—'} />}
          {pg && <PortRow id={`${id}:peering`} cls="peer" name="피어링" val={`:${pg.port}/${pg.protocol ?? 'udp'}${pg.local_node ? ` · ${pg.local_node}` : ''}`} />}
          {!a && !pg && <PlainRow name="수신점" val="없음 — 관측만" />}
        </>}
        {n.role === 'media' && <><PortRow id={`${id}:rtp`} cls="rtp" name="RTP" val={n.media?.rtp_range ? `${n.media.rtp_range[0]}–${n.media.rtp_range[1]}` : '범위 없음'} />{n.media?.control ? <PlainRow name="제어" val={`:${n.media.control}`} /> : null}</>}
        {n.role === 'subscriber' && (n.api ? <PortRow id={`${id}:api`} cls="db" name="API" val={`:${n.api.port}${n.api.tls ? ' tls' : ''}`} /> : <PlainRow name="API" val="없음" />)}
        {n.role === 'oam' && <PortRow id={`${id}:oam`} cls="db" name="OAM" val={`:${n.oam?.port ?? 4419} · dep ${n.oam?.csp_deployment_id ?? '자동'}`} />}
        {n.role === 'db' && <PortRow id={`${id}:db`} cls="db" name="DB" val={`:${n.db?.port ?? 3306} / ${n.db?.name ?? 'cims'}`} />}
        {n.role === 'tas' && <PlainRow name="SIP" val={n.tas?.port ? `:${n.tas.port} (코어 내부)` : '—'} />}
      </div>
    )
  }
  const PoolCard = ({ pn, p }: { pn: string; p: PoolDoc }) => {
    const dragging = drag?.type === 'pool' && drag.id === pn
    const group = p.group ? <Badge variant="neutralSoft" className="h-4 px-1 text-[10px]" title={`논리 풀 ${p.group} — 시나리오는 이 이름으로 참조`}>≡ {p.group}</Badge> : null
    const anchorColor = M.isPeer(p) ? EDGE_COLOR.peer : EDGE_COLOR[(p.transport ?? 'udp') as 'udp' | 'tcp' | 'tls']
    let body: ReactNode
    if (M.isPeer(p)) { const idn = p.identities ?? {}; const rng = idn.e164_range ? `${idn.e164_range[0]}…${idn.e164_range[1].slice(-4)}` : idn.did_range ? `${idn.did_range[0]}…${idn.did_range[1].slice(-3)}` : '신원 없음'
      body = <><Badge variant={p.answer === 'silent' ? 'dangerSoft' : 'warningSoft'} className="h-4 px-1 text-[10px]">{p.profile}{p.answer === 'silent' ? ' · silent' : ''}</Badge><b className="truncate">{pn}</b><span className="truncate font-mono text-[10px] text-muted-foreground">:{p.bind.port}/{p.bind.protocol ?? 'udp'} · {rng}</span></> }
    else { const src = 'db' in p.source ? `${p.source.table.replace('_subscriptions', '')} ${p.source.offset ?? 0}+${p.source.count}` : `creds${p.source.count ? ` ${p.source.count}` : ''}`
      body = <><Badge variant={p.kind === 'ue' ? 'infoSoft' : 'successSoft'} className="h-4 px-1 text-[10px]">{p.kind}</Badge>{group}<b className="truncate">{pn}</b><span className="truncate font-mono text-[10px] text-muted-foreground">→ {p.access || '?'} {p.transport ?? 'udp'}{p.srtp && p.srtp !== 'off' ? '+srtp' : ''} · {src}</span></> }
    return (
      <div data-pool={pn} onPointerDown={startPool(pn)} title={pn}
           className={`relative flex cursor-grab select-none items-center gap-1.5 rounded-sm border bg-muted px-2 py-1 text-xs ${isSel('pool', pn) ? 'border-primary ring-1 ring-primary' : 'border-border'} ${dragging ? 'opacity-40' : ''}`}>
        {body}{errBadge({ kind: 'pool', id: pn })}
        <span data-pool-anchor={pn} className="absolute -right-1 top-1/2 h-2 w-2 -translate-y-1/2 rounded-full" style={{ background: anchorColor }} />
      </div>
    )
  }
  const WorkerCard = ({ name, abs }: { name: string; abs?: Pos }) => {
    const w = M.worker(doc, name)!; const it = L.items[name] ?? { x: 14, y: 12 }; const pos = abs ?? it
    const hc = doc.hosts[w.host] ? HC(M.colorOf(doc, w.host)) : 'var(--destructive)'
    const h = healthOf(name); const wr = checkOf(`worker_${name}`); const up = wr ? wr.ok : h ? h.up : null
    const pools = Object.entries(doc.pools).filter(([, p]) => p.worker === name)
    return (
      <div data-card data-worker={name} onPointerDown={startCard('worker', name)}
           className={`absolute w-[310px] cursor-grab select-none rounded-md border bg-card text-xs shadow-sm ${isSel('worker', name) ? 'ring-2 ring-primary' : ''} ${isHot('worker', name) ? 'ring-2 ring-success' : ''} ${doc.hosts[w.host] ? 'border-border' : 'border-dashed border-destructive'}`}
           style={{ left: pos.x, top: pos.y, borderTopColor: hc, borderTopWidth: 3, zIndex: drag?.type === 'card' && drag.id === name ? 20 : undefined }}>
        <div className="flex items-center gap-1.5 px-2 py-1">
          <Cpu size={12} className="text-muted-foreground" /><b>{name}</b>
          <span className={`inline-block h-2 w-2 rounded-full ${up === null ? 'bg-muted-foreground' : up ? 'bg-success' : 'bg-destructive'}`} />{errBadge({ kind: 'worker', id: name })}
          <span className="ml-auto truncate font-mono text-[10px] text-muted-foreground">:{w.port ?? 7100} · {w.cpus ?? '?'} cpu{h?.health ? ` · ${h.health.active_endpoints ?? 0}/${h.health.max_endpoints ?? '?'} ep` : ''}</span>
        </div>
        <div className="flex min-h-[30px] flex-col gap-1 border-t border-border p-1.5">
          {pools.length ? pools.map(([pn, p]) => <PoolCard key={pn} pn={pn} p={p} />) : <div className="rounded-sm border border-dashed border-border px-2 py-1 text-center text-[11px] text-muted-foreground">풀을 여기에 놓으십시오</div>}
        </div>
      </div>
    )
  }

  // ── 렌더 ────────────────────────────────────────────────────────────────
  const orphanW = doc.workers.filter(w => !doc.hosts[w.host]), orphanN = M.nodes(doc).filter(([, n]) => !doc.hosts[n.host]), orphanP = Object.entries(doc.pools).filter(([, p]) => !M.worker(doc, p.worker))
  const dragCard = drag?.type === 'card' ? drag : null
  const stageW = Math.max(1440, ...M.hosts(doc).map(([id]) => (L.regions[id]?.x ?? 0) + (L.regions[id]?.w ?? 0) + 40))
  const stageH = Math.max(900, ...M.hosts(doc).map(([id]) => (L.regions[id]?.y ?? 0) + (L.regions[id]?.h ?? 0) + 40))
  const errN = issues.filter(i => i.level === 'err').length, warnN = issues.filter(i => i.level === 'warn').length

  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <div className="grid min-h-0 flex-1 grid-cols-[210px_minmax(0,1fr)_320px]">
        {/* 팔레트 */}
        <aside className="min-h-0 overflow-auto border-r border-border bg-card p-2 text-xs">
          {PALETTE.map(g => (
            <div key={g.group} className="mb-2">
              <button className="flex w-full items-center gap-1 py-1 text-[11px] font-semibold text-muted-foreground" onClick={() => setCollapsed(c => { const n = { ...c, [g.group]: !c[g.group] }; try { localStorage.setItem('tester-topo-palette', JSON.stringify(n)) } catch { /* 무시 */ } return n })}>
                {collapsed[g.group] ? <ChevronRight size={12} /> : <ChevronDown size={12} />}{g.group}
              </button>
              {!collapsed[g.group] && g.items.map(it => (
                <div key={it.kind} onPointerDown={startNew(it.kind)} tabIndex={0}
                     className={`mb-1 flex select-none items-center gap-2 rounded-sm border border-border bg-muted px-2 py-1.5 ${canWrite ? 'cursor-grab hover:border-primary' : 'opacity-60'}`}>
                  <span className="text-muted-foreground">{it.icon}</span><span><b>{it.label}</b><div className="text-[10px] text-muted-foreground">{it.hint}</div></span>
                </div>
              ))}
            </div>
          ))}
          <div className="mt-2 rounded-sm border border-border p-2 text-[11px] leading-relaxed text-muted-foreground">
            <b>놓는 자리가 소속을 정합니다.</b> 호스트 영역 안에 워커·대상 노드, 워커 카드 안에 풀. 빈 곳에 놓으면 담을 상자를 만듭니다. 선은 그리지 않습니다 — 풀 카드를 SIP 노드 위에 놓으면 접속점/다음 홉이 바뀝니다. <kbd>Del</kbd> 삭제 · <kbd>Esc</kbd> 해제
          </div>
          <div className="mt-2 flex flex-col gap-1">
            <span className="text-[11px] font-semibold text-muted-foreground">프리셋</span>
            {(Object.keys(M.PRESETS) as (keyof typeof M.PRESETS)[]).map(k => (
              <Button key={k} variant="outline" size="sm" disabled={!canWrite} onClick={async () => {
                if (!await confirm({ title: `프리셋 ${M.PRESETS[k].name}`, body: '대상 노드·풀(과 대상 전용 호스트)을 바꿉니다. 워커가 있는 호스트는 유지합니다.', confirmLabel: '적용' })) return
                onChange(M.fromPreset(k, doc)); setSel(null)
              }}>{M.PRESETS[k].name} <span className="text-muted-foreground">· {k}</span></Button>
            ))}
          </div>
        </aside>

        {/* 캔버스 */}
        <div ref={wrapRef} className="relative min-h-0 overflow-auto bg-background" onPointerDown={e => { if (e.target === e.currentTarget || (e.target as HTMLElement).dataset.stage) setSel(null) }}>
          <div ref={stageRef} data-stage className="relative" style={{ width: stageW, height: stageH, backgroundImage: 'radial-gradient(var(--border) 1px, transparent 1px)', backgroundSize: '20px 20px' }}>
            <svg className="pointer-events-none absolute inset-0" width={stageW} height={stageH}>
              {edges.map((e, i) => <path key={i} d={e.d} fill="none" stroke={e.fail ? 'var(--destructive)' : EDGE_COLOR[e.cls]} strokeWidth={e.hi ? 2.4 : 1.4} strokeDasharray={e.cls === 'reg' || e.cls === 'rtp' || e.cls === 'db' ? '4 3' : undefined} opacity={e.dim ? 0.25 : 0.9} />)}
            </svg>
            {edges.filter(e => e.label).map((e, i) => (
              <span key={`l${i}`} className={`pointer-events-none absolute -translate-x-1/2 -translate-y-1/2 rounded-sm border bg-card px-1 text-[10px] ${e.fail ? 'border-destructive text-destructive' : 'border-border text-muted-foreground'}`} style={{ left: e.lx, top: e.ly, opacity: e.dim ? 0.35 : 1 }}>{e.label}</span>
            ))}
            {M.hosts(doc).map(([hid, h]) => {
              const r = L.regions[hid] ?? { x: 40, y: 40, w: 300, h: 120 }; const hc = HC(M.colorOf(doc, hid)); const hk = M.hostKind(doc, hid)
              const ws = doc.workers.filter(w => w.host === hid), ns = M.nodes(doc).filter(([, n]) => n.host === hid)
              return (
                <div key={hid} data-region={hid} onPointerDown={startRegion(hid)}
                     className={`absolute rounded-md border-2 ${isSel('host', hid) ? 'ring-2 ring-primary' : ''} ${isHot('host', hid) ? 'ring-2 ring-success' : ''}`}
                     style={{ left: r.x, top: r.y, width: r.w, height: r.h, borderColor: hc, background: `color-mix(in srgb, ${hc} 6%, var(--card))` }}>
                  <div className="flex h-[30px] cursor-move items-center gap-1.5 px-2 text-xs">
                    <span className="inline-block h-2.5 w-2.5 rounded-full" style={{ background: hc }} /><Server size={12} className="text-muted-foreground" /><b>{h.name ?? hid}</b>
                    <Badge variant="neutralSoft" className="h-4 px-1 text-[10px]">{M.HOST_LABEL[hk]}</Badge>{errBadge({ kind: 'host', id: hid })}
                    {h.ssh && <span data-port={`${hid}:ssh`} className="inline-flex items-center gap-1 text-[10px] text-muted-foreground"><span data-pa /> ssh {h.ssh.user}@ {resBadge(`${hid}:ssh`)}</span>}
                    <span className="ml-auto font-mono text-[11px] text-muted-foreground">{h.ip}</span>
                  </div>
                  <div data-body className="relative" style={{ height: r.h - 30 }}>
                    {ns.map(([nid, n]) => dragCard?.kind === 'node' && dragCard.id === nid ? null : <NodeCard key={nid} id={nid} n={n} />)}
                    {ws.map(w => dragCard?.kind === 'worker' && dragCard.id === w.name ? null : <WorkerCard key={w.name} name={w.name} />)}
                    {!ns.length && !ws.length && <div className="absolute inset-2 flex items-center justify-center rounded-sm border border-dashed border-border text-[11px] text-muted-foreground">워커나 대상 노드를 여기에 놓으십시오</div>}
                  </div>
                  <span data-resize className="absolute bottom-0 right-0 h-3 w-3 cursor-nwse-resize rounded-tl-sm" style={{ background: hc }} title="크기 조절" />
                </div>
              )
            })}
            {orphanN.map(([nid, n]) => dragCard?.id === nid ? null : <NodeCard key={nid} id={nid} n={n} abs={L.items[nid] ?? { x: 30, y: 820 }} />)}
            {orphanW.map(w => dragCard?.id === w.name ? null : <WorkerCard key={w.name} name={w.name} abs={L.items[w.name] ?? { x: 360, y: 820 }} />)}
            {orphanP.length > 0 && (
              <div className="absolute left-[30px] top-[860px] w-[310px] rounded-md border border-dashed border-destructive bg-card p-1.5 text-xs">
                <div className="mb-1 flex items-center gap-1.5"><Badge variant="dangerSoft">워커 없는 풀</Badge><span className="text-muted-foreground">워커 위로 끌어 놓으십시오</span></div>
                <div className="flex flex-col gap-1">{orphanP.map(([pn, p]) => <PoolCard key={pn} pn={pn} p={p} />)}</div>
              </div>
            )}
            {dragCard && (dragCard.kind === 'node' ? <NodeCard id={dragCard.id} n={doc.target.nodes[dragCard.id]} abs={{ x: dragCard.x, y: dragCard.y }} /> : <WorkerCard name={dragCard.id} abs={{ x: dragCard.x, y: dragCard.y }} />)}
          </div>
          {drag && (drag.type === 'new' || drag.type === 'pool') && (
            <div className="pointer-events-none fixed z-[200] rounded-sm border border-primary bg-card px-2 py-1 text-xs shadow-md" style={{ left: drag.x + 8, top: drag.y + 8 }}>
              {drag.type === 'new' ? PALETTE.flatMap(g => g.items).find(i => i.kind === drag.kind)?.label : drag.id}
            </div>
          )}
        </div>

        {/* 속성 */}
        <aside className="min-h-0 overflow-auto border-l border-border bg-card p-3 text-xs">
          <Inspector doc={doc} sel={sel} setSel={setSel} mutate={mutate} issues={sel ? errsFor(sel) : []} canWrite={canWrite} onDelete={del} workers={workers} />
        </aside>
      </div>

      {/* 드로어 */}
      <div className="border-t border-border bg-card">
        <div className="flex items-center gap-1 px-3 py-1 text-xs">
          {(['issues', 'json', 'check'] as const).map(t => (
            <button key={t} onClick={() => { setTab(t); setDrawerOpen(true) }} className={`h-6 rounded-sm px-2 ${tab === t && drawerOpen ? 'bg-primary text-primary-foreground' : 'text-muted-foreground hover:bg-accent'}`}>
              {t === 'issues' ? <>검증 <Badge variant={errN ? 'dangerSoft' : warnN ? 'warningSoft' : 'successSoft'} className="ml-1 h-4 px-1 text-[10px]">{issues.length}</Badge></> : t === 'json' ? '레코드 JSON' : <>연결 검사 <Badge variant={check ? (check.items.every(i => i.ok || i.info) ? 'successSoft' : 'dangerSoft') : 'neutralSoft'} className="ml-1 h-4 px-1 text-[10px]">{check ? `${check.items.filter(i => i.ok).length}/${check.items.length}` : '—'}</Badge></>}
            </button>
          ))}
          <button className="ml-auto text-muted-foreground" onClick={() => setDrawerOpen(o => !o)}>{drawerOpen ? <ChevronDown size={14} /> : <ChevronRight size={14} />}</button>
        </div>
        {drawerOpen && (
          <div className="max-h-[220px] overflow-auto border-t border-border px-3 py-2 text-xs">
            {tab === 'issues' && (issues.length === 0 ? <div className="text-success">스키마·구조 검증 통과 — 저장할 수 있습니다</div> : (
              <div className="flex flex-col gap-1">{issues.map((i, k) => (
                <button key={k} onClick={() => focusIssue(i.focus)} className={`flex items-center gap-2 rounded-sm border-l-2 px-2 py-1 text-left hover:bg-accent ${i.level === 'err' ? 'border-destructive' : i.level === 'warn' ? 'border-warning' : 'border-info'}`}>
                  <Badge variant={i.level === 'err' ? 'dangerSoft' : i.level === 'warn' ? 'warningSoft' : 'infoSoft'}>{i.level === 'err' ? '오류' : i.level === 'warn' ? '경고' : '참고'}</Badge><span className="font-mono text-muted-foreground">{i.who}</span><span>{i.msg}</span>
                </button>))}</div>
            ))}
            {tab === 'json' && <JsonTab doc={doc} onApply={d => { onChange(d); setSel(null) }} canWrite={canWrite} />}
            {tab === 'check' && (check ? (
              <DataTable>
                <thead><tr><Th>항목</Th><Th width={70}>결과</Th><Th>상세</Th><Th align="right">ms</Th></tr></thead>
                <tbody>{check.items.map(i => (
                  <tr key={i.name} className="cursor-pointer hover:bg-accent" onClick={() => i.target?.id && setSel({ kind: (i.target.kind as Focus['kind']) || 'node', id: i.target.id })}>
                    <Td mono>{i.name}</Td><Td>{i.ok ? <Badge variant="successSoft">OK</Badge> : i.info ? <Badge variant="neutralSoft">참고</Badge> : <Badge variant="dangerSoft">실패</Badge>}</Td><Td className="break-all text-xs">{i.detail}</Td><Td align="right" mono>{i.ms}</Td>
                  </tr>))}</tbody>
              </DataTable>
            ) : <div className="text-muted-foreground">[연결 검사] 는 저장된 레코드에 대해 수신점 단위로 확인합니다 — SIP 접속점 OPTIONS/TCP/TLS · 피어링(참고) · 가입자 API · OAM 토큰 · DB 접속 · 호스트 SSH · 워커 health. 결과는 카드의 같은 행에 붙습니다.{onFocusCheck && <Button variant="outline" size="sm" className="ml-2" onClick={onFocusCheck}>검사 실행</Button>}</div>)}
          </div>
        )}
      </div>
    </div>
  )
}

// ── JSON 탭 ────────────────────────────────────────────────────────────────
function JsonTab({ doc, onApply, canWrite }: { doc: TopologyDoc; onApply: (d: TopologyDoc) => void; canWrite: boolean }) {
  const text = useMemo(() => JSON.stringify(doc, null, 2), [doc])
  const [edit, setEdit] = useState<string | null>(null)
  const [err, setErr] = useState<string | null>(null)
  const apply = () => { try { const d = JSON.parse(edit ?? text) as TopologyDoc; onApply(d); setEdit(null); setErr(null) } catch (e) { setErr(String(e)) } }
  return (
    <div className="flex gap-2">
      <textarea value={edit ?? text} onChange={e => setEdit(e.target.value)} readOnly={!canWrite} spellCheck={false}
                className="h-[180px] min-w-0 flex-1 resize-none rounded-sm border border-border bg-muted p-2 font-mono text-[11px]" />
      <div className="flex w-[160px] flex-col gap-1 text-[11px] text-muted-foreground">
        <b>레코드 = 이 JSON</b><span>캔버스 편집이 곧 이 문서다. 여기서 고치고 [적용]하면 캔버스가 다시 그려진다. 저장은 툴바.</span>
        <Button variant="outline" size="sm" disabled={!canWrite || edit == null} onClick={apply}>적용</Button>
        <Button variant="ghost" size="sm" disabled={edit == null} onClick={() => { setEdit(null); setErr(null) }}>취소</Button>
        {err && <span className="text-destructive">{err}</span>}
      </div>
    </div>
  )
}

// ── 속성 패널 ─────────────────────────────────────────────────────────────
function F({ label, children, help }: { label: string; children: ReactNode; help?: string }) {
  return <label className="flex flex-col gap-0.5"><span className="text-[11px] text-muted-foreground">{label}</span>{children}{help && <span className="text-[10px] text-muted-foreground">{help}</span>}</label>
}
function Txt({ value, onCommit, mono, type, placeholder, disabled }: { value: string | number | undefined | null; onCommit: (v: string) => void; mono?: boolean; type?: string; placeholder?: string; disabled?: boolean }) {
  const [v, setV] = useState(value == null ? '' : String(value))
  useEffect(() => { setV(value == null ? '' : String(value)) }, [value])
  return <Input value={v} type={type} placeholder={placeholder} disabled={disabled} onChange={e => setV(e.target.value)} onBlur={() => { if (v !== (value == null ? '' : String(value))) onCommit(v) }} onKeyDown={e => { if (e.key === 'Enter') (e.target as HTMLInputElement).blur() }} className={`h-[26px] text-xs ${mono ? 'font-mono' : ''}`} />
}
function Sel({ value, options, onChange, empty, disabled }: { value: string | undefined | null; options: { v: string; l?: string }[]; onChange: (v: string) => void; empty?: string; disabled?: boolean }) {
  const NONE = '__none__'
  return (
    <Select value={value || NONE} onValueChange={v => onChange(v === NONE ? '' : v)} disabled={disabled}>
      <SelectTrigger className="h-[26px] font-mono text-xs"><SelectValue /></SelectTrigger>
      <SelectContent>{empty !== undefined && <SelectItem value={NONE}>{empty}</SelectItem>}{options.map(o => <SelectItem key={o.v} value={o.v}>{o.l ?? o.v}</SelectItem>)}</SelectContent>
    </Select>
  )
}
function Sec({ title, right, children }: { title: string; right?: ReactNode; children: ReactNode }) {
  return <div className="mt-3 flex flex-col gap-1.5 border-t border-border pt-2"><div className="flex items-center gap-2 text-[11px] font-semibold text-muted-foreground">{title}<span className="ml-auto">{right}</span></div>{children}</div>
}
const num = (v: string) => (v === '' ? undefined : Number(v))
const csv = (v: string) => v.split(',').map(s => s.trim()).filter(Boolean)

function Inspector({ doc, sel, setSel, mutate, issues, canWrite, onDelete, workers }: {
  doc: TopologyDoc; sel: Focus | null; setSel: (f: Focus | null) => void; mutate: (fn: (d: TopologyDoc) => void) => void; issues: Issue[]; canWrite: boolean; onDelete: (f: Focus) => void; workers: WorkerRow[]
}) {
  const ro = !canWrite
  const issueBlock = issues.length ? <div className="mb-2 flex flex-col gap-1">{issues.map((i, k) => <div key={k} className={`rounded-sm border-l-2 px-2 py-1 ${i.level === 'err' ? 'border-destructive bg-dangersoft/40' : i.level === 'warn' ? 'border-warning bg-warning-soft/40' : 'border-info bg-info-soft/40'}`}>{i.msg}</div>)}</div> : null
  const head = (title: string, badge: ReactNode) => <div className="mb-2 flex items-center gap-2"><b className="text-sm">{title}</b>{badge}</div>
  const delBtn = sel && <div className="mt-4 border-t border-border pt-2"><Button variant="destructive" size="sm" disabled={ro} onClick={() => onDelete(sel)}><Trash2 size={12} /> {sel.kind === 'host' ? '호스트' : sel.kind === 'worker' ? '워커' : sel.kind === 'node' ? '노드' : '풀'} 삭제</Button></div>

  if (!sel) return (
    <div>
      <b className="text-sm">속성</b>
      <div className="mt-2 leading-relaxed text-muted-foreground">캔버스에서 카드를 누르면 여기서 고칩니다.
        <ul className="mt-1 list-disc pl-4"><li><b>호스트</b> — 이름·주소·SSH 자격. 주소는 여기에만 있습니다</li><li><b>워커</b> — 제어 포트·cpus·health, 그 위의 풀</li><li><b>대상 노드</b> — 역할별 설정(수신점·RTP·API·OAM·DB) + 감시 프로세스</li><li><b>풀</b> — 접속점/다음 홉 노드·transport·원천·신원·시드</li></ul>
        선은 그리지 않습니다. 놓는 자리와 속성값에서 파생됩니다.</div>
      <Sec title="대상">
        <F label="대상 이름"><Txt value={doc.target.name} disabled={ro} onCommit={v => mutate(d => { d.target.name = v })} /></F>
        <F label="kind" help="cims 만 oam 노드로 컬렉션 시드·target_build"><Sel value={doc.target.kind ?? 'cims'} disabled={ro} options={[{ v: 'cims' }, { v: 'ims' }, { v: 'pbx' }]} onChange={v => mutate(d => { d.target.kind = v as 'cims' | 'ims' | 'pbx' })} /></F>
      </Sec>
    </div>
  )
  if (sel.kind === 'host') {
    const id = sel.id, h = doc.hosts[id]; if (!h) { setSel(null); return null }
    const ws = doc.workers.filter(w => w.host === id), ns = M.nodes(doc).filter(([, n]) => n.host === id)
    return <div>
      {head(h.name ?? id, <Badge variant="neutralSoft">{M.HOST_LABEL[M.hostKind(doc, id)]}</Badge>)}{issueBlock}
      <div className="grid grid-cols-2 gap-2">
        <F label="id"><Txt value={id} mono disabled={ro} onCommit={v => mutate(d => { if (M.renameHost(d, id, v)) setSel({ kind: 'host', id: v }) })} /></F>
        <F label="이름"><Txt value={h.name} disabled={ro} onCommit={v => mutate(d => { d.hosts[id].name = v || undefined })} /></F>
      </div>
      <F label="주소 (관리·서비스)" help="워커 URL · 노드 수신점 · 피어 수신점 ip 가 전부 여기서 파생"><Txt value={h.ip} mono disabled={ro} onCommit={v => mutate(d => { d.hosts[id].ip = v })} /></F>
      <Sec title="SSH 관측 — 이 서버의 프로세스 CPU/메모리" right={<label className="inline-flex items-center gap-1"><Checkbox checked={!!h.ssh} disabled={ro} onCheckedChange={v => mutate(d => { if (v) d.hosts[id].ssh = { user: 'cims', key_env: 'TESTER_SSH_KEY' }; else delete d.hosts[id].ssh })} /> 있음</label>}>
        {h.ssh ? <div className="grid grid-cols-2 gap-2"><F label="사용자"><Txt value={h.ssh.user} mono disabled={ro} onCommit={v => mutate(d => { d.hosts[id].ssh!.user = v })} /></F><F label="키 환경변수" help="값이 아니라 이름만"><Txt value={h.ssh.key_env} mono disabled={ro} onCommit={v => mutate(d => { d.hosts[id].ssh!.key_env = v })} /></F></div>
          : <span className="text-muted-foreground">팔레트의 [SSH 관측]을 카드 위에 놓아도 됩니다. 감시 프로세스는 각 노드에: {ns.flatMap(([, n]) => n.procs ?? []).join(', ') || '(없음)'}</span>}
      </Sec>
      <Sec title="이 서버 위">{ws.map(w => <div key={w.name} className="flex justify-between"><span>{w.name} (워커)</span><span className="font-mono text-muted-foreground">:{w.port ?? 7100} · 풀 {Object.values(doc.pools).filter(p => p.worker === w.name).length}</span></div>)}{ns.map(([nid, n]) => <div key={nid} className="flex justify-between"><span>{n.label ?? nid} ({M.ROLE[n.role].label})</span><span className="text-muted-foreground">{n.fn}</span></div>)}{!ws.length && !ns.length && <span className="text-muted-foreground">비어 있음</span>}</Sec>
      {delBtn}
    </div>
  }
  if (sel.kind === 'worker') {
    const w = M.worker(doc, sel.id); if (!w) { setSel(null); return null }
    const ip = M.ipOfWorker(doc, w.name); const h = workers.find(x => x.name === w.name)?.health ?? null
    const pools = Object.entries(doc.pools).filter(([, p]) => p.worker === w.name)
    return <div>
      {head(w.name, <Badge variant="neutralSoft">워커</Badge>)}{issueBlock}
      <div className="grid grid-cols-2 gap-2">
        <F label="이름"><Txt value={w.name} mono disabled={ro} onCommit={v => mutate(d => { if (M.renameWorker(d, w.name, v)) setSel({ kind: 'worker', id: v }) })} /></F>
        <F label="호스트"><Sel value={w.host} disabled={ro} options={M.hosts(doc).map(([id, hh]) => ({ v: id, l: `${id} (${hh.ip})` }))} onChange={v => mutate(d => { M.worker(d, w.name)!.host = v })} /></F>
        <F label="제어 포트"><Txt value={w.port ?? 7100} mono type="number" disabled={ro} onCommit={v => mutate(d => { M.worker(d, w.name)!.port = num(v) ?? 7100 })} /></F>
        <F label="cpus"><Txt value={w.cpus} mono type="number" disabled={ro} onCommit={v => mutate(d => { M.worker(d, w.name)!.cpus = num(v) })} /></F>
      </div>
      <div className="mt-1 text-muted-foreground">URL http://{ip || '?'}:{w.port ?? 7100} (파생)</div>
      <Sec title="상태 (GET /workers)">{h ? <><div className="flex justify-between"><span>버전</span><span className="font-mono">v{h.version}</span></div><div className="flex justify-between"><span>단말 / 최대</span><span className="font-mono">{h.active_endpoints} / {h.max_endpoints}</span></div><div className="flex justify-between"><span>최대 SApS</span><span className="font-mono">{h.max_saps}</span></div><div className="flex justify-between"><span>CPU</span><span className="font-mono">{h.cpu_pct} %</span></div>{h.media && <div className="flex justify-between"><span>RTP</span><span className="font-mono">{h.media.rtp_streams ?? 0} / {h.media.max_rtp_streams ?? '?'}</span></div>}</> : <span className="text-muted-foreground">저장 뒤 health 조회 — 응답 없으면 워커 프로세스·주소를 확인</span>}</Sec>
      <Sec title="미디어 (샘플 라이브러리 §7 ⓖ)">
        <F label="보유 샘플 (쉼표)"><Txt value={(w.media?.samples ?? []).join(',')} mono disabled={ro} onCommit={v => mutate(d => { const ww = M.worker(d, w.name)!; ww.media = { ...(ww.media ?? {}), samples: csv(v) }; if (!ww.media.samples?.length && ww.media.max_rtp_streams == null) delete ww.media })} /></F>
        <F label="max_rtp_streams"><Txt value={w.media?.max_rtp_streams} mono type="number" disabled={ro} onCommit={v => mutate(d => { const ww = M.worker(d, w.name)!; ww.media = { ...(ww.media ?? {}), max_rtp_streams: num(v) } })} /></F>
      </Sec>
      <Sec title={`이 워커의 풀 (${pools.length})`}>{pools.map(([pn, p]) => <div key={pn} className="flex justify-between"><span>{pn} ({M.isPeer(p) ? p.profile : p.kind})</span><span className="font-mono text-muted-foreground">{M.isPeer(p) ? `${ip}:${p.bind.port}/${p.bind.protocol ?? 'udp'}` : `${M.poolSize(p) || '전체'} ep → ${p.access}${p.group ? ` · ≡ ${p.group}` : ''}`}</span></div>)}{!pools.length && <span className="text-muted-foreground">풀을 이 카드 위에 놓으십시오</span>}</Sec>
      {delBtn}
    </div>
  }
  if (sel.kind === 'node') {
    const id = sel.id, n = doc.target.nodes[id]; if (!n) { setSel(null); return null }
    const N = (fn: (x: TopoNode, d: TopologyDoc) => void) => mutate(d => fn(d.target.nodes[id], d))
    const a = n.sip?.access, pg = n.sip?.peering
    return <div>
      {head(n.label ?? id, <Badge variant="neutralSoft">{M.ROLE[n.role].label}</Badge>)}{issueBlock}
      <div className="grid grid-cols-2 gap-2">
        <F label="노드 id" help="풀이 참조하는 키"><Txt value={id} mono disabled={ro} onCommit={v => mutate(d => { if (M.renameNode(d, id, v)) setSel({ kind: 'node', id: v }) })} /></F>
        <F label="표시 이름"><Txt value={n.label} disabled={ro} onCommit={v => N(x => { x.label = v || undefined })} /></F>
        <F label="기능 (fn)"><Sel value={n.fn} disabled={ro} options={[...new Set([...(n.fn ? [n.fn] : []), ...M.ROLE[n.role].fns])].map(v => ({ v }))} onChange={v => N(x => { x.fn = v })} /></F>
        <F label="호스트"><Sel value={n.host} disabled={ro} options={M.hosts(doc).map(([hid, hh]) => ({ v: hid, l: `${hid} (${hh.ip})` }))} onChange={v => N(x => { x.host = v })} /></F>
      </div>
      <div className="mt-1 text-muted-foreground">주소 {M.ipOfNode(doc, id) || '?'} (호스트에서 파생)</div>
      {n.role === 'sip' && <>
        <Sec title="접속 수신점 — UE 풀이 등록·발신하는 곳" right={<label className="inline-flex items-center gap-1"><Checkbox checked={!!a} disabled={ro} onCheckedChange={v => N(x => { x.sip = x.sip ?? {}; if (v) x.sip.access = { udp: 5060, tcp: 5060, tls: 5061, domains: [] }; else delete x.sip.access })} /> 켬</label>}>
          {a ? <><div className="grid grid-cols-3 gap-2">{(['udp', 'tcp', 'tls'] as const).map(t => <F key={t} label={t.toUpperCase()}><Txt value={a[t]} mono type="number" placeholder="없음" disabled={ro} onCommit={v => N(x => { x.sip!.access![t] = num(v) })} /></F>)}</div>
            <F label="도메인 (쉼표)" help="첫 항목 = 기본 홈 도메인, ptt 가 든 항목 = PTT"><Txt value={(a.domains ?? []).join(',')} mono disabled={ro} onCommit={v => N(x => { x.sip!.access!.domains = csv(v) })} /></F></>
            : <span className="text-muted-foreground">P-CSCF · SBC · CSP 에 켭니다</span>}
        </Sec>
        <Sec title="피어링 수신점 — 피어 풀의 다음 홉" right={<label className="inline-flex items-center gap-1"><Checkbox checked={!!pg} disabled={ro} onCheckedChange={v => N((x, d) => { x.sip = x.sip ?? {}; if (v) x.sip.peering = { port: (d.target.kind ?? 'cims') === 'cims' ? 5070 : 5060, protocol: 'udp', ...((d.target.kind ?? 'cims') === 'cims' ? { local_node: 'cims-tester-peering' } : {}) }; else delete x.sip.peering })} /> 켬</label>}>
          {pg ? <><div className="grid grid-cols-2 gap-2"><F label="포트"><Txt value={pg.port} mono type="number" disabled={ro} onCommit={v => N(x => { x.sip!.peering!.port = num(v) ?? 5070 })} /></F><F label="프로토콜"><Sel value={pg.protocol ?? 'udp'} disabled={ro} options={[{ v: 'udp' }, { v: 'tcp' }, { v: 'tls' }]} onChange={v => N(x => { x.sip!.peering!.protocol = v as Transport })} /></F></div>
            {(doc.target.kind ?? 'cims') === 'cims' ? <F label="local_node (대상 local_nodes 이름 · ACL scope)"><Txt value={pg.local_node} mono disabled={ro} onCommit={v => N(x => { x.sip!.peering!.local_node = v || undefined })} /></F> : <span className="text-muted-foreground">타 IMS — 컬렉션 시드 없음, 대상 쪽 라우팅을 미리 잡아 둡니다</span>}</>
            : <span className="text-muted-foreground">IBCF · I-CSCF · CSP 피어링 리스너에 켭니다</span>}
        </Sec>
      </>}
      {n.role === 'media' && <Sec title="RTP — 미디어 leg 지표를 이 노드에 귀속"><div className="grid grid-cols-2 gap-2"><F label="범위 시작"><Txt value={n.media?.rtp_range?.[0]} mono type="number" disabled={ro} onCommit={v => N(x => { x.media = x.media ?? {}; x.media.rtp_range = [num(v) ?? 10000, x.media.rtp_range?.[1] ?? 19999] })} /></F><F label="끝"><Txt value={n.media?.rtp_range?.[1]} mono type="number" disabled={ro} onCommit={v => N(x => { x.media = x.media ?? {}; x.media.rtp_range = [x.media.rtp_range?.[0] ?? 10000, num(v) ?? 19999] })} /></F></div><F label="제어 포트 (선택)"><Txt value={n.media?.control} mono type="number" placeholder="CMP 9001" disabled={ro} onCommit={v => N(x => { x.media = x.media ?? {}; x.media.control = num(v) })} /></F></Sec>}
      {n.role === 'subscriber' && <Sec title="프로비저닝 API" right={<label className="inline-flex items-center gap-1"><Checkbox checked={!!n.api} disabled={ro} onCheckedChange={v => N(x => { if (v) x.api = { port: 4430, tls: true }; else delete x.api })} /> 있음</label>}>{n.api ? <div className="grid grid-cols-2 gap-2"><F label="포트"><Txt value={n.api.port} mono type="number" disabled={ro} onCommit={v => N(x => { x.api!.port = num(v) ?? 4430 })} /></F><label className="mt-4 inline-flex items-center gap-1"><Checkbox checked={n.api.tls !== false} disabled={ro} onCheckedChange={v => N(x => { x.api!.tls = v === true })} /> TLS</label></div> : <span className="text-muted-foreground">HSS 처럼 API 가 없으면 관측만</span>}</Sec>}
      {n.role === 'oam' && <Sec title="OAM — 통계·알람 + 컬렉션 시드">
        <div className="grid grid-cols-2 gap-2"><F label="포트"><Txt value={n.oam?.port ?? 4419} mono type="number" disabled={ro} onCommit={v => N(x => { x.oam = { ...(x.oam ?? {}), port: num(v) ?? 4419 } })} /></F><F label="CSP 배포 id" help="비면 배포 목록에서 csp 자동"><Txt value={n.oam?.csp_deployment_id} mono type="number" disabled={ro} onCommit={v => N(x => { x.oam = { ...(x.oam ?? {}), csp_deployment_id: num(v) } })} /></F></div>
        <F label="토큰 환경변수" help="값이 아니라 이름만 (기본 TESTER_OAM_TOKEN)"><Txt value={n.oam?.token_env} mono disabled={ro} onCommit={v => N(x => { x.oam = { ...(x.oam ?? {}), token_env: v || undefined } })} /></F>
        <label className="inline-flex items-center gap-1"><Checkbox checked={n.oam?.tls !== false} disabled={ro} onCheckedChange={v => N(x => { x.oam = { ...(x.oam ?? {}), tls: v === true } })} /> HTTPS</label>
        <div className="flex flex-wrap gap-1">{['oam_stats', 'oam_alarms', 'agent_heartbeat'].map(o => { const on = (n.oam?.observe ?? []).includes(o); return <button key={o} disabled={ro} onClick={() => N(x => { const s = new Set(x.oam?.observe ?? []); if (s.has(o)) s.delete(o); else s.add(o); x.oam = { ...(x.oam ?? {}), observe: [...s] } })} className={`h-5 rounded-sm border px-1.5 text-[10px] ${on ? 'border-primary bg-primary text-primary-foreground' : 'border-border'}`}>{o}</button> })}</div>
      </Sec>}
      {n.role === 'db' && <Sec title="DB 접속 — H(A1) 보유 가입자 원천"><div className="grid grid-cols-2 gap-2"><F label="포트"><Txt value={n.db?.port ?? 3306} mono type="number" disabled={ro} onCommit={v => N(x => { x.db = { ...(x.db ?? {}), port: num(v) ?? 3306 } })} /></F><F label="DB 이름"><Txt value={n.db?.name ?? 'cims'} mono disabled={ro} onCommit={v => N(x => { x.db = { ...(x.db ?? {}), name: v } })} /></F><F label="사용자 환경변수"><Txt value={n.db?.user_env} mono disabled={ro} onCommit={v => N(x => { x.db = { ...(x.db ?? {}), user_env: v || undefined } })} /></F><F label="비밀번호 환경변수"><Txt value={n.db?.password_env} mono disabled={ro} onCommit={v => N(x => { x.db = { ...(x.db ?? {}), password_env: v || undefined } })} /></F></div></Sec>}
      {n.role === 'tas' && <Sec title="TAS — 코어 내부"><F label="SIP 포트 (참고)"><Txt value={n.tas?.port} mono type="number" disabled={ro} onCommit={v => N(x => { x.tas = { port: num(v) } })} /></F></Sec>}
      <Sec title="감시 프로세스 — 호스트 SSH 관측으로 CPU/메모리"><F label="프로세스 이름 (쉼표)"><Txt value={(n.procs ?? []).join(',')} mono placeholder="csp / pcscf" disabled={ro} onCommit={v => N(x => { x.procs = csv(v) })} /></F>{!doc.hosts[n.host]?.ssh && <span className="text-muted-foreground">호스트에 SSH 관측이 없습니다 — 호스트 카드에 [SSH 관측]을 올리십시오</span>}</Sec>
      {delBtn}
    </div>
  }
  // pool
  const pn = sel.id, p = doc.pools[pn]; if (!p) { setSel(null); return null }
  const P = (fn: (x: PoolDoc, d: TopologyDoc) => void) => mutate(d => fn(d.pools[pn], d))
  const wsel = <F label="워커" help="풀 하나 = 워커 하나. 카드를 다른 워커로 끌어도 됩니다"><Sel value={p.worker} disabled={ro} options={doc.workers.map(w => ({ v: w.name, l: `${w.name} @ ${M.ipOfWorker(doc, w.name)}` }))} empty="(선택)" onChange={v => P(x => { x.worker = v })} /></F>
  const gsel = <F label="group (논리 풀 이름)" help="워커마다 풀을 두고 같은 group 을 주면 시나리오는 pool: <group> 하나로 전 워커에서 돈다"><Txt value={p.group} mono placeholder="비우면 풀 이름으로만 참조" disabled={ro} onCommit={v => P(x => { if (v) x.group = v; else delete x.group })} /></F>
  if (M.isPeer(p)) {
    const idn = p.identities ?? {}; const seed = p.seed ?? {}; const cims = (doc.target.kind ?? 'cims') === 'cims'
    const PP = (fn: (x: PeerPoolDoc) => void) => P(x => fn(x as PeerPoolDoc))
    return <div>
      {head(pn, <Badge variant="warningSoft">{p.profile} 피어</Badge>)}{issueBlock}
      <F label="풀 이름" help="시나리오 roles.pool 이 참조"><Txt value={pn} mono disabled={ro} onCommit={v => mutate(d => { if (M.renamePool(d, pn, v)) setSel({ kind: 'pool', id: v }) })} /></F>
      <div className="grid grid-cols-2 gap-2">
        <F label="프로파일"><Sel value={p.profile} disabled={ro} options={[{ v: 'ibcf' }, { v: 'pbx' }, { v: 'mgcf' }]} onChange={v => PP(x => { x.profile = v as PeerPoolDoc['profile']; if (v !== 'pbx') { delete x.register; if (x.identities?.did_range) { x.identities = { e164_range: ['+82200000000', '+82200000099'] } } } else if (x.identities?.e164_range) { x.identities = { did_range: ['0212345000', '0212345099'], ext_len: 4 } } })} /></F>
        <F label="응답"><Sel value={p.answer ?? 'normal'} disabled={ro} options={[{ v: 'normal' }, { v: 'silent', l: 'silent (무응답 — failover 상대)' }]} onChange={v => PP(x => { x.answer = v as 'normal' | 'silent' })} /></F>
        {wsel}{gsel}
      </div>
      <F label="도메인"><Txt value={p.domain} mono disabled={ro} onCommit={v => PP(x => { x.domain = v })} /></F>
      <Sec title="수신점 (bind) — 워커 호스트 주소에 엽니다"><div className="grid grid-cols-2 gap-2"><F label="port"><Txt value={p.bind.port} mono type="number" disabled={ro} onCommit={v => PP(x => { x.bind.port = num(v) ?? 5080 })} /></F><F label="proto"><Sel value={p.bind.protocol ?? 'udp'} disabled={ro} options={[{ v: 'udp' }, { v: 'tcp' }, { v: 'tls' }]} onChange={v => PP(x => { x.bind.protocol = v as Transport })} /></F></div><span className="text-muted-foreground">= {M.ipOfWorker(doc, p.worker) || '?'}:{p.bind.port}/{p.bind.protocol ?? 'udp'}</span></Sec>
      <Sec title="다음 홉 — 피어링 수신점 있는 대상 노드"><Sel value={p.peering} disabled={ro} options={M.peeringNodes(doc).map(v => ({ v }))} empty="(선택)" onChange={v => PP(x => { x.peering = v })} /></Sec>
      <Sec title="신원">
        {p.profile === 'pbx' ? <><div className="grid grid-cols-2 gap-2"><F label="DID 시작"><Txt value={idn.did_range?.[0]} mono disabled={ro} onCommit={v => PP(x => { x.identities = { ...x.identities, did_range: [v, x.identities?.did_range?.[1] ?? v] }; delete x.identities.e164_range })} /></F><F label="DID 끝"><Txt value={idn.did_range?.[1]} mono disabled={ro} onCommit={v => PP(x => { x.identities = { ...x.identities, did_range: [x.identities?.did_range?.[0] ?? v, v] }; delete x.identities.e164_range })} /></F></div><F label="내선 길이"><Txt value={idn.ext_len} mono type="number" disabled={ro} onCommit={v => PP(x => { x.identities = { ...x.identities, ext_len: num(v) } })} /></F></>
          : <div className="grid grid-cols-2 gap-2"><F label="E.164 시작"><Txt value={idn.e164_range?.[0]} mono disabled={ro} onCommit={v => PP(x => { x.identities = { ...x.identities, e164_range: [v, x.identities?.e164_range?.[1] ?? v] }; delete x.identities.did_range })} /></F><F label="E.164 끝"><Txt value={idn.e164_range?.[1]} mono disabled={ro} onCommit={v => PP(x => { x.identities = { ...x.identities, e164_range: [x.identities?.e164_range?.[0] ?? v, v] }; delete x.identities.did_range })} /></F></div>}
        <F label="count (동시 신원)"><Txt value={idn.count} mono type="number" placeholder="범위 전체" disabled={ro} onCommit={v => PP(x => { x.identities = { ...x.identities, count: num(v) } })} /></F>
      </Sec>
      <Sec title="코덱·시그널링"><F label="코덱 (쉼표, 우선순위)" help="비면 프로파일 기본 — ibcf/mgcf AMR-WB,AMR,PCMU,PCMA · pbx PCMA,PCMU"><Txt value={(p.codecs ?? []).join(',')} mono disabled={ro} onCommit={v => PP(x => { const c = csv(v); if (c.length) x.codecs = c; else delete x.codecs })} /></F>
        <div className="flex gap-3"><label className="inline-flex items-center gap-1"><Checkbox checked={p.prack === true} disabled={ro} onCheckedChange={v => PP(x => { if (v) x.prack = true; else delete x.prack })} /> 100rel/PRACK (비면 프로파일 기본)</label><label className="inline-flex items-center gap-1"><Checkbox checked={p.dtmf !== false} disabled={ro} onCheckedChange={v => PP(x => { x.dtmf = v === true })} /> RFC 4733 DTMF</label></div></Sec>
      {cims && <Sec title="CSP 컬렉션 시드" right={<label className="inline-flex items-center gap-1"><Checkbox checked={seed.enabled !== false} disabled={ro} onCheckedChange={v => PP(x => { x.seed = { ...(x.seed ?? {}), enabled: v === true } })} /> 시드</label>}>
        <div className="grid grid-cols-2 gap-2"><F label="route_set" help="비면 풀 이름"><Txt value={seed.route_set} mono disabled={ro} onCommit={v => PP(x => { x.seed = { ...(x.seed ?? {}), route_set: v || undefined } })} /></F><F label="priority"><Txt value={seed.priority ?? 100} mono type="number" disabled={ro} onCommit={v => PP(x => { x.seed = { ...(x.seed ?? {}), priority: num(v) ?? 100 } })} /></F>
          <F label="distribution"><Sel value={seed.distribution ?? 'failover'} disabled={ro} options={['failover', 'round_robin', 'weighted', 'hash_by_caller'].map(v => ({ v }))} onChange={v => PP(x => { x.seed = { ...(x.seed ?? {}), distribution: v } })} /></F><F label="ACL"><Sel value={seed.acl} disabled={ro} options={[{ v: 'deny' }, { v: 'allow' }]} empty="없음" onChange={v => PP(x => { x.seed = { ...(x.seed ?? {}) }; if (v) x.seed.acl = v as 'deny' | 'allow'; else delete x.seed.acl })} /></F></div></Sec>}
      <Sec title="트렁크 REGISTER (SIPconnect 등록 모드)" right={<label className="inline-flex items-center gap-1"><Checkbox checked={!!p.register} disabled={ro} onCheckedChange={v => PP(x => { if (v) x.register = { user: pn, ha1_env: `${pn.toUpperCase()}_HA1` }; else delete x.register })} /> 있음</label>}>
        {p.register && <div className="grid grid-cols-2 gap-2"><F label="user"><Txt value={p.register.user} mono disabled={ro} onCommit={v => PP(x => { x.register!.user = v })} /></F><F label="ha1 환경변수" help="값이 아니라 이름만"><Txt value={p.register.ha1_env} mono disabled={ro} onCommit={v => PP(x => { x.register!.ha1_env = v || undefined })} /></F><F label="password 환경변수"><Txt value={p.register.password_env} mono disabled={ro} onCommit={v => PP(x => { x.register!.password_env = v || undefined })} /></F><F label="realm" help="비면 접속점 기본 도메인"><Txt value={p.register.realm} mono disabled={ro} onCommit={v => PP(x => { x.register!.realm = v || undefined })} /></F></div>}
      </Sec>
      {delBtn}
    </div>
  }
  const u = p as UePoolDoc; const isDb = 'db' in u.source; const acc = doc.target.nodes[u.access]
  const PU = (fn: (x: UePoolDoc) => void) => P(x => fn(x as UePoolDoc))
  return <div>
    {head(pn, <Badge variant={p.kind === 'ue' ? 'infoSoft' : 'successSoft'}>{p.kind === 'ue' ? 'UE 풀' : '실단말 풀'}</Badge>)}{issueBlock}
    <F label="풀 이름" help="시나리오 roles.pool 이 참조 (이름 또는 group)"><Txt value={pn} mono disabled={ro} onCommit={v => mutate(d => { if (M.renamePool(d, pn, v)) setSel({ kind: 'pool', id: v }) })} /></F>
    <div className="grid grid-cols-2 gap-2">{wsel}{gsel}</div>
    <Sec title="접속점 — 등록·발신이 닿는 SIP 서버">
      <div className="grid grid-cols-3 gap-2">
        <F label="노드"><Sel value={u.access} disabled={ro} options={M.accessNodes(doc).map(v => ({ v }))} empty="(선택)" onChange={v => PU(x => { x.access = v; const a = doc.target.nodes[v]?.sip?.access; if (a && !a[x.transport ?? 'udp']) x.transport = (['udp', 'tcp', 'tls'] as Transport[]).find(t => a[t]) ?? x.transport })} /></F>
        <F label="transport"><Sel value={u.transport ?? 'udp'} disabled={ro} options={(['udp', 'tcp', 'tls'] as Transport[]).filter(t => !acc?.sip?.access || acc.sip.access[t] || t === u.transport).map(v => ({ v }))} onChange={v => PU(x => { x.transport = v as Transport })} /></F>
        <F label="srtp"><Sel value={u.srtp ?? 'off'} disabled={ro} options={[{ v: 'off' }, { v: 'optional' }, { v: 'required' }]} onChange={v => PU(x => { x.srtp = v as UePoolDoc['srtp'] })} /></F>
      </div>
      <span className="text-muted-foreground">{acc?.sip?.access ? `${acc.label ?? u.access} ${M.ipOfNode(doc, u.access)} — ${(['udp', 'tcp', 'tls'] as const).filter(t => acc.sip!.access![t]).map(t => `${t} ${acc.sip!.access![t]}`).join(' · ')}` : '카드를 SIP 서버 노드 위로 끌어 놓으십시오'}</span>
    </Sec>
    <Sec title="신원 원천" right={p.kind === 'ue' && <span className="inline-flex gap-1">{(['db', 'creds'] as const).map(k => <button key={k} disabled={ro} onClick={() => PU(x => { x.source = k === 'db' ? { db: M.dbNodes(doc)[0] ?? '', table: 'volte_subscriptions', offset: 0, count: 100 } : { creds: `creds/${pn}.jsonl` } })} className={`h-5 rounded-sm border px-1.5 text-[10px] ${(k === 'db') === isDb ? 'border-primary bg-primary text-primary-foreground' : 'border-border'}`}>{k === 'db' ? 'DB 노드' : 'creds'}</button>)}</span>}>
      {isDb && 'db' in u.source ? <>
        <F label="원천 노드"><Sel value={u.source.db} disabled={ro} options={M.dbNodes(doc).map(v => ({ v }))} empty="(선택)" onChange={v => PU(x => { if ('db' in x.source) x.source.db = v })} /></F>
        <F label="가입 테이블"><Sel value={u.source.table} disabled={ro} options={['volte_subscriptions', 'voip_subscriptions', 'ptt_subscriptions'].map(v => ({ v }))} onChange={v => PU(x => { if ('db' in x.source) x.source.table = v })} /></F>
        <div className="grid grid-cols-2 gap-2"><F label="offset"><Txt value={u.source.offset ?? 0} mono type="number" disabled={ro} onCommit={v => PU(x => { if ('db' in x.source) x.source.offset = num(v) ?? 0 })} /></F><F label="count"><Txt value={u.source.count} mono type="number" disabled={ro} onCommit={v => PU(x => { if ('db' in x.source) x.source.count = num(v) ?? 1 })} /></F></div>
        <span className="text-muted-foreground">DB 원천은 컨트롤러 후속(지금은 creds-from-db 로 JSONL 을 만들어 creds 로) · 워커 둘에 나누려면 풀 둘을 offset 으로 잘라 같은 group</span></>
        : <><F label="creds JSONL" help="scenarios/·DataDir 상대 또는 절대 경로 (cims-tester creds-from-db)"><Txt value={'creds' in u.source ? u.source.creds : ''} mono disabled={ro} onCommit={v => PU(x => { if ('creds' in x.source) x.source.creds = v })} /></F><F label="count"><Txt value={'creds' in u.source ? u.source.count : undefined} mono type="number" placeholder="파일 전체" disabled={ro} onCommit={v => PU(x => { if ('creds' in x.source) x.source.count = num(v) })} /></F></>}
    </Sec>
    {p.kind === 'ue' && <Sec title="등록·시그널링"><div className="grid grid-cols-2 gap-2"><F label="register_expires"><Txt value={u.register_expires ?? 3600} mono type="number" disabled={ro} onCommit={v => PU(x => { x.register_expires = num(v) ?? 3600 })} /></F><div className="mt-4 flex flex-col gap-1"><label className="inline-flex items-center gap-1"><Checkbox checked={!!u.prack} disabled={ro} onCheckedChange={v => PU(x => { x.prack = v === true })} /> 100rel/PRACK</label><label className="inline-flex items-center gap-1"><Checkbox checked={u.dtmf !== false} disabled={ro} onCheckedChange={v => PU(x => { x.dtmf = v === true })} /> RFC 4733 DTMF</label></div></div></Sec>}
    {delBtn}
  </div>
}
