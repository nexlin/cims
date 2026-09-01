// ⓘ 말풍선 — 화면의 뜻처럼 "한 번 읽으면 되는 설명"을 자리 차지 없이 접어 둔다.
//
// 설명을 본문에 상자로 깔면 매번 읽지 않는데도 계속 세로 공간을 먹고, 정작 봐야 할 표를 밀어낸다.
// 그래서 기본은 점 하나이고, 누를 때만 펼친다(hover 는 native title 로 요약이 뜬다).
import { useEffect, useRef, useState, type ReactNode } from 'react'

export function InfoDot({ label, children }: { label?: string; children: ReactNode }) {
  const [open, setOpen] = useState(false)
  const box = useRef<HTMLSpanElement>(null)

  // 바깥 클릭 / Esc 로 닫는다 — 열어둔 채 다른 조작을 하면 시야를 가린다.
  useEffect(() => {
    if (!open) return
    const onDown = (e: MouseEvent) => {
      if (box.current && !box.current.contains(e.target as Node)) setOpen(false)
    }
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') setOpen(false) }
    document.addEventListener('mousedown', onDown)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', onDown)
      document.removeEventListener('keydown', onKey)
    }
  }, [open])

  return (
    <span className="info-dot-wrap" ref={box}>
      <button type="button" className={`info-dot ${open ? 'info-dot--open' : ''}`}
              aria-expanded={open} aria-label={label || '설명'} title={label || '설명 보기'}
              onClick={() => setOpen(v => !v)}>ⓘ</button>
      {open && <span className="info-pop" role="note">{children}</span>}
    </span>
  )
}
