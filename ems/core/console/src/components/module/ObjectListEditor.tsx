import type { ReactNode } from 'react'
import type { ConfigTemplateField } from '../../api/deployment'

type Item = Record<string, unknown>

/**
 * object_list 필드 편집기 — item_schema.fields 를 열로 하는 행 테이블 + 행 추가/삭제.
 * 배포 스칼라 폼(ModuleConfigModal)과 컬렉션 편집기(ModuleConfigEditor) 양쪽에서 공유한다.
 *
 * - renderCell: 셀 입력 렌더러 주입(선택). 미지정 시 내장 최소 dispatch(string/int/bool/enum/password)만
 *   지원한다. ref/ref_list 처럼 참조가 필요한 item 필드는 renderCell 로 상위 renderInput 을 넘긴다.
 * - ensureOne: true 면 항목이 없을 때 빈 1행을 상시 표시하고 마지막 1행은 삭제 불가(ip/port 처럼
 *   최소 1개가 자연스러운 필드용). false(기본)면 "항목 없음" 빈 상태를 보인다.
 */
export function ObjectListEditor({ field, value, onChange, renderCell, ensureOne = false }: {
  field: ConfigTemplateField
  value: unknown
  onChange: (v: unknown) => void
  renderCell?: (f: ConfigTemplateField, v: unknown, on: (nv: unknown) => void) => ReactNode
  ensureOne?: boolean
}) {
  const itemFields = field.item_schema?.fields || []
  // 레거시 호환: 값이 콤마문자열 "ip:port, .." 또는 ["ip:port", ..] 로 저장돼 있으면 표시·편집 시
  //   [{ip,port}, ..] 로 정규화한다(수정 시 정규화된 배열로 저장됨). 이미 객체 배열이면 그대로.
  const items: Item[] = coerceItems(value, itemFields)

  function blank(): Item {
    const r: Item = {}
    for (const f of itemFields) if (f.default !== undefined) r[f.key] = f.default
    return r
  }

  // ensureOne: 빈 배열이면 편집 가능한 빈 1행을 화면에 띄운다(편집 시 실제 항목으로 승격).
  const display: Item[] = items.length ? items : (ensureOne ? [blank()] : [])

  function addItem() { onChange([...display, blank()]) }
  function removeItem(i: number) { onChange(display.filter((_, idx) => idx !== i)) }
  function updateItemField(i: number, key: string, v: unknown) {
    onChange(display.map((it, idx) => idx === i ? { ...it, [key]: v } : it))
  }

  const cell = renderCell || defaultCell

  return (
    <div style={{ border: '1px dashed var(--border)', borderRadius: 4, padding: 6 }}>
      {display.length === 0 ? (
        <div style={{ fontSize: 11, color: 'var(--muted-foreground)', marginBottom: 4 }}>항목 없음</div>
      ) : (
        <table style={{ width: '100%', fontSize: 12 }}>
          <thead>
            <tr>
              {itemFields.map(f => <th key={f.key} style={{ textAlign: 'left' }}>{f.label}</th>)}
              <th style={{ width: 40 }}></th>
            </tr>
          </thead>
          <tbody>
            {display.map((it, i) => (
              <tr key={i}>
                {itemFields.map(f => (
                  <td key={f.key} style={{ padding: '2px 4px' }}>
                    {cell(f, it[f.key], (v) => updateItemField(i, f.key, v))}
                  </td>
                ))}
                <td>
                  <button className="btn btn--sm btn--danger" onClick={() => removeItem(i)}
                    disabled={ensureOne && display.length <= 1}>×</button>
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

// 저장값 → item 배열 정규화. 객체 배열은 그대로, 콤마문자열/문자열배열("ip:port")은 파싱.
function coerceItems(value: unknown, itemFields: ConfigTemplateField[]): Item[] {
  if (Array.isArray(value)) {
    if (value.every(v => v && typeof v === 'object' && !Array.isArray(v))) return value as Item[]
    return value.map(v => strToItem(String(v), itemFields)).filter(Boolean) as Item[]
  }
  if (typeof value === 'string' && value.trim()) {
    return value.split(',').map(s => strToItem(s.trim(), itemFields)).filter(Boolean) as Item[]
  }
  return []
}

// "host:port" → { <field0>: host, <field1>: port }. ':' 없으면 전체를 첫 필드로.
function strToItem(s: string, itemFields: ConfigTemplateField[]): Item | null {
  if (!s) return null
  const item: Item = {}
  const k0 = itemFields[0]?.key
  const k1 = itemFields[1]?.key
  const idx = s.lastIndexOf(':')
  if (idx >= 0 && k1) {
    if (k0) item[k0] = s.slice(0, idx)
    const portStr = s.slice(idx + 1)
    item[k1] = itemFields[1]?.type === 'int' ? Number(portStr) : portStr
  } else if (k0) {
    item[k0] = s
  }
  return item
}

// 내장 최소 셀 렌더러 — 참조가 필요 없는 스칼라 item 필드용(ip/port 등).
function defaultCell(f: ConfigTemplateField, v: unknown, on: (nv: unknown) => void): ReactNode {
  if (f.type === 'bool') {
    return <input type="checkbox" checked={!!v} onChange={e => on(e.target.checked)} />
  }
  if (f.type === 'enum') {
    return (
      <select className="form-input" value={(v as string) ?? ''} onChange={e => on(e.target.value)}>
        {(f.options || []).map(o => <option key={o} value={o}>{o}</option>)}
      </select>
    )
  }
  if (f.type === 'int') {
    return (
      <input className="form-input" type="number" min={f.min} max={f.max}
        value={v === null || v === undefined ? '' : Number(v)}
        onChange={e => on(e.target.value === '' ? null : Number(e.target.value))} />
    )
  }
  if (f.type === 'password') {
    return (
      <input className="form-input" type="password" value={(v as string) ?? ''}
        onChange={e => on(e.target.value)} />
    )
  }
  return (
    <input className="form-input" type="text" value={(v as string) ?? ''}
      onChange={e => on(e.target.value)} />
  )
}
