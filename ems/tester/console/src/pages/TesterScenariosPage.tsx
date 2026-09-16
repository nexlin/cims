// 시험 > 시나리오 — 시나리오·부하 프로파일 목록(패키지 동봉 + 운영자본, 검증 오류 포함) + YAML 편집기(스키마 검증)·
// 저장(운영자본)·삭제(운영자본만)·단발 실행. 동봉본을 저장하면 같은 id 의 운영자본(override)이 생긴다.
import { useCallback, useEffect, useMemo, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { RefreshCw, Plus, Save, Trash2, Play, RotateCcw } from 'lucide-react'
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
import { testerApi, type ScenarioRow, type ProfileRow } from '@tester/api/tester'
import YamlEditor from '@tester/components/YamlEditor'
import RunStartDialog from '@tester/components/RunStartDialog'

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
  const [scenarios, setScenarios] = useState<ScenarioRow[]>([])
  const [profiles, setProfiles] = useState<ProfileRow[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const [tab, setTab] = useState<Kind>('scenario')
  const [selScenario, setSelScenario] = useState<string | null | undefined>(undefined)   // undefined = 아직 안 고름
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

        <TabsContent value="scenario" className="min-h-0 flex-1 overflow-auto p-4">
          <div className="grid min-h-0 gap-4 lg:grid-cols-[minmax(360px,2fr)_3fr]">
            <section className="flex flex-col gap-2">
              <div className="flex flex-wrap items-center gap-1.5">
                <Button variant={filter === '' ? 'default' : 'outline'} size="sm" onClick={() => setFilter('')}>전체</Button>
                {tags.map(t => <Button key={t} variant={filter === t ? 'default' : 'outline'} size="sm" onClick={() => setFilter(t)}>{t}</Button>)}
                <Button variant="outline" size="sm" className="ml-auto" onClick={() => setSelScenario(null)}><Plus size={13} /> 새 시나리오</Button>
              </div>
              {shown.length === 0 ? <EmptyState title="시나리오가 없습니다" description="패키지 scenarios/ 또는 Tester.DataDir/scenarios/ 에 YAML 을 둡니다." /> : (
                <DataTable>
                  <thead><tr><Th>id</Th><Th>제목</Th><Th>태그</Th><Th align="right">단계</Th><Th>출처</Th><Th>검증</Th></tr></thead>
                  <tbody>
                    {shown.map(s => (
                      <TrLink key={s.id} selected={selScenario === s.id} onClick={() => setSelScenario(s.id)}>
                        <Td mono>{s.id}</Td>
                        <Td>{orDash(s.title)}</Td>
                        <Td><span className="flex flex-wrap gap-1">{s.tags.map(t => <Badge key={t} variant="neutralSoft">{t}</Badge>)}</span></Td>
                        <Td align="right">{s.steps}</Td>
                        <Td><Badge variant={s.source === 'user' ? 'brandSoft' : 'neutralSoft'}>{s.source === 'user' ? '운영자' : '동봉'}</Badge></Td>
                        <Td>{s.errors.length === 0 ? <Badge variant="successSoft">OK</Badge> : <Badge variant="dangerSoft" title={s.errors.join('\n')}>오류 {s.errors.length}</Badge>}</Td>
                      </TrLink>
                    ))}
                  </tbody>
                </DataTable>
              )}
            </section>
            <section className="flex min-h-0 flex-col">
              {selScenario === undefined ? (
                <EmptyState title="시나리오를 고르십시오" description="왼쪽에서 행을 누르면 YAML 을 편집합니다. 검증 오류가 있는 파일도 열어 고칠 수 있습니다." />
              ) : (
                <Editor kind="scenario" keyName={selScenario} onSaved={k => { load(); setSelScenario(k) }} onDeleted={() => { load(); setSelScenario(undefined) }} />
              )}
            </section>
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
