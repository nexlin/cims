import { useState, type ReactNode } from 'react'
import { ChevronDown, ChevronRight } from 'lucide-react'

/**
 * SubSection — 접히는 섹션 머리(Level 2). 정본 = Figma `02 Components`
 * CollapsibleSectionHeader (19:37) · 계약은 `cims-design-handoff/components/contracts.md`:
 * Level 2 = 중간 굵기 + 얇은 화살표 + 좌측 들여쓰기 레일, **중첩은 2단까지**,
 * 항목 수를 괄호로 붙인다, **접힌 섹션에 저장/적용 버튼을 노출하지 않는다.**
 *
 * `right` 는 헤더 우측 슬롯이다 — ES-5 의 `미적용 n건`(warningSoft) 배지처럼
 * 접힌 상태에서도 봐야 하는 표식만 넣는다. 액션 버튼은 넣지 않는다(위 계약).
 */
export function SubSection({ title, count, hint, right, defaultOpen = true, level = 2, children }: {
  title: string
  count?: number
  hint?: string
  right?: ReactNode
  defaultOpen?: boolean
  /**
   * 1 = 화면의 큰 구획(그룹 설정·멤버·VIP…). 굵은 라벨 + **머리 아래 구분선** · 본문 들여쓰기 없음.
   * 2 = 그 안의 하위 구획(IP / Routing…). 중간 굵기 + 얇은 화살표 + 좌측 들여쓰기 레일.
   * Figma `Sec/CollapsibleSectionHeader`(19:37) 실측 — Level 1 은 높이 36, Level 2 는 32.
   */
  level?: 1 | 2
  children: ReactNode
}) {
  const [open, setOpen] = useState(defaultOpen)
  if (level === 1) {
    return (
      <div>
        <button onClick={() => setOpen(o => !o)}
                className="flex h-9 w-full select-none items-center gap-2 border-b border-border text-left">
          <span className="w-4 shrink-0 text-foreground">
            {open ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
          </span>
          <span className="text-md font-semibold">
            {title}{count !== undefined && ` (${count})`}
          </span>
          {hint && <span className="truncate text-xs font-normal text-muted-foreground">{hint}</span>}
          {right && <span className="ml-auto flex shrink-0 items-center gap-1.5 pr-1">{right}</span>}
        </button>
        {open && <div className="pb-4 pt-3.5">{children}</div>}
      </div>
    )
  }
  return (
    <div className="border-b border-border last:border-b-0">
      <button onClick={() => setOpen(o => !o)}
              className="flex h-8 w-full select-none items-center gap-2 text-left">
        <span className="w-3.5 shrink-0 text-muted-foreground">
          {open ? <ChevronDown size={11} /> : <ChevronRight size={11} />}
        </span>
        <span className="text-md font-medium">
          {title}{count !== undefined && ` (${count})`}
        </span>
        {hint && <span className="truncate text-xs text-muted-foreground">{hint}</span>}
        {right && <span className="ml-auto flex shrink-0 items-center gap-1.5 pr-1">{right}</span>}
      </button>
      {open && <div className="pb-3 pl-[22px]">{children}</div>}
    </div>
  )
}
