// ──────────────────────────────────────────────────────────────
//  NetTuningPanel — 서버별 네트워크 튜닝(RPS + sysctl) 설정/적용.
//  배경: 단일 NIC 큐 + RPS off → RX softirq 가 IRQ 코어 1개에 집중 → 고RTP 시
//  ksoftirqd 포화 → 네트워크 stall(8코어여도 1코어 천장). RPS 로 softirq 분산.
//  적용 시 agent: sysctl=/etc/sysctl.d 영속, RPS=sysfs 적용+부팅 재적용.
// ──────────────────────────────────────────────────────────────
import { useState } from 'react'
import type { Agent, AgentNetTuning } from '../../api/deployment'
import { ImeSafeInput } from './ImeSafeInput'
import { Button } from '../../components/ui/button'
import { DataTable, Th, Td, orDash } from '../../components/custom/data-table'

const SYSCTL_FIELDS: Array<{ key: string; label: string; def: number; hint: string }> = [
  { key: 'net.core.netdev_max_backlog', label: 'netdev_max_backlog', def: 5000,     hint: 'RX backlog 큐 길이 (커널기본 1000) — softirq 적체 시 드롭 방지' },
  { key: 'net.core.netdev_budget',      label: 'netdev_budget',      def: 600,      hint: 'softirq 1회 처리 패킷 수 (커널기본 300)' },
  { key: 'net.core.rmem_max',           label: 'rmem_max',           def: 16777216, hint: '수신 소켓버퍼 최대 bytes' },
  { key: 'net.core.wmem_max',           label: 'wmem_max',           def: 16777216, hint: '송신 소켓버퍼 최대 bytes' },
]

// 코어 수 → 전체코어 16진 비트마스크 (8→"ff", 4→"f")
function allCoresMask(cores: number): string {
  const n = Math.max(1, Math.min(cores || 1, 31))
  return ((Math.pow(2, n) - 1) >>> 0).toString(16)
}

export function NetTuningPanel({ title, agent, applying, onApply }: {
  title: string
  agent: Agent
  applying?: boolean
  onApply: (tuning: AgentNetTuning, label: string) => void
}) {
  const stored = agent.net_tuning || null
  const cores = agent.cpu_cores || 1
  const recMask = allCoresMask(cores)

  // 고유 iface 목록 (interfaces 는 IP별 중복 가능 → name 으로 dedup, lo 제외)
  const ifaces = Array.from(new Set((agent.interfaces || [])
    .map(i => i.name).filter(n => !!n && n !== 'lo')))

  const [sysctl, setSysctl] = useState<Record<string, string>>(() => {
    const init: Record<string, string> = {}
    for (const f of SYSCTL_FIELDS) {
      const cur = stored?.sysctl?.[f.key]
      init[f.key] = String(cur ?? f.def)
    }
    return init
  })
  const [rps, setRps] = useState<Record<string, string>>(() => {
    const init: Record<string, string> = {}
    for (const name of ifaces) {
      const cur = stored?.rps?.find(r => r.iface === name)?.cpus
      init[name] = cur ?? ''
    }
    return init
  })

  const apply = () => {
    const sysctlOut: Record<string, number> = {}
    for (const f of SYSCTL_FIELDS) {
      const v = parseInt(sysctl[f.key], 10)
      if (!isNaN(v) && v >= 0) sysctlOut[f.key] = v
    }
    const rpsOut = ifaces
      .filter(name => (rps[name] || '').trim() !== '')
      .map(name => ({ iface: name, cpus: rps[name].trim() }))
    if (Object.keys(sysctlOut).length === 0 && rpsOut.length === 0) return
    onApply({ sysctl: sysctlOut, rps: rpsOut },
            `net-tuning: sysctl ${Object.keys(sysctlOut).length} / rps ${rpsOut.length}`)
  }

  return (
    // 상위 SubSection 이 제목·힌트·들여쓰기를 그린다 — 회색 패널을 또 두르지 않는다.
    <div>
      {title && (
        <div className="mb-2 text-sm font-semibold text-muted-foreground">
          {title}
          <span className="ml-2 text-xs font-normal">
            (sysctl 은 /etc/sysctl.d 영속 · RPS 는 적용+부팅 재적용 · 이 서버 {cores}코어)
          </span>
        </div>
      )}

      {/* RPS */}
      <div className="mb-1 mt-1.5 text-sm font-semibold">
        RPS — RX softirq 코어 분산{' '}
        <span className="font-normal text-muted-foreground">
          (16진 비트마스크, 권장 전체코어=<code className="font-mono">{recMask}</code>,{' '}
          <code className="font-mono">0</code>=비활성)
        </span>
      </div>
      <DataTable className="mb-3">
        <thead>
          <tr>
            <Th width={120}>인터페이스</Th>
            <Th width={260}>rps_cpus 마스크</Th>
            <Th>현재 저장값</Th>
          </tr>
        </thead>
        <tbody>
          {ifaces.length === 0 && (
            <tr><Td colSpan={3} className="text-muted-foreground">
              인터페이스 정보 없음 — heartbeat 대기
            </Td></tr>
          )}
          {ifaces.map(name => {
            const cur = stored?.rps?.find(r => r.iface === name)?.cpus
            return (
              <tr key={name}>
                <Td mono>{name}</Td>
                <Td>
                  <div className="flex items-center gap-1.5">
                    <span className="inline-block w-[140px]">
                      <ImeSafeInput value={rps[name] ?? ''} onCommit={v => setRps(p => ({ ...p, [name]: v }))}
                                    placeholder={recMask} className="font-mono" />
                    </span>
                    <Button variant="ghost" disabled={applying}
                            onClick={() => setRps(p => ({ ...p, [name]: recMask }))}>전체코어</Button>
                  </div>
                </Td>
                <Td mono className="text-muted-foreground">{orDash(cur)}</Td>
              </tr>
            )
          })}
        </tbody>
      </DataTable>

      {/* sysctl */}
      <div className="mb-1 mt-1.5 text-sm font-semibold">sysctl (net.core.*)</div>
      <DataTable>
        <thead>
          <tr>
            <Th width={180}>키</Th>
            <Th width={160}>값</Th>
            <Th>설명</Th>
          </tr>
        </thead>
        <tbody>
          {SYSCTL_FIELDS.map(f => (
            <tr key={f.key}>
              <Td mono>{f.label}</Td>
              <Td>
                <span className="inline-block w-[140px]">
                  <ImeSafeInput value={sysctl[f.key] ?? ''} onCommit={v => setSysctl(p => ({ ...p, [f.key]: v }))}
                                placeholder={String(f.def)} className="font-mono" />
                </span>
              </Td>
              <Td className="text-xs font-normal text-muted-foreground">{f.hint}</Td>
            </tr>
          ))}
        </tbody>
      </DataTable>

      <Button variant="outline" className="mt-2.5" onClick={apply} disabled={applying}>
        {applying ? '적용 중…' : '네트워크 튜닝 적용'}
      </Button>
    </div>
  )
}
