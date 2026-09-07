import * as React from "react"
import { cva, type VariantProps } from "class-variance-authority"

import { cn } from "@core/lib/utils"

const alertVariants = cva(
  // 정본 = Figma `02 Components` Sec/SectionMessage (20:23) — Info·Warning·Danger·Success 4톤.
  // shadcn 기본 2종(default/destructive)으로는 부족해
  // `cims-design-handoff/components/custom/alert-variants.ts` 로 교체한 것이다.
  // **화면당 1개 원칙** (DESIGN-RULES §2).
  "relative w-full rounded-md border px-3 py-2.5 text-md [&>svg]:size-4 [&>svg]:shrink-0",
  {
    variants: {
      variant: {
        info: "border-info bg-info-soft text-info-on",
        success: "border-success bg-success-soft text-success-on",
        warning: "border-warning bg-warning-soft text-warning-on",
        danger: "border-destructive bg-dangersoft text-dangersoft-on",
      },
    },
    defaultVariants: {
      variant: "info",
    },
  }
)

const Alert = React.forwardRef<
  HTMLDivElement,
  React.HTMLAttributes<HTMLDivElement> & VariantProps<typeof alertVariants>
>(({ className, variant, ...props }, ref) => (
  <div
    ref={ref}
    role="alert"
    className={cn(alertVariants({ variant }), className)}
    {...props}
  />
))
Alert.displayName = "Alert"

const AlertTitle = React.forwardRef<
  HTMLParagraphElement,
  React.HTMLAttributes<HTMLHeadingElement>
>(({ className, ...props }, ref) => (
  <h5
    ref={ref}
    className={cn("mb-1 font-medium leading-none tracking-tight", className)}
    {...props}
  />
))
AlertTitle.displayName = "AlertTitle"

const AlertDescription = React.forwardRef<
  HTMLParagraphElement,
  React.HTMLAttributes<HTMLParagraphElement>
>(({ className, ...props }, ref) => (
  <div
    ref={ref}
    className={cn("text-sm [&_p]:leading-relaxed", className)}
    {...props}
  />
))
AlertDescription.displayName = "AlertDescription"

export { Alert, AlertTitle, AlertDescription }
