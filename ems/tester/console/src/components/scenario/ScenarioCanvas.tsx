// 시나리오 캔버스 편집기 — 팔레트(역할·단계 축별, vocab 이 정본) · 시퀀스 캔버스(레인 = 역할, 행 = 단계, 구간 띠 자동, 세션 열, during 마커) ·
// 속성 패널(시나리오/역할/단계/during) · 하단 드로어(YAML 양방향 · 검증 · 토폴로지 적합성 = compile-check · 절차표) (test_instrument.md §7).
// 문서는 부모(페이지)가 소유하고 저장한다. 기준 토폴로지는 편집 문맥이지 시나리오 속성이 아니다(저장 안 함) — 그래서 검증 배지는
// 문서 오류(저장 게이트)와 토폴로지 적합 오류(문맥)를 따로 센다. YAML 드로어는 YamlEditor(타이핑 중 컨트롤러 검증) + [적용]. 드로어 높이는 손잡이로.
// 키: ↑↓ 행 이동 · Alt+↑↓ 순서 바꾸기 · Del · Ctrl+D 복제 · Esc.
// during ↔ 행: in-dialog 행을 통화 유지 바 위에 놓으면 during 이 되고, during 마커를 바 밖(행 사이)으로 끌어 놓으면 행이 된다 — 속성 패널 버튼도 같은 일.
import { useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from 'react'
import { ChevronDown, ChevronRight, Trash2, Copy, Plus, X } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Input } from '@core/components/ui/input'
import { Checkbox } from '@core/components/ui/checkbox'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { DataTable, Th, Td } from '@core/components/custom/data-table'
import { useToast } from '@core/components/Toast'
import { useConfirm } from '@core/components/custom/confirm'
import { testerApi, type TopologyDoc, type ScenarioVocab, type PlanResult, type ProfileRow } from '@tester/api/tester'
import PlanPreview from '@tester/components/PlanPreview'
import YamlEditor from '@tester/components/YamlEditor'
import { useDrawerHeight } from '@tester/lib/use-drawer-height'
import * as S from '@tester/lib/scenario-model'
import type { Doc, Step, Sel, During } from '@tester/lib/scenario-model'
import { fmtNum } from '@tester/lib/fmt'

const NONE = '__none__'
const GROUP_COLOR: Record<string, string> = { reg: 'var(--chart-9)', call: 'var(--chart-1)', media: 'var(--chart-11)', peer: 'var(--chart-2)', xfer: 'var(--chart-3)', ptt: 'var(--chart-5)', ctl: 'var(--chart-6)' }
const PH: Record<string, [string, string]> = { prelude: ['PRELUDE', 'run 시작 때 역할 단말 전부 한 번 등록'], body: ['BODY', '시나리오 인스턴스 하나가 실행하는 단위 — SApS 로 발생'], epilogue: ['EPILOGUE', 'run 종료 시'] }

type Drag = { type: 'newstep'; step: string } | { type: 'newrole'; role: 'ue' | 'peer' } | { type: 'move'; idx: number } | { type: 'lane'; id: string }

export default function ScenarioCanvas({ doc, onChange, onCommit, topologies, topoId, setTopoId, profiles, profileId, setProfileId, vocab, canWrite, source }: {
  doc: Doc; onChange: (d: Doc, transient?: boolean) => void; onCommit?: () => void
  topologies: { id: number; name: string; doc: TopologyDoc }[]; topoId: number | null; setTopoId: (id: number | null) => void
  profiles: ProfileRow[]; profileId: string; setProfileId: (p: string) => void
  vocab: ScenarioVocab | null; canWrite: boolean; source: 'bundled' | 'user' | null
}) {
  const { show } = useToast()
  const confirm = useConfirm()
  const topo = useMemo(() => topologies.find(t => t.id === topoId)?.doc ?? null, [topologies, topoId])
  const [sel, setSel] = useState<Sel>({ kind: 'scenario' })
  const [tab, setTab] = useState<'yaml' | 'issues' | 'fit' | 'table'>('yaml')
  const [drawerOpen, setDrawerOpen] = useState(true)
  const [drawerH, onDrawerHandle] = useDrawerHeight('tester-scn-drawer', 240)
  const [bind, setBind] = useState<S.Bind>({ ht: 20 })
  const [collapsed, setCollapsed] = useState<Record<string, boolean>>({})
  const [drag, setDrag] = useState<Drag | null>(null)
  const [hot, setHot] = useState<string | null>(null)
  const [mk, setMk] = useState<{ idx: number; k: number; rect: DOMRect; len: number } | null>(null)
  const hotRef = useRef<string | null>(null)
  const [plan, setPlan] = useState<PlanResult | null>(null)
  const [planning, setPlanning] = useState(false)
  const profDoc = useMemo(() => profiles.find(p => p.name === profileId), [profiles, profileId])
  const dragRef = useRef<Drag | null>(null); dragRef.current = drag

  useEffect(() => { if (profileId && profileId !== NONE) testerApi.profile(profileId).then(p => { const ht = (p.doc as { ht?: number }).ht; if (ht != null) setBind(b => ({ ...b, ht })) }).catch(() => {}) }, [profileId])

  const mutate = useCallback((fn: (d: Doc) => void, transient = false) => { const d = S.deep(doc); fn(d); onChange(d, transient) }, [doc, onChange])
  const R = S.roles(doc)
  const issues = useMemo(() => S.validate(doc, topo, vocab, bind), [doc, topo, vocab, bind])
  // 문서 자체(토폴로지 없이) 오류 = 저장 게이트 · 나머지 = 기준 토폴로지 적합 오류(문맥)
  const docKeys = useMemo(() => new Set(S.validate(doc, null, vocab, bind).map(i => `${i.lv}|${i.who}|${i.msg}`)), [doc, vocab, bind])
  const isTopoIssue = (i: S.Issue) => !docKeys.has(`${i.lv}|${i.who}|${i.msg}`)
  const errN = issues.filter(i => i.lv === 'error').length, warnN = issues.filter(i => i.lv === 'warning').length
  const docErrN = issues.filter(i => i.lv === 'error' && !isTopoIssue(i)).length, topoErrN = errN - docErrN
  const LV = { error: 0, warning: 1, info: 2 }
  const sortedIssues = useMemo(() => [...issues].sort((a, b) => LV[a.lv] - LV[b.lv] || a.who.localeCompare(b.who)), [issues])   // eslint-disable-line react-hooks/exhaustive-deps
  const SESS = useMemo(() => S.sessions(doc), [doc])
  const TL = useMemo(() => S.timeline(doc, bind), [doc, bind])
  const rp = (n: string) => S.resolvePool(doc, topo, n)
  const kindColor = (n: string) => { const k = rp(n)?.kind; return k === 'peer' ? 'var(--chart-2)' : k ? 'var(--chart-1)' : 'var(--destructive)' }

  // 적합성 — compile-check(500 ms 디바운스)
  useEffect(() => {
    if (tab !== 'fit' || !topoId || errN) { return }
    let alive = true; setPlanning(true)
    const id = window.setTimeout(async () => {
      try { const p = await testerApi.compileCheck({ doc: { ...doc, comment: undefined }, topology_id: topoId, profile: profileId && profileId !== NONE ? profileId : undefined, bindings: bind, probe: false }); if (alive) setPlan(p) }
      catch (e) { if (alive) setPlan({ ok: false, errors: [String(e)], warnings: [], notes: [] }) } finally { if (alive) setPlanning(false) }
    }, 500)
    return () => { alive = false; window.clearTimeout(id) }
  }, [doc, topoId, profileId, bind, tab, errN])

  // ── 편집 ────────────────────────────────────────────────────────────────
  const insertStep = (kind: string, at: number) => mutate(d => { d.flow.splice(at, 0, S.newStep(kind, d, topo, vocab)); setSel({ kind: 'step', idx: at }) })
  const moveStep = (from: number, to: number) => mutate(d => { if (to > from) to--; const [s] = d.flow.splice(from, 1); d.flow.splice(to, 0, s); setSel({ kind: 'step', idx: to }) })
  const addRole = (k: 'ue' | 'peer') => mutate(d => { const n = S.addRole(d, topo, k); setSel({ kind: 'role', id: n }) })
  const removeStep = (i: number) => mutate(d => { d.flow.splice(i, 1); setSel({ kind: 'scenario' }) })
  const dupStep = (i: number) => mutate(d => { d.flow.splice(i + 1, 0, S.deep(d.flow[i])); setSel({ kind: 'step', idx: i + 1 }) })

  // HTML5 DnD — 팔레트/행/레인
  const onDragStart = (dg: Drag) => (e: React.DragEvent) => { if (!canWrite) { e.preventDefault(); return } setDrag(dg); e.dataTransfer.effectAllowed = 'move'; try { e.dataTransfer.setData('text/plain', 'x') } catch { /* 무시 */ } }
  const onDragEnd = () => { setDrag(null); setHot(null) }
  const gapOver = (i: number) => (e: React.DragEvent) => { const d = dragRef.current; if (d && (d.type === 'newstep' || d.type === 'move')) { e.preventDefault(); setHot(`gap:${i}`) } }
  const gapDrop = (i: number) => (e: React.DragEvent) => { const d = dragRef.current; if (!d) return; e.preventDefault(); if (d.type === 'newstep') insertStep(d.step, i); else if (d.type === 'move') moveStep(d.idx, i); onDragEnd() }
  // 통화 유지 바 위 — 팔레트의 in-dialog 단계(새 during) 또는 기존 in-dialog 행(행 → during)
  const holdAccepts = (d: Drag | null, i: number) => !!d && ((d.type === 'newstep' && S.DURING_OK.has(d.step)) || (d.type === 'move' && d.idx !== i && S.DURING_OK.has(doc.flow[d.idx]?.step)))
  const holdOver = (i: number) => (e: React.DragEvent) => { if (holdAccepts(dragRef.current, i)) { e.preventDefault(); e.stopPropagation(); setHot(`hold:${i}`) } }
  const holdDrop = (i: number) => (e: React.DragEvent) => {
    const d = dragRef.current; if (!holdAccepts(d, i) || !d) return
    e.preventDefault(); e.stopPropagation()
    const r = e.currentTarget.getBoundingClientRect(); const len = S.secondsOf(doc.flow[i], bind) || 1
    const at = Math.round(Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)) * len * 2) / 2
    if (d.type === 'move') { mutate(x => { const res = S.stepToDuring(x, d.idx, i, at); if (res) setSel({ kind: 'sub', ...res }) }); onDragEnd(); return }
    if (d.type !== 'newstep') return
    mutate(x => { const s = x.flow[i]; const peer = R.find(n => rp(n)?.kind === 'peer'); const from = d.step === 'refer' ? (peer ?? R[0]) : R[0]
      const dd: During = S.MEDIA_CTL.has(d.step) ? { at_s: at, step: d.step as During['step'], who: [R[0]] } : { at_s: at, step: d.step as During['step'], from }
      if (d.step === 'dtmf') dd.payload = '1234#'; if (d.step === 'refer') dd.to = R.find(n => n !== from) ?? R[0]
      s.during = [...(s.during ?? []), dd]; setSel({ kind: 'sub', idx: i, k: s.during.length - 1 }) })
    onDragEnd()
  }
  const laneOver = (id: string) => (e: React.DragEvent) => { const d = dragRef.current; if (d && d.type === 'lane' && d.id !== id) { e.preventDefault(); setHot(`lane:${id}`) } }
  const laneDrop = (id: string) => (e: React.DragEvent) => { const d = dragRef.current; if (d?.type === 'lane') { e.preventDefault(); mutate(x => S.reorderRoles(x, d.id, id)) } onDragEnd() }
  const addOver = (e: React.DragEvent) => { const d = dragRef.current; if (d?.type === 'newrole') { e.preventDefault(); setHot('laneadd') } }
  const addDrop = (e: React.DragEvent) => { const d = dragRef.current; if (d?.type === 'newrole') { e.preventDefault(); addRole(d.role) } onDragEnd() }

  // during 마커 포인터 드래그 — 바 위에서는 at_s, 바에서 세로로 24px 넘게 벗어나 행 사이(gap)에 놓으면 독립 행으로
  hotRef.current = hot
  useEffect(() => {
    if (!mk) return
    const move = (e: PointerEvent) => {
      const dy = e.clientY - (mk.rect.top + mk.rect.height / 2)
      if (Math.abs(dy) > 24) { const g = document.elementFromPoint(e.clientX, e.clientY)?.closest('[data-gap]') as HTMLElement | null; setHot(g ? `gap:${g.dataset.gap}` : 'detach'); return }
      if (hotRef.current) setHot(null)
      const f = Math.max(0, Math.min(1, (e.clientX - mk.rect.left) / mk.rect.width)); const at = Math.round(f * mk.len * 2) / 2; const d = doc.flow[mk.idx]?.during?.[mk.k]; if (d && d.at_s !== at) mutate(x => { x.flow[mk.idx].during![mk.k].at_s = at }, true)
    }
    const up = () => {
      const h = hotRef.current; setHot(null); setMk(null); onCommit?.()
      if (h?.startsWith('gap:')) { const at = Number(h.slice(4)); mutate(x => { const n = S.duringToStep(x, mk.idx, mk.k, at); if (n >= 0) setSel({ kind: 'step', idx: n }) }); return }
      setSel({ kind: 'sub', idx: mk.idx, k: mk.k })
    }
    window.addEventListener('pointermove', move); window.addEventListener('pointerup', up)
    return () => { window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up) }
  }, [mk, doc, mutate])

  useEffect(() => {
    const kd = (e: KeyboardEvent) => {
      if ((e.target as HTMLElement).closest('input,select,textarea')) return
      if (e.key === 'Escape') setSel({ kind: 'scenario' })
      // ↑↓ = 행 선택 이동, Alt+↑↓ = 순서 바꾸기
      if ((e.key === 'ArrowUp' || e.key === 'ArrowDown') && doc.flow.length && !(e.target as HTMLElement).closest('button,[role=combobox],[role=listbox],[role=option]')) {
        const dir = e.key === 'ArrowUp' ? -1 : 1
        if (e.altKey) { if (canWrite && sel.kind === 'step') { const to = sel.idx + dir; if (to >= 0 && to < doc.flow.length) { e.preventDefault(); moveStep(sel.idx, dir > 0 ? to + 1 : to) } } return }
        e.preventDefault()
        const cur = sel.kind === 'step' ? sel.idx : sel.kind === 'sub' ? sel.idx : dir > 0 ? -1 : doc.flow.length
        setSel({ kind: 'step', idx: Math.max(0, Math.min(doc.flow.length - 1, cur + dir)) }); return
      }
      if (!canWrite) return
      if (e.key === 'Delete' && sel.kind === 'step') removeStep(sel.idx)
      if (e.key === 'Delete' && sel.kind === 'sub') mutate(d => { d.flow[sel.idx].during!.splice(sel.k, 1); setSel({ kind: 'step', idx: sel.idx }) })
      if ((e.ctrlKey || e.metaKey) && e.key === 'd' && sel.kind === 'step') { e.preventDefault(); dupStep(sel.idx) }
    }
    window.addEventListener('keydown', kd); return () => window.removeEventListener('keydown', kd)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [sel, doc, canWrite])

  const isSelStep = (i: number) => sel.kind === 'step' && sel.idx === i
  const errIdx = useMemo(() => new Set(issues.filter(i => i.lv === 'error' && i.ref?.kind === 'step').map(i => (i.ref as { idx: number }).idx)), [issues])

  // ── 렌더: 시퀀스 ─────────────────────────────────────────────────────────
  const n = Math.max(1, R.length)
  const cx = (r: string) => ((R.indexOf(r) + 0.5) / n) * 100
  const stepLabel = (s: Step) => {
    switch (s.step) {
      case 'invite': return (s.media ? [s.media.audio, s.media.video].filter(Boolean).join('/') : 'INVITE') + (s.media?.rtp && s.media.rtp !== 'auto' ? ` · rtp ${s.media.rtp}` : '')
      case 'media_send': return `송출 ${s.sample ?? '기본 원천'}${s.loop === false ? ' · 1회' : ''}${s.after_ms ? ` · ${s.after_ms} ms` : ''}`
      case 'media_stop': return '송출 정지'
      case 'answer': return `200 · ${s.after_ms ?? 0} ms`
      case 'reject': return `${s.payload ?? '4xx'} · ${s.after_ms ?? 0} ms${s.cause ? ` · Q.850 ${s.cause}` : ''}`
      case 'bye': return `BYE${s.cause ? ` · Q.850 ${s.cause}` : ''}`
      case 'dtmf': return `DTMF ${s.payload ?? ''}`; case 'progress': return '183 + SDP'; case 'register': return 'REGISTER'; case 'deregister': return 'Expires: 0'
      case 'sds_send': return `SDS${s.plane === 'media' ? '(MSRP)' : ''} ${s.to ? `→ ${s.to}` : `그룹${(s as Step).group ? ` ${(s as Step).group}` : ''}`}${s.disposition ? ' · delivery' : ''}`; case 'sds_recv': return 'SDS 도착 대기'
      case 'check': return `판정 ${s.payload ?? '?'}${s.to ? ` (${s.to})` : ''}`
      case 'refer': return 'REFER'
      case 'pickup': return `픽업 ${s.payload ?? ''}${s.to ? ` → ${s.to}` : ''}`
      case 'replaces': return 'INVITE-Replaces'; case 'join': return 'INVITE-Join (recvonly)'
      case 'subscribe': return `SUBSCRIBE ${s.payload ?? 'dialog'}${s.to ? ` → ${s.to}` : ''}`
      case 'publish': return `PUBLISH ${s.payload ?? 'affiliate'}${s.group ? ` ${s.group}` : ''}`
      case 'group_call': return `${s.payload === 'listen' ? '청취 합류(recvonly)' : '그룹콜'}${s.group ? ` ${s.group}` : ''}${s.media ? ` · ${s.media.audio ?? 'amr-wb'}` : ''}${s.media?.rtp && s.media.rtp !== 'auto' ? ` · rtp ${s.media.rtp}` : ''}`
      case 'floor_request': return `Floor 요청${s.payload && s.payload !== 'granted' ? ` → ${s.payload}` : ''}`
      case 'floor_release': return 'Floor 해제'
      default: return s.step
    }
  }
  const lanesFor = (s: Step, i: number): ReactNode => {
    const D = vocab?.steps[s.step]
    const actors = s.who?.length ? s.who : [s.from].filter((x): x is string => !!x)
    const grid = R.map((_, k) => <span key={k} className="absolute inset-y-0 border-l border-dashed border-border" style={{ left: `${(k / n) * 100}%` }} />)
    if (s.step === 'media_hold') {
      const len = S.secondsOf(s, bind) || 1
      const so = SESS.find(o => o.est != null && i > o.est && i < o.end)
      return <>{grid}
        <span data-hold={i} onDragOver={holdOver(i)} onDrop={holdDrop(i)} className={`absolute inset-x-[6%] top-1/2 h-2 -translate-y-1/2 rounded-sm ${hot === `hold:${i}` ? 'ring-2 ring-success' : ''}`} style={{ background: `color-mix(in srgb, ${GROUP_COLOR.media} 45%, transparent)` }}>
          {[0, 0.25, 0.5, 0.75, 1].map(f => <span key={f} className="absolute top-2.5 -translate-x-1/2 text-[10px] text-muted-foreground" style={{ left: `${f * 100}%` }}>{+(f * len).toFixed(1)}</span>)}
        </span>
        <span className="absolute left-[6%] top-[3px] text-[10px] text-muted-foreground">통화 유지 {String(s.seconds ?? '?')} s · {so ? 'RTP 표본' : '세션 밖'}</span>
        {(s.during ?? []).map((d, k) => { const x = 6 + Math.max(0, Math.min(1, (d.at_s ?? 0) / len)) * 88; const a = d.from ?? (d.who ?? [])[0] ?? ''; const on = sel.kind === 'sub' && sel.idx === i && sel.k === k
          const lab = d.step === 'dtmf' ? `DTMF ${d.payload ?? ''}` : d.step === 'refer' ? `REFER → ${d.to ?? '?'}` : d.step === 'media_send' ? `송출 ${d.sample ?? '기본'}` : d.step === 'media_stop' ? '송출 정지' : d.step.toUpperCase()
          return <span key={k} onPointerDown={e => { e.stopPropagation(); e.preventDefault(); if (!canWrite) { setSel({ kind: 'sub', idx: i, k }); return } const hb = (e.currentTarget.parentElement as HTMLElement).querySelector(`[data-hold="${i}"]`)!; setMk({ idx: i, k, rect: hb.getBoundingClientRect(), len }) }}
                       className={`absolute top-1/2 h-3.5 w-3.5 -translate-x-1/2 -translate-y-1/2 cursor-ew-resize rounded-full border-2 bg-card ${on ? 'ring-2 ring-primary' : ''}`} style={{ left: `${x}%`, borderColor: kindColor(a) }} title={`${d.step} @ ${d.at_s}s · ${a} — 좌우로 끌면 시각, 위아래로 끌어 행 사이에 놓으면 독립 행`}>
            <span className={`absolute left-1/2 -translate-x-1/2 whitespace-nowrap text-[10px] ${k % 2 ? 'top-4' : '-top-4'}`}>+{d.at_s}s {a} {lab}</span></span> })}
      </>
    }
    if (D?.actor === 'seconds' || D?.actor === 'none') return <>{grid}<span className="absolute inset-x-[6%] top-1/2 h-1 -translate-y-1/2 rounded-sm bg-muted-foreground/40" /><span className="absolute left-[6%] top-[3px] text-[10px] text-muted-foreground">{s.step === 'expect' ? '누계 게이트' : `대기 ${String(s.seconds ?? '?')} s`}</span></>
    if ((D?.actor === 'fromto' || s.step === 'invite') && s.from && s.to && R.includes(s.from) && R.includes(s.to)) {
      const a = cx(s.from), b = cx(s.to); const c = kindColor(s.from)
      return <>{grid}
        <span className="absolute top-1/2 h-2.5 w-2.5 -translate-x-1/2 -translate-y-1/2 rounded-full" style={{ left: `${a}%`, background: c }} />
        <span className="absolute top-1/2 h-2.5 w-2.5 -translate-x-1/2 -translate-y-1/2 rounded-full border-2 bg-card" style={{ left: `${b}%`, borderColor: kindColor(s.to) }} />
        <span className="absolute top-1/2 h-[2px] -translate-y-1/2" style={{ left: `${Math.min(a, b)}%`, width: `${Math.abs(b - a)}%`, background: c }} />
        <span className="absolute top-1/2 -translate-y-1/2 text-[10px]" style={{ left: `${Math.max(a, b)}%`, transform: `translate(${b < a ? '-100%' : '-100%'}, -50%)`, color: c }}>{b < a ? '◀' : '▶'}</span>
        <span className="absolute -top-0.5 -translate-x-1/2 rounded-sm bg-card px-1 text-[10px]" style={{ left: `${(a + b) / 2}%` }}>{stepLabel(s)}</span></>
    }
    const xs = actors.filter(r => R.includes(r)).map(cx)
    return <>{grid}
      {actors.filter(r => R.includes(r)).map(r => <span key={r} className="absolute top-1/2 h-2.5 w-2.5 -translate-x-1/2 -translate-y-1/2 rounded-full" style={{ left: `${cx(r)}%`, background: kindColor(r) }} />)}
      {xs.length > 0 && <span className="absolute -top-0.5 -translate-x-1/2 rounded-sm bg-card px-1 text-[10px]" style={{ left: `${(Math.min(...xs) + Math.max(...xs)) / 2}%` }}>{stepLabel(s)}</span>}
    </>
  }
  const railFor = (i: number): ReactNode => SESS.map((o, si) => {
    const x = 6 + si * 22; const first = o.prog ?? o.est
    const els: ReactNode[] = []
    if (i >= o.start && (first == null ? i <= o.end : i <= first)) els.push(<span key="ring" className="absolute inset-y-0 w-[3px] border-l-2 border-dashed" style={{ left: x, borderColor: 'var(--muted-foreground)' }} title={`${o.a} → ${o.b} INVITE~확립`} />)
    if (i === o.start) els.push(<span key="lab" className="absolute top-0 text-[10px] text-muted-foreground" style={{ left: x + 6 }}>{o.via === 'refer' ? 'REFER' : 'INVITE'}</span>)
    if (first != null && i >= first && i <= o.end) els.push(<span key="media" className="absolute inset-y-0 w-[6px] rounded-sm" style={{ left: x, background: GROUP_COLOR.media, opacity: 0.8 }} title={`${o.a} ⇄ ${o.b} RTP`} />)
    return <span key={si}>{els}</span>
  })

  const phaseRow = (ph: string) => <div key={`ph-${ph}`} className="flex items-center gap-2 border-t border-border bg-muted px-3 py-0.5 text-[10px] font-semibold text-muted-foreground"><span>{PH[ph][0]}</span><span className="font-normal">{PH[ph][1]}</span></div>
  // 끌기 중에는 놓을 자리를 키운다(8 → 16px)
  const Gap = ({ i }: { i: number }) => <div data-gap={i} onDragOver={gapOver(i)} onDrop={gapDrop(i)} className={`transition-all ${(drag && (drag.type === 'newstep' || drag.type === 'move')) || mk ? 'h-4' : 'h-2'} ${hot === `gap:${i}` ? 'bg-success/50' : drag || mk ? 'bg-primary/10' : ''}`} />
  let lastPh: string | null = null

  return (
    <div className="flex min-h-0 flex-1 flex-col">
      {/* 바인딩 바 */}
      <div className="flex flex-wrap items-center gap-2 border-b border-border bg-muted px-3 py-1 text-xs">
        <span className="text-muted-foreground">기준 토폴로지</span>
        <Select value={topoId != null ? String(topoId) : NONE} onValueChange={v => setTopoId(v === NONE ? null : Number(v))}>
          <SelectTrigger className="h-[24px] w-[220px] text-xs"><SelectValue /></SelectTrigger>
          <SelectContent><SelectItem value={NONE}>(없음 — 해석 안 함)</SelectItem>{topologies.map(t => <SelectItem key={t.id} value={String(t.id)}>{t.name} · {t.doc.target?.kind ?? 'cims'}</SelectItem>)}</SelectContent>
        </Select>
        <span className="text-muted-foreground">미리보기 프로파일</span>
        <Select value={profileId || NONE} onValueChange={setProfileId}>
          <SelectTrigger className="h-[24px] w-[190px] text-xs"><SelectValue /></SelectTrigger>
          <SelectContent><SelectItem value={NONE}>(단발 — 프로파일 없음)</SelectItem>{profiles.map(p => <SelectItem key={p.name} value={p.name}>{p.name} · {p.model}</SelectItem>)}</SelectContent>
        </Select>
        <span className="ml-2 text-muted-foreground">바인딩</span>
        {S.bindVars(doc).length === 0 && <span className="font-mono text-muted-foreground">없음 — seconds 에 ${'{ht}'} 를 쓰면 프로파일 ht 로 묶입니다</span>}
        {S.bindVars(doc).map(v => { const numeric = doc.flow.some(s => S.bindRef(s.seconds) === v); return <span key={v} className={`inline-flex items-center gap-1 rounded-sm border px-1.5 py-0.5 font-mono ${v in bind ? 'border-border' : 'border-warning'}`}>${'{'}{v}{'}'} = <Input value={bind[v] ?? ''} onChange={e => setBind(b => { const nb = { ...b }; const t = e.target.value; if (t === '') delete nb[v]; else nb[v] = numeric ? Number(t) : t; return nb })} className="h-5 w-16 px-1 text-xs" inputMode={numeric ? 'numeric' : undefined} placeholder={numeric ? '' : '**'} />{numeric ? ' s' : ''}</span> })}
        {profDoc && <span className="text-muted-foreground">profile {profDoc.model}</span>}
      </div>

      <div className="grid min-h-0 flex-1 grid-cols-[200px_minmax(0,1fr)_320px]">
        {/* 팔레트 */}
        <aside className="min-h-0 overflow-auto border-r border-border bg-card p-2 text-xs">
          <div className="mb-2">
            <div className="py-1 text-[11px] font-semibold text-muted-foreground">역할 (레인) <span className="font-normal">{R.length}</span></div>
            {(['ue', 'peer'] as const).map(k => <div key={k} draggable={canWrite} onDragStart={onDragStart({ type: 'newrole', role: k })} onDragEnd={onDragEnd} onClick={() => canWrite && addRole(k)} className={`mb-1 flex select-none items-center gap-2 rounded-sm border border-border bg-muted px-2 py-1.5 ${canWrite ? 'cursor-grab hover:border-primary' : 'opacity-60'}`}>
              <span className="inline-block h-3 w-3 rounded-sm" style={{ background: k === 'peer' ? 'var(--chart-2)' : 'var(--chart-1)' }} /><span><b>{k === 'ue' ? 'UE 역할' : '피어 역할'}</b><div className="text-[10px] text-muted-foreground">{k === 'ue' ? '토폴로지 UE 풀 / group' : 'IBCF · PBX · MGCF 풀'}</div></span></div>)}
          </div>
          {(vocab?.groups ?? []).map(g => {
            const items = Object.entries(vocab?.steps ?? {}).filter(([, d]) => d.group === g.id)
            return <div key={g.id} className="mb-2">
              <button className="flex w-full items-center gap-1 py-1 text-[11px] font-semibold text-muted-foreground" onClick={() => setCollapsed(c => ({ ...c, [g.id]: !c[g.id] }))}>{collapsed[g.id] ? <ChevronRight size={12} /> : <ChevronDown size={12} />}{g.label}<span className="ml-1 font-normal">{items.length}</span></button>
              {!collapsed[g.id] && items.map(([k, d]) => (
                <div key={k} draggable={canWrite && d.supported} onDragStart={onDragStart({ type: 'newstep', step: k })} onDragEnd={onDragEnd}
                     onClick={() => canWrite && d.supported && insertStep(k, sel.kind === 'step' ? sel.idx + 1 : doc.flow.length)}
                     title={d.supported ? '캔버스 행 사이로 끌어 놓기 (클릭 = 선택 단계 뒤에 삽입)' : '워커 미지원 — 워커 재빌드 필요'}
                     className={`mb-1 flex select-none items-center gap-2 rounded-sm border border-border bg-muted px-2 py-1 ${d.supported && canWrite ? 'cursor-grab hover:border-primary' : 'opacity-45'}`}>
                  <span className="inline-flex h-5 w-6 shrink-0 items-center justify-center rounded-sm text-[10px] font-bold text-white" style={{ background: GROUP_COLOR[g.id] ?? 'var(--chart-6)' }}>{k.slice(0, 2).toUpperCase()}</span>
                  <span className="min-w-0"><b>{k}</b><div className="truncate text-[10px] text-muted-foreground">{d.desc}{d.supported ? '' : ' · 미지원'}</div></span>
                </div>))}
            </div>
          })}
          <div className="rounded-sm border border-border p-2 text-[11px] leading-relaxed text-muted-foreground"><b>레인 = 역할, 행 = 단계.</b> 팔레트에서 행 사이로 끌어 놓습니다. 구간은 자동(앞쪽 register/wait = prelude · 끝 deregister = epilogue). 세션 열(오른쪽)은 다이얼로그마다 막대 하나 — INVITE~확립 점선, 확립~bye RTP. 통화 중 동작(dtmf·hold·resume·refer)은 확립된 세션 안에만, <b>통화 유지 바 위에 놓으면 during</b>(at_s) — 기존 행을 끌어 놓아도, 마커를 행 사이로 끌어내도 됩니다. <kbd>↑↓</kbd> 행 이동 · <kbd>Alt+↑↓</kbd> 순서 · <kbd>Del</kbd> 삭제 · <kbd>Ctrl+D</kbd> 복제 · <kbd>Ctrl+Z</kbd>/<kbd>Ctrl+Y</kbd> 실행취소/다시실행 · <kbd>Esc</kbd></div>
        </aside>

        {/* 시퀀스 캔버스 */}
        <div className="min-h-0 overflow-auto bg-background" onClick={e => { if (e.target === e.currentTarget) setSel({ kind: 'scenario' }) }}>
          <div className="min-w-[860px]">
            <div className="sticky top-0 z-[3] grid grid-cols-[56px_150px_minmax(0,1fr)_84px_180px_28px] border-b border-border bg-card text-xs">
              <div className="px-2 py-1 text-muted-foreground">#</div><div className="px-2 py-1 text-muted-foreground">단계</div>
              <div className="grid" style={{ gridTemplateColumns: `repeat(${n}, minmax(0,1fr)) 70px` }}>
                {R.map(r => { const res = rp(r); const role = doc.roles[r]; const on = sel.kind === 'role' && sel.id === r
                  return <div key={r} draggable={canWrite} onDragStart={onDragStart({ type: 'lane', id: r })} onDragEnd={onDragEnd} onDragOver={laneOver(r)} onDrop={laneDrop(r)} onClick={() => setSel({ kind: 'role', id: r })}
                              className={`cursor-pointer border-l border-border px-2 py-1 ${on ? 'bg-accent' : ''} ${hot === `lane:${r}` ? 'ring-2 ring-success' : ''}`}>
                    <div className="flex items-center gap-1"><b>{r}</b>{role.count ? <span className="font-mono text-muted-foreground">×{role.count}</span> : null}{role.disjoint_from && <span title={`disjoint_from ${role.disjoint_from}`}>⟷</span>}<Badge variant={res ? (res.kind === 'peer' ? 'warningSoft' : 'infoSoft') : 'dangerSoft'} className="ml-auto">{res ? S.roleKindTag(doc, topo, r) : '?'}</Badge></div>
                    <div className="truncate font-mono text-[10px] text-muted-foreground">{role.pool}{res && !res.byName ? ' ≡ group' : ''}{res ? ` → ${Object.keys(res.pools).join('+')}` : topo ? ' → 풀 없음' : ''}</div>
                  </div> })}
                <div onDragOver={addOver} onDrop={addDrop} onClick={() => canWrite && addRole('ue')} className={`flex cursor-pointer items-center justify-center border-l border-dashed border-border text-[10px] text-muted-foreground hover:bg-accent ${hot === 'laneadd' ? 'ring-2 ring-success' : ''}`}>+ 역할</div>
              </div>
              <div className="px-2 py-1 text-muted-foreground">세션</div><div className="px-2 py-1 text-muted-foreground">기대치 (expect)</div><div />
            </div>
            {doc.flow.length === 0 && <div data-gap={0} onDragOver={gapOver(0)} onDrop={gapDrop(0)} className={`m-4 rounded-md border-2 border-dashed p-6 text-center text-sm text-muted-foreground ${hot === 'gap:0' ? 'border-success' : 'border-border'}`}>단계를 여기로 끌어 놓으세요 (팔레트 클릭 = 끝에 추가)</div>}
            {doc.flow.map((s, i) => {
              const ph = S.phaseOf(doc, i); const header = ph !== lastPh ? phaseRow(ph) : null; lastPh = ph
              const D = vocab?.steps[s.step]
              const sum = [s.who?.length ? `who ${s.who.join(',')}` : '', s.from ? `from ${s.from}` : '', s.to ? `to ${s.to}` : '', s.after_ms ? `+${s.after_ms}ms` : '', s.during?.length ? `during ×${s.during.length}` : ''].filter(Boolean).join(' · ')
              return <div key={i}>
                {header}<Gap i={i} />
                <div draggable={canWrite} onDragStart={onDragStart({ type: 'move', idx: i })} onDragEnd={onDragEnd} onClick={() => setSel({ kind: 'step', idx: i })}
                     className={`group grid grid-cols-[56px_150px_minmax(0,1fr)_84px_180px_28px] items-stretch border-b border-border text-xs ${isSelStep(i) ? 'bg-accent' : 'hover:bg-accent/40'} ${errIdx.has(i) ? 'shadow-[inset_3px_0_0_var(--destructive)]' : ''} ${drag?.type === 'move' && drag.idx === i ? 'opacity-40' : ''}`} style={{ minHeight: s.step === 'media_hold' ? 56 : 40 }}>
                  <div className="flex flex-col justify-center px-2 font-mono"><span>{i + 1}</span>{TL[i] != null && <span className="text-[10px] text-muted-foreground">t+{+(TL[i] as number).toFixed(1)}</span>}</div>
                  <div className="flex min-w-0 flex-col justify-center px-2"><b>{s.step}{D && !D.supported && <span title="워커 미지원" className="ml-1 text-warning">⚠</span>}</b><span className="truncate text-[10px] text-muted-foreground">{sum || D?.desc}</span></div>
                  <div className="relative">{lanesFor(s, i)}</div>
                  <div className="relative border-l border-border">{railFor(i)}</div>
                  <div className="flex flex-wrap items-center gap-1 px-2 py-1">{Object.entries(s.expect ?? {}).map(([k, v]) => <Badge key={k} variant={vocab && !vocab.metrics[k] ? 'dangerSoft' : 'neutralSoft'} className="font-mono" title={vocab?.metrics[k]}>{k} {typeof v === 'object' && v ? Object.entries(v as Record<string, unknown>).map(([a, b]) => `${a}${S.thrOp(a)}${b}`).join(' ') : `= ${String(v)}`}</Badge>)}{!Object.keys(s.expect ?? {}).length && <span className="text-muted-foreground">—</span>}</div>
                  <div className="flex items-center justify-center">{canWrite && <button onClick={e => { e.stopPropagation(); removeStep(i) }} className="opacity-0 group-hover:opacity-100" title="삭제"><X size={12} /></button>}</div>
                </div>
              </div>
            })}
            {doc.flow.length > 0 && <Gap i={doc.flow.length} />}
            <div className="h-6" />
          </div>
        </div>

        {/* 속성 */}
        <aside className="min-h-0 overflow-auto border-l border-border bg-card p-3 text-xs">
          <Inspector doc={doc} sel={sel} setSel={setSel} mutate={mutate} topo={topo} vocab={vocab} issues={issues} canWrite={canWrite} source={source} bind={bind} plan={plan}
                     onRemoveRole={async n => { const refs = doc.flow.filter(s => (s.who ?? []).includes(n) || s.from === n || s.to === n).length; if (refs && !await confirm({ title: `역할 ${n} 삭제`, body: `${refs} 개 단계가 참조합니다. 참조는 비워집니다.`, confirmLabel: '삭제', tone: 'danger' })) return; mutate(d => S.removeRole(d, n)); setSel({ kind: 'scenario' }) }}
                     onRemoveStep={removeStep} onDupStep={dupStep}
                     onDuringToStep={(i, k) => mutate(x => { const n = S.duringToStep(x, i, k, i + 1); if (n >= 0) setSel({ kind: 'step', idx: n }) })}
                     onStepToDuring={i => { const h = S.nearestHold(doc, i); if (h < 0) { show('붙일 media_hold(통화 유지) 단계가 없습니다', 'err'); return } const len = S.secondsOf(doc.flow[h], bind); mutate(x => { const res = S.stepToDuring(x, i, h, h < i ? len : 0); if (res) setSel({ kind: 'sub', ...res }) }) }} />
        </aside>
      </div>

      {/* 드로어 */}
      <div className="border-t border-border bg-card">
        {drawerOpen && <div onPointerDown={onDrawerHandle} className="group flex h-2 cursor-row-resize items-center justify-center hover:bg-accent" title="끌어서 높이 조절"><span className="h-0.5 w-10 rounded-full bg-border group-hover:bg-muted-foreground" /></div>}
        <div className="flex items-center gap-1 px-3 py-1 text-xs">
          {(['yaml', 'issues', 'fit', 'table'] as const).map(t => (
            <button key={t} onClick={() => { setTab(t); setDrawerOpen(true) }} className={`h-6 rounded-sm px-2 ${tab === t && drawerOpen ? 'bg-primary text-primary-foreground' : 'text-muted-foreground hover:bg-accent'}`}>
              {t === 'yaml' ? 'YAML' : t === 'issues' ? <>검증 {docErrN ? <Badge variant="dangerSoft" className="ml-1" title="문서 오류 — 저장이 잠깁니다">문서 {docErrN}</Badge> : null}{topoErrN ? <Badge variant="warningSoft" className="ml-1" title={`기준 토폴로지 ${topo?.name ?? ''} 와의 적합 오류 — 저장은 되지만 이 토폴로지로는 실행할 수 없습니다`}>토폴로지 {topoErrN}</Badge> : null}{!errN && <Badge variant={warnN ? 'warningSoft' : 'successSoft'} className="ml-1">{issues.length}</Badge>}</> : t === 'fit' ? <>토폴로지 적합성 {plan && <Badge variant={plan.ok ? (plan.warnings.length ? 'warningSoft' : 'successSoft') : 'dangerSoft'} className="ml-1">{plan.ok ? (plan.warnings.length ? `경고 ${plan.warnings.length}` : 'OK') : `오류 ${plan.errors.length}`}</Badge>}</> : '절차표'}
            </button>))}
          <span className="ml-2 text-muted-foreground">{doc.flow.length} 단계 · {R.length} 역할 · {source === 'user' ? '운영자본' : source === 'bundled' ? '동봉' : '새 문서'}</span>
          <button className="ml-auto text-muted-foreground" onClick={() => setDrawerOpen(o => !o)}>{drawerOpen ? <ChevronDown size={14} /> : <ChevronRight size={14} />}</button>
        </div>
        {drawerOpen && (
          <div className="overflow-auto border-t border-border px-3 py-2 text-xs" style={{ height: drawerH }}>
            {tab === 'yaml' && <YamlTab doc={doc} height={drawerH - 20} onApply={d => { onChange(d); setSel({ kind: 'scenario' }); show('YAML 적용 — 캔버스 재구성', 'ok') }} canWrite={canWrite} />}
            {tab === 'issues' && (issues.length === 0 ? <div className="text-success">검증 통과 — 스키마·행위자·구간·kind 게이트 모두 정상{topo ? ` · 기준 토폴로지 ${topo.name} 적합` : ''}</div> : <div className="flex flex-col gap-1">
              {topoErrN > 0 && docErrN === 0 && <div className="text-muted-foreground">문서는 유효합니다(저장 가능). 아래 오류는 기준 토폴로지 <b>{topo?.name}</b> 와의 적합 문제 — 토폴로지를 바꾸거나 역할의 pool 을 고치십시오</div>}
              {sortedIssues.map((it, k) => (
              <button key={k} onClick={() => it.ref && setSel(it.ref)} className={`flex items-center gap-2 rounded-sm border-l-2 px-2 py-1 text-left hover:bg-accent ${it.lv === 'error' ? 'border-destructive' : it.lv === 'warning' ? 'border-warning' : 'border-info'}`}>
                <Badge variant={it.lv === 'error' ? 'dangerSoft' : it.lv === 'warning' ? 'warningSoft' : 'infoSoft'}>{it.lv === 'error' ? '오류' : it.lv === 'warning' ? '경고' : '참고'}</Badge>{isTopoIssue(it) && <Badge variant="neutralSoft" title="기준 토폴로지 문맥의 문제">토폴로지</Badge>}<span className="font-mono text-muted-foreground">{it.who}</span><span>{it.msg}</span></button>))}</div>)}
            {tab === 'fit' && (!topoId ? <div className="text-muted-foreground">기준 토폴로지를 고르면 compile_run 드라이런(역할→풀→워커 창·용량·시드·Little 검산)을 보입니다</div> : errN ? <div className="text-muted-foreground">검증 오류를 먼저 해결하십시오 — 오류가 있는 문서는 컴파일하지 않습니다</div> : <PlanPreview plan={plan} loading={planning} compact />)}
            {tab === 'table' && <ProcedureTable doc={doc} vocab={vocab} plan={plan} />}
          </div>
        )}
      </div>
    </div>
  )
}

function YamlTab({ doc, height, onApply, canWrite }: { doc: Doc; height: number; onApply: (d: Doc) => void; canWrite: boolean }) {
  const text = useMemo(() => S.toYaml(doc), [doc])
  const [edit, setEdit] = useState<string | null>(null)
  const [parsed, setParsed] = useState<unknown>(null)     // 타이핑 중 컨트롤러가 돌려준 doc — [적용] 은 이것으로 캔버스를 다시 그린다
  const [ok, setOk] = useState(true)
  const { show } = useToast()
  const apply = () => { if (!parsed) return; onApply(S.fromApiDoc(parsed as Record<string, unknown>, edit ?? text)); setEdit(null) }
  return (
    <div className="flex gap-2">
      <div className="min-w-0 flex-1">
        <YamlEditor kind="scenario" value={edit ?? text} onChange={v => setEdit(v)} disabled={!canWrite} minHeight={Math.max(120, height - 4)} onValid={(o, d) => { setOk(o); setParsed(d ?? null) }} />
      </div>
      <div className="flex w-[180px] flex-col gap-1 text-[11px] text-muted-foreground">
        <b>정본은 YAML</b><span>캔버스 편집 → YAML 재생성(머리 주석 보존, 행 안 주석 유실). 여기서 고치면 타이핑 중 컨트롤러가 검증하고, [적용] 으로 캔버스를 다시 그립니다.</span>
        <Button variant="outline" size="sm" disabled={!canWrite || edit == null || !parsed} onClick={apply} title={edit != null && !ok ? '검증 오류가 있어도 적용은 됩니다 — 캔버스 [검증] 에서 고치십시오' : undefined}>적용{edit != null && !ok ? ' (오류 있음)' : ''}</Button>
        <Button variant="ghost" size="sm" onClick={async () => { try { await navigator.clipboard.writeText(edit ?? text); show('YAML 복사', 'ok') } catch { show('클립보드 접근 실패', 'err') } }}><Copy size={12} /> 복사</Button>
        {edit != null && <Button variant="ghost" size="sm" onClick={() => setEdit(null)}>취소</Button>}
      </div>
    </div>
  )
}

function ProcedureTable({ doc, vocab, plan }: { doc: Doc; vocab: ScenarioVocab | null; plan: PlanResult | null }) {
  const proc = (s: Step | During): string => {
    const a = (s.who ?? []).join(', ') || s.from || ''
    switch (s.step) {
      case 'register': return `${a} 등록 (REGISTER)`; case 'deregister': return `${a} 등록 해제`
      case 'invite': return `${(s as Step).from} → ${(s as Step).to} 발신${(s as Step).media ? ` (${[(s as Step).media!.audio, (s as Step).media!.video].filter(Boolean).join('/')}${(s as Step).media!.rtp && (s as Step).media!.rtp !== 'auto' ? `, rtp ${(s as Step).media!.rtp}` : ''})` : ''}`
      case 'answer': return `${a} 착신 ${(s as Step).after_ms ?? 0} ms 뒤 응답`; case 'reject': return `${a} ${s.payload ?? ''} 거절${(s as Step).cause ? ` (Q.850 ${(s as Step).cause})` : ''}`
      case 'bye': return `${a} 종료 (BYE${(s as Step).cause ? `, Q.850 ${(s as Step).cause}` : ''})`; case 'media_hold': return `통화 유지 ${(s as Step).seconds} s`; case 'wait': return `대기 ${(s as Step).seconds} s`
      case 'dtmf': return `${a} DTMF ${s.payload} 송신`; case 'refer': return `${s.from} 가 ${s.to} 로 전달 (REFER)`; case 'progress': return `${a} 183 + SDP (early media)`
      case 'sds_send': return `${a} 가 ${s.to ?? '그룹'} 에 MCData SDS ${s.plane === 'media' ? 'media plane(MSRP) 송신 — INVITE m=message → cmdp' : 'MESSAGE 송신'}${s.disposition ? ' (delivery 요청)' : ''}`; case 'sds_recv': return `${a} 가 SDS 를 받을 때까지`
      case 'check': return `${a} 관측 정합 판정 ${s.payload ?? ''}${s.to ? ` — 대상 ${s.to}` : ''}`
      case 'hold': return `${a} 보류 (re-INVITE sendonly)`; case 'resume': return `${a} 재개`
      case 'media_send': return `${a} RTP 송출 시작 (${s.sample ? `샘플 ${s.sample}` : '기본 원천'}${s.loop === false ? ', 한 번 재생' : ''})`; case 'media_stop': return `${a} RTP 송출 정지 (수신은 계속)`
      case 'pickup': return `${a} 당겨받기 — 피처코드 ${s.payload ?? '?'}${(s as Step).to ? ` + ${(s as Step).to} 번호 (지정 픽업)` : ' (그룹 픽업)'}`
      case 'replaces': return `${a} 가 ${(s as Step).to} 의 다이얼로그를 INVITE-Replaces 로 가져온다 (RFC 3891)`
      case 'join': return `${a} 가 ${(s as Step).to} 의 세션에 INVITE-Join recvonly 로 합류 (RFC 3911, 청취)`
      case 'subscribe': return `${a} SUBSCRIBE Event: ${s.payload ?? 'dialog'} → ${(s as Step).to ?? '자기 AoR'}`
      case 'publish': return `${a} PUBLISH affiliation ${s.payload ?? 'affiliate'}${(s as Step).group ? ` (group ${(s as Step).group})` : ''}`
      default: return `${a} ${s.step}`
    }
  }
  const exp = (e: Record<string, unknown> | undefined) => Object.entries(e ?? {}).map(([k, v]) => { const lab = (vocab?.metrics[k] ?? k).split(' — ')[0]; return typeof v === 'object' && v ? Object.entries(v as Record<string, unknown>).map(([p, x]) => `${lab} ${p} ${S.thrOp(p)} ${x}`).join(', ') : `${lab} = ${String(v)}` }).join('; ') || '—'
  const how = (s: Step) => vocab?.steps[s.step]?.actor === 'seconds' ? '발생기 RTP 표본(손실·지터)' : '발생기 SIP 관측 (RFC 6076)'
  return <>
    <DataTable>
      <thead><tr><Th width={40}>#</Th><Th width={70}>구간</Th><Th>절차</Th><Th>예상 결과</Th><Th>확인 방법</Th></tr></thead>
      <tbody>
        {doc.flow.map((s, i) => <>
          <tr key={i}><Td mono>{i + 1}</Td><Td className="text-[10px] text-muted-foreground">{plan?.procedure?.[i]?.phase ?? S.phaseOf(doc, i)}</Td><Td>{proc(s)}</Td><Td className="text-xs">{exp(s.expect)}</Td><Td className="text-xs text-muted-foreground">{how(s)}</Td></tr>
          {[...(s.during ?? [])].sort((a, b) => a.at_s - b.at_s).map((d, k) => <tr key={`${i}.${k}`}><Td mono>{i + 1}.{k + 1}</Td><Td /><Td>↳ 통화 {d.at_s} s 시점 — {proc(d)}</Td><Td className="text-xs">(유지 단계 expect 에서 판정)</Td><Td className="text-xs text-muted-foreground">발생기 SIP/RTP 관측</Td></tr>)}
        </>)}
        {(doc.target_evidence ?? []).map((e, k) => <tr key={`e${k}`}><Td mono>E{k + 1}</Td><Td /><Td>대상 증거 — {e.kind}{e.code ? ` ${e.code}` : ''}</Td><Td className="text-xs">{e.min != null ? `≥ ${e.min}` : ''}{e.max != null ? ` ≤ ${e.max}` : ''}</Td><Td className="text-xs text-muted-foreground">대상 OAM 관측 (2차)</Td></tr>)}
      </tbody>
    </DataTable>
    <div className="mt-1 text-[11px] text-muted-foreground">보고서(/test/results)의 절차·예상 결과·확인 결과 표와 같은 행 — ptt-test-scenario CSV 형식</div>
  </>
}

// ── 속성 패널 ─────────────────────────────────────────────────────────────
function F({ label, children, help }: { label: ReactNode; children: ReactNode; help?: string }) { return <label className="flex flex-col gap-0.5"><span className="text-[11px] text-muted-foreground">{label}</span>{children}{help && <span className="text-[10px] text-muted-foreground">{help}</span>}</label> }
function Txt({ value, onCommit, mono, type, placeholder, disabled, list }: { value: string | number | undefined | null; onCommit: (v: string) => void; mono?: boolean; type?: string; placeholder?: string; disabled?: boolean; list?: string }) {
  const [v, setV] = useState(value == null ? '' : String(value)); useEffect(() => { setV(value == null ? '' : String(value)) }, [value])
  return <Input value={v} type={type} list={list} placeholder={placeholder} disabled={disabled} onChange={e => setV(e.target.value)} onBlur={() => { if (v !== (value == null ? '' : String(value))) onCommit(v) }} onKeyDown={e => { if (e.key === 'Enter') (e.target as HTMLInputElement).blur() }} className={`h-[26px] text-xs ${mono ? 'font-mono' : ''}`} />
}
function Sel({ value, options, onChange, empty, disabled }: { value: string | undefined | null; options: { v: string; l?: string }[]; onChange: (v: string) => void; empty?: string; disabled?: boolean }) {
  return <Select value={value || NONE} onValueChange={v => onChange(v === NONE ? '' : v)} disabled={disabled}><SelectTrigger className="h-[26px] font-mono text-xs"><SelectValue /></SelectTrigger><SelectContent>{empty !== undefined && <SelectItem value={NONE}>{empty}</SelectItem>}{options.map(o => <SelectItem key={o.v} value={o.v}>{o.l ?? o.v}</SelectItem>)}</SelectContent></Select>
}
function Sec({ title, right, children }: { title: string; right?: ReactNode; children: ReactNode }) { return <div className="mt-3 flex flex-col gap-1.5 border-t border-border pt-2"><div className="flex items-center gap-2 text-[11px] font-semibold text-muted-foreground">{title}<span className="ml-auto">{right}</span></div>{children}</div> }
/** 여러 줄 입력 — blur 때 커밋(키 입력마다 문서를 다시 만들지 않게) */
/** media_send 의 원천 — 샘플(기준 토폴로지 media.samples 의 id, 비면 풀 기본 원천) + 반복 여부 */
function SampleFields({ sample, loop, samples, disabled, onSample, onLoop }: { sample?: string; loop?: boolean; samples: string[]; disabled?: boolean; onSample: (v: string) => void; onLoop: (v: boolean) => void }) {
  const opts = [...samples, ...(sample && !samples.includes(sample) ? [sample] : [])]
  return <div className="grid grid-cols-[1fr_auto] items-end gap-2">
    <F label="sample" help={samples.length ? '기준 토폴로지 media.samples 의 id — 합의 코덱 항목이 없으면 합성' : '기준 토폴로지에 샘플 라이브러리(media.samples)가 없다 — 토폴로지 속성에서 추가'}>
      <Sel value={sample} disabled={disabled} options={opts.map(v => ({ v, l: samples.includes(v) ? v : `${v} (라이브러리에 없음)` }))} empty="기본 원천" onChange={onSample} />
    </F>
    <label className="mb-1.5 inline-flex items-center gap-1"><Checkbox checked={loop !== false} disabled={disabled} onCheckedChange={v => onLoop(v === true)} /> 반복</label>
  </div>
}
function Area({ value, onCommit, disabled, rows = 3 }: { value: string; onCommit: (v: string) => void; disabled?: boolean; rows?: number }) {
  const [v, setV] = useState(value); useEffect(() => { setV(value) }, [value])
  return <textarea value={v} disabled={disabled} rows={rows} onChange={e => setV(e.target.value)} onBlur={() => { if (v !== value) onCommit(v) }} className="rounded-sm border border-border bg-background p-1.5 text-xs" />
}
function Chips({ roles, selected, multi, gate, onToggle, disabled, kindOf }: { roles: string[]; selected: string[]; multi: boolean; gate?: (r: string) => boolean; onToggle: (r: string) => void; disabled?: boolean; kindOf: (r: string) => string | undefined }) {
  return <div className="flex flex-wrap gap-1">{roles.map(r => { const on = selected.includes(r); const off = gate && !gate(r); const k = kindOf(r)
    return <button key={r} disabled={disabled || (off && !on)} onClick={() => onToggle(r)} title={off ? `'${r}' 는 이 단계의 행위자가 될 수 없습니다 (풀 kind ${k ?? '없음'})` : (k ?? '풀 없음')} className={`inline-flex h-6 items-center gap-1 rounded-sm border px-1.5 text-[11px] ${on ? 'border-primary bg-primary text-primary-foreground' : 'border-border'} ${off ? 'border-dashed text-muted-foreground line-through' : ''}`}><span className="inline-block h-2 w-2 rounded-full" style={{ background: k === 'peer' ? 'var(--chart-2)' : k ? 'var(--chart-1)' : 'var(--destructive)' }} />{r}</button> })}{!multi && <button disabled={disabled} onClick={() => onToggle('')} className={`h-6 rounded-sm border px-1.5 text-[11px] ${!selected.length ? 'border-primary bg-primary text-primary-foreground' : 'border-border'}`}>없음</button>}</div>
}

function Inspector({ doc, sel, setSel, mutate, topo, vocab, issues, canWrite, source, bind, plan, onRemoveRole, onRemoveStep, onDupStep, onDuringToStep, onStepToDuring }: {
  doc: Doc; sel: Sel; setSel: (s: Sel) => void; mutate: (fn: (d: Doc) => void) => void; topo: TopologyDoc | null; vocab: ScenarioVocab | null; issues: S.Issue[]; canWrite: boolean; source: 'bundled' | 'user' | null; bind: S.Bind; plan: PlanResult | null
  onRemoveRole: (n: string) => void; onRemoveStep: (i: number) => void; onDupStep: (i: number) => void
  onDuringToStep: (i: number, k: number) => void; onStepToDuring: (i: number) => void
}) {
  const ro = !canWrite
  const R = S.roles(doc)
  const kindOf = (r: string) => S.resolvePool(doc, topo, r)?.kind
  const sampleIds = Object.keys(topo?.media?.samples ?? {})
  const mine = issues.filter(i => i.ref && JSON.stringify(i.ref) === JSON.stringify(sel))
  const issueBlock = mine.length ? <div className="mb-2 flex flex-col gap-1">{mine.map((i, k) => <div key={k} className={`rounded-sm border-l-2 px-2 py-1 ${i.lv === 'error' ? 'border-destructive bg-dangersoft/40' : i.lv === 'warning' ? 'border-warning bg-warning-soft/40' : 'border-info bg-info-soft/40'}`}>{i.msg}</div>)}</div> : null
  const head = (title: string, badge: ReactNode) => <div className="mb-2 flex items-center gap-2"><b className="text-sm">{title}</b>{badge}</div>

  if (sel.kind === 'scenario') {
    const { pre, epi } = S.phases(doc)
    return <div>
      {head('시나리오', <Badge variant={source === 'user' ? 'brandSoft' : 'neutralSoft'}>{source === 'user' ? '운영자본' : source === 'bundled' ? '동봉' : '새 문서'}</Badge>)}{issueBlock}
      <F label="id" help="대문자·숫자·하이픈 3~64. 보고서·run 색인의 키 — 저장 경로 id 와 같아야 한다"><Txt value={doc.id} mono disabled={ro} onCommit={v => mutate(d => { d.id = v.trim() })} /></F>
      <F label="title"><Txt value={doc.title} disabled={ro} onCommit={v => mutate(d => { d.title = v.trim() || undefined })} /></F>
      <F label="tags (쉼표)" help="목록 필터 — volte · ptt · trunk · …"><Txt value={(doc.tags ?? []).join(', ')} mono disabled={ro} onCommit={v => mutate(d => { d.tags = v.split(',').map(x => x.trim()).filter(Boolean) })} /></F>
      <F label="머리 주석" help="YAML 첫 줄 # 주석 — 재직렬화 때 보존"><Area value={doc.comment ?? ''} disabled={ro} onCommit={v => mutate(d => { d.comment = v })} /></F>
      <Sec title="대상 증거 (2차 판정)" right={<Button variant="ghost" size="sm" disabled={ro} onClick={() => mutate(d => { d.target_evidence = [...(d.target_evidence ?? []), { kind: 'recording_created', min: 1 }] })}><Plus size={12} /> 추가</Button>}>
        {(doc.target_evidence ?? []).map((e, k) => <div key={k} className="grid grid-cols-[1fr_60px_60px_24px] gap-1">
          <Sel value={e.kind} disabled={ro} options={(vocab?.evidence_kinds ?? ['recording_created', 'log_errors', 'alarm_raised', 'event_logged', 'rss_growth_mb', 'fd_growth']).map(v => ({ v }))} onChange={v => mutate(d => { const x = d.target_evidence![k]; x.kind = v; if (!/growth/.test(v)) delete x.proc; if (!/alarm_raised|event_logged/.test(v)) delete x.code })} />
          {/growth/.test(e.kind) && <Txt value={e.proc ?? ''} mono placeholder="proc" disabled={ro} onCommit={v => mutate(d => { const x = d.target_evidence![k]; if (v.trim() === '') delete x.proc; else x.proc = v.trim() })} />}
          {/alarm_raised|event_logged/.test(e.kind) && <Txt value={e.code ?? ''} mono placeholder="code" disabled={ro} onCommit={v => mutate(d => { const x = d.target_evidence![k]; if (v.trim() === '') delete x.code; else x.code = v.trim() })} />}
          <Txt value={e.min} mono type="number" placeholder="min" disabled={ro} onCommit={v => mutate(d => { const x = d.target_evidence![k]; if (v === '') delete x.min; else x.min = Number(v) })} />
          <Txt value={e.max} mono type="number" placeholder="max" disabled={ro} onCommit={v => mutate(d => { const x = d.target_evidence![k]; if (v === '') delete x.max; else x.max = Number(v) })} />
          <button disabled={ro} onClick={() => mutate(d => { d.target_evidence!.splice(k, 1) })}><X size={12} /></button></div>)}
        {!(doc.target_evidence ?? []).length && <span className="text-muted-foreground">없음 — 발생기 측 관측(1차)만으로 판정</span>}
        {(doc.target_evidence ?? []).some(e => /growth/.test(e.kind)) && <span className="text-muted-foreground">rss_growth_mb / fd_growth = 호스트 SSH 관측(hosts.*.ssh + nodes.*.procs)의 프로세스별 처음↔끝 차 — soak 프로파일과 짝. proc 은 "host/proc" 또는 이름(비면 최댓값)</span>}
      </Sec>
      <Sec title="구간 요약"><div className="grid grid-cols-3 gap-1 text-center"><div><div className="text-muted-foreground">prelude</div><b>{pre}</b></div><div><div className="text-muted-foreground">body</div><b>{epi - pre}</b></div><div><div className="text-muted-foreground">epilogue</div><b>{doc.flow.length - epi}</b></div></div></Sec>
      <div className="mt-3 text-muted-foreground"><ul className="list-disc pl-4"><li>레인 헤더 클릭 = 역할 속성</li><li>행 클릭 = 단계 속성</li><li>빈 곳 클릭 = 시나리오 속성</li></ul></div>
    </div>
  }
  if (sel.kind === 'role') {
    const n = sel.id; const r = doc.roles[n]; if (!r) { setSel({ kind: 'scenario' }); return null }
    const res = S.resolvePool(doc, topo, n); const same = R.filter(x => x !== n && doc.roles[x].pool === r.pool)
    const opts = [...new Set([...Object.keys(topo?.pools ?? {}), ...Object.values(topo?.pools ?? {}).map(p => p.group).filter((x): x is string => !!x)])]
    const pr = plan?.roles?.[n]
    return <div>
      {head(`역할 ${n}`, <Badge variant={res ? (res.kind === 'peer' ? 'warningSoft' : 'infoSoft') : 'dangerSoft'}>{res ? S.roleKindTag(doc, topo, n) : '풀 없음'}</Badge>)}{issueBlock}
      <F label="이름" help="바꾸면 flow 의 참조도 같이 바뀐다"><Txt value={n} mono disabled={ro} onCommit={v => mutate(d => { if (S.renameRole(d, n, v.trim())) setSel({ kind: 'role', id: v.trim() }) })} /></F>
      <F label={<>pool <span className="text-muted-foreground">토폴로지 {topo?.name ?? '(없음)'} 의 풀 이름 또는 group</span></>}><Txt value={r.pool} mono list="tester-pool-list" disabled={ro} onCommit={v => mutate(d => { d.roles[n].pool = v.trim() })} /><datalist id="tester-pool-list">{opts.map(o => <option key={o} value={o}>{topo?.pools[o] ? `${topo.pools[o].kind} · ${topo.pools[o].worker}` : `group → ${Object.entries(topo?.pools ?? {}).filter(([, p]) => p.group === o).map(([k, p]) => `${k}@${p.worker}`).join(', ')}`}</option>)}</datalist></F>
      <div className="grid grid-cols-2 gap-2">
        <F label="disjoint_from" help="같은 풀의 역할과 신원 창을 겹치지 않게"><Sel value={r.disjoint_from} disabled={ro} options={[...same, ...(r.disjoint_from && !same.includes(r.disjoint_from) ? [r.disjoint_from] : [])].map(v => ({ v }))} empty="(없음)" onChange={v => mutate(d => { if (v) d.roles[n].disjoint_from = v; else delete d.roles[n].disjoint_from })} /></F>
        <F label="count" help="생략 = 전체 (disjoint 면 균등 분할)"><Txt value={r.count} mono type="number" placeholder="풀 전체" disabled={ro} onCommit={v => mutate(d => { if (v === '') delete d.roles[n].count; else d.roles[n].count = Math.max(1, Number(v)) })} /></F>
      </div>
      <label className="mt-1 inline-flex items-center gap-1"><Checkbox checked={!!r.multi} disabled={ro} onCheckedChange={v => mutate(d => { if (v) d.roles[n].multi = true; else delete d.roles[n].multi })} /> multi — 인스턴스마다 단말 여럿 <span className="text-muted-foreground">(그룹 세션: 단일 역할이 잡고 남은 그룹 멤버 전부)</span></label>
      <label className="mt-1 inline-flex items-center gap-1"><Checkbox checked={r.member === false} disabled={ro} onCheckedChange={v => mutate(d => { if (v) d.roles[n].member = false; else delete d.roles[n].member })} /> 그룹 밖 (member: false) <span className="text-muted-foreground">(그룹 세션: 잡은 그룹의 비멤버 PTT 단말 — 청취 관제사·비멤버 거절)</span></label>
      <Sec title="토폴로지 해석" right={topo && <Badge variant="neutralSoft">{topo.name}</Badge>}>
        {res ? <div className="flex flex-col gap-0.5">
          <div className="flex justify-between"><span className="text-muted-foreground">풀</span><span className="font-mono">{Object.entries(res.pools).map(([k, p]) => `${k}@${p.worker}`).join(', ')}{res.byName ? '' : ' (group)'}</span></div>
          <div className="flex justify-between"><span className="text-muted-foreground">kind</span><span>{res.kind}{(Object.values(res.pools)[0] as { profile?: string }).profile ? ` · ${(Object.values(res.pools)[0] as { profile?: string }).profile}` : ''}{(Object.values(res.pools)[0] as { answer?: string }).answer === 'silent' ? ' · silent' : ''}</span></div>
          {pr && <><div className="flex justify-between"><span className="text-muted-foreground">신원 (이 역할 창)</span><span className="font-mono">{fmtNum(pr.total, 0)}</span></div><div className="flex justify-between"><span className="text-muted-foreground">워커</span><span className="font-mono">{Object.entries(pr.workers).map(([w, [p, b, e]]) => `${w}:${p}[${b},${e})`).join(' ')}</span></div></>}
          {!pr && <span className="text-muted-foreground">[토폴로지 적합성] 탭을 열면 워커·신원 창이 계산됩니다</span>}
        </div> : <span className="text-muted-foreground">{topo ? `풀 '${r.pool}' 을 토폴로지에서 찾을 수 없다 — 이름 또는 group` : '기준 토폴로지를 고르십시오'}</span>}
      </Sec>
      <div className="mt-4 border-t border-border pt-2"><Button variant="destructive" size="sm" disabled={ro} onClick={() => onRemoveRole(n)}><Trash2 size={12} /> 역할 삭제</Button></div>
    </div>
  }
  if (sel.kind === 'sub') {
    const s = doc.flow[sel.idx]; const d = s?.during?.[sel.k]; if (!d) { setSel({ kind: 'scenario' }); return null }
    const D = (fn: (x: During) => void) => mutate(x => fn(x.flow[sel.idx].during![sel.k]))
    const len = S.secondsOf(s, bind)
    return <div>
      {head(`during ${d.step}`, <Badge variant="neutralSoft">#{sel.idx + 1} 통화 유지 안</Badge>)}{issueBlock}
      <div className="grid grid-cols-2 gap-2">
        <F label="at_s" help={`확립 뒤 경과 초 (0 ~ ${len})`}><Txt value={d.at_s} mono type="number" disabled={ro} onCommit={v => D(x => { x.at_s = Math.max(0, Number(v)) })} /></F>
        <F label="동작"><Sel value={d.step} disabled={ro} options={(vocab?.during_steps ?? ['dtmf', 'hold', 'resume', 'refer']).map(v => ({ v }))} onChange={v => D(x => { x.step = v as During['step']; if (v !== 'dtmf') delete x.payload; else x.payload = x.payload ?? '1234#'; if (v !== 'refer') delete x.to; else x.to = x.to ?? R.find(r => r !== x.from)
          if (S.MEDIA_CTL.has(v)) { if (!x.who?.length) x.who = [x.from ?? R[0]].filter(Boolean); delete x.from } else { if (!x.from) x.from = x.who?.[0] ?? R[0]; delete x.who }
          if (v !== 'media_send') { delete x.sample; delete x.loop } })} /></F>
      </div>
      {S.MEDIA_CTL.has(d.step)
        ? <F label="행위자 (who — 여럿)"><Chips roles={R} selected={d.who ?? []} multi disabled={ro} kindOf={kindOf} onToggle={r => D(x => { const w = new Set(x.who ?? []); if (w.has(r)) w.delete(r); else w.add(r); x.who = R.filter(n => w.has(n)) })} /></F>
        : <F label="행위자 (from)"><Chips roles={R} selected={[d.from ?? ''].filter(Boolean)} multi={false} disabled={ro} kindOf={kindOf} gate={d.step === 'refer' ? r => kindOf(r) === 'peer' : undefined} onToggle={r => D(x => { if (r) x.from = r; else delete x.from })} /></F>}
      {d.step === 'media_send' && <SampleFields sample={d.sample} loop={d.loop} samples={sampleIds} disabled={ro} onSample={v => D(x => { if (v) x.sample = v; else delete x.sample })} onLoop={v => D(x => { if (v) delete x.loop; else x.loop = false })} />}
      {d.step === 'dtmf' && <F label="payload (0-9 * # A-D)"><Txt value={d.payload} mono disabled={ro} onCommit={v => D(x => { x.payload = v })} /></F>}
      {d.step === 'refer' && <F label="전달 대상 (to)"><Chips roles={R} selected={[d.to ?? ''].filter(Boolean)} multi={false} disabled={ro} kindOf={kindOf} onToggle={r => D(x => { if (r) x.to = r; else delete x.to })} /></F>}
      <div className="mt-4 flex flex-wrap gap-2 border-t border-border pt-2"><Button variant="outline" size="sm" onClick={() => setSel({ kind: 'step', idx: sel.idx })}>단계로</Button><Button variant="outline" size="sm" disabled={ro} onClick={() => onDuringToStep(sel.idx, sel.k)} title="통화 유지 단계 바로 뒤의 독립 행으로 뺍니다 (마커를 행 사이로 끌어도 됨)">행으로 빼기</Button><Button variant="destructive" size="sm" disabled={ro} onClick={() => mutate(x => { x.flow[sel.idx].during!.splice(sel.k, 1); setSel({ kind: 'step', idx: sel.idx }) })}><Trash2 size={12} /> 동작 삭제</Button></div>
    </div>
  }
  // step
  const i = sel.idx; const s = doc.flow[i]; if (!s) { setSel({ kind: 'scenario' }); return null }
  const D = vocab?.steps[s.step]
  const ST = (fn: (x: Step) => void) => mutate(x => fn(x.flow[i]))
  const kindGate = D?.kind === 'peer' ? (r: string) => kindOf(r) === 'peer' : D?.kind === 'ue' ? (r: string) => kindOf(r) !== 'peer'
    : D?.kind === 'ptt' ? (r: string) => { const x = S.resolvePool(doc, topo, r); return !x || ((x.kind === 'ue' || x.kind === 'real-ue') && x.service === 'ptt') } : undefined
  // 실단말(real-ue) 역할은 vocab.steps[*].real 이 참인 단계만 행위자가 된다(cimsue-cli drive 명령이 있는 것 — test_instrument.md §3.3)
  const gate = D && D.real === false ? (r: string) => (kindGate ? kindGate(r) : true) && S.resolvePool(doc, topo, r)?.kind !== 'real-ue' : kindGate
  const M = S.multiRoles(doc)
  const isBind = typeof s.seconds === 'string' && /^\$\{/.test(s.seconds)
  const suggested = D?.metrics ?? []; const allMetrics = Object.keys(vocab?.metrics ?? {})
  const thr = vocab?.thresholds ?? ['p50', 'p95', 'p99', 'max', 'min']
  return <div>
    {head(`#${i + 1} ${s.step}`, <Badge variant={D?.supported === false ? 'warningSoft' : 'neutralSoft'}>{S.phaseOf(doc, i)}{D?.supported === false ? ' · 워커 미지원' : ''}</Badge>)}{issueBlock}
    <div className="mb-1 text-muted-foreground">{D?.desc}</div>
    <F label="종류"><Sel value={s.step} disabled={ro} options={Object.entries(vocab?.steps ?? {}).map(([k, d]) => ({ v: k, l: `${k}${d.supported ? '' : ' (미지원)'}` }))} onChange={v => ST(x => { x.step = v })} /></F>
    {(D?.actor === 'who' || D?.actor === 'from' || D?.actor === 'fromto') && <>
      {D.actor === 'who' ? <F label="행위자 (who — 여럿)"><Chips roles={R} selected={s.who ?? []} multi disabled={ro} kindOf={kindOf} gate={D.kind === 'ue|trunk' ? r => { const x = S.resolvePool(doc, topo, r); return !x || x.kind !== 'peer' || !!(Object.values(x.pools)[0] as { register?: unknown }).register } : gate} onToggle={r => ST(x => { const w = new Set(x.who ?? []); if (w.has(r)) w.delete(r); else w.add(r); x.who = [...w] })} /></F>
        : <F label="행위자 (from)"><Chips roles={R} selected={[s.from ?? ''].filter(Boolean)} multi={false} disabled={ro} kindOf={kindOf} gate={gate} onToggle={r => ST(x => { if (r) x.from = r; else delete x.from })} /></F>}
      {(D.actor === 'fromto' || s.step === 'invite' || s.step === 'subscribe') && <F label={s.step === 'refer' ? '전달 대상 (to)' : s.step === 'group_call' ? '합류 대기 (to — multi 역할, 선택)' : s.step === 'pickup' ? '지정 픽업 대상 (to — 선택, 생략 = 그룹 픽업)' : s.step === 'subscribe' ? '감시 대상 (to — 선택, 생략 = 자기 AoR)' : s.step === 'replaces' || s.step === 'join' ? '대상 다이얼로그의 당사자 (to)' : '상대 (to)'}><Chips roles={s.step === 'group_call' ? R.filter(r => M.includes(r)) : R} selected={[s.to ?? ''].filter(Boolean)} multi={false} disabled={ro} kindOf={kindOf} onToggle={r => ST(x => { if (r && x.to !== r) x.to = r; else delete x.to })} /></F>}
      {(s.step === 'invite' || s.step === 'pickup' || s.step === 'subscribe') && <F label={s.step === 'invite' ? '또는 다이얼 번호 (to — 대표번호 리터럴 · ${pilot} 바인딩)' : s.step === 'subscribe' ? '또는 감시할 대표번호 (to 리터럴 · ${pilot} — 그룹원 BLF, F7)' : '또는 링잉 대표번호 (to 리터럴 · ${pilot})'} help="역할이 아닌 번호를 부른다 — TS 24.239 대표번호 포크: 인스턴스의 다른 UE 역할(그룹원)이 착신, 승자 외 CANCEL 은 정상(fork_alert_pct)"><Txt value={s.to && !R.includes(s.to) ? s.to : ''} mono placeholder="${pilot}" disabled={ro} onCommit={v => ST(x => { const t = v.trim(); if (t) x.to = t; else if (x.to && !R.includes(x.to)) delete x.to })} /></F>}
    </>}
    {(s.step === 'answer' || s.step === 'progress' || s.step === 'reject' || s.step === 'invite' || S.MEDIA_CTL.has(s.step)) && <F label="after_ms" help="직전 이벤트 뒤 지연(ms)"><Txt value={s.after_ms} mono type="number" placeholder="0" disabled={ro} onCommit={v => ST(x => { if (v === '') delete x.after_ms; else x.after_ms = Math.max(0, Number(v)) })} /></F>}
    {D?.actor === 'seconds' && <div className="grid grid-cols-[1fr_auto] items-end gap-2">
      <F label={isBind ? '바인딩 변수' : 'seconds'}><Txt value={isBind ? String(s.seconds).slice(2, -1) : s.seconds} mono type={isBind ? undefined : 'number'} disabled={ro} onCommit={v => ST(x => { x.seconds = isBind ? `\${${v.trim()}}` : Number(v) })} /></F>
      <Button variant="outline" size="sm" disabled={ro} onClick={() => ST(x => { x.seconds = isBind ? (Number(bind.ht) || 10) : '${ht}' })}>{isBind ? '고정값으로' : '바인딩 ${ht}'}</Button>
    </div>}
    {s.step === 'pickup' && <F label="피처코드 (payload)" help="접속서비스 pickup_feature_code — ${pickup_code} 바인딩을 권장(대상마다 다르다)"><Txt value={s.payload} mono placeholder="${pickup_code}" disabled={ro} onCommit={v => ST(x => { x.payload = v.trim() })} /></F>}
    {s.step === 'check' && <><F label="판정 종류 (payload)" help="conference_roster_visible|hidden = who 의 conference NOTIFY 로스터에 to 역할이 있는가/없는가(listen_visibility) · conference_warning_138 = who 의 conference SUBSCRIBE 거절 Warning 138 · dialog_consistent = who 가 받은 dialog NOTIFY 열 정합(RFC 4235 — F7). ${var} 바인딩도 된다"><Sel value={s.payload} disabled={ro} options={(vocab?.check_kinds ?? ['conference_roster_visible', 'conference_roster_hidden', 'conference_warning_138', 'dialog_consistent']).map(v => ({ v }))} empty="(선택)" onChange={v => ST(x => { if (v) x.payload = v; else delete x.payload })} /></F>
      {(s.payload ?? '').startsWith('conference_roster_') && <F label="로스터에서 찾을 역할 (to)"><Chips roles={R} selected={[s.to ?? ''].filter(Boolean)} multi={false} disabled={ro} kindOf={kindOf} onToggle={r => ST(x => { if (r && x.to !== r) x.to = r; else delete x.to })} /></F>}</>}
    {s.step === 'subscribe' && <F label="이벤트 패키지 (payload)" help="RFC 6665 event-type — 기본 dialog(RFC 4235) · conference(RFC 4575 — 그룹 AoR, group 또는 인스턴스 그룹; 인가 판정 200/403 Warning 138). 미지 패키지는 489 를 기대치로"><Txt value={s.payload} mono placeholder="dialog" disabled={ro} onCommit={v => ST(x => { if (v.trim()) x.payload = v.trim(); else delete x.payload })} /></F>}
    {s.step === 'publish' && <F label="affiliation 명령 (payload)" help="TS 24.379 §9 — affiliate(Expires 3600) · deaffiliate(Expires 0)"><Sel value={s.payload ?? 'affiliate'} disabled={ro} options={S.PUBLISH_COMMANDS.map(v => ({ v }))} onChange={v => ST(x => { if (v === 'affiliate') delete x.payload; else x.payload = v })} /></F>}
    {s.step === 'subscribe' && s.payload === 'conference' && <F label="group" help="conference 구독 대상 그룹 — 생략 = 인스턴스가 잡은 그룹(그룹 세션)"><Txt value={(s as Step).group} mono placeholder="(인스턴스 그룹)" disabled={ro} onCommit={v => ST(x => { if (v.trim()) (x as Step).group = v.trim(); else delete (x as Step).group })} /></F>}
    {(s.step === 'group_call' || s.step === 'publish') && <F label="group" help={s.step === 'publish' ? 'affiliation 대상 MCPTT 그룹 id — 생략 = 신원의 그룹' : 'MCPTT 그룹 id 직접 지정 — 생략 = 인스턴스가 잡은 그룹(발신 멤버의 affiliation 그룹)'}><Txt value={s.group} mono placeholder="(인스턴스 그룹)" disabled={ro} onCommit={v => ST(x => { if (v.trim()) x.group = v.trim(); else delete x.group })} /></F>}
    {s.step === 'group_call' && <F label="합류 방식 (payload)" help="비면 멤버 개시(fan-out) · listen = 그룹 밖 역할(member: false)의 a=recvonly 청취 합류 — 진행 중 세션 뒤에, 자격·범위 인가는 대상(dispatch_center.md §5.6)"><Sel value={s.payload ?? ''} disabled={ro} options={S.GROUP_CALL_MODES.map(v => ({ v }))} empty="(멤버 개시)" onChange={v => ST(x => { if (v) x.payload = v; else delete x.payload })} /></F>}
    {s.step === 'floor_request' && <F label="기대 결과 (payload)" help="granted = 허가(기본) · denied = 거절 · queued = 큐 · any = 결과만 나오면 됨(동시 요청 경합)"><Sel value={s.payload ?? 'granted'} disabled={ro} options={S.FLOOR_OUTCOMES.map(v => ({ v }))} onChange={v => ST(x => { if (v === 'granted') delete x.payload; else x.payload = v })} /></F>}
    {(s.step === 'invite' || s.step === 'group_call') && <div className="grid grid-cols-3 gap-2">
      <F label="audio"><Sel value={s.media?.audio ?? 'amr-wb'} disabled={ro} options={(vocab?.audio ?? ['amr-wb', 'amr', 'pcmu', 'pcma', 'g722']).map(v => ({ v }))} onChange={v => ST(x => { x.media = { ...(x.media ?? {}), audio: v } })} /></F>
      <F label="video"><Sel value={s.media?.video} disabled={ro} options={[{ v: 'h264' }]} empty="없음" onChange={v => ST(x => { x.media = { ...(x.media ?? {}) }; if (v) x.media.video = v; else delete x.media.video })} /></F>
      <F label="rtp" help="auto = SDP 교환 즉시 송출 · none = 시그널링 전용(RTP 없음) · explicit = media_send 가 부를 때만 송출(수신은 시작)"><Sel value={s.media?.rtp ?? 'auto'} disabled={ro} options={(vocab?.rtp_modes ?? ['auto', 'none', 'explicit']).map(v => ({ v }))} onChange={v => ST(x => { x.media = { ...(x.media ?? {}) }; if (v && v !== 'auto') x.media.rtp = v as 'none' | 'explicit'; else delete x.media.rtp })} /></F>
    </div>}
    {s.step === 'media_send' && <SampleFields sample={s.sample} loop={s.loop} samples={sampleIds} disabled={ro} onSample={v => ST(x => { if (v) x.sample = v; else delete x.sample })} onLoop={v => ST(x => { if (v) delete x.loop; else x.loop = false })} />}
    {s.step === 'dtmf' && <F label="payload (0-9 * # A-D)"><Txt value={s.payload} mono disabled={ro} onCommit={v => ST(x => { x.payload = v })} /></F>}
    {s.step === 'sds_send' && <><F label="SDS 본문 (payload)" help="TS 24.282 DATA PAYLOAD TEXT. to 역할 = 1:1 · to 없음 = 그룹 SDS(인스턴스가 잡은 그룹 또는 group — 수신자는 multi 역할)"><Txt value={s.payload} disabled={ro} onCommit={v => ST(x => { x.payload = v })} /></F>
      <F label="group" help="그룹 SDS 의 그룹 id 직접 지정 — 생략 = 인스턴스가 잡은 affiliation 그룹(to 가 있으면 무시)"><Txt value={(s as Step).group} mono placeholder="(인스턴스 그룹)" disabled={ro} onCommit={v => ST(x => { if (v.trim()) (x as Step).group = v.trim(); else delete (x as Step).group })} /></F>
      <F label="plane" help="control = SIP MESSAGE(C-plane) · media = MSRP media plane(TS 24.282 §9.2.3 — INVITE m=message → cmdp 종단, 수신자는 풀 msrp 면 MSRP 배포·아니면 FILEURL 폴백). 대상 CSP 는 그룹 SDS 만 media plane 을 받는다"><Sel value={s.plane ?? 'control'} disabled={ro} options={[{ v: 'control', l: 'control — SIP MESSAGE' }, { v: 'media', l: 'media — MSRP (대용량)' }]} onChange={v => ST(x => { if (v === 'media') x.plane = 'media'; else delete x.plane })} /></F>
      <label className="inline-flex items-center gap-1"><Checkbox checked={!!s.disposition} disabled={ro} onCheckedChange={v => ST(x => { if (v === true) x.disposition = true; else delete x.disposition })} /> disposition(delivery) 요청 — 수신 단말의 SDS NOTIFICATION 회신(sds_disposition_pct)</label></>}
    {s.step === 'reject' && <F label="응답 코드 (payload)"><Txt value={s.payload} mono placeholder="486" disabled={ro} onCommit={v => ST(x => { x.payload = v })} /></F>}
    {(s.step === 'bye' || s.step === 'reject') && <F label="cause — Reason: Q.850 (RFC 3326, 피어만)"><Sel value={s.cause != null ? String(s.cause) : ''} disabled={ro} options={Object.entries(vocab?.q850 ?? {}).map(([k, l]) => ({ v: k, l: `${k} ${l}` }))} empty="(없음)" onChange={v => ST(x => { if (v) x.cause = Number(v); else delete x.cause })} /></F>}
    {s.step === 'media_hold' && <Sec title="통화 중 동작 (during)" right={<Button variant="ghost" size="sm" disabled={ro} onClick={() => ST(x => { x.during = [...(x.during ?? []), { at_s: 0, step: 'dtmf', from: R[0], payload: '1234#' }] })}><Plus size={12} /> 동작</Button>}>
      {(s.during ?? []).map((d, k) => <button key={k} onClick={() => setSel({ kind: 'sub', idx: i, k })} className="flex items-center gap-2 rounded-sm border border-border px-2 py-1 text-left hover:bg-accent"><span className="font-mono">+{d.at_s}s</span><b>{d.step}</b><span className="truncate text-muted-foreground">{d.from ?? (d.who ?? [])[0]}{d.payload ? ` ${d.payload}` : ''}{d.to ? ` → ${d.to}` : ''}</span></button>)}
      {!(s.during ?? []).length && <span className="text-muted-foreground">없음 — 팔레트의 dtmf/hold/resume/refer 를 유지 바 위에 놓거나 [+ 동작]. RTP 표본은 마지막 조각에서 판정</span>}
    </Sec>}
    <Sec title="기대치 (expect)" right={<Sel value="" disabled={ro} options={[...suggested, ...allMetrics.filter(m => !suggested.includes(m))].filter(m => !(s.expect ?? {})[m]).map(m => ({ v: m, l: `${suggested.includes(m) ? '★ ' : ''}${m} — ${(vocab?.metrics[m] ?? '').split(' — ').pop()}` }))} empty="+ 지표" onChange={v => { if (v) ST(x => { x.expect = { ...(x.expect ?? {}), [v]: v === 'code' ? 200 : v.endsWith('_pct') ? (v === 'rtp_loss_pct' ? { max: 0.5 } : 99) : { p95: 1000 } } }) }} />}>
      {Object.entries(s.expect ?? {}).map(([m, e]) => <div key={m} className="rounded-sm border border-border p-1.5">
        <div className="flex items-center gap-1"><b className="font-mono">{m}</b><span className="truncate text-muted-foreground">{vocab?.metrics[m]}</span><button disabled={ro} className="ml-auto" onClick={() => ST(x => { delete x.expect![m] })}><X size={12} /></button></div>
        {typeof e === 'number' || m === 'code' || (vocab?.pct_metrics ?? []).includes(m) && typeof e !== 'object' ? <Txt value={e as number} mono type="number" disabled={ro} onCommit={v => ST(x => { x.expect![m] = Number(v) })} />
          : <div className="grid grid-cols-5 gap-1">{thr.map(q => <F key={q} label={q}><Txt value={(e as Record<string, number>)[q]} mono type="number" disabled={ro} onCommit={v => ST(x => { const o = { ...(x.expect![m] as Record<string, number>) }; if (v === '') delete o[q]; else o[q] = Number(v); x.expect![m] = Object.keys(o).length ? o : { p95: 0 } })} /></F>)}</div>}
      </div>)}
      {!Object.keys(s.expect ?? {}).length && <span className="text-muted-foreground">없음 — ★ 는 이 단계에 맞는 지표</span>}
    </Sec>
    <div className="mt-4 flex flex-wrap gap-2 border-t border-border pt-2"><Button variant="outline" size="sm" disabled={ro} onClick={() => onDupStep(i)}><Copy size={12} /> 복제</Button>{S.DURING_OK.has(s.step) && <Button variant="outline" size="sm" disabled={ro} onClick={() => onStepToDuring(i)} title="가장 가까운 통화 유지(media_hold) 단계의 during 으로 옮깁니다 (행을 유지 바 위로 끌어도 됨)">during 으로</Button>}<Button variant="destructive" size="sm" disabled={ro} onClick={() => onRemoveStep(i)}><Trash2 size={12} /> 단계 삭제</Button></div>
  </div>
}
