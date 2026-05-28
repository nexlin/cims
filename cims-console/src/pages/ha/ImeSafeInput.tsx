import { useState, type InputHTMLAttributes } from 'react'

// IME-safe input — 한글 입력 시 외부 setState 의 input.value 강제 재할당으로
// composition 이 깨지는 현상 방지. 외부 commit 은 compositionend / blur / Enter 시점만.
// useEffect 동기화 제거 — 사용자 입력 중 외부 props 변경이 덮어쓰는 일 방지.
export function ImeSafeInput({ value, onCommit, ...rest }: {
  value: string
  onCommit: (v: string) => void
} & Omit<InputHTMLAttributes<HTMLInputElement>, 'value' | 'onChange' | 'onCompositionStart' | 'onCompositionEnd' | 'onBlur' | 'onKeyDown'>) {
  const [local, setLocal] = useState(value)
  return (
    <input
      value={local}
      onChange={e => setLocal(e.target.value)}
      onCompositionEnd={(e) => {
        const v = (e.target as HTMLInputElement).value
        setLocal(v); onCommit(v)
      }}
      onBlur={() => onCommit(local)}
      onKeyDown={(e) => { if (e.key === 'Enter') onCommit(local) }}
      {...rest}
    />
  )
}
