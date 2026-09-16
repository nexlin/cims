// 시험 > 시나리오 — 패키지 동봉 + 운영자 추가 시나리오·부하 프로파일 목록. 검증 오류가 있는
// 파일도 숨기지 않고 오류와 함께 보인다(조용히 사라지면 오타 난 시나리오를 못 찾는다).
import { useCallback, useEffect, useState } from 'react'
import { RefreshCw } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { DataTable, Th, Td, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { testerApi, type ScenarioRow, type ProfileRow } from '@tester/api/tester'

export default function TesterScenariosPage() {
  const [scenarios, setScenarios] = useState<ScenarioRow[]>([])
  const [profiles, setProfiles] = useState<ProfileRow[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [s, p] = await Promise.all([testerApi.scenarios(), testerApi.profiles()])
      setScenarios(s.scenarios); setProfiles(p.profiles); setError(null)
    } catch (e) { setError(String(e)) }
    finally { setLoading(false) }
  }, [])
  useEffect(() => { load() }, [load])

  return (
    <div className="flex h-full flex-col">
      <div className="toolbar flex flex-wrap items-center gap-2.5 border-b border-border bg-muted px-4 py-3">
        <span className="whitespace-nowrap text-md font-semibold text-foreground">시나리오 · 부하 프로파일</span>
        <span className="text-sm text-muted-foreground">시나리오 {scenarios.length} · 프로파일 {profiles.length}</span>
        {error && <span className="text-sm text-destructive">{error}</span>}
        <div className="ml-auto">
          <Button variant="outline" size="sm" onClick={load} disabled={loading}><RefreshCw size={13} /> 새로고침</Button>
        </div>
      </div>

      <div className="page-scroll flex-1 min-h-0 overflow-auto p-4 flex flex-col gap-4">
        <section className="flex flex-col gap-2">
          <h3 className="text-sm font-semibold text-muted-foreground">시나리오</h3>
          {scenarios.length === 0 ? (
            <EmptyState title="시나리오가 없습니다" description="패키지 scenarios/ 또는 Tester.DataDir/scenarios/ 에 YAML 을 둡니다." />
          ) : (
            <DataTable>
              <thead>
                <tr><Th>id</Th><Th>제목</Th><Th>태그</Th><Th align="right">단계</Th><Th>출처</Th><Th>검증</Th></tr>
              </thead>
              <tbody>
                {scenarios.map(s => (
                  <tr key={s.id}>
                    <Td mono>{s.id}</Td>
                    <Td>{orDash(s.title)}</Td>
                    <Td><span className="flex flex-wrap gap-1">{s.tags.map(t => <Badge key={t} variant="neutralSoft">{t}</Badge>)}</span></Td>
                    <Td align="right">{s.steps}</Td>
                    <Td><Badge variant={s.source === 'user' ? 'brandSoft' : 'neutralSoft'}>{s.source}</Badge></Td>
                    <Td>
                      {s.errors.length === 0
                        ? <Badge variant="successSoft">OK</Badge>
                        : <span className="flex flex-col gap-0.5">{s.errors.map((e, i) => <span key={i} className="text-sm text-destructive">{e}</span>)}</span>}
                    </Td>
                  </tr>
                ))}
              </tbody>
            </DataTable>
          )}
        </section>

        <section className="flex flex-col gap-2">
          <h3 className="text-sm font-semibold text-muted-foreground">부하 프로파일 (ETSI TS 186 008)</h3>
          {profiles.length === 0 ? (
            <EmptyState title="프로파일이 없습니다" />
          ) : (
            <DataTable>
              <thead><tr><Th>이름</Th><Th>모델</Th><Th>출처</Th><Th>검증</Th></tr></thead>
              <tbody>
                {profiles.map(p => (
                  <tr key={p.name}>
                    <Td mono>{p.name}</Td>
                    <Td>{orDash(p.model)}</Td>
                    <Td><Badge variant={p.source === 'user' ? 'brandSoft' : 'neutralSoft'}>{p.source}</Badge></Td>
                    <Td>
                      {p.errors.length === 0
                        ? <Badge variant="successSoft">OK</Badge>
                        : <span className="flex flex-col gap-0.5">{p.errors.map((e, i) => <span key={i} className="text-sm text-destructive">{e}</span>)}</span>}
                    </Td>
                  </tr>
                ))}
              </tbody>
            </DataTable>
          )}
        </section>
      </div>
    </div>
  )
}
