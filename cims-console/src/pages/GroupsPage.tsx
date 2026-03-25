import { useState, useEffect, useCallback } from 'react'
import { groupsApi, type Group, type Member } from '../api/groups'
import Modal from '../components/Modal'
import { useToast } from '../components/Toast'

const EMPTY_GROUP = { id: '', name: '' }

export default function GroupsPage() {
  const { show } = useToast()
  const [groups, setGroups] = useState<Group[]>([])
  const [loading, setLoading] = useState(true)
  const [search, setSearch] = useState('')

  // group form
  const [formOpen, setFormOpen] = useState(false)
  const [editing, setEditing] = useState<Group | null>(null)
  const [form, setForm] = useState(EMPTY_GROUP)
  const [saving, setSaving] = useState(false)

  // delete
  const [delTarget, setDelTarget] = useState<Group | null>(null)

  // members panel
  const [membersGroup, setMembersGroup] = useState<Group | null>(null)
  const [newMemberId, setNewMemberId] = useState('')
  const [newMemberPrio, setNewMemberPrio] = useState(0)
  const [memberSaving, setMemberSaving] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      setGroups(await groupsApi.list())
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [show])

  useEffect(() => { load() }, [load])

  // ── refresh members panel when groups reload ───────────────
  useEffect(() => {
    if (!membersGroup) return
    const updated = groups.find(g => g.id === membersGroup.id)
    if (updated) setMembersGroup(updated)
  }, [groups]) // eslint-disable-line react-hooks/exhaustive-deps

  function openAdd() {
    setEditing(null)
    setForm(EMPTY_GROUP)
    setFormOpen(true)
  }

  function openEdit(g: Group) {
    setEditing(g)
    setForm({ id: g.id, name: g.name })
    setFormOpen(true)
  }

  async function handleSave() {
    setSaving(true)
    try {
      if (editing) {
        await groupsApi.update(editing.id, { name: form.name })
        show('그룹 정보가 수정되었습니다.')
      } else {
        await groupsApi.create({ id: form.id, name: form.name })
        show('그룹이 생성되었습니다.')
      }
      setFormOpen(false)
      await load()
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setSaving(false)
    }
  }

  async function handleDelete() {
    if (!delTarget) return
    try {
      await groupsApi.delete(delTarget.id)
      show('그룹이 삭제되었습니다.')
      setDelTarget(null)
      if (membersGroup?.id === delTarget.id) setMembersGroup(null)
      await load()
    } catch (e: unknown) {
      show(String(e), 'err')
    }
  }

  async function handleAddMember() {
    if (!membersGroup || !newMemberId.trim()) return
    setMemberSaving(true)
    try {
      await groupsApi.addMember(membersGroup.id, {
        user_id: newMemberId.trim(),
        priority: newMemberPrio,
      })
      show('멤버가 추가되었습니다.')
      setNewMemberId('')
      setNewMemberPrio(0)
      await load()
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setMemberSaving(false)
    }
  }

  async function handleRemoveMember(userId: string) {
    if (!membersGroup) return
    try {
      await groupsApi.removeMember(membersGroup.id, userId)
      show('멤버가 삭제되었습니다.')
      await load()
    } catch (e: unknown) {
      show(String(e), 'err')
    }
  }

  const filtered = groups.filter(g =>
    g.id.includes(search) || g.name.toLowerCase().includes(search.toLowerCase())
  )

  return (
    <div className="page groups-layout">
      {/* ── left: group list ──────────────────────────────── */}
      <div className="panel">
        <div className="toolbar">
          <input
            className="search-input"
            placeholder="그룹 ID / 이름 검색…"
            value={search}
            onChange={e => setSearch(e.target.value)}
          />
          <button className="btn btn--primary" onClick={openAdd}>＋ 그룹 추가</button>
          <button className="btn btn--ghost" onClick={load}>↻</button>
        </div>

        {loading ? (
          <div className="empty">로딩 중…</div>
        ) : (
          <div className="table-wrap">
            <table className="data-table">
              <thead>
                <tr>
                  <th>그룹 ID</th>
                  <th>그룹명</th>
                  <th>멤버 수</th>
                  <th style={{ width: 150 }}>작업</th>
                </tr>
              </thead>
              <tbody>
                {filtered.length === 0 ? (
                  <tr><td colSpan={4} className="empty-cell">그룹 없음</td></tr>
                ) : filtered.map(g => (
                  <tr
                    key={g.id}
                    className={membersGroup?.id === g.id ? 'row--selected' : ''}
                    onClick={() => setMembersGroup(g)}
                    style={{ cursor: 'pointer' }}
                  >
                    <td>{g.id}</td>
                    <td>{g.name}</td>
                    <td><span className="badge badge--blue">{g.members.length}</span></td>
                    <td className="actions" onClick={e => e.stopPropagation()}>
                      <button className="btn btn--sm btn--outline" onClick={() => openEdit(g)}>편집</button>
                      <button className="btn btn--sm btn--danger"  onClick={() => setDelTarget(g)}>삭제</button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>

      {/* ── right: members panel ──────────────────────────── */}
      <div className="panel panel--members">
        {membersGroup ? (
          <>
            <div className="panel-header">
              <span className="panel-title">멤버 — {membersGroup.name} <small>({membersGroup.id})</small></span>
            </div>

            {/* add member row */}
            <div className="member-add-row">
              <input
                className="form-input"
                placeholder="User ID (MSISDN)"
                value={newMemberId}
                onChange={e => setNewMemberId(e.target.value)}
                onKeyDown={e => e.key === 'Enter' && handleAddMember()}
              />
              <input
                type="number"
                className="form-input form-input--sm"
                placeholder="우선순위"
                value={newMemberPrio}
                onChange={e => setNewMemberPrio(Number(e.target.value))}
                min={0}
              />
              <button
                className="btn btn--primary"
                onClick={handleAddMember}
                disabled={memberSaving || !newMemberId.trim()}
              >
                추가
              </button>
            </div>

            <div className="table-wrap">
              <table className="data-table">
                <thead>
                  <tr>
                    <th>User ID</th>
                    <th style={{ width: 80 }}>우선순위</th>
                    <th style={{ width: 70 }}>삭제</th>
                  </tr>
                </thead>
                <tbody>
                  {membersGroup.members.length === 0 ? (
                    <tr><td colSpan={3} className="empty-cell">멤버 없음</td></tr>
                  ) : membersGroup.members.map((m: Member) => (
                    <tr key={m.user_id}>
                      <td>{m.user_id}</td>
                      <td>{m.priority}</td>
                      <td>
                        <button
                          className="btn btn--sm btn--danger"
                          onClick={() => handleRemoveMember(m.user_id)}
                        >
                          삭제
                        </button>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </>
        ) : (
          <div className="empty members-hint">← 그룹을 선택하면 멤버를 관리할 수 있습니다.</div>
        )}
      </div>

      {/* ── group form modal ── */}
      {formOpen && (
        <Modal title={editing ? `그룹 편집 — ${editing.id}` : '그룹 추가'} onClose={() => setFormOpen(false)}>
          <div className="form-grid">
            <label>그룹 ID *</label>
            <input
              className="form-input"
              value={form.id}
              disabled={!!editing}
              onChange={e => setForm(f => ({ ...f, id: e.target.value }))}
              placeholder="+821001000000"
            />
            <label>그룹명 *</label>
            <input
              className="form-input"
              value={form.name}
              onChange={e => setForm(f => ({ ...f, name: e.target.value }))}
              placeholder="예: 작전1팀"
            />
          </div>
          <div className="modal-footer">
            <button className="btn btn--ghost" onClick={() => setFormOpen(false)}>취소</button>
            <button className="btn btn--primary" onClick={handleSave} disabled={saving}>
              {saving ? '저장 중…' : '저장'}
            </button>
          </div>
        </Modal>
      )}

      {/* ── delete confirm ── */}
      {delTarget && (
        <Modal title="그룹 삭제" onClose={() => setDelTarget(null)}>
          <p className="confirm-text">
            <strong>{delTarget.name}</strong> ({delTarget.id}) 그룹을 삭제하시겠습니까?<br />
            멤버 목록도 함께 삭제됩니다.
          </p>
          <div className="modal-footer">
            <button className="btn btn--ghost" onClick={() => setDelTarget(null)}>취소</button>
            <button className="btn btn--danger" onClick={handleDelete}>삭제</button>
          </div>
        </Modal>
      )}
    </div>
  )
}
