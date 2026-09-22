// 시험 > 토폴로지 — 왼쪽 레일 한 열에 탭 둘([토폴로지] 레코드 목록: 검색·kind 칩·행 = 이름/대상/워커/갱신, [새 토폴로지…] 프리셋 선택 /
// [팔레트]: 캔버스가 포털로 그린다 — 레코드를 고르면 팔레트 탭으로 넘어간다) + 툴바(이름 · 대상 배지 · 검증 배지(누르면 속성 패널의 검증 목록) ·
// 마지막 연결 검사 배지(누르면 드로어) · 삭제 · 실행취소/다시실행 · [⋯] 자동 배치/되돌리기/레코드 JSON · 연결 검사 · 저장/생성) + 캔버스 편집기(TopologyCanvas,
// 레코드마다 key 로 다시 만들어 열 때 화면 맞춤). 레코드 JSON 은 Modal(940×76vh) 안의 TopologyJsonEditor.
// 레코드는 컨트롤러 `topology` 스키마(호스트›워커·대상 노드›풀)로 저장 전 검증하고, 카드 위치·영역 크기는 레코드 layout 에 같이 저장한다.
// 문서 이력은 useDocHistory(Ctrl+Z/Y) — 레코드 전환·저장 뒤 비운다. 변경이 있으면 전환·새로 만들기 전에 묻고, 탭 닫기도 경고한다.
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { RefreshCw, Plus, Save, Trash2, RotateCcw, PlugZap, LayoutGrid, Undo2, Redo2, MoreHorizontal, Braces, List, Shapes } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger } from '@core/components/ui/select'
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '@core/components/ui/dropdown-menu'
import Modal from '@core/components/Modal'
import { EmptyState } from '@core/components/custom/empty-state'
import { useToast } from '@core/components/Toast'
import { useConfirm } from '@core/components/custom/confirm'
import { useAuth } from '@core/contexts/AuthContext'
import { hasRole } from '@core/utils/permissions'
import { testerApi, type TopologyRow, type TopologyDoc, type WorkerRow, type CheckItem } from '@tester/api/tester'
import TopologyCanvas, { TopologyJsonEditor, type TopologyCanvasHandle } from '@tester/components/topology/TopologyCanvas'
import ListRail, { RailRow } from '@tester/components/ListRail'
import * as M from '@tester/lib/topology-model'
import { fmtTime } from '@tester/lib/fmt'
import { useDocHistory, useUndoKeys, useUnsavedGuard } from '@tester/lib/use-history'

/** 표시용 — SIP 접속점 노드 첫 항목의 호스트 주소·기본 도메인 */
export function topoTargetLabel(doc: TopologyDoc): string {
  const sip = Object.values(doc.target?.nodes ?? {}).find(n => n.role === 'sip' && M.accessListeners(n).length)
  if (!sip) return doc.target?.name ?? '—'
  const ip = sip.addr || (doc.hosts?.[sip.host]?.ip ?? sip.host)
  const dom = sip.sip?.domains?.[0]
  return dom ? `${ip} · ${dom}` : ip
}

const NEW = '__new__'
type PresetKey = keyof typeof M.PRESETS
const PRESET_LABEL: Record<PresetKey, string> = { cims: 'CIMS 한 호스트 (CSP·CMP·CSC·OAM·DB)', ims: '일반 IMS 분리 배치 (P/S-CSCF·IBCF·TAS·MRF·HSS)', pbx: 'IP-PBX (PBX + PBX 미디어)' }

export default function TesterTopologiesPage() {
  const { show } = useToast()
  const confirm = useConfirm()
  const { user } = useAuth()
  const canWrite = hasRole(user, 'operator')
  const canDelete = hasRole(user, 'manager')
  const [rows, setRows] = useState<TopologyRow[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const [sel, setSel] = useState<number | typeof NEW | null>(null)
  const [preset, setPreset] = useState<PresetKey>('cims')
  const [q, setQ] = useState('')
  const [kindChip, setKindChip] = useState<'' | 'cims' | 'ims' | 'pbx'>('')
  const H = useDocHistory<TopologyDoc>()
  const doc = H.doc
  const [orig, setOrig] = useState('')
  const [saving, setSaving] = useState(false)
  const [workers, setWorkers] = useState<WorkerRow[]>([])
  const [check, setCheck] = useState<{ items: CheckItem[]; at: string } | null>(null)
  const [checking, setChecking] = useState(false)
  const [serverErrs, setServerErrs] = useState<string[]>([])
  const [leftTab, setLeftTab] = useState<'rec' | 'pal'>('rec')
  const [palHost, setPalHost] = useState<HTMLDivElement | null>(null)   // 레일 [팔레트] 탭의 상자 — 캔버스가 여기에 포털
  const [jsonOpen, setJsonOpen] = useState(false)
  const canvasRef = useRef<TopologyCanvasHandle>(null)

  const load = useCallback(async () => {
    setLoading(true)
    try { const r = await testerApi.topologies(); setRows(r.topologies); setError(null); return r.topologies }
    catch (e) { setError(String(e)); return [] } finally { setLoading(false) }
  }, [])
  useEffect(() => { load().then(t => { if (t.length && sel === null) setSel(t[0].id) }) }, [load])   // eslint-disable-line react-hooks/exhaustive-deps

  const current = useMemo(() => (typeof sel === 'number' ? rows.find(r => r.id === sel) ?? null : null), [rows, sel])
  useEffect(() => {
    setCheck(null); setWorkers([]); setServerErrs([])
    if (sel === NEW) { const d = M.fromPreset(preset, null); H.reset(d); setOrig(JSON.stringify(d)); return }
    if (!current) { H.reset(null); return }
    const d = M.deep(current.doc); M.ensureLayout(d); H.reset(d); setOrig(JSON.stringify(d))
    testerApi.workers(current.id).then(w => setWorkers(w.workers)).catch(() => {})
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [sel, current])

  const dirty = doc != null && JSON.stringify(doc) !== orig
  const issues = useMemo(() => (doc ? M.validate(doc) : []), [doc])
  const errN = issues.filter(i => i.level === 'err').length
  const warnN = issues.filter(i => i.level === 'warn').length
  useUndoKeys(H.undo, H.redo, canWrite && !!doc)
  useUnsavedGuard(dirty)

  /** 저장 안 한 변경이 있으면 버릴지 묻는다 */
  const discardOk = async () => !dirty || await confirm({ title: '변경을 버릴까요?', body: '저장하지 않은 편집이 있습니다. 다른 레코드로 옮기면 사라집니다.', confirmLabel: '버리고 이동', tone: 'danger' })
  const pick = async (id: number) => { if (id === sel) { setLeftTab('pal'); return } if (!await discardOk()) return; setSel(id); setLeftTab('pal') }
  const shown = useMemo(() => rows.filter(r => (!kindChip || (r.doc.target?.kind ?? 'cims') === kindChip) && (!q.trim() || `${r.name} ${topoTargetLabel(r.doc)} ${r.doc.target?.name ?? ''}`.toLowerCase().includes(q.trim().toLowerCase()))), [rows, kindChip, q])
  const startNew = async (k: PresetKey) => { if (!await discardOk()) return; setPreset(k); setSel(NEW); setLeftTab('pal') }

  const save = async () => {
    if (!doc) return
    setSaving(true); setServerErrs([])
    try {
      const rec = sel === NEW ? await testerApi.createTopology(doc) : await testerApi.saveTopology(sel as number, doc)
      show(sel === NEW ? '토폴로지 생성' : '저장', 'ok')
      await load(); setSel(rec.id)
    } catch (e) {
      const errs = (e as { data?: { errors?: string[] } })?.data?.errors
      if (errs?.length) setServerErrs(errs)
      show(String(e), 'err')
    } finally { setSaving(false) }
  }
  const del = async () => {
    if (!current) return
    if (!await confirm({ title: '토폴로지 삭제', body: `${current.name} 을 지웁니다. 이 토폴로지로 돌린 run 색인은 남습니다.`, confirmLabel: '삭제', tone: 'danger' })) return
    try { await testerApi.deleteTopology(current.id); show('삭제', 'ok'); setSel(null); H.reset(null); load() } catch (e) { show(String(e), 'err') }
  }
  const runCheck = async () => {
    if (!current) return
    setChecking(true)
    try { const r = await testerApi.checkTopology(current.id); setCheck({ items: r.items, at: new Date().toISOString() }); show(r.ok ? '연결 검사: 전부 도달' : '연결 검사: 미도달 항목 있음 — 카드의 붉은 표시', r.ok ? 'ok' : 'err') }
    catch (e) { show(String(e), 'err') } finally { setChecking(false) }
    testerApi.workers(current.id).then(w => setWorkers(w.workers)).catch(() => {})
  }
  const auto = () => { if (!doc) return; const d = M.deep(doc); M.autoLayout(d, id => { const el = document.querySelector<HTMLElement>(`[data-node="${id}"],[data-worker="${id}"]`); return { w: el?.offsetWidth ?? 250, h: el?.offsetHeight ?? 100 } }); H.set(d) }
  const revert = () => { if (orig) H.set(JSON.parse(orig)) }

  const checkReason = !current ? '저장된 레코드만' : dirty ? '저장 뒤 검사' : null
  const saveReason = !doc ? null : errN ? `오류 ${errN} 해결 뒤` : (!dirty && sel !== NEW) ? '변경 없음' : null

  return (
    <div className="flex h-full flex-col">
      <div className="toolbar flex flex-wrap items-center gap-2.5 border-b border-border bg-muted px-4 py-2">
        <span className="whitespace-nowrap text-md font-semibold text-foreground">토폴로지</span>
        {sel === NEW ? <Badge variant="infoSoft">새 레코드 — 프리셋 {M.PRESETS[preset].name}</Badge> : current && <span className="font-mono text-sm text-muted-foreground">#{current.id}</span>}
        {doc && <Input value={doc.name} onChange={e => H.set({ ...doc, name: e.target.value })} disabled={!canWrite} placeholder="이름" className="h-[28px] w-[160px] text-sm" />}
        {doc && <Badge variant="neutralSoft" title="대상 이름·종류 — 캔버스 빈 곳을 누르면 속성 패널에서 고칩니다. kind 는 시드·검증 규칙을 정합니다">대상 {doc.target?.name || '—'} · {doc.target?.kind ?? 'cims'}</Badge>}
        {doc && <button onClick={() => canvasRef.current?.showIssues()} title="누르면 속성 패널에 검증 목록" className="rounded-md"><Badge variant={errN ? 'dangerSoft' : warnN ? 'warningSoft' : 'successSoft'}>{errN ? `오류 ${errN}` : warnN ? `경고 ${warnN}` : '검증 통과'}</Badge></button>}
        {dirty && <Badge variant="warningSoft">변경됨</Badge>}
        {check && <button onClick={() => canvasRef.current?.showCheck()} title="마지막 연결 검사 — 누르면 결과 드로어" className="rounded-md"><Badge variant={check.items.every(i => i.ok || i.info) ? 'successSoft' : 'dangerSoft'}>검사 {fmtTime(check.at)} · {check.items.filter(i => i.ok || i.info).length}/{check.items.length}</Badge></button>}
        {current?.doc && (current as { migrated_from?: string }).migrated_from && <Badge variant="infoSoft" title="이전 꼴(target.csp/workers[].url) 레코드를 v2 로 승계했다">v1 승계</Badge>}
        {error && <span className="text-sm text-destructive">{error}</span>}
        {serverErrs.map((e, i) => <span key={i} className="text-xs text-destructive">{e}</span>)}
        <div className="ml-auto flex flex-wrap items-center gap-1.5">
          {current && canDelete && <Button variant="outline" size="sm" className="text-destructive" onClick={del} title="이 토폴로지를 지웁니다(확인 창) — 이 토폴로지로 돌린 run 색인은 남습니다"><Trash2 size={13} /> 삭제</Button>}
          <Button variant="ghost" size="iconSm" onClick={H.undo} disabled={!canWrite || !H.canUndo} title="실행취소 (Ctrl+Z)"><Undo2 size={13} /></Button>
          <Button variant="ghost" size="iconSm" onClick={H.redo} disabled={!canWrite || !H.canRedo} title="다시실행 (Ctrl+Y)"><Redo2 size={13} /></Button>
          <DropdownMenu>
            <DropdownMenuTrigger asChild><Button variant="outline" size="iconSm" disabled={!doc} title="더 보기"><MoreHorizontal size={14} /></Button></DropdownMenuTrigger>
            <DropdownMenuContent align="end" className="w-56 text-xs">
              <DropdownMenuItem disabled={!doc || !canWrite} onSelect={auto}><LayoutGrid size={13} /> 자동 배치</DropdownMenuItem>
              <DropdownMenuItem disabled={!dirty} onSelect={revert}><RotateCcw size={13} /> 저장 시점으로 되돌리기</DropdownMenuItem>
              <DropdownMenuItem disabled={!doc} onSelect={() => setJsonOpen(true)}><Braces size={13} /> 레코드 JSON…</DropdownMenuItem>
            </DropdownMenuContent>
          </DropdownMenu>
          <Button variant="outline" size="sm" onClick={runCheck} disabled={!!checkReason || checking || !canWrite} title={checkReason ?? '저장된 레코드를 수신점 단위로 확인합니다'}><PlugZap size={13} /> {checking ? '검사 중…' : '연결 검사'}</Button>
          <Button variant="default" size="sm" onClick={save} disabled={!canWrite || saving || !doc || !!saveReason} title={saveReason ?? undefined}><Save size={13} /> {sel === NEW ? '생성' : '저장'}</Button>
        </div>
      </div>
      <div className="flex min-h-0 flex-1">
        <ListRail storageKey="tester-topo-rail" label="토폴로지" search={{ value: q, onChange: setQ, placeholder: '이름·대상·주소 검색' }}
                  tab={leftTab} onTab={k => setLeftTab(k as 'rec' | 'pal')}
                  tabs={[
                    { key: 'rec', label: '토폴로지', icon: <List size={13} />, badge: <Badge variant="neutralSoft">{rows.length}</Badge> },
                    { key: 'pal', label: '팔레트', icon: <Shapes size={13} />, content: doc ? <div ref={setPalHost} className="min-h-0 flex-1" /> : <div className="p-3 text-xs text-muted-foreground">레코드를 열면 여기에 팔레트가 나옵니다 — 호스트·워커·풀·대상 노드를 캔버스로 끌어 놓거나 클릭해 추가합니다</div> },
                  ]}
                  chips={<>{([['', '전체'], ['cims', 'cims'], ['ims', 'ims'], ['pbx', 'pbx']] as const).map(([c, l]) => (
                    <button key={c} onClick={() => setKindChip(c)} className={`h-6 rounded-sm border px-2 text-xs ${kindChip === c ? 'border-primary bg-primary text-primary-foreground' : 'border-border text-muted-foreground hover:bg-accent'}`}>{l}</button>))}
                    <Button variant="ghost" size="iconSm" className="ml-auto" onClick={() => load()} disabled={loading} title="목록 새로고침"><RefreshCw size={13} /></Button></>}
                  action={<Select value="" onValueChange={v => startNew(v as PresetKey)} disabled={!canWrite}>
                    <SelectTrigger className="h-[28px] w-full text-sm"><span className="inline-flex items-center gap-1"><Plus size={13} /> 새 토폴로지…</span></SelectTrigger>
                    <SelectContent>{(Object.keys(M.PRESETS) as PresetKey[]).map(k => <SelectItem key={k} value={k}>{PRESET_LABEL[k]}</SelectItem>)}</SelectContent>
                  </Select>}>
          {sel === NEW && <RailRow selected top={<><Badge variant="infoSoft">새</Badge><span className="truncate font-medium">{doc?.name || '(이름 없음)'}</span></>} bottom={<span>프리셋 {M.PRESETS[preset].name} — 저장하면 목록에 들어갑니다</span>} onClick={() => {}} />}
          {shown.length === 0 && sel !== NEW && <div className="p-3 text-xs text-muted-foreground">{rows.length ? '검색 결과 없음' : '토폴로지 없음 — [새 토폴로지…]'}</div>}
          {shown.map(r => (
            <RailRow key={r.id} selected={r.id === sel} onClick={() => pick(r.id)} title={`#${r.id} · ${r.doc.target?.name ?? ''}`}
                     top={<><span className="truncate font-medium">{r.name}</span>{r.id === sel && dirty && <Badge variant="warningSoft">변경됨</Badge>}<span className="ml-auto whitespace-nowrap font-mono text-muted-foreground">{r.doc.target?.kind ?? 'cims'}</span></>}
                     bottom={<><span className="truncate font-mono">{topoTargetLabel(r.doc)}</span><span className="ml-auto whitespace-nowrap">워커 {r.doc.workers?.length ?? 0} · {fmtTime(r.updated_at)}</span></>} />
          ))}
        </ListRail>
        <div className="flex min-h-0 min-w-0 flex-1 flex-col">
          {doc ? (
            <TopologyCanvas key={sel === NEW ? 'new' : String(sel)} ref={canvasRef} doc={doc} onChange={H.set} onLayout={H.replace} onCommit={H.commit} check={check} checkStale={dirty} workers={workers} canWrite={canWrite} onFocusCheck={current && !dirty ? runCheck : undefined} paletteHost={leftTab === 'pal' ? palHost : null} />
          ) : (
            <div className="p-4"><EmptyState title={rows.length ? '왼쪽에서 토폴로지를 고르십시오' : '토폴로지가 없습니다'} description="[새 토폴로지…] 에서 프리셋(CIMS 한 호스트 / 일반 IMS / IP-PBX)을 골라 시작합니다 — 호스트 주소를 고치고 워커·대상 노드·풀을 끌어 놓으십시오. 예시 = 패키지 scenarios/topology.sample.yaml"
                                         action={<Button variant="default" size="sm" onClick={() => startNew('cims')} disabled={!canWrite}><Plus size={13} /> 새 토폴로지 (CIMS)</Button>} /></div>
          )}
        </div>
      </div>
      {jsonOpen && doc && (
        <Modal title={`레코드 JSON — ${doc.name || '(이름 없음)'}${current ? ` (#${current.id})` : ''}`} onClose={() => setJsonOpen(false)} width={940} height="min(76vh, calc(100vh - 80px))">
          <TopologyJsonEditor doc={doc} onApply={d => { H.set(d); setJsonOpen(false) }} canWrite={canWrite} />
        </Modal>
      )}
    </div>
  )
}
