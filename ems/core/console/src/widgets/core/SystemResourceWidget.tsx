// 코어 위젯 — 시스템 리소스(차트). 서버별 CPU/메모리/디스크/네트워크 추이를 area 차트로.
// 지표는 체크박스로 모두 또는 선택한 것만 동시 표시. agent metric(2s 수집, tail-read) 사용.
import { useState, useEffect, useCallback, Fragment } from 'react'
import { useNavigate } from 'react-router-dom'
import { deploymentApi, type Agent, type AgentMetric } from '../../api/deployment'
import type { WidgetDef } from '../types'

type MetricKey = 'cpu' | 'mem' | 'disk' | 'net'
const ALL_METRICS: { k: MetricKey; label: string; pct: boolean }[] = [
  { k: 'cpu', label: 'CPU', pct: true },
  { k: 'mem', label: '메모리', pct: true },
  { k: 'disk', label: '디스크', pct: true },
  { k: 'net', label: '네트워크', pct: false },
]
const WARN = 85
const C_RED = '#e74c3c', C_AMBER = '#f59e0b', C_GREEN = '#22c55e', C_PRIMARY = 'var(--primary)'

interface Srv { id: number; host: string; online: boolean; items: AgentMetric[] }

function latest(items: AgentMetric[]): AgentMetric | null {
  if (!items || items.length === 0) return null
  return items.reduce((a, b) => ((a.ts || '') >= (b.ts || '') ? a : b))
}
function pctColor(v: number): string { return v >= WARN ? C_RED : v >= WARN - 15 ? C_AMBER : C_GREEN }
function fmtRate(bps: number): string {
  if (bps >= 1e6) return `${(bps / 1e6).toFixed(1)} MB/s`
  if (bps >= 1e3) return `${(bps / 1e3).toFixed(1)} KB/s`
  return `${Math.round(bps)} B/s`
}
function metricVal(m: AgentMetric | null, k: MetricKey): number | null {
  if (!m) return null
  if (k === 'cpu') return m.cpu_pct
  if (k === 'mem') return m.mem_pct
  if (k === 'disk') return m.disk_pct
  // net = 전 인터페이스 rx_rate+tx_rate 합 (Bps)
  const ifs = m.per_iface || []
  if (ifs.length === 0) return null
  return ifs.reduce((s, f) => s + (f.rx_rate ?? 0) + (f.tx_rate ?? 0), 0)
}

// area 추이 차트 (pct 면 0~100 고정 스케일, rate 면 자동 스케일).
function Area({ data, color, pct }: { data: number[]; color: string; pct: boolean }) {
  const h = 40, w = 160, pad = 3
  if (data.length < 2) return <div style={{ height: h, fontSize: 10, color: 'var(--muted-foreground)', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>데이터 수집 중…</div>
  const max = pct ? 100 : Math.max(...data, 1)
  const min = 0, range = max - min || 1
  const step = (w - pad * 2) / (data.length - 1)
  const xy = (v: number, i: number): [number, number] => [pad + i * step, pad + (h - pad * 2) * (1 - (v - min) / range)]
  const pts = data.map((v, i) => xy(v, i).map(n => n.toFixed(1)).join(',')).join(' ')
  const [lx, ly] = xy(data[data.length - 1], data.length - 1)
  return (
    <svg width="100%" height={h} viewBox={`0 0 ${w} ${h}`} preserveAspectRatio="none" style={{ display: 'block' }}>
      {/* 기준선(바닥) */}
      <line x1={pad} y1={h - pad} x2={w - pad} y2={h - pad} stroke="var(--border)" strokeWidth={0.5} />
      <polygon points={`${pad},${h - pad} ${pts} ${w - pad},${h - pad}`} fill={color} opacity={0.16} />
      <polyline points={pts} fill="none" stroke={color} strokeWidth={1.6} strokeLinejoin="round" strokeLinecap="round" />
      {/* 현재(최신) 지점 강조 */}
      <circle cx={lx} cy={ly} r={2.2} fill={color} />
    </svg>
  )
}

function SystemResourceWidget() {
  const navigate = useNavigate()
  const [rows, setRows] = useState<Srv[]>([])
  const [loaded, setLoaded] = useState(false)
  const [stale, setStale] = useState(false)
  const [sel, setSel] = useState<Set<MetricKey>>(new Set(ALL_METRICS.map(m => m.k)))

  const load = useCallback(async () => {
    let agents: Agent[]
    try { agents = (await deploymentApi.listAgents()).filter(a => a.status !== 'revoked') }
    catch { setStale(true); setLoaded(true); return }
    const base = (a: Agent): Srv => ({ id: a.id, host: a.name, online: a.status === 'online', items: [] })
    setStale(false); setLoaded(true)
    const withTimeout = <T,>(p: Promise<T>, ms: number) =>
      Promise.race([p, new Promise<null>(res => setTimeout(() => res(null), ms))])
    const out = await Promise.all(agents.map(async (a): Promise<Srv> => {
      if (a.status !== 'online') return base(a)
      try { const r = await withTimeout(deploymentApi.agentMetrics(a.id), 4000); return { ...base(a), items: r ? r.items : [] } }
      catch { return base(a) }
    }))
    setRows(out)
  }, [])
  useEffect(() => { load(); const iv = setInterval(load, 15000); return () => clearInterval(iv) }, [load])

  const toggle = (k: MetricKey) => setSel(s => {
    const n = new Set(s); if (n.has(k)) n.delete(k); else n.add(k)
    return n.size === 0 ? new Set(ALL_METRICS.map(m => m.k)) : n   // 전부 해제 방지
  })
  const cols = ALL_METRICS.filter(m => sel.has(m.k))
  const seriesOf = (s: Srv, k: MetricKey) => s.items
    .slice().sort((a, b) => (a.ts || '').localeCompare(b.ts || ''))
    .map(m => metricVal(m, k)).filter((x): x is number => x != null)
  const fmt = (v: number | null, pct: boolean) => v == null ? '—' : pct ? `${Math.round(v)}%` : fmtRate(v)

  return (
    <div className="panel" style={{ padding: 16 }}>
      <div style={{ fontWeight: 600, marginBottom: 8, fontSize: 14, display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
        시스템 리소스 ({rows.length})
        {stale && <span title="갱신 일시 실패 — 직전 값" style={{ fontSize: 11, color: C_AMBER }}>⚠ 갱신 지연</span>}
        <span style={{ display: 'inline-flex', gap: 10, fontSize: 12, fontWeight: 400 }}>
          {ALL_METRICS.map(m => (
            <label key={m.k} style={{ display: 'inline-flex', alignItems: 'center', gap: 3, cursor: 'pointer', color: 'var(--muted-foreground)' }}>
              <input type="checkbox" checked={sel.has(m.k)} onChange={() => toggle(m.k)} />{m.label}
            </label>
          ))}
        </span>
        <a onClick={() => navigate('/deploy/servers')}
           style={{ marginLeft: 'auto', fontSize: 12, fontWeight: 500, color: 'var(--primary)', cursor: 'pointer' }}>서버 →</a>
      </div>

      {rows.length === 0 ? (
        <div style={{ padding: '20px 4px', fontSize: 13, color: 'var(--muted-foreground)', textAlign: 'center' }}>
          {loaded ? '표시할 서버가 없습니다.' : '불러오는 중…'}
        </div>
      ) : (
        <div style={{ display: 'grid', gridTemplateColumns: `minmax(150px, 1fr) repeat(${cols.length}, 1fr)`,
                      gap: 0, alignItems: 'stretch' }}>
          {/* 헤더 */}
          <div style={{ fontSize: 11, color: 'var(--muted-foreground)', fontWeight: 600, padding: '0 4px 6px' }}>서버</div>
          {cols.map(c => (
            <div key={c.k} style={{ fontSize: 11, color: 'var(--muted-foreground)', textAlign: 'center', fontWeight: 600, padding: '0 4px 6px' }}>
              {c.label} <span style={{ fontWeight: 400, opacity: 0.7 }}>{c.pct ? '%' : 'rate'}</span>
            </div>
          ))}
          {/* 서버별 행 */}
          {rows.map((s, ri) => {
            const lm = latest(s.items)
            const rowBd = ri < rows.length - 1 ? { borderBottom: '1px solid var(--border)' } : {}
            return (
              <Fragment key={s.id}>
                <div onClick={() => navigate(`/deploy/servers?agent=${s.id}`)}
                     style={{ cursor: 'pointer', opacity: s.online ? 1 : 0.5, fontSize: 12, fontWeight: 600,
                              display: 'flex', alignItems: 'center', gap: 5,
                              padding: '8px 4px', overflow: 'hidden', ...rowBd }}>
                  <span style={{ width: 8, height: 8, borderRadius: '50%', flexShrink: 0,
                                 background: s.online ? C_GREEN : C_RED }} />
                  <span title={s.host} style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{s.host}</span>
                  {!s.online && <span style={{ fontSize: 10, color: C_RED, flexShrink: 0 }}>offline</span>}
                </div>
                {cols.map(c => {
                  const v = metricVal(lm, c.k)
                  const hot = c.pct && v != null && v >= WARN
                  const warnish = c.pct && v != null && v >= WARN - 15 && v < WARN
                  const color = c.pct && v != null ? pctColor(v) : C_PRIMARY
                  // 임계 셀 배경 틴트 — 위험(빨강)/경고(주황) 한눈에 (EMS 관례).
                  const tint = hot ? 'rgba(231,76,60,0.07)' : warnish ? 'rgba(245,158,11,0.07)' : undefined
                  return (
                    <div key={c.k} style={{ opacity: s.online ? 1 : 0.5, padding: '8px 6px 6px', position: 'relative',
                                            background: tint, ...rowBd }}>
                      <div style={{ position: 'absolute', top: 5, right: 6, fontSize: 12, fontWeight: 700,
                                    color: hot ? C_RED : color, background: 'var(--card)',
                                    padding: '0 4px', borderRadius: 4, lineHeight: 1.4 }}>{fmt(v, c.pct)}</div>
                      <Area data={seriesOf(s, c.k)} color={color} pct={c.pct} />
                    </div>
                  )
                })}
              </Fragment>
            )
          })}
        </div>
      )}
    </div>
  )
}

export const systemResourceWidget: WidgetDef = {
  id: 'core.system-resource',
  title: '시스템 리소스',
  category: 'infra',
  component: SystemResourceWidget,
  apis: ['nodes.list', 'nodes.metrics'],
  defaultSize: { w: 6 },
}
