// 서비스 정의(Service Descriptor) 관리 — OAM 플랫폼화 5-6.
// 서비스 pack 이 등록한 모듈(name/port/proto/controllable) + alert_rules 를 조회/편집.
// 코어(ha/build/service_control/alert sweeper)가 이 descriptor 를 읽어 동작.
import { useState, useEffect, useCallback } from 'react'
import Modal from '../components/Modal'
import { useToast } from '../components/Toast'
import { serviceDescriptorsApi, type ServiceDescriptor } from '../api/serviceDescriptors'
import type { DataSourceSpec } from '../widgets/shapes/dataSourceSpec'

const DS_TEMPLATE = `{
  "id": "cims.svc.example",
  "label": "예시 소스",
  "shapes": ["time-bar"],
  "endpoint": "/stats/messages/sip",
  "query": ["date"],
  "map": {
    "time-bar": { "from": "buckets", "label": ["hour"], "value": "count" }
  }
}`

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
        modules[].name/port/proto/controllable · alert_rules[].type/severity/check/threshold · data_sources[].id/label/shapes/endpoint/map
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

// 데이터 소스 1건 편집기 — descriptor 의 data_sources[index] 를 JSON 으로 추가/수정.
// (매핑 스펙이 shape별 중첩이라 단일 소스 JSON 편집이 폼보다 실용적.) 저장 시 descriptor 통째 PUT.
function DataSourceEditor({ svc, index, onClose, onSaved }: {
  svc: ServiceDescriptor; index: number | null; onClose: () => void; onSaved: () => void
}) {
  const { show } = useToast()
  const existing = index != null ? (svc.data_sources || [])[index] : null
  const [text, setText] = useState(existing ? JSON.stringify(existing, null, 2) : DS_TEMPLATE)
  const [saving, setSaving] = useState(false)

  const save = async () => {
    let src: DataSourceSpec
    try { src = JSON.parse(text) }
    catch (e) { show(`JSON 파싱 오류: ${(e as Error).message}`, 'err'); return }
    if (!src.id || !Array.isArray(src.shapes) || !src.shapes.length || !src.endpoint) {
      show('id · shapes[] · endpoint 가 필요합니다', 'err'); return
    }
    const sources = [...(svc.data_sources || [])]
    // id 중복 체크 (자기 자신 제외).
    const dupAt = sources.findIndex(s => s.id === src.id)
    if (dupAt >= 0 && dupAt !== index) { show(`id 중복: ${src.id}`, 'err'); return }
    if (index != null) sources[index] = src; else sources.push(src)
    setSaving(true)
    try {
      await serviceDescriptorsApi.put(svc.id, { ...svc, data_sources: sources })
      show('데이터 소스 저장됨', 'ok'); onSaved(); onClose()
    } catch (e) { show((e as Error).message, 'err') }
    finally { setSaving(false) }
  }

  return (
    <Modal title={index != null ? `데이터 소스 편집 — ${existing?.id}` : '데이터 소스 추가'} onClose={onClose} width={640}>
      <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 8, lineHeight: 1.6 }}>
        <b>shapes</b>: time-bar · kpi · distribution · table (지원 shape) · <b>endpoint</b> + <b>query</b>(date/granularity)<br />
        <b>map</b>[shape]: <code>from</code>(배열 중첩경로) · <code>label</code>(필드후보 배열) · <code>value</code> · <code>fromObject</code>(dict→행/항목) · <code>totalPath</code> · <code>path</code>(KPI) · <code>format</code>(duration)
      </div>
      <textarea value={text} onChange={e => setText(e.target.value)} spellCheck={false}
        style={{ width: '100%', height: 320, fontFamily: 'Consolas, monospace', fontSize: 12,
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
  const [dsEdit, setDsEdit] = useState<{ svc: ServiceDescriptor; index: number | null } | null>(null)

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

  const removeDataSource = async (svc: ServiceDescriptor, index: number) => {
    const ds = (svc.data_sources || [])[index]
    if (!confirm(`데이터 소스 '${ds?.id}' 를 삭제할까요?`)) return
    const sources = (svc.data_sources || []).filter((_, i) => i !== index)
    try { await serviceDescriptorsApi.put(svc.id, { ...svc, data_sources: sources }); show('삭제됨', 'ok'); load() }
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

          {/* 데이터 소스 — shape 위젯(차트/표/KPI/분포)이 선택하는 소스 카탈로그 */}
          <div style={{ padding: '0 16px 14px' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
              <span style={{ fontSize: 12, fontWeight: 600, color: 'var(--text-muted)' }}>
                데이터 소스 ({(svc.data_sources || []).length})
              </span>
              <button className="btn btn--sm btn--outline" style={{ marginLeft: 'auto' }}
                      onClick={() => setDsEdit({ svc, index: null })}>＋ 데이터 소스</button>
            </div>
            {(svc.data_sources || []).length === 0 ? (
              <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>
                — (shape 위젯에 노출할 차트/표/KPI/분포 데이터 소스를 등록)
              </div>
            ) : (
              <table className="data-table" style={{ margin: 0 }}>
                <thead><tr><th>id</th><th>이름</th><th>shapes</th><th>endpoint</th><th style={{ width: 90 }}></th></tr></thead>
                <tbody>
                  {(svc.data_sources || []).map((d, i) => (
                    <tr key={d.id}>
                      <td><code style={{ fontSize: 11 }}>{d.id}</code></td>
                      <td>{d.label}</td>
                      <td style={{ fontSize: 11 }}>{(d.shapes || []).join(', ')}</td>
                      <td style={{ fontFamily: 'monospace', fontSize: 11 }}>{d.endpoint}</td>
                      <td style={{ display: 'flex', gap: 4 }}>
                        <button className="btn btn--sm btn--outline" onClick={() => setDsEdit({ svc, index: i })}>편집</button>
                        <button className="btn btn--sm btn--outline" style={{ color: 'var(--danger)' }}
                                onClick={() => removeDataSource(svc, i)}>✕</button>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            )}
          </div>
        </div>
      ))}

      {editing && (
        <JsonEditor title={editing.title} initial={editing.json}
                    onClose={() => setEditing(null)} onSaved={load} />
      )}
      {dsEdit && (
        <DataSourceEditor svc={dsEdit.svc} index={dsEdit.index}
                          onClose={() => setDsEdit(null)} onSaved={load} />
      )}
    </div>
  )
}
