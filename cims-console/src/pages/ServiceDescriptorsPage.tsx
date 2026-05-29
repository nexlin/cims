// 서비스 정의(Service Descriptor) 관리 — OAM 플랫폼화 5-6.
// 서비스 pack 이 등록한 모듈(name/port/proto/controllable) + alert_rules 를 조회/편집.
// 코어(ha/build/service_control/alert sweeper)가 이 descriptor 를 읽어 동작.
import { useState, useEffect, useCallback } from 'react'
import Modal from '../components/Modal'
import { useToast } from '../components/Toast'
import { serviceDescriptorsApi, type ServiceDescriptor } from '../api/serviceDescriptors'

const NEW_TEMPLATE = `{
  "id": "myservice",
  "label": "My Service",
  "modules": [
    { "name": "mymod", "port": 8080, "proto": "tcp", "controllable": true }
  ],
  "alert_rules": []
}`

function JsonEditor({ initial, title, onClose, onSaved }: {
  initial: string; title: string; onClose: () => void; onSaved: () => void
}) {
  const { show } = useToast()
  const [text, setText] = useState(initial)
  const [saving, setSaving] = useState(false)

  const save = async () => {
    let doc: ServiceDescriptor
    try {
      doc = JSON.parse(text)
    } catch (e) { show(`JSON 파싱 오류: ${(e as Error).message}`, 'err'); return }
    if (!doc.id || !Array.isArray(doc.modules)) {
      show('id 와 modules[] 가 필요합니다', 'err'); return
    }
    setSaving(true)
    try {
      await serviceDescriptorsApi.put(doc.id, doc)
      show('서비스 정의 저장됨', 'ok'); onSaved(); onClose()
    } catch (e) { show((e as Error).message, 'err') }
    finally { setSaving(false) }
  }

  return (
    <Modal title={title} onClose={onClose} width={640}>
      <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 8 }}>
        modules[].name/port/proto/controllable · alert_rules[].type/severity/check(process_down·db_down·rtp_pct_gte)/threshold
      </div>
      <textarea value={text} onChange={e => setText(e.target.value)} spellCheck={false}
        style={{ width: '100%', height: 360, fontFamily: 'Consolas, monospace', fontSize: 12,
                 padding: 10, border: '1px solid var(--border)', borderRadius: 'var(--radius)',
                 background: 'var(--bg-soft)', color: 'var(--text)', resize: 'vertical' }} />
      <div className="modal-footer">
        <button className="btn btn--outline" onClick={onClose} disabled={saving}>취소</button>
        <button className="btn btn--primary" onClick={save} disabled={saving}>저장</button>
      </div>
    </Modal>
  )
}

export default function ServiceDescriptorsPage() {
  const { show } = useToast()
  const [list, setList] = useState<ServiceDescriptor[]>([])
  const [loading, setLoading] = useState(false)
  const [editing, setEditing] = useState<{ title: string; json: string } | null>(null)

  const load = useCallback(async () => {
    setLoading(true)
    try { setList((await serviceDescriptorsApi.list()).services || []) }
    catch (e) { show((e as Error).message, 'err') }
    finally { setLoading(false) }
  }, [show])
  useEffect(() => { load() }, [load])

  const remove = async (id: string) => {
    if (!confirm(`서비스 정의 '${id}' 를 삭제할까요?\n(코어가 이 서비스의 모듈/알람 규칙을 더 이상 인식하지 않습니다)`)) return
    try { await serviceDescriptorsApi.remove(id); show('삭제됨', 'ok'); load() }
    catch (e) { show((e as Error).message, 'err') }
  }

  return (
    <div className="page">
      <div className="toolbar" style={{ borderRadius: 'var(--radius)', border: '1px solid var(--border)' }}>
        <div style={{ fontWeight: 600 }}>서비스 정의 ({list.length})</div>
        <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>
          서비스 pack 이 등록한 모듈/알람 규칙. 코어(HA·빌드·제어·알람)가 이 정의를 읽어 동작합니다.
        </span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
          <button className="btn btn--sm btn--ghost" onClick={load}>↻</button>
          <button className="btn btn--sm btn--primary"
                  onClick={() => setEditing({ title: '새 서비스 정의', json: NEW_TEMPLATE })}>＋ 서비스 추가</button>
        </div>
      </div>

      {loading && list.length === 0 ? (
        <div className="empty">로딩 중…</div>
      ) : list.length === 0 ? (
        <div className="empty">등록된 서비스 정의 없음</div>
      ) : list.map(svc => (
        <div key={svc.id} className="panel" style={{ padding: 0 }}>
          <div style={{ padding: '12px 16px', borderBottom: '1px solid var(--border)',
                        display: 'flex', alignItems: 'center', gap: 8 }}>
            <span style={{ fontWeight: 600 }}>{svc.label || svc.id}</span>
            <code style={{ fontSize: 11, color: 'var(--text-muted)' }}>{svc.id}</code>
            <div style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
              <button className="btn btn--sm btn--outline"
                      onClick={() => setEditing({ title: `서비스 정의 편집 — ${svc.id}`, json: JSON.stringify(svc, null, 2) })}>
                JSON 편집
              </button>
              <button className="btn btn--sm btn--outline" style={{ color: 'var(--danger)' }}
                      onClick={() => remove(svc.id)}>삭제</button>
            </div>
          </div>
          <div style={{ padding: '12px 16px', display: 'flex', gap: 24, flexWrap: 'wrap' }}>
            <div style={{ flex: 1, minWidth: 280 }}>
              <div style={{ fontSize: 12, fontWeight: 600, color: 'var(--text-muted)', marginBottom: 6 }}>
                모듈 ({svc.modules.length})
              </div>
              <table className="data-table" style={{ margin: 0 }}>
                <thead><tr><th>이름</th><th>포트</th><th>proto</th><th>제어</th></tr></thead>
                <tbody>
                  {svc.modules.map(m => (
                    <tr key={m.name}>
                      <td><b>{m.name}</b></td>
                      <td>{m.port ?? '—'}</td>
                      <td>{m.proto ?? '—'}</td>
                      <td>{m.controllable ? '●' : ''}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
            <div style={{ flex: 1, minWidth: 280 }}>
              <div style={{ fontSize: 12, fontWeight: 600, color: 'var(--text-muted)', marginBottom: 6 }}>
                알람 규칙 ({(svc.alert_rules || []).length})
              </div>
              {(svc.alert_rules || []).length === 0 ? (
                <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>—</div>
              ) : (
                <table className="data-table" style={{ margin: 0 }}>
                  <thead><tr><th>유형</th><th>심각도</th><th>check</th><th>threshold</th></tr></thead>
                  <tbody>
                    {(svc.alert_rules || []).map(r => (
                      <tr key={r.type}>
                        <td>{r.type}</td>
                        <td><span className={`badge ${r.severity === 'critical' ? 'badge--red' : 'badge--yellow'}`}>{r.severity}</span></td>
                        <td style={{ fontFamily: 'monospace', fontSize: 12 }}>{r.check}</td>
                        <td>{r.threshold != null ? `${r.threshold}${r.unit || ''}` : '—'}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              )}
            </div>
          </div>
        </div>
      ))}

      {editing && (
        <JsonEditor title={editing.title} initial={editing.json}
                    onClose={() => setEditing(null)} onSaved={load} />
      )}
    </div>
  )
}
