// 워커·대상 띠 — 토폴로지의 워커마다 SIP 단말·RTP 스트림·CPU 막대, 최대 SApS·진행 run·시계 오차(미응답은 점선), 대상 카드는 접속점·
// 빌드(진행 run 의 target_build)·노드 목록 (test_instrument.md §7 실행 라이브 ①). 워커 health 는 GET /workers?topology= (병렬 probe).
import { useCallback, useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { RefreshCw, Server, Cpu, Radio, ArrowRight } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { StatusDot } from '@core/components/custom/status-dot'
import { testerApi, type WorkerRow, type TopologyRow } from '@tester/api/tester'
import { fmtNum } from '@tester/lib/fmt'

function Bar({ v, max, tone }: { v: number | null | undefined; max: number | null | undefined; tone?: 'warning' | 'danger' }) {
  const pct = v != null && max ? Math.min(100, (100 * v) / max) : 0
  const cls = tone === 'danger' ? 'bg-destructive' : tone === 'warning' ? 'bg-warning' : 'bg-primary'
  return <div className="h-1.5 w-full overflow-hidden rounded-sm bg-neutral-soft"><div className={`h-full ${cls}`} style={{ width: `${pct}%` }} /></div>
}

export default function WorkerFleet({ topology, targetBuild, activeRunId }: {
  topology: TopologyRow | null
  targetBuild?: string | null
  activeRunId?: string | null
}) {
  const nav = useNavigate()
  const [workers, setWorkers] = useState<WorkerRow[]>([])
  const [busy, setBusy] = useState(false)
  const load = useCallback(async () => {
    if (!topology) { setWorkers([]); return }
    setBusy(true)
    try { const r = await testerApi.workers(topology.id); setWorkers(r.workers) } catch { setWorkers([]) } finally { setBusy(false) }
  }, [topology])
  useEffect(() => { load() }, [load])
  useEffect(() => {
    if (!topology) return
    const id = window.setInterval(load, activeRunId ? 5000 : 30000)
    return () => window.clearInterval(id)
  }, [load, topology, activeRunId])

  if (!topology) return null
  const doc = topology.doc
  const sipNodes = Object.entries(doc.target?.nodes ?? {}).filter(([, n]) => n.role === 'sip')
  const nodeCount = Object.keys(doc.target?.nodes ?? {}).length

  return (
    <section className="rounded-md border border-border bg-card p-3">
      <div className="mb-2 flex flex-wrap items-center gap-2">
        <h3 className="text-sm font-semibold text-muted-foreground">워커 · 대상</h3>
        <span className="font-mono text-xs text-muted-foreground">{topology.name}</span>
        <div className="ml-auto flex items-center gap-1.5">
          <Button variant="ghost" size="sm" onClick={load} disabled={busy}><RefreshCw size={12} /></Button>
          <Button variant="ghost" size="sm" onClick={() => nav('/test/topologies')}>토폴로지 <ArrowRight size={12} /></Button>
        </div>
      </div>
      <div className="grid gap-2 sm:grid-cols-2 xl:grid-cols-4">
        {workers.map(w => {
          const h = w.health
          const ep = h?.active_endpoints ?? 0, epMax = h?.max_endpoints ?? null
          const rtp = h?.media?.rtp_streams ?? null, rtpMax = h?.media?.max_rtp_streams ?? w.media?.max_rtp_streams ?? null
          const cpu = h?.cpu_pct ?? null
          const skew = h?.clock_skew_ms ?? 0
          return (
            <div key={w.name} className={`rounded-md border p-2.5 ${w.up ? 'border-border' : 'border-dashed border-destructive'}`}>
              <div className="flex items-center gap-1.5 text-sm">
                <Server size={13} className="text-muted-foreground" />
                <span className="font-semibold">{w.name}</span>
                <span className="font-mono text-xs text-muted-foreground">{w.url?.replace(/^https?:\/\//, '')}</span>
                <span className="ml-auto"><StatusDot tone={w.up ? 'success' : 'danger'} label={w.up ? `v${h?.version ?? ''}` : (w.error ?? '미응답')} /></span>
              </div>
              {w.up && (
                <div className="mt-1.5 flex flex-col gap-1 text-xs">
                  <div className="flex items-center gap-2"><span className="w-14 text-muted-foreground">SIP 단말</span><Bar v={ep} max={epMax} /><span className="w-24 text-right font-mono">{fmtNum(ep, 0)} / {fmtNum(epMax, 0)}</span></div>
                  <div className="flex items-center gap-2"><span className="w-14 text-muted-foreground">RTP</span><Bar v={rtp} max={rtpMax} /><span className="w-24 text-right font-mono">{rtp == null ? '—' : fmtNum(rtp, 0)} / {fmtNum(rtpMax, 0)}</span></div>
                  <div className="flex items-center gap-2"><span className="w-14 text-muted-foreground">CPU</span><Bar v={cpu} max={100} tone={cpu != null && cpu > 85 ? 'danger' : cpu != null && cpu > 70 ? 'warning' : undefined} /><span className="w-24 text-right font-mono">{fmtNum(cpu, 0)} %</span></div>
                  <div className="flex flex-wrap items-center gap-1.5 text-muted-foreground">
                    <span>최대 {fmtNum(h?.max_saps, 0)} SApS</span>
                    {w.cpus ? <span>· {w.cpus} cpu</span> : null}
                    {h?.active_run ? <Badge variant="infoSoft">run {h.active_run}</Badge> : null}
                    {Math.abs(skew) > 50 ? <Badge variant="warningSoft">시계 {skew} ms</Badge> : <span>· 시계 {skew} ms</span>}
                  </div>
                </div>
              )}
            </div>
          )
        })}
        <div className="rounded-md border border-border p-2.5">
          <div className="flex items-center gap-1.5 text-sm">
            <Radio size={13} className="text-muted-foreground" />
            <span className="font-semibold">{doc.target?.name ?? '대상'}</span>
            <Badge variant="neutralSoft">{doc.target?.kind ?? 'cims'}</Badge>
            <span className="ml-auto text-xs text-muted-foreground">노드 {nodeCount}</span>
          </div>
          <div className="mt-1.5 flex flex-col gap-0.5 text-xs">
            {sipNodes.map(([id, n]) => {
              const ip = doc.hosts?.[n.host]?.ip ?? n.host
              const acc = n.sip?.access
              const ports = acc ? (['udp', 'tcp', 'tls'] as const).filter(k => acc[k]).map(k => `${k} ${acc[k]}`).join(' · ') : ''
              return <div key={id} className="font-mono"><span className="text-muted-foreground">{n.fn ?? id}</span> {ip} {ports}{n.sip?.peering ? ` · peering ${n.sip.peering.port}` : ''}</div>
            })}
            <div className="text-muted-foreground"><Cpu size={11} className="mr-1 inline" />빌드 <span className="font-mono text-foreground">{targetBuild ?? '(run 시작 시 기록)'}</span></div>
          </div>
        </div>
      </div>
    </section>
  )
}
