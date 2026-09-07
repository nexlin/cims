import * as React from "react"
import { cva, type VariantProps } from "class-variance-authority"

import { cn } from "@core/lib/utils"

const badgeVariants = cva(
  // 정본 = Figma `02 Components` Sec/Badge (16:33) — Tone 6 × Style 2 = 12종.
  // shadcn 기본 4종으로는 부족해 `cims-design-handoff/components/custom/badge-variants.ts`
  // 로 교체한 것이다 (docs/design/console_design_system.md §4).
  // Soft = 값·분류 표시(기본) / Solid = 개수·심각도처럼 눈에 띄어야 하는 것에만.
  "inline-flex items-center gap-1 rounded-full border px-2 py-0.5 text-sm font-semibold leading-4 whitespace-nowrap transition-colors focus-visible:shadow-focus",
  {
    variants: {
      variant: {
        brandSoft: "border-transparent bg-brandsoft text-brandsoft-on",
        successSoft: "border-transparent bg-success-soft text-success-on",
        warningSoft: "border-transparent bg-warning-soft text-warning-on",
        dangerSoft: "border-transparent bg-dangersoft text-dangersoft-on",
        infoSoft: "border-transparent bg-info-soft text-info-on",
        neutralSoft: "border-transparent bg-neutral-soft text-neutral-on",
        brandSolid: "border-transparent bg-primary text-primary-foreground",
        successSolid: "border-transparent bg-success text-white",
        warningSolid: "border-transparent bg-warning text-white",
        dangerSolid: "border-transparent bg-destructive text-white",
        infoSolid: "border-transparent bg-info text-white",
        neutralSolid: "border-transparent bg-neutral text-white",
      },
    },
    defaultVariants: {
      variant: "neutralSoft",
    },
  }
)

export interface BadgeProps
  extends React.HTMLAttributes<HTMLDivElement>,
    VariantProps<typeof badgeVariants> {}

function Badge({ className, variant, ...props }: BadgeProps) {
  return (
    <div className={cn(badgeVariants({ variant }), className)} {...props} />
  )
}

export { Badge, badgeVariants }
