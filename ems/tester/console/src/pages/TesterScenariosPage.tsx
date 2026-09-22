// 시험 > 시나리오 — 탭 둘. **시나리오** = 왼쪽 레일 한 열에 탭 둘([시나리오] 목록 / [팔레트] — 캔버스가 포털로 그린다, 고르면 팔레트 탭으로) +
// 시퀀스 캔버스 편집기(ScenarioCanvas — 레인×행·속성(폭 조절)·YAML/검증/적합성/절차표 드로어) +
// 툴바(선택·템플릿에서 새로·검증 배지·되돌리기·저장(운영자본)·삭제·단발 실행·프로파일 결합). **부하 프로파일** = YAML 편집기(스키마 검증).
// 동봉본을 저장하면 같은 id 의 운영자본(override)이 생긴다. 기준 토폴로지·미리보기 프로파일은 편집 문맥이라 저장하지 않는다.
// 문서 이력은 useDocHistory(Ctrl+Z/Y). 변경이 있으면 다른 시나리오로 옮기기 전에 묻고 탭 닫기도 경고한다. 변경 중 [저장하고 실행] 이 저장 → 실행 창을 한 번에 연다.
// 부하 프로파일 탭은 YAML 편집기 + 검증된 문서의 시간축 율 곡선(ProfileCurve).
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { RefreshCw, Plus, Save, Trash2, Play, RotateCcw, Undo2, Redo2, Gauge, List, Shapes } from 'lucide-react'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Input } from '@core/components/ui/input'
import { Tabs, TabsList, TabsTrigger, TabsContent } from '@core/components/ui/tabs'
import { DataTable, Th, Td, TrLink, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { useToast } from '@core/components/Toast'
import { useConfirm } from '@core/components/custom/confirm'
import { useAuth } from '@core/contexts/AuthContext'
import { hasRole } from '@core/utils/permissions'
import { useSearchParams } from 'react-router-dom'
import { testerApi, type ScenarioRow, type ProfileRow, type TopologyRow, type ScenarioVocab } from '@tester/api/tester'
import YamlEditor from '@tester/components/YamlEditor'
import RunStartDialog from '@tester/components/RunStartDialog'
import ScenarioCanvas from '@tester/components/scenario/ScenarioCanvas'
import ListRail, { RailRow, RailGroup } from '@tester/components/ListRail'
import ProfileCurve from '@tester/components/ProfileCurve'
import * as S from '@tester/lib/scenario-model'
import type { Doc } from '@tester/lib/scenario-model'
import type { ProfileLike } from '@tester/lib/metrics'
import { useDocHistory, useUndoKeys, useUnsavedGuard } from '@tester/lib/use-history'

type Kind = 'scenario' | 'profile'

const NEW_SCENARIO = `# 새 시나리오 — id 는 대문자·숫자·하이픈(3~64). roles 의 pool 은 토폴로지 pools 이름.
id: NEW-SCENARIO
title: 설명
tags: [volte]
roles:
  caller: { pool: volte_ue }
  callee: { pool: volte_ue, disjoint_from: caller }
flow:
  - { step: register,   who: [caller, callee], expect: { code: 200, rrd_ms: { p95: 500 } } }
  - { step: invite,     from: caller, to: callee, media: { audio: amr-wb } }
  - { step: answer,     who: [callee], after_ms: 1500, expect: { srd_ms: { p95: 2000 } } }
  - { step: media_hold, seconds: "\${ht}", expect: { rtp_loss_pct: { max: 0.5 } } }
  - { step: bye,        from: caller, expect: { sdd_ms: { p95: 300 } } }
`
const NEW_PROFILE = `# 부하 프로파일 — model: constant | step | ramp | soak | burst (ETSI TS 186 008)
model: constant
unit: saps
rate: 2
duration_s: 60
ht: 10
stop_on: { csp_5xx_pct: 1.0 }
`

const TEMPLATE_IDS = ['VOLTE-CALL-BASIC', 'VOLTE-REGISTER', 'TRUNK-IBCF-OUTBOUND', 'TRUNK-PBX-TRANSFER', 'TRUNK-PBX-DTMF']

function ScenarioEditor({ scenarioId, rows, onSaved, onDeleted, onDirty, autoRun, onAutoRunDone, paletteHost }: {
  scenarioId: string | null          // null = 새 문서
  rows: ScenarioRow[]
  onSaved: (id: string, thenRun?: 'single' | 'profile') => void
  onDeleted: () => void
  onDirty: (dirty: boolean) => void  // 페이지가 전환 전에 묻기 위해
  autoRun?: 'single' | 'profile' | null   // 새 문서 저장 → id 가 바뀌어 다시 마운트된 뒤 실행 창을 이어 연다
  onAutoRunDone?: () => void
  paletteHost?: HTMLElement | null        // 레일 [팔레트] 탭의 상자
}) {
  const nav = useNavigate()
  const { show } = useToast()
  const confirm = useConfirm()
  const { user } = useAuth()
  const canWrite = hasRole(user, 'operator')
  const canDelete = hasRole(user, 'manager')
  const H = useDocHistory<Doc>()
  const doc = H.doc; const setDoc = H.set
  const [orig, setOrig] = useState('')
  const [source, setSource] = useState<'bundled' | 'user' | null>(null)
  const [serverErrs, setServerErrs] = useState<string[]>([])
  const [saving, setSaving] = useState(false)
  const [runOpen, setRunOpen] = useState<null | 'single' | 'profile'>(null)
  const [topologies, setTopologies] = useState<TopologyRow[]>([])
  const [profiles, setProfiles] = useState<ProfileRow[]>([])
  const [vocab, setVocab] = useState<ScenarioVocab | null>(null)
  const [topoId, setTopoId] = useState<number | null>(() => { try { const v = localStorage.getItem('tester-scn-topo'); return v ? Number(v) : null } catch { return null } })
  const [profileId, setProfileId] = useState<string>('__none__')

  useEffect(() => {
    Promise.all([testerApi.topologies(), testerApi.profiles(), testerApi.vocab()]).then(([t, p, v]) => {
      setTopologies(t.topologies); setProfiles(p.profiles.filter(x => x.errors.length === 0)); setVocab(v)
      if (topoId == null && t.topologies.length) setTopoId(t.topologies[0].id)
    }).catch(e => show(String(e), 'err'))
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])
  useEffect(() => { try { if (topoId != null) localStorage.setItem('tester-scn-topo', String(topoId)) } catch { /* 무시 */ } }, [topoId])

  const loadDoc = useCallback(async (id: string | null) => {
    setServerErrs([])
    if (!id) {
      const d: Doc = { id: 'NEW-SCENARIO', title: '설명', tags: ['volte'], roles: { caller: { pool: 'volte_ue' }, callee: { pool: 'volte_ue', disjoint_from: 'caller' } },
        flow: [{ step: 'register', who: ['caller', 'callee'], expect: { code: 200, rrd_ms: { p95: 500 } } }, { step: 'invite', from: 'caller', to: 'callee', media: { audio: 'amr-wb' } },
               { step: 'answer', who: ['callee'], after_ms: 1500, expect: { srd_ms: { p95: 2000 } } }, { step: 'media_hold', seconds: '${ht}', expect: { rtp_loss_pct: { max: 0.5 } } }, { step: 'bye', from: 'caller', expect: { sdd_ms: { p95: 300 } } }],
        target_evidence: [], comment: '새 시나리오 — id 는 대문자·숫자·하이픈(3~64). roles 의 pool 은 토폴로지 풀 이름 또는 group.' }
      H.reset(d); setOrig(JSON.stringify(d)); setSource(null); return
    }
    try {
      const r = await testerApi.scenario(id)
      const d = S.fromApiDoc(r.doc, r.yaml)
      if (!d.id) d.id = id
      H.reset(d); setOrig(JSON.stringify(d)); setSource(r.source)
      if (r.errors.length) setServerErrs(r.errors)
    } catch (e) { show(String(e), 'err') }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [show])
  useEffect(() => { loadDoc(scenarioId) }, [scenarioId, loadDoc])

  const dirty = doc != null && JSON.stringify(doc) !== orig
  useEffect(() => { onDirty(dirty); return () => onDirty(false) }, [dirty, onDirty])
  useUndoKeys(H.undo, H.redo, canWrite && !!doc)
  useUnsavedGuard(dirty)
  const errN = useMemo(() => (doc ? S.validate(doc, null, vocab, {}).filter(i => i.lv === 'error').length : 0), [doc, vocab])

  /** 저장 — 성공하면 저장된 id, 실패하면 null. thenRun 이면 페이지가 (id 가 바뀌어 다시 마운트되더라도) 실행 창을 이어 연다 */
  const save = async (thenRun?: 'single' | 'profile'): Promise<string | null> => {
    if (!doc) return null
    setSaving(true); setServerErrs([])
    try {
      const row = await testerApi.saveScenario(doc.id, S.toYaml(doc))
      show(source === 'bundled' ? '저장 — 운영자본(override)' : '저장 — 운영자본', 'ok'); setOrig(JSON.stringify(doc)); setSource('user'); onSaved(row.id, thenRun); return row.id
    } catch (e) { const errs = (e as { data?: { errors?: string[] } })?.data?.errors; if (errs?.length) setServerErrs(errs); show(String(e), 'err'); return null } finally { setSaving(false) }
  }
  /** 변경이 있으면 저장부터 하고 실행 창을 연다 */
  const runAfterSave = async (mode: 'single' | 'profile') => {
    if (dirty || !scenarioId) { await save(mode); return }
    setRunOpen(mode)
  }
  useEffect(() => { if (autoRun && scenarioId && doc && !dirty) { setRunOpen(autoRun); onAutoRunDone?.() } }, [autoRun, scenarioId, doc, dirty, onAutoRunDone])
  const del = async () => {
    if (!scenarioId) return
    if (!await confirm({ title: '시나리오 삭제', body: `${scenarioId} 의 운영자본을 지웁니다. 같은 id 의 패키지 동봉본이 있으면 그것이 다시 보입니다.`, confirmLabel: '삭제', tone: 'danger' })) return
    try { await testerApi.deleteScenario(scenarioId); show('삭제', 'ok'); onDeleted() } catch (e) { show(String(e), 'err') }
  }
  const fromTemplate = async (id: string) => {
    try { const r = await testerApi.scenario(id); const d = S.fromApiDoc(r.doc, r.yaml); d.id = 'NEW-' + d.id; d.comment = `템플릿 ${id} 에서`; setDoc(d); setSource(null); show(`템플릿 ${id} 로 새 문서`, 'ok') } catch (e) { show(String(e), 'err') }
  }

  if (!doc) return <EmptyState title="불러오는 중…" />
  const runReason = !canWrite ? null : errN ? `오류 ${errN} 해결 뒤` : null
  const saveReason = errN ? `오류 ${errN} 해결 뒤` : (!dirty && !!scenarioId && source === 'user') ? '변경 없음' : null
  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <div className="flex flex-wrap items-center gap-2 border-b border-border bg-muted px-3 py-1.5 text-xs">
        <span className="font-mono text-sm font-semibold">{doc.id}</span>
        {source && <Badge variant={source === 'user' ? 'brandSoft' : 'neutralSoft'}>{source === 'user' ? '운영자본' : '패키지 동봉'}</Badge>}
        {!scenarioId && <Badge variant="infoSoft">새 문서</Badge>}
        {errN ? <Badge variant="dangerSoft" title="문서 자체의 오류 — 저장이 잠깁니다. 기준 토폴로지와의 적합성은 캔버스 아래 [검증] 드로어">문서 오류 {errN}</Badge> : <Badge variant="successSoft" title="문서 자체는 유효합니다. 기준 토폴로지와의 적합성은 캔버스 아래 [검증] 드로어에 따로 보입니다">문서 검증 통과</Badge>}
        {dirty && <Badge variant="warningSoft">변경됨</Badge>}
        {source === 'bundled' && <span className="text-muted-foreground">저장하면 같은 id 의 운영자본이 생겨 동봉본을 덮습니다</span>}
        {serverErrs.map((e, i) => <span key={i} className="text-destructive">{e}</span>)}
        <div className="ml-auto flex flex-wrap items-center gap-1.5">
          <Select value="" onValueChange={fromTemplate} disabled={!canWrite}>
            <SelectTrigger className="h-[26px] w-[170px] text-xs"><SelectValue placeholder="템플릿에서 새로…" /></SelectTrigger>
            <SelectContent>{rows.filter(r => TEMPLATE_IDS.includes(r.id) && !r.errors.length).map(r => <SelectItem key={r.id} value={r.id}>{r.id}</SelectItem>)}</SelectContent>
          </Select>
          {scenarioId && source === 'user' && canDelete && <Button variant="outline" size="sm" className="text-destructive" onClick={del}><Trash2 size={13} /> 삭제</Button>}
          <Button variant="ghost" size="iconSm" onClick={H.undo} disabled={!canWrite || !H.canUndo} title="실행취소 (Ctrl+Z)"><Undo2 size={13} /></Button>
          <Button variant="ghost" size="iconSm" onClick={H.redo} disabled={!canWrite || !H.canRedo} title="다시실행 (Ctrl+Y)"><Redo2 size={13} /></Button>
          <Button variant="outline" size="sm" onClick={() => setDoc(JSON.parse(orig))} disabled={!dirty} title="저장 시점으로 전부 되돌리기"><RotateCcw size={13} /> 되돌리기</Button>
          <span className="inline-flex items-center gap-1">
            <Button variant="default" size="sm" onClick={() => save()} disabled={!canWrite || saving || !!saveReason}><Save size={13} /> {source === 'bundled' ? '저장 (override)' : '저장'}</Button>
            {saveReason && canWrite && <span className="text-muted-foreground">{saveReason}</span>}
          </span>
          <span className="inline-flex items-center gap-1">
            <Button variant="outline" size="sm" onClick={() => runAfterSave('single')} disabled={!canWrite || saving || !!runReason} title="인스턴스 몇 개로 기능 확인 — 변경이 있으면 먼저 저장합니다"><Play size={13} /> {dirty || !scenarioId ? '저장하고 단발 실행' : '단발 실행'}</Button>
            <Button variant="outline" size="sm" onClick={() => runAfterSave('profile')} disabled={!canWrite || saving || !!runReason} title="부하 프로파일(미리보기 프로파일이 기본값)로 실행 창 열기 — 변경이 있으면 먼저 저장합니다"><Gauge size={13} /> {dirty || !scenarioId ? '저장하고 부하 실행…' : '부하 실행…'}</Button>
            {runReason && <span className="text-muted-foreground">{runReason}</span>}
          </span>
        </div>
      </div>
      <ScenarioCanvas doc={doc} onChange={setDoc} onCommit={H.commit} topologies={topologies} topoId={topoId} setTopoId={setTopoId} profiles={profiles} profileId={profileId} setProfileId={setProfileId} vocab={vocab} canWrite={canWrite} source={source} paletteHost={paletteHost} />
      {runOpen && scenarioId && (
        <RunStartDialog scenarioId={scenarioId} lastTopologyId={topoId} initial={runOpen === 'profile' && profileId !== '__none__' ? { scenario_id: scenarioId, profile: profileId } : undefined}
                        onClose={() => setRunOpen(null)} onStarted={() => { setRunOpen(null); nav('/test/runs') }} />
      )}
    </div>
  )
}

function Editor({ kind, keyName, onSaved, onDeleted }: {
  kind: Kind
  keyName: string | null          // 선택된 id/name — null 이면 새 문서
  onSaved: (key: string) => void
  onDeleted: () => void
}) {
  const nav = useNavigate()
  const { show } = useToast()
  const confirm = useConfirm()
  const { user } = useAuth()
  const canWrite = hasRole(user, 'operator')
  const canDelete = hasRole(user, 'manager')
  const [orig, setOrig] = useState('')
  const [text, setText] = useState('')
  const [source, setSource] = useState<'bundled' | 'user' | null>(null)
  const [newKey, setNewKey] = useState('')
  const [valid, setValid] = useState(false)
  const [parsed, setParsed] = useState<ProfileLike | null>(null)
  const [saving, setSaving] = useState(false)
  const [runOpen, setRunOpen] = useState(false)

  useEffect(() => {
    if (!keyName) { const t = kind === 'scenario' ? NEW_SCENARIO : NEW_PROFILE; setOrig(t); setText(t); setSource(null); setNewKey(''); return }
    ;(kind === 'scenario' ? testerApi.scenario(keyName) : testerApi.profile(keyName))
      .then(d => { const t = d.yaml ?? ''; setOrig(t); setText(t); setSource(d.source) })
      .catch(e => show(String(e), 'err'))
  }, [kind, keyName, show])

  const dirty = text !== orig
  const targetKey = keyName ?? newKey.trim()

  const save = async () => {
    if (!targetKey) { show(kind === 'scenario' ? 'id 를 입력하십시오' : 'name 을 입력하십시오', 'err'); return }
    setSaving(true)
    try {
      if (kind === 'scenario') await testerApi.saveScenario(targetKey, text)
      else await testerApi.saveProfile(targetKey, text)
      show('저장 — 운영자본', 'ok'); setOrig(text); setSource('user'); onSaved(targetKey)
    } catch (e) { show(String(e), 'err') } finally { setSaving(false) }
  }

  const del = async () => {
    if (!keyName) return
    if (!await confirm({ title: `${kind === 'scenario' ? '시나리오' : '프로파일'} 삭제`, body: `${keyName} 의 운영자본을 지웁니다. 같은 id 의 패키지 동봉본이 있으면 그것이 다시 보입니다.`, confirmLabel: '삭제', tone: 'danger' })) return
    try {
      if (kind === 'scenario') await testerApi.deleteScenario(keyName); else await testerApi.deleteProfile(keyName)
      show('삭제', 'ok'); onDeleted()
    } catch (e) { show(String(e), 'err') }
  }

  return (
    <div className="flex min-h-0 flex-1 flex-col gap-2">
      <div className="flex flex-wrap items-center gap-2">
        {keyName ? (
          <>
            <span className="font-mono text-sm font-semibold">{keyName}</span>
            {source && <Badge variant={source === 'user' ? 'brandSoft' : 'neutralSoft'}>{source === 'user' ? '운영자본' : '패키지 동봉'}</Badge>}
            {source === 'bundled' && <span className="text-xs text-muted-foreground">저장하면 같은 id 의 운영자본이 생겨 동봉본을 덮습니다</span>}
          </>
        ) : (
          <Input value={newKey} onChange={e => setNewKey(e.target.value)} placeholder={kind === 'scenario' ? '새 시나리오 id (문서의 id 와 같게)' : '새 프로파일 name'} className="h-[26px] w-[300px] font-mono text-sm" />
        )}
        {dirty && <Badge variant="warningSoft">변경됨</Badge>}
        <div className="ml-auto flex items-center gap-2">
          {kind === 'scenario' && keyName && !dirty && (
            <Button variant="outline" size="sm" onClick={() => setRunOpen(true)} disabled={!canWrite}><Play size={13} /> 단발 실행</Button>
          )}
          <Button variant="outline" size="sm" onClick={() => setText(orig)} disabled={!dirty}><RotateCcw size={13} /> 되돌리기</Button>
          {keyName && source === 'user' && canDelete && <Button variant="destructive" size="sm" onClick={del}><Trash2 size={13} /> 삭제</Button>}
          <Button variant="default" size="sm" onClick={save} disabled={!canWrite || saving || !valid || (!dirty && !!keyName && source === 'user')}><Save size={13} /> 저장</Button>
        </div>
      </div>
      <YamlEditor kind={kind} value={text} onChange={setText} disabled={!canWrite} onValid={(ok, d) => { setValid(ok); setParsed(ok && d ? (d as ProfileLike) : null) }} minHeight={kind === 'profile' ? 300 : 420} />
      {kind === 'profile' && <ProfileCurve profile={parsed} />}
      {runOpen && keyName && (
        <RunStartDialog scenarioId={keyName} onClose={() => setRunOpen(false)}
                        onStarted={() => { setRunOpen(false); nav('/test/runs') }} />
      )}
    </div>
  )
}

export default function TesterScenariosPage() {
  const confirm = useConfirm()
  const [params, setParams] = useSearchParams()
  const dirtyRef = useRef(false)
  const setDirty = useCallback((d: boolean) => { dirtyRef.current = d }, [])
  const discardOk = async () => !dirtyRef.current || await confirm({ title: '변경을 버릴까요?', body: '저장하지 않은 편집이 있습니다. 다른 시나리오로 옮기면 사라집니다.', confirmLabel: '버리고 이동', tone: 'danger' })
  const [scenarios, setScenarios] = useState<ScenarioRow[]>([])
  const [profiles, setProfiles] = useState<ProfileRow[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const [tab, setTab] = useState<Kind>('scenario')
  const selScenario: string | null | undefined = params.has('id') ? (params.get('id') || null) : undefined   // undefined = 아직 안 고름 · '' = 새 문서
  const setSelScenario = (v: string | null | undefined) => setParams(p => { const q = new URLSearchParams(p); if (v === undefined) q.delete('id'); else q.set('id', v ?? ''); return q })
  const pickScenario = async (v: string | null | undefined) => { if (v !== selScenario && !await discardOk()) return; setSelScenario(v); setLeftTab('pal') }
  const [leftTab, setLeftTab] = useState<'rec' | 'pal'>('rec')
  const [palHost, setPalHost] = useState<HTMLDivElement | null>(null)   // 레일 [팔레트] 탭의 상자 — 캔버스가 여기에 포털
  const [selProfile, setSelProfile] = useState<string | null | undefined>(undefined)
  const [autoRun, setAutoRun] = useState<'single' | 'profile' | null>(null)
  const autoRunDone = useCallback(() => setAutoRun(null), [])
  const [filter, setFilter] = useState('')
  const [q, setQ] = useState('')

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [s, p] = await Promise.all([testerApi.scenarios(), testerApi.profiles()])
      setScenarios(s.scenarios); setProfiles(p.profiles); setError(null)
    } catch (e) { setError(String(e)) }
    finally { setLoading(false) }
  }, [])
  useEffect(() => { load() }, [load])

  const tags = useMemo(() => Array.from(new Set(scenarios.flatMap(s => s.tags))).sort(), [scenarios])
  // 레일 = 태그 칩 + 검색(id·제목), 출처별 그룹(운영자본 먼저)
  const shown = useMemo(() => { const k = q.trim().toLowerCase(); return scenarios.filter(s => (!filter || s.tags.includes(filter)) && (!k || s.id.toLowerCase().includes(k) || (s.title ?? '').toLowerCase().includes(k))) }, [scenarios, filter, q])
  const railGroups = useMemo(() => (['user', 'bundled'] as const).map(src => [src, shown.filter(s => s.source === src)] as const).filter(([, r]) => r.length), [shown])

  return (
    <div className="flex h-full flex-col">
      <div className="toolbar flex flex-wrap items-center gap-2.5 border-b border-border bg-muted px-4 py-3">
        <span className="whitespace-nowrap text-md font-semibold text-foreground">시나리오 · 부하 프로파일</span>
        <span className="text-sm text-muted-foreground">시나리오 {scenarios.length} · 프로파일 {profiles.length} · 운영자본은 Tester.DataDir/scenarios/</span>
        {error && <span className="text-sm text-destructive">{error}</span>}
        <div className="ml-auto">
          <Button variant="outline" size="sm" onClick={load} disabled={loading}><RefreshCw size={13} /> 새로고침</Button>
        </div>
      </div>

      <Tabs value={tab} onValueChange={v => setTab(v as Kind)} className="flex min-h-0 flex-1 flex-col">
        <TabsList className="mx-4 mt-3 w-fit">
          <TabsTrigger value="scenario">시나리오 ({scenarios.length})</TabsTrigger>
          <TabsTrigger value="profile">부하 프로파일 ({profiles.length})</TabsTrigger>
        </TabsList>

        <TabsContent value="scenario" className="flex min-h-0 flex-1">
          <ListRail storageKey="tester-scn-rail" label="시나리오" search={{ value: q, onChange: setQ, placeholder: 'id·제목 검색' }}
                    tab={leftTab} onTab={k => setLeftTab(k as 'rec' | 'pal')}
                    tabs={[
                      { key: 'rec', label: '시나리오', icon: <List size={13} />, badge: <Badge variant="neutralSoft">{scenarios.length}</Badge> },
                      { key: 'pal', label: '팔레트', icon: <Shapes size={13} />, content: selScenario !== undefined ? <div ref={setPalHost} className="min-h-0 flex-1" /> : <div className="p-3 text-xs text-muted-foreground">시나리오를 열면 여기에 팔레트가 나옵니다 — 역할(레인)과 단계를 캔버스 행 사이로 끌어 놓거나 클릭해 삽입합니다</div> },
                    ]}
                    chips={<>{[['', '전체'], ...tags.map(t => [t, t])].map(([c, l]) => (
                      <button key={c} onClick={() => setFilter(c)} className={`h-6 rounded-sm border px-2 text-xs ${filter === c ? 'border-primary bg-primary text-primary-foreground' : 'border-border text-muted-foreground hover:bg-accent'}`}>{l}</button>))}</>}
                    action={<Button variant="outline" size="sm" className="w-full" onClick={() => pickScenario(null)}><Plus size={13} /> 새 시나리오</Button>}>
            {selScenario === null && <RailRow selected top={<><Badge variant="infoSoft">새</Badge><span className="font-mono font-medium">NEW-SCENARIO</span></>} bottom={<span>저장하면 운영자본으로 목록에 들어갑니다</span>} onClick={() => {}} />}
            {railGroups.length === 0 && <div className="p-3 text-xs text-muted-foreground">{scenarios.length ? '검색 결과 없음' : '시나리오 없음'}</div>}
            {railGroups.map(([src, rows]) => (
              <div key={src}>
                <RailGroup>{src === 'user' ? '운영자본' : '패키지 동봉'} · {rows.length}</RailGroup>
                {rows.map(s => (
                  <RailRow key={s.id} selected={s.id === selScenario} onClick={() => pickScenario(s.id)} title={s.path}
                           top={<><span className="truncate font-mono font-medium">{s.id}</span>{s.errors.length > 0 && <Badge variant="dangerSoft" title={s.errors.join('\n')}>오류 {s.errors.length}</Badge>}<span className="ml-auto whitespace-nowrap font-mono text-muted-foreground">{s.steps} 단계</span></>}
                           bottom={<><span className="truncate">{s.title ?? '—'}</span><span className="ml-auto whitespace-nowrap font-mono">{s.tags.join(' ')}</span></>} />
                ))}
              </div>
            ))}
          </ListRail>
          <div className="flex min-h-0 min-w-0 flex-1 flex-col">
            {selScenario === undefined ? (
              <div className="p-4"><EmptyState title="왼쪽에서 시나리오를 고르십시오" description="고르면 시퀀스 캔버스(레인 = 역할, 행 = 단계)로 편집합니다. 기준 토폴로지를 고르면 역할→풀→워커 해석과 컴파일 드라이런(적합성)을 편집 시점에 미리 봅니다. 검증 오류가 있는 파일도 열어 고칠 수 있습니다."
                                            action={<Button variant="outline" size="sm" onClick={() => pickScenario(null)}><Plus size={13} /> 새 시나리오</Button>} /></div>
            ) : (
              <ScenarioEditor key={selScenario ?? '__new__'} scenarioId={selScenario} rows={scenarios} onDirty={setDirty} autoRun={autoRun} onAutoRunDone={autoRunDone} paletteHost={leftTab === 'pal' ? palHost : null}
                              onSaved={(k, run) => { load(); setSelScenario(k); setAutoRun(run ?? null) }} onDeleted={() => { load(); setSelScenario(undefined) }} />
            )}
          </div>
        </TabsContent>

        <TabsContent value="profile" className="min-h-0 flex-1 overflow-auto p-4">
          <div className="grid min-h-0 gap-4 lg:grid-cols-[minmax(320px,2fr)_3fr]">
            <section className="flex flex-col gap-2">
              <div className="flex items-center">
                <span className="text-sm text-muted-foreground">ETSI TS 186 008 — constant / step / ramp / soak / burst</span>
                <Button variant="outline" size="sm" className="ml-auto" onClick={() => setSelProfile(null)}><Plus size={13} /> 새 프로파일</Button>
              </div>
              {profiles.length === 0 ? <EmptyState title="프로파일이 없습니다" /> : (
                <DataTable>
                  <thead><tr><Th>이름</Th><Th>모델</Th><Th>출처</Th><Th>검증</Th></tr></thead>
                  <tbody>
                    {profiles.map(p => (
                      <TrLink key={p.name} selected={selProfile === p.name} onClick={() => setSelProfile(p.name)}>
                        <Td mono>{p.name}</Td>
                        <Td>{orDash(p.model)}</Td>
                        <Td><Badge variant={p.source === 'user' ? 'brandSoft' : 'neutralSoft'}>{p.source === 'user' ? '운영자' : '동봉'}</Badge></Td>
                        <Td>{p.errors.length === 0 ? <Badge variant="successSoft">OK</Badge> : <Badge variant="dangerSoft" title={p.errors.join('\n')}>오류 {p.errors.length}</Badge>}</Td>
                      </TrLink>
                    ))}
                  </tbody>
                </DataTable>
              )}
            </section>
            <section className="flex min-h-0 flex-col">
              {selProfile === undefined ? (
                <EmptyState title="프로파일을 고르십시오" description="step 프로파일은 hold_s 마다 IHS 를 보고 임계 이내면 +step — 초과 직전 단계가 DOC 입니다." />
              ) : (
                <Editor kind="profile" keyName={selProfile} onSaved={k => { load(); setSelProfile(k) }} onDeleted={() => { load(); setSelProfile(undefined) }} />
              )}
            </section>
          </div>
        </TabsContent>
      </Tabs>
    </div>
  )
}
