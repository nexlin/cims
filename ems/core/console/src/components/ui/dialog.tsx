import * as React from "react"
import * as DialogPrimitive from "@radix-ui/react-dialog"
import { X } from "lucide-react"

import { cn } from "@core/lib/utils"

const Dialog = DialogPrimitive.Root

const DialogTrigger = DialogPrimitive.Trigger

const DialogPortal = DialogPrimitive.Portal

const DialogClose = DialogPrimitive.Close

const DialogOverlay = React.forwardRef<
  React.ElementRef<typeof DialogPrimitive.Overlay>,
  React.ComponentPropsWithoutRef<typeof DialogPrimitive.Overlay>
>(({ className, ...props }, ref) => (
  <DialogPrimitive.Overlay
 ref={ref}
 className={cn(
      // 시안 실측 backdrop `#91939a` ≈ 흰 배경 위 40% 검정. shadcn 기본 80% 는 너무 어둡다.
      "fixed inset-0 z-50 bg-black/40 data-[state=open]:animate-in data-[state=closed]:animate-out data-[state=closed]:fade-out-0 data-[state=open]:fade-in-0",
 className
    )}
    {...props}
  />
))
DialogOverlay.displayName = DialogPrimitive.Overlay.displayName

const DialogContent = React.forwardRef<
  React.ElementRef<typeof DialogPrimitive.Content>,
  React.ComponentPropsWithoutRef<typeof DialogPrimitive.Content>
>(({ className, children, ...props }, ref) => (
  <DialogPortal>
    <DialogOverlay />
    <DialogPrimitive.Content
 ref={ref}
 className={cn(
        // 정본 = Figma `02 Components` Sec/Modal (392:108) — 헤더 51(구분선) · 본문 여백 20 ·
        // 푸터 우측 정렬(간격 8). 라운드 8(`--radius`) · 채움 `--surface` · 그림자 `Elevation/lg`.
        // shadcn 기본(`p-6 gap-4 rounded-lg bg-background`)은 세 영역을 한 상자로 봐서 어긋난다 —
        // 여백은 헤더·본문·푸터가 각자 갖는다.
        "fixed left-[50%] top-[50%] z-50 flex max-h-[calc(100vh-80px)] w-full max-w-[560px] translate-x-[-50%] translate-y-[-50%] flex-col overflow-hidden rounded-md border border-border bg-card shadow-lg duration-200 data-[state=open]:animate-in data-[state=closed]:animate-out data-[state=closed]:fade-out-0 data-[state=open]:fade-in-0 data-[state=closed]:zoom-out-95 data-[state=open]:zoom-in-95",
 className
      )}
      {...props}
    >
      {children}
      <DialogPrimitive.Close aria-label="닫기"
        className="absolute right-5 top-[17px] rounded-sm p-0.5 text-muted-foreground transition-colors hover:bg-accent hover:text-foreground focus-visible:outline-none focus-visible:shadow-focus">
        <X className="size-4" />
        <span className="sr-only">Close</span>
      </DialogPrimitive.Close>
    </DialogPrimitive.Content>
  </DialogPortal>
))
DialogContent.displayName = DialogPrimitive.Content.displayName

const DialogHeader = ({
 className,
  ...props
}: React.HTMLAttributes<HTMLDivElement>) => (
  <div
 className={cn(
      "flex shrink-0 flex-row items-center justify-between border-b border-border px-5 py-4 pr-12",
 className
    )}
    {...props}
  />
)
DialogHeader.displayName = "DialogHeader"

const DialogFooter = ({
 className,
  ...props
}: React.HTMLAttributes<HTMLDivElement>) => (
  <div
 className={cn(
      "flex shrink-0 flex-row justify-end gap-2 px-5 pb-4",
 className
    )}
    {...props}
  />
)
DialogFooter.displayName = "DialogFooter"

const DialogTitle = React.forwardRef<
  React.ElementRef<typeof DialogPrimitive.Title>,
  React.ComponentPropsWithoutRef<typeof DialogPrimitive.Title>
>(({ className, ...props }, ref) => (
  <DialogPrimitive.Title
 ref={ref}
 className={cn(
      "text-md font-medium leading-none",
 className
    )}
    {...props}
  />
))
DialogTitle.displayName = DialogPrimitive.Title.displayName

const DialogDescription = React.forwardRef<
  React.ElementRef<typeof DialogPrimitive.Description>,
  React.ComponentPropsWithoutRef<typeof DialogPrimitive.Description>
>(({ className, ...props }, ref) => (
  <DialogPrimitive.Description
 ref={ref}
 className={cn("text-sm text-muted-foreground", className)}
    {...props}
  />
))
DialogDescription.displayName = DialogPrimitive.Description.displayName

export {
  Dialog,
  DialogPortal,
  DialogOverlay,
  DialogTrigger,
  DialogClose,
  DialogContent,
  DialogHeader,
  DialogFooter,
  DialogTitle,
  DialogDescription,
}
