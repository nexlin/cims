import { useState, useEffect, useCallback } from 'react'
import { usersApi, type UserSummary, type UserInput } from '../../../api/users'
import { orgApi, type Organization } from '../../../api/organizations'
import OrgTreePanel from '../../../components/OrgTreePanel'
import { useToast } from '../../../components/Toast'
import { useAuth } from '../../../contexts/AuthContext'
import { canWriteConfig, canAssignRole, ROLE_LABELS, ASSIGNABLE_ROLES } from '../../../utils/permissions'

/** CSV 셀 escape — 쉼표/줄바꿈/큰따옴표 포함 시 큰따옴표로 감싸고 내부 따옴표는 이중화. */
function csvCell(v: string | number | null | undefined): string {
  const s = v == null ? '' : String(v)
  if (/[",\n\r]/.test(s)) return `"${s.replace(/"/g, '""')}"`
  return s
}

function downloadCsv(filename: string, rows: string[][]): void {
  const csv = rows.map(r => r.map(csvCell).join(',')).join('\r\n')
  // UTF-8 BOM — Excel 한글 인코딩 자동 인식
  const blob = new Blob(['﻿' + csv], { type: 'text/csv;charset=utf-8' })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = filename
  document.body.appendChild(a)
  a.click()
  document.body.removeChild(a)
  URL.revokeObjectURL(url)
}

export default function MembersPage() {
  const { show } = useToast()
  const { user: me } = useAuth()
  const canWrite = canWriteConfig(me)   // manager+ : 생성/수정/삭제
  const canRole = canAssignRole(me)     // admin : 역할(권한) 지정
  const [users, setUsers] = useState<UserSummary[]>([])
  const [loading, setLoading] = useState(true)
  const [orgPathFilter, setOrgPathFilter] = useState<string | null>(null)
  const [orgName, setOrgName] = useState('전체')
  const [search, setSearch] = useState('')

  const [editId, setEditId] = useState<number | null>(null)
  const [editForm, setEditForm] = useState<UserInput>({ name: '', login_id: '', org_id: '', details: '' })
  const [adding, setAdding] = useState(false)
  const [addForm, setAddForm] = useState<UserInput>({ name: '', login_id: '', org_id: '', details: '' })
  const [selected, setSelected] = useState<Set<number>>(new Set())
  const [orgList, setOrgList] = useState<Organization[]>([])

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [u, o] = await Promise.all([usersApi.list(), orgApi.list()])
      setUsers(u)
      setOrgList(o)
    }
    catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [show])

  useEffect(() => { load() }, [load])

  // code_path 기반 필터: 선택된 조직의 code_path로 startsWith 비교
  function matchOrg(userOrgId: string): boolean {
    if (!orgPathFilter) return true
    // user의 org_id → orgList에서 code_path 찾기
    const userOrg = orgList.find(o => o.code === userOrgId)
    const userPath = userOrg?.code_path || userOrgId || ''
    return userPath.startsWith(orgPathFilter)
  }

  const filtered = users.filter(u => {
    if (!matchOrg(u.org_id || '')) return false
    if (search) {
      const s = search.toLowerCase()
      return u.name.toLowerCase().includes(s) || (u.login_id || '').toLowerCase().includes(s)
    }
    return true
  })

  function startEdit(u: UserSummary) {
    setEditId(u.id)
    setEditForm({ name: u.name, login_id: u.login_id, org_id: u.org_id, details: u.details || '', role: u.role || 'user' })
    setAdding(false)
  }
  async function saveEdit() {
    if (!editId) return
    try { await usersApi.update(editId, editForm); show('수정 완료', 'ok'); setEditId(null); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  function startAdd() {
    setAdding(true); setEditId(null)
    // 현재 필터의 code_path에서 마지막 code를 org_id로 사용
    const filterCode = orgPathFilter ? orgPathFilter.split('/').pop() || '' : ''
    setAddForm({ name: '', login_id: '', org_id: filterCode, details: '', role: 'user' })
  }
  async function saveAdd() {
    if (!addForm.name) { show('이름은 필수입니다', 'err'); return }
    try { await usersApi.create(addForm); show('생성 완료', 'ok'); setAdding(false); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  async function handleDelete(id: number) {
    if (!confirm('구성원을 삭제합니다. 연결된 구독도 삭제됩니다.')) return
    try { await usersApi.delete(id); show('삭제', 'ok'); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }
  async function handleBatchDelete() {
    if (!confirm(`${selected.size}명을 삭제합니다.`)) return
    try { await usersApi.batchDelete(Array.from(selected)); show('삭제 완료', 'ok'); setSelected(new Set()); load() }
    catch (e: unknown) { show(String(e), 'err') }
  }

  function toggleSelect(id: number) {
    setSelected(prev => { const n = new Set(prev); if (n.has(id)) n.delete(id); else n.add(id); return n })
  }

  return (
    <div style={{ display: 'flex', gap: 16, alignItems: 'flex-start' }}>
      <OrgTreePanel selectedPath={orgPathFilter} onSelect={(path, name) => { setOrgPathFilter(path); setOrgName(name) }} />

      <div style={{ flex: 1 }}>
        <div className="toolbar">
          <span style={{ fontWeight: 600, fontSize: 14 }}>{orgName}</span>
          <input className="search-input" placeholder="이름/ID 검색" value={search}
            onChange={e => setSearch(e.target.value)} style={{ maxWidth: 180 }} />
          <button className="btn btn--outline btn--sm" onClick={() => {
            const rows: string[][] = [
              ['이름', '로그인 ID', '조직 코드', '조직명', '상세', 'Call 번호', 'PTT 번호'],
              ...filtered.map(u => [
                u.name,
                u.login_id || '',
                u.org_id || '',
                orgList.find(o => o.code === u.org_id)?.name || '',
                u.details || '',
                u.call_subscriptions.map(s => s.id).join('; '),
                u.ptt_subscriptions.map(s => s.id).join('; '),
              ]),
            ]
            const ts = new Date().toISOString().slice(0, 19).replace(/[T:]/g, '').slice(0, 13)
            downloadCsv(`cims_members_${ts}.csv`, rows)
          }}>CSV 내보내기 ({filtered.length})</button>
          {canWrite && selected.size > 0 && (
            <button className="btn btn--danger btn--sm" onClick={handleBatchDelete}>선택 삭제 ({selected.size})</button>
          )}
        </div>

        {loading ? <div className="empty">로딩 중...</div> : (
          <div className="table-wrap">
            <table className="data-table">
              <thead>
                <tr>
                  <th style={{ width: 36 }}>
                    <input type="checkbox"
                      checked={filtered.length > 0 && filtered.every(u => selected.has(u.id))}
                      onChange={() => filtered.every(u => selected.has(u.id)) ? setSelected(new Set()) : setSelected(new Set(filtered.map(u => u.id)))} />
                  </th>
                  <th>이름</th>
                  <th>로그인 ID</th>
                  <th style={{ width: 100 }}>권한</th>
                  <th>조직</th>
                  <th>상세</th>
                  <th style={{ width: 120 }}>작업</th>
                </tr>
              </thead>
              <tbody>
                {filtered.map(u => {
                  const isEditing = editId === u.id
                  return (
                    <tr key={u.id} style={selected.has(u.id) ? { background: 'rgba(74,144,217,0.08)' } : undefined}>
                      <td><input type="checkbox" checked={selected.has(u.id)} onChange={() => toggleSelect(u.id)} /></td>
                      <td>{isEditing ?
                        <input className="form-input" value={editForm.name} onChange={e => setEditForm({...editForm, name: e.target.value})} style={{ width: '100%' }} autoFocus /> :
                        <span style={{ fontWeight: 500 }}>{u.name}</span>}
                      </td>
                      <td>{isEditing ?
                        <input className="form-input" value={editForm.login_id || ''} onChange={e => setEditForm({...editForm, login_id: e.target.value})} style={{ width: '100%' }} /> :
                        <span className="ts">{u.login_id || '—'}</span>}
                      </td>
                      <td>{isEditing && canRole ?
                        <select className="form-input" value={editForm.role || 'user'} onChange={e => setEditForm({...editForm, role: e.target.value as UserInput['role']})} style={{ width: '100%' }}>
                          {ASSIGNABLE_ROLES.map(r => <option key={r} value={r}>{ROLE_LABELS[r]}</option>)}
                        </select> :
                        <span className="badge">{ROLE_LABELS[u.role || 'user']}</span>}
                      </td>
                      <td>{isEditing ?
                        <select className="form-input" value={editForm.org_id} onChange={e => setEditForm({...editForm, org_id: e.target.value})} style={{ width: '100%' }}>
                          <option value="">없음</option>
                          {orgList.map(o => <option key={o.id} value={o.code}>{o.name} ({o.code})</option>)}
                        </select> :
                        <span className="ts">{orgList.find(o => o.code === u.org_id)?.name || u.org_id || '—'}</span>}
                      </td>
                      <td>{isEditing ?
                        <input className="form-input" value={editForm.details || ''} onChange={e => setEditForm({...editForm, details: e.target.value})} style={{ width: '100%' }} /> :
                        <span className="ts">{u.details || '—'}</span>}
                      </td>
                      <td className="actions">
                        {isEditing ? (
                          <><button className="btn btn--sm btn--primary" onClick={saveEdit}>저장</button>
                          <button className="btn btn--sm btn--ghost" onClick={() => setEditId(null)}>취소</button></>
                        ) : canWrite ? (
                          <><button className="btn btn--sm btn--outline" onClick={() => startEdit(u)}>편집</button>
                          <button className="btn btn--sm btn--danger" onClick={() => handleDelete(u.id)}>삭제</button></>
                        ) : <span className="ts" style={{ color: 'var(--text-muted)' }}>—</span>}
                      </td>
                    </tr>
                  )
                })}
                {/* 추가 행 */}
                {adding ? (
                  <tr style={{ background: 'rgba(74,144,217,0.08)' }}>
                    <td></td>
                    <td><input className="form-input" placeholder="이름 *" value={addForm.name} onChange={e => setAddForm({...addForm, name: e.target.value})} style={{ width: '100%' }} autoFocus /></td>
                    <td><input className="form-input" placeholder="로그인 ID" value={addForm.login_id || ''} onChange={e => setAddForm({...addForm, login_id: e.target.value})} style={{ width: '100%' }} /></td>
                    <td>{canRole ?
                      <select className="form-input" value={addForm.role || 'user'} onChange={e => setAddForm({...addForm, role: e.target.value as UserInput['role']})} style={{ width: '100%' }}>
                        {ASSIGNABLE_ROLES.map(r => <option key={r} value={r}>{ROLE_LABELS[r]}</option>)}
                      </select> :
                      <span className="badge">{ROLE_LABELS['user']}</span>}
                    </td>
                    <td><select className="form-input" value={addForm.org_id} onChange={e => setAddForm({...addForm, org_id: e.target.value})} style={{ width: '100%' }}>
                      <option value="">없음</option>
                      {orgList.map(o => <option key={o.id} value={o.code}>{o.name}</option>)}
                    </select></td>
                    <td><input className="form-input" placeholder="상세" value={addForm.details || ''} onChange={e => setAddForm({...addForm, details: e.target.value})} style={{ width: '100%' }} /></td>
                    <td className="actions">
                      <button className="btn btn--sm btn--primary" onClick={saveAdd}>저장</button>
                      <button className="btn btn--sm btn--ghost" onClick={() => setAdding(false)}>취소</button>
                    </td>
                  </tr>
                ) : canWrite ? (
                  <tr><td colSpan={7} style={{ textAlign: 'center' }}>
                    <button className="btn btn--ghost btn--sm" onClick={startAdd} style={{ color: 'var(--primary)', fontSize: 12 }}>＋ 구성원 추가</button>
                  </td></tr>
                ) : null}
              </tbody>
            </table>
          </div>
        )}
      </div>
    </div>
  )
}
