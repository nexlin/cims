// 코어 위젯 — 시스템 형상(구성도) + 상태. HA 그룹(AS/AA)/단독(SA)별로 VIP→멤버 노드→**설치된** 모듈
// 구성을 그리고, 각 서버/모듈 상태색을 **활성 알람 등급**으로 구동(offline/critical/major 🔴,
// minor/warning 🟡, 정상 🟢, 설치만 되고 미기동 ⚪).
// 서비스 무지(범용 인프라). 자체 폴링 15s. 비관리자/오류 시 빈.
import { useState, useEffect, useCallback } from 'react'
import { useNavigate } from 'react-router-dom'
import { haGroupsApi, type HaGroup } from '../../api/ha_groups'
import { deploymentApi, type Agent } from '../../api/deployment'
import { alertsApi, type AlertEvent } from '../../api/alerts'
import { externalSystemsApi, type ExternalSystem, type ProbeResult } from '../../api/external_systems'
import { depEffectiveStatus } from '../../pages/deploy/deployHelpers'
import type { WidgetDef } from '../types'

const EXT_TYPE_LABEL: Record<string, string> = {
  db: 'DB', monitoring: '모니터링', storage: '스토리지', auth: '인증', other: '기타',
}

const SEV_RANK: Record<string, number> = { critical: 4, major: 3, minor: 2, warning: 1 }
const C_RED = '#e74c3c', C_AMBER = '#f59e0b', C_GREEN = '#22c55e', C_GRAY = '#9aa5b4', C_BLUE = '#3498db'
// EMS 관례 — 단일문자 상태/설정 배지 (A/S, M/B). hover 시 title 로 풀워드.
const STATE_BADGE = {
  fontSize: 9, fontWeight: 700, minWidth: 15, height: 15, lineHeight: '15px',
  textAlign: 'center' as const, borderRadius: 3, padding: '0 3px', display: 'inline-block',
} as const
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

// 노드가 담는 모듈 = **설치된** 모듈. running 은 실측 기동 여부 — 설치만 되고 안 뜬
// 모듈(AS 대기 노드의 cold 모듈 등)도 회색 칩으로 보인다.
interface NodeModule { name: string; running: boolean }
interface Node { agentId: number; host: string; online: boolean; role?: string; active?: boolean; version?: string; modules: NodeModule[] }
interface Sys { key: string; name: string; mode: 'AS' | 'AA' | 'SA'; vip?: string; vipSlot?: string; nodes: Node[] }

function chipTint(rank: number, running: boolean): string {
  if (rank >= 3) return 'rgba(231,76,60,0.12)'
  if (rank >= 1) return 'rgba(245,158,11,0.13)'
  return running ? 'rgba(34,197,94,0.10)' : 'var(--surface-2)'
}

function ModuleChip({ host, module, running, sevByMo }: { host: string; module: string; running: boolean; sevByMo: Map<string, number> }) {
  const rank = Math.max(sevByMo.get(`${host}/${module}`) ?? 0, sevByMo.get(`cims/${module}`) ?? 0)
  const col = sevColor(rank, running)
  return (
    <span style={{ display: 'inline-flex', alignItems: 'center', gap: 4, fontSize: 11, padding: '2px 8px',
                   border: `1px solid var(--border)`, borderRadius: 12, background: chipTint(rank, running) }}
          title={`${module} — ${rank >= 3 ? '알람(심각)' : rank >= 1 ? '알람(경고)' : running ? '정상' : '설치됨·미기동'}`}>
      <span style={{ width: 7, height: 7, borderRadius: '50%', background: col, display: 'inline-block' }} />{module}
    </span>
  )
}

function NodeBox({ n, sevByMo, onClick }: { n: Node; sevByMo: Map<string, number>; onClick: () => void }) {
  // 노드 등급 = offline → critical, 아니면 host/모듈 알람 최고 등급.
  // 서비스 레벨 알람(cims/<모듈>)은 **기동 중인** 모듈에만 귀속 — 정지된 대기 노드까지 물들지 않게.
  let rank = n.online ? 0 : 4
  for (const [mo, r] of sevByMo) {
    if (mo.split('/')[0] === n.host) rank = Math.max(rank, r)
    if (n.modules.some(m => m.running && mo === `cims/${m.name}`)) rank = Math.max(rank, r)
  }
  const col = sevColor(rank, n.online)
  return (
    <div onClick={onClick} title="클릭: 서버 Inspector"
         style={{ border: '1px solid var(--border)', borderTop: `3px solid ${col}`, borderRadius: 8,
                  background: 'var(--surface)', cursor: 'pointer', overflow: 'hidden', boxShadow: 'var(--shadow)' }}>
      {/* 헤더: 상태점 + 호스트 + [A/S 상태]·[M/B 설정] 단축 배지(hover=풀워드) + 버전 */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 5, padding: '7px 10px 6px' }}>
        <span style={{ width: 9, height: 9, borderRadius: '50%', background: col, display: 'inline-block', flexShrink: 0 }} />
        <b style={{ fontSize: 13, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{n.host}</b>
        {!n.online
          ? <span style={{ fontSize: 10, color: C_RED, flexShrink: 0 }}>offline</span>
          : <span style={{ display: 'inline-flex', gap: 3, flexShrink: 0 }}>
              {/* 상태 A/S — 채움 배지 */}
              <span title={n.active ? 'Active (현재 서비스 중)' : 'Standby (대기)'}
                    style={{ ...STATE_BADGE, background: n.active ? C_GREEN : 'var(--surface-2)',
                             color: n.active ? '#fff' : 'var(--text-muted)',
                             border: n.active ? 'none' : '1px solid var(--border)' }}>
                {n.active ? 'A' : 'S'}</span>
              {/* 설정 M/B — 외곽 배지 (AS 만) */}
              {n.role && <span title={`${n.role === 'master' ? 'Master' : 'Backup'} (설정·VRRP 우선순위)`}
                    style={{ ...STATE_BADGE, color: C_BLUE, border: `1px solid ${C_BLUE}` }}>
                {n.role === 'master' ? 'M' : 'B'}</span>}
              {/* 절체 드리프트 — 설정 선호 ≠ 현재 Active */}
              {n.role && ((n.role === 'master') !== !!n.active) &&
                <span title="절체됨 — 설정 선호 노드와 현재 Active 가 다름" style={{ color: C_AMBER, fontSize: 11, fontWeight: 700 }}>⚠</span>}
            </span>}
        <span style={{ marginLeft: 'auto', fontSize: 10, color: 'var(--text-muted)', flexShrink: 0 }}>{n.version ? `v${n.version}` : ''}</span>
      </div>
      {/* 모듈 칩 */}
      <div style={{ borderTop: '1px solid var(--border)', padding: '6px 10px 8px', display: 'flex', flexWrap: 'wrap', gap: 5,
                    background: 'var(--bg-soft)' }}>
        {n.modules.length === 0
          ? <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>(설치된 모듈 없음)</span>
          : n.modules.map(m => <ModuleChip key={m.name} host={n.host} module={m.name}
                                           running={n.online && m.running} sevByMo={sevByMo} />)}
      </div>
    </div>
  )
}

// 노드 수 기반 균형 배치 열 수 — 2→2, 3·4→2(2x2), 5~9→3 ...
function gridCols(n: number): number {
  if (n <= 1) return 1
  return Math.min(Math.ceil(Math.sqrt(n)), 4)
}

function ExternalBox({ sys, status, onClick }: { sys: ExternalSystem; status?: ProbeResult; onClick: () => void }) {
  const hasProbe = (sys.probe?.mode ?? 'none') !== 'none'
  const st = status?.status
  const col = !hasProbe ? C_GRAY : st === 'up' ? C_GREEN : st === 'down' ? C_RED : C_GRAY
  return (
    <div onClick={onClick} title="클릭: 외부 시스템 관리"
         style={{ border: `2px dashed ${col}`, borderRadius: 8, padding: '8px 10px', minWidth: 150,
                  background: 'var(--surface)', cursor: 'pointer' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
        <span style={{ width: 9, height: 9, borderRadius: '50%', background: col, display: 'inline-block' }} />
        <b style={{ fontSize: 13 }}>{sys.name}</b>
        <span style={{ fontSize: 10, padding: '1px 5px', borderRadius: 3, color: '#fff', background: '#8e44ad' }}>외부</span>
        <span style={{ fontSize: 10, padding: '1px 5px', borderRadius: 3, border: '1px solid var(--border)', color: 'var(--text-muted)' }}>
          {EXT_TYPE_LABEL[sys.type] || sys.type}</span>
      </div>
      <div style={{ marginTop: 4 }}>
        {(sys.endpoints || []).map((e, i) => (
          <span key={i} style={{ fontSize: 11, padding: '1px 6px', border: '1px solid var(--border)',
                                 borderRadius: 10, marginRight: 4, marginTop: 3, display: 'inline-block' }}>
            <code style={{ fontSize: 11 }}>{e.host}:{e.port}</code></span>
        ))}
      </div>
      {hasProbe && st === 'up' && status?.latency_ms != null &&
        <div style={{ fontSize: 10, color: 'var(--text-muted)', marginTop: 2 }}>{status.latency_ms}ms</div>}
    </div>
  )
}

function SystemTopologyWidget() {
  const navigate = useNavigate()
  const [systems, setSystems] = useState<Sys[]>([])
  const [sevByMo, setSevByMo] = useState<Map<string, number>>(new Map())
  const [ext, setExt] = useState<ExternalSystem[]>([])
  const [extStatus, setExtStatus] = useState<Map<number, ProbeResult>>(new Map())

  const load = useCallback(async () => {
    try {
      const [groups, agents, deps, alerts, extList] = await Promise.all([
        haGroupsApi.list(), deploymentApi.listAgents(), deploymentApi.listDeployments(),
        alertsApi.list({ days: 7, limit: 1000 }).then(r => r.events).catch(() => [] as AlertEvent[]),
        externalSystemsApi.list().catch(() => [] as ExternalSystem[]),
      ])
      setExt(extList.filter(s => s.enabled))
      // probe 상태는 느릴 수 있어 비블로킹 — 도착하면 색 갱신.
      externalSystemsApi.status()
        .then(items => setExtStatus(new Map(items.map(i => [i.id, i]))))
        .catch(() => {})
      const byId = new Map<number, Agent>(agents.map(a => [a.id, a]))
      // 설치된 모듈 = 배포기록 존재(pending=미설치 · removed=삭제 제외). 기동 여부는 실측
      // 우선(depEffectiveStatus) — 배포기록 status 는 운영자 지시 이력이라 HA notify 가
      // 로컬에서 켠/끈 모듈과 어긋난다. 같은 모듈 중복 기록은 name 으로 합치고 running 은 OR.
      const modsOf = (aid: number): NodeModule[] => {
        const byName = new Map<string, NodeModule>()
        for (const d of deps) {
          if (d.agent_id !== aid || !(d.process_name || '')) continue
          if (d.status === 'pending' || d.status === 'removed') continue
          const name = (d.process_name || '').toLowerCase()
          const running = depEffectiveStatus(d) === 'running' || !!byName.get(name)?.running
          byName.set(name, { name, running })
        }
        return [...byName.values()]
      }
      // role = 설정(master/backup), active = 런타임 상태(현재 VIP 보유 = Active).
      const node = (aid: number, role?: string, active?: boolean): Node => {
        const a = byId.get(aid)
        return { agentId: aid, host: a?.name || String(aid), online: a?.status === 'online',
                 role, active, version: a?.agent_version || undefined, modules: modsOf(aid) }
      }
      const holdsVip = (aid: number, vipIps: Set<string>): boolean => {
        const a = byId.get(aid)
        return !!a?.interfaces?.some(i => i.ip && vipIps.has(i.ip))
      }
      const grouped = new Set<number>()
      const sys: Sys[] = []
      for (const g of groups as HaGroup[]) {
        const vb = (g.vip_bindings || [])[0]
        // 그룹의 모든 VIP IP — Active 판정(노드 interface 에 VIP 존재)용.
        const vipIps = new Set<string>()
        ;(g.vip_bindings || []).forEach(b => b.ip && vipIps.add(b.ip))
        if (g.vip) vipIps.add(g.vip)
        const isAS = g.mode === 'active_standby'
        const members = g.members.slice().sort((a, b) => b.priority - a.priority)
        members.forEach(m => grouped.add(m.agent_id))
        sys.push({ key: `g${g.id}`, name: g.name, mode: isAS ? 'AS' : 'AA',
                   vip: vb?.ip || g.vip || undefined, vipSlot: vb?.slot,
                   nodes: members.map(m => {
                     const online = byId.get(m.agent_id)?.status === 'online'
                     // AS: VIP 보유 노드 = Active. AA: 가동 노드 모두 Active.
                     const active = isAS ? (online && holdsVip(m.agent_id, vipIps)) : online
                     return node(m.agent_id, isAS ? m.role : undefined, active)
                   }) })
      }
      for (const a of agents) {
        if (grouped.has(a.id) || a.status === 'revoked') continue
        sys.push({ key: `a${a.id}`, name: a.name, mode: 'SA', nodes: [node(a.id, undefined, a.status === 'online')] })
      }
      setSystems(sys)
      setSevByMo(activeSevByMo(alerts))
    } catch { setSystems([]) }
  }, [])

  useEffect(() => { load(); const iv = setInterval(load, 15000); return () => clearInterval(iv) }, [load])
  if (systems.length === 0 && ext.length === 0) return null

  const sysRank = (s: Sys): number => {
    let r = 0
    for (const n of s.nodes) {
      if (!n.online) r = Math.max(r, 4)
      for (const [mo, rr] of sevByMo) {
        if (mo.split('/')[0] === n.host || n.modules.some(m => m.running && mo === `cims/${m.name}`)) r = Math.max(r, rr)
      }
    }
    return r
  }

  return (
    <div className="panel" style={{ padding: 16 }}>
      <div style={{ fontWeight: 600, marginBottom: 12, fontSize: 14, display: 'flex', alignItems: 'center' }}>
        시스템 형상 ({systems.length}{ext.length > 0 ? ` + 외부 ${ext.length}` : ''})
        <a onClick={() => navigate('/deploy/servers')}
           style={{ marginLeft: 'auto', fontSize: 12, fontWeight: 500, color: 'var(--primary)', cursor: 'pointer' }}>시스템/인프라 →</a>
      </div>
      {/* 시스템 카드들 — 다중일 때 좌우로 흐르도록 auto-fit 그리드 (상하좌우 균등). */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(360px, 1fr))', gap: 14, alignItems: 'start' }}>
        {systems.map(s => {
          const r = sysRank(s)
          const dot = sevColor(r, true)
          const mb = MODE_BADGE[s.mode]
          const cols = gridCols(s.nodes.length)
          return (
            <div key={s.key} style={{ border: `1px solid var(--border)`, borderLeft: `4px solid ${dot}`,
                                      borderRadius: 8, padding: '10px 14px', background: 'var(--bg-soft)' }}>
              <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 8 }}>
                <span style={{ width: 10, height: 10, borderRadius: '50%', background: dot, display: 'inline-block' }} />
                <b style={{ fontSize: 13 }}>{s.name}</b>
                <span style={{ fontSize: 10, padding: '1px 6px', borderRadius: 3, color: '#fff', background: mb.c }}>{mb.t}</span>
                {s.mode === 'AS' && s.nodes.length > 1 &&
                  <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>⇄ VRRP</span>}
                {s.vip && <span style={{ marginLeft: 'auto', fontSize: 11, color: 'var(--text-muted)' }}>
                  ◆ VIP <code style={{ fontSize: 11 }}>{s.vip}</code>{s.vipSlot ? ` /${s.vipSlot}` : ''}</span>}
              </div>
              {/* 노드 — 수에 따라 균형 그리드 (2→2열, 4→2x2 ...). */}
              <div style={{ display: 'grid', gridTemplateColumns: `repeat(${cols}, minmax(150px, 1fr))`, gap: 10 }}>
                {s.nodes.map(n => (
                  <NodeBox key={n.agentId} n={n} sevByMo={sevByMo}
                           onClick={() => navigate(`/deploy/servers?agent=${n.agentId}`)} />
                ))}
              </div>
            </div>
          )
        })}
        {/* 외부 시스템 — 점선 테두리로 내부 노드와 구분. */}
        {ext.length > 0 && (
          <div style={{ border: `1px dashed var(--border)`, borderLeft: `4px dashed #8e44ad`,
                        borderRadius: 8, padding: '10px 14px', background: 'var(--bg-soft)' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 8 }}>
              <span style={{ fontSize: 10, padding: '1px 6px', borderRadius: 3, color: '#fff', background: '#8e44ad' }}>외부 시스템</span>
              <b style={{ fontSize: 13 }}>External</b>
            </div>
            <div style={{ display: 'grid', gridTemplateColumns: `repeat(${gridCols(ext.length)}, minmax(150px, 1fr))`, gap: 10 }}>
              {ext.map(s => (
                <ExternalBox key={s.id} sys={s} status={extStatus.get(s.id)}
                             onClick={() => navigate('/deploy/external-systems')} />
              ))}
            </div>
          </div>
        )}
      </div>
      {/* 상태 범례 */}
      <div style={{ display: 'flex', gap: 14, marginTop: 10, fontSize: 11, color: 'var(--text-muted)', flexWrap: 'wrap' }}>
        {[['정상', C_GREEN], ['경고', C_AMBER], ['장애/오프라인', C_RED], ['설치됨·미기동', C_GRAY], ['외부', '#8e44ad']].map(([t, c]) => (
          <span key={t as string} style={{ display: 'inline-flex', alignItems: 'center', gap: 4 }}>
            <span style={{ width: 8, height: 8, borderRadius: '50%', background: c as string, display: 'inline-block' }} />{t}
          </span>
        ))}
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
