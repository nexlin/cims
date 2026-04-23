import { useCallback, useEffect, useMemo, useState } from 'react'
import Modal from '../Modal'
import { useToast } from '../Toast'
import {
  deploymentApi,
  type Deployment, type ConfigTemplate, type ConfigTemplateField,
} from '../../api/deployment'
import ModuleConfigEditor, { type ModuleConfigEditorSource } from './ModuleConfigEditor'

type FieldValue = string | number | boolean | null
type Tab = 'scalar' | string   // 'scalar' = sections 탭, 나머지는 collection.key

export type ModuleConfigSource =
  | { type: 'deployment'; deployment: Deployment }
  | { type: 'module';     name: string; version?: string }

interface Props {
  source: ModuleConfigSource
  onClose: () => void
  onDone?: () => void | Promise<void>
}

/**
 * 모듈 설정 모달 — deployment 모드 (배포 > 서버) / module 모드 (테스트베드 > 모듈관리) 공용.
 *
 *  - deployment 모드: agent_deployment 레코드 대상. PUT → DB + update_config job.
 *  - module 모드:     Phase 1 로컬. PUT → build/dist/config.json (scalar) /
 *                     build/dist/{name}/config/*.jsonl (collection) + 로컬 PID SIGUSR1.
 */
export default function ModuleConfigModal({ source, onClose, onDone }: Props) {
  const { show } = useToast()
  const [loading, setLoading]     = useState(true)
  const [saving, setSaving]       = useState(false)
  const [template, setTemplate]   = useState<ConfigTemplate | null>(null)
  const [values, setValues]       = useState<Record<string, FieldValue>>({})
  const [initial, setInitial]     = useState<Record<string, FieldValue>>({})
  const [appliedAt, setAppliedAt] = useState<string | null>(null)
  const [tab, setTab]             = useState<Tab>('scalar')

  // 제목/식별자
  const title = source.type === 'deployment'
    ? `${source.deployment.package_name} v${source.deployment.package_version} — 설정`
    : `${source.name}${source.version ? ` v${source.version}` : ''} — 설정 (로컬)`

  // Editor 에 전달할 source — 매 렌더마다 새 객체를 만들면 Editor 가 useEffect 재실행 →
  // 편집 중이던 행이 서버 응답으로 덮어써진다. identity 고정 필수.
  const editorSource: ModuleConfigEditorSource = useMemo(
    () => source.type === 'deployment'
      ? { type: 'deployment', deploymentId: source.deployment.id }
      : { type: 'module',     moduleName: source.name },
    [source]
  )

  // source 분기 fetch
  const fetchConfig = useCallback(async () => {
    if (source.type === 'deployment') {
      const r = await deploymentApi.getDeploymentConfig(source.deployment.id)
      return {
        template: r.template,
        config:   r.config || {},
        appliedAt: r.config_applied_at,
      }
    }
    const r = await deploymentApi.getModuleConfig(source.name)
    return {
      template:  r.template,
      config:    r.current || {},
      appliedAt: null,
    }
  }, [source])

  // source 분기 save
  const saveConfig = useCallback(async (vals: Record<string, FieldValue>,
                                        changedKeys: Set<string>) => {
    if (source.type === 'deployment') {
      const r = await deploymentApi.putDeploymentConfig(source.deployment.id, vals, true)
      return { ok: true, message: r.job_id ? `저장됨. update_config job #${r.job_id}` : '저장됨' }
    }
    // module 모드: 변경된 키만 보냄 (not_owned_by_module 오류 회피 위해 모든 키 아닌 템플릿 소유 키만)
    const payload: Record<string, unknown> = {}
    for (const k of changedKeys) payload[k] = vals[k]
    const r = await deploymentApi.putModuleConfig(source.name, payload)
    return {
      ok: true,
      message: `${r.applied}개 저장${r.removed ? ` · ${r.removed}개 제거` : ''}${r.restart_required ? ' · 재시작 필요' : ''}`,
    }
  }, [source])

  const load = useCallback(async () => {
    try {
      const r = await fetchConfig()
      setTemplate(r.template)
      setAppliedAt(r.appliedAt)
      const base: Record<string, FieldValue> = {}
      if (r.template) {
        for (const s of r.template.sections) {
          for (const f of s.fields) {
            const existing = r.config[f.key]
            base[f.key] = existing !== undefined
              ? (existing as FieldValue)
              : defaultValue(f)
          }
        }
      }
      for (const [k, v] of Object.entries(r.config)) {
        if (!(k in base)) base[k] = v as FieldValue
      }
      setValues(base)
      setInitial(base)
    } catch (e) {
      show((e as Error).message, 'err')
    } finally {
      setLoading(false)
    }
  }, [fetchConfig, show])

  useEffect(() => { void load() }, [load])

  // 변경된 필드 추적
  const changed = useMemo(() => {
    const s = new Set<string>()
    for (const k of new Set([...Object.keys(values), ...Object.keys(initial)])) {
      if (values[k] !== initial[k]) s.add(k)
    }
    return s
  }, [values, initial])

  // 변경된 필드 중 restart:true 포함 여부
  const restartRequired = useMemo(() => {
    if (!template) return false
    for (const s of template.sections) {
      for (const f of s.fields) {
        if (changed.has(f.key) && (f.restart !== false)) return true
      }
    }
    return false
  }, [template, changed])

  async function save() {
    if (changed.size === 0) { show('변경된 항목 없음', 'err'); return }
    if (restartRequired) {
      if (!confirm('재기동이 필요한 설정이 포함되어 있습니다. 저장 후 수동으로 Restart 해야 적용됩니다.\n계속할까요?')) return
    }
    setSaving(true)
    try {
      const r = await saveConfig(values, changed)
      show(r.message, 'ok')
      if (onDone) await onDone()
      // deployment 모드는 저장 후 닫음 (기존 동작), module 모드는 재로드만 (UI 상 작업 계속)
      if (source.type === 'deployment') {
        onClose()
      } else {
        await load()
      }
    } catch (e) {
      show((e as Error).message, 'err')
    } finally {
      setSaving(false)
    }
  }

  return (
    <Modal title={title} onClose={onClose} fullscreen>
      <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
        {loading ? (
          <div className="empty" style={{ padding: 40 }}>로딩 중...</div>
        ) : !template ? (
          <div className="empty" style={{ padding: 20 }}>
            이 패키지에는 <code>config_template.json</code> 이 포함되어 있지 않습니다.
            <br/>설정 가능한 항목이 없습니다.
          </div>
        ) : (
          <>
            {/* 탭 (sticky top) */}
            <div style={{
              flex: '0 0 auto',
              display: 'flex', gap: 0, borderBottom: '1px solid #eee',
              padding: '0 20px', flexWrap: 'wrap', background: '#fafbfc',
            }}>
              <TabBtn active={tab === 'scalar'} onClick={() => setTab('scalar')}>
                설정 ({template.sections.reduce((n, s) => n + s.fields.length, 0)})
              </TabBtn>
              {(template.collections || []).map(c => (
                <TabBtn key={c.key} active={tab === c.key} onClick={() => setTab(c.key)}>
                  {c.title}
                </TabBtn>
              ))}
            </div>

            {/* 스크롤 영역 */}
            <div style={{ flex: 1, overflow: 'auto', padding: 20 }}>
              {tab === 'scalar' ? (
                <>
                  <div style={{ fontSize: 12, color: '#666', marginBottom: 12 }}>
                    <span style={{ marginRight: 12 }}>🔁 재기동 필요 · ⚡ 즉시 적용</span>
                    {appliedAt && <span>· 마지막 적용: {appliedAt}</span>}
                  </div>
                  {template.sections.map(sec => (
                    <SectionBlock key={sec.key} section={sec} values={values}
                      changed={changed}
                      onChange={(k, v) => setValues(p => ({ ...p, [k]: v }))} />
                  ))}
                  {restartRequired && (
                    <div style={{
                      marginTop: 12, padding: 10, background: '#fff3e0',
                      border: '1px solid #f39c12', borderRadius: 4, fontSize: 12,
                    }}>
                      ⚠ 변경된 항목 중 <b>재기동이 필요한</b> 항목이 있습니다. 저장 후
                      <b> Restart</b> 버튼으로 프로세스를 재기동해야 반영됩니다.
                    </div>
                  )}
                </>
              ) : (
                (() => {
                  const coll = (template.collections || []).find(c => c.key === tab)
                  if (!coll) return <div className="empty">collection 을 찾을 수 없음</div>
                  return <ModuleConfigEditor source={editorSource} collection={coll} />
                })()
              )}
            </div>
          </>
        )}

        {/* sticky footer */}
        <div className="modal-footer" style={{ flex: '0 0 auto', marginTop: 0 }}>
          <button className="btn btn--outline" onClick={onClose}>닫기</button>
          {template && tab === 'scalar' && (
            <button className="btn btn--primary" onClick={save}
              disabled={saving || changed.size === 0}>
              {saving ? '저장 중...' : `저장 (${changed.size} 변경)`}
            </button>
          )}
        </div>
      </div>
    </Modal>
  )
}

function TabBtn({ active, children, onClick }: {
  active: boolean; children: React.ReactNode; onClick: () => void
}) {
  return (
    <button onClick={onClick}
      style={{
        padding: '8px 16px', border: 'none',
        background: active ? '#fff' : 'transparent',
        borderBottom: `2px solid ${active ? '#3498db' : 'transparent'}`,
        fontWeight: active ? 600 : 400, cursor: 'pointer', fontSize: 13,
      }}>
      {children}
    </button>
  )
}

function SectionBlock({ section, values, changed, onChange }: {
  section: { key: string; title: string; description?: string; fields: ConfigTemplateField[] }
  values: Record<string, FieldValue>
  changed: Set<string>
  onChange: (key: string, v: FieldValue) => void
}) {
  const [collapsed, setCollapsed] = useState(false)
  return (
    <div style={{
      border: '1px solid #e5e5e5', borderRadius: 6, marginBottom: 12,
      background: '#fff',
    }}>
      <div onClick={() => setCollapsed(c => !c)}
        style={{
          padding: '10px 14px', cursor: 'pointer', userSelect: 'none',
          display: 'flex', alignItems: 'baseline', gap: 8,
          borderBottom: collapsed ? 'none' : '1px solid #eee',
          background: '#fafafa',
        }}>
        <span style={{ color: '#999', fontSize: 11 }}>{collapsed ? '▸' : '▾'}</span>
        <b>{section.title}</b>
        {section.description && (
          <span style={{ fontSize: 11, color: '#888' }}>— {section.description}</span>
        )}
      </div>
      {!collapsed && (
        <div style={{ padding: 12, display: 'grid',
                      gridTemplateColumns: '200px 1fr', rowGap: 10, columnGap: 10,
                      alignItems: 'start' }}>
          {section.fields.map(f => (
            <FieldRow key={f.key} field={f}
              value={values[f.key]}
              isChanged={changed.has(f.key)}
              onChange={v => onChange(f.key, v)} />
          ))}
        </div>
      )}
    </div>
  )
}

function FieldRow({ field, value, isChanged, onChange }: {
  field: ConfigTemplateField
  value: FieldValue
  isChanged: boolean
  onChange: (v: FieldValue) => void
}) {
  const needsRestart = field.restart !== false
  return (
    <>
      <label style={{
        paddingTop: 6, fontSize: 13,
        color: isChanged ? '#2980b9' : undefined,
      }}>
        <span>{field.label}</span>
        <span style={{ marginLeft: 6 }} title={needsRestart ? '재기동 필요' : '즉시 적용'}>
          {needsRestart ? '🔁' : '⚡'}
        </span>
        {field.required && <span style={{ color: '#e74c3c', marginLeft: 4 }}>*</span>}
        {isChanged && <span style={{ marginLeft: 6, color: '#2980b9', fontSize: 11 }}>●</span>}
      </label>
      <div>
        {renderInput(field, value, onChange)}
        {field.help && (
          <div style={{ fontSize: 11, color: '#888', marginTop: 3 }}>{field.help}</div>
        )}
        {!needsRestart && field.reload_hint && (
          <div style={{ fontSize: 11, color: '#27ae60', marginTop: 3 }}>⚡ {field.reload_hint}</div>
        )}
      </div>
    </>
  )
}

function renderInput(f: ConfigTemplateField, value: FieldValue, onChange: (v: FieldValue) => void) {
  if (f.type === 'bool') {
    return (
      <input type="checkbox" checked={!!value}
        onChange={e => onChange(e.target.checked)} />
    )
  }
  if (f.type === 'enum') {
    return (
      <select className="form-input" value={(value as string) ?? ''}
        onChange={e => onChange(e.target.value)}>
        {(f.options || []).map(o => <option key={o} value={o}>{o}</option>)}
      </select>
    )
  }
  if (f.type === 'int') {
    return (
      <input className="form-input" type="number"
        min={f.min} max={f.max}
        value={value === null || value === undefined ? '' : Number(value)}
        onChange={e => {
          const s = e.target.value
          onChange(s === '' ? null : Number(s))
        }} />
    )
  }
  if (f.type === 'password') {
    return (
      <input className="form-input" type="password"
        value={(value as string) ?? ''}
        onChange={e => onChange(e.target.value)} />
    )
  }
  // string / path
  return (
    <input className="form-input" type="text"
      value={(value as string) ?? ''}
      onChange={e => onChange(e.target.value)} />
  )
}

function defaultValue(f: ConfigTemplateField): FieldValue {
  if (f.default !== undefined && f.default !== null) {
    return f.default as FieldValue
  }
  switch (f.type) {
    case 'bool': return false
    case 'int':  return 0
    default:     return ''
  }
}
