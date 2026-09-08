import type { ReactNode } from 'react'
import { cn } from '@core/lib/utils'

/**
 * 폼 필드 한 칸. 정본 = Figma `02 Components` TextInput/Field (17:58) —
 * **라벨 + 필수(`*`) + 값 + 헬프텍스트 + 우측 마커 배지**가 한 세트다
 * (`cims-design-handoff/components/contracts.md` §TextInput/Select).
 *
 * `marker` 는 입력칸 **오른쪽**에 세로 가운데로 붙는다 (Figma S3 93:2218 실측:
 * 입력 725 · 간격 10 · 마커 62). 설정 화면의 `재기동`/`즉시` 가 여기 들어간다.
 * `aside` 는 입력 오른쪽 안쪽 — 되돌리기처럼 값에 딸린 조작에 쓴다.
 */
export function FormField({ label, required, help, error, marker, aside, changed, children, className }: {
  label: ReactNode
  required?: boolean
  help?: ReactNode
  error?: string
  marker?: ReactNode
  aside?: ReactNode
  /** 저장 전 변경된 필드 — 라벨을 브랜드색으로 들어 올린다 */
  changed?: boolean
  children: ReactNode
  className?: string
}) {
  return (
    <label className={cn('flex flex-col gap-1', className)}>
      <span className={cn('text-sm font-medium', changed && 'text-primary')}>
        {label}{required && <span className="ml-0.5 text-destructive">*</span>}
      </span>
      <span className="flex items-center gap-2.5">
        <span className="min-w-0 flex-1">{children}</span>
        {aside}
        {marker && <span className="shrink-0">{marker}</span>}
      </span>
      {(error || help) && (
        <span className={cn('text-xs', error ? 'text-destructive' : 'text-muted-foreground')}>
          {error || help}
        </span>
      )}
    </label>
  )
}
