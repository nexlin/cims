import { useState, useEffect, useCallback } from 'react'
import { groupsApi, type Group } from '../api/groups'
import { useToast } from '../components/Toast'

interface GroupExt extends Group {
  priority?: number
  encryption?: boolean
  emergency_call?: boolean
  org_code?: string
  session_start?: string | null
  session_end?: string | null
}

export default function PttGroupsPage() {
  const { show } = useToast()
  const [groups, setGroups] = useState<GroupExt[]>([])
  const [loading, setLoading] = useState(true)
  const [search, setSearch] = useState('')
  const [selected, setSelected] = useState<Set<string>>(new Set())

  // 인라인 편집
  const [editId, setEditId] = useState<string | null>(null)
  const [editForm, setEditForm] = useState<Partial<GroupExt>>({})

  // 추가
  const [adding, setAdding] = useState(false)
  const [addForm, setAddForm] = useState<Partial<GroupExt>>({ id: '', name: '', priority: 5, encryption: false, emergency_call: false, org_code: '' })

  // 멤버 편집
  const [memberGroupId, setMemberGroupId] = useState<string | null>(null)
  const [members, setMembers] = useState<Array<{user_id: string, priority: number}>>([])
  const [addMember, setAddMember] = useState({ user_id: '', priority: 5 })

  const load = useCallback(async () => {
    setLoading(true)
    try { setGroups(await groupsApi.list() as GroupExt[]) }
    catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [show])
  useEffect(() => { load() }, [load])

  const filtered = groups.filter(g => {
    if (!search) return true
    const s = search.toLowerCase()
    return g.id.toLowerCase().includes(s) || g.name.toLowerCase().includes(s)
  })

  function startEdit(g: GroupExt) {
    setEditId(g.id); setAdding(false)
    setEditForm({ name: g.name, priority: g.priority ?? 5, encryption: g.encryption, emergency_call: g.emergency_call, org_code: g.org_code || '' })
  }
  async function saveEdit() {
    if (!editId) return
    try { await groupsApi.update(editId, editForm as any); show('수정', 'ok'); setEditId(null); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  function startAdd() { setAdding(true); setEditId(null); setAddForm({ id: '', name: '', priority: 5, encryption: false, emergency_call: false, org_code: '' }) }
  async function saveAdd() {
    if (!addForm.id || !addForm.name) { show('ID/이름 필수', 'err'); return }
    try { await groupsApi.create(addForm as any); show('생성', 'ok'); setAdding(false); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  async function handleDelete(id: string) {
    if (!confirm(`그룹 ${id} 삭제?`)) return
    try { await groupsApi.delete(id); show('삭제', 'ok'); load(); if (memberGroupId === id) setMemberGroupId(null) }
    catch (e: unknown) { show(String(e), 'err') }
  }

  function toggleSelect(id: string) {
    setSelected(prev => { const n = new Set(prev); if (n.has(id)) n.delete(id); else n.add(id); return n })
  }

  async function openMembers(gid: string) {
    setMemberGroupId(gid)
    try {
      const ms = await groupsApi.listMembers(gid)
      setMembers(ms)
    } catch { setMembers([]) }
  }

  async function handleAddMember() {
    if (!memberGroupId || !addMember.user_id) return
    try {
      await groupsApi.addMember(memberGroupId, addMember)
      show('멤버 추가', 'ok')
      setAddMember({ user_id: '', priority: 5 })
      openMembers(memberGroupId)
      load()
    } catch (e: unknown) { show(String(e), 'err') }
  }

  async function handleRemoveMember(uid: string) {
    if (!memberGroupId) return
    try { await groupsApi.removeMember(memberGroupId, uid); show('삭제', 'ok'); openMembers(memberGroupId); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  return (
    <div style={{ display: 'flex', gap: 16, alignItems: 'flex-start' }}>
      {/* 좌측: 그룹 목록 */}
      <div style={{ flex: 1 }}>
        <div className="toolbar">
          <input className="search-input" placeholder="그룹 검색" value={search}
            onChange={e => setSearch(e.target.value)} style={{ maxWidth: 200 }} />
          {selected.size > 0 && <button className="btn btn--danger btn--sm">선택 삭제 ({selected.size})</button>}
        </div>

        {loading ? <div className="empty">로딩 중...</div> : (
          <div className="table-wrap">
            <table className="data-table">
              <thead>
                <tr>
                  <th style={{ width: 36 }}><input type="checkbox"
                    checked={filtered.length > 0 && filtered.every(g => selected.has(g.id))}
                    onChange={() => filtered.every(g => selected.has(g.id)) ? setSelected(new Set()) : setSelected(new Set(filtered.map(g => g.id)))} /></th>
                  <th>그룹명</th>
                  <th>그룹 ID</th>
                  <th style={{ width: 50 }}>우선</th>
                  <th style={{ width: 50 }}>암호</th>
                  <th style={{ width: 50 }}>긴급</th>
                  <th style={{ width: 80 }}>조직</th>
                  <th style={{ width: 50 }}>멤버</th>
                  <th style={{ width: 130 }}>작업</th>
                </tr>
              </thead>
              <tbody>
                {filtered.map(g => {
                  const isEditing = editId === g.id
                  return (
                    <tr key={g.id} style={selected.has(g.id) ? { background: 'rgba(74,144,217,0.08)' } : undefined}>
                      <td><input type="checkbox" checked={selected.has(g.id)} onChange={() => toggleSelect(g.id)} /></td>
                      <td>{isEditing ?
                        <input className="form-input" value={editForm.name || ''} onChange={e => setEditForm({...editForm, name: e.target.value})} style={{ width: '100%' }} autoFocus /> :
                        <span style={{ fontWeight: 500 }}>{g.name}</span>}
                      </td>
                      <td><span className="ts">{g.id}</span></td>
                      <td>{isEditing ?
                        <input className="form-input" type="number" value={editForm.priority ?? 5} onChange={e => setEditForm({...editForm, priority: Number(e.target.value)})} style={{ width: '100%' }} /> :
                        <span className="ts">{g.priority ?? 5}</span>}
                      </td>
                      <td>{isEditing ?
                        <input type="checkbox" checked={editForm.encryption || false} onChange={e => setEditForm({...editForm, encryption: e.target.checked})} /> :
                        <span className={`badge ${g.encryption ? 'badge--green' : 'badge--gray'}`}>{g.encryption ? 'Y' : 'N'}</span>}
                      </td>
                      <td>{isEditing ?
                        <input type="checkbox" checked={editForm.emergency_call || false} onChange={e => setEditForm({...editForm, emergency_call: e.target.checked})} /> :
                        <span className={`badge ${g.emergency_call ? 'badge--red' : 'badge--gray'}`}>{g.emergency_call ? 'Y' : 'N'}</span>}
                      </td>
                      <td>{isEditing ?
                        <input className="form-input" value={editForm.org_code || ''} onChange={e => setEditForm({...editForm, org_code: e.target.value})} style={{ width: '100%' }} /> :
                        <span className="ts">{g.org_code || '—'}</span>}
                      </td>
                      <td>
                        <button className="btn btn--sm btn--ghost" onClick={() => openMembers(g.id)}
                          style={{ color: memberGroupId === g.id ? 'var(--primary)' : undefined }}>
                          {g.members?.length ?? 0}명
                        </button>
                      </td>
                      <td className="actions">
                        {isEditing ? (
                          <><button className="btn btn--sm btn--primary" onClick={saveEdit}>저장</button>
                          <button className="btn btn--sm btn--ghost" onClick={() => setEditId(null)}>취소</button></>
                        ) : (
                          <><button className="btn btn--sm btn--outline" onClick={() => startEdit(g)}>편집</button>
                          <button className="btn btn--sm btn--danger" onClick={() => handleDelete(g.id)}>삭제</button></>
                        )}
                      </td>
                    </tr>
                  )
                })}
                {adding ? (
                  <tr style={{ background: 'rgba(74,144,217,0.08)' }}>
                    <td></td>
                    <td><input className="form-input" placeholder="그룹명 *" value={addForm.name || ''} onChange={e => setAddForm({...addForm, name: e.target.value})} style={{ width: '100%' }} /></td>
                    <td><input className="form-input" placeholder="그룹 ID *" value={addForm.id || ''} onChange={e => setAddForm({...addForm, id: e.target.value})} style={{ width: '100%' }} autoFocus /></td>
                    <td><input className="form-input" type="number" value={addForm.priority ?? 5} onChange={e => setAddForm({...addForm, priority: Number(e.target.value)})} style={{ width: '100%' }} /></td>
                    <td><input type="checkbox" checked={addForm.encryption || false} onChange={e => setAddForm({...addForm, encryption: e.target.checked})} /></td>
                    <td><input type="checkbox" checked={addForm.emergency_call || false} onChange={e => setAddForm({...addForm, emergency_call: e.target.checked})} /></td>
                    <td><input className="form-input" placeholder="조직" value={addForm.org_code || ''} onChange={e => setAddForm({...addForm, org_code: e.target.value})} style={{ width: '100%' }} /></td>
                    <td></td>
                    <td className="actions">
                      <button className="btn btn--sm btn--primary" onClick={saveAdd}>저장</button>
                      <button className="btn btn--sm btn--ghost" onClick={() => setAdding(false)}>취소</button>
                    </td>
                  </tr>
                ) : (
                  <tr><td colSpan={9} style={{ textAlign: 'center' }}>
                    <button className="btn btn--ghost btn--sm" onClick={startAdd} style={{ color: 'var(--primary)', fontSize: 12 }}>＋ 그룹 추가</button>
                  </td></tr>
                )}
              </tbody>
            </table>
          </div>
        )}
      </div>

      {/* 우측: 멤버 편집 */}
      {memberGroupId && (
        <div className="panel" style={{ width: 300 }}>
          <div style={{ padding: '10px 12px', fontWeight: 600, fontSize: 13, borderBottom: '1px solid var(--border)' }}>
            멤버 — {memberGroupId}
          </div>
          <table className="data-table">
            <thead><tr><th>PTT MSISDN</th><th style={{ width: 50 }}>우선</th><th style={{ width: 40 }}></th></tr></thead>
            <tbody>
              {members.map(m => (
                <tr key={m.user_id}>
                  <td className="ts">{m.user_id}</td>
                  <td className="ts">{m.priority}</td>
                  <td><button className="btn btn--sm btn--danger" onClick={() => handleRemoveMember(m.user_id)}>×</button></td>
                </tr>
              ))}
              <tr>
                <td><input className="form-input" placeholder="PTT MSISDN" value={addMember.user_id}
                  onChange={e => setAddMember({...addMember, user_id: e.target.value})} style={{ width: '100%' }} /></td>
                <td><input className="form-input" type="number" value={addMember.priority}
                  onChange={e => setAddMember({...addMember, priority: Number(e.target.value)})} style={{ width: '100%' }} /></td>
                <td><button className="btn btn--sm btn--primary" onClick={handleAddMember}>＋</button></td>
              </tr>
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}
