import { useState, useEffect, useCallback } from 'react'
import {
  cspRuntimeApi,
  type SipService, type SipServiceInput,
  type ServiceKind, type InboundPolicy, type SipListener,
} from '../api/cspRuntime'
import { useToast } from '../components/Toast'

const KINDS: ServiceKind[] = ['voip', 'ptt', 'ibcf', 'system', 'console']
const POLICIES: InboundPolicy[] = ['any', 'restricted']

const EMPTY: SipServiceInput = {
  name: '',
  kind: 'voip',
  domain: '',
  auth_realm: null,
  inbound_policy: 'any',
  priority: 100,
  enabled: true,
  listeners: [],
  note: null,
}

export default function SipServicesPage() {
  const { show } = useToast()
  const [items, setItems] = useState<SipService[]>([])
  const [listeners, setListeners] = useState<SipListener[]>([])
  const [loading, setLoading] = useState(true)
  const [editOpen, setEditOpen] = useState(false)
  const [editId, setEditId] = useState<number | null>(null)
  const [form, setForm] = useState<SipServiceInput>(EMPTY)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [svcs, lsnrs] = await Promise.all([
        cspRuntimeApi.listServices(),
        cspRuntimeApi.listListeners(),
      ])
      setItems(svcs); setListeners(lsnrs)
    } catch (e) { show((e as Error).message, 'err') }
    finally { setLoading(false) }
  }, [show])

  useEffect(() => { void load() }, [load])

  function openCreate() {
    setEditId(null); setForm(JSON.parse(JSON.stringify(EMPTY))); setEditOpen(true)
  }
  function openEdit(row: SipService) {
    setEditId(row.id)
    setForm({
      name: row.name, kind: row.kind, domain: row.domain,
      auth_realm: row.auth_realm, inbound_policy: row.inbound_policy,
      priority: row.priority, enabled: row.enabled,
      listeners: row.listeners, note: row.note,
    })
    setEditOpen(true)
  }

  async function save() {
    try {
      if (editId == null) { await cspRuntimeApi.createService(form); show('서비스 등록', 'ok') }
      else                 { await cspRuntimeApi.updateService(editId, form); show('서비스 수정', 'ok') }
      setEditOpen(false); await load()
    } catch (e) { show((e as Error).message, 'err') }
  }

  async function remove(row: SipService) {
    if (!confirm(`서비스 "${row.name}" (${row.domain}) 을 삭제할까요?\n소속 가입자/트렁크는 service_id=NULL 이 되고, REGISTER/라우팅 시 거부됩니다.`)) return
    try { await cspRuntimeApi.deleteService(row.id); show('삭제됨', 'ok'); await load() }
    catch (e) { show((e as Error).message, 'err') }
  }

  function toggleListener(lid: number) {
    setForm(f => {
      const cur = new Set(f.listeners || [])
      if (cur.has(lid)) cur.delete(lid); else cur.add(lid)
      return { ...f, listeners: Array.from(cur) }
    })
  }

  return (
    <div>
      <div style={{ marginBottom: 16, display: 'flex', gap: 12, alignItems: 'center' }}>
        <h3 style={{ margin: 0 }}>SIP 서비스</h3>
        <span className="text-muted" style={{ fontSize: 13 }}>
          도메인/실(Realm) 기준 비즈니스 경계. 가입자·트렁크가 서비스에 귀속되며 REGISTER 인증은 service.domain 기반.
        </span>
        <div style={{ marginLeft: 'auto' }}>
          <button className="btn btn--primary" onClick={openCreate}>＋ 서비스 추가</button>
        </div>
      </div>

      {loading ? (
        <div className="empty">로딩 중...</div>
      ) : items.length === 0 ? (
        <div className="empty">등록된 서비스 없음</div>
      ) : (
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 40 }}>ID</th>
              <th>이름</th>
              <th style={{ width: 70 }}>Kind</th>
              <th>Domain</th>
              <th>Auth Realm</th>
              <th style={{ width: 100 }}>Inbound</th>
              <th style={{ width: 70 }}>Priority</th>
              <th style={{ width: 60 }}>활성</th>
              <th style={{ width: 140 }}>작업</th>
            </tr>
          </thead>
          <tbody>
            {items.map(r => (
              <tr key={r.id}>
                <td>{r.id}</td>
                <td>{r.name}</td>
                <td><span className="tag">{r.kind}</span></td>
                <td style={{ fontFamily: 'monospace', fontSize: 12 }}>{r.domain}</td>
                <td style={{ fontFamily: 'monospace', fontSize: 12 }}>{r.auth_realm || <em>(= domain)</em>}</td>
                <td>{r.inbound_policy === 'any' ? 'any' : `restricted(${r.listeners.length})`}</td>
                <td>{r.priority}</td>
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
              <span className="modal-title">{editId == null ? 'SIP 서비스 추가' : 'SIP 서비스 수정'}</span>
              <button className="modal-close" onClick={() => setEditOpen(false)}>✕</button>
            </div>
            <div className="modal-body">
              <div className="form-grid">
                <label>이름</label>
                <input className="form-input" value={form.name}
                  onChange={e => setForm(f => ({ ...f, name: e.target.value }))} />
                <label>Kind</label>
                <select className="form-input" value={form.kind}
                  onChange={e => setForm(f => ({ ...f, kind: e.target.value as ServiceKind }))}>
                  {KINDS.map(k => <option key={k}>{k}</option>)}
                </select>
                <label>Domain</label>
                <input className="form-input" value={form.domain}
                  placeholder="ims.mnc001.mcc001.3gppnetwork.org"
                  onChange={e => setForm(f => ({ ...f, domain: e.target.value }))} />
                <label>Auth Realm (선택)</label>
                <input className="form-input" value={form.auth_realm ?? ''}
                  placeholder="(비우면 domain 그대로 사용)"
                  onChange={e => setForm(f => ({ ...f, auth_realm: e.target.value || null }))} />
                <label>우선순위</label>
                <input className="form-input" type="number" value={form.priority ?? 100}
                  onChange={e => setForm(f => ({ ...f, priority: parseInt(e.target.value || '0', 10) }))} />
                <label>Inbound Policy</label>
                <select className="form-input" value={form.inbound_policy ?? 'any'}
                  onChange={e => setForm(f => ({ ...f, inbound_policy: e.target.value as InboundPolicy }))}>
                  {POLICIES.map(p => <option key={p}>{p}</option>)}
                </select>
                <label>활성</label>
                <input type="checkbox" checked={form.enabled ?? true}
                  onChange={e => setForm(f => ({ ...f, enabled: e.target.checked }))} />
                <label>메모</label>
                <input className="form-input" value={form.note ?? ''}
                  onChange={e => setForm(f => ({ ...f, note: e.target.value }))} />
              </div>

              {form.inbound_policy === 'restricted' && (
                <div style={{ marginTop: 12 }}>
                  <div style={{ fontSize: 13, marginBottom: 6 }}>
                    <b>허용 리스너</b> (restricted 정책 — 선택한 리스너로만 수신 허용)
                  </div>
                  <div style={{ display: 'flex', flexWrap: 'wrap', gap: 8 }}>
                    {listeners.map(l => {
                      const checked = (form.listeners || []).includes(l.id)
                      return (
                        <label key={l.id} style={{
                          display: 'inline-flex', alignItems: 'center', gap: 4,
                          padding: '4px 8px', border: '1px solid #ccc', borderRadius: 4,
                          background: checked ? '#e6f3ff' : '#fff',
                        }}>
                          <input type="checkbox" checked={checked}
                            onChange={() => toggleListener(l.id)} />
                          <span style={{ fontSize: 12 }}>
                            #{l.id} {l.bind_ip}:{l.bind_port}/{l.protocol}
                          </span>
                        </label>
                      )
                    })}
                  </div>
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
