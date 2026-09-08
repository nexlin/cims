import { X } from 'lucide-react'
import { type ReactNode } from 'react'
import { Dialog, DialogContent, DialogHeader, DialogTitle } from '@core/components/ui/dialog'

/**
 * 화면 위에 띄우는 창. 껍데기는 시안 Modal(Figma `02 Components` 392:108) —
 * 헤더 51(구분선) · 본문 여백 20 · 푸터 우측 정렬. 그 값은 `ui/dialog` 가 갖고 있고
 * 여기서는 **호출부 API 만 유지**한다(title·onClose·children·wide·width).
 *
 * Radix 로 옮기면서 공짜로 얻는 것 — 포커스 가둠·복귀, `aria-modal`, 바깥 스크롤 잠금,
 * Esc 처리. 손으로 만든 `.modal-overlay` 는 Esc 만 `window` 리스너로 흉내 냈었다.
 *
 * `fullscreen` 은 **모달이 아니다** — 상단 헤더와 사이드바를 덮지 않고 content 영역만
 * 가리는 화면 전환에 가깝다. 시안에 대응이 없어 Radix 를 쓰지 않고 고정 패널로 둔다.
 */
interface Props {
  title: ReactNode
  onClose: () => void
  children: ReactNode
  wide?: boolean
  fullscreen?: boolean
  width?: number | string
}

export default function Modal({ title, onClose, children, wide, fullscreen, width }: Props) {
  if (fullscreen) {
    // 상단 헤더 높이와 사이드바 폭은 CSS 변수(--header-h, --sidebar-w)로 주입된다.
    return (
      <div className="fixed bottom-0 right-0 z-[100] flex flex-col bg-background shadow-[0_0_0_1px_var(--border)] left-[var(--sidebar-w,220px)] top-[var(--header-h,48px)]">
        <div className="flex shrink-0 items-center justify-between border-b border-border bg-card px-5 py-4">
          <span className="text-md font-medium">{title}</span>
          <button onClick={onClose} aria-label="닫기"
                  className="rounded-sm p-0.5 text-muted-foreground transition-colors hover:bg-accent hover:text-foreground focus-visible:outline-none focus-visible:shadow-focus">
            <X size={16} />
          </button>
        </div>
        <div className="flex-1 overflow-hidden">{children}</div>
      </div>
    )
  }

  return (
    <Dialog open onOpenChange={open => { if (!open) onClose() }}>
      <DialogContent style={width ? { width, maxWidth: 'calc(100vw - 40px)' } : undefined}
                     className={wide ? 'max-w-[620px]' : 'max-w-[480px]'}>
        <DialogHeader>
          <DialogTitle>{title}</DialogTitle>
        </DialogHeader>
        <div className="min-h-0 flex-1 overflow-auto p-5">{children}</div>
      </DialogContent>
    </Dialog>
  )
}
