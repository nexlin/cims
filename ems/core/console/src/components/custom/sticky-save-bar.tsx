import type { ReactNode } from 'react'
import { ArrowLeft, Check } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { cn } from '@core/lib/utils'

/**
 * 폼 하단 저장바. 정본 = Figma `02 Components` StickySaveBar (459:7508) —
 * `[배지] 설명 ···· [되돌리기] [저장]`.
 *
 * 배지 자리는 화면마다 다르다: G1 은 `변경 n건 · 전 멤버 적용`, S3 는 마커(`재기동`)와
 * "저장 후 무엇을 해야 반영되는가" 를 알린다. 그래서 배지·설명을 통째로 받는다.
 *
 * `되돌리기` 는 핸드오프가 **제안**으로 표시한 항목인데 시안 그림에는 들어 있고
 * 통합 저장과 짝이라 채택했다 (docs/design/console_design_system.md §7-10).
 */
export function StickySaveBar({ badge, note, saveLabel, disabled, saving,
                                onRevert, onSave, extra, className }: {
  badge: ReactNode
  note: ReactNode
  saveLabel: ReactNode
  disabled?: boolean
  saving?: boolean
  onRevert?: () => void
  onSave?: () => void
  /** 저장·되돌리기 앞에 붙는 보조 액션 — 모달로 열렸을 때의 [닫기] 같은 것 */
  extra?: ReactNode
  className?: string
}) {
  return (
    <div className={cn('flex shrink-0 items-center gap-3 border-t border-border bg-card px-4 py-2.5',
                       className)}>
      {badge}
      <span className="truncate text-xs text-muted-foreground">{note}</span>
      <div className="ml-auto flex shrink-0 items-center gap-1.5">
        {extra}
        {onRevert && (
          <Button variant="ghost" onClick={onRevert} disabled={disabled || saving}
                  title="저장하지 않은 변경을 되돌린다">
            <ArrowLeft /> 되돌리기
          </Button>
        )}
        <Button variant="default" onClick={onSave} disabled={disabled || saving}>
          <Check /> {saving ? '저장 중…' : saveLabel}
        </Button>
      </div>
    </div>
  )
}
