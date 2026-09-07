import { cn } from '@core/lib/utils'

/**
 * Radio — 정본 = Figma `02 Components` Radio (43:33). 16×16 ·
 * Unselected = `surface` 채움 + `border` 링 / Selected = `primary` 링 + 흰 점.
 *
 * shadcn 대응은 `radio-group`(Radix)이지만 Radix 는 `<RadioGroup>` 이 div 를 렌더해
 * **표 안에서 쓸 수 없다**(그룹이 `<tbody>` 를 감싸야 한다). 그래서 네이티브 input 을
 * `appearance-none` 으로 다시 칠했다 — name 그룹핑·키보드 이동이 그대로 살아 있다.
 */
export function Radio({ className, ...props }: React.InputHTMLAttributes<HTMLInputElement>) {
  return (
    <input type="radio"
           className={cn(
             'size-4 shrink-0 cursor-pointer appearance-none rounded-full border border-border bg-background',
             // 링 두께 5 → 안쪽 흰 점 6px (시안 실측과 같은 비율)
             'checked:border-[5px] checked:border-primary',
             'focus-visible:outline-none focus-visible:shadow-focus',
             'disabled:cursor-not-allowed disabled:bg-neutral-soft',
             className)}
           {...props} />
  )
}
