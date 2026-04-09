import { useState, useEffect, useCallback } from 'react'
import { usersApi, type UserSummary, type UserInput, type Subscription } from '../api/users'
import Modal from '../components/Modal'
import { useToast } from '../components/Toast'

/** +82 제거 후 XXX-XXXX-XXXX 형태로 포맷 (UI 표시용) */
function fmtPhone(raw: string): string {
  let num = raw.replace(/^tel:/, '').replace(/^sip:/, '').split('@')[0]
  if (num.startsWith('+82')) num = '0' + num.slice(3)
  while (num.length < 11) num = '0' + num
  if (num.length === 11) return `${num.slice(0,3)}-${num.slice(3,7)}-${num.slice(7)}`
  return num
}

// ── Subscription form ─────────────────────────────────────────
interface SubForm {
  id: string
  auth_id: string
  passwd: string
  dnd: boolean
  forward_id: string
}
const EMPTY_SUB: SubForm = { id: '', auth_id: '', passwd: '', dnd: false, forward_id: '' }

// ── Person form ───────────────────────────────────────────────
interface PersonForm {
  name: string
  login_id: string
  org_id: string
  details: string
  reject_id: string[]
}
const EMPTY_PERSON: PersonForm = { name: '', login_id: '', org_id: '', details: '', reject_id: [] }

export default function UsersPage() {
  const { show } = useToast()
  const [users, setUsers] = useState<UserSummary[]>([])
  const [loading, setLoading] = useState(true)
  const [search, setSearch] = useState('')

  // person add/edit modal
  const [personOpen, setPersonOpen] = useState(false)
  const [editingId, setEditingId] = useState<number | null>(null)
  const [personForm, setPersonForm] = useState<PersonForm>(EMPTY_PERSON)
  const [rejectStr, setRejectStr] = useState('')
  const [personSaving, setPersonSaving] = useState(false)

  // delete confirm
  const [delTarget, setDelTarget] = useState<UserSummary | null>(null)

  // subscription add modal: { userId, svc }
  const [addSub, setAddSub] = useState<{ userId: number; svc: 'call' | 'ptt' } | null>(null)
  const [subForm, setSubForm] = useState<SubForm>(EMPTY_SUB)
  const [subSaving, setSubSaving] = useState(false)

  // subscription edit modal
  const [editSub, setEditSub] = useState<{ userId: number; svc: 'call' | 'ptt'; sub: Subscription } | null>(null)
  const [editSubForm, setEditSubForm] = useState<SubForm>(EMPTY_SUB)
  const [editSubSaving, setEditSubSaving] = useState(false)

  // Excel import
  const [importOpen, setImportOpen] = useState(false)
  const [importResult, setImportResult] = useState<{total:number, created_users:number, created_voip:number, created_ptt:number, errors:Array<{row:number,sheet:string,error:string}>} | null>(null)
  const [importLoading, setImportLoading] = useState(false)

  async function handleImportFile(e: React.ChangeEvent<HTMLInputElement>) {
    const file = e.target.files?.[0]
    if (!file) return
    setImportLoading(true)
    setImportResult(null)
    try {
      const buf = await file.arrayBuffer()
      const base64 = btoa(String.fromCharCode(...new Uint8Array(buf)))
      const result = await usersApi.importExcel(base64)
      setImportResult(result)
      if (result.total > 0) load()
    } catch (err: unknown) {
      show(String(err), 'err')
    } finally {
      setImportLoading(false)
      e.target.value = ''  // reset file input
    }
  }

  // 다중 선택 삭제
  const [selected, setSelected] = useState<Set<number>>(new Set())

  function toggleSelect(id: number) {
    setSelected(prev => {
      const next = new Set(prev)
      if (next.has(id)) next.delete(id); else next.add(id)
      return next
    })
  }
  function toggleSelectAll(filtered: UserSummary[]) {
    if (selected.size === filtered.length && filtered.every(u => selected.has(u.id))) {
      setSelected(new Set())
    } else {
      setSelected(new Set(filtered.map(u => u.id)))
    }
  }
  async function handleBatchDelete() {
    if (selected.size === 0) return
    if (!confirm(`${selected.size}명의 가입자와 관련 구독을 삭제합니다.\n되돌릴 수 없습니다.`)) return
    try {
      const result = await usersApi.batchDelete(Array.from(selected))
      show(`${result.deleted}건 삭제 완료${result.errors.length ? `, ${result.errors.length}건 실패` : ''}`, result.errors.length ? 'err' : 'ok')
      setSelected(new Set())
      load()
    } catch (e: unknown) { show(String(e), 'err') }
  }

  // ── load ────────────────────────────────────────────────────
  const load = useCallback(async () => {
    setLoading(true)
    try { setUsers(await usersApi.list()) }
    catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [show])

  useEffect(() => { load() }, [load])

  // ── person CRUD ─────────────────────────────────────────────
  function openAdd() {
    setEditingId(null)
    setPersonForm(EMPTY_PERSON)
    setRejectStr('')
    setPersonOpen(true)
  }

  function openEdit(u: UserSummary) {
    setEditingId(u.id)
    setPersonForm({ name: u.name, login_id: u.login_id, org_id: u.org_id, details: u.details ?? '', reject_id: u.reject_id })
    setRejectStr(u.reject_id.join(', '))
    setPersonOpen(true)
  }

  async function handlePersonSave() {
    const rejectIds = rejectStr.split(',').map(s => s.trim()).filter(Boolean)
    setPersonSaving(true)
    try {
      const payload: UserInput = {
        name: personForm.name,
        login_id: personForm.login_id,
        org_id: personForm.org_id,
        details: personForm.details || undefined,
        reject_id: rejectIds,
      }
      if (editingId != null) {
        await usersApi.update(editingId, payload)
        show('가입자 정보가 수정되었습니다.')
      } else {
        await usersApi.create(payload)
        show('가입자가 등록되었습니다.')
      }
      setPersonOpen(false)
      await load()
    } catch (e: unknown) { show(String(e), 'err') }
    finally { setPersonSaving(false) }
  }

  async function handleDelete() {
    if (!delTarget) return
    try {
      await usersApi.delete(delTarget.id)
      show('가입자가 삭제되었습니다.')
      setDelTarget(null)
      await load()
    } catch (e: unknown) { show(String(e), 'err') }
  }

  // ── subscription CRUD ───────────────────────────────────────
  function openAddSub(userId: number, svc: 'call' | 'ptt') {
    setSubForm(EMPTY_SUB)
    setAddSub({ userId, svc })
  }

  async function handleAddSub() {
    if (!addSub) return
    setSubSaving(true)
    try {
      await usersApi.addSub(addSub.userId, addSub.svc, {
        id: subForm.id, auth_id: subForm.auth_id || subForm.id,
        passwd: subForm.passwd, dnd: subForm.dnd, forward_id: subForm.forward_id,
      })
      show('번호가 추가되었습니다.')
      setAddSub(null)
      await load()
    } catch (e: unknown) { show(String(e), 'err') }
    finally { setSubSaving(false) }
  }

  function openEditSub(userId: number, svc: 'call' | 'ptt', sub: Subscription) {
    setEditSub({ userId, svc, sub })
    setEditSubForm({ id: sub.id, auth_id: sub.auth_id, passwd: '', dnd: sub.dnd, forward_id: sub.forward_id })
  }

  async function handleEditSub() {
    if (!editSub) return
    setEditSubSaving(true)
    try {
      await usersApi.updateSub(editSub.userId, editSub.svc, editSub.sub.id, {
        auth_id: editSubForm.auth_id, passwd: editSubForm.passwd,
        dnd: editSubForm.dnd, forward_id: editSubForm.forward_id,
      })
      show('번호 정보가 수정되었습니다.')
      setEditSub(null)
      await load()
    } catch (e: unknown) { show(String(e), 'err') }
    finally { setEditSubSaving(false) }
  }

  async function handleDeleteSub(userId: number, svc: 'call' | 'ptt', msisdn: string) {
    try {
      await usersApi.deleteSub(userId, svc, msisdn)
      show('번호가 삭제되었습니다.')
      await load()
    } catch (e: unknown) { show(String(e), 'err') }
  }

  // ── filter ──────────────────────────────────────────────────
  const filtered = users.filter(u =>
    u.name.toLowerCase().includes(search.toLowerCase()) ||
    u.login_id.toLowerCase().includes(search.toLowerCase()) ||
    u.org_id.toLowerCase().includes(search.toLowerCase()) ||
    u.call_subscriptions.some(s => s.id.includes(search)) ||
    u.ptt_subscriptions.some(s => s.id.includes(search))
  )

  return (
    <div className="page">
      {/* toolbar */}
      <div className="toolbar">
        <input
          className="search-input"
          placeholder="이름 / 아이디 / 조직 / 번호 검색…"
          value={search}
          onChange={e => setSearch(e.target.value)}
        />
        <button className="btn btn--primary" onClick={openAdd}>＋ 추가</button>
        <button className="btn btn--outline" onClick={() => setImportOpen(true)}>Excel 가져오기</button>
        {selected.size > 0 && (
          <button className="btn btn--danger" onClick={handleBatchDelete}>
            선택 삭제 ({selected.size}건)
          </button>
        )}
        <button className="btn btn--ghost btn--sm" onClick={load}>↻</button>
      </div>

      {/* table */}
      {loading ? (
        <div className="empty">로딩 중…</div>
      ) : (
        <div className="table-wrap">
          <table className="data-table">
            <thead>
              <tr>
                <th style={{ width: 36 }}>
                  <input type="checkbox"
                    checked={filtered.length > 0 && filtered.every(u => selected.has(u.id))}
                    onChange={() => toggleSelectAll(filtered)} />
                </th>
                <th>이름</th>
                <th>조직</th>
                <th>Call 번호</th>
                <th>PTT 번호</th>
                <th style={{ width: 100 }}>작업</th>
              </tr>
            </thead>
            <tbody>
              {filtered.length === 0 ? (
                <tr><td colSpan={6} className="empty-cell">가입자 없음</td></tr>
              ) : filtered.map(u => (
                <tr key={u.id} style={selected.has(u.id) ? { background: 'rgba(74,144,217,0.1)' } : undefined}>
                  <td onClick={e => e.stopPropagation()}>
                    <input type="checkbox" checked={selected.has(u.id)} onChange={() => toggleSelect(u.id)} />
                  </td>
                  <td>
                    <div style={{ fontWeight: 500 }}>{u.name}</div>
                    {u.login_id && <div style={{ fontSize: 11, color: 'var(--text-muted)' }}>{u.login_id}</div>}
                  </td>
                  <td>{u.org_id || '—'}</td>

                  {/* Call 번호 인라인 관리 */}
                  <td>
                    <div className="sub-chips">
                      {u.call_subscriptions.map(s => (
                        <span key={s.id} className="sub-chip sub-chip--call">
                          <button className="sub-chip-label" onClick={() => openEditSub(u.id, 'call', s)} title="편집">
                            {fmtPhone(s.id)}
                          </button>
                          <button className="sub-chip-del" onClick={() => handleDeleteSub(u.id, 'call', s.id)} title="삭제">×</button>
                        </span>
                      ))}
                      <button className="sub-chip-add" onClick={() => openAddSub(u.id, 'call')} title="Call 번호 추가">＋</button>
                    </div>
                  </td>

                  {/* PTT 번호 인라인 관리 */}
                  <td>
                    <div className="sub-chips">
                      {u.ptt_subscriptions.map(s => (
                        <span key={s.id} className="sub-chip sub-chip--ptt">
                          <button className="sub-chip-label" onClick={() => openEditSub(u.id, 'ptt', s)} title="편집">
                            {fmtPhone(s.id)}
                          </button>
                          <button className="sub-chip-del" onClick={() => handleDeleteSub(u.id, 'ptt', s.id)} title="삭제">×</button>
                        </span>
                      ))}
                      <button className="sub-chip-add" onClick={() => openAddSub(u.id, 'ptt')} title="PTT 번호 추가">＋</button>
                    </div>
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
      {personOpen && (
        <Modal title={editingId != null ? '가입자 편집' : '가입자 추가'} onClose={() => setPersonOpen(false)}>
          <div className="form-grid">
            <label>이름 *</label>
            <input
              className="form-input"
              value={personForm.name}
              onChange={e => setPersonForm(f => ({ ...f, name: e.target.value }))}
              placeholder="표시 이름"
            />

            <label>아이디</label>
            <input
              className="form-input"
              value={personForm.login_id}
              onChange={e => setPersonForm(f => ({ ...f, login_id: e.target.value }))}
              placeholder="test001"
            />

            <label>조직 ID</label>
            <input
              className="form-input"
              value={personForm.org_id}
              onChange={e => setPersonForm(f => ({ ...f, org_id: e.target.value }))}
            />

            <label>세부사항</label>
            <input
              className="form-input"
              value={personForm.details}
              onChange={e => setPersonForm(f => ({ ...f, details: e.target.value }))}
            />

            <label>착신거부</label>
            <input
              className="form-input"
              value={rejectStr}
              onChange={e => setRejectStr(e.target.value)}
              placeholder="+82100001, +82100002 (쉼표 구분)"
            />
          </div>

          <div className="modal-footer">
            <button className="btn btn--ghost" onClick={() => setPersonOpen(false)}>취소</button>
            <button className="btn btn--primary" onClick={handlePersonSave} disabled={personSaving}>
              {personSaving ? '저장 중…' : '저장'}
            </button>
          </div>
        </Modal>
      )}

      {/* ── subscription add modal ── */}
      {addSub && (
        <Modal
          title={`${addSub.svc === 'call' ? 'Call' : 'PTT'} 번호 추가`}
          onClose={() => setAddSub(null)}
        >
          <div className="form-grid">
            <label>MSISDN *</label>
            <input
              className="form-input"
              value={subForm.id}
              onChange={e => setSubForm(f => ({ ...f, id: e.target.value }))}
              placeholder="+821001234567"
              autoFocus
            />

            <label>Auth ID</label>
            <input
              className="form-input"
              value={subForm.auth_id}
              onChange={e => setSubForm(f => ({ ...f, auth_id: e.target.value }))}
              placeholder="비우면 MSISDN 사용"
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
              <input type="checkbox" checked={subForm.dnd}
                onChange={e => setSubForm(f => ({ ...f, dnd: e.target.checked }))} />
              <span className="toggle-track" />
              <span className="toggle-label">{subForm.dnd ? '켜짐' : '꺼짐'}</span>
            </label>
          </div>

          <div className="modal-footer">
            <button className="btn btn--ghost" onClick={() => setAddSub(null)}>취소</button>
            <button className="btn btn--primary" onClick={handleAddSub} disabled={subSaving}>
              {subSaving ? '저장 중…' : '추가'}
            </button>
          </div>
        </Modal>
      )}

      {/* ── subscription edit modal ── */}
      {editSub && (
        <Modal
          title={`${editSub.svc === 'call' ? 'Call' : 'PTT'} 번호 편집 — ${editSub.sub.id}`}
          onClose={() => setEditSub(null)}
        >
          <div className="form-grid">
            <label>MSISDN</label>
            <input className="form-input" value={editSubForm.id} disabled />

            <label>Auth ID</label>
            <input
              className="form-input"
              value={editSubForm.auth_id}
              onChange={e => setEditSubForm(f => ({ ...f, auth_id: e.target.value }))}
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
              <input type="checkbox" checked={editSubForm.dnd}
                onChange={e => setEditSubForm(f => ({ ...f, dnd: e.target.checked }))} />
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
            <strong>{delTarget.name}</strong> 가입자를 삭제하시겠습니까?<br />
            연결된 Call/PTT 번호도 함께 삭제됩니다.
          </p>
          <div className="modal-footer">
            <button className="btn btn--ghost" onClick={() => setDelTarget(null)}>취소</button>
            <button className="btn btn--danger" onClick={handleDelete}>삭제</button>
          </div>
        </Modal>
      )}

      {/* ── Excel Import 모달 ── */}
      {importOpen && (
        <Modal title="Excel 가져오기" onClose={() => { setImportOpen(false); setImportResult(null) }}>
          <div className="modal-body">
            <p style={{ marginBottom: 12 }}>
              가입자, VoIP 구독, PTT 구독을 Excel 파일(.xlsx)로 일괄 등록합니다.
            </p>
            <div style={{ display: 'flex', gap: 12, alignItems: 'center', marginBottom: 16 }}>
              <label className="btn btn--primary" style={{ cursor: 'pointer' }}>
                파일 선택
                <input type="file" accept=".xlsx" onChange={handleImportFile} style={{ display: 'none' }} />
              </label>
              <a href={usersApi.templateUrl} className="btn btn--outline" download>
                템플릿 다운로드
              </a>
              {importLoading && <span className="ts">처리 중...</span>}
            </div>

            {importResult && (
              <div style={{ background: 'var(--surface)', borderRadius: 8, padding: 16 }}>
                <div style={{ fontWeight: 600, marginBottom: 8 }}>등록 결과</div>
                <div style={{ display: 'grid', gridTemplateColumns: 'auto 1fr', gap: '4px 16px', fontSize: 14 }}>
                  <span>가입자 생성:</span><span><strong>{importResult.created_users}</strong>건</span>
                  <span>VoIP 구독 생성:</span><span><strong>{importResult.created_voip}</strong>건</span>
                  <span>PTT 구독 생성:</span><span><strong>{importResult.created_ptt}</strong>건</span>
                </div>
                {importResult.errors.length > 0 && (
                  <div style={{ marginTop: 12 }}>
                    <div style={{ color: 'var(--danger)', fontWeight: 600, marginBottom: 4 }}>
                      오류 {importResult.errors.length}건
                    </div>
                    <div style={{ maxHeight: 200, overflow: 'auto', fontSize: 12 }}>
                      {importResult.errors.map((e, i) => (
                        <div key={i} style={{ color: 'var(--danger)', padding: '2px 0' }}>
                          [{e.sheet}] 행 {e.row}: {e.error}
                        </div>
                      ))}
                    </div>
                  </div>
                )}
              </div>
            )}
          </div>
          <div className="modal-footer">
            <button className="btn btn--ghost" onClick={() => { setImportOpen(false); setImportResult(null) }}>닫기</button>
          </div>
        </Modal>
      )}
    </div>
  )
}
