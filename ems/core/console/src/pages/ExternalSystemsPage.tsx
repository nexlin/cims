// 외부 시스템 관리 — 외부 DB / 모니터링 / 스토리지 / 인증 등 등록. 대시보드 시스템 형상에 표시.
// file_store 컬렉션(OAM /api/v1/external-systems) 기반 CRUD + TCP 라이브니스 probe.
import { useState, useEffect, useCallback } from 'react'
import Modal from '../components/Modal'
import { useToast } from '../components/Toast'
import {
  externalSystemsApi,
  type ExternalSystem, type ExternalSystemInput, type ExternalSystemType,
  type Endpoint, type ProbeMode, type ProbeResult,
} from '../api/external_systems'

const TYPE_LABEL: Record<ExternalSystemType, string> = {
  db: 'DB', monitoring: '모니터링', storage: '스토리지', auth: '인증', other: '기타',
}
const TYPES: ExternalSystemType[] = ['db', 'monitoring', 'storage', 'auth', 'other']
const PROBE_MODES: ProbeMode[] = ['none', 'tcp', 'http', 'icmp']

function StatusDot({ st }: { st?: ProbeResult }) {
  const s = st?.status
  const c = s === 'up' ? '#22c55e' : s === 'down' ? '#e74c3c' : '#9aa5b4'
  const label = s === 'up' ? `정상${st?.latency_ms != null ? ` ${st.latency_ms}ms` : ''}`
    : s === 'down' ? '응답없음' : '미확인'
  return <span title={label}><span style={{ display: 'inline-block', width: 9, height: 9,
    borderRadius: '50%', background: c, marginRight: 6 }} />{label}</span>
}

function blank(): ExternalSystemInput {
  return { name: '', type: 'db', endpoints: [{ host: '', port: 0 }], description: '',
           probe: { mode: 'tcp', timeout: 2 }, tags: [], enabled: true }
}

function EditModal({ initial, onClose, onSaved }: {
  initial: ExternalSystem | null; onClose: () => void; onSaved: () => void
}) {
  const { show } = useToast()
  const [f, setF] = useState<ExternalSystemInput>(initial
    ? { name: initial.name, type: initial.type, endpoints: initial.endpoints.length ? initial.endpoints : [{ host: '', port: 0 }],
        description: initial.description || '', probe: initial.probe || { mode: 'none' },
        tags: initial.tags || [], enabled: initial.enabled }
    : blank())
  const [tagText, setTagText] = useState((initial?.tags || []).join(', '))
  const [saving, setSaving] = useState(false)

  const setEp = (i: number, patch: Partial<Endpoint>) =>
    setF(s => ({ ...s, endpoints: s.endpoints.map((e, j) => j === i ? { ...e, ...patch } : e) }))
  const addEp = () => setF(s => ({ ...s, endpoints: [...s.endpoints, { host: '', port: 0 }] }))
  const rmEp = (i: number) => setF(s => ({ ...s, endpoints: s.endpoints.filter((_, j) => j !== i) }))

  const save = async () => {
    const eps = f.endpoints.filter(e => e.host.trim() && e.port > 0)
    if (!f.name.trim()) { show('이름을 입력하세요', 'err'); return }
    if (eps.length === 0) { show('엔드포인트(host:port) 1개 이상 필요', 'err'); return }
    const payload: ExternalSystemInput = {
      ...f, endpoints: eps,
      tags: tagText.split(',').map(t => t.trim()).filter(Boolean),
    }
    setSaving(true)
    try {
      if (initial) await externalSystemsApi.update(initial.id, payload)
      else await externalSystemsApi.create(payload)
      show(initial ? '수정됨' : '등록됨', 'ok'); onSaved(); onClose()
    } catch (e) { show((e as Error).message, 'err') }
    finally { setSaving(false) }
  }

  const lbl = { fontSize: 12, color: 'var(--muted-foreground)', display: 'block', marginBottom: 4 } as const
  const row = { marginBottom: 12 } as const

  return (
    <Modal title={initial ? `외부 시스템 수정 — ${initial.name}` : '외부 시스템 등록'} onClose={onClose} width={560}>
      <div style={row}>
        <label style={lbl}>이름</label>
        <input value={f.name} onChange={e => setF(s => ({ ...s, name: e.target.value }))}
               style={{ width: '100%' }} placeholder="예: 외부 가입자 DB" />
      </div>
      <div style={{ ...row, display: 'flex', gap: 12 }}>
        <div style={{ flex: 1 }}>
          <label style={lbl}>유형</label>
          <select value={f.type} onChange={e => setF(s => ({ ...s, type: e.target.value as ExternalSystemType }))}
                  style={{ width: '100%' }}>
            {TYPES.map(t => <option key={t} value={t}>{TYPE_LABEL[t]}</option>)}
          </select>
        </div>
        <div style={{ flex: 1, display: 'flex', alignItems: 'flex-end' }}>
          <label style={{ fontSize: 13 }}>
            <input type="checkbox" checked={f.enabled ?? true}
                   onChange={e => setF(s => ({ ...s, enabled: e.target.checked }))} /> 활성(형상 표시)
          </label>
        </div>
      </div>
      <div style={row}>
        <label style={lbl}>엔드포인트</label>
        {f.endpoints.map((e, i) => (
          <div key={i} style={{ display: 'flex', gap: 6, marginBottom: 4 }}>
            <input value={e.host} onChange={ev => setEp(i, { host: ev.target.value })} placeholder="host/IP" style={{ flex: 2 }} />
            <input type="number" value={e.port || ''} onChange={ev => setEp(i, { port: parseInt(ev.target.value) || 0 })} placeholder="port" style={{ flex: 1 }} />
            <input value={e.label || ''} onChange={ev => setEp(i, { label: ev.target.value })} placeholder="label(선택)" style={{ flex: 1 }} />
            <button className="btn btn--outline" onClick={() => rmEp(i)} disabled={f.endpoints.length <= 1}>✕</button>
          </div>
        ))}
        <button className="btn btn--outline" onClick={addEp} style={{ fontSize: 12 }}>+ 엔드포인트</button>
      </div>
      <div style={row}>
        <label style={lbl}>상태 점검(probe)</label>
        <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
          <select value={f.probe?.mode || 'none'} onChange={e => setF(s => ({ ...s, probe: { ...(s.probe || {}), mode: e.target.value as ProbeMode } }))}>
            {PROBE_MODES.map(m => <option key={m} value={m}>{m}</option>)}
          </select>
          <input value={f.probe?.host || ''} onChange={e => setF(s => ({ ...s, probe: { ...(s.probe || { mode: 'tcp' }), host: e.target.value } }))}
                 placeholder="host(미지정=ep1)" style={{ flex: 2 }} />
          <input type="number" value={f.probe?.port || ''} onChange={e => setF(s => ({ ...s, probe: { ...(s.probe || { mode: 'tcp' }), port: parseInt(e.target.value) || undefined } }))}
                 placeholder="port" style={{ flex: 1 }} />
          <input type="number" value={f.probe?.timeout ?? 2} onChange={e => setF(s => ({ ...s, probe: { ...(s.probe || { mode: 'tcp' }), timeout: parseFloat(e.target.value) || 2 } }))}
                 placeholder="timeout" style={{ width: 70 }} title="timeout(s)" />
        </div>
        <div style={{ fontSize: 11, color: 'var(--muted-foreground)', marginTop: 2 }}>tcp 만 구현 — http/icmp 는 미확인 처리.</div>
      </div>
      <div style={row}>
        <label style={lbl}>설명</label>
        <input value={f.description || ''} onChange={e => setF(s => ({ ...s, description: e.target.value }))} style={{ width: '100%' }} />
      </div>
      <div style={row}>
        <label style={lbl}>태그 (쉼표 구분)</label>
        <input value={tagText} onChange={e => setTagText(e.target.value)} style={{ width: '100%' }} placeholder="prod, db" />
      </div>
      <div style={{ display: 'flex', justifyContent: 'flex-end', gap: 8, marginTop: 8 }}>
        <button className="btn btn--outline" onClick={onClose}>취소</button>
        <button className="btn btn--primary" onClick={save} disabled={saving}>{saving ? '저장 중…' : '저장'}</button>
      </div>
    </Modal>
  )
}

export default function ExternalSystemsPage() {
  const { show } = useToast()
  const [items, setItems] = useState<ExternalSystem[]>([])
  const [status, setStatus] = useState<Map<number, ProbeResult>>(new Map())
  const [loading, setLoading] = useState(true)
  const [editing, setEditing] = useState<ExternalSystem | 'new' | null>(null)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const list = await externalSystemsApi.list()
      setItems(list)
      externalSystemsApi.status().then(items => setStatus(new Map(items.map(i => [i.id, i])))).catch(() => {})
    } catch (e) { show((e as Error).message, 'err') }
    finally { setLoading(false) }
  }, [show])

  useEffect(() => { load() }, [load])

  const remove = async (s: ExternalSystem) => {
    if (!window.confirm(`'${s.name}' 외부 시스템을 삭제할까요?`)) return
    try { await externalSystemsApi.delete(s.id); show('삭제됨', 'ok'); load() }
    catch (e) { show((e as Error).message, 'err') }
  }
  const probeNow = async (s: ExternalSystem) => {
    try { const r = await externalSystemsApi.probe(s.id); setStatus(m => new Map(m).set(s.id, r)) }
    catch (e) { show((e as Error).message, 'err') }
  }

  return (
    <div className="panel" style={{ padding: 16 }}>
      <div style={{ display: 'flex', alignItems: 'center', marginBottom: 12 }}>
        <div>
          <div style={{ fontWeight: 600, fontSize: 15 }}>외부 시스템 ({items.length})</div>
          <div style={{ fontSize: 12, color: 'var(--muted-foreground)' }}>외부 DB·모니터링·스토리지 등 등록 — 대시보드 시스템 형상에 표시.</div>
        </div>
        <button className="btn btn--primary" style={{ marginLeft: 'auto' }} onClick={() => setEditing('new')}>+ 외부 시스템 추가</button>
      </div>
      {loading ? <div style={{ padding: 20, color: 'var(--muted-foreground)' }}>불러오는 중…</div>
        : items.length === 0 ? <div style={{ padding: 20, color: 'var(--muted-foreground)' }}>등록된 외부 시스템이 없습니다.</div>
        : (
        <table className="data-table" style={{ fontSize: 13 }}>
          <thead><tr>
            <th>상태</th><th>이름</th><th>유형</th><th>엔드포인트</th><th>태그</th><th>활성</th><th>작업</th>
          </tr></thead>
          <tbody>
            {items.map(s => (
              <tr key={s.id}>
                <td>{(s.probe?.mode ?? 'none') !== 'none' ? <StatusDot st={status.get(s.id)} /> : <span style={{ color: 'var(--muted-foreground)' }}>—</span>}</td>
                <td><b>{s.name}</b>{s.description && <div style={{ fontSize: 11, color: 'var(--muted-foreground)' }}>{s.description}</div>}</td>
                <td><span style={{ fontSize: 11, padding: '1px 6px', border: '1px solid var(--border)', borderRadius: 3 }}>{TYPE_LABEL[s.type]}</span></td>
                <td>{(s.endpoints || []).map((e, i) => <code key={i} style={{ fontSize: 11, marginRight: 6 }}>{e.host}:{e.port}</code>)}</td>
                <td>{(s.tags || []).map(t => <span key={t} style={{ fontSize: 10, padding: '1px 5px', background: 'var(--secondary)', borderRadius: 8, marginRight: 3 }}>{t}</span>)}</td>
                <td>{s.enabled ? '✓' : '—'}</td>
                <td style={{ whiteSpace: 'nowrap' }}>
                  {(s.probe?.mode ?? 'none') !== 'none' &&
                    <button className="btn btn--outline" style={{ fontSize: 12, marginRight: 4 }} onClick={() => probeNow(s)}>점검</button>}
                  <button className="btn btn--outline" style={{ fontSize: 12, marginRight: 4 }} onClick={() => setEditing(s)}>편집</button>
                  <button className="btn btn--outline" style={{ fontSize: 12 }} onClick={() => remove(s)}>삭제</button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      {editing && (
        <EditModal initial={editing === 'new' ? null : editing}
                   onClose={() => setEditing(null)} onSaved={load} />
      )}
    </div>
  )
}
