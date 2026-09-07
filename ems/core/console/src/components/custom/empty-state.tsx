import type { ReactNode } from 'react'
import { cn } from '@core/lib/utils'

/**
 * EmptyState — 빈 상태 5종에 전부 이걸 쓴다. 정본 = Figma `02 Components` EmptyState (20:26)
 * + 문구는 `cims-design-handoff/screens/empty-states.md` (ES-1~ES-5) 그대로.
 * **화면마다 다른 문구·다른 모양을 만들지 않는다.**
 *
 * 시안 실측: 배경 `--bg-soft`(= `--muted`) · 라운드 14(`rounded-lg`) · 여백 20 ·
 * 제목 14px Medium `--text` · 설명 11px `--text-muted`. **테두리 없음** —
 * 핸드오프 `empty-state.tsx` 는 `border-dashed` 인데 그림에는 없어 그림을 따랐다.
 */
export function EmptyState({ title, description, action, className }: {
  title: string
  description?: string
  action?: ReactNode
  className?: string
}) {
  return (
    <div className={cn('flex flex-col items-center gap-1.5 rounded-lg bg-muted p-5 text-center', className)}>
      <p className="text-base font-medium text-foreground">{title}</p>
      {description && <p className="max-w-[72ch] text-xs text-muted-foreground">{description}</p>}
      {action && <div className="mt-2">{action}</div>}
    </div>
  )
}
