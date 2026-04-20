import { useState, useEffect, useCallback } from 'react'
import {
  cspRuntimeApi,
  type AccessEntry, type AccessEntryInput,
  type AccessScope, type AccessKind, type AccessMatchType,
} from '../api/cspRuntime'
import { useToast } from '../components/Toast'

const SCOPES: AccessScope[] = ['global', 'listener', 'trunk']
const KINDS: AccessKind[] = ['allow', 'deny']
const TYPES: AccessMatchType[] = ['ip', 'cidr', 'ua_regex']

const EMPTY: AccessEntryInput = {
  scope: 'global',
  kind: 'deny',
  match_type: 'ip',
  value: '',
  enabled: true,
  priority: 100,
}

export default function SipAccessPage() {
  const { show } = useToast()
  const [items, setItems] = useState<AccessEntry[]>([])
  const [loading, setLoading] = useState(true)
  const [editOpen, setEditOpen] = useState(false)
  const [editId, setEditId] = useState<number | null>(null)
  const [form, setForm] = useState<AccessEntryInput>(EMPTY)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const list = await cspRuntimeApi.listAccess()
      setItems(list)
    } catch (e) { show((e as Error).message, 'err') }
    finally { setLoading(false) }
  }, [show])

  useEffect(() => { void load() }, [load])

  function openCreate() {
    setEditId(null); setForm({ ...EMPTY }); setEditOpen(true)
  }
  function openEdit(row: AccessEntry) {
    setEditId(row.id)
    setForm({
      scope: row.scope, scope_ref_id: row.scope_ref_id,
      kind: row.kind, match_type: row.match_type,
      value: row.value, enabled: row.enabled, priority: row.priority,
      note: row.note,
    })
    setEditOpen(true)
  }

  async function save() {
    try {
      if (editId == null) { await cspRuntimeApi.createAccess(form); show('등록 완료', 'ok') }
      else                 { await cspRuntimeApi.updateAccess(editId, form); show('수정 완료', 'ok') }
      setEditOpen(false); await load()
    } catch (e) { show((e as Error).message, 'err') }
  }

  async function remove(row: AccessEntry) {
    if (!confirm(`ACL id=${row.id} (${row.kind} ${row.match_type}:${row.value}) 을 삭제할까요?`)) return
    try {
      await cspRuntimeApi.deleteAccess(row.id)
      show('삭제 완료', 'ok'); await load()
    } catch (e) { show((e as Error).message, 'err') }
  }

  return (
    <div>
      <div style={{ marginBottom: 16, display: 'flex', gap: 12, alignItems: 'center' }}>
        <h3 style={{ margin: 0 }}>SIP 접근제어</h3>
        <span className="text-muted" style={{ fontSize: 13 }}>
          IP/CIDR/UA-regex 기반 allow/deny. deny 가 먼저 매칭되면 403 응답. 우선순위 낮을수록 먼저 평가.
        </span>
        <div style={{ marginLeft: 'auto' }}>
          <button className="btn btn--primary" onClick={openCreate}>＋ 항목 추가</button>
        </div>
      </div>

      {loading ? (
        <div className="empty">로딩 중...</div>
      ) : items.length === 0 ? (
        <div className="empty">등록된 ACL 없음 (기본 허용)</div>
      ) : (
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 40 }}>ID</th>
              <th style={{ width: 70 }}>Priority</th>
              <th style={{ width: 80 }}>Scope</th>
              <th style={{ width: 80 }}>Kind</th>
              <th style={{ width: 100 }}>Match</th>
              <th>Value</th>
              <th style={{ width: 60 }}>활성</th>
              <th>메모</th>
              <th style={{ width: 140 }}>작업</th>
            </tr>
          </thead>
          <tbody>
            {items.map(r => (
              <tr key={r.id}>
                <td>{r.id}</td>
                <td>{r.priority}</td>
                <td>{r.scope}</td>
                <td>
                  <span className="tag" style={{
                    background: r.kind === 'deny' ? '#e74c3c' : '#2ecc71',
                    color: '#fff',
                  }}>{r.kind}</span>
                </td>
                <td>{r.match_type}</td>
                <td style={{ fontFamily: 'monospace' }}>{r.value}</td>
                <td>{r.enabled ? '✓' : '×'}</td>
                <td>{r.note || '—'}</td>
                <td>
                  <button className="btn btn--sm" onClick={() => openEdit(r)}>수정</button>{' '}
                  <button className="btn btn--sm btn--danger" onClick={() => remove(r)}>삭제</button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}

      {editOpen && (
        <div className="modal-overlay" onClick={() => setEditOpen(false)}>
          <div className="modal-box" onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">{editId == null ? 'ACL 추가' : 'ACL 수정'}</span>
              <button className="modal-close" onClick={() => setEditOpen(false)}>✕</button>
            </div>
            <div className="modal-body">
              <div className="form-grid">
                <label>Scope</label>
                <select className="form-input" value={form.scope ?? 'global'}
                  onChange={e => setForm(f => ({ ...f, scope: e.target.value as AccessScope }))}>
                  {SCOPES.map(s => <option key={s}>{s}</option>)}
                </select>
                {form.scope !== 'global' && (
                  <>
                    <label>Scope Ref ID</label>
                    <input className="form-input" type="number" value={form.scope_ref_id ?? 0}
                      onChange={e => setForm(f => ({ ...f, scope_ref_id: parseInt(e.target.value || '0', 10) }))} />
                  </>
                )}
                <label>Kind</label>
                <select className="form-input" value={form.kind}
                  onChange={e => setForm(f => ({ ...f, kind: e.target.value as AccessKind }))}>
                  {KINDS.map(k => <option key={k}>{k}</option>)}
                </select>
                <label>Match Type</label>
                <select className="form-input" value={form.match_type}
                  onChange={e => setForm(f => ({ ...f, match_type: e.target.value as AccessMatchType }))}>
                  {TYPES.map(t => <option key={t}>{t}</option>)}
                </select>
                <label>Value</label>
                <input className="form-input" value={form.value} placeholder={
                  form.match_type === 'cidr' ? '예: 10.0.0.0/8'
                    : form.match_type === 'ua_regex' ? '예: friendly-scanner'
                    : '예: 203.0.113.45'
                }
                  onChange={e => setForm(f => ({ ...f, value: e.target.value }))} />
                <label>우선순위</label>
                <input className="form-input" type="number" value={form.priority ?? 100}
                  onChange={e => setForm(f => ({ ...f, priority: parseInt(e.target.value || '0', 10) }))} />
                <label>활성</label>
                <input type="checkbox" checked={form.enabled ?? true}
                  onChange={e => setForm(f => ({ ...f, enabled: e.target.checked }))} />
                <label>메모</label>
                <input className="form-input" value={form.note ?? ''}
                  onChange={e => setForm(f => ({ ...f, note: e.target.value }))} />
              </div>
            </div>
            <div className="modal-footer">
              <button className="btn btn--outline" onClick={() => setEditOpen(false)}>취소</button>
              <button className="btn btn--primary" onClick={save}>저장</button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
