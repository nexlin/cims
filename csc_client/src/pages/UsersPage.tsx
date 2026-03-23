import { useState, useEffect, useCallback } from 'react'
import { usersApi, type UserSummary, type UserDetail, type UserInput, type CallAuth } from '../api/users'
import Modal from '../components/Modal'
import { useToast } from '../components/Toast'

// ── Auth section initial state ──────────────────────────────
const EMPTY_AUTH: Partial<CallAuth> = {
  auth_id: '', passwd: '', dnd: false, forward_id: '',
}

// ── Form state ──────────────────────────────────────────────
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

// ── Detail modal tab type ────────────────────────────────────
type DetailTab = 'base' | 'call' | 'ptt'

export default function UsersPage() {
  const { show } = useToast()
  const [users, setUsers] = useState<UserSummary[]>([])
  const [loading, setLoading] = useState(true)
  const [search, setSearch] = useState('')

  // form modal
  const [formOpen, setFormOpen] = useState(false)
  const [editingId, setEditingId] = useState<string | null>(null)
  const [form, setForm] = useState<FormState>(EMPTY_FORM)
  const [rejectStr, setRejectStr] = useState('')
  const [callEnabled, setCallEnabled] = useState(false)
  const [callAuth, setCallAuth] = useState<Partial<CallAuth>>(EMPTY_AUTH)
  const [pttEnabled, setPttEnabled] = useState(false)
  const [pttAuth, setPttAuth] = useState<Partial<CallAuth>>(EMPTY_AUTH)
  const [saving, setSaving] = useState(false)

  // detail modal
  const [detail, setDetail] = useState<UserDetail | null>(null)
  const [detailTab, setDetailTab] = useState<DetailTab>('base')
  const [detailLoading, setDetailLoading] = useState(false)

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

  // ── open add form ─────────────────────────────────────────
  function openAdd() {
    setEditingId(null)
    setForm(EMPTY_FORM)
    setRejectStr('')
    setCallEnabled(false)
    setCallAuth(EMPTY_AUTH)
    setPttEnabled(false)
    setPttAuth(EMPTY_AUTH)
    setFormOpen(true)
  }

  // ── open edit form (from detail) ──────────────────────────
  async function openEdit(u: UserSummary) {
    setEditingId(u.id)
    setForm({
      id: u.id,
      name: u.name,
      org_id: u.org_id,
      details: u.details ?? '',
      reject_id: u.reject_id,
    })
    setRejectStr(u.reject_id.join(', '))

    // fetch detail to populate auth fields
    try {
      const d = await usersApi.get(u.id)
      if (d.call_auth) {
        setCallEnabled(true)
        setCallAuth({ ...d.call_auth, passwd: '' })
      } else {
        setCallEnabled(false)
        setCallAuth(EMPTY_AUTH)
      }
      if (d.ptt_auth) {
        setPttEnabled(true)
        setPttAuth({ ...d.ptt_auth, passwd: '' })
      } else {
        setPttEnabled(false)
        setPttAuth(EMPTY_AUTH)
      }
    } catch {
      setCallEnabled(false)
      setCallAuth(EMPTY_AUTH)
      setPttEnabled(false)
      setPttAuth(EMPTY_AUTH)
    }

    setFormOpen(true)
  }

  // ── save ──────────────────────────────────────────────────
  async function handleSave() {
    const rejectIds = rejectStr.split(',').map(s => s.trim()).filter(Boolean)
    setSaving(true)
    try {
      if (editingId) {
        // update base fields
        await usersApi.update(editingId, {
          name: form.name,
          org_id: form.org_id,
          details: form.details || undefined,
          reject_id: rejectIds,
        })
        // handle call auth
        if (callEnabled) {
          await usersApi.upsertCall(editingId, callAuth)
        } else {
          try { await usersApi.deleteCall(editingId) } catch { /* ignore if not found */ }
        }
        // handle ptt auth
        if (pttEnabled) {
          await usersApi.upsertPtt(editingId, pttAuth)
        } else {
          try { await usersApi.deletePtt(editingId) } catch { /* ignore if not found */ }
        }
        show('가입자 정보가 수정되었습니다.')
      } else {
        const payload: UserInput = {
          id: form.id,
          name: form.name,
          org_id: form.org_id,
          details: form.details || undefined,
          reject_id: rejectIds,
          call_auth: callEnabled ? callAuth : null,
          ptt_auth: pttEnabled ? pttAuth : null,
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

  // ── open detail modal ─────────────────────────────────────
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

  // ── delete ────────────────────────────────────────────────
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
                <th>ID (MSISDN)</th>
                <th>이름</th>
                <th>조직</th>
                <th>서비스</th>
                <th style={{ width: 130 }}>작업</th>
              </tr>
            </thead>
            <tbody>
              {filtered.length === 0 ? (
                <tr><td colSpan={5} className="empty-cell">가입자 없음</td></tr>
              ) : filtered.map(u => (
                <tr key={u.id}>
                  <td>
                    <button className="link-btn" onClick={() => openDetail(u)}>{u.id}</button>
                  </td>
                  <td>{u.name || '—'}</td>
                  <td>{u.org_id || '—'}</td>
                  <td>
                    {u.has_call && (
                      <span className="badge badge--blue" style={{ marginRight: 4 }}>Call</span>
                    )}
                    {u.has_ptt && (
                      <span className="badge badge--green">PTT</span>
                    )}
                    {!u.has_call && !u.has_ptt && <span className="badge badge--gray">없음</span>}
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

      {/* ── add/edit modal ── */}
      {formOpen && (
        <Modal title={editingId ? `가입자 편집 — ${editingId}` : '가입자 추가'} onClose={() => setFormOpen(false)}>

          {/* 기본정보 section */}
          <div className="form-section-title">기본정보</div>
          <div className="form-grid">
            <label>ID (MSISDN) *</label>
            <input
              className="form-input"
              value={form.id}
              disabled={!!editingId}
              onChange={e => setForm(f => ({ ...f, id: e.target.value }))}
              placeholder="+821001234567"
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

          {/* Call 인증 section */}
          <div className="form-section-title" style={{ marginTop: 16 }}>
            <label className="toggle" style={{ display: 'inline-flex', alignItems: 'center', gap: 8 }}>
              <input
                type="checkbox"
                checked={callEnabled}
                onChange={e => setCallEnabled(e.target.checked)}
              />
              <span className="toggle-track" />
              <span>Call 인증 (VoLTE)</span>
            </label>
          </div>
          {callEnabled && (
            <div className="form-grid">
              <label>Auth ID</label>
              <input
                className="form-input"
                value={callAuth.auth_id ?? ''}
                onChange={e => setCallAuth(a => ({ ...a, auth_id: e.target.value }))}
                placeholder="단말 인증 ID (IMPI)"
              />

              <label>비밀번호</label>
              <input
                type="password"
                className="form-input"
                value={callAuth.passwd ?? ''}
                onChange={e => setCallAuth(a => ({ ...a, passwd: e.target.value }))}
                placeholder={editingId ? '변경 시에만 입력' : ''}
              />

              <label>착신전환</label>
              <input
                className="form-input"
                value={callAuth.forward_id ?? ''}
                onChange={e => setCallAuth(a => ({ ...a, forward_id: e.target.value }))}
                placeholder="+821009999999"
              />

              <label>DND</label>
              <label className="toggle">
                <input
                  type="checkbox"
                  checked={callAuth.dnd ?? false}
                  onChange={e => setCallAuth(a => ({ ...a, dnd: e.target.checked }))}
                />
                <span className="toggle-track" />
                <span className="toggle-label">{callAuth.dnd ? '켜짐' : '꺼짐'}</span>
              </label>
            </div>
          )}

          {/* PTT 인증 section */}
          <div className="form-section-title" style={{ marginTop: 16 }}>
            <label className="toggle" style={{ display: 'inline-flex', alignItems: 'center', gap: 8 }}>
              <input
                type="checkbox"
                checked={pttEnabled}
                onChange={e => setPttEnabled(e.target.checked)}
              />
              <span className="toggle-track" />
              <span>PTT 인증</span>
            </label>
          </div>
          {pttEnabled && (
            <div className="form-grid">
              <label>Auth ID</label>
              <input
                className="form-input"
                value={pttAuth.auth_id ?? ''}
                onChange={e => setPttAuth(a => ({ ...a, auth_id: e.target.value }))}
                placeholder="단말 인증 ID (IMPI)"
              />

              <label>비밀번호</label>
              <input
                type="password"
                className="form-input"
                value={pttAuth.passwd ?? ''}
                onChange={e => setPttAuth(a => ({ ...a, passwd: e.target.value }))}
                placeholder={editingId ? '변경 시에만 입력' : ''}
              />

              <label>착신전환</label>
              <input
                className="form-input"
                value={pttAuth.forward_id ?? ''}
                onChange={e => setPttAuth(a => ({ ...a, forward_id: e.target.value }))}
                placeholder="+821009999999"
              />

              <label>DND</label>
              <label className="toggle">
                <input
                  type="checkbox"
                  checked={pttAuth.dnd ?? false}
                  onChange={e => setPttAuth(a => ({ ...a, dnd: e.target.checked }))}
                />
                <span className="toggle-track" />
                <span className="toggle-label">{pttAuth.dnd ? '켜짐' : '꺼짐'}</span>
              </label>
            </div>
          )}

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
                    {tab === 'base' ? '기본정보' : tab === 'call' ? 'Call 인증' : 'PTT 인증'}
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
                detail.call_auth ? (
                  <dl className="detail-list">
                    <dt>Auth ID</dt>   <dd>{detail.call_auth.auth_id}</dd>
                    <dt>DND</dt>       <dd>{detail.call_auth.dnd ? '켜짐' : '꺼짐'}</dd>
                    <dt>착신전환</dt>  <dd>{detail.call_auth.forward_id || '—'}</dd>
                    <dt>등록시간</dt>  <dd>{detail.call_auth.register_time ?? '—'}</dd>
                    <dt>로그아웃</dt>  <dd>{detail.call_auth.logout_time ?? '—'}</dd>
                  </dl>
                ) : (
                  <div className="empty">Call 인증 정보 없음</div>
                )
              )}

              {detailTab === 'ptt' && (
                detail.ptt_auth ? (
                  <dl className="detail-list">
                    <dt>Auth ID</dt>   <dd>{detail.ptt_auth.auth_id}</dd>
                    <dt>DND</dt>       <dd>{detail.ptt_auth.dnd ? '켜짐' : '꺼짐'}</dd>
                    <dt>착신전환</dt>  <dd>{detail.ptt_auth.forward_id || '—'}</dd>
                    <dt>등록시간</dt>  <dd>{detail.ptt_auth.register_time ?? '—'}</dd>
                    <dt>로그아웃</dt>  <dd>{detail.ptt_auth.logout_time ?? '—'}</dd>
                  </dl>
                ) : (
                  <div className="empty">PTT 인증 정보 없음</div>
                )
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
