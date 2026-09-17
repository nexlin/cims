// run 시작 창 — 시나리오·토폴로지·프로파일(없으면 단발)·인스턴스·율·${ht} 바인딩·라벨 → POST /runs. 오른쪽에 **계획 미리보기**
// (POST /runs/plan 드라이런 — 입력이 바뀌면 500 ms 뒤 다시). 컴파일 오류가 있으면 시작을 막는다.
// 시나리오 화면의 [단발 실행](scenarioId 고정)·색인의 [재실행](initial 로 같은 조건) 도 같은 창을 쓴다.
import { useEffect, useMemo, useState } from 'react'
import { Play } from 'lucide-react'
import Modal from '@core/components/Modal'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { Badge } from '@core/components/ui/badge'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { FormField } from '@core/components/custom/form-field'
import { useToast } from '@core/components/Toast'
import { testerApi, type ScenarioRow, type ProfileRow, type TopologyRow, type RunRequest, type PlanResult, type PlanRequest } from '@tester/api/tester'
import { topoTargetLabel } from '@tester/pages/TesterTopologiesPage'
import PlanPreview from '@tester/components/PlanPreview'

const NONE = '__none__'

export interface RunStartInitial { scenario_id?: string; topology?: string; profile?: string | null; bindings?: Record<string, unknown>; label?: string | null; instances?: number }

export default function RunStartDialog({ onClose, onStarted, scenarioId, lastTopologyId, initial }: {
  onClose: () => void
  onStarted: (runId: string) => void
  scenarioId?: string
  lastTopologyId?: number | null
  initial?: RunStartInitial
}) {
  const { show } = useToast()
  const [scenarios, setScenarios] = useState<ScenarioRow[]>([])
  const [profiles, setProfiles] = useState<ProfileRow[]>([])
  const [topologies, setTopologies] = useState<TopologyRow[]>([])
  const [scenario, setScenario] = useState(scenarioId ?? initial?.scenario_id ?? '')
  const [topology, setTopology] = useState<string>(lastTopologyId ? String(lastTopologyId) : '')
  const [profile, setProfile] = useState<string>(initial?.profile ? initial.profile : NONE)
  const [instances, setInstances] = useState(String(initial?.instances ?? 1))
  const [rate, setRate] = useState('')
  const [ht, setHt] = useState(initial?.bindings?.ht != null ? String(initial.bindings.ht) : '')
  // ${var} 바인딩 추가분(k=v 한 줄에 하나 — pickup 피처코드 등 문자열 값도). ht 는 위 필드
  const [binds, setBinds] = useState(Object.entries(initial?.bindings ?? {}).filter(([k]) => k !== 'ht').map(([k, v]) => `${k}=${String(v)}`).join('\n'))
  const [label, setLabel] = useState(initial?.label ?? '')
  const [busy, setBusy] = useState(false)
  const [err, setErr] = useState<string | null>(null)
  const [plan, setPlan] = useState<PlanResult | null>(null)
  const [planning, setPlanning] = useState(false)

  useEffect(() => {
    Promise.all([testerApi.scenarios(), testerApi.profiles(), testerApi.topologies()]).then(([s, p, t]) => {
      setScenarios(s.scenarios.filter(x => x.errors.length === 0)); setProfiles(p.profiles.filter(x => x.errors.length === 0)); setTopologies(t.topologies)
      if (initial?.topology) { const hit = t.topologies.find(x => x.name === initial.topology || (x.doc?.name === initial.topology)); if (hit) setTopology(String(hit.id)) }
      else if (!topology && t.topologies.length >= 1) setTopology(String(t.topologies[0].id))
    }).catch(e => setErr(String(e)))
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const sc = useMemo(() => scenarios.find(x => x.id === scenario), [scenarios, scenario])
  const body = useMemo<RunRequest | null>(() => {
    if (!scenario || !topology) return null
    const b: RunRequest = { scenario_id: scenario, topology_id: Number(topology) }
    if (profile !== NONE) b.profile = profile
    else {
      const n = parseInt(instances, 10); if (isFinite(n) && n >= 1) b.instances = n
      const r = parseFloat(rate); if (isFinite(r) && r > 0) b.rate_saps = r
    }
    const bb: Record<string, number | string> = {}
    for (const line of binds.split(/\n|,/)) { const [k, ...rest] = line.split('='); const v = rest.join('=').trim(); if (k.trim() && v) bb[k.trim()] = /^\d+$/.test(v) ? Number(v) : v }
    const h = parseInt(ht, 10); if (isFinite(h) && h >= 0) bb.ht = h
    if (Object.keys(bb).length) b.bindings = bb
    if (label.trim()) b.label = label.trim()
    return b
  }, [scenario, topology, profile, instances, rate, ht, binds, label])

  // 계획 미리보기 — 입력 디바운스
  useEffect(() => {
    if (!body) { setPlan(null); return }
    let alive = true
    setPlanning(true)
    const id = window.setTimeout(async () => {
      try {
        const req: PlanRequest = { scenario_id: body.scenario_id, topology_id: body.topology_id, profile: body.profile, bindings: body.bindings, instances: body.instances, rate_saps: body.rate_saps }
        const p = await testerApi.plan(req)
        if (alive) setPlan(p)
      } catch (e) { if (alive) setPlan({ ok: false, errors: [String(e)], warnings: [], notes: [] }) }
      finally { if (alive) setPlanning(false) }
    }, 500)
    return () => { alive = false; window.clearTimeout(id) }
  }, [body])

  const canStart = !!body && !busy && (plan?.ok ?? false) && !(plan?.active_runs?.length)

  const start = async () => {
    if (!body) return
    setBusy(true); setErr(null)
    try {
      const r = await testerApi.startRun(body)
      show(`run ${r.id} 시작`, 'ok')
      onStarted(r.id)
    } catch (e) { setErr(String(e)) } finally { setBusy(false) }
  }

  return (
    <Modal title={initial ? 'run 재실행 — 같은 조건' : 'run 시작'} onClose={onClose} width={960}>
      <div className="grid gap-4 p-4 md:grid-cols-[minmax(320px,1fr)_minmax(360px,1.1fr)]">
        <div className="flex flex-col gap-3">
          <FormField label="시나리오" required help={sc ? `${sc.title ?? ''} · ${sc.steps} 단계 · ${sc.tags.join(', ')}` : '검증 통과한 시나리오만 고를 수 있습니다'}>
            <Select value={scenario} onValueChange={setScenario} disabled={!!scenarioId}>
              <SelectTrigger><SelectValue placeholder="시나리오 선택" /></SelectTrigger>
              <SelectContent>{scenarios.map(s => <SelectItem key={s.id} value={s.id}>{s.id}{s.title ? ` — ${s.title}` : ''}</SelectItem>)}</SelectContent>
            </Select>
          </FormField>
          <FormField label="토폴로지" required help="호스트›워커·대상 노드›풀. [시험 > 토폴로지] 에서 편집">
            <Select value={topology} onValueChange={setTopology}>
              <SelectTrigger><SelectValue placeholder="토폴로지 선택" /></SelectTrigger>
              <SelectContent>{topologies.map(t => <SelectItem key={t.id} value={String(t.id)}>{t.name} <span className="text-muted-foreground">· {topoTargetLabel(t.doc)} · 워커 {t.doc.workers?.length ?? 0}</span></SelectItem>)}</SelectContent>
            </Select>
          </FormField>
          <FormField label="부하 프로파일" help="없음 = 단발(기능) 실행 — 아래 인스턴스·율을 쓴다">
            <Select value={profile} onValueChange={setProfile}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value={NONE}>없음 (단발)</SelectItem>
                {profiles.map(p => <SelectItem key={p.name} value={p.name}>{p.name} <span className="text-muted-foreground">· {p.model}</span></SelectItem>)}
              </SelectContent>
            </Select>
          </FormField>
          {profile === NONE && (
            <div className="grid grid-cols-2 gap-3">
              <FormField label="인스턴스 수" help="시나리오 시도 횟수 (기본 1)">
                <Input value={instances} onChange={e => setInstances(e.target.value)} inputMode="numeric" />
              </FormField>
              <FormField label="발생율 (SApS)" help="비면 인스턴스를 1 초 안에">
                <Input value={rate} onChange={e => setRate(e.target.value)} inputMode="decimal" placeholder="자동" />
              </FormField>
            </div>
          )}
          <div className="grid grid-cols-2 gap-3">
            <FormField label="${'{ht}'} 세션 유지(초)" help="시나리오의 ${'{ht}'} 바인딩 — 프로파일 ht 보다 우선">
              <Input value={ht} onChange={e => setHt(e.target.value)} inputMode="numeric" placeholder="프로파일/기본" />
            </FormField>
            <FormField label="바인딩 추가" help="시나리오의 다른 ${'{var}'} — k=v (쉼표/줄바꿈 구분). 예: pickup_code=** (당겨받기 피처코드)">
              <Input value={binds} onChange={e => setBinds(e.target.value)} className="font-mono" placeholder="pickup_code=**" />
            </FormField>
            <FormField label="라벨" help="보고서·비교에 표시 (예: CSP 0.2.126 회귀)">
              <Input value={label} onChange={e => setLabel(e.target.value)} maxLength={120} />
            </FormField>
          </div>
          <div className="text-xs text-muted-foreground">대상 빌드는 시작 시 대상 OAM 에서 자동 기록된다(cims 대상 · oam 노드).</div>
          {err && <div className="text-sm text-destructive">{err}</div>}
        </div>
        <PlanPreview plan={plan} loading={planning} />
      </div>
      <div className="flex items-center justify-end gap-2 border-t border-border px-4 py-3">
        {topologies.length === 0 && <Badge variant="warningSoft">토폴로지가 없습니다 — 먼저 만드십시오</Badge>}
        <Button variant="outline" onClick={onClose}>닫기</Button>
        <Button variant="default" onClick={start} disabled={!canStart} title={plan && !plan.ok ? '계획 오류를 먼저 해결' : undefined}><Play size={13} /> {busy ? '시작 중…' : '시작'}</Button>
      </div>
    </Modal>
  )
}
