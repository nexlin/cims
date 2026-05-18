import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useToast } from '../Toast'
import {
  deploymentApi, type ConfigTemplateCollection, type ConfigTemplateField,
} from '../../api/deployment'

type Record_ = Record<string, unknown>

// ref 필드의 옵션 (collection name → option list) 을 전역 캐시로 관리.
// 한 페이지에서 여러 editor 가 열려도 중복 fetch 방지.
type RefOptions = Record<string, string[]>
const refOptionsCache: { current: RefOptions } = { current: {} }

export type ModuleConfigEditorSource =
  | { type: 'deployment'; deploymentId: number }
  | { type: 'module';     moduleName: string }
  // HA 그룹 단위 — scope=service collection 의 정합 보장. fetch 는 첫 멤버, save 는 모든 멤버에 PUT.
  | { type: 'group';      deploymentIds: number[] }

interface Props {
  source: ModuleConfigEditorSource
  collection: ConfigTemplateCollection
}

// T2 (2026-05-18) drift 정보 응답 구조 — UI 가 ha_group 멤버 정합 표시용.
interface DriftInfo {
  detected: boolean
  peers: Array<{ deployment_id: number; agent_id: number; status: number;
                 ok: boolean; count: number | null; hash: string }>
  mode?: string | null
  scope?: string | null
}

function ModuleConfigEditorInner({ source, collection }: Props) {
  const { show } = useToast()
  const [records, setRecords]   = useState<Record_[]>([])
  const [original, setOriginal] = useState<Record_[]>([])
  const [loading, setLoading]   = useState(true)
  const [saving, setSaving]     = useState(false)
  const [showAdvanced, setShowAdvanced] = useState(false)
  const [editingIdx, setEditingIdx] = useState<number | null>(null)
  const [tagFilter, setTagFilter] = useState<string>('')
  const [refOpts, setRefOpts]     = useState<RefOptions>(refOptionsCache.current)
  const [drift, setDrift]       = useState<DriftInfo>({ detected: false, peers: [] })
  const refOptsLoaded = useRef(new Set<string>())

  const fields = collection.schema.fields
  const idField = collection.schema.id_field || 'id'

  // ref/ref_list 필드가 참조하는 collection 들 자동 fetch
  useEffect(() => {
    const needs = new Set<string>()
    const walk = (fs: ConfigTemplateField[]) => {
      for (const f of fs) {
        if ((f.type === 'ref' || f.type === 'ref_list') && f.ref_collection) {
          needs.add(f.ref_collection)
        }
        if (f.type === 'object_list' && f.item_schema) walk(f.item_schema.fields)
      }
    }
    walk(fields)
    const todo = Array.from(needs).filter(c => !refOptsLoaded.current.has(c))
    if (todo.length === 0) return
    todo.forEach(c => refOptsLoaded.current.add(c))
    const getter = source.type === 'deployment'
      ? (key: string) => deploymentApi.getDeploymentCollection(source.deploymentId, key)
      : source.type === 'group'
      ? (key: string) => deploymentApi.getDeploymentCollection(source.deploymentIds[0], key)
      : (key: string) => deploymentApi.getModuleCollection(source.moduleName, key)
    Promise.all(todo.map(async c => {
      try {
        const r = await getter(c)
        const names = (r.records as Record_[])
          .map(rec => String(rec['name'] || rec['id'] || ''))
          .filter(s => !!s)
        return [c, names] as [string, string[]]
      } catch {
        return [c, [] as string[]] as [string, string[]]
      }
    })).then(pairs => {
      const merge: RefOptions = { ...refOptionsCache.current }
      for (const [c, v] of pairs) merge[c] = v
      refOptionsCache.current = merge
      setRefOpts({ ...merge })
    })
  }, [fields, source])

  // source 분기 fetch/save
  // T1/T2 (2026-05-18): csc 가 _put_deployment_collection 에서 자동 fan-out 함.
  // group 케이스도 deployment 1개에만 PUT 하면 csc 가 ha_group 멤버 전체에 분배.
  // GET 응답에 drift_detected / peers 가 포함되어 UI 가 양 멤버 정합 표시 가능.
  const fetchCollection = useCallback(() => {
    if (source.type === 'deployment')
      return deploymentApi.getDeploymentCollection(source.deploymentId, collection.key)
    if (source.type === 'group')
      return deploymentApi.getDeploymentCollection(source.deploymentIds[0], collection.key)
    return deploymentApi.getModuleCollection(source.moduleName, collection.key)
  }, [source, collection.key])

  const saveCollection = useCallback(async (recs: Record_[]) => {
    if (source.type === 'deployment') {
      return deploymentApi.putDeploymentCollection(source.deploymentId, collection.key, recs, true)
    }
    if (source.type === 'group') {
      // csc 가 자동 fan-out → 첫 멤버만 PUT 해도 양 멤버 동기화됨.
      return deploymentApi.putDeploymentCollection(source.deploymentIds[0], collection.key, recs, true)
    }
    return deploymentApi.putModuleCollection(source.moduleName, collection.key, recs, true)
  }, [source, collection.key])

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const r = await fetchCollection() as Record_ & {
        records: Record_[]
        drift_detected?: boolean
        peers?: DriftInfo['peers']
        ha_group_mode?: string | null
        scope?: string | null
      }
      setRecords(r.records)
      setOriginal(JSON.parse(JSON.stringify(r.records)))
      setDrift({
        detected: !!r.drift_detected,
        peers:    r.peers || [],
        mode:     r.ha_group_mode ?? null,
        scope:    r.scope ?? null,
      })
    } catch (e) {
      show(`${collection.title} 로드 실패: ${(e as Error).message}`, 'err')
    } finally {
      setLoading(false)
    }
  }, [fetchCollection, collection.title, show])

  useEffect(() => { void load() }, [load])

  const dirty = useMemo(
    () => JSON.stringify(records) !== JSON.stringify(original),
    [records, original]
  )

  function addRow() {
    const r: Record_ = {}
    for (const f of fields) {
      if (f.auto) continue
      if (f.default !== undefined) r[f.key] = f.default
    }
    setRecords(rs => [...rs, r])
    setEditingIdx(records.length)
  }

  function removeRow(i: number) {
    setRecords(rs => rs.filter((_, idx) => idx !== i))
    if (editingIdx === i) setEditingIdx(null)
  }

  function updateField(i: number, key: string, value: unknown) {
    setRecords(rs => rs.map((r, idx) => idx === i ? { ...r, [key]: value } : r))
  }

  async function save() {
    setSaving(true)
    try {
      const r = await saveCollection(records)
      show(`${collection.title} 저장됨 (${r.count}개, signal: ${r.signaled.length ? r.signaled.join(',') : 'n/a'})`, 'ok')
      setOriginal(JSON.parse(JSON.stringify(records)))
    } catch (e) {
      show(`저장 실패: ${(e as Error).message}`, 'err')
    } finally {
      setSaving(false)
    }
  }

  async function reload() {
    await load()
    setEditingIdx(null)
  }

  const visibleFields = fields.filter(f =>
    !f.readonly && (showAdvanced || !f.advanced)
  )
  const summaryFields = fields.filter(f =>
    !f.advanced && !f.readonly && f.type !== 'object_list'
  ).slice(0, 4) // 목록 테이블에 표시할 주요 필드 (최대 4개)

  // tag 기반 필터링 + 전체 태그 집합 수집
  const allTags = useMemo(() => {
    const s = new Set<string>()
    for (const r of records) {
      const t = r['tags']
      if (Array.isArray(t)) for (const v of t) if (typeof v === 'string' && v) s.add(v)
    }
    return Array.from(s).sort()
  }, [records])
  const visibleIdx = useMemo(() => {
    if (!tagFilter) return records.map((_, i) => i)
    return records
      .map((r, i) => ({ r, i }))
      .filter(({ r }) => Array.isArray(r['tags']) && (r['tags'] as unknown[]).includes(tagFilter))
      .map(({ i }) => i)
  }, [records, tagFilter])

  if (loading) return <div className="empty" style={{ padding: 20 }}>로딩 중...</div>

  return (
    <div>
      {collection.description && (
        <div style={{ fontSize: 12, color: '#666', marginBottom: 10 }}>
          {collection.description}
          {collection.reload_hint && (
            <span style={{ marginLeft: 8, color: '#27ae60' }}>⚡ {collection.reload_hint}</span>
          )}
        </div>
      )}

      {/* T2 drift 배너 — ha_group 멤버 정합 불일치 */}
      {drift.detected && (
        <div style={{
          background: '#fff8e1', border: '1px solid #f5c046',
          borderRadius: 4, padding: '8px 12px', marginBottom: 10,
          fontSize: 12, color: '#7a5a00',
        }}>
          ⚠️ HA 그룹 멤버 간 정합 불일치 — 양 멤버의 jsonl 이 다릅니다.
          {drift.peers.length > 0 && (
            <span style={{ marginLeft: 8 }}>
              ({drift.peers.map(p => `dep#${p.deployment_id}: ${p.count ?? 'err'}건 (${p.hash.slice(0, 6) || '–'})`).join(' / ')})
            </span>
          )}
          <span style={{ marginLeft: 8 }}>저장 시 자동으로 양 멤버에 동기화됩니다.</span>
        </div>
      )}
      {!drift.detected && drift.peers.length > 1 && (
        <div style={{
          fontSize: 11, color: '#27ae60', marginBottom: 8,
        }}>
          ✓ HA 그룹 멤버 정합 (mode={drift.mode || '?'}, {drift.peers.length} 멤버)
        </div>
      )}

      {/* tag filter chip */}
      {allTags.length > 0 && (
        <div style={{ marginBottom: 8, display: 'flex', gap: 4, flexWrap: 'wrap', alignItems: 'center' }}>
          <span style={{ fontSize: 11, color: '#666' }}>태그 필터:</span>
          <button
            className={`btn btn--sm ${tagFilter === '' ? 'btn--primary' : 'btn--outline'}`}
            onClick={() => setTagFilter('')}>전체</button>
          {allTags.map(t => (
            <button key={t}
              className={`btn btn--sm ${tagFilter === t ? 'btn--primary' : 'btn--outline'}`}
              onClick={() => setTagFilter(t)}>{t}</button>
          ))}
        </div>
      )}

      {/* 행 목록 */}
      <table className="data-table" style={{ width: '100%' }}>
        <thead>
          <tr>
            {summaryFields.map(f => <th key={f.key}>{f.label}</th>)}
            <th style={{ width: 160 }}>작업</th>
          </tr>
        </thead>
        <tbody>
          {visibleIdx.length === 0 ? (
            <tr><td colSpan={summaryFields.length + 1} style={{ textAlign: 'center', color: '#888', padding: 20 }}>
              {records.length === 0 ? '행 없음 — "＋ 추가" 로 생성' : '태그 필터 결과 없음'}
            </td></tr>
          ) : (
            visibleIdx.map(i => {
              const r = records[i]
              return (
                <RowDisplay key={String(r[idField] || i)} row={r}
                  summaryFields={summaryFields}
                  active={editingIdx === i}
                  onEdit={() => setEditingIdx(editingIdx === i ? null : i)}
                  onRemove={() => removeRow(i)} />
              )
            })
          )}
        </tbody>
      </table>

      {/* 편집 영역 */}
      {editingIdx !== null && records[editingIdx] && (
        <div style={{
          marginTop: 10, padding: 12, border: '1px solid #ddd', borderRadius: 6,
          background: '#fafbfc',
        }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10, marginBottom: 8 }}>
            <b style={{ fontSize: 13 }}>행 #{editingIdx + 1} 편집</b>
            <label style={{ fontSize: 12, marginLeft: 'auto' }}>
              <input type="checkbox" checked={showAdvanced}
                onChange={e => setShowAdvanced(e.target.checked)} /> 고급 필드
            </label>
          </div>
          <div style={{
            display: 'grid', gridTemplateColumns: '160px 1fr',
            rowGap: 8, columnGap: 10, alignItems: 'start',
          }}>
            {visibleFields.map(f => (
              <FieldEditor key={f.key} field={f}
                value={records[editingIdx][f.key]}
                refOpts={refOpts}
                onChange={v => updateField(editingIdx, f.key, v)} />
            ))}
          </div>
        </div>
      )}

      <div style={{ marginTop: 10, display: 'flex', gap: 8 }}>
        <button className="btn btn--sm btn--outline" onClick={addRow}>＋ 추가</button>
        <button className="btn btn--sm btn--outline" onClick={reload} disabled={saving}>↻ 다시 로드</button>
        <span style={{ marginLeft: 'auto' }}>
          <button className="btn btn--sm btn--primary" onClick={save}
            disabled={!dirty || saving}>
            {saving ? '저장 중...' : `저장 (${records.length}개)`}
          </button>
        </span>
      </div>
    </div>
  )
}

/** 부모 (ModuleConfigModal) 가 재렌더링될 때, props (source, collection) 가
 *  동일 reference 이면 editor 재렌더 자체를 막는다. addRow 등 로컬 state 가
 *  상위 polling 에 의해 초기화되는 것을 방지하는 최종 방어선. */
const ModuleConfigEditor = memo(
  ModuleConfigEditorInner,
  (prev, next) => prev.source === next.source && prev.collection === next.collection
)
export default ModuleConfigEditor

function RowDisplay({ row, summaryFields, active, onEdit, onRemove }: {
  row: Record_
  summaryFields: ConfigTemplateField[]
  active: boolean
  onEdit: () => void
  onRemove: () => void
}) {
  return (
    <tr style={{ background: active ? '#eef5ff' : undefined }}>
      {summaryFields.map(f => (
        <td key={f.key} style={{ fontSize: 12 }}>
          {formatValue(row[f.key], f)}
        </td>
      ))}
      <td>
        <div style={{ display: 'flex', gap: 4 }}>
          <button className="btn btn--sm" onClick={onEdit}>
            {active ? '닫기' : '편집'}
          </button>
          <button className="btn btn--sm btn--danger" onClick={onRemove}>삭제</button>
        </div>
      </td>
    </tr>
  )
}

function formatValue(v: unknown, f: ConfigTemplateField): string {
  if (v === undefined || v === null || v === '') return '—'
  if (f.type === 'bool') return v ? '✓' : ''
  if (f.type === 'password') return '••••'
  if (f.type === 'string_list' || f.type === 'ref_list') {
    return Array.isArray(v) ? (v.length ? v.join(', ') : '—') : String(v)
  }
  if (f.type === 'object_list') {
    return Array.isArray(v) ? `${v.length}개` : '—'
  }
  return String(v)
}

function FieldEditor({ field, value, refOpts, onChange }: {
  field: ConfigTemplateField
  value: unknown
  refOpts: RefOptions
  onChange: (v: unknown) => void
}) {
  return (
    <>
      <label style={{ fontSize: 13, paddingTop: 6 }}>
        {field.label}
        {field.required && <span style={{ color: '#e74c3c', marginLeft: 4 }}>*</span>}
      </label>
      <div>
        {renderInput(field, value, onChange, refOpts)}
        {field.help && (
          <div style={{ fontSize: 11, color: '#888', marginTop: 3 }}>{field.help}</div>
        )}
      </div>
    </>
  )
}

function renderInput(f: ConfigTemplateField, value: unknown, onChange: (v: unknown) => void,
                     refOpts: RefOptions = {}) {
  if (f.type === 'bool') {
    return <input type="checkbox" checked={!!value} onChange={e => onChange(e.target.checked)} />
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
        onChange={e => onChange(e.target.value === '' ? null : Number(e.target.value))} />
    )
  }
  if (f.type === 'password') {
    return (
      <input className="form-input" type="password"
        value={(value as string) ?? ''}
        onChange={e => onChange(e.target.value)} />
    )
  }
  if (f.type === 'ref') {
    const options = (f.ref_collection && refOpts[f.ref_collection]) || []
    return (
      <select className="form-input" value={(value as string) ?? ''}
        onChange={e => onChange(e.target.value)}>
        <option value="">(선택 안함)</option>
        {options.map(o => <option key={o} value={o}>{o}</option>)}
      </select>
    )
  }
  if (f.type === 'string_list') {
    // 콤마 분리 입력 ↔ 문자열 배열
    const arr = Array.isArray(value) ? (value as unknown[]).map(String) : []
    return (
      <input className="form-input" type="text"
        value={arr.join(', ')}
        placeholder="콤마로 구분"
        onChange={e => {
          const raw = e.target.value
          const parts = raw.split(',').map(s => s.trim()).filter(s => s !== '')
          onChange(parts)
        }} />
    )
  }
  if (f.type === 'ref_list') {
    const options = (f.ref_collection && refOpts[f.ref_collection]) || []
    const selected = new Set(Array.isArray(value) ? (value as unknown[]).map(String) : [])
    return (
      <div style={{ display: 'flex', flexDirection: 'column', gap: 2, maxHeight: 120, overflowY: 'auto',
                    border: '1px solid #ddd', borderRadius: 4, padding: 4 }}>
        {options.length === 0 ? (
          <div style={{ fontSize: 11, color: '#999' }}>(참조할 항목 없음)</div>
        ) : options.map(o => (
          <label key={o} style={{ fontSize: 12 }}>
            <input type="checkbox" checked={selected.has(o)}
              onChange={e => {
                const next = new Set(selected)
                if (e.target.checked) next.add(o); else next.delete(o)
                onChange(Array.from(next))
              }} /> {o}
          </label>
        ))}
      </div>
    )
  }
  if (f.type === 'object_list') {
    return <ObjectListEditor field={f} value={value} onChange={onChange} refOpts={refOpts} />
  }
  return (
    <input className="form-input" type="text"
      value={(value as string) ?? ''}
      onChange={e => onChange(e.target.value)} />
  )
}

function ObjectListEditor({ field, value, onChange, refOpts }: {
  field: ConfigTemplateField
  value: unknown
  onChange: (v: unknown) => void
  refOpts: RefOptions
}) {
  const items = Array.isArray(value) ? (value as Record_[]) : []
  const itemFields = field.item_schema?.fields || []
  function addItem() {
    const r: Record_ = {}
    for (const f of itemFields) {
      if (f.default !== undefined) r[f.key] = f.default
    }
    onChange([...items, r])
  }
  function removeItem(i: number) {
    onChange(items.filter((_, idx) => idx !== i))
  }
  function updateItemField(i: number, key: string, v: unknown) {
    onChange(items.map((it, idx) => idx === i ? { ...it, [key]: v } : it))
  }
  return (
    <div style={{ border: '1px dashed #bbb', borderRadius: 4, padding: 6 }}>
      {items.length === 0 ? (
        <div style={{ fontSize: 11, color: '#888', marginBottom: 4 }}>항목 없음</div>
      ) : (
        <table style={{ width: '100%', fontSize: 12 }}>
          <thead>
            <tr>
              {itemFields.map(f => <th key={f.key} style={{ textAlign: 'left' }}>{f.label}</th>)}
              <th style={{ width: 40 }}></th>
            </tr>
          </thead>
          <tbody>
            {items.map((it, i) => (
              <tr key={i}>
                {itemFields.map(f => (
                  <td key={f.key} style={{ padding: '2px 4px' }}>
                    {renderInput(f, it[f.key], (v) => updateItemField(i, f.key, v), refOpts)}
                  </td>
                ))}
                <td>
                  <button className="btn btn--sm btn--danger" onClick={() => removeItem(i)}>×</button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      <button className="btn btn--sm btn--outline" onClick={addItem} style={{ marginTop: 4 }}>＋ 항목</button>
    </div>
  )
}
