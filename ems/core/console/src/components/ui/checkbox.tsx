import * as React from "react"
import * as CheckboxPrimitive from "@radix-ui/react-checkbox"
import { Check, Minus } from "lucide-react"

import { cn } from "@core/lib/utils"

/**
 * 정본 = Figma `02 Components` Sec/Checkbox (163:53) — 실측:
 * **16×16 · 라운드 6** · 꺼짐은 `--surface` 채움 + `--border-strong` 테두리,
 * 켜짐은 `--primary` 채움 + **테두리 없음** + `Check` **12px**.
 * 설명: *"적용/저장 버튼 뒤에 있는 폼의 on/off 값에 사용. 즉시 적용되는 on/off 는 Switch."*
 *
 * shadcn 기본과 다른 점 넷: 그림자 없음 · 꺼짐 테두리가 primary 가 아니라 `border-strong` ·
 * 포커스는 `ring-1` 이 아니라 `shadow-focus`(§3-2 그 링) · **비활성에 불투명도를 쓰지 않는다**
 * (배경 `--secondary` + `text-disabled`).
 */
const Checkbox = React.forwardRef<
  React.ElementRef<typeof CheckboxPrimitive.Root>,
  React.ComponentPropsWithoutRef<typeof CheckboxPrimitive.Root>
>(({ className, ...props }, ref) => (
  <CheckboxPrimitive.Root
    ref={ref}
    className={cn(
      "group grid size-4 shrink-0 place-content-center rounded-sm border border-border-strong bg-card text-transparent transition-colors",
      "focus-visible:outline-none focus-visible:shadow-focus",
      "data-[state=checked]:border-transparent data-[state=checked]:bg-primary data-[state=checked]:text-primary-foreground",
      "data-[state=indeterminate]:border-transparent data-[state=indeterminate]:bg-primary data-[state=indeterminate]:text-primary-foreground",
      "disabled:cursor-not-allowed disabled:border-border disabled:bg-secondary disabled:text-text-disabled",
      className
    )}
    {...props}
  >
    <CheckboxPrimitive.Indicator className="grid place-content-center text-current">
      {/* 3상태 — 「일부 선택」에 체크를 그리면 전체 선택과 구별되지 않는다 (VerificationV2Page
          그룹 체크박스). 시안에 없는 상태라 켜짐과 같은 채움에 표식만 가로줄로 가른다. */}
      <Check className="size-3 group-data-[state=indeterminate]:hidden" />
      <Minus className="hidden size-3 group-data-[state=indeterminate]:block" />
    </CheckboxPrimitive.Indicator>
  </CheckboxPrimitive.Root>
))
Checkbox.displayName = CheckboxPrimitive.Root.displayName

export { Checkbox }
