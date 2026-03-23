import { useState, useEffect, useCallback } from 'react'
import { usersApi, type User, type UserInput } from '../api/users'
import Modal from '../components/Modal'
import { useToast } from '../components/Toast'

const EMPTY: UserInput = {
  id: '', auth_id: '', passwd: '', org_id: '',
  dnd: false, forward_id: '', reject_id: [],
}

export default function UsersPage() {
  const { show } = useToast()
  const [users, setUsers] = useState<User[]>([])
  const [loading, setLoading] = useState(true)
  const [search, setSearch] = useState('')

  // form modal
  const [formOpen, setFormOpen] = useState(false)
  const [editing, setEditing] = useState<User | null>(null)
  const [form, setForm] = useState<UserInput>(EMPTY)
  const [rejectStr, setRejectStr] = useState('')
  const [saving, setSaving] = useState(false)

  // detail modal
  const [detail, setDetail] = useState<User | null>(null)

  // delete confirm
  const [delTarget, setDelTarget] = useState<User | null>(null)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      setUsers(await usersApi.list())
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [show])

  useEffect(() => { load() }, [load])

  // ── open add form ──────────────────────────────────────────
  function openAdd() {
    setEditing(null)
    setForm(EMPTY)
    setRejectStr('')
    setFormOpen(true)
  }

  // ── open edit form ─────────────────────────────────────────
  function openEdit(u: User) {
    setEditing(u)
    setForm({
      id: u.id, auth_id: u.auth_id, passwd: '',
      org_id: u.org_id, dnd: u.dnd,
      forward_id: u.forward_id, reject_id: u.reject_id,
    })
    setRejectStr(u.reject_id.join(', '))
    setFormOpen(true)
  }

  // ── save ───────────────────────────────────────────────────
  async function handleSave() {
    const payload: UserInput = {
      ...form,
      reject_id: rejectStr.split(',').map(s => s.trim()).filter(Boolean),
    }
    setSaving(true)
    try {
      if (editing) {
        const { id, ...rest } = payload
        void id
        await usersApi.update(editing.id, rest)
        show('가입자 정보가 수정되었습니다.')
      } else {
        await usersApi.create(payload)
        show('가입자가 등록되었습니다.')
      }
      setFormOpen(false)
      await load()
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setSaving(false)
    }
  }

  // ── delete ─────────────────────────────────────────────────
  async function handleDelete() {
    if (!delTarget) return
    try {
      await usersApi.delete(delTarget.id)
      show('가입자가 삭제되었습니다.')
      setDelTarget(null)
      await load()
    } catch (e: unknown) {
      show(String(e), 'err')
    }
  }

  const filtered = users.filter(u =>
    u.id.includes(search) ||
    u.auth_id.toLowerCase().includes(search.toLowerCase()) ||
    u.org_id.toLowerCase().includes(search.toLowerCase())
  )

  return (
    <div className="page">
      {/* toolbar */}
      <div className="toolbar">
        <input
          className="search-input"
          placeholder="ID / Auth ID / 조직 검색…"
          value={search}
          onChange={e => setSearch(e.target.value)}
        />
        <button className="btn btn--primary" onClick={openAdd}>＋ 가입자 추가</button>
        <button className="btn btn--ghost" onClick={load}>↻ 새로고침</button>
      </div>

      {/* table */}
      {loading ? (
        <div className="empty">로딩 중…</div>
      ) : (
        <div className="table-wrap">
          <table className="data-table">
            <thead>
              <tr>
                <th>ID (MSISDN)</th>
                <th>Auth ID</th>
                <th>조직</th>
                <th>DND</th>
                <th>착신전환</th>
                <th>등록일시</th>
                <th style={{ width: 130 }}>작업</th>
              </tr>
            </thead>
            <tbody>
              {filtered.length === 0 ? (
                <tr><td colSpan={7} className="empty-cell">가입자 없음</td></tr>
              ) : filtered.map(u => (
                <tr key={u.id}>
                  <td>
                    <button className="link-btn" onClick={() => setDetail(u)}>{u.id}</button>
                  </td>
                  <td>{u.auth_id}</td>
                  <td>{u.org_id || '—'}</td>
                  <td>
                    <span className={`badge ${u.dnd ? 'badge--red' : 'badge--gray'}`}>
                      {u.dnd ? 'ON' : 'OFF'}
                    </span>
                  </td>
                  <td>{u.forward_id || '—'}</td>
                  <td className="ts">{u.register_time ? u.register_time.replace('T', ' ') : '—'}</td>
                  <td className="actions">
                    <button className="btn btn--sm btn--outline" onClick={() => openEdit(u)}>편집</button>
                    <button className="btn btn--sm btn--danger" onClick={() => setDelTarget(u)}>삭제</button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

      {/* ── add/edit modal ── */}
      {formOpen && (
        <Modal title={editing ? `가입자 편집 — ${editing.id}` : '가입자 추가'} onClose={() => setFormOpen(false)}>
          <div className="form-grid">
            <label>ID (MSISDN) *</label>
            <input
              className="form-input"
              value={form.id}
              disabled={!!editing}
              onChange={e => setForm(f => ({ ...f, id: e.target.value }))}
              placeholder="+821001234567"
            />

            <label>Auth ID</label>
            <input
              className="form-input"
              value={form.auth_id}
              onChange={e => setForm(f => ({ ...f, auth_id: e.target.value }))}
              placeholder="단말 인증 ID (비우면 ID와 동일)"
            />

            <label>비밀번호</label>
            <input
              type="password"
              className="form-input"
              value={form.passwd}
              onChange={e => setForm(f => ({ ...f, passwd: e.target.value }))}
              placeholder={editing ? '변경 시에만 입력' : ''}
            />

            <label>조직 ID</label>
            <input
              className="form-input"
              value={form.org_id}
              onChange={e => setForm(f => ({ ...f, org_id: e.target.value }))}
            />

            <label>착신전환</label>
            <input
              className="form-input"
              value={form.forward_id}
              onChange={e => setForm(f => ({ ...f, forward_id: e.target.value }))}
              placeholder="+821009999999"
            />

            <label>착신거부 목록</label>
            <input
              className="form-input"
              value={rejectStr}
              onChange={e => setRejectStr(e.target.value)}
              placeholder="+82100001, +82100002 (쉼표 구분)"
            />

            <label>DND</label>
            <label className="toggle">
              <input
                type="checkbox"
                checked={form.dnd}
                onChange={e => setForm(f => ({ ...f, dnd: e.target.checked }))}
              />
              <span className="toggle-track" />
              <span className="toggle-label">{form.dnd ? '켜짐' : '꺼짐'}</span>
            </label>
          </div>

          <div className="modal-footer">
            <button className="btn btn--ghost" onClick={() => setFormOpen(false)}>취소</button>
            <button className="btn btn--primary" onClick={handleSave} disabled={saving}>
              {saving ? '저장 중…' : '저장'}
            </button>
          </div>
        </Modal>
      )}

      {/* ── detail modal ── */}
      {detail && (
        <Modal title={`상세 — ${detail.id}`} onClose={() => setDetail(null)}>
          <dl className="detail-list">
            <dt>ID</dt>       <dd>{detail.id}</dd>
            <dt>Auth ID</dt>  <dd>{detail.auth_id}</dd>
            <dt>조직</dt>     <dd>{detail.org_id || '—'}</dd>
            <dt>DND</dt>      <dd>{detail.dnd ? '켜짐' : '꺼짐'}</dd>
            <dt>착신전환</dt>  <dd>{detail.forward_id || '—'}</dd>
            <dt>착신거부</dt>  <dd>{detail.reject_id.length ? detail.reject_id.join(', ') : '—'}</dd>
            <dt>생성시간</dt>  <dd>{detail.create_time ?? '—'}</dd>
            <dt>수정시간</dt>  <dd>{detail.update_time ?? '—'}</dd>
            <dt>등록시간</dt>  <dd>{detail.register_time ?? '—'}</dd>
            <dt>로그아웃</dt>  <dd>{detail.logout_time ?? '—'}</dd>
          </dl>
          <div className="modal-footer">
            <button className="btn btn--outline" onClick={() => { setDetail(null); openEdit(detail) }}>편집</button>
            <button className="btn btn--ghost" onClick={() => setDetail(null)}>닫기</button>
          </div>
        </Modal>
      )}

      {/* ── delete confirm ── */}
      {delTarget && (
        <Modal title="가입자 삭제" onClose={() => setDelTarget(null)}>
          <p className="confirm-text">
            <strong>{delTarget.id}</strong> 가입자를 삭제하시겠습니까?<br />
            이 작업은 되돌릴 수 없습니다.
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
