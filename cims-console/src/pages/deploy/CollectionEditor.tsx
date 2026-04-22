import { useCallback, useEffect, useMemo, useState } from 'react'
import { useToast } from '../../components/Toast'
import {
  deploymentApi, type ConfigTemplateCollection, type ConfigTemplateField,
} from '../../api/deployment'

type Record_ = Record<string, unknown>

export default function CollectionEditor({ deploymentId, collection }: {
  deploymentId: number
  collection: ConfigTemplateCollection
}) {
  const { show } = useToast()
  const [records, setRecords]   = useState<Record_[]>([])
  const [original, setOriginal] = useState<Record_[]>([])
  const [loading, setLoading]   = useState(true)
  const [saving, setSaving]     = useState(false)
  const [showAdvanced, setShowAdvanced] = useState(false)
  const [editingIdx, setEditingIdx] = useState<number | null>(null)

  const fields = collection.schema.fields
  const idField = collection.schema.id_field || 'id'

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const r = await deploymentApi.getDeploymentCollection(deploymentId, collection.key)
      setRecords(r.records)
      setOriginal(JSON.parse(JSON.stringify(r.records)))
    } catch (e) {
      show(`${collection.title} 로드 실패: ${(e as Error).message}`, 'err')
    } finally {
      setLoading(false)
    }
  }, [deploymentId, collection.key, collection.title, show])

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
      const r = await deploymentApi.putDeploymentCollection(
        deploymentId, collection.key, records, true
      )
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
    !f.advanced && !f.readonly
  ).slice(0, 4) // 목록 테이블에 표시할 주요 필드 (최대 4개)

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

      {/* 행 목록 */}
      <table className="data-table" style={{ width: '100%' }}>
        <thead>
          <tr>
            {summaryFields.map(f => <th key={f.key}>{f.label}</th>)}
            <th style={{ width: 160 }}>작업</th>
          </tr>
        </thead>
        <tbody>
          {records.length === 0 ? (
            <tr><td colSpan={summaryFields.length + 1} style={{ textAlign: 'center', color: '#888', padding: 20 }}>
              행 없음 — "＋ 추가" 로 생성
            </td></tr>
          ) : (
            records.map((r, i) => (
              <RowDisplay key={String(r[idField] || i)} row={r}
                summaryFields={summaryFields}
                active={editingIdx === i}
                onEdit={() => setEditingIdx(editingIdx === i ? null : i)}
                onRemove={() => removeRow(i)} />
            ))
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
  return String(v)
}

function FieldEditor({ field, value, onChange }: {
  field: ConfigTemplateField
  value: unknown
  onChange: (v: unknown) => void
}) {
  return (
    <>
      <label style={{ fontSize: 13, paddingTop: 6 }}>
        {field.label}
        {field.required && <span style={{ color: '#e74c3c', marginLeft: 4 }}>*</span>}
      </label>
      <div>
        {renderInput(field, value, onChange)}
        {field.help && (
          <div style={{ fontSize: 11, color: '#888', marginTop: 3 }}>{field.help}</div>
        )}
      </div>
    </>
  )
}

function renderInput(f: ConfigTemplateField, value: unknown, onChange: (v: unknown) => void) {
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
  return (
    <input className="form-input" type="text"
      value={(value as string) ?? ''}
      onChange={e => onChange(e.target.value)} />
  )
}
