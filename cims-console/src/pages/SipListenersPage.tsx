import { useState, useEffect, useCallback } from 'react'
import { cspRuntimeApi, type SipListener, type SipListenerCreate, type SipProtocol, type SipService } from '../api/cspRuntime'
import { useToast } from '../components/Toast'

type Mode = 'form' | 'json'

const PROTOCOLS: SipProtocol[] = ['UDP', 'TCP', 'TLS', 'WS', 'WSS']
const SERVICES: SipService[] = ['volte', 'mcptt', 'system', 'console']

const EMPTY_FORM: SipListenerCreate = {
  name: '',
  bind_ip: '0.0.0.0',
  bind_port: 5060,
  protocol: 'UDP',
  domain: '',
  service: 'volte',
  enabled: true,
  thread_count: 2,
  tls_cert_path: null,
  tls_key_path: null,
  tls_ca_path: null,
  tls_verify_peer: false,
  max_connections: 0,
  note: null,
}

export default function SipListenersPage() {
  const { show } = useToast()
  const [items, setItems] = useState<SipListener[]>([])
  const [loading, setLoading] = useState(true)

  const [editOpen, setEditOpen] = useState(false)
  const [editId, setEditId] = useState<number | null>(null)
  const [mode, setMode] = useState<Mode>('form')
  const [form, setForm] = useState<SipListenerCreate>(EMPTY_FORM)
  const [jsonText, setJsonText] = useState('')
  const [jsonErr, setJsonErr] = useState('')

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const list = await cspRuntimeApi.listListeners()
      setItems(list)
    } catch (e) {
      show(`리스너 목록 조회 실패: ${(e as Error).message}`, 'err')
    } finally {
      setLoading(false)
    }
  }, [show])

  useEffect(() => { void load() }, [load])

  function openCreate() {
    setEditId(null)
    setMode('form')
    setForm(EMPTY_FORM)
    setJsonText(JSON.stringify(EMPTY_FORM, null, 2))
    setJsonErr('')
    setEditOpen(true)
  }

  function openEdit(row: SipListener) {
    setEditId(row.id)
    setMode('form')
    const f: SipListenerCreate = {
      name: row.name,
      bind_ip: row.bind_ip,
      bind_port: row.bind_port,
      protocol: row.protocol,
      domain: row.domain,
      service: row.service,
      enabled: row.enabled,
      thread_count: row.thread_count,
      tls_cert_path: row.tls_cert_path,
      tls_key_path: row.tls_key_path,
      tls_ca_path: row.tls_ca_path,
      tls_verify_peer: row.tls_verify_peer,
      max_connections: row.max_connections,
      note: row.note,
    }
    setForm(f)
    setJsonText(JSON.stringify(f, null, 2))
    setJsonErr('')
    setEditOpen(true)
  }

  function toggleMode(newMode: Mode) {
    if (newMode === mode) return
    if (newMode === 'json') {
      setJsonText(JSON.stringify(form, null, 2))
      setJsonErr('')
    } else {
      try {
        const parsed = JSON.parse(jsonText) as SipListenerCreate
        setForm({ ...EMPTY_FORM, ...parsed })
        setJsonErr('')
      } catch (e) {
        setJsonErr((e as Error).message)
        return
      }
    }
    setMode(newMode)
  }

  async function save() {
    let body: SipListenerCreate
    if (mode === 'json') {
      try {
        body = JSON.parse(jsonText) as SipListenerCreate
      } catch (e) {
        setJsonErr((e as Error).message)
        return
      }
    } else {
      body = { ...form }
    }
    try {
      if (editId == null) {
        await cspRuntimeApi.createListener(body)
        show('리스너 추가됨 (즉시 적용)', 'ok')
      } else {
        await cspRuntimeApi.updateListener(editId, body)
        show('리스너 수정됨 (즉시 적용)', 'ok')
      }
      setEditOpen(false)
      await load()
    } catch (e) {
      show((e as Error).message, 'err')
    }
  }

  async function remove(row: SipListener) {
    if (!confirm(`리스너 "${row.name}" (${row.bind_ip}:${row.bind_port}/${row.protocol}) 을 삭제할까요?\n진행 중 호는 유지되고 신규 바인드만 제거됩니다.`)) return
    try {
      await cspRuntimeApi.deleteListener(row.id)
      show('리스너 삭제됨 (즉시 적용)', 'ok')
      await load()
    } catch (e) {
      show((e as Error).message, 'err')
    }
  }

  return (
    <div>
      <div style={{ marginBottom: 16, display: 'flex', gap: 12, alignItems: 'center' }}>
        <h3 style={{ margin: 0 }}>SIP 리스너</h3>
        <span className="text-muted" style={{ fontSize: 13 }}>
          CSP 가 SIP 메시지를 수신할 IP/포트/프로토콜. 추가·수정·삭제는 재기동 없이 즉시 반영됩니다.
        </span>
        <div style={{ marginLeft: 'auto' }}>
          <button className="btn btn--primary" onClick={openCreate}>＋ 리스너 추가</button>
        </div>
      </div>

      {loading ? (
        <div className="empty">로딩 중...</div>
      ) : items.length === 0 ? (
        <div className="empty">등록된 리스너 없음 (부트스트랩 포트만 사용 중)</div>
      ) : (
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 60 }}>ID</th>
              <th>이름</th>
              <th>서비스</th>
              <th>Bind IP</th>
              <th>Port</th>
              <th>Proto</th>
              <th>Domain</th>
              <th>활성</th>
              <th style={{ width: 140 }}>작업</th>
            </tr>
          </thead>
          <tbody>
            {items.map(r => (
              <tr key={r.id}>
                <td>{r.id}</td>
                <td>{r.name}</td>
                <td><span className={`tag tag--${r.service}`}>{r.service}</span></td>
                <td>{r.bind_ip}</td>
                <td>{r.bind_port}</td>
                <td>{r.protocol}</td>
                <td>{r.domain || '—'}</td>
                <td>{r.enabled ? '✓' : '×'}</td>
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
          <div className="modal-box" style={{ minWidth: 560 }} onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">{editId == null ? 'SIP 리스너 추가' : 'SIP 리스너 수정'}</span>
              <button className="modal-close" onClick={() => setEditOpen(false)}>✕</button>
            </div>
            <div className="modal-body">
              <div style={{ marginBottom: 12, display: 'flex', gap: 8 }}>
                <button
                  className={`btn btn--sm ${mode === 'form' ? 'btn--primary' : 'btn--outline'}`}
                  onClick={() => toggleMode('form')}>폼</button>
                <button
                  className={`btn btn--sm ${mode === 'json' ? 'btn--primary' : 'btn--outline'}`}
                  onClick={() => toggleMode('json')}>JSON</button>
              </div>

              {mode === 'form' ? (
                <div className="form-grid">
                  <label>이름</label>
                  <input className="form-input" value={form.name}
                    onChange={e => setForm(f => ({ ...f, name: e.target.value }))} />
                  <label>Bind IP</label>
                  <input className="form-input" value={form.bind_ip}
                    onChange={e => setForm(f => ({ ...f, bind_ip: e.target.value }))} />
                  <label>Port</label>
                  <input className="form-input" type="number" value={form.bind_port}
                    onChange={e => setForm(f => ({ ...f, bind_port: parseInt(e.target.value || '0', 10) }))} />
                  <label>Protocol</label>
                  <select className="form-input" value={form.protocol}
                    onChange={e => setForm(f => ({ ...f, protocol: e.target.value as SipProtocol }))}>
                    {PROTOCOLS.map(p => <option key={p} value={p}>{p}</option>)}
                  </select>
                  <label>Service</label>
                  <select className="form-input" value={form.service}
                    onChange={e => setForm(f => ({ ...f, service: e.target.value as SipService }))}>
                    {SERVICES.map(s => <option key={s} value={s}>{s}</option>)}
                  </select>
                  <label>Domain</label>
                  <input className="form-input" value={form.domain || ''}
                    onChange={e => setForm(f => ({ ...f, domain: e.target.value }))} />
                  <label>Thread Count</label>
                  <input className="form-input" type="number" value={form.thread_count}
                    onChange={e => setForm(f => ({ ...f, thread_count: parseInt(e.target.value || '0', 10) }))} />
                  <label>활성</label>
                  <input type="checkbox" checked={form.enabled ?? true}
                    onChange={e => setForm(f => ({ ...f, enabled: e.target.checked }))} />
                  {(form.protocol === 'TLS' || form.protocol === 'WSS') && (
                    <>
                      <label>TLS Cert 경로</label>
                      <input className="form-input" value={form.tls_cert_path || ''}
                        onChange={e => setForm(f => ({ ...f, tls_cert_path: e.target.value }))} />
                      <label>TLS Key 경로</label>
                      <input className="form-input" value={form.tls_key_path || ''}
                        onChange={e => setForm(f => ({ ...f, tls_key_path: e.target.value }))} />
                    </>
                  )}
                  <label>메모</label>
                  <input className="form-input" value={form.note || ''}
                    onChange={e => setForm(f => ({ ...f, note: e.target.value }))} />
                </div>
              ) : (
                <div>
                  <textarea className="form-input" style={{ width: '100%', minHeight: 320, fontFamily: 'monospace' }}
                    value={jsonText} onChange={e => setJsonText(e.target.value)} />
                  {jsonErr && <div className="auth-error" style={{ marginTop: 8 }}>JSON 오류: {jsonErr}</div>}
                </div>
              )}
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
