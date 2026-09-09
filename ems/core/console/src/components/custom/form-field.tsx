import type { ReactNode } from 'react'
import { cn } from '@core/lib/utils'

/**
 * 폼 한 칸 — 라벨 + 필수 표시 + 컨트롤 + 도움말/에러를 한 덩어리로 묶는다.
 * 정본 = Figma `02 Components` Sec/TextInput (Field) (17:58) + 화면 도안의 `field`
 * (S3 93:2218 · G3-3 213:3730 · SA3 462:8406).
 *
 * 도안의 `field` 는 **가로 두 칸**이다 — 왼쪽에 「라벨·컨트롤·도움말」 한 덩어리(폭 729),
 * 오른쪽에 폭 **62 고정**의 마커 자리(사이 10). 라벨과 도움말이 마커 밑으로 흐르지 않는다.
 *
 * 마커는 **위로 붙인다**(`marker` y=0) — G3-3(213:3738·214:3779)이 모든 필드에서 그렇고,
 * `CMP probe 엔드포인트` 같은 **키 큰 위젯**(표 191px)에서는 가운데 정렬이 성립하지 않는다.
 * S3·SA3·G3-1 은 마커를 입력칸 높이 가운데에 뒀지만 그 세 장은 전부 한 줄짜리 입력만 있어
 * 이 경우를 정하지 못한다 — **모든 필드 유형이 다 나오는 도안(G3-3)이 결정한다.**
 */
export function FormField({ label, required, help, error, marker, aside, changed,
                            inlineLabel, children, className }: {
  label: ReactNode
  required?: boolean
  help?: ReactNode
  error?: string
  marker?: ReactNode
  aside?: ReactNode
  /** 저장 전 변경된 필드 — 라벨을 브랜드색으로 들어 올린다 */
  changed?: boolean
  /**
   * 체크박스처럼 **컨트롤이 라벨보다 작은** 값 — 라벨을 위에 두지 않고 컨트롤 오른쪽에
   * 붙여 한 줄로 그린다. 정본 = Figma G3-5 `field · checkbox`(221:3984):
   * `checkboxRow` = Checkbox 16 + 8 + 라벨(12px), 필드 높이 **18**.
   * 라벨을 위에 두면 체크박스 한 칸에 세 줄(라벨·박스·도움말)이 나가 폼이 늘어진다.
   */
  inlineLabel?: boolean
  children: ReactNode
  className?: string
}) {
  const labelText = (
    <span className={cn('text-sm font-medium', changed && 'text-primary')}>
      {label}{required && <span className="ml-0.5 text-destructive">*</span>}
    </span>
  )
  return (
    <label className={cn('flex items-start gap-2.5', className)}>
      <span className="flex min-w-0 flex-1 flex-col gap-1.5">
        {inlineLabel ? (
          <span className="flex items-center gap-2">
            {children}{labelText}{aside}
          </span>
        ) : (
          <>
            {labelText}
            <span className="flex items-center gap-2.5">
              <span className="min-w-0 flex-1">{children}</span>
              {aside}
            </span>
          </>
        )}
        {(error || help) && (
          <span className={cn('text-xs', error ? 'text-destructive' : 'text-muted-foreground')}>
            {error || help}
          </span>
        )}
      </span>
      {marker && <span className="w-[62px] shrink-0">{marker}</span>}
    </label>
  )
}
