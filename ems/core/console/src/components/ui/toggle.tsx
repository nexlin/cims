"use client"

import * as React from "react"
import * as TogglePrimitive from "@radix-ui/react-toggle"
import { cva, type VariantProps } from "class-variance-authority"

import { cn } from "@core/lib/utils"

const toggleVariants = cva(
  // 정본 = Figma `02 Components` Sec/SegmentedItem (17:26) — Default·Selected 두 상태.
  // **선택은 브랜드 채움 + 흰 글자**이고, 비선택은 배경 없이 muted 글자다. shadcn 기본
  // (`data-[state=on]:bg-accent`)은 선택이 거의 안 보여 세그먼트로 못 쓴다.
  // 치수도 시안 실측: 높이 28 · 좌우 12 (컨테이너가 padding 3 · 항목 간 4 를 준다).
  "inline-flex items-center justify-center gap-1.5 whitespace-nowrap rounded-md text-md font-medium transition-colors hover:bg-accent focus-visible:outline-none focus-visible:shadow-focus disabled:pointer-events-none disabled:text-text-disabled data-[state=on]:bg-primary data-[state=on]:font-semibold data-[state=on]:text-primary-foreground data-[state=on]:hover:bg-primary [&_svg]:pointer-events-none [&_svg]:size-3.5 [&_svg]:shrink-0",
  {
    variants: {
      variant: {
        default: "bg-transparent text-muted-foreground",
        outline:
          "border border-border bg-background text-foreground hover:bg-accent",
      },
      size: {
        default: "h-7 px-3",
        sm: "h-[26px] px-2.5 text-sm",
        icon: "size-7",
      },
    },
    defaultVariants: {
      variant: "default",
      size: "default",
    },
  }
)

const Toggle = React.forwardRef<
  React.ElementRef<typeof TogglePrimitive.Root>,
  React.ComponentPropsWithoutRef<typeof TogglePrimitive.Root> &
    VariantProps<typeof toggleVariants>
>(({ className, variant, size, ...props }, ref) => (
  <TogglePrimitive.Root
    ref={ref}
    className={cn(toggleVariants({ variant, size, className }))}
    {...props}
  />
))

Toggle.displayName = TogglePrimitive.Root.displayName

export { Toggle, toggleVariants }
