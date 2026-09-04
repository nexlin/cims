import { cva } from "class-variance-authority";

/**
 * SectionMessage → shadcn alert.
 * 기본 alert 는 default/destructive 2종뿐이라 4 tone 으로 확장합니다.
 * 화면당 1개 원칙 (DESIGN-RULES.md §2).
 */
export const alertVariants = cva(
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
    defaultVariants: { variant: "info" },
  },
);
