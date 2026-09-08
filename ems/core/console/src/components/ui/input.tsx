import * as React from "react"

import { cn } from "@core/lib/utils"

/**
 * 정본 = Figma `02 Components` Sec/TextInput (Field) (17:58) — Default·Focus·Error·Disabled 4상태.
 * 실측(17:29): 컨트롤 높이 **33** · 좌우 10 · 글자 12px · 라운드 8(`--radius`) · 채움 `--surface`.
 * 라벨(12px Medium)·필수·도움말(11px)은 이 컴포넌트가 아니라 `FormField` 가 감싼다 —
 * 시안도 그 셋을 한 세트로 묶어 놨다(contracts.md §TextInput).
 *
 * shadcn 기본값과 다른 점 셋:
 * 1. `shadow-sm` 을 뺀다 — 시안 입력칸에 그림자가 없다.
 * 2. 포커스는 `ring-1` 이 아니라 **테두리 primary + `shadow-focus`** 다 (Figma `Focus/ring`
 *    = 0 0 0 3px #4F46E559 = `--cims-focus-ring`). 절대 규칙 §3-2 의 그 링이다.
 * 3. **비활성에 불투명도를 쓰지 않는다** (contracts.md) — 배경을 `--secondary` 로 내리고
 *    글자를 muted 로 둔다. 반투명은 뒤 배경에 따라 대비가 무너진다.
 *
 * 에러는 `aria-invalid` 로 켠다 — 클래스를 따로 넘기지 않아도 스크린리더와 색이 같이 간다.
 */
const Input = React.forwardRef<HTMLInputElement, React.ComponentProps<"input">>(
  ({ className, type, ...props }, ref) => {
    return (
      <input
        type={type}
        className={cn(
          "flex h-[33px] w-full rounded-md border border-border bg-card px-2.5 text-sm text-foreground transition-colors",
          "file:border-0 file:bg-transparent file:text-sm file:font-medium file:text-foreground",
          "placeholder:text-muted-foreground",
          "focus-visible:border-primary focus-visible:outline-none focus-visible:shadow-focus",
          "aria-[invalid=true]:border-destructive",
          "disabled:cursor-not-allowed disabled:bg-secondary disabled:text-muted-foreground",
          className
        )}
        ref={ref}
        {...props}
      />
    )
  }
)
Input.displayName = "Input"

export { Input }
