import { useState, useEffect, useCallback } from 'react'
import { groupsApi, type Group } from '../api/groups'
import { useToast } from '../components/Toast'

interface GroupExt extends Group {
  priority?: number
  encryption?: boolean
  emergency_call?: boolean
  org_code?: string
  video_enabled?: boolean
  session_start?: string | null
  session_end?: string | null
}

function fmtDt(v: string | null | undefined) {
  if (!v) return '—'
  return v.replace('T', ' ').substring(0, 16)
}

export default function PttGroupsPage() {
  const { show } = useToast()
  const [groups, setGroups] = useState<GroupExt[]>([])
  const [loading, setLoading] = useState(true)
  const [search, setSearch] = useState('')

  // 편집/추가
  const [editId, setEditId] = useState<string | null>(null)
  const [editForm, setEditForm] = useState<Partial<GroupExt>>({})
  const [adding, setAdding] = useState(false)
  const [addForm, setAddForm] = useState<Partial<GroupExt>>({})

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
    setEditForm({
      name: g.name, priority: g.priority ?? 5,
      encryption: g.encryption, emergency_call: g.emergency_call,
      video_enabled: g.video_enabled, org_code: g.org_code || '',
      session_start: g.session_start || '', session_end: g.session_end || '',
    })
  }
  async function saveEdit() {
    if (!editId) return
    const body = { ...editForm }
    if (body.session_start === '') body.session_start = null
    if (body.session_end === '') body.session_end = null
    try { await groupsApi.update(editId, body as any); show('수정', 'ok'); setEditId(null); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  function startAdd() {
    setAdding(true); setEditId(null)
    setAddForm({ id: '', name: '', priority: 5, encryption: false, emergency_call: false, video_enabled: false, org_code: '', session_start: '', session_end: '' })
  }
  async function saveAdd() {
    if (!addForm.id || !addForm.name) { show('ID/이름 필수', 'err'); return }
    const body = { ...addForm }
    if (body.session_start === '') body.session_start = null
    if (body.session_end === '') body.session_end = null
    try { await groupsApi.create(body as any); show('생성', 'ok'); setAdding(false); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  async function handleDelete(id: string) {
    if (!confirm(`그룹 ${id} 삭제?`)) return
    try { await groupsApi.delete(id); show('삭제', 'ok'); load(); if (memberGroupId === id) setMemberGroupId(null) }
    catch (e: unknown) { show(String(e), 'err') }
  }

  async function openMembers(gid: string) {
    setMemberGroupId(prev => prev === gid ? null : gid)
    try { setMembers(await groupsApi.listMembers(gid)) } catch { setMembers([]) }
  }
  async function handleAddMember() {
    if (!memberGroupId || !addMember.user_id) return
    try {
      await groupsApi.addMember(memberGroupId, addMember)
      show('멤버 추가', 'ok'); setAddMember({ user_id: '', priority: 5 })
      openMembers(memberGroupId); load()
    } catch (e: unknown) { show(String(e), 'err') }
  }
  async function handleRemoveMember(uid: string) {
    if (!memberGroupId) return
    try { await groupsApi.removeMember(memberGroupId, uid); show('삭제', 'ok'); openMembers(memberGroupId); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  // ── 카드 렌더링 ──

  function renderEditFields(form: Partial<GroupExt>, setForm: (f: Partial<GroupExt>) => void, isNew: boolean) {
    return (
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '6px 10px', fontSize: 12 }}>
        {isNew && <>
          <label>그룹 ID *</label>
          <input className="form-input" value={form.id || ''} onChange={e => setForm({...form, id: e.target.value})} autoFocus />
        </>}
        <label>그룹명 *</label>
        <input className="form-input" value={form.name || ''} onChange={e => setForm({...form, name: e.target.value})} />
        <label>우선순위</label>
        <input className="form-input" type="number" value={form.priority ?? 5} onChange={e => setForm({...form, priority: Number(e.target.value)})} />
        <label>조직 코드</label>
        <input className="form-input" value={form.org_code || ''} onChange={e => setForm({...form, org_code: e.target.value})} />
        <label>세션 시작</label>
        <input className="form-input" type="datetime-local" value={(form.session_start || '').replace(' ','T').substring(0,16)}
          onChange={e => setForm({...form, session_start: e.target.value || null})} />
        <label>세션 종료</label>
        <input className="form-input" type="datetime-local" value={(form.session_end || '').replace(' ','T').substring(0,16)}
          onChange={e => setForm({...form, session_end: e.target.value || null})} />
        <label>암호화</label>
        <input type="checkbox" checked={form.encryption || false} onChange={e => setForm({...form, encryption: e.target.checked})} />
        <label>긴급통화</label>
        <input type="checkbox" checked={form.emergency_call || false} onChange={e => setForm({...form, emergency_call: e.target.checked})} />
        <label>영상</label>
        <input type="checkbox" checked={form.video_enabled || false} onChange={e => setForm({...form, video_enabled: e.target.checked})} />
      </div>
    )
  }

  function renderCard(g: GroupExt) {
    const isEditing = editId === g.id
    const isMemberOpen = memberGroupId === g.id
    return (
      <div key={g.id} style={{
        border: '1px solid var(--border)', borderRadius: 8, padding: 12,
        background: isEditing ? 'rgba(74,144,217,0.06)' : 'var(--bg-card)',
        display: 'flex', flexDirection: 'column', gap: 8, minWidth: 0,
      }}>
        {/* 헤더 */}
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
          <div style={{ fontWeight: 600, fontSize: 14, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
            {g.name}
          </div>
          <div style={{ display: 'flex', gap: 4 }}>
            {g.encryption && <span className="badge badge--green" style={{ fontSize: 9 }}>암호</span>}
            {g.emergency_call && <span className="badge badge--red" style={{ fontSize: 9 }}>긴급</span>}
            {g.video_enabled && <span className="badge badge--blue" style={{ fontSize: 9 }}>영상</span>}
          </div>
        </div>

        {isEditing ? (
          <>
            {renderEditFields(editForm, setEditForm, false)}
            <div style={{ display: 'flex', gap: 6, justifyContent: 'flex-end', marginTop: 4 }}>
              <button className="btn btn--sm btn--primary" onClick={saveEdit}>저장</button>
              <button className="btn btn--sm btn--ghost" onClick={() => setEditId(null)}>취소</button>
            </div>
          </>
        ) : (
          <>
            {/* 속성 */}
            <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '2px 8px', fontSize: 11, color: 'var(--text-muted)' }}>
              <span>ID</span><span className="ts" style={{ fontWeight: 500 }}>{g.id}</span>
              <span>우선순위</span><span>{g.priority ?? 5}</span>
              <span>조직</span><span>{g.org_code || '—'}</span>
              <span>세션 시작</span><span>{fmtDt(g.session_start)}</span>
              <span>세션 종료</span><span>{fmtDt(g.session_end)}</span>
            </div>

            {/* 멤버 */}
            <div style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 12 }}>
              <button className="btn btn--sm btn--ghost" onClick={() => openMembers(g.id)}
                style={{ color: isMemberOpen ? 'var(--primary)' : undefined, padding: '2px 6px' }}>
                멤버 {g.members?.length ?? 0}명 {isMemberOpen ? '▲' : '▼'}
              </button>
            </div>

            {/* 멤버 인라인 */}
            {isMemberOpen && (
              <div style={{ border: '1px solid var(--border)', borderRadius: 6, padding: 8, fontSize: 11 }}>
                {members.map(m => (
                  <div key={m.user_id} style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '2px 0' }}>
                    <span className="ts">{m.user_id} <span style={{ color: 'var(--text-muted)' }}>(P:{m.priority})</span></span>
                    <button className="btn btn--sm btn--danger" style={{ padding: '0 4px', fontSize: 10 }} onClick={() => handleRemoveMember(m.user_id)}>×</button>
                  </div>
                ))}
                <div style={{ display: 'flex', gap: 4, marginTop: 4 }}>
                  <input className="form-input" placeholder="PTT MSISDN" value={addMember.user_id}
                    onChange={e => setAddMember({...addMember, user_id: e.target.value})} style={{ flex: 1, fontSize: 11 }} />
                  <input className="form-input" type="number" value={addMember.priority}
                    onChange={e => setAddMember({...addMember, priority: Number(e.target.value)})} style={{ width: 40, fontSize: 11 }} />
                  <button className="btn btn--sm btn--primary" style={{ padding: '2px 6px' }} onClick={handleAddMember}>＋</button>
                </div>
              </div>
            )}

            {/* 작업 */}
            <div style={{ display: 'flex', gap: 6, justifyContent: 'flex-end' }}>
              <button className="btn btn--sm btn--outline" onClick={() => startEdit(g)}>편집</button>
              <button className="btn btn--sm btn--danger" onClick={() => handleDelete(g.id)}>삭제</button>
            </div>
          </>
        )}
      </div>
    )
  }

  return (
    <div className="page">
      <div className="toolbar">
        <input className="search-input" placeholder="그룹 검색" value={search}
          onChange={e => setSearch(e.target.value)} style={{ maxWidth: 200 }} />
        <button className="btn btn--primary btn--sm" onClick={startAdd} style={{ marginLeft: 'auto' }}>＋ 그룹 추가</button>
      </div>

      {loading ? <div className="empty">로딩 중...</div> : (
        <div style={{
          display: 'grid',
          gridTemplateColumns: 'repeat(auto-fill, minmax(300px, 1fr))',
          gap: 12,
          padding: '4px 0',
        }}>
          {/* 추가 카드 */}
          {adding && (
            <div style={{
              border: '2px dashed var(--primary)', borderRadius: 8, padding: 12,
              background: 'rgba(74,144,217,0.04)',
              display: 'flex', flexDirection: 'column', gap: 8,
            }}>
              <div style={{ fontWeight: 600, fontSize: 14, color: 'var(--primary)' }}>새 그룹</div>
              {renderEditFields(addForm, setAddForm, true)}
              <div style={{ display: 'flex', gap: 6, justifyContent: 'flex-end', marginTop: 4 }}>
                <button className="btn btn--sm btn--primary" onClick={saveAdd}>생성</button>
                <button className="btn btn--sm btn--ghost" onClick={() => setAdding(false)}>취소</button>
              </div>
            </div>
          )}

          {filtered.map(g => renderCard(g))}

          {filtered.length === 0 && !adding && (
            <div className="empty" style={{ gridColumn: '1/-1' }}>그룹 없음</div>
          )}
        </div>
      )}
    </div>
  )
}
