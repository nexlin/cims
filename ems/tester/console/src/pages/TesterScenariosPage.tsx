// 시험 > 시나리오 — 탭 둘. **시나리오** = 시퀀스 캔버스 편집기(ScenarioCanvas — 레인×행·팔레트·속성·YAML/검증/적합성/절차표 드로어) +
// 툴바(선택·템플릿에서 새로·검증 배지·되돌리기·저장(운영자본)·삭제·단발 실행·프로파일 결합). **부하 프로파일** = YAML 편집기(스키마 검증).
// 동봉본을 저장하면 같은 id 의 운영자본(override)이 생긴다. 기준 토폴로지·미리보기 프로파일은 편집 문맥이라 저장하지 않는다.
import { useCallback, useEffect, useMemo, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { RefreshCw, Plus, Save, Trash2, Play, RotateCcw } from 'lucide-react'
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
import * as S from '@tester/lib/scenario-model'
import type { Doc } from '@tester/lib/scenario-model'

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

function ScenarioEditor({ scenarioId, rows, onSaved, onDeleted }: {
  scenarioId: string | null          // null = 새 문서
  rows: ScenarioRow[]
  onSaved: (id: string) => void
  onDeleted: () => void
}) {
  const nav = useNavigate()
  const { show } = useToast()
  const confirm = useConfirm()
  const { user } = useAuth()
  const canWrite = hasRole(user, 'operator')
  const canDelete = hasRole(user, 'manager')
  const [doc, setDoc] = useState<Doc | null>(null)
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
      setDoc(d); setOrig(JSON.stringify(d)); setSource(null); return
    }
    try {
      const r = await testerApi.scenario(id)
      const d = S.fromApiDoc(r.doc, r.yaml)
      if (!d.id) d.id = id
      setDoc(d); setOrig(JSON.stringify(d)); setSource(r.source)
      if (r.errors.length) setServerErrs(r.errors)
    } catch (e) { show(String(e), 'err') }
  }, [show])
  useEffect(() => { loadDoc(scenarioId) }, [scenarioId, loadDoc])

  const dirty = doc != null && JSON.stringify(doc) !== orig
  const errN = useMemo(() => (doc ? S.validate(doc, null, vocab, {}).filter(i => i.lv === 'error').length : 0), [doc, vocab])

  const save = async () => {
    if (!doc) return
    setSaving(true); setServerErrs([])
    try {
      const row = await testerApi.saveScenario(doc.id, S.toYaml(doc))
      show(source === 'bundled' ? '저장 — 운영자본(override)' : '저장 — 운영자본', 'ok'); setOrig(JSON.stringify(doc)); setSource('user'); onSaved(row.id)
    } catch (e) { const errs = (e as { data?: { errors?: string[] } })?.data?.errors; if (errs?.length) setServerErrs(errs); show(String(e), 'err') } finally { setSaving(false) }
  }
  const del = async () => {
    if (!scenarioId) return
    if (!await confirm({ title: '시나리오 삭제', body: `${scenarioId} 의 운영자본을 지웁니다. 같은 id 의 패키지 동봉본이 있으면 그것이 다시 보입니다.`, confirmLabel: '삭제', tone: 'danger' })) return
    try { await testerApi.deleteScenario(scenarioId); show('삭제', 'ok'); onDeleted() } catch (e) { show(String(e), 'err') }
  }
  const fromTemplate = async (id: string) => {
    try { const r = await testerApi.scenario(id); const d = S.fromApiDoc(r.doc, r.yaml); d.id = 'NEW-' + d.id; d.comment = `템플릿 ${id} 에서`; setDoc(d); setSource(null); show(`템플릿 ${id} 로 새 문서`, 'ok') } catch (e) { show(String(e), 'err') }
  }

  if (!doc) return <EmptyState title="불러오는 중…" />
  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <div className="flex flex-wrap items-center gap-2 border-b border-border bg-muted px-3 py-1.5 text-xs">
        <span className="font-mono text-sm font-semibold">{doc.id}</span>
        {source && <Badge variant={source === 'user' ? 'brandSoft' : 'neutralSoft'}>{source === 'user' ? '운영자본' : '패키지 동봉'}</Badge>}
        {!scenarioId && <Badge variant="infoSoft">새 문서</Badge>}
        {errN ? <Badge variant="dangerSoft">오류 {errN}</Badge> : <Badge variant="successSoft">검증 통과</Badge>}
        {dirty && <Badge variant="warningSoft">변경됨</Badge>}
        {source === 'bundled' && <span className="text-muted-foreground">저장하면 같은 id 의 운영자본이 생겨 동봉본을 덮습니다</span>}
        {serverErrs.map((e, i) => <span key={i} className="text-destructive">{e}</span>)}
        <div className="ml-auto flex flex-wrap items-center gap-1.5">
          <Select value="" onValueChange={fromTemplate} disabled={!canWrite}>
            <SelectTrigger className="h-[26px] w-[170px] text-xs"><SelectValue placeholder="템플릿에서 새로…" /></SelectTrigger>
            <SelectContent>{rows.filter(r => TEMPLATE_IDS.includes(r.id) && !r.errors.length).map(r => <SelectItem key={r.id} value={r.id}>{r.id}</SelectItem>)}</SelectContent>
          </Select>
          <Button variant="outline" size="sm" onClick={() => setDoc(JSON.parse(orig))} disabled={!dirty}><RotateCcw size={13} /> 되돌리기</Button>
          {scenarioId && source === 'user' && canDelete && <Button variant="destructive" size="sm" onClick={del}><Trash2 size={13} /> 삭제</Button>}
          <Button variant="default" size="sm" onClick={save} disabled={!canWrite || saving || errN > 0 || (!dirty && !!scenarioId && source === 'user')}><Save size={13} /> {source === 'bundled' ? '저장 (override)' : '저장'}</Button>
          <Button variant="outline" size="sm" onClick={() => setRunOpen('single')} disabled={!canWrite || dirty || !scenarioId || errN > 0} title={dirty ? '저장 뒤 실행' : undefined}><Play size={13} /> 단발 실행</Button>
          <Button variant="outline" size="sm" onClick={() => setRunOpen('profile')} disabled={!canWrite || dirty || !scenarioId || errN > 0}>프로파일 결합</Button>
        </div>
      </div>
      <ScenarioCanvas doc={doc} onChange={setDoc} topologies={topologies} topoId={topoId} setTopoId={setTopoId} profiles={profiles} profileId={profileId} setProfileId={setProfileId} vocab={vocab} canWrite={canWrite} source={source} />
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
      <YamlEditor kind={kind} value={text} onChange={setText} disabled={!canWrite} onValid={setValid} minHeight={420} />
      {runOpen && keyName && (
        <RunStartDialog scenarioId={keyName} onClose={() => setRunOpen(false)}
                        onStarted={() => { setRunOpen(false); nav('/test/runs') }} />
      )}
    </div>
  )
}

export default function TesterScenariosPage() {
  const [params, setParams] = useSearchParams()
  const [scenarios, setScenarios] = useState<ScenarioRow[]>([])
  const [profiles, setProfiles] = useState<ProfileRow[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const [tab, setTab] = useState<Kind>('scenario')
  const selScenario: string | null | undefined = params.has('id') ? (params.get('id') || null) : undefined   // undefined = 아직 안 고름 · '' = 새 문서
  const setSelScenario = (v: string | null | undefined) => setParams(p => { const q = new URLSearchParams(p); if (v === undefined) q.delete('id'); else q.set('id', v ?? ''); return q })
  const [selProfile, setSelProfile] = useState<string | null | undefined>(undefined)
  const [filter, setFilter] = useState('')

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
  const shown = useMemo(() => scenarios.filter(s => !filter || s.tags.includes(filter) || s.id.includes(filter.toUpperCase())), [scenarios, filter])

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

        <TabsContent value="scenario" className="flex min-h-0 flex-1 flex-col">
          <div className="flex flex-wrap items-center gap-1.5 border-b border-border px-4 py-2">
            <Select value={selScenario === undefined ? '' : (selScenario ?? '__new__')} onValueChange={v => setSelScenario(v === '__new__' ? null : v)}>
              <SelectTrigger className="h-[28px] w-[380px] text-sm"><SelectValue placeholder="시나리오 선택" /></SelectTrigger>
              <SelectContent>
                {shown.map(s => <SelectItem key={s.id} value={s.id}><span className="font-mono">{s.id}</span> <span className="text-muted-foreground">— {s.title ?? ''} · {s.steps} 단계 · {s.source === 'user' ? '운영자' : '동봉'}{s.errors.length ? ` · 오류 ${s.errors.length}` : ''}</span></SelectItem>)}
                {selScenario === null && <SelectItem value="__new__">(새 시나리오)</SelectItem>}
              </SelectContent>
            </Select>
            <Button variant={filter === '' ? 'default' : 'outline'} size="sm" onClick={() => setFilter('')}>전체</Button>
            {tags.map(t => <Button key={t} variant={filter === t ? 'default' : 'outline'} size="sm" onClick={() => setFilter(t)}>{t}</Button>)}
            <Button variant="outline" size="sm" className="ml-auto" onClick={() => setSelScenario(null)}><Plus size={13} /> 새 시나리오</Button>
          </div>
          {selScenario === undefined ? (
            <div className="p-4"><EmptyState title="시나리오를 고르십시오" description="위에서 고르면 시퀀스 캔버스(레인 = 역할, 행 = 단계)로 편집합니다. 기준 토폴로지를 고르면 역할→풀→워커 해석과 컴파일 드라이런(적합성)을 편집 시점에 미리 봅니다. 검증 오류가 있는 파일도 열어 고칠 수 있습니다."
                                          action={<Button variant="outline" size="sm" onClick={() => setSelScenario(null)}><Plus size={13} /> 새 시나리오</Button>} /></div>
          ) : (
            <ScenarioEditor key={selScenario ?? '__new__'} scenarioId={selScenario} rows={scenarios} onSaved={k => { load(); setSelScenario(k) }} onDeleted={() => { load(); setSelScenario(undefined) }} />
          )}
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
