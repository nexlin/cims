import * as React from "react"
import { Slot } from "@radix-ui/react-slot"
import { cva, type VariantProps } from "class-variance-authority"

import { cn } from "@core/lib/utils"

const buttonVariants = cva(
  // 정본 = Figma `02 Components` Sec/Button (16:5) — variant 4 × size 2 × state 2 = 16종.
  // 시안 실측: 라운드 8(`--radius`) · sm 26px/12px SemiBold/좌우 10 · md 36px/13px Medium/좌우 14.
  // shadcn 기본(h-9/h-8, text-sm)보다 작아 size 를 재정의했다 (MAPPING.md `Button` 행).
  //
  // **Disabled 는 불투명도를 쓰지 않는다** (contracts.md §Button) — solid 계열은 배경을
  // `neutral-soft` 로 내리고, outline/ghost 는 배경을 유지한 채 라벨만 `text-disabled` 로 둔다.
  // 그래서 `disabled:` 는 base 가 아니라 variant 마다 따로 쓴다.
  // 사유는 눈에 보이게 함께 적는다 — 툴팁만으로는 계약 위반이다.
  //
  // 기본값을 shadcn 관습(Primary/md)이 아니라 **Secondary sm** 으로 둔 이유: 시안에서 이게
  // 압도적 다수이고, Primary 를 쓰려면 `variant="default"` 를 명시하게 만들어 「화면당 1개」
  // 규칙이 눈에 걸리게 하려는 것이다.
  "inline-flex items-center justify-center gap-1.5 whitespace-nowrap rounded-md transition-colors focus-visible:outline-none focus-visible:shadow-focus disabled:pointer-events-none [&_svg]:pointer-events-none [&_svg]:shrink-0",
  {
    variants: {
      variant: {
        // Primary — 화면당 1개 (저장·생성)
        default:
          "bg-primary text-primary-foreground hover:bg-[var(--cims-brand-hover)] disabled:bg-neutral-soft disabled:text-text-disabled",
        // Danger — 그룹/전체 단위 파괴적 액션만
        destructive:
          "bg-destructive text-white hover:bg-[var(--cims-danger-hover)] disabled:bg-neutral-soft disabled:text-text-disabled",
        // Secondary — 대부분의 액션. 행 단위 파괴적 액션도 여기
        outline:
          "border border-border bg-background text-foreground hover:bg-accent disabled:bg-background disabled:text-text-disabled",
        // Ghost — 보조 (되돌리기·새로고침)
        ghost:
          "text-muted-foreground hover:bg-accent hover:text-foreground disabled:bg-transparent disabled:text-text-disabled",
        link: "text-primary underline-offset-4 hover:underline disabled:text-text-disabled",
      },
      size: {
        sm: "h-[26px] px-2.5 text-sm font-semibold [&_svg]:size-3.5",
        default: "h-9 px-3.5 text-md font-medium [&_svg]:size-4",
        iconSm: "size-[26px] [&_svg]:size-3.5",
        icon: "size-9 [&_svg]:size-4",
      },
    },
    defaultVariants: {
      variant: "outline",
      size: "sm",
    },
  }
)

export interface ButtonProps
  extends React.ButtonHTMLAttributes<HTMLButtonElement>,
    VariantProps<typeof buttonVariants> {
  asChild?: boolean
}

const Button = React.forwardRef<HTMLButtonElement, ButtonProps>(
  ({ className, variant, size, asChild = false, ...props }, ref) => {
    const Comp = asChild ? Slot : "button"
    return (
      <Comp className={cn(buttonVariants({ variant, size, className }))} ref={ref} {...props} />
    )
  }
)
Button.displayName = "Button"

export { Button, buttonVariants }
