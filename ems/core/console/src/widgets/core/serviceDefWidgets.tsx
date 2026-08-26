// 서비스 정의(Service Descriptor) 화면 위젯 — OAM 플랫폼화 5-6.
// 서비스 pack 이 등록한 모듈(name/port/proto/controllable) · 알람 규칙 · 데이터 소스를 조회/편집.
// 코어(ha/build/service_control/alert sweeper, shape 위젯)가 이 descriptor 를 읽어 동작한다.
//
// 세 컬렉션은 모두 **선택된 서비스에 종속**이라, 서비스 선택을 페이지 파라미터 `svc` 로 외부화하고
// 각 위젯이 그것을 읽는다(알람 심각도↔목록과 같은 구조). 덕분에 서비스마다 카드를 반복하지 않고
// "하나를 골라 그 서비스의 3종을 본다"가 되어 위젯으로 떼어낼 수 있다.
//
// 편집 경로는 **항목 단위 인라인 CRUD 로 통일**한다 — 각 위젯이 자기 컬렉션의 [＋ 추가]/[편집]/[✕] 를
// 온전히 담당한다. 예전에는 데이터 소스만 인라인이고 모듈·알람 규칙은 서비스 편집 모달 안에 있어
// 같은 성격인데 조작 방법이 갈렸다.
import { useState } from 'react'
import { serviceDescriptorsApi, type ServiceDescriptor } from '../../api/serviceDescriptors'
import { ServiceForm, ModuleForm, AlertRuleForm, DataSourceForm } from '../../pages/descriptors/forms'
import Modal from '../../components/Modal'
import { useToast } from '../../components/Toast'
import { makeSharedByKey } from '../sharedFetch'
import { usePageControl, usePageParam } from '../pageParams'
import type { WidgetDef } from '../types'

// 목록은 조건이 없어 키가 하나 — 위젯이 몇 개든 조회는 1회.
const useDescriptorsRaw = makeSharedByKey(() => serviceDescriptorsApi.list())

// 선택된 서비스 — 파라미터가 비어 있거나 사라진 id 면 첫 서비스로 떨어진다.
function useSelectedService() {
  const { data, loading, error, reload } = useDescriptorsRaw('all')
  const [svcId] = usePageParam('svc')
  const list: ServiceDescriptor[] = data?.services ?? []
  const svc = list.find(s => s.id === svcId) ?? list[0] ?? null
  return { list, svc, loading, error, reload }
}

function Header({ title, count, action, loading, error }: {
  title: string; count?: number; action?: React.ReactNode; loading?: boolean; error?: string
}) {
  return (
    <div style={{ padding: '10px 16px', borderBottom: '1px solid var(--border)', flex: 'none',
                  display: 'flex', alignItems: 'center', gap: 8 }}>
      <span style={{ fontWeight: 600, fontSize: 14 }}>
        {title}{count != null && ` (${count})`}
      </span>
      {loading && <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>· 갱신 중…</span>}
      {error && <span style={{ fontSize: 11, color: 'var(--danger)' }}>· 조회 실패</span>}
      {action && <span style={{ marginLeft: 'auto' }}>{action}</span>}
    </div>
  )
}

function Empty({ text }: { text: string }) {
  return <div className="empty" style={{ fontSize: 12 }}>{text}</div>
}

// 행마다 [수정][삭제] 가 늘 떠 있으면 표가 산만하다 — 헤더의 [편집] 토글을 켰을 때만 보인다.
function EditToggle({ on, disabled, onToggle }: { on: boolean; disabled?: boolean; onToggle: () => void }) {
  return (
    <button className={`btn btn--sm ${on ? 'btn--primary' : 'btn--outline'}`} disabled={disabled}
            title={on ? '수정·삭제 버튼 숨기기' : '행별 수정·삭제 보기'} onClick={onToggle}>편집</button>
  )
}

function RowActions({ onEdit, onRemove }: { onEdit: () => void; onRemove: () => void }) {
  return (
    <td style={{ display: 'flex', gap: 4 }}>
      <button className="btn btn--sm btn--outline" onClick={onEdit}>수정</button>
      <button className="btn btn--sm btn--outline" style={{ color: 'var(--danger)' }}
              onClick={onRemove}>삭제</button>
    </td>
  )
}

// ── 서비스 선택 (컨트롤) — 드롭다운 + 서비스 추가 ─────────────────────────
function ServicePicker() {
  usePageControl('svc')
  const { list, svc, loading, reload } = useSelectedService()
  const [, setSvcId] = usePageParam('svc')
  const [adding, setAdding] = useState(false)
  return (
    <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
      <span style={{ fontSize: 13, color: 'var(--text-muted)' }}>서비스</span>
      <select className="form-input" style={{ width: 200, fontSize: 13 }}
              value={svc?.id ?? ''} onChange={e => setSvcId(e.target.value)}>
        {list.length === 0 && <option value="">{loading ? '로딩 중…' : '(등록된 서비스 없음)'}</option>}
        {list.map(s => <option key={s.id} value={s.id}>{s.label || s.id}</option>)}
      </select>
      <button className="btn btn--sm btn--primary" style={{ marginLeft: 'auto' }}
              onClick={() => setAdding(true)}>＋ 서비스 추가</button>
      {adding && <ServiceForm initial={null} onClose={() => setAdding(false)}
                              onSaved={reload} />}
    </div>
  )
}

// ── 서비스 (이름 + JSON/삭제) ─────────────────────────────────────────────
function ServiceHeaderBlock() {
  const { show } = useToast()
  const { svc, loading, error, reload } = useSelectedService()
  const [, setSvcId] = usePageParam('svc')
  const [json, setJson] = useState<string | null>(null)

  const remove = async () => {
    if (!svc) return
    if (!confirm(`서비스 정의 '${svc.id}' 를 삭제할까요?\n(코어가 이 서비스의 모듈/알람 규칙을 더 이상 인식하지 않습니다)`)) return
    try { await serviceDescriptorsApi.remove(svc.id); show('삭제됨', 'ok'); setSvcId(''); reload() }
    catch (e) { show((e as Error).message, 'err') }
  }

  return (
    <div className="panel" style={{ padding: 0, display: 'flex', flexDirection: 'column' }}>
      <div style={{ padding: '12px 16px', display: 'flex', alignItems: 'center', gap: 8 }}>
        {!svc ? <span style={{ color: 'var(--text-muted)', fontSize: 13 }}>
          {loading ? '로딩 중…' : error ? '조회 실패' : '서비스를 선택하세요'}</span> : (
          <>
            <span style={{ fontWeight: 600 }}>{svc.label || svc.id}</span>
            <code style={{ fontSize: 11, color: 'var(--text-muted)' }}>{svc.id}</code>
            <span style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
              <button className="btn btn--sm btn--outline" title="고급 — 전체 JSON 직접 편집"
                      onClick={() => setJson(JSON.stringify(svc, null, 2))}>JSON</button>
              <button className="btn btn--sm btn--outline" style={{ color: 'var(--danger)' }}
                      onClick={remove}>삭제</button>
            </span>
          </>
        )}
      </div>
      {json != null && svc && (
        <JsonEditor title={`JSON(고급) — ${svc.id}`} initial={json}
                    onClose={() => setJson(null)} onSaved={reload} />
      )}
    </div>
  )
}

// 고급(JSON) 편집 — 폼으로 다루기 어려운 케이스용 fallback.
function JsonEditor({ initial, title, onClose, onSaved }: {
  initial: string; title: string; onClose: () => void; onSaved: () => void
}) {
  const { show } = useToast()
  const [text, setText] = useState(initial)
  const [saving, setSaving] = useState(false)
  const save = async () => {
    let doc: ServiceDescriptor
    try { doc = JSON.parse(text) }
    catch (e) { show(`JSON 파싱 오류: ${(e as Error).message}`, 'err'); return }
    if (!doc.id || !Array.isArray(doc.modules)) { show('id 와 modules[] 가 필요합니다', 'err'); return }
    setSaving(true)
    try { await serviceDescriptorsApi.put(doc.id, doc); show('서비스 정의 저장됨', 'ok'); onSaved(); onClose() }
    catch (e) { show((e as Error).message, 'err') }
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

// ── 모듈 ─────────────────────────────────────────────────────────────────
function ModulesBlock() {
  const { show } = useToast()
  const { svc, loading, error, reload } = useSelectedService()
  const [edit, setEdit] = useState<{ index: number | null } | null>(null)
  const [editMode, setEditMode] = useState(false)
  const mods = svc?.modules ?? []

  const remove = async (i: number) => {
    if (!svc) return
    if (!confirm(`모듈 '${mods[i]?.name}' 를 삭제할까요?`)) return
    try {
      await serviceDescriptorsApi.put(svc.id, { ...svc, modules: mods.filter((_, k) => k !== i) })
      show('삭제됨', 'ok'); reload()
    } catch (e) { show((e as Error).message, 'err') }
  }

  return (
    <div className="panel" style={{ display: 'flex', flexDirection: 'column', minHeight: 0 }}>
      <Header title="모듈" count={mods.length} loading={loading} error={error}
              action={<span style={{ display: 'flex', gap: 6 }}>
                <EditToggle on={editMode} disabled={!svc} onToggle={() => setEditMode(v => !v)} />
                <button className="btn btn--sm btn--outline" disabled={!svc}
                        onClick={() => setEdit({ index: null })}>＋ 모듈</button>
              </span>} />
      <div className="scroll-fill">
        {mods.length === 0 ? <Empty text={svc ? '등록된 모듈 없음' : '서비스를 선택하세요'} /> : (
          <table className="data-table" style={{ margin: 0 }}>
            <thead><tr><th>이름</th><th style={{ width: 70 }}>포트</th><th style={{ width: 60 }}>proto</th>
              <th style={{ width: 64 }}>제어</th>
              {editMode && <th style={{ width: 118 }} />}</tr></thead>
            <tbody>
              {mods.map((m, i) => (
                <tr key={m.name}>
                  <td><b>{m.name}</b></td>
                  <td>{m.port ?? '—'}</td>
                  <td>{m.proto ?? '—'}</td>
                  <td>{m.controllable ? '●' : ''}</td>
                  {editMode && <RowActions onEdit={() => setEdit({ index: i })} onRemove={() => remove(i)} />}
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
      {edit && svc && (
        <ModuleForm svc={svc} index={edit.index} onClose={() => setEdit(null)} onSaved={reload} />
      )}
    </div>
  )
}

// ── 알람 규칙 ────────────────────────────────────────────────────────────
function AlertRulesBlock() {
  const { show } = useToast()
  const { svc, loading, error, reload } = useSelectedService()
  const [edit, setEdit] = useState<{ index: number | null } | null>(null)
  const [editMode, setEditMode] = useState(false)
  const rules = svc?.alert_rules ?? []

  const remove = async (i: number) => {
    if (!svc) return
    if (!confirm(`알람 규칙 '${rules[i]?.code || rules[i]?.type}' 를 삭제할까요?`)) return
    try {
      await serviceDescriptorsApi.put(svc.id, { ...svc, alert_rules: rules.filter((_, k) => k !== i) })
      show('삭제됨', 'ok'); reload()
    } catch (e) { show((e as Error).message, 'err') }
  }

  return (
    <div className="panel" style={{ display: 'flex', flexDirection: 'column', minHeight: 0 }}>
      <Header title="알람 규칙" count={rules.length} loading={loading} error={error}
              action={<span style={{ display: 'flex', gap: 6 }}>
                <EditToggle on={editMode} disabled={!svc} onToggle={() => setEditMode(v => !v)} />
                <button className="btn btn--sm btn--outline" disabled={!svc}
                        onClick={() => setEdit({ index: null })}>＋ 규칙</button>
              </span>} />
      <div className="scroll-fill">
        {rules.length === 0 ? <Empty text={svc ? '등록된 알람 규칙 없음' : '서비스를 선택하세요'} /> : (
          <table className="data-table" style={{ margin: 0 }}>
            <thead><tr><th style={{ width: 100 }}>코드</th><th>클래스</th><th style={{ width: 90 }}>심각도</th>
              <th>소스</th>{editMode && <th style={{ width: 118 }} />}</tr></thead>
            <tbody>
              {rules.map((r, i) => {
                const sev = r.perceived_severity || r.severity || 'warning'
                const cls = sev === 'critical' || sev === 'major' ? 'badge--red'
                  : sev === 'indeterminate' ? 'badge--blue' : 'badge--yellow'
                return (
                  <tr key={`${r.code}-${r.mo_instance || r.target || i}`}
                      title={[r.effect && `영향: ${r.effect}`,
                              r.recommended_action && `조치: ${r.recommended_action}`].filter(Boolean).join('\n')}>
                    <td style={{ fontFamily: 'monospace', fontSize: 11 }}>{r.code || '—'}</td>
                    <td>{r.type}</td>
                    <td><span className={`badge ${cls}`}>{sev}</span></td>
                    <td style={{ fontFamily: 'monospace', fontSize: 11 }}>
                      {r.mo_instance || (r.target ? `(관측 신원)/${r.target}` : '—')}</td>
                    {editMode && <RowActions onEdit={() => setEdit({ index: i })} onRemove={() => remove(i)} />}
                  </tr>
                )
              })}
            </tbody>
          </table>
        )}
      </div>
      {edit && svc && (
        <AlertRuleForm svc={svc} index={edit.index} onClose={() => setEdit(null)} onSaved={reload} />
      )}
    </div>
  )
}

// ── 데이터 소스 (shape 위젯이 고르는 소스 카탈로그) ──────────────────────
function DataSourcesBlock() {
  const { show } = useToast()
  const { svc, loading, error, reload } = useSelectedService()
  const [edit, setEdit] = useState<{ index: number | null } | null>(null)
  const [editMode, setEditMode] = useState(false)
  const sources = svc?.data_sources ?? []

  const remove = async (i: number) => {
    if (!svc) return
    if (!confirm(`데이터 소스 '${sources[i]?.id}' 를 삭제할까요?`)) return
    try {
      await serviceDescriptorsApi.put(svc.id, { ...svc, data_sources: sources.filter((_, k) => k !== i) })
      show('삭제됨', 'ok'); reload()
    } catch (e) { show((e as Error).message, 'err') }
  }

  return (
    <div className="panel" style={{ display: 'flex', flexDirection: 'column', minHeight: 0 }}>
      <Header title="데이터 소스" count={sources.length} loading={loading} error={error}
              action={<span style={{ display: 'flex', gap: 6 }}>
                <EditToggle on={editMode} disabled={!svc} onToggle={() => setEditMode(v => !v)} />
                <button className="btn btn--sm btn--outline" disabled={!svc}
                        onClick={() => setEdit({ index: null })}>＋ 데이터 소스</button>
              </span>} />
      <div className="scroll-fill">
        {sources.length === 0 ? (
          <Empty text={svc ? 'shape 위젯에 노출할 차트/표/지표/분포 소스를 등록하세요' : '서비스를 선택하세요'} />
        ) : (
          <table className="data-table" style={{ margin: 0 }}>
            <thead><tr><th style={{ width: 130 }}>id</th><th>이름</th><th style={{ width: 150 }}>shapes</th>
              <th>endpoint</th>{editMode && <th style={{ width: 118 }} />}</tr></thead>
            <tbody>
              {sources.map((d, i) => (
                <tr key={d.id}>
                  <td><code style={{ fontSize: 11 }}>{d.id}</code></td>
                  <td>{d.label}</td>
                  <td style={{ fontSize: 11 }}>{(d.shapes || []).join(', ')}</td>
                  <td style={{ fontFamily: 'monospace', fontSize: 11 }}>{d.endpoint}</td>
                  {editMode && <RowActions onEdit={() => setEdit({ index: i })} onRemove={() => remove(i)} />}
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
      {edit && svc && (
        <DataSourceForm svc={svc} index={edit.index} onClose={() => setEdit(null)} onSaved={reload} />
      )}
    </div>
  )
}

export const servicePickerWidget: WidgetDef = {
  id: 'core.service-picker', title: '서비스 선택 (정의)', category: 'control',
  component: ServicePicker, defaultSize: { w: 12, h: 4 }, adminOnly: true,
}

const def = (id: string, title: string, component: WidgetDef['component'], h: number): WidgetDef =>
  ({ id, title, category: 'service', component, defaultSize: { w: 12, h }, adminOnly: true })

export const SERVICE_DEF_WIDGETS: WidgetDef[] = [
  servicePickerWidget,
  def('core.service-def.header', '서비스 정의 — 이름/JSON/삭제', ServiceHeaderBlock, 5),
  def('core.service-def.modules', '서비스 정의 — 모듈', ModulesBlock, 16),
  def('core.service-def.rules', '서비스 정의 — 알람 규칙', AlertRulesBlock, 20),
  def('core.service-def.sources', '서비스 정의 — 데이터 소스', DataSourcesBlock, 20),
]
