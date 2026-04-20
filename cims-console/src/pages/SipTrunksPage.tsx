import { useState, useEffect, useCallback } from 'react'
import { cspRuntimeApi, type SipTrunk, type SipTrunkCreate, type SipTrunkStatus } from '../api/cspRuntime'
import { statsApi } from '../api/stats'
import { useToast } from '../components/Toast'

type Mode = 'form' | 'json'

const EMPTY_FORM: SipTrunkCreate = {
  name: '',
  remote_ip: '',
  remote_port: 5060,
  remote_domain: '',
  protocol: 'UDP',
  enabled: true,
  options_ping_sec: 60,
  options_dead_threshold: 3,
  register_to_remote: false,
  dns_fallback: true,
  srv_lookup: false,
  register_expires: 3600,
  max_concurrent_calls: 0,
  cps_limit: 0,
  note: null,
}

export default function SipTrunksPage() {
  const { show } = useToast()
  const [items, setItems] = useState<SipTrunk[]>([])
  const [status, setStatus] = useState<Record<number, SipTrunkStatus>>({})
  const [loading, setLoading] = useState(true)

  const [editOpen, setEditOpen] = useState(false)
  const [editId, setEditId] = useState<number | null>(null)
  const [mode, setMode] = useState<Mode>('form')
  const [form, setForm] = useState<SipTrunkCreate>(EMPTY_FORM)
  const [jsonText, setJsonText] = useState('')
  const [jsonErr, setJsonErr] = useState('')

  const loadStatus = useCallback(async () => {
    try {
      const h = await statsApi.health()
      const map: Record<number, SipTrunkStatus> = {}
      ;(h.csp?.trunks || []).forEach(t => { map[t.id] = t })
      setStatus(map)
    } catch {
      // silent — 주기 업데이트
    }
  }, [])

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const list = await cspRuntimeApi.listTrunks()
      setItems(list)
    } catch (e) {
      show(`트렁크 목록 조회 실패: ${(e as Error).message}`, 'err')
    } finally {
      setLoading(false)
    }
    await loadStatus()
  }, [show, loadStatus])

  useEffect(() => { void load() }, [load])

  // 10초 주기로 헬스 상태 갱신
  useEffect(() => {
    const id = setInterval(() => { void loadStatus() }, 10_000)
    return () => clearInterval(id)
  }, [loadStatus])

  function openCreate() {
    setEditId(null); setMode('form')
    setForm(EMPTY_FORM); setJsonText(JSON.stringify(EMPTY_FORM, null, 2))
    setJsonErr(''); setEditOpen(true)
  }

  function openEdit(row: SipTrunk) {
    setEditId(row.id); setMode('form')
    const f: SipTrunkCreate = { ...EMPTY_FORM, ...row }
    setForm(f); setJsonText(JSON.stringify(f, null, 2))
    setJsonErr(''); setEditOpen(true)
  }

  function toggleMode(newMode: Mode) {
    if (newMode === mode) return
    if (newMode === 'json') {
      setJsonText(JSON.stringify(form, null, 2)); setJsonErr('')
    } else {
      try {
        const parsed = JSON.parse(jsonText) as SipTrunkCreate
        setForm({ ...EMPTY_FORM, ...parsed }); setJsonErr('')
      } catch (e) {
        setJsonErr((e as Error).message); return
      }
    }
    setMode(newMode)
  }

  async function save() {
    let body: SipTrunkCreate
    if (mode === 'json') {
      try { body = JSON.parse(jsonText) as SipTrunkCreate }
      catch (e) { setJsonErr((e as Error).message); return }
    } else {
      body = { ...form }
    }
    try {
      if (editId == null) {
        await cspRuntimeApi.createTrunk(body)
        show('트렁크 추가됨 (즉시 적용)', 'ok')
      } else {
        await cspRuntimeApi.updateTrunk(editId, body)
        show('트렁크 수정됨 (즉시 적용)', 'ok')
      }
      setEditOpen(false); await load()
    } catch (e) {
      show((e as Error).message, 'err')
    }
  }

  async function remove(row: SipTrunk) {
    if (!confirm(`트렁크 "${row.name}" (${row.remote_ip}:${row.remote_port}) 을 삭제할까요?`)) return
    try {
      await cspRuntimeApi.deleteTrunk(row.id)
      show('트렁크 삭제됨', 'ok'); await load()
    } catch (e) { show((e as Error).message, 'err') }
  }

  function healthBadge(id: number) {
    const s = status[id]
    if (!s) return <span className="tag">—</span>
    if (!s.enabled) return <span className="tag" style={{ background: '#ccc', color: '#333' }}>disabled</span>
    if (s.alive) return (
      <span className="tag" style={{ background: '#2ecc71', color: '#fff' }}>
        alive {s.last_rtt_ms >= 0 ? `(${s.last_rtt_ms}ms)` : ''}
      </span>
    )
    return <span className="tag" style={{ background: '#e74c3c', color: '#fff' }}>dead (fails={s.fail_count})</span>
  }

  return (
    <div>
      <div style={{ marginBottom: 16, display: 'flex', gap: 12, alignItems: 'center' }}>
        <h3 style={{ margin: 0 }}>SIP 트렁크 (원격 서버)</h3>
        <span className="text-muted" style={{ fontSize: 13 }}>
          CSP 가 호를 전달할 외부 SIP 서버. OPTIONS 핑 주기로 alive/dead 자동 추적.
        </span>
        <div style={{ marginLeft: 'auto' }}>
          <button className="btn btn--outline" style={{ marginRight: 8 }} onClick={() => void load()}>↻ 새로고침</button>
          <button className="btn btn--primary" onClick={openCreate}>＋ 트렁크 추가</button>
        </div>
      </div>

      {loading ? (
        <div className="empty">로딩 중...</div>
      ) : items.length === 0 ? (
        <div className="empty">등록된 트렁크 없음</div>
      ) : (
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 60 }}>ID</th>
              <th>이름</th>
              <th>원격</th>
              <th>Proto</th>
              <th>Ping 주기</th>
              <th>헬스</th>
              <th>활성</th>
              <th style={{ width: 140 }}>작업</th>
            </tr>
          </thead>
          <tbody>
            {items.map(r => (
              <tr key={r.id}>
                <td>{r.id}</td>
                <td>{r.name}</td>
                <td>{r.remote_ip}:{r.remote_port}{r.remote_domain ? ` (${r.remote_domain})` : ''}</td>
                <td>{r.protocol}</td>
                <td>{r.options_ping_sec > 0 ? `${r.options_ping_sec}s` : '—'}</td>
                <td>{healthBadge(r.id)}</td>
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
          <div className="modal-box" style={{ minWidth: 600 }} onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">{editId == null ? 'SIP 트렁크 추가' : 'SIP 트렁크 수정'}</span>
              <button className="modal-close" onClick={() => setEditOpen(false)}>✕</button>
            </div>
            <div className="modal-body">
              <div style={{ marginBottom: 12, display: 'flex', gap: 8 }}>
                <button className={`btn btn--sm ${mode === 'form' ? 'btn--primary' : 'btn--outline'}`}
                        onClick={() => toggleMode('form')}>폼</button>
                <button className={`btn btn--sm ${mode === 'json' ? 'btn--primary' : 'btn--outline'}`}
                        onClick={() => toggleMode('json')}>JSON</button>
              </div>

              {mode === 'form' ? (
                <div className="form-grid">
                  <label>이름</label>
                  <input className="form-input" value={form.name}
                    onChange={e => setForm(f => ({ ...f, name: e.target.value }))} />
                  <label>원격 IP</label>
                  <input className="form-input" value={form.remote_ip}
                    onChange={e => setForm(f => ({ ...f, remote_ip: e.target.value }))} />
                  <label>원격 Port</label>
                  <input className="form-input" type="number" value={form.remote_port ?? 5060}
                    onChange={e => setForm(f => ({ ...f, remote_port: parseInt(e.target.value || '0', 10) }))} />
                  <label>원격 Domain</label>
                  <input className="form-input" value={form.remote_domain ?? ''}
                    onChange={e => setForm(f => ({ ...f, remote_domain: e.target.value }))} />
                  <label>Protocol</label>
                  <select className="form-input" value={form.protocol ?? 'UDP'}
                    onChange={e => setForm(f => ({ ...f, protocol: e.target.value as 'UDP'|'TCP'|'TLS' }))}>
                    <option>UDP</option><option>TCP</option><option>TLS</option>
                  </select>
                  <label>OPTIONS ping (초)</label>
                  <input className="form-input" type="number" value={form.options_ping_sec ?? 60}
                    onChange={e => setForm(f => ({ ...f, options_ping_sec: parseInt(e.target.value || '0', 10) }))} />
                  <label>Dead 임계치(연속 실패)</label>
                  <input className="form-input" type="number" value={form.options_dead_threshold ?? 3}
                    onChange={e => setForm(f => ({ ...f, options_dead_threshold: parseInt(e.target.value || '0', 10) }))} />
                  <label>동시 호 상한</label>
                  <input className="form-input" type="number" value={form.max_concurrent_calls ?? 0}
                    onChange={e => setForm(f => ({ ...f, max_concurrent_calls: parseInt(e.target.value || '0', 10) }))} />
                  <label>CPS 상한</label>
                  <input className="form-input" type="number" value={form.cps_limit ?? 0}
                    onChange={e => setForm(f => ({ ...f, cps_limit: parseInt(e.target.value || '0', 10) }))} />
                  <label>활성</label>
                  <input type="checkbox" checked={form.enabled ?? true}
                    onChange={e => setForm(f => ({ ...f, enabled: e.target.checked }))} />
                  <label>원격 등록(REGISTER)</label>
                  <input type="checkbox" checked={form.register_to_remote ?? false}
                    onChange={e => setForm(f => ({ ...f, register_to_remote: e.target.checked }))} />
                  {form.register_to_remote && (
                    <>
                      <label>Auth 사용자</label>
                      <input className="form-input" value={form.auth_user ?? ''}
                        onChange={e => setForm(f => ({ ...f, auth_user: e.target.value }))} />
                      <label>Auth 비밀번호</label>
                      <input className="form-input" type="password" value={form.auth_password ?? ''}
                        onChange={e => setForm(f => ({ ...f, auth_password: e.target.value }))} />
                      <label>Auth Realm</label>
                      <input className="form-input" value={form.auth_realm ?? ''}
                        onChange={e => setForm(f => ({ ...f, auth_realm: e.target.value }))} />
                    </>
                  )}
                  <label>메모</label>
                  <input className="form-input" value={form.note ?? ''}
                    onChange={e => setForm(f => ({ ...f, note: e.target.value }))} />
                </div>
              ) : (
                <div>
                  <textarea className="form-input" style={{ width: '100%', minHeight: 360, fontFamily: 'monospace' }}
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
