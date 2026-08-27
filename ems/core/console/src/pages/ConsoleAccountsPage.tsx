// 콘솔 계정 관리 — OAM 로그인 계정(file_store 도메인 console_accounts) CRUD.
// DB users(가입자 person)와 분리. 내장 admin(oam.json)은 여기 표시되지 않음(부트스트랩 전용).
import { useCallback, useEffect, useState } from 'react'
import { Pencil, Trash2, KeyRound, Plus } from 'lucide-react'
import { useToast } from '../components/Toast'
import { ROLE_LABELS } from '../utils/permissions'
import {
  consoleAccountsApi, CONSOLE_ROLES,
  type ConsoleAccount, type ConsoleRole,
} from '../api/consoleAccounts'

type Form = { login_id: string; name: string; role: ConsoleRole; email: string; password: string }
const EMPTY: Form = { login_id: '', name: '', role: 'operator', email: '', password: '' }

export default function ConsoleAccountsPage() {
  const { show } = useToast()
  const [rows, setRows] = useState<ConsoleAccount[]>([])
  const [loading, setLoading] = useState(true)
  const [adding, setAdding] = useState(false)
  const [editId, setEditId] = useState<string | null>(null)
  const [form, setForm] = useState<Form>(EMPTY)
  const [busy, setBusy] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try { setRows(await consoleAccountsApi.list()) }
    catch (e) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [show])
  useEffect(() => { load() }, [load])

  function startAdd() { setForm({ ...EMPTY }); setEditId(null); setAdding(true) }
  function startEdit(a: ConsoleAccount) {
    setForm({ login_id: a.login_id, name: a.name, role: a.role, email: a.email || '', password: '' })
    setAdding(false); setEditId(a.login_id)
  }
  function cancel() { setAdding(false); setEditId(null); setForm(EMPTY) }

  async function submitAdd() {
    if (!form.login_id.trim()) { show('아이디를 입력하세요', 'err'); return }
    if (!form.password || form.password.length < 4) { show('비밀번호는 4자 이상', 'err'); return }
    setBusy(true)
    try {
      await consoleAccountsApi.create({
        login_id: form.login_id.trim(), name: form.name.trim() || form.login_id.trim(),
        role: form.role, password: form.password, email: form.email.trim(),
      })
      show('콘솔 계정 생성', 'ok'); cancel(); load()
    } catch (e) { show(String(e), 'err') } finally { setBusy(false) }
  }

  async function submitEdit() {
    if (!editId) return
    setBusy(true)
    try {
      await consoleAccountsApi.update(editId, { name: form.name.trim(), role: form.role, email: form.email.trim() })
      show('수정 완료', 'ok'); cancel(); load()
    } catch (e) { show(String(e), 'err') } finally { setBusy(false) }
  }

  async function resetPassword(a: ConsoleAccount) {
    const pw = window.prompt(`'${a.login_id}' 새 비밀번호 (4자 이상)`)
    if (pw == null) return
    if (pw.length < 4) { show('비밀번호는 4자 이상', 'err'); return }
    try { await consoleAccountsApi.setPassword(a.login_id, pw); show('비밀번호 변경', 'ok') }
    catch (e) { show(String(e), 'err') }
  }

  async function remove(a: ConsoleAccount) {
    if (!window.confirm(`콘솔 계정 '${a.login_id}' 을(를) 삭제할까요?`)) return
    try { await consoleAccountsApi.delete(a.login_id); show('삭제 완료', 'ok'); load() }
    catch (e) { show(String(e), 'err') }
  }

  return (
    <div style={{ padding: 16 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 12 }}>
        <h2 style={{ margin: 0, fontSize: 18 }}>콘솔 계정</h2>
        <span className="ts" style={{ color: 'var(--text-muted)' }}>
          OAM 로그인 계정 (가입자와 분리). 내장 admin 계정은 oam.json 으로 관리되어 표시되지 않습니다.
        </span>
        {!adding && !editId && (
          <button className="btn btn--sm btn--primary" style={{ marginLeft: 'auto' }} onClick={startAdd}>
            <Plus size={14} /> 계정 추가
          </button>
        )}
      </div>

      {(adding || editId) && (
        <div style={{ display: 'flex', gap: 8, alignItems: 'flex-end', flexWrap: 'wrap',
                      padding: 12, background: 'var(--surface-2, #f6f7f9)', borderRadius: 8, marginBottom: 12 }}>
          <Field label="아이디 *" w={150}>
            <input className="form-input" value={form.login_id} disabled={!!editId} autoFocus={!editId}
                   onChange={e => setForm({ ...form, login_id: e.target.value })} />
          </Field>
          <Field label="이름" w={140}>
            <input className="form-input" value={form.name} autoFocus={!!editId}
                   onChange={e => setForm({ ...form, name: e.target.value })} />
          </Field>
          <Field label="권한" w={130}>
            <select className="form-input" value={form.role}
                    onChange={e => setForm({ ...form, role: e.target.value as ConsoleRole })}>
              {CONSOLE_ROLES.map(r => <option key={r} value={r}>{ROLE_LABELS[r]}</option>)}
            </select>
          </Field>
          <Field label="이메일" w={180}>
            <input className="form-input" value={form.email}
                   onChange={e => setForm({ ...form, email: e.target.value })} />
          </Field>
          {!editId && (
            <Field label="비밀번호 *" w={150}>
              <input className="form-input" type="password" value={form.password}
                     onChange={e => setForm({ ...form, password: e.target.value })} />
            </Field>
          )}
          <div style={{ display: 'flex', gap: 6 }}>
            <button className="btn btn--sm btn--primary" disabled={busy} onClick={editId ? submitEdit : submitAdd}>
              {editId ? '저장' : '생성'}
            </button>
            <button className="btn btn--sm btn--ghost" onClick={cancel}>취소</button>
          </div>
        </div>
      )}

      <table className="table" style={{ width: '100%' }}>
        <thead>
          <tr>
            <th style={{ textAlign: 'left' }}>아이디</th>
            <th style={{ textAlign: 'left' }}>이름</th>
            <th style={{ textAlign: 'left' }}>권한</th>
            <th style={{ textAlign: 'left' }}>이메일</th>
            <th style={{ textAlign: 'left' }}>수정시각</th>
            <th style={{ width: 120 }}></th>
          </tr>
        </thead>
        <tbody>
          {loading && <tr><td colSpan={6} className="ts">불러오는 중…</td></tr>}
          {!loading && rows.length === 0 && <tr><td colSpan={6} className="ts">계정 없음</td></tr>}
          {rows.map(a => (
            <tr key={a.login_id}>
              <td><strong>{a.login_id}</strong></td>
              <td>{a.name}</td>
              <td><span className="badge">{ROLE_LABELS[a.role]}</span></td>
              <td className="ts">{a.email || '—'}</td>
              <td className="ts">{a.update_time || '—'}</td>
              <td style={{ textAlign: 'right', whiteSpace: 'nowrap' }}>
                <IconBtn title="편집" onClick={() => startEdit(a)}><Pencil size={14} /></IconBtn>
                <IconBtn title="비밀번호 재설정" onClick={() => resetPassword(a)}><KeyRound size={14} /></IconBtn>
                <IconBtn title="삭제" tone="danger" onClick={() => remove(a)}><Trash2 size={14} /></IconBtn>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

function Field({ label, w, children }: { label: string; w?: number; children: React.ReactNode }) {
  return (
    <label style={{ display: 'flex', flexDirection: 'column', gap: 3, width: w }}>
      <span className="ts" style={{ fontSize: 11, color: 'var(--text-muted)' }}>{label}</span>
      {children}
    </label>
  )
}

function IconBtn({ title, tone, onClick, children }: {
  title: string; tone?: 'danger'; onClick: () => void; children: React.ReactNode
}) {
  return (
    <button className={`btn btn--icon btn--sm${tone === 'danger' ? ' btn--danger' : ''}`}
            title={title} onClick={onClick} style={{ marginLeft: 4 }}>
      {children}
    </button>
  )
}
