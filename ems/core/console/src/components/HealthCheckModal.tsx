import { useCallback, useEffect, useState } from 'react'
import { deploymentApi, type Agent, type AgentHealthCheck } from '../api/deployment'
import Modal from './Modal'
import { AlertTriangle, Check, ChevronDown, ChevronRight, RotateCw, X } from 'lucide-react'
import { Alert } from './ui/alert'
import { Badge } from './ui/badge'
import { Button } from './ui/button'
import { DataTable, Th, Td, orDash } from './custom/data-table'
import { agentDisplayName } from './agentDisplay'

// 단일 agent 의 on-demand 점검 결과 패널 (모달 wrapper 없음 — 재사용 단위).
// agent sync REST (/health-check) 를 csc admin proxy 로 호출.
function HealthCheckPanel({ agent }: { agent: Agent }) {
  const [data, setData] = useState<AgentHealthCheck | null>(null)
  const [err,  setErr]  = useState<string>('')
  const [loading, setLoading] = useState(true)

  const refresh = useCallback(async () => {
    setLoading(true); setErr(''); setData(null)
    try {
      const r = await deploymentApi.healthCheck(agent.id, 'all')
      setData(r)
    } catch (e) { setErr((e as Error).message) }
    finally { setLoading(false) }
  }, [agent.id])

  useEffect(() => { void refresh() }, [refresh])

  // 판정은 눈에 띄어야 하는 심각도라 Solid 배지다 (시안 실측 `#16a34a` 채움 + 흰 글자).
  const verdictTone = data?.verdict === 'healthy' ? 'successSolid' as const
                    : data?.verdict === 'partial' ? 'warningSolid' as const
                    : data?.verdict === 'broken'  ? 'dangerSolid' as const
                    : 'neutralSolid' as const

  return (
    <div>
      <div className="mb-3 flex items-center gap-3">
        <span className="font-semibold">{agentDisplayName(agent.name)}</span>
        {data && <Badge variant={verdictTone}>{data.verdict}</Badge>}
        {data?.ts && <span className="text-xs text-muted-foreground">ts: {data.ts}</span>}
        <Button variant="outline" className="ml-auto" onClick={() => void refresh()}>
          <RotateCw /> 새로고침
        </Button>
      </div>
      {loading && <div className="text-sm text-muted-foreground">점검 중…</div>}
      {err && (
        <div className="flex items-center gap-1.5 text-sm text-destructive">
          <AlertTriangle size={13} /> {err}
        </div>
      )}
      {data && (
        <div className="flex flex-col gap-4">
          {/* issues */}
          {data.issues.length > 0 && (
            <Alert variant="warning">
              <div className="font-medium">발견된 이슈</div>
              <ul className="mt-1 list-disc pl-5 text-xs">
                {data.issues.map((it, i) => <li key={i}>{it}</li>)}
              </ul>
            </Alert>
          )}
          {/* HA */}
          {data.ha && (
            <div>
              <div className="mb-1 text-md font-semibold">HA (keepalived)</div>
              <div className="text-md text-muted-foreground">
                {data.ha.keepalived_installed
                  ? (data.ha.keepalived_active
                      ? <span className="inline-flex items-center gap-1.5">
                          <Check size={13} className="text-[var(--cims-success)]" />
                          <code className="font-mono">keepalived</code> active
                        </span>
                      : <span className="inline-flex items-center gap-1.5 text-destructive">
                          <X size={13} /> keepalived installed but inactive
                        </span>)
                  : <span>(keepalived 미설치)</span>}
              </div>
              {data.ha.vips.length > 0 && (
                <div className="mt-1 font-mono text-xs text-muted-foreground">
                  VIP: {data.ha.vips.map(v => `${v.iface}:${v.ip}/${v.mask}`).join(', ')}
                </div>
              )}
              {data.ha.journal_tail && data.ha.journal_tail.length > 0 && (
                <details className="group mt-1.5">
                  {/* 네이티브 마커(삼각형 글리프)를 끄고 Lucide 로 바꾼다 */}
                  <summary className="flex cursor-pointer list-none items-center gap-1 text-xs text-muted-foreground [&::-webkit-details-marker]:hidden">
                    <ChevronRight size={12} className="group-open:hidden" />
                    <ChevronDown size={12} className="hidden group-open:inline" />
                    journal tail ({data.ha.journal_tail.length} lines)
                  </summary>
                  <pre className="mt-1 max-h-[200px] overflow-auto rounded-sm border border-border bg-muted p-2 font-mono text-xs">
                    {data.ha.journal_tail.join('\n')}
                  </pre>
                </details>
              )}
            </div>
          )}
          {/* Modules */}
          {data.modules && (
            <div>
              <div className="mb-1 text-md font-semibold">모듈</div>
              <DataTable>
                <thead>
                  <tr>
                    <Th width={150}>이름</Th>
                    <Th width={90}>실행</Th>
                    <Th width={150}>PID</Th>
                    <Th width={110}>CPU%</Th>
                    <Th width={130}>RSS(MB)</Th>
                    <Th>UPTIME</Th>
                  </tr>
                </thead>
                <tbody>
                  {data.modules.map(m => (
                    <tr key={m.name}>
                      <Td>{m.name}</Td>
                      <Td>{m.running
                        ? <Check size={14} className="text-[var(--cims-success)]" aria-label="실행 중" />
                        : <span className="text-muted-foreground">—</span>}</Td>
                      <Td mono>{orDash(m.pid)}</Td>
                      <Td mono>{orDash(m.cpu_pct)}</Td>
                      <Td mono>{orDash(m.mem_mb)}</Td>
                      <Td mono>{m.uptime_sec != null ? `${Math.floor(m.uptime_sec / 60)}m` : '—'}</Td>
                    </tr>
                  ))}
                </tbody>
              </DataTable>
            </div>
          )}
          {/* Metrics */}
          {data.metrics && (
            <div>
              <div className="mb-1 text-md font-semibold">시스템 메트릭</div>
              <div className="mb-1.5 text-md text-muted-foreground">
                MEM: {data.metrics.mem_pct ?? '—'}% · Disk: {data.metrics.disk_pct ?? '—'}% ·
                {' '}Load: {data.metrics.load_avg ?? '—'}
              </div>
              {data.metrics.per_iface && data.metrics.per_iface.length > 0 && (
                <DataTable>
                  <thead>
                    <tr>
                      <Th width={150}>IFACE</Th>
                      <Th width={190}>RX RATE</Th>
                      <Th width={190}>TX RATE</Th>
                      <Th width={190}>RX ERRORS</Th>
                      <Th>TX ERRORS</Th>
                    </tr>
                  </thead>
                  <tbody>
                    {data.metrics.per_iface.map(i => (
                      <tr key={i.name}>
                        <Td mono>{i.name}</Td>
                        <Td mono>{i.rx_rate != null ? `${(i.rx_rate / 1024).toFixed(1)} KB/s` : '—'}</Td>
                        <Td mono>{i.tx_rate != null ? `${(i.tx_rate / 1024).toFixed(1)} KB/s` : '—'}</Td>
                        <Td mono>{i.rx_errors}</Td>
                        <Td mono>{i.tx_errors}</Td>
                      </tr>
                    ))}
                  </tbody>
                </DataTable>
              )}
            </div>
          )}
          {data.agent_version && (
            <div className="text-xs text-muted-foreground">
              agent v{data.agent_version} · {data.hostname}
            </div>
          )}
        </div>
      )}
    </div>
  )
}

// 1개(서버 단위) 또는 N개(HA 그룹 멤버) agent 를 한 모달에 점검.
export default function HealthCheckModal({ agents, onClose }: { agents: Agent[]; onClose: () => void }) {
  const title = agents.length === 1
    ? `${agentDisplayName(agents[0].name)} — 실시간 점검 (sync REST)`
    : `그룹 점검 — ${agents.map(a => agentDisplayName(a.name)).join(', ')}`
  return (
    // 시안 M2(194:3219) — 푸터에 [닫기] Primary 하나 (M1 과 달리 여기는 버튼이 있다).
    <Modal title={title} onClose={onClose} width={agents.length > 1 ? 1000 : 960}>
      <div className="flex flex-col gap-5">
        {agents.map((a, i) => (
          <div key={a.id} className={i > 0 ? 'border-t border-border pt-4' : undefined}>
            <HealthCheckPanel agent={a} />
          </div>
        ))}
      </div>
      <div className="mt-4 flex justify-end">
        <Button variant="default" onClick={onClose}>닫기</Button>
      </div>
    </Modal>
  )
}
