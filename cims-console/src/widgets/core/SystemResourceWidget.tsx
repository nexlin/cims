// 코어 위젯 — 시스템 리소스 모니터링. 서버(agent)별 CPU/MEM/DISK 현재값 바 + 임계색.
// agent heartbeat metric(2s 수집) 의 최신값. 시스템 형상 위젯 옆 배치용(½폭). 자체 폴링 15s.
import { useState, useEffect, useCallback } from 'react'
import { useNavigate } from 'react-router-dom'
import { deploymentApi, type Agent, type AgentMetric } from '../../api/deployment'
import type { WidgetDef } from '../types'

interface Row { id: number; host: string; online: boolean; cpu: number | null; mem: number | null; disk: number | null }
const WARN = { cpu: 85, mem: 90, disk: 90 }

function barColor(v: number | null, warn: number): string {
  if (v == null) return 'var(--border)'
  if (v >= warn) return '#e74c3c'
  if (v >= warn - 15) return '#f59e0b'
  return '#22c55e'
}

function Bar({ v, warn }: { v: number | null; warn: number }) {
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
      <div style={{ flex: 1, height: 8, background: 'var(--surface-2)', borderRadius: 4, overflow: 'hidden', minWidth: 36 }}>
        <div style={{ width: `${Math.min(v ?? 0, 100)}%`, height: '100%', background: barColor(v, warn) }} />
      </div>
      <span style={{ fontSize: 11, width: 34, textAlign: 'right', color: v != null && v >= warn ? '#e74c3c' : 'var(--text-muted)' }}>
        {v != null ? `${Math.round(v)}%` : '—'}
      </span>
    </div>
  )
}

function latest(items: AgentMetric[]): AgentMetric | null {
  if (!items || items.length === 0) return null
  return items.reduce((a, b) => ((a.ts || '') >= (b.ts || '') ? a : b))
}

function SystemResourceWidget() {
  const navigate = useNavigate()
  const [rows, setRows] = useState<Row[]>([])
  const [loaded, setLoaded] = useState(false)   // 최초 로드 완료 여부 (loading vs empty 구분)
  const [stale, setStale] = useState(false)      // 직전 갱신이 일시 오류였는지 (flapping)

  const load = useCallback(async () => {
    let agents: Agent[]
    try {
      agents = (await deploymentApi.listAgents()).filter(a => a.status !== 'revoked')
    } catch {
      // 목록 조회 실패(엔드포인트 flapping) — 기존 데이터 유지, 위젯은 사라지지 않음.
      setStale(true); setLoaded(true); return
    }
    // 1) 서버 목록을 즉시 표시(메트릭 null) — metrics 가 느려도 표가 걸리지 않게.
    const base = (a: Agent): Row => ({ id: a.id, host: a.name, online: a.status === 'online', cpu: null, mem: null, disk: null })
    setRows(agents.map(base)); setStale(false); setLoaded(true)
    // 2) online 에이전트 메트릭을 타임아웃(4s) 가드로 보강 — 느린/빈 응답에 행 걸리지 않게.
    const withTimeout = <T,>(p: Promise<T>, ms: number) =>
      Promise.race([p, new Promise<null>(res => setTimeout(() => res(null), ms))])
    const out = await Promise.all(agents.map(async (a: Agent): Promise<Row> => {
      if (a.status !== 'online') return base(a)
      try {
        const r = await withTimeout(deploymentApi.agentMetrics(a.id), 4000)
        const m = r ? latest(r.items) : null
        return { ...base(a), cpu: m?.cpu_pct ?? null, mem: m?.mem_pct ?? null, disk: m?.disk_pct ?? null }
      } catch { return base(a) }
    }))
    setRows(out)
  }, [])

  useEffect(() => { load(); const iv = setInterval(load, 15000); return () => clearInterval(iv) }, [load])

  return (
    <div className="panel" style={{ padding: 16 }}>
      <div style={{ fontWeight: 600, marginBottom: 10, fontSize: 14, display: 'flex', alignItems: 'center' }}>
        시스템 리소스 ({rows.length})
        {stale && <span title="갱신 일시 실패 — 직전 값 표시" style={{ marginLeft: 6, fontSize: 11, color: 'var(--warning, #f59e0b)' }}>⚠ 갱신 지연</span>}
        <a onClick={() => navigate('/deploy/servers')}
           style={{ marginLeft: 'auto', fontSize: 12, fontWeight: 500, color: 'var(--primary)', cursor: 'pointer' }}>서버 →</a>
      </div>
      {rows.length === 0 ? (
        <div style={{ padding: '20px 4px', fontSize: 13, color: 'var(--text-muted)', textAlign: 'center' }}>
          {loaded ? '표시할 서버가 없습니다.' : '불러오는 중…'}
        </div>
      ) : (
      <table className="data-table" style={{ fontSize: 12 }}>
        <thead><tr><th>서버</th><th style={{ width: '26%' }}>CPU</th><th style={{ width: '26%' }}>MEM</th><th style={{ width: '26%' }}>DISK</th></tr></thead>
        <tbody>
          {rows.map(r => (
            <tr key={r.id} style={{ cursor: 'pointer', opacity: r.online ? 1 : 0.5 }}
                onClick={() => navigate(`/deploy/servers?agent=${r.id}`)}>
              <td><b>{r.host}</b>{!r.online && <span style={{ fontSize: 10, color: 'var(--danger)', marginLeft: 4 }}>offline</span>}</td>
              <td><Bar v={r.cpu} warn={WARN.cpu} /></td>
              <td><Bar v={r.mem} warn={WARN.mem} /></td>
              <td><Bar v={r.disk} warn={WARN.disk} /></td>
            </tr>
          ))}
        </tbody>
      </table>
      )}
    </div>
  )
}

export const systemResourceWidget: WidgetDef = {
  id: 'core.system-resource',
  title: '시스템 리소스',
  category: 'infra',
  component: SystemResourceWidget,
  defaultSize: { w: 6 },
}
