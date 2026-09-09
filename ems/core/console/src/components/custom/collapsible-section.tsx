import { useState, type ReactNode } from 'react'
import { ChevronDown, ChevronRight } from 'lucide-react'

/**
 * SubSection — 접히는 섹션 머리(Level 2). 정본 = Figma `02 Components`
 * CollapsibleSectionHeader (19:37) · 계약은 `cims-design-handoff/components/contracts.md`:
 * Level 2 = 중간 굵기 + 얇은 화살표 + 좌측 들여쓰기 레일(실선), **중첩은 2단까지**,
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
   *     **가로 구분선은 Level 1 만** — 19:36 의 네 상태에서 Level 2 에는 선이 없다.
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
        {open && <div className="pb-4 pt-3">{children}</div>}
      </div>
    )
  }
  return (
    <div className="mb-[18px] last:mb-0">
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
      {/* 본문 = 시안 `SectionBody`(174:2860) 그대로 — 폭 8 의 빈 레일 + `border-l`
          `--border-strong` + `pl-13`. 세로선이 Level 2 의 소속 표시다(Level 1 은 머리 밑
          가로선). 머리와 12 띄우고, 섹션끼리는 18 띄운다(93:2217·93:2255 실측). */}
      {open && (
        <div className="mt-3 ml-2 border-l border-border-strong pl-[13px]">{children}</div>
      )}
    </div>
  )
}
