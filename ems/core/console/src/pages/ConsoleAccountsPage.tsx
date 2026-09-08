// 콘솔 계정 관리 — OAM 로그인 계정(file_store 도메인 console_accounts) CRUD.
// DB users(가입자 person)와 분리. 내장 admin(oam.json)은 여기 표시되지 않음(부트스트랩 전용).
import { useConfirm } from '../components/custom/confirm'
import { useCallback, useEffect, useState } from 'react'
import { Pencil, Trash2, KeyRound, Plus } from 'lucide-react'
import { useToast } from '../components/Toast'
import { ROLE_LABELS } from '../utils/permissions'
import {
  consoleAccountsApi, CONSOLE_ROLES,
  type ConsoleAccount, type ConsoleRole,
} from '../api/consoleAccounts'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { fromSel, toSel } from '@core/components/custom/select-value'
import { Badge } from '@core/components/ui/badge'
import { DataTable, Th, Td } from '@core/components/custom/data-table'

type Form = { login_id: string; name: string; role: ConsoleRole; email: string; password: string }
const EMPTY: Form = { login_id: '', name: '', role: 'operator', email: '', password: '' }

export default function ConsoleAccountsPage() {
  const { show } = useToast()
  const confirm = useConfirm()
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
    if (!await confirm({ title: '콘솔 계정 삭제', tone: 'danger', confirmLabel: '삭제',
      body: `콘솔 계정 '${a.login_id}' 을(를) 삭제할까요?` })) return
    try { await consoleAccountsApi.delete(a.login_id); show('삭제 완료', 'ok'); load() }
    catch (e) { show(String(e), 'err') }
  }

  return (
    <div className="p-4">
      <div className="flex items-center gap-3 mb-3">
        <h2 className="m-0 text-xl">콘솔 계정</h2>
        <span className="text-sm text-muted-foreground">
          OAM 로그인 계정 (가입자와 분리). 내장 admin 계정은 oam.json 으로 관리되어 표시되지 않습니다.
        </span>
        {!adding && !editId && (
          <Button className="ml-auto" variant="default" onClick={startAdd}>
            <Plus size={14} /> 계정 추가
          </Button>
        )}
      </div>

      {(adding || editId) && (
        <div className="flex gap-2 items-end flex-wrap p-3 bg-secondary rounded-md mb-3">
          <Field label="아이디 *" w={150}>
            <Input  value={form.login_id} disabled={!!editId} autoFocus={!editId}
                   onChange={e => setForm({ ...form, login_id: e.target.value })} />
          </Field>
          <Field label="이름" w={140}>
            <Input  value={form.name} autoFocus={!!editId}
                   onChange={e => setForm({ ...form, name: e.target.value })} />
          </Field>
          <Field label="권한" w={130}>
            <Select value={toSel(form.role)} onValueChange={(v: string) => setForm({ ...form, role: fromSel(v) as ConsoleRole })}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>
                {CONSOLE_ROLES.map(r => <SelectItem key={r} value={r}>{ROLE_LABELS[r]}</SelectItem>)}
              </SelectContent>
            </Select>
          </Field>
          <Field label="이메일" w={180}>
            <Input  value={form.email}
                   onChange={e => setForm({ ...form, email: e.target.value })} />
          </Field>
          {!editId && (
            <Field label="비밀번호 *" w={150}>
              <Input  type="password" value={form.password}
                     onChange={e => setForm({ ...form, password: e.target.value })} />
            </Field>
          )}
          <div className="flex gap-1.5">
            <Button variant="default" disabled={busy} onClick={editId ? submitEdit : submitAdd}>
              {editId ? '저장' : '생성'}
            </Button>
            <Button variant="ghost" onClick={cancel}>취소</Button>
          </div>
        </div>
      )}

      <DataTable sticky>
        <thead>
          <tr>
            <Th className="text-left">아이디</Th>
            <Th className="text-left">이름</Th>
            <Th className="text-left">권한</Th>
            <Th className="text-left">이메일</Th>
            <Th className="text-left">수정시각</Th>
            <Th className="w-[120px]"></Th>
          </tr>
        </thead>
        <tbody>
          {loading && <tr><Td colSpan={6} className="text-sm text-muted-foreground">불러오는 중…</Td></tr>}
          {!loading && rows.length === 0 && <tr><Td colSpan={6} className="text-sm text-muted-foreground">계정 없음</Td></tr>}
          {rows.map(a => (
            <tr key={a.login_id}>
              <Td><strong>{a.login_id}</strong></Td>
              <Td>{a.name}</Td>
              <Td><Badge >{ROLE_LABELS[a.role]}</Badge></Td>
              <Td className="text-sm text-muted-foreground">{a.email || '—'}</Td>
              <Td className="text-sm text-muted-foreground">{a.update_time || '—'}</Td>
              <Td className="text-right whitespace-nowrap">
                <IconBtn title="편집" onClick={() => startEdit(a)}><Pencil size={14} /></IconBtn>
                <IconBtn title="비밀번호 재설정" onClick={() => resetPassword(a)}><KeyRound size={14} /></IconBtn>
                <IconBtn title="삭제" tone="danger" onClick={() => remove(a)}><Trash2 size={14} /></IconBtn>
              </Td>
            </tr>
          ))}
        </tbody>
      </DataTable>
    </div>
  )
}

function Field({ label, w, children }: { label: string; w?: number; children: React.ReactNode }) {
  return (
    <label style={{ display: 'flex', flexDirection: 'column', gap: 3, width: w }}>
      <span className="text-xs text-muted-foreground">{label}</span>
      {children}
    </label>
  )
}

function IconBtn({ title, tone, onClick, children }: {
  title: string; tone?: 'danger'; onClick: () => void; children: React.ReactNode
}) {
  return (
    <Button className="ml-1" variant={tone === 'danger' ? 'destructive' : 'outline'}
            title={title} onClick={onClick}>
      {children}
    </Button>
  )
}
