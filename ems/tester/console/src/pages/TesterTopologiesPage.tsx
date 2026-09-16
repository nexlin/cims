// 시험 > 토폴로지 — 대상(CSP/CSC/OAM 주소·도메인)·워커·풀 정의 레코드(런타임 store) 편집 + 워커 상태·용량 + 연결 검사.
// 문서는 JSON(레코드 그대로)으로 편집하고 컨트롤러 스키마(topology)로 검증한다. 비밀(토큰·H(A1))은 환경변수 이름만 적는다.
import { useCallback, useEffect, useMemo, useState } from 'react'
import { RefreshCw, Plus, Save, Trash2, RotateCcw, PlugZap, Server } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { DataTable, Th, Td, TrLink, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { StatusDot } from '@core/components/custom/status-dot'
import { useToast } from '@core/components/Toast'
import { useConfirm } from '@core/components/custom/confirm'
import { useAuth } from '@core/contexts/AuthContext'
import { hasRole } from '@core/utils/permissions'
import { testerApi, type TopologyRow, type TopologyDoc, type WorkerRow, type CheckItem } from '@tester/api/tester'
import YamlEditor from '@tester/components/YamlEditor'
import { fmtNum, fmtTime } from '@tester/lib/fmt'

const NEW_DOC: TopologyDoc = {
  name: 'new-target',
  hosts: {
    h45: { name: 'media01', ip: '10.0.0.45' },
    h61: { name: 'tester-a', ip: '10.0.0.61' },
  },
  workers: [{ name: 'w1', host: 'h61', port: 7100, cpus: 8 }],
  target: {
    name: 'sut', kind: 'cims',
    nodes: {
      csp: { role: 'sip', fn: 'CSP', host: 'h45', procs: ['csp'],
             sip: { access: { udp: 5060, tcp: 25061, tls: 5061, domains: ['volte.cims.example.kr', 'ptt.cims.example.kr'] },
                    peering: { port: 5070, protocol: 'udp', local_node: 'cims-tester-peering' } } },
      cmp: { role: 'media', fn: 'CMP', host: 'h45', procs: ['cmp'], media: { rtp_range: [20000, 29999], control: 9001 } },
      csc: { role: 'subscriber', fn: 'CSC', host: 'h45', procs: ['csc'], api: { port: 4430, tls: true } },
      oam: { role: 'oam', fn: 'OAM', host: 'h45', procs: ['oam'], oam: { port: 4419, tls: true, token_env: 'TESTER_OAM_TOKEN' } },
    },
  },
  pools: {
    volte_ue: { kind: 'ue', worker: 'w1', access: 'csp', source: { creds: 'creds/volte.jsonl' }, transport: 'udp' },
  },
}

/** 표시용 — SIP 접속점 노드 첫 항목의 호스트 주소·기본 도메인 */
export function topoTargetLabel(doc: TopologyDoc): string {
  const sip = Object.values(doc.target?.nodes ?? {}).find(n => n.role === 'sip' && n.sip?.access)
  if (!sip) return doc.target?.name ?? '—'
  const ip = doc.hosts?.[sip.host]?.ip ?? sip.host
  const dom = sip.sip?.access?.domains?.[0]
  return dom ? `${ip} · ${dom}` : ip
}

function poolSummary(doc: TopologyDoc): string {
  return Object.entries(doc.pools ?? {}).map(([n, p]) => `${n}(${p.kind}${p.kind === 'peer' ? `/${p.profile}` : ''}@${p.worker})`).join(', ')
}

export default function TesterTopologiesPage() {
  const { show } = useToast()
  const confirm = useConfirm()
  const { user } = useAuth()
  const canWrite = hasRole(user, 'operator')
  const canDelete = hasRole(user, 'manager')
  const [rows, setRows] = useState<TopologyRow[]>([])
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const [sel, setSel] = useState<number | null | undefined>(undefined)     // null = 새 문서
  const [orig, setOrig] = useState('')
  const [text, setText] = useState('')
  const [valid, setValid] = useState(false)
  const [saving, setSaving] = useState(false)
  const [workers, setWorkers] = useState<WorkerRow[]>([])
  const [check, setCheck] = useState<{ ok: boolean; items: CheckItem[]; at: string } | null>(null)
  const [checking, setChecking] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try { const r = await testerApi.topologies(); setRows(r.topologies); setError(null) }
    catch (e) { setError(String(e)) } finally { setLoading(false) }
  }, [])
  useEffect(() => { load() }, [load])

  const current = useMemo(() => rows.find(r => r.id === sel) ?? null, [rows, sel])

  useEffect(() => {
    setCheck(null); setWorkers([])
    if (sel === undefined) return
    const doc = sel === null ? NEW_DOC : current?.doc
    const t = JSON.stringify(doc ?? {}, null, 2)
    setOrig(t); setText(t)
    if (sel !== null) testerApi.workers(sel).then(w => setWorkers(w.workers)).catch(() => {})
  }, [sel, current])

  const dirty = text !== orig

  const save = async () => {
    let doc: TopologyDoc
    try { doc = JSON.parse(text) } catch (e) { show(`JSON 파싱 실패: ${String(e)}`, 'err'); return }
    setSaving(true)
    try {
      const rec = sel === null ? await testerApi.createTopology(doc) : await testerApi.saveTopology(sel!, doc)
      show(sel === null ? '토폴로지 생성' : '저장', 'ok')
      await load(); setSel(rec.id)
    } catch (e) { show(String(e), 'err') } finally { setSaving(false) }
  }

  const del = async () => {
    if (!current) return
    if (!await confirm({ title: '토폴로지 삭제', body: `${current.name} 을 지웁니다. 이 토폴로지로 돌린 run 색인은 남습니다.`, confirmLabel: '삭제', tone: 'danger' })) return
    try { await testerApi.deleteTopology(current.id); show('삭제', 'ok'); setSel(undefined); load() } catch (e) { show(String(e), 'err') }
  }

  const runCheck = async () => {
    if (!current) return
    setChecking(true)
    try { const r = await testerApi.checkTopology(current.id); setCheck({ ok: r.ok, items: r.items, at: new Date().toISOString() }) }
    catch (e) { show(String(e), 'err') } finally { setChecking(false) }
    testerApi.workers(current.id).then(w => setWorkers(w.workers)).catch(() => {})
  }

  return (
    <div className="flex h-full flex-col">
      <div className="toolbar flex flex-wrap items-center gap-2.5 border-b border-border bg-muted px-4 py-3">
        <span className="whitespace-nowrap text-md font-semibold text-foreground">토폴로지</span>
        <span className="text-sm text-muted-foreground">호스트 › 워커·대상 노드 › 풀 — run 이 참조하는 레코드. 주소는 hosts 에만, 비밀은 환경변수 이름만</span>
        {error && <span className="text-sm text-destructive">{error}</span>}
        <div className="ml-auto flex items-center gap-2">
          <Button variant="outline" size="sm" onClick={load} disabled={loading}><RefreshCw size={13} /> 새로고침</Button>
          <Button variant="outline" size="sm" onClick={() => setSel(null)} disabled={!canWrite}><Plus size={13} /> 새 토폴로지</Button>
        </div>
      </div>

      <div className="page-scroll flex-1 min-h-0 overflow-auto p-4">
        <div className="grid gap-4 lg:grid-cols-[minmax(340px,2fr)_3fr]">
          <section className="flex flex-col gap-2">
            {rows.length === 0 ? (
              <EmptyState title="토폴로지가 없습니다" description="[새 토폴로지] 로 대상 CSP 주소·워커 URL·풀을 정의합니다. 예시 = 패키지 scenarios/topology.sample.yaml" />
            ) : (
              <DataTable>
                <thead><tr><Th>이름</Th><Th>대상 접속점</Th><Th>워커</Th><Th>풀</Th><Th>수정</Th></tr></thead>
                <tbody>
                  {rows.map(r => (
                    <TrLink key={r.id} selected={sel === r.id} onClick={() => setSel(r.id)}>
                      <Td><span className="font-medium">{r.name}</span> <span className="font-mono text-xs text-muted-foreground">#{r.id}</span></Td>
                      <Td mono>{topoTargetLabel(r.doc)}</Td>
                      <Td>{(r.doc.workers ?? []).map(w => `${w.name}@${r.doc.hosts?.[w.host]?.ip ?? w.host}`).join(', ') || '—'}</Td>
                      <Td className="text-xs">{poolSummary(r.doc)}</Td>
                      <Td mono>{fmtTime(r.updated_at, true)}</Td>
                    </TrLink>
                  ))}
                </tbody>
              </DataTable>
            )}

            {sel != null && current && (
              <>
                <h3 className="mt-2 flex items-center gap-1.5 text-sm font-semibold text-muted-foreground"><Server size={13} /> 워커 상태·용량</h3>
                {workers.length === 0 ? <EmptyState title="워커 정의 없음 또는 조회 중" /> : (
                  <DataTable>
                    <thead><tr><Th>워커</Th><Th>URL</Th><Th>상태</Th><Th align="right">단말</Th><Th align="right">최대 SApS</Th><Th align="right">CPU</Th><Th>진행 run</Th><Th align="right">시계 오차</Th></tr></thead>
                    <tbody>
                      {workers.map(w => (
                        <tr key={w.name}>
                          <Td>{w.name}</Td>
                          <Td mono className="text-xs">{w.url}</Td>
                          <Td><StatusDot tone={w.up ? 'success' : 'danger'} label={w.up ? `v${w.health?.version ?? ''}` : (w.error ?? '미응답')} /></Td>
                          <Td align="right" mono>{w.up ? `${w.health?.active_endpoints ?? 0} / ${w.health?.max_endpoints ?? '—'}` : '—'}</Td>
                          <Td align="right" mono>{w.up ? fmtNum(w.health?.max_saps, 0) : '—'}</Td>
                          <Td align="right" mono>{w.up ? `${fmtNum(w.health?.cpu_pct, 0)} %` : '—'}</Td>
                          <Td mono className="text-xs">{orDash(w.health?.active_run)}</Td>
                          <Td align="right" mono className={Math.abs(w.health?.clock_skew_ms ?? 0) > 50 ? 'text-warning' : ''}>{w.up ? `${w.health?.clock_skew_ms ?? 0} ms` : '—'}</Td>
                        </tr>
                      ))}
                    </tbody>
                  </DataTable>
                )}

                <div className="mt-2 flex items-center gap-2">
                  <h3 className="flex items-center gap-1.5 text-sm font-semibold text-muted-foreground"><PlugZap size={13} /> 연결 검사</h3>
                  <Button variant="outline" size="sm" onClick={runCheck} disabled={checking || !canWrite}>{checking ? '검사 중…' : '검사 실행'}</Button>
                  {check && <Badge variant={check.ok ? 'successSoft' : 'dangerSoft'}>{check.ok ? '전부 도달' : '미도달 있음'} · {fmtTime(check.at)}</Badge>}
                </div>
                {check && (
                  <DataTable>
                    <thead><tr><Th>항목</Th><Th width={60}>결과</Th><Th>상세</Th><Th align="right">ms</Th></tr></thead>
                    <tbody>
                      {check.items.map(i => (
                        <tr key={i.name}>
                          <Td mono>{i.name}</Td>
                          <Td>{i.ok ? <Badge variant="successSoft">OK</Badge> : i.info ? <Badge variant="neutralSoft">참고</Badge> : <Badge variant="dangerSoft">실패</Badge>}</Td>
                          <Td className="break-all text-xs">{i.detail}</Td>
                          <Td align="right" mono>{i.ms}</Td>
                        </tr>
                      ))}
                    </tbody>
                  </DataTable>
                )}
              </>
            )}
          </section>

          <section className="flex min-h-0 flex-col gap-2">
            {sel === undefined ? (
              <EmptyState title="토폴로지를 고르십시오" description="왼쪽에서 행을 누르면 JSON 문서를 편집합니다. 풀 하나 = 워커 하나(worker) — 워커 여럿에 나누려면 워커마다 풀 + 같은 group. 피어 풀(kind: peer)은 워커 호스트 주소:bind.port 에 수신점을 열고, 시나리오가 그 풀을 쓰면 run 이 대상 CSP 컬렉션을 시드·복원합니다." />
            ) : (
              <>
                <div className="flex flex-wrap items-center gap-2">
                  {current ? <span className="text-sm font-semibold">{current.name} <span className="font-mono text-xs text-muted-foreground">#{current.id}</span></span> : <span className="text-sm font-semibold">새 토폴로지</span>}
                  {dirty && <Badge variant="warningSoft">변경됨</Badge>}
                  <div className="ml-auto flex items-center gap-2">
                    <Button variant="outline" size="sm" onClick={() => setText(orig)} disabled={!dirty}><RotateCcw size={13} /> 되돌리기</Button>
                    {current && canDelete && <Button variant="destructive" size="sm" onClick={del}><Trash2 size={13} /> 삭제</Button>}
                    <Button variant="default" size="sm" onClick={save} disabled={!canWrite || saving || !valid || (!dirty && sel !== null)}><Save size={13} /> {sel === null ? '생성' : '저장'}</Button>
                  </div>
                </div>
                <YamlEditor kind="topology" mode="json" value={text} onChange={setText} disabled={!canWrite} onValid={setValid} minHeight={520} />
              </>
            )}
          </section>
        </div>
      </div>
    </div>
  )
}
