// 코어 위젯 — 시스템 형상(구성도) + 상태. HA 그룹(AS/AA)/단독(SA)별로 VIP→멤버 노드→모듈 구성을
// 그리고, 각 서버/모듈 상태색을 **활성 알람 등급**으로 구동(offline/critical/major 🔴, minor/warning 🟡, 정상 🟢).
// 서비스 무지(범용 인프라). 자체 폴링 15s. 비관리자/오류 시 빈.
import { useState, useEffect, useCallback } from 'react'
import { useNavigate } from 'react-router-dom'
import { haGroupsApi, type HaGroup } from '../../api/ha_groups'
import { deploymentApi, type Agent, type Deployment } from '../../api/deployment'
import { alertsApi, type AlertEvent } from '../../api/alerts'
import type { WidgetDef } from '../types'

const SEV_RANK: Record<string, number> = { critical: 4, major: 3, minor: 2, warning: 1 }
const C_RED = '#e74c3c', C_AMBER = '#f59e0b', C_GREEN = '#22c55e', C_GRAY = '#9aa5b4'
// 등급 → 색 (worst-of). 0/없음 = 정상색.
function sevColor(rank: number, up = true): string {
  if (rank >= 3) return C_RED
  if (rank >= 1) return C_AMBER
  return up ? C_GREEN : C_GRAY
}
const MODE_BADGE: Record<string, { t: string; c: string }> = {
  AS: { t: 'AS', c: '#3498db' }, AA: { t: 'AA', c: '#27ae60' }, SA: { t: 'SA', c: '#95a5a6' },
}

// 활성 알람 → mo_instance별 최고 등급 맵.
function activeSevByMo(events: AlertEvent[]): Map<string, number> {
  const asc = [...events].sort((a, b) => (a.ts || '').localeCompare(b.ts || ''))
  const open: Record<string, AlertEvent> = {}
  for (const ev of asc) {
    const k = ev.alarm_id ? ev.alarm_id.replace(/@\d+$/, '') : ev.type
    if (ev.action === 'open') open[k] = ev
    else if (ev.action === 'close') delete open[k]
  }
  const m = new Map<string, number>()
  for (const ev of Object.values(open)) {
    const mo = ev.source?.mo_instance
    if (!mo) continue
    const r = SEV_RANK[ev.perceived_severity || ev.severity || ''] ?? 1
    m.set(mo, Math.max(m.get(mo) ?? 0, r))
  }
  return m
}

interface Node { agentId: number; host: string; online: boolean; role?: string; version?: string; modules: string[] }
interface Sys { key: string; name: string; mode: 'AS' | 'AA' | 'SA'; vip?: string; vipSlot?: string; nodes: Node[] }

function ModuleChip({ host, module, running, sevByMo }: { host: string; module: string; running: boolean; sevByMo: Map<string, number> }) {
  const rank = Math.max(sevByMo.get(`${host}/${module}`) ?? 0, sevByMo.get(`cims/${module}`) ?? 0)
  const col = sevColor(rank, running)
  return (
    <span style={{ display: 'inline-flex', alignItems: 'center', gap: 3, fontSize: 11, padding: '1px 6px',
                   border: `1px solid var(--border)`, borderRadius: 10, marginRight: 4, marginTop: 3 }}
          title={`${module} — ${rank >= 3 ? '알람(심각)' : rank >= 1 ? '알람(경고)' : running ? '정상' : '미실행'}`}>
      <span style={{ width: 7, height: 7, borderRadius: '50%', background: col, display: 'inline-block' }} />{module}
    </span>
  )
}

function NodeBox({ n, sevByMo, onClick }: { n: Node; sevByMo: Map<string, number>; onClick: () => void }) {
  // 노드 등급 = offline → critical, 아니면 host/모듈 알람 최고 등급.
  let rank = n.online ? 0 : 4
  for (const [mo, r] of sevByMo) {
    if (mo.split('/')[0] === n.host) rank = Math.max(rank, r)
    if (n.modules.some(m => mo === `cims/${m}`)) rank = Math.max(rank, r)
  }
  const col = sevColor(rank, n.online)
  return (
    <div onClick={onClick} title="클릭: 서버 Inspector"
         style={{ border: `2px solid ${col}`, borderRadius: 8, padding: '8px 10px', minWidth: 150,
                  background: 'var(--surface)', cursor: 'pointer' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
        <span style={{ width: 9, height: 9, borderRadius: '50%', background: col, display: 'inline-block' }} />
        <b style={{ fontSize: 13 }}>{n.host}</b>
        {n.role && <span style={{ fontSize: 10, padding: '1px 5px', borderRadius: 3, color: '#fff',
                                  background: n.role === 'master' ? '#e67e22' : '#7f8c8d' }}>
          {n.role === 'master' ? '▶MASTER' : 'STANDBY'}</span>}
        {!n.online && <span style={{ fontSize: 10, color: C_RED }}>offline</span>}
      </div>
      {n.version && <div style={{ fontSize: 10, color: 'var(--text-muted)', margin: '2px 0' }}>v{n.version}</div>}
      <div>{n.modules.length === 0
        ? <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>(모듈 없음)</span>
        : n.modules.map(m => <ModuleChip key={m} host={n.host} module={m} running={n.online} sevByMo={sevByMo} />)}</div>
    </div>
  )
}

function SystemTopologyWidget() {
  const navigate = useNavigate()
  const [systems, setSystems] = useState<Sys[]>([])
  const [sevByMo, setSevByMo] = useState<Map<string, number>>(new Map())

  const load = useCallback(async () => {
    try {
      const [groups, agents, deps, alerts] = await Promise.all([
        haGroupsApi.list(), deploymentApi.listAgents(), deploymentApi.listDeployments(),
        alertsApi.list({ days: 7, limit: 1000 }).then(r => r.events).catch(() => [] as AlertEvent[]),
      ])
      const byId = new Map<number, Agent>(agents.map(a => [a.id, a]))
      const modsOf = (aid: number) => deps
        .filter((d: Deployment) => d.agent_id === aid && (d.process_name || '') && d.status === 'running')
        .map((d: Deployment) => (d.process_name || '').toLowerCase())
      const node = (aid: number, role?: string): Node => {
        const a = byId.get(aid)
        return { agentId: aid, host: a?.name || String(aid), online: a?.status === 'online',
                 role, version: a?.agent_version || undefined, modules: [...new Set(modsOf(aid))] }
      }
      const grouped = new Set<number>()
      const sys: Sys[] = []
      for (const g of groups as HaGroup[]) {
        const vb = (g.vip_bindings || [])[0]
        const members = g.members.slice().sort((a, b) => b.priority - a.priority)
        members.forEach(m => grouped.add(m.agent_id))
        sys.push({ key: `g${g.id}`, name: g.name, mode: g.mode === 'active_standby' ? 'AS' : 'AA',
                   vip: vb?.ip || g.vip || undefined, vipSlot: vb?.slot,
                   nodes: members.map(m => node(m.agent_id, g.mode === 'active_standby' ? m.role : undefined)) })
      }
      for (const a of agents) {
        if (grouped.has(a.id) || a.status === 'revoked') continue
        sys.push({ key: `a${a.id}`, name: a.name, mode: 'SA', nodes: [node(a.id)] })
      }
      setSystems(sys)
      setSevByMo(activeSevByMo(alerts))
    } catch { setSystems([]) }
  }, [])

  useEffect(() => { load(); const iv = setInterval(load, 15000); return () => clearInterval(iv) }, [load])
  if (systems.length === 0) return null

  const sysRank = (s: Sys): number => {
    let r = 0
    for (const n of s.nodes) {
      if (!n.online) r = Math.max(r, 4)
      for (const [mo, rr] of sevByMo) {
        if (mo.split('/')[0] === n.host || n.modules.some(m => mo === `cims/${m}`)) r = Math.max(r, rr)
      }
    }
    return r
  }

  return (
    <div className="panel" style={{ padding: 16 }}>
      <div style={{ fontWeight: 600, marginBottom: 12, fontSize: 14, display: 'flex', alignItems: 'center' }}>
        시스템 형상 ({systems.length})
        <a onClick={() => navigate('/deploy/servers')}
           style={{ marginLeft: 'auto', fontSize: 12, fontWeight: 500, color: 'var(--primary)', cursor: 'pointer' }}>시스템/인프라 →</a>
      </div>
      <div style={{ display: 'flex', flexDirection: 'column', gap: 14 }}>
        {systems.map(s => {
          const r = sysRank(s)
          const dot = sevColor(r, true)
          const mb = MODE_BADGE[s.mode]
          return (
            <div key={s.key} style={{ border: `1px solid var(--border)`, borderLeft: `4px solid ${dot}`,
                                      borderRadius: 8, padding: '10px 14px', background: 'var(--bg-soft)' }}>
              <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 8 }}>
                <span style={{ width: 10, height: 10, borderRadius: '50%', background: dot, display: 'inline-block' }} />
                <b style={{ fontSize: 13 }}>{s.name}</b>
                <span style={{ fontSize: 10, padding: '1px 6px', borderRadius: 3, color: '#fff', background: mb.c }}>{mb.t}</span>
                {s.vip && <span style={{ marginLeft: 'auto', fontSize: 11, color: 'var(--text-muted)' }}>
                  ◆ VIP <code style={{ fontSize: 11 }}>{s.vip}</code>{s.vipSlot ? ` /${s.vipSlot}` : ''}</span>}
              </div>
              <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
                {s.nodes.map((n, i) => (
                  <span key={n.agentId} style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
                    {i > 0 && s.mode === 'AS' && (
                      <span style={{ fontSize: 10, color: 'var(--text-muted)', whiteSpace: 'nowrap' }}>╌ vrrp ╌</span>)}
                    <NodeBox n={n} sevByMo={sevByMo} onClick={() => navigate(`/deploy/servers?agent=${n.agentId}`)} />
                  </span>
                ))}
              </div>
            </div>
          )
        })}
      </div>
    </div>
  )
}

export const systemTopologyWidget: WidgetDef = {
  id: 'core.system-topology',
  title: '시스템 형상',
  category: 'infra',
  component: SystemTopologyWidget,
  defaultSize: { w: 12 },
}
