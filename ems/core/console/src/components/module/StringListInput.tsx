import { useState } from 'react'

/** string_list/ref_list 콤마 구분 입력 ↔ string[].
 *  배열 파생값(join)으로 input 을 직접 그리면 타이핑한 끝 콤마가 빈 조각 필터로
 *  즉시 사라져 콤마 입력 자체가 불가능하다 — raw 텍스트를 로컬 상태로 유지하고
 *  배열 변환은 파생으로만 내보낸다. 외부 변경(초기화 버튼 등)은 현재 텍스트와
 *  의미가 다를 때만 반영해 편집 중인 raw(끝 콤마·공백)를 지키지 않는다. */
export default function StringListInput({ value, placeholder, onChange }: {
  value: unknown
  placeholder?: string
  onChange: (v: string[]) => void
}) {
  const parse = (s: string) => s.split(',').map(p => p.trim()).filter(p => p !== '')
  const canonical = (Array.isArray(value) ? (value as unknown[]).map(String) : []).join(', ')
  const [text, setText] = useState(canonical)
  const [seen, setSeen] = useState(canonical)
  if (canonical !== seen) {
    // 외부 값 변경 감지 — render 중 상태 조정 (React 공식 패턴)
    setSeen(canonical)
    if (parse(text).join(', ') !== canonical) setText(canonical)
  }
  return (
    <input className="form-input" type="text" value={text}
      placeholder={placeholder || '콤마로 구분'}
      onChange={e => {
        setText(e.target.value)
        onChange(parse(e.target.value))
      }} />
  )
}
