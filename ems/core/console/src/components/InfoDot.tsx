// ⓘ 말풍선 — 화면의 뜻처럼 "한 번 읽으면 되는 설명"을 자리 차지 없이 접어 둔다.
//
// 설명을 본문에 상자로 깔면 매번 읽지 않는데도 계속 세로 공간을 먹고, 정작 봐야 할 표를 밀어낸다.
// 그래서 기본은 점 하나이고, 누를 때만 펼친다(hover 는 native title 로 요약이 뜬다).
import { Info } from 'lucide-react'
import { cn } from '@core/lib/utils'
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
    <span className="relative inline-flex" ref={box}>
      {/* 아이콘은 Lucide 다 — 구 코드는 텍스트 글리프 `ⓘ` 였다(§3-2 위반). */}
      <button type="button"
              className={cn('inline-flex px-0.5 leading-none hover:text-primary',
                            open ? 'text-primary' : 'text-muted-foreground')}
              aria-expanded={open} aria-label={label || '설명'} title={label || '설명 보기'}
              onClick={() => setOpen(v => !v)}><Info size={14} /></button>
      {open && (
        // 꼬리는 테두리 삼각형 위에 배경 삼각형을 겹쳐 선이 이어져 보이게 한다.
        <span role="note"
              className={cn('absolute left-[-6px] top-[calc(100%+8px)] z-40 w-max max-w-[340px]',
                            'rounded-md border border-border bg-card px-3 py-2.5 text-sm font-normal',
                            'leading-[1.55] text-foreground shadow-lg text-left whitespace-normal',
                            "before:absolute before:bottom-full before:left-2.5 before:content-['']",
                            'before:border-[6px] before:border-transparent before:border-b-border',
                            "after:absolute after:bottom-full after:left-2.5 after:-mb-px after:content-['']",
                            'after:border-[6px] after:border-transparent after:border-b-card')}>
          {children}
        </span>
      )}
    </span>
  )
}
