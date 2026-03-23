import { useState, useEffect, useCallback } from 'react'
import { usersApi, type UserSummary, type UserDetail, type UserInput, type Subscription } from '../api/users'
import Modal from '../components/Modal'
import { useToast } from '../components/Toast'

// ── Subscription form state ───────────────────────────────────
interface SubForm {
  id: string
  auth_id: string
  passwd: string
  dnd: boolean
  forward_id: string
}

const EMPTY_SUB_FORM: SubForm = {
  id: '', auth_id: '', passwd: '', dnd: false, forward_id: '',
}

// ── Person form state ─────────────────────────────────────────
interface FormState {
  id: string
  name: string
  org_id: string
  details: string
  reject_id: string[]
}

const EMPTY_FORM: FormState = {
  id: '', name: '', org_id: '', details: '', reject_id: [],
}

// ── Detail modal tab type ─────────────────────────────────────
type DetailTab = 'base' | 'call' | 'ptt'

export default function UsersPage() {
  const { show } = useToast()
  const [users, setUsers] = useState<UserSummary[]>([])
  const [loading, setLoading] = useState(true)
  const [search, setSearch] = useState('')

  // person add/edit form modal
  const [formOpen, setFormOpen] = useState(false)
  const [editingId, setEditingId] = useState<string | null>(null)
  const [form, setForm] = useState<FormState>(EMPTY_FORM)
  const [rejectStr, setRejectStr] = useState('')
  const [saving, setSaving] = useState(false)

  // detail modal
  const [detail, setDetail] = useState<UserDetail | null>(null)
  const [detailTab, setDetailTab] = useState<DetailTab>('base')
  const [detailLoading, setDetailLoading] = useState(false)

  // subscription add modal
  const [addSubOpen, setAddSubOpen] = useState<{svc: 'call'|'ptt'} | null>(null)
  const [subForm, setSubForm] = useState<SubForm>(EMPTY_SUB_FORM)
  const [subSaving, setSubSaving] = useState(false)

  // subscription edit modal
  const [editSub, setEditSub] = useState<{svc: 'call'|'ptt'; sub: Subscription} | null>(null)
  const [editSubForm, setEditSubForm] = useState<SubForm>(EMPTY_SUB_FORM)
  const [editSubSaving, setEditSubSaving] = useState(false)

  // delete confirm
  const [delTarget, setDelTarget] = useState<UserSummary | null>(null)

  // ── load list ─────────────────────────────────────────────
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

  // ── refresh detail ─────────────────────────────────────────
  const refreshDetail = useCallback(async (pid: string) => {
    try {
      const d = await usersApi.get(pid)
      setDetail(d)
    } catch (e: unknown) {
      show(String(e), 'err')
    }
  }, [show])

  // ── open add person form ───────────────────────────────────
  function openAdd() {
    setEditingId(null)
    setForm(EMPTY_FORM)
    setRejectStr('')
    setFormOpen(true)
  }

  // ── open edit person form ──────────────────────────────────
  function openEdit(u: UserSummary) {
    setEditingId(u.id)
    setForm({
      id: u.id,
      name: u.name,
      org_id: u.org_id,
      details: u.details ?? '',
      reject_id: u.reject_id,
    })
    setRejectStr(u.reject_id.join(', '))
    setFormOpen(true)
  }

  // ── save person ────────────────────────────────────────────
  async function handleSave() {
    const rejectIds = rejectStr.split(',').map(s => s.trim()).filter(Boolean)
    setSaving(true)
    try {
      if (editingId) {
        await usersApi.update(editingId, {
          name: form.name,
          org_id: form.org_id,
          details: form.details || undefined,
          reject_id: rejectIds,
        })
        show('가입자 정보가 수정되었습니다.')
      } else {
        const payload: UserInput = {
          id: form.id,
          name: form.name,
          org_id: form.org_id,
          details: form.details || undefined,
          reject_id: rejectIds,
        }
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

  // ── open detail modal ──────────────────────────────────────
  async function openDetail(u: UserSummary) {
    setDetail(null)
    setDetailTab('base')
    setDetailLoading(true)
    try {
      const d = await usersApi.get(u.id)
      setDetail(d)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setDetailLoading(false)
    }
  }

  // ── delete person ──────────────────────────────────────────
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

  // ── open subscription add modal ────────────────────────────
  function openAddSub(svc: 'call'|'ptt') {
    setSubForm(EMPTY_SUB_FORM)
    setAddSubOpen({ svc })
  }

  // ── save new subscription ──────────────────────────────────
  async function handleAddSub() {
    if (!detail || !addSubOpen) return
    setSubSaving(true)
    try {
      await usersApi.addSub(detail.id, addSubOpen.svc, {
        id: subForm.id,
        auth_id: subForm.auth_id,
        passwd: subForm.passwd,
        dnd: subForm.dnd,
        forward_id: subForm.forward_id,
      })
      show('번호가 추가되었습니다.')
      setAddSubOpen(null)
      await refreshDetail(detail.id)
      await load()
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setSubSaving(false)
    }
  }

  // ── open subscription edit modal ───────────────────────────
  function openEditSub(svc: 'call'|'ptt', sub: Subscription) {
    setEditSub({ svc, sub })
    setEditSubForm({
      id: sub.id,
      auth_id: sub.auth_id,
      passwd: '',
      dnd: sub.dnd,
      forward_id: sub.forward_id,
    })
  }

  // ── save subscription edit ─────────────────────────────────
  async function handleEditSub() {
    if (!detail || !editSub) return
    setEditSubSaving(true)
    try {
      await usersApi.updateSub(detail.id, editSub.svc, editSub.sub.id, {
        auth_id: editSubForm.auth_id,
        passwd: editSubForm.passwd,
        dnd: editSubForm.dnd,
        forward_id: editSubForm.forward_id,
      })
      show('번호 정보가 수정되었습니다.')
      setEditSub(null)
      await refreshDetail(detail.id)
      await load()
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setEditSubSaving(false)
    }
  }

  // ── delete subscription ────────────────────────────────────
  async function handleDeleteSub(svc: 'call'|'ptt', msisdn: string) {
    if (!detail) return
    try {
      await usersApi.deleteSub(detail.id, svc, msisdn)
      show('번호가 삭제되었습니다.')
      await refreshDetail(detail.id)
      await load()
    } catch (e: unknown) {
      show(String(e), 'err')
    }
  }

  const filtered = users.filter(u =>
    u.id.includes(search) ||
    u.name.toLowerCase().includes(search.toLowerCase()) ||
    u.org_id.toLowerCase().includes(search.toLowerCase())
  )

  return (
    <div className="page">
      {/* toolbar */}
      <div className="toolbar">
        <input
          className="search-input"
          placeholder="ID / 이름 / 조직 검색…"
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
                <th>ID (개인)</th>
                <th>이름</th>
                <th>조직</th>
                <th>Call 번호</th>
                <th>PTT 번호</th>
                <th style={{ width: 130 }}>작업</th>
              </tr>
            </thead>
            <tbody>
              {filtered.length === 0 ? (
                <tr><td colSpan={6} className="empty-cell">가입자 없음</td></tr>
              ) : filtered.map(u => (
                <tr key={u.id}>
                  <td>
                    <button className="link-btn" onClick={() => openDetail(u)}>{u.id}</button>
                  </td>
                  <td>{u.name || '—'}</td>
                  <td>{u.org_id || '—'}</td>
                  <td>
                    {u.call_count > 0
                      ? <span className="badge badge--blue">{u.call_count}개</span>
                      : <span className="badge badge--gray">없음</span>
                    }
                  </td>
                  <td>
                    {u.ptt_count > 0
                      ? <span className="badge badge--green">{u.ptt_count}개</span>
                      : <span className="badge badge--gray">없음</span>
                    }
                  </td>
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

      {/* ── person add/edit modal ── */}
      {formOpen && (
        <Modal title={editingId ? `가입자 편집 — ${editingId}` : '가입자 추가'} onClose={() => setFormOpen(false)}>
          <div className="form-section-title">기본정보</div>
          <div className="form-grid">
            <label>ID (개인 식별자) *</label>
            <input
              className="form-input"
              value={form.id}
              disabled={!!editingId}
              onChange={e => setForm(f => ({ ...f, id: e.target.value }))}
              placeholder="+821357007001"
            />

            <label>이름</label>
            <input
              className="form-input"
              value={form.name}
              onChange={e => setForm(f => ({ ...f, name: e.target.value }))}
              placeholder="표시 이름"
            />

            <label>조직 ID</label>
            <input
              className="form-input"
              value={form.org_id}
              onChange={e => setForm(f => ({ ...f, org_id: e.target.value }))}
            />

            <label>세부사항</label>
            <input
              className="form-input"
              value={form.details}
              onChange={e => setForm(f => ({ ...f, details: e.target.value }))}
            />

            <label>착신거부 목록</label>
            <input
              className="form-input"
              value={rejectStr}
              onChange={e => setRejectStr(e.target.value)}
              placeholder="+82100001, +82100002 (쉼표 구분)"
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

      {/* ── detail modal ── */}
      {(detail || detailLoading) && (
        <Modal
          title={detail ? `상세 — ${detail.id}` : '로딩 중…'}
          onClose={() => { setDetail(null); setDetailLoading(false) }}
        >
          {detailLoading ? (
            <div className="empty">로딩 중…</div>
          ) : detail ? (
            <>
              {/* tabs */}
              <div className="tab-bar">
                {(['base', 'call', 'ptt'] as DetailTab[]).map(tab => (
                  <button
                    key={tab}
                    className={`tab-btn${detailTab === tab ? ' tab-btn--active' : ''}`}
                    onClick={() => setDetailTab(tab)}
                  >
                    {tab === 'base' ? '기본정보' : tab === 'call' ? 'Call 번호' : 'PTT 번호'}
                  </button>
                ))}
              </div>

              {detailTab === 'base' && (
                <dl className="detail-list">
                  <dt>ID</dt>       <dd>{detail.id}</dd>
                  <dt>이름</dt>     <dd>{detail.name || '—'}</dd>
                  <dt>조직</dt>     <dd>{detail.org_id || '—'}</dd>
                  <dt>세부사항</dt> <dd>{detail.details || '—'}</dd>
                  <dt>착신거부</dt> <dd>{detail.reject_id.length ? detail.reject_id.join(', ') : '—'}</dd>
                  <dt>생성시간</dt> <dd>{detail.create_time ?? '—'}</dd>
                  <dt>수정시간</dt> <dd>{detail.update_time ?? '—'}</dd>
                </dl>
              )}

              {detailTab === 'call' && (
                <>
                  <div style={{ display: 'flex', justifyContent: 'flex-end', marginBottom: 8 }}>
                    <button className="btn btn--primary btn--sm" onClick={() => openAddSub('call')}>＋ 번호 추가</button>
                  </div>
                  {detail.call_subscriptions.length === 0 ? (
                    <div className="empty">Call 번호 없음</div>
                  ) : (
                    <div className="table-wrap">
                      <table className="data-table">
                        <thead>
                          <tr>
                            <th>MSISDN</th>
                            <th>Auth ID</th>
                            <th>DND</th>
                            <th>착신전환</th>
                            <th>등록시간</th>
                            <th style={{ width: 100 }}>작업</th>
                          </tr>
                        </thead>
                        <tbody>
                          {detail.call_subscriptions.map(s => (
                            <tr key={s.id}>
                              <td>{s.id}</td>
                              <td>{s.auth_id}</td>
                              <td>{s.dnd ? '켜짐' : '꺼짐'}</td>
                              <td>{s.forward_id || '—'}</td>
                              <td>{s.register_time ?? '—'}</td>
                              <td className="actions">
                                <button className="btn btn--sm btn--outline" onClick={() => openEditSub('call', s)}>편집</button>
                                <button className="btn btn--sm btn--danger" onClick={() => handleDeleteSub('call', s.id)}>삭제</button>
                              </td>
                            </tr>
                          ))}
                        </tbody>
                      </table>
                    </div>
                  )}
                </>
              )}

              {detailTab === 'ptt' && (
                <>
                  <div style={{ display: 'flex', justifyContent: 'flex-end', marginBottom: 8 }}>
                    <button className="btn btn--primary btn--sm" onClick={() => openAddSub('ptt')}>＋ 번호 추가</button>
                  </div>
                  {detail.ptt_subscriptions.length === 0 ? (
                    <div className="empty">PTT 번호 없음</div>
                  ) : (
                    <div className="table-wrap">
                      <table className="data-table">
                        <thead>
                          <tr>
                            <th>MSISDN</th>
                            <th>Auth ID</th>
                            <th>DND</th>
                            <th>착신전환</th>
                            <th>등록시간</th>
                            <th style={{ width: 100 }}>작업</th>
                          </tr>
                        </thead>
                        <tbody>
                          {detail.ptt_subscriptions.map(s => (
                            <tr key={s.id}>
                              <td>{s.id}</td>
                              <td>{s.auth_id}</td>
                              <td>{s.dnd ? '켜짐' : '꺼짐'}</td>
                              <td>{s.forward_id || '—'}</td>
                              <td>{s.register_time ?? '—'}</td>
                              <td className="actions">
                                <button className="btn btn--sm btn--outline" onClick={() => openEditSub('ptt', s)}>편집</button>
                                <button className="btn btn--sm btn--danger" onClick={() => handleDeleteSub('ptt', s.id)}>삭제</button>
                              </td>
                            </tr>
                          ))}
                        </tbody>
                      </table>
                    </div>
                  )}
                </>
              )}

              <div className="modal-footer">
                <button className="btn btn--outline" onClick={() => {
                  const summary = users.find(u => u.id === detail.id)
                  setDetail(null)
                  if (summary) openEdit(summary)
                }}>편집</button>
                <button className="btn btn--ghost" onClick={() => setDetail(null)}>닫기</button>
              </div>
            </>
          ) : null}
        </Modal>
      )}

      {/* ── subscription add modal ── */}
      {addSubOpen && detail && (
        <Modal
          title={`${addSubOpen.svc === 'call' ? 'Call' : 'PTT'} 번호 추가 — ${detail.id}`}
          onClose={() => setAddSubOpen(null)}
        >
          <div className="form-grid">
            <label>MSISDN *</label>
            <input
              className="form-input"
              value={subForm.id}
              onChange={e => setSubForm(f => ({ ...f, id: e.target.value }))}
              placeholder="+821001234567"
            />

            <label>Auth ID</label>
            <input
              className="form-input"
              value={subForm.auth_id}
              onChange={e => setSubForm(f => ({ ...f, auth_id: e.target.value }))}
              placeholder="단말 인증 ID (IMPI)"
            />

            <label>비밀번호</label>
            <input
              type="password"
              className="form-input"
              value={subForm.passwd}
              onChange={e => setSubForm(f => ({ ...f, passwd: e.target.value }))}
            />

            <label>착신전환</label>
            <input
              className="form-input"
              value={subForm.forward_id}
              onChange={e => setSubForm(f => ({ ...f, forward_id: e.target.value }))}
              placeholder="+821009999999"
            />

            <label>DND</label>
            <label className="toggle">
              <input
                type="checkbox"
                checked={subForm.dnd}
                onChange={e => setSubForm(f => ({ ...f, dnd: e.target.checked }))}
              />
              <span className="toggle-track" />
              <span className="toggle-label">{subForm.dnd ? '켜짐' : '꺼짐'}</span>
            </label>
          </div>

          <div className="modal-footer">
            <button className="btn btn--ghost" onClick={() => setAddSubOpen(null)}>취소</button>
            <button className="btn btn--primary" onClick={handleAddSub} disabled={subSaving}>
              {subSaving ? '저장 중…' : '추가'}
            </button>
          </div>
        </Modal>
      )}

      {/* ── subscription edit modal ── */}
      {editSub && detail && (
        <Modal
          title={`${editSub.svc === 'call' ? 'Call' : 'PTT'} 번호 편집 — ${editSub.sub.id}`}
          onClose={() => setEditSub(null)}
        >
          <div className="form-grid">
            <label>MSISDN</label>
            <input
              className="form-input"
              value={editSubForm.id}
              disabled
            />

            <label>Auth ID</label>
            <input
              className="form-input"
              value={editSubForm.auth_id}
              onChange={e => setEditSubForm(f => ({ ...f, auth_id: e.target.value }))}
              placeholder="단말 인증 ID (IMPI)"
            />

            <label>비밀번호</label>
            <input
              type="password"
              className="form-input"
              value={editSubForm.passwd}
              onChange={e => setEditSubForm(f => ({ ...f, passwd: e.target.value }))}
              placeholder="변경 시에만 입력"
            />

            <label>착신전환</label>
            <input
              className="form-input"
              value={editSubForm.forward_id}
              onChange={e => setEditSubForm(f => ({ ...f, forward_id: e.target.value }))}
              placeholder="+821009999999"
            />

            <label>DND</label>
            <label className="toggle">
              <input
                type="checkbox"
                checked={editSubForm.dnd}
                onChange={e => setEditSubForm(f => ({ ...f, dnd: e.target.checked }))}
              />
              <span className="toggle-track" />
              <span className="toggle-label">{editSubForm.dnd ? '켜짐' : '꺼짐'}</span>
            </label>
          </div>

          <div className="modal-footer">
            <button className="btn btn--ghost" onClick={() => setEditSub(null)}>취소</button>
            <button className="btn btn--primary" onClick={handleEditSub} disabled={editSubSaving}>
              {editSubSaving ? '저장 중…' : '저장'}
            </button>
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
