// ──────────────────────────────────────────────────────────────
//  NetTuningPanel — 서버별 네트워크 튜닝(RPS + sysctl) 설정/적용.
//  배경: 단일 NIC 큐 + RPS off → RX softirq 가 IRQ 코어 1개에 집중 → 고RTP 시
//  ksoftirqd 포화 → 네트워크 stall(8코어여도 1코어 천장). RPS 로 softirq 분산.
//  적용 시 agent: sysctl=/etc/sysctl.d 영속, RPS=sysfs 적용+부팅 재적용.
// ──────────────────────────────────────────────────────────────
import { useState } from 'react'
import type { Agent, AgentNetTuning } from '../../api/deployment'
import { ImeSafeInput } from './ImeSafeInput'
import { btnSmall } from './styles'

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

  const inputStyle = { width: 140, padding: '2px 6px', fontSize: 12, border: '1px solid var(--border)', borderRadius: 3, fontFamily: 'monospace' }

  return (
    <div style={{ borderLeft: '3px solid var(--border)', borderRadius: 4, padding: '10px 12px', background: 'var(--bg-soft)' }}>
      <div style={{ fontSize: 12, fontWeight: 'bold', color: 'var(--text-muted)', marginBottom: 8 }}>
        {title}
        <span style={{ marginLeft: 8, fontSize: 11, fontWeight: 'normal' }}>
          (sysctl 은 /etc/sysctl.d 영속 · RPS 는 적용+부팅 재적용 · 이 서버 {cores}코어)
        </span>
      </div>

      {/* RPS */}
      <div style={{ fontSize: 12, fontWeight: 'bold', margin: '6px 0 4px' }}>
        RPS — RX softirq 코어 분산 <span style={{ fontWeight: 'normal', color: 'var(--text-muted)' }}>(16진 비트마스크, 권장 전체코어=<code>{recMask}</code>, <code>0</code>=비활성)</span>
      </div>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12, marginBottom: 8 }}>
        <thead>
          <tr style={{ background: 'var(--surface-2)', color: 'var(--text-muted)' }}>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 120 }}>인터페이스</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 180 }}>rps_cpus 마스크</th>
            <th style={{ padding: '4px 8px', textAlign: 'left' }}>현재 저장값</th>
          </tr>
        </thead>
        <tbody>
          {ifaces.length === 0 && (
            <tr><td colSpan={3} style={{ padding: 8, color: 'var(--text-muted)' }}>(인터페이스 정보 없음 — heartbeat 대기)</td></tr>
          )}
          {ifaces.map(name => {
            const cur = stored?.rps?.find(r => r.iface === name)?.cpus
            return (
              <tr key={name}>
                <td style={{ padding: '4px 8px', fontFamily: 'monospace' }}>{name}</td>
                <td style={{ padding: '4px 8px' }}>
                  <ImeSafeInput value={rps[name] ?? ''} onCommit={v => setRps(p => ({ ...p, [name]: v }))}
                                placeholder={recMask} style={inputStyle} />
                  <button onClick={() => setRps(p => ({ ...p, [name]: recMask }))}
                          style={{ ...btnSmall(), marginLeft: 4 }} disabled={applying}>전체코어</button>
                </td>
                <td style={{ padding: '4px 8px', fontFamily: 'monospace', color: 'var(--text-muted)' }}>{cur ?? '-'}</td>
              </tr>
            )
          })}
        </tbody>
      </table>

      {/* sysctl */}
      <div style={{ fontSize: 12, fontWeight: 'bold', margin: '6px 0 4px' }}>sysctl (net.core.*)</div>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ background: 'var(--surface-2)', color: 'var(--text-muted)' }}>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 180 }}>키</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 160 }}>값</th>
            <th style={{ padding: '4px 8px', textAlign: 'left' }}>설명</th>
          </tr>
        </thead>
        <tbody>
          {SYSCTL_FIELDS.map(f => (
            <tr key={f.key}>
              <td style={{ padding: '4px 8px', fontFamily: 'monospace' }}>{f.label}</td>
              <td style={{ padding: '4px 8px' }}>
                <ImeSafeInput value={sysctl[f.key] ?? ''} onCommit={v => setSysctl(p => ({ ...p, [f.key]: v }))}
                              placeholder={String(f.def)} style={inputStyle} />
              </td>
              <td style={{ padding: '4px 8px', fontSize: 11, color: 'var(--text-muted)' }}>{f.hint}</td>
            </tr>
          ))}
        </tbody>
      </table>

      <div style={{ marginTop: 10 }}>
        <button onClick={apply} style={btnSmall()} disabled={applying}>
          {applying ? '적용 중…' : '＋ 네트워크 튜닝 적용'}
        </button>
      </div>
    </div>
  )
}
