import { cva } from "class-variance-authority";

/**
 * shadcn 기본 badge 는 variant 4종뿐입니다.
 * CIMS 는 Tone 6 × Style 2 = 12종이 필요합니다.
 * components/ui/badge.tsx 의 badgeVariants 를 이걸로 교체하세요.
 */
export const badgeVariants = cva(
  "inline-flex items-center gap-1 rounded-full border px-2 py-0.5 text-sm font-semibold leading-4 whitespace-nowrap transition-colors focus-visible:shadow-focus",
  {
    variants: {
      variant: {
        // Soft — 기본. 값·분류 표시
        brandSoft: "border-transparent bg-brandsoft text-brandsoft-on",
        successSoft: "border-transparent bg-success-soft text-success-on",
        warningSoft: "border-transparent bg-warning-soft text-warning-on",
        dangerSoft: "border-transparent bg-dangersoft text-dangersoft-on",
        infoSoft: "border-transparent bg-info-soft text-info-on",
        neutralSoft: "border-transparent bg-neutral-soft text-neutral-on",
        // Solid — 개수/심각도처럼 눈에 띄어야 하는 것에만
        brandSolid: "border-transparent bg-primary text-primary-foreground",
        successSolid: "border-transparent bg-success text-white",
        warningSolid: "border-transparent bg-warning text-white",
        dangerSolid: "border-transparent bg-destructive text-white",
        infoSolid: "border-transparent bg-info text-white",
        neutralSolid: "border-transparent bg-neutral text-white",
      },
    },
    defaultVariants: { variant: "neutralSoft" },
  },
);
