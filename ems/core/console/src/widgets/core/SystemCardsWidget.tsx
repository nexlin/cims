// 코어 위젯 — 시스템 카드 grid (HA 그룹 AS/AA + standalone SA). 서비스 무지(범용 인프라).
// 자체 폴링(15초). 비관리자/오류 시 빈 → 카드 없음.

import { useState, useEffect, useCallback } from 'react'
import { useNavigate } from 'react-router-dom'
import { haGroupsApi } from '../../api/ha_groups'
import { deploymentApi } from '../../api/deployment'
import type { WidgetDef } from '../types'

interface SystemCard {
  key: string
  name: string
  mode: 'AS' | 'AA' | 'SA'
  online: number
  total: number
}

const MODE_COLOR: Record<SystemCard['mode'], string> = { AS: '#3498db', AA: '#27ae60', SA: '#95a5a6' }
const MODE_TIP: Record<SystemCard['mode'], string> = {
  AS: 'Active/Standby — VRRP 이중화 (1 active + standby)',
  AA: 'All-Active — 전 멤버 동시 활성',
  SA: 'Standalone — 단독 노드 (그룹 없음)',
}

function SystemCardsWidget() {
  const navigate = useNavigate()
  const [systems, setSystems] = useState<SystemCard[]>([])
  const [loaded, setLoaded] = useState(false)   // 미로딩 vs 진짜 비어있음 구분

  const load = useCallback(async () => {
    try {
      const [groups, agents] = await Promise.all([haGroupsApi.list(), deploymentApi.listAgents()])
      const byId = new Map(agents.map(a => [a.id, a]))
      const grouped = new Set<number>()
      const cards: SystemCard[] = []
      for (const g of groups) {
        let online = 0
        for (const m of g.members) {
          grouped.add(m.agent_id)
          if (byId.get(m.agent_id)?.status === 'online') online++
        }
        cards.push({ key: `g${g.id}`, name: g.name,
                     mode: g.mode === 'active_standby' ? 'AS' : 'AA',
                     online, total: g.members.length })
      }
      for (const a of agents) {
        if (grouped.has(a.id) || a.status === 'revoked') continue
        cards.push({ key: `a${a.id}`, name: a.name, mode: 'SA',
                     online: a.status === 'online' ? 1 : 0, total: 1 })
      }
      setSystems(cards)
      setLoaded(true)
    } catch { setSystems([]); setLoaded(true) }
  }, [])

  useEffect(() => {
    load()
    const iv = setInterval(load, 15000)
    return () => clearInterval(iv)
  }, [load])

  const onOpen = () => navigate('/deploy/servers')
  return (
    <div className="panel" style={{ padding: 16 }}>
      <div style={{ fontWeight: 600, marginBottom: 10, fontSize: 14, display: 'flex', alignItems: 'center' }}>
        시스템 ({systems.length})
        <a onClick={onOpen} style={{ marginLeft: 'auto', fontSize: 12, fontWeight: 500, color: 'var(--primary)', cursor: 'pointer' }}>
          시스템/인프라 →
        </a>
      </div>
      {/* 데이터가 없어도 카드는 유지 — null 을 돌려주면 로딩 동안 위젯이 사라졌다 팝인한다. */}
      {systems.length === 0 && (
        <div className="empty">{loaded ? '등록된 시스템이 없습니다.' : '불러오는 중…'}</div>
      )}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(180px, 1fr))', gap: 10 }}>
        {systems.map(s => {
          const healthy = s.total > 0 && s.online === s.total
          const dot = healthy ? 'var(--cims-success)' : s.online > 0 ? '#f59e0b' : 'var(--destructive)'
          return (
            <div key={s.key} onClick={onOpen}
                 style={{ background: 'var(--card)', border: '1px solid var(--border)',
                          borderRadius: 'var(--radius)', padding: '12px 14px', cursor: 'pointer' }}
                 title={`${s.name} — ${MODE_TIP[s.mode]}\n온라인 ${s.online}/${s.total}`}>
              <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 8 }}>
                <span style={{ display: 'inline-block', width: 8, height: 8, borderRadius: '50%', background: dot }} />
                <span style={{ fontWeight: 600, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{s.name}</span>
                <span style={{ marginLeft: 'auto', fontSize: 10, fontWeight: 700, color: '#fff',
                               background: MODE_COLOR[s.mode], padding: '1px 6px', borderRadius: 3 }}>{s.mode}</span>
              </div>
              <div style={{ fontSize: 12, color: 'var(--muted-foreground)' }}>
                온라인 <b style={{ color: healthy ? 'var(--cims-success)' : 'inherit' }}>{s.online}</b>/{s.total}
              </div>
            </div>
          )
        })}
      </div>
    </div>
  )
}

export const systemCardsWidget: WidgetDef = {
  id: 'core.system-cards',
  title: '시스템 카드',
  category: 'infra',
  component: SystemCardsWidget,
  // HA·배포는 선언 대상이 아니다(api_docs.md 범위 정책) — 노드 사양만.
  apis: ['nodes.list'],
  defaultSize: { w: 12 },
}
