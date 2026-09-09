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
    // 실측(459:7238) — 좌우 16 · 상하 12 · 요소 사이 10, 위 테두리 1.
    <div className={cn('flex shrink-0 items-center gap-2.5 border-t border-border bg-card px-4 py-3',
                       className)}>
      {badge}
      <span className="min-w-0 flex-1 truncate text-xs text-muted-foreground">{note}</span>
      <div className="flex shrink-0 items-center gap-2.5">
        {extra}
        {/* 저장바 버튼만 **md**(h36 · 좌우 14 · 13px Medium) 다 — 바 높이 60 =
            상하 12 + 36 이 그 실측이다. 다른 자리의 기본 sm(26)과 다른 유일한 예외. */}
        {onRevert && (
          <Button variant="ghost" size="default" onClick={onRevert} disabled={disabled || saving}
                  title="저장하지 않은 변경을 되돌린다">
            <ArrowLeft /> 되돌리기
          </Button>
        )}
        <Button variant="default" size="default" onClick={onSave} disabled={disabled || saving}>
          <Check /> {saving ? '저장 중…' : saveLabel}
        </Button>
      </div>
    </div>
  )
}
