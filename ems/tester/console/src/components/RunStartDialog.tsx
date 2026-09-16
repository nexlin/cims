// run 시작 창 — 시나리오·토폴로지·프로파일(없으면 단발)·인스턴스·율·${ht} 바인딩·라벨 → POST /runs.
// 시나리오 화면의 [단발 실행] 도 같은 창을 쓴다(scenarioId 고정).
import { useEffect, useMemo, useState } from 'react'
import { Play } from 'lucide-react'
import Modal from '@core/components/Modal'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { Badge } from '@core/components/ui/badge'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { FormField } from '@core/components/custom/form-field'
import { useToast } from '@core/components/Toast'
import { testerApi, type ScenarioRow, type ProfileRow, type TopologyRow, type RunRequest } from '@tester/api/tester'

const NONE = '__none__'

export default function RunStartDialog({ onClose, onStarted, scenarioId, lastTopologyId }: {
  onClose: () => void
  onStarted: (runId: string) => void
  scenarioId?: string
  lastTopologyId?: number | null
}) {
  const { show } = useToast()
  const [scenarios, setScenarios] = useState<ScenarioRow[]>([])
  const [profiles, setProfiles] = useState<ProfileRow[]>([])
  const [topologies, setTopologies] = useState<TopologyRow[]>([])
  const [scenario, setScenario] = useState(scenarioId ?? '')
  const [topology, setTopology] = useState<string>(lastTopologyId ? String(lastTopologyId) : '')
  const [profile, setProfile] = useState<string>(NONE)
  const [instances, setInstances] = useState('1')
  const [rate, setRate] = useState('')
  const [ht, setHt] = useState('')
  const [label, setLabel] = useState('')
  const [busy, setBusy] = useState(false)
  const [err, setErr] = useState<string | null>(null)

  useEffect(() => {
    Promise.all([testerApi.scenarios(), testerApi.profiles(), testerApi.topologies()]).then(([s, p, t]) => {
      setScenarios(s.scenarios.filter(x => x.errors.length === 0)); setProfiles(p.profiles.filter(x => x.errors.length === 0)); setTopologies(t.topologies)
      if (!topology && t.topologies.length === 1) setTopology(String(t.topologies[0].id))
    }).catch(e => setErr(String(e)))
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const sc = useMemo(() => scenarios.find(x => x.id === scenario), [scenarios, scenario])
  const canStart = !!scenario && !!topology && !busy

  const start = async () => {
    const body: RunRequest = { scenario_id: scenario, topology_id: Number(topology) }
    if (profile !== NONE) body.profile = profile
    else {
      const n = parseInt(instances, 10); if (isFinite(n) && n >= 1) body.instances = n
      const r = parseFloat(rate); if (isFinite(r) && r > 0) body.rate_saps = r
    }
    const h = parseInt(ht, 10); if (isFinite(h) && h >= 0) body.bindings = { ht: h }
    if (label.trim()) body.label = label.trim()
    setBusy(true); setErr(null)
    try {
      const r = await testerApi.startRun(body)
      show(`run ${r.id} 시작`, 'ok')
      onStarted(r.id)
    } catch (e) { setErr(String(e)) } finally { setBusy(false) }
  }

  return (
    <Modal title="run 시작" onClose={onClose} width={560}>
      <div className="flex flex-col gap-3 p-4">
        <FormField label="시나리오" required help={sc ? `${sc.title ?? ''} · ${sc.steps} 단계 · ${sc.tags.join(', ')}` : '검증 통과한 시나리오만 고를 수 있습니다'}>
          <Select value={scenario} onValueChange={setScenario} disabled={!!scenarioId}>
            <SelectTrigger><SelectValue placeholder="시나리오 선택" /></SelectTrigger>
            <SelectContent>{scenarios.map(s => <SelectItem key={s.id} value={s.id}>{s.id}{s.title ? ` — ${s.title}` : ''}</SelectItem>)}</SelectContent>
          </Select>
        </FormField>
        <FormField label="토폴로지" required help="대상(SUT)·워커·풀. [시험 > 토폴로지] 에서 편집">
          <Select value={topology} onValueChange={setTopology}>
            <SelectTrigger><SelectValue placeholder="토폴로지 선택" /></SelectTrigger>
            <SelectContent>{topologies.map(t => <SelectItem key={t.id} value={String(t.id)}>{t.name} <span className="text-muted-foreground">· {t.doc.target?.csp?.ip} · 워커 {t.doc.workers?.length ?? 0}</span></SelectItem>)}</SelectContent>
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
          <FormField label="라벨" help="보고서·비교에 표시 (예: CSP 0.2.126 회귀)">
            <Input value={label} onChange={e => setLabel(e.target.value)} maxLength={120} />
          </FormField>
        </div>
        {err && <div className="text-sm text-destructive">{err}</div>}
        <div className="flex items-center justify-end gap-2 border-t border-border pt-3">
          {topologies.length === 0 && <Badge variant="warningSoft">토폴로지가 없습니다 — 먼저 만드십시오</Badge>}
          <Button variant="outline" onClick={onClose}>닫기</Button>
          <Button variant="default" onClick={start} disabled={!canStart}><Play size={13} /> {busy ? '시작 중…' : '시작'}</Button>
        </div>
      </div>
    </Modal>
  )
}
