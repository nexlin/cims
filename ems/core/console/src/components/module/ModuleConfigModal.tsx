import { useCallback, useEffect, useMemo, useState } from 'react'
import Modal from '../Modal'
import { useToast } from '../Toast'
import {
  deploymentApi,
  type Deployment, type ConfigTemplate, type ConfigTemplateField,
  type DeploymentConfigHa,
} from '../../api/deployment'
import ModuleConfigEditor, { type ModuleConfigEditorSource } from './ModuleConfigEditor'
import StringListInput from './StringListInput'

export type FieldValue = string | number | boolean | null | string[]
type Tab = 'scalar' | string   // 'scalar' = sections 탭, 나머지는 collection.key

export type ModuleConfigSource =
  | { type: 'deployment'; deployment: Deployment }
  | { type: 'module';     name: string; version?: string }

interface Props {
  source: ModuleConfigSource
  onClose: () => void
  onDone?: () => void | Promise<void>
  // true 면 Modal 오버레이 없이 패널만 렌더 (시스템/인프라 [패키지 설정] 탭의 페이지 임베드).
  inline?: boolean
}

/**
 * 모듈 설정 모달 — deployment 모드 (배포 > 서버) / module 모드 (빌드 · 검증 > 모듈관리) 공용.
 *
 *  - deployment 모드: agent_deployment 레코드 대상. PUT → DB + update_config job.
 *    모든 섹션·컬렉션을 이 화면에서 편집 (그룹/서버 이원화 폐지 — R2). HA 그룹 멤버면
 *    필드별 🔗 동기화 체크박스 — 체크+변경된 필드만 저장 시 그룹 멤버 전체에 전파,
 *    체크 상태는 ha_group.config_sync 에 영속 (기본값: scope=service 섹션 필드 ON).
 *  - module 모드:     Phase 1 로컬. PUT → build/dist/config.json (scalar) /
 *                     build/dist/{name}/config/*.jsonl (collection) + 로컬 PID SIGUSR1.
 */
export default function ModuleConfigModal({ source: sourceProp, onClose, onDone, inline }: Props) {
  // 부모(ServersPage 등)가 주기 폴링으로 재렌더하며 source 객체를 매번 새로 만들면
  // fetch/editor 의 useEffect 가 재실행돼 편집값이 서버 값으로 덮어써진다 —
  // mount 시점 스냅샷으로 identity 고정 (모듈 전환은 caller 가 key 로 리마운트).
  const [source] = useState(sourceProp)
  const { show } = useToast()
  const [loading, setLoading]     = useState(true)
  const [saving, setSaving]       = useState(false)
  const [template, setTemplate]   = useState<ConfigTemplate | null>(null)
  const [values, setValues]       = useState<Record<string, FieldValue>>({})
  const [initial, setInitial]     = useState<Record<string, FieldValue>>({})
  const [appliedAt, setAppliedAt] = useState<string | null>(null)
  const [tab, setTab]             = useState<Tab>('scalar')
  // HA 동기화 (deployment 모드 + HA 그룹 멤버일 때만) — ha=null 이면 체크박스 미노출.
  const [ha, setHa]               = useState<DeploymentConfigHa | null>(null)
  const [syncChecked, setSyncChecked] = useState<Set<string>>(new Set())

  // 제목/식별자
  const title = source.type === 'deployment'
    ? `${source.deployment.package_name} v${source.deployment.package_version} — 설정`
    : `${source.name}${source.version ? ` v${source.version}` : ''} — 설정 (로컬)`

  // 설치 전(pending) — 저장은 overlay 로 보존되고 설치 시 반영. 프로세스가 없어 restart 불가.
  const isPending = source.type === 'deployment' && source.deployment.status === 'pending'

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
        ha:       r.ha ?? null,
      }
    }
    const r = await deploymentApi.getModuleConfig(source.name)
    return {
      template:  r.template,
      config:    r.current || {},
      appliedAt: null,
      ha:        null,
    }
  }, [source])

  // source 분기 save
  const saveConfig = useCallback(async (vals: Record<string, FieldValue>,
                                        changedKeys: Set<string>) => {
    if (source.type === 'deployment') {
      // sync.keys = 변경∩체크 (피어에 merge 할 키만 — 체크됐지만 안 바뀐 필드는 미전파,
      // 잔여 드리프트는 그룹 비교 뷰가 경고로 노출). standalone(ha 없음)은 빈 배열 —
      // 항상 sync 를 보내 레거시 통짜 전파 경로를 쓰지 않는다.
      const sync = ha
        ? { keys: [...changedKeys].filter(k => syncChecked.has(k)), checked: [...syncChecked] }
        : { keys: [], checked: [] }
      const r = await deploymentApi.putDeploymentConfig(source.deployment.id, vals, true,
                                                        undefined, sync)
      const synced = r.sync_keys_applied?.length ?? 0
      const base = r.job_id ? `저장됨. update_config job #${r.job_id}` : '저장됨'
      return { ok: true, message: synced > 0 ? `${base} · ${synced}개 필드 그룹 동기화` : base }
    }
    // module 모드: 변경된 키만 보냄 (not_owned_by_module 오류 회피 위해 모든 키 아닌 템플릿 소유 키만)
    const payload: Record<string, unknown> = {}
    for (const k of changedKeys) payload[k] = vals[k]
    const r = await deploymentApi.putModuleConfig(source.name, payload)
    return {
      ok: true,
      message: `${r.applied}개 저장${r.removed ? ` · ${r.removed}개 제거` : ''}${r.restart_required ? ' · 재시작 필요' : ''}`,
    }
  }, [source, ha, syncChecked])

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
      // 동기화 체크 복원 — 영속값(ha.sync_keys) 없으면 scope=service 섹션 필드 기본 체크
      setHa(r.ha)
      setSyncChecked(new Set(r.ha ? (r.ha.sync_keys ?? serviceScopeKeys(r.template)) : []))
    } catch (e) {
      show((e as Error).message, 'err')
    } finally {
      setLoading(false)
    }
  }, [fetchConfig, show])

  useEffect(() => { void load() }, [load])

  // 변경된 필드 추적 — string[] 은 참조가 아닌 내용 비교 (StringListInput 이 새 배열을
  // 반환해도 값이 같으면 미변경 — 체크된 배열 필드의 불필요한 피어 전파 차단)
  const changed = useMemo(() => {
    const s = new Set<string>()
    for (const k of new Set([...Object.keys(values), ...Object.keys(initial)])) {
      if (!fieldValueEq(values[k], initial[k])) s.add(k)
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

  // 저장 전 validation — required + range
  function validate(): string | null {
    if (!template) return null
    for (const s of template.sections) {
      for (const f of s.fields) {
        const v = values[f.key]
        if (f.required && (v === '' || v === null || v === undefined)) {
          return `필수 항목 비어있음: ${f.label} (${f.key})`
        }
        if (f.type === 'int' && typeof v === 'number') {
          if (f.min !== undefined && v < f.min) return `${f.label}: ${v} < min(${f.min})`
          if (f.max !== undefined && v > f.max) return `${f.label}: ${v} > max(${f.max})`
        }
      }
    }
    return null
  }

  async function save(opts: { restartAfter?: boolean } = {}) {
    if (changed.size === 0) { show('변경된 항목 없음', 'err'); return }
    const err = validate()
    if (err) { show(err, 'err'); return }
    setSaving(true)
    try {
      const r = await saveConfig(values, changed)
      show(r.message, 'ok')
      // 재기동 옵션 (deployment 모드 + restart_required + restartAfter true)
      if (opts.restartAfter && source.type === 'deployment') {
        try {
          const jr = await deploymentApi.queueJob(source.deployment.id, 'restart')
          show(`재기동 큐 등록 (#${jr.job_id})`, 'ok')
        } catch (e) {
          show(`재기동 실패: ${(e as Error).message}`, 'err')
        }
      }
      if (onDone) await onDone()
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

  const body = (
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
              {/* 컬렉션은 백엔드가 scope 기반 자동 전파(should_propagate) — 멤버 어디서
                  편집해도 정합 유지되므로 서버 화면에서 항상 편집 가능 (R2 잠금 폐지). */}
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
                  <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 12,
                                display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
                    <span>🔁 재기동 필요 · ⚡ 즉시 적용</span>
                    {ha && <span>· 🔗 체크 = 저장 시 그룹({ha.group_name}) 멤버 전체에 반영, 해제 = 이 서버만</span>}
                    {appliedAt && <span>· 마지막 적용: {appliedAt}</span>}
                  </div>
                  {changed.size > 0 && (
                    <ChangeSummaryPanel template={template} values={values} initial={initial}
                      changed={changed}
                      onReset={(k) => setValues(p => ({ ...p, [k]: initial[k] }))}
                      onResetAll={() => setValues({ ...initial })} />
                  )}
                  {/* R2: 모든 섹션을 항상 편집 — scope=service 도 여기서 편집하고,
                      그룹 정합은 필드별 🔗 동기화(기본 ON)로 유지한다. */}
                  {template.sections.map(sec => (
                    <SectionBlock key={sec.key} section={sec} values={values}
                      initial={initial} changed={changed}
                      onChange={(k, v) => setValues(p => ({ ...p, [k]: v }))}
                      onReset={(k) => setValues(p => ({ ...p, [k]: initial[k] }))}
                      syncCtx={ha ? {
                        checked: syncChecked,
                        onToggle: (k) => setSyncChecked(prev => {
                          const next = new Set(prev)
                          if (next.has(k)) next.delete(k); else next.add(k)
                          return next
                        }),
                      } : undefined} />
                  ))}
                  {isPending ? (
                    <div style={{
                      marginTop: 12, padding: 10, background: '#e8f4fd',
                      border: '1px solid #5dade2', borderRadius: 4, fontSize: 12,
                    }}>
                      ℹ 아직 <b>설치 전</b>입니다 — 저장한 값은 [패키지 설치] 탭에서 <b>설치</b> 실행 시 반영됩니다.
                    </div>
                  ) : restartRequired && (
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
          {!inline && <button className="btn btn--outline" onClick={onClose}>닫기</button>}
          {template && tab === 'scalar' && (
            <>
              <button className="btn btn--primary" onClick={() => void save()}
                disabled={saving || changed.size === 0}>
                {saving ? '저장 중...' : `저장 (${changed.size} 변경)`}
              </button>
              {source.type === 'deployment' && restartRequired && !isPending && (
                <button className="btn btn--primary" onClick={() => void save({ restartAfter: true })}
                  disabled={saving || changed.size === 0}
                  style={{ background: '#e67e22', borderColor: '#e67e22' }}
                  title="저장 직후 restart job 자동 큐잉">
                  저장 + 재기동
                </button>
              )}
            </>
          )}
        </div>
      </div>
  )
  if (inline) return body
  return (
    <Modal title={title} onClose={onClose} fullscreen>
      {body}
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

function ChangeSummaryPanel({ template, values, initial, changed, onReset, onResetAll }: {
  template: ConfigTemplate
  values: Record<string, FieldValue>
  initial: Record<string, FieldValue>
  changed: Set<string>
  onReset: (key: string) => void
  onResetAll: () => void
}) {
  const [collapsed, setCollapsed] = useState(false)
  // key → field 매핑 (label + restart 추출)
  const fieldByKey = useMemo(() => {
    const m = new Map<string, ConfigTemplateField>()
    for (const s of template.sections) for (const f of s.fields) m.set(f.key, f)
    return m
  }, [template])

  const restartKeys = Array.from(changed).filter(k => (fieldByKey.get(k)?.restart !== false))
  const hotKeys     = Array.from(changed).filter(k => (fieldByKey.get(k)?.restart === false))

  function display(v: FieldValue): string {
    if (v === null || v === undefined || v === '') return '(빈 값)'
    if (typeof v === 'boolean') return v ? 'true' : 'false'
    return String(v)
  }

  return (
    <div style={{
      border: '1px solid #b8d4f5', borderRadius: 6, marginBottom: 12,
      background: '#fafcfe',
    }}>
      <div onClick={() => setCollapsed(c => !c)}
        style={{
          padding: '8px 14px', cursor: 'pointer', userSelect: 'none',
          display: 'flex', alignItems: 'center', gap: 8,
          background: '#e8f0fe', borderBottom: collapsed ? 'none' : '1px solid #b8d4f5',
          borderRadius: '6px 6px 0 0',
        }}>
        <span style={{ color: '#1a73e8', fontSize: 11 }}>{collapsed ? '▸' : '▾'}</span>
        <b style={{ color: '#1a73e8' }}>변경 사항 ({changed.size})</b>
        <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>
          🔁 재기동 {restartKeys.length} · ⚡ 즉시 {hotKeys.length}
        </span>
        <button onClick={(e) => { e.stopPropagation(); onResetAll() }}
                style={{ marginLeft: 'auto', fontSize: 11, padding: '2px 8px',
                         background: '#fff', border: '1px solid #ccc', borderRadius: 3, cursor: 'pointer' }}>
          전체 초기화
        </button>
      </div>
      {!collapsed && (
        <div style={{ padding: 8, maxHeight: 240, overflow: 'auto' }}>
          <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
            <thead>
              <tr style={{ color: 'var(--text-muted)' }}>
                <th style={{ textAlign: 'left', padding: '4px 6px', width: 220 }}>필드</th>
                <th style={{ textAlign: 'left', padding: '4px 6px' }}>옛 값</th>
                <th style={{ width: 30, textAlign: 'center' }}>→</th>
                <th style={{ textAlign: 'left', padding: '4px 6px' }}>새 값</th>
                <th style={{ width: 60, textAlign: 'center' }}></th>
              </tr>
            </thead>
            <tbody>
              {Array.from(changed).map(k => {
                const f = fieldByKey.get(k)
                const restart = f?.restart !== false
                return (
                  <tr key={k} style={{ borderTop: '1px solid #eee' }}>
                    <td style={{ padding: '4px 6px' }}>
                      <span title={k}>{f?.label ?? k}</span>
                      <span style={{ marginLeft: 4, fontSize: 10, color: restart ? '#c0392b' : '#1e7d34' }}>
                        {restart ? '🔁' : '⚡'}
                      </span>
                    </td>
                    <td style={{ padding: '4px 6px', color: 'var(--text-muted)', fontFamily: 'monospace' }}>
                      {display(initial[k])}
                    </td>
                    <td style={{ textAlign: 'center', color: '#1a73e8' }}>→</td>
                    <td style={{ padding: '4px 6px', color: '#1a73e8', fontFamily: 'monospace' }}>
                      {display(values[k])}
                    </td>
                    <td style={{ textAlign: 'center' }}>
                      <button onClick={() => onReset(k)}
                              title="이 필드만 초기화"
                              style={{ fontSize: 11, padding: '1px 6px', background: '#fff',
                                       border: '1px solid #ccc', borderRadius: 3, cursor: 'pointer' }}>
                        ↺
                      </button>
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}

// HA 동기화 체크 컨텍스트 — 없으면(standalone/module 모드) 🔗 체크박스 미렌더.
export interface SyncCtx {
  checked: Set<string>
  onToggle: (key: string) => void
}

export function SectionBlock({ section, values, initial, changed, onChange, onReset, syncCtx }: {
  section: {
    key: string; title: string; description?: string
    fields: ConfigTemplateField[]
    hidden?: boolean
    groups?: { key: string; title: string; description?: string }[]
  }
  values: Record<string, FieldValue>
  initial: Record<string, FieldValue>
  changed: Set<string>
  onChange: (key: string, v: FieldValue) => void
  onReset: (key: string) => void
  syncCtx?: SyncCtx
}) {
  // 인프라 section 은 기본 접힘 (헤더 클릭으로 펼침) — 모든 필드는 노출.
  const [collapsed, setCollapsed] = useState(!!section.hidden)

  // 모든 필드 노출 (고급/숨김 구분 제거).
  const visibleFields = section.fields

  // 필드를 group 단위로 묶기 — groups 정의 없으면 단일 묶음.
  // 그룹 선언된 순서대로 정렬하고, 소속 없는 필드는 '기타' 로.
  const groupDefs = section.groups || []
  type Bucket = { key: string; title: string; description?: string; fields: ConfigTemplateField[] }
  const buckets: Bucket[] = []
  if (groupDefs.length === 0) {
    buckets.push({ key: '__all__', title: '', fields: visibleFields })
  } else {
    const byKey = new Map<string, Bucket>()
    for (const g of groupDefs) {
      const b: Bucket = { key: g.key, title: g.title, description: g.description, fields: [] }
      byKey.set(g.key, b); buckets.push(b)
    }
    const misc: Bucket = { key: '__misc__', title: '기타', fields: [] }
    for (const f of visibleFields) {
      const target = f.group && byKey.get(f.group)
      if (target) target.fields.push(f)
      else misc.fields.push(f)
    }
    if (misc.fields.length) buckets.push(misc)
  }
  const nonEmptyBuckets = buckets.filter(b => b.fields.length > 0)

  return (
    <div style={{
      border: '1px solid #e5e5e5', borderRadius: 6, marginBottom: 12,
      background: '#fff',
      ...(section.hidden ? { borderStyle: 'dashed', background: '#fcfbf7' } : {}),
    }}>
      <div onClick={() => setCollapsed(c => !c)}
        style={{
          padding: '10px 14px', cursor: 'pointer', userSelect: 'none',
          display: 'flex', alignItems: 'baseline', gap: 8,
          borderBottom: collapsed ? 'none' : '1px solid #eee',
          background: section.hidden ? '#f5efe0' : '#fafafa',
        }}>
        <span style={{ color: 'var(--text-muted)', fontSize: 11 }}>{collapsed ? '▸' : '▾'}</span>
        <b>{section.title}</b>
        {section.hidden && (
          <span style={{
            fontSize: 10, padding: '1px 6px', borderRadius: 3,
            background: '#7f8c8d', color: '#fff',
          }}>인프라</span>
        )}
        {section.description && (
          <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>— {section.description}</span>
        )}
      </div>
      {!collapsed && (
        <div style={{ padding: 12 }}>
          {nonEmptyBuckets.map((b, idx) => (
            <div key={b.key} style={{ marginBottom: idx === nonEmptyBuckets.length - 1 ? 0 : 14 }}>
              {b.title && (
                <div style={{
                  fontSize: 12, fontWeight: 600, color: 'var(--text-muted)',
                  borderBottom: '1px solid #eee', paddingBottom: 4, marginBottom: 8,
                  display: 'flex', alignItems: 'baseline', gap: 6,
                }}>
                  <span>{b.title}</span>
                  {b.description && (
                    <span style={{ fontSize: 10, fontWeight: 400, color: 'var(--text-muted)' }}>— {b.description}</span>
                  )}
                </div>
              )}
              <div style={{
                display: 'grid',
                gridTemplateColumns: '200px 1fr', rowGap: 10, columnGap: 10,
                alignItems: 'start',
              }}>
                {b.fields.map(f => (
                  <FieldRow key={f.key} field={f}
                    value={values[f.key]}
                    initialValue={initial[f.key]}
                    isChanged={changed.has(f.key)}
                    onChange={v => onChange(f.key, v)}
                    onReset={() => onReset(f.key)}
                    syncCtx={syncCtx} />
                ))}
              </div>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}

function FieldRow({ field, value, initialValue, isChanged, onChange, onReset, syncCtx }: {
  field: ConfigTemplateField
  value: FieldValue
  initialValue: FieldValue
  isChanged: boolean
  onChange: (v: FieldValue) => void
  onReset: () => void
  syncCtx?: SyncCtx
}) {
  const needsRestart = field.restart !== false
  const badgeStyle: React.CSSProperties = {
    display: 'inline-block', fontSize: 10, padding: '1px 5px',
    borderRadius: 3, marginLeft: 6, fontWeight: 500,
    background: needsRestart ? '#fbe9e7' : '#e8f5e9',
    color:      needsRestart ? '#c0392b' : '#1e7d34',
    border: `1px solid ${needsRestart ? '#f5c6a7' : '#b7e0bd'}`,
    whiteSpace: 'nowrap',
  }
  return (
    <>
      <label style={{
        paddingTop: 6, fontSize: 13,
        color: isChanged ? '#2980b9' : undefined,
      }}>
        <span>{field.label}</span>
        <span style={badgeStyle} title={needsRestart ? '재기동 후 반영' : '저장 즉시 반영'}>
          {needsRestart ? '🔁 재기동' : '⚡ 즉시'}
        </span>
        {field.required && <span style={{ color: '#e74c3c', marginLeft: 4 }}>*</span>}
        {isChanged && <span style={{ marginLeft: 6, color: '#2980b9', fontSize: 11 }}>●</span>}
      </label>
      <div>
        <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
          <div style={{ flex: 1 }}>
            {renderInput(field, value, onChange)}
          </div>
          {isChanged && (
            <button onClick={onReset}
                    title={`초기값으로 되돌림: ${initialValue === null || initialValue === '' ? '(빈 값)' : String(initialValue)}`}
                    style={{ fontSize: 12, padding: '2px 8px', background: '#fff',
                             border: '1px solid #ccc', borderRadius: 3, cursor: 'pointer',
                             flexShrink: 0 }}>
              ↺
            </button>
          )}
          {syncCtx && (() => {
            const on = syncCtx.checked.has(field.key)
            return (
              <label title={on ? '🔗 동기화 — 이 필드를 변경해 저장하면 그룹 내 다른 멤버에도 같은 값 반영'
                               : '동기화 해제 — 이 서버에만 저장'}
                     style={{ display: 'flex', alignItems: 'center', gap: 3, fontSize: 11,
                              color: on ? '#1a73e8' : 'var(--text-muted)',
                              whiteSpace: 'nowrap', cursor: 'pointer', flexShrink: 0, userSelect: 'none' }}>
                <input type="checkbox" checked={on} onChange={() => syncCtx.onToggle(field.key)} />
                🔗
              </label>
            )
          })()}
        </div>
        {field.help && (
          <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 3 }}>{field.help}</div>
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
  if (f.type === 'string_list' || f.type === 'ref_list') {
    // 콤마 분리 입력 ↔ 문자열 배열 (ModuleConfigEditor 와 동일 동작).
    return (
      <StringListInput value={value}
        placeholder="콤마로 구분 (예: 10.0.1.48:9000, 10.0.1.49:9000)"
        onChange={onChange} />
    )
  }
  // string / path
  return (
    <input className="form-input" type="text"
      value={(value as string) ?? ''}
      onChange={e => onChange(e.target.value)} />
  )
}

export function defaultValue(f: ConfigTemplateField): FieldValue {
  if (f.default !== undefined && f.default !== null) {
    return f.default as FieldValue
  }
  switch (f.type) {
    case 'bool': return false
    case 'int':  return 0
    default:     return ''
  }
}

// FieldValue 내용 비교 — string[] 은 참조가 아닌 원소 비교 (changed 오탐 방지).
export function fieldValueEq(a: FieldValue | undefined, b: FieldValue | undefined): boolean {
  if (Array.isArray(a) || Array.isArray(b)) return JSON.stringify(a) === JSON.stringify(b)
  return a === b
}

// scope=service 섹션의 전 필드 키 — 동기화 체크 기본값 (영속값 없을 때).
// ModuleConfigModal 과 GroupConfigCompareView 가 동일 규칙 공유.
export function serviceScopeKeys(t: ConfigTemplate | null): string[] {
  if (!t) return []
  const out: string[] = []
  for (const s of t.sections) {
    if (s.scope !== 'service') continue
    for (const f of s.fields) out.push(f.key)
  }
  return out
}
