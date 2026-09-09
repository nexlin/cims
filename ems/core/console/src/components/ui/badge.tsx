import * as React from "react"
import { cva, type VariantProps } from "class-variance-authority"

import { cn } from "@core/lib/utils"

const badgeVariants = cva(
  // 정본 = Figma `02 Components` Sec/Badge (16:33) — Tone 6 × Style 2 = 12종.
  // shadcn 기본 4종으로는 부족해 `cims-design-handoff/components/custom/badge-variants.ts`
  // 로 교체한 것이다 (docs/design/console_design_system.md §4).
  // Soft = 값·분류 표시(기본) / Solid = 개수·심각도처럼 눈에 띄어야 하는 것에만.
  // Soft 는 **테두리가 있다** — 채움 `--*-soft`, 테두리·글자 `--*-on-soft` (Figma 실측).
  // 핸드오프 `badge-variants.ts` 는 `border-transparent` 인데 그림과 어긋나 그림을 따랐다.
  // 실측(16:8) — 라운드 **6**(`radius/sm`, 알약 아님) · 좌우 6 · 상하 2 · 12px SemiBold · 줄높이 1.2.
  "inline-flex items-center gap-1 rounded-sm border px-1.5 py-0.5 text-sm font-semibold leading-[1.2] whitespace-nowrap transition-colors focus-visible:shadow-focus",
  {
    variants: {
      variant: {
        brandSoft: "border-current bg-brandsoft text-brandsoft-on",
        successSoft: "border-current bg-success-soft text-success-on",
        warningSoft: "border-current bg-warning-soft text-warning-on",
        dangerSoft: "border-current bg-dangersoft text-dangersoft-on",
        infoSoft: "border-current bg-info-soft text-info-on",
        neutralSoft: "border-current bg-neutral-soft text-neutral-on",
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

/** 배지 톤 — 상태를 배지로 옮기는 헬퍼(`sevBadgeClass` 등)의 반환 타입. */
export type BadgeTone = NonNullable<VariantProps<typeof badgeVariants>['variant']>

export interface BadgeProps
  extends React.HTMLAttributes<HTMLDivElement>,
    VariantProps<typeof badgeVariants> {}

function Badge({ className, variant, ...props }: BadgeProps) {
  return (
    <div className={cn(badgeVariants({ variant }), className)} {...props} />
  )
}

export { Badge, badgeVariants }
