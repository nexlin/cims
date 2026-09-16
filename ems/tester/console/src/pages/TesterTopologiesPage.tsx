// 시험 > 토폴로지 — 레코드 선택 · 이름 · 검증 배지 · 자동 배치 · 되돌리기 · 연결 검사 · 저장/생성/삭제 + 캔버스 편집기(TopologyCanvas).
// 레코드는 컨트롤러 `topology` 스키마(호스트›워커·대상 노드›풀)로 저장 전 검증하고, 카드 위치·영역 크기는 레코드 layout 에 같이 저장한다.
import { useCallback, useEffect, useMemo, useState } from 'react'
import { RefreshCw, Plus, Save, Trash2, RotateCcw, PlugZap, LayoutGrid } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { EmptyState } from '@core/components/custom/empty-state'
import { useToast } from '@core/components/Toast'
import { useConfirm } from '@core/components/custom/confirm'
import { useAuth } from '@core/contexts/AuthContext'
import { hasRole } from '@core/utils/permissions'
import { testerApi, type TopologyRow, type TopologyDoc, type WorkerRow, type CheckItem } from '@tester/api/tester'
import TopologyCanvas from '@tester/components/topology/TopologyCanvas'
import * as M from '@tester/lib/topology-model'
import { fmtTime } from '@tester/lib/fmt'

/** 표시용 — SIP 접속점 노드 첫 항목의 호스트 주소·기본 도메인 */
export function topoTargetLabel(doc: TopologyDoc): string {
  const sip = Object.values(doc.target?.nodes ?? {}).find(n => n.role === 'sip' && M.accessListeners(n).length)
  if (!sip) return doc.target?.name ?? '—'
  const ip = sip.addr || (doc.hosts?.[sip.host]?.ip ?? sip.host)
  const dom = sip.sip?.domains?.[0]
  return dom ? `${ip} · ${dom}` : ip
}

const NEW = '__new__'

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
  const [doc, setDoc] = useState<TopologyDoc | null>(null)
  const [orig, setOrig] = useState('')
  const [saving, setSaving] = useState(false)
  const [workers, setWorkers] = useState<WorkerRow[]>([])
  const [check, setCheck] = useState<{ items: CheckItem[]; at: string } | null>(null)
  const [checking, setChecking] = useState(false)
  const [serverErrs, setServerErrs] = useState<string[]>([])

  const load = useCallback(async () => {
    setLoading(true)
    try { const r = await testerApi.topologies(); setRows(r.topologies); setError(null); return r.topologies }
    catch (e) { setError(String(e)); return [] } finally { setLoading(false) }
  }, [])
  useEffect(() => { load().then(t => { if (t.length && sel === null) setSel(t[0].id) }) }, [load])   // eslint-disable-line react-hooks/exhaustive-deps

  const current = useMemo(() => (typeof sel === 'number' ? rows.find(r => r.id === sel) ?? null : null), [rows, sel])
  useEffect(() => {
    setCheck(null); setWorkers([]); setServerErrs([])
    if (sel === NEW) { const d = M.fromPreset('cims', null); setDoc(d); setOrig(JSON.stringify(d)); return }
    if (!current) { setDoc(null); return }
    const d = M.deep(current.doc); M.ensureLayout(d); setDoc(d); setOrig(JSON.stringify(d))
    testerApi.workers(current.id).then(w => setWorkers(w.workers)).catch(() => {})
  }, [sel, current])

  const dirty = doc != null && JSON.stringify(doc) !== orig
  const issues = useMemo(() => (doc ? M.validate(doc) : []), [doc])
  const errN = issues.filter(i => i.level === 'err').length

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
    try { await testerApi.deleteTopology(current.id); show('삭제', 'ok'); setSel(null); setDoc(null); load() } catch (e) { show(String(e), 'err') }
  }
  const runCheck = async () => {
    if (!current) return
    setChecking(true)
    try { const r = await testerApi.checkTopology(current.id); setCheck({ items: r.items, at: new Date().toISOString() }); show(r.ok ? '연결 검사: 전부 도달' : '연결 검사: 미도달 항목 있음 — 카드의 붉은 표시', r.ok ? 'ok' : 'err') }
    catch (e) { show(String(e), 'err') } finally { setChecking(false) }
    testerApi.workers(current.id).then(w => setWorkers(w.workers)).catch(() => {})
  }
  const auto = () => { if (!doc) return; const d = M.deep(doc); M.autoLayout(d, id => { const el = document.querySelector<HTMLElement>(`[data-node="${id}"],[data-worker="${id}"]`); return { w: el?.offsetWidth ?? 250, h: el?.offsetHeight ?? 100 } }); setDoc(d) }

  return (
    <div className="flex h-full flex-col">
      <div className="toolbar flex flex-wrap items-center gap-2.5 border-b border-border bg-muted px-4 py-2">
        <span className="whitespace-nowrap text-md font-semibold text-foreground">토폴로지</span>
        <Select value={sel == null ? '' : String(sel)} onValueChange={v => setSel(v === NEW ? NEW : Number(v))}>
          <SelectTrigger className="h-[28px] w-[300px] text-sm"><SelectValue placeholder={rows.length ? '토폴로지 선택' : '토폴로지 없음 — 새로 만드십시오'} /></SelectTrigger>
          <SelectContent>
            {rows.map(r => <SelectItem key={r.id} value={String(r.id)}>{r.name} <span className="text-muted-foreground">#{r.id} · {topoTargetLabel(r.doc)} · 워커 {r.doc.workers?.length ?? 0} · {fmtTime(r.updated_at, true)}</span></SelectItem>)}
            {sel === NEW && <SelectItem value={NEW}>(새 토폴로지)</SelectItem>}
          </SelectContent>
        </Select>
        {doc && <Input value={doc.name} onChange={e => setDoc({ ...doc, name: e.target.value })} disabled={!canWrite} placeholder="이름" className="h-[28px] w-[160px] text-sm" />}
        {doc && (errN ? <Badge variant="dangerSoft">오류 {errN}</Badge> : issues.some(i => i.level === 'warn') ? <Badge variant="warningSoft">경고 {issues.filter(i => i.level === 'warn').length}</Badge> : <Badge variant="successSoft">검증 통과</Badge>)}
        {dirty && <Badge variant="warningSoft">변경됨</Badge>}
        {current?.doc && (current as { migrated_from?: string }).migrated_from && <Badge variant="infoSoft" title="이전 꼴(target.csp/workers[].url) 레코드를 v2 로 승계했다">v1 승계</Badge>}
        {error && <span className="text-sm text-destructive">{error}</span>}
        {serverErrs.map((e, i) => <span key={i} className="text-xs text-destructive">{e}</span>)}
        <div className="ml-auto flex flex-wrap items-center gap-1.5">
          <Button variant="outline" size="sm" onClick={() => load()} disabled={loading}><RefreshCw size={13} /></Button>
          <Button variant="outline" size="sm" onClick={() => setSel(NEW)} disabled={!canWrite}><Plus size={13} /> 새 토폴로지</Button>
          <Button variant="outline" size="sm" onClick={auto} disabled={!doc || !canWrite}><LayoutGrid size={13} /> 자동 배치</Button>
          <Button variant="outline" size="sm" onClick={() => { if (orig) setDoc(JSON.parse(orig)) }} disabled={!dirty}><RotateCcw size={13} /> 되돌리기</Button>
          <Button variant="outline" size="sm" onClick={runCheck} disabled={!current || checking || !canWrite || dirty} title={dirty ? '저장 뒤 검사' : undefined}><PlugZap size={13} /> {checking ? '검사 중…' : '연결 검사'}</Button>
          {current && canDelete && <Button variant="destructive" size="sm" onClick={del}><Trash2 size={13} /> 삭제</Button>}
          <Button variant="default" size="sm" onClick={save} disabled={!canWrite || saving || !doc || errN > 0 || (!dirty && sel !== NEW)}><Save size={13} /> {sel === NEW ? '생성' : '저장'}</Button>
        </div>
      </div>
      {doc ? (
        <TopologyCanvas doc={doc} onChange={setDoc} check={check} workers={workers} canWrite={canWrite} onFocusCheck={current && !dirty ? runCheck : undefined} />
      ) : (
        <div className="p-4"><EmptyState title={rows.length ? '토폴로지를 고르십시오' : '토폴로지가 없습니다'} description="[새 토폴로지] 는 CIMS 한 호스트 프리셋으로 시작합니다 — 호스트 주소를 고치고 워커·대상 노드·풀을 끌어 놓으십시오. 예시 = 패키지 scenarios/topology.sample.yaml"
                                     action={<Button variant="default" size="sm" onClick={() => setSel(NEW)} disabled={!canWrite}><Plus size={13} /> 새 토폴로지</Button>} /></div>
      )}
    </div>
  )
}
