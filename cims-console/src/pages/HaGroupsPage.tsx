import { useCallback, useEffect, useMemo, useState } from 'react'
import { haGroupsApi, type HaGroup, type HaMember, type HaMode, type HaRole } from '../api/ha_groups'
import { deploymentApi, type Agent } from '../api/deployment'

const MODE_LABEL: Record<HaMode, string> = {
  active_standby: 'A/S (2 노드)',
  all_active: 'All Active (N 노드)',
}

export default function HaGroupsPage() {
  const [groups, setGroups] = useState<HaGroup[]>([])
  const [agents, setAgents] = useState<Agent[]>([])
  const [loading, setLoading] = useState(false)
  const [err, setErr] = useState<string>('')
  const [showCreate, setShowCreate] = useState(false)

  const load = useCallback(async () => {
    setLoading(true); setErr('')
    try {
      const [gs, as] = await Promise.all([
        haGroupsApi.list(),
        deploymentApi.listAgents(),
      ])
      setGroups(gs); setAgents(as)
    } catch (e) {
      setErr(String(e))
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => { load() }, [load])

  // 다른 그룹에 이미 속한 agent set — 그룹 추가/생성 시 select 제한용
  const reservedAgentIds = useMemo(() => {
    const s = new Set<number>()
    for (const g of groups) for (const m of g.members) s.add(m.agent_id)
    return s
  }, [groups])

  const onDelete = async (g: HaGroup) => {
    if (!confirm(`HA 그룹 "${g.name}" 을 삭제하시겠습니까?`)) return
    try { await haGroupsApi.delete(g.id); await load() }
    catch (e) { alert(String(e)) }
  }

  const onRemoveMember = async (g: HaGroup, m: HaMember) => {
    if (!confirm(`${m.agent_name ?? `agent#${m.agent_id}`} 을 그룹에서 제거하시겠습니까?`)) return
    try { await haGroupsApi.removeMember(g.id, m.agent_id); await load() }
    catch (e) { alert(String(e)) }
  }

  return (
    <div style={{ padding: 16 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', marginBottom: 16 }}>
        <h1 style={{ margin: 0 }}>HA 그룹</h1>
        <div>
          <button onClick={load} disabled={loading} style={{ marginRight: 8 }}>새로고침</button>
          <button onClick={() => setShowCreate(true)}>＋ 그룹 추가</button>
        </div>
      </div>

      {err && <div style={{ color: '#c00', marginBottom: 12 }}>오류: {err}</div>}
      {loading && <div>로딩중...</div>}

      <div style={{
        display: 'grid',
        gridTemplateColumns: 'repeat(auto-fill, minmax(360px, 1fr))',
        gap: 12,
      }}>
        {groups.map(g => (
          <GroupCard key={g.id} group={g} agents={agents}
                     reservedAgentIds={reservedAgentIds}
                     onChanged={load}
                     onDelete={() => onDelete(g)}
                     onRemoveMember={(m) => onRemoveMember(g, m)} />
        ))}
        {groups.length === 0 && !loading && (
          <div style={{ color: '#888', padding: 24 }}>등록된 HA 그룹이 없습니다. "＋ 그룹 추가" 버튼으로 생성하세요.</div>
        )}
      </div>

      {showCreate && (
        <CreateModal onClose={() => setShowCreate(false)}
                     onCreated={() => { setShowCreate(false); load() }}
                     agents={agents}
                     reservedAgentIds={reservedAgentIds} />
      )}
    </div>
  )
}

function GroupCard({
  group, agents, reservedAgentIds, onChanged, onDelete, onRemoveMember,
}: {
  group: HaGroup
  agents: Agent[]
  reservedAgentIds: Set<number>
  onChanged: () => Promise<void> | void
  onDelete: () => void
  onRemoveMember: (m: HaMember) => void
}) {
  const [showAdd, setShowAdd] = useState(false)
  const [addAgentId, setAddAgentId] = useState<number>(0)
  const [addRole, setAddRole] = useState<HaRole>('backup')

  const myMemberIds = useMemo(() => new Set(group.members.map(m => m.agent_id)), [group])
  const selectable = agents.filter(a => !reservedAgentIds.has(a.id) || myMemberIds.has(a.id))

  const onAdd = async () => {
    if (!addAgentId) { alert('agent 선택'); return }
    try {
      await haGroupsApi.addMember(group.id, { agent_id: addAgentId, role: addRole })
      setShowAdd(false); setAddAgentId(0)
      await onChanged()
    } catch (e) { alert(String(e)) }
  }

  return (
    <div style={{
      border: '1px solid #ddd', borderRadius: 8, padding: 12,
      background: '#fff',
    }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 8 }}>
        <span style={{ fontWeight: 'bold', fontSize: 16 }}>{group.name}</span>
        <span style={{
          fontSize: 11, padding: '2px 6px',
          borderRadius: 4, background: group.mode === 'active_standby' ? '#3498db' : '#27ae60',
          color: '#fff',
        }}>{MODE_LABEL[group.mode]}</span>
        <span style={{ marginLeft: 'auto', color: '#888' }}>#{group.id}</span>
      </div>
      <div style={{ fontSize: 13, color: '#555', marginBottom: 8 }}>
        VIP: <code>{group.vip}/{group.vip_mask}</code> · VRID: <code>{group.vrid}</code>
        {group.note && <> · {group.note}</>}
      </div>
      <div style={{ fontSize: 13, marginBottom: 4 }}>
        멤버 ({group.members.length}):
      </div>
      <ul style={{ margin: 0, padding: '0 0 0 16px', fontSize: 13 }}>
        {group.members.map(m => (
          <li key={m.agent_id} style={{ marginBottom: 2 }}>
            {m.agent_name ?? `agent#${m.agent_id}`}
            {' '}<span style={{
              fontSize: 10, padding: '1px 4px', borderRadius: 3,
              background: m.role === 'master' ? '#e67e22' : '#95a5a6', color: '#fff',
            }}>{m.role}</span>
            {' '}<span style={{ color: '#888' }}>(prio={m.priority})</span>
            {' '}<button onClick={() => onRemoveMember(m)}
                         style={{ fontSize: 11, padding: '0 4px', marginLeft: 4 }}>×</button>
          </li>
        ))}
      </ul>
      <div style={{ marginTop: 8, display: 'flex', gap: 6 }}>
        {!showAdd ? (
          <button onClick={() => setShowAdd(true)} style={{ fontSize: 12 }}>＋ 멤버 추가</button>
        ) : (
          <>
            <select value={addAgentId} onChange={e => setAddAgentId(parseInt(e.target.value))} style={{ fontSize: 12 }}>
              <option value={0}>-- agent 선택 --</option>
              {selectable.filter(a => !myMemberIds.has(a.id)).map(a => (
                <option key={a.id} value={a.id}>{a.name} ({a.ip_address ?? a.hostname ?? '?'})</option>
              ))}
            </select>
            <select value={addRole} onChange={e => setAddRole(e.target.value as HaRole)} style={{ fontSize: 12 }}>
              <option value="backup">backup</option>
              <option value="master">master</option>
            </select>
            <button onClick={onAdd} style={{ fontSize: 12 }}>추가</button>
            <button onClick={() => { setShowAdd(false); setAddAgentId(0) }} style={{ fontSize: 12 }}>취소</button>
          </>
        )}
        <button onClick={onDelete} style={{ fontSize: 12, marginLeft: 'auto', color: '#c00' }}>그룹 삭제</button>
      </div>
    </div>
  )
}

function CreateModal({
  onClose, onCreated, agents, reservedAgentIds,
}: {
  onClose: () => void
  onCreated: () => void
  agents: Agent[]
  reservedAgentIds: Set<number>
}) {
  const [name, setName] = useState('')
  const [mode, setMode] = useState<HaMode>('active_standby')
  const [vip, setVip] = useState('')
  const [vipMask, setVipMask] = useState(24)
  const [authPass, setAuthPass] = useState('')
  const [note, setNote] = useState('')
  const [selected, setSelected] = useState<number[]>([])
  const [busy, setBusy] = useState(false)

  const free = useMemo(() => agents.filter(a => !reservedAgentIds.has(a.id)), [agents, reservedAgentIds])

  const toggle = (id: number) => {
    setSelected(prev => prev.includes(id) ? prev.filter(x => x !== id) : [...prev, id])
  }

  const validate = (): string | null => {
    if (!name.trim()) return 'name 필요'
    if (!vip.trim()) return 'VIP 필요'
    if (!authPass.trim() || authPass.length > 8) return 'auth_pass 필요 (max 8)'
    if (mode === 'active_standby' && selected.length !== 2) return 'A/S 는 정확히 2 노드 선택'
    if (mode === 'all_active' && selected.length < 1) return 'AA 는 최소 1 노드 선택'
    return null
  }

  const onSubmit = async () => {
    const e = validate(); if (e) { alert(e); return }
    setBusy(true)
    try {
      await haGroupsApi.create({
        name, mode, vip, vip_mask: vipMask, auth_pass: authPass, note,
        members: selected.map((aid, idx) => ({
          agent_id: aid,
          role: mode === 'active_standby' ? (idx === 0 ? 'master' : 'backup') : 'backup',
          priority: mode === 'active_standby' ? (idx === 0 ? 100 : 90) : 100,
        })),
      })
      onCreated()
    } catch (e) { alert(String(e)) }
    finally { setBusy(false) }
  }

  return (
    <div style={{
      position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.4)',
      display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 1000,
    }}>
      <div style={{ background: '#fff', padding: 20, borderRadius: 8, minWidth: 420, maxWidth: 600 }}>
        <h2 style={{ marginTop: 0 }}>HA 그룹 생성</h2>
        <div style={{ display: 'grid', gridTemplateColumns: '120px 1fr', gap: 8, fontSize: 13 }}>
          <label>이름</label><input value={name} onChange={e => setName(e.target.value)} />
          <label>모드</label>
          <select value={mode} onChange={e => { setMode(e.target.value as HaMode); setSelected([]) }}>
            <option value="active_standby">A/S (2 노드)</option>
            <option value="all_active">All Active (N 노드)</option>
          </select>
          <label>VIP</label><input value={vip} onChange={e => setVip(e.target.value)} placeholder="10.0.0.100" />
          <label>VIP mask</label>
          <input type="number" value={vipMask} onChange={e => setVipMask(parseInt(e.target.value))} />
          <label>auth_pass</label>
          <input value={authPass} onChange={e => setAuthPass(e.target.value)} maxLength={8} placeholder="8 chars max" />
          <label>note</label><input value={note} onChange={e => setNote(e.target.value)} />
        </div>
        <div style={{ marginTop: 12, fontSize: 13, fontWeight: 'bold' }}>
          멤버 선택 ({mode === 'active_standby' ? '정확히 2개' : '1개 이상'}):
        </div>
        <div style={{ maxHeight: 200, overflowY: 'auto', border: '1px solid #eee', padding: 4, fontSize: 13 }}>
          {free.length === 0 && <div style={{ color: '#888' }}>그룹 미정의 agent 없음 (모든 agent 가 이미 그룹 소속)</div>}
          {free.map(a => (
            <label key={a.id} style={{ display: 'block', padding: '2px 0' }}>
              <input type="checkbox" checked={selected.includes(a.id)} onChange={() => toggle(a.id)} />
              {' '}{a.name} ({a.ip_address ?? a.hostname ?? '?'}) — {a.status}
            </label>
          ))}
        </div>
        <div style={{ marginTop: 16, textAlign: 'right' }}>
          <button onClick={onClose} disabled={busy} style={{ marginRight: 8 }}>취소</button>
          <button onClick={onSubmit} disabled={busy}>{busy ? '생성중...' : '생성'}</button>
        </div>
      </div>
    </div>
  )
}
