import { createContext, useCallback, useContext, useRef, useState, type ReactNode } from 'react'
import { Button } from '@core/components/ui/button'
import {
  Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle,
} from '@core/components/ui/dialog'

/**
 * 확인 대화상자. 정본 = Figma `02 Components` Modal (392:108) — Tone Default·Danger.
 * 머리(제목 + ✕) · 구분선 · 본문 · 푸터(취소 + 확인)이고 **되돌릴 수 없는 액션만**
 * 확인 버튼이 `destructive` 다 (contracts.md §Dialog).
 *
 * 브라우저 네이티브 `window.confirm()` 을 대체한다. 네이티브는 토큰·Lucide·포커스 링이
 * 전부 안 먹고 문구에 서식을 넣을 수 없어 §3 절대 규칙을 만족할 수 없다
 * (docs/design/console_design_system.md §7-7). **어느 액션에 붙일지와 문구는 현행 유지** —
 * 시안이 대상·문구를 정하지 않았으므로 옮기기만 한다.
 *
 * 호출부는 네이티브와 모양이 거의 같다:
 *   `if (!await confirm({ title, body, confirmLabel, tone: 'danger' })) return`
 */
export interface ConfirmOptions {
  title: string
  /** 본문. 줄바꿈이 필요한 문장은 노드로 넘긴다 (네이티브의 `\n` 대체). */
  body?: ReactNode
  confirmLabel?: string
  cancelLabel?: string
  /** `danger` = 되돌릴 수 없는 액션 (삭제·폐기·일괄 중지 등) */
  tone?: 'default' | 'danger'
}

type Ask = (o: ConfirmOptions) => Promise<boolean>

const Ctx = createContext<Ask>(async () => false)

export function ConfirmProvider({ children }: { children: ReactNode }) {
  const [opts, setOpts] = useState<ConfirmOptions | null>(null)
  const resolver = useRef<((v: boolean) => void) | null>(null)

  const ask = useCallback<Ask>((o) => {
    setOpts(o)
    return new Promise<boolean>(resolve => { resolver.current = resolve })
  }, [])

  // 닫히는 경로가 셋이다(취소·확인·바깥/Esc) — 어느 쪽이든 약속을 반드시 매듭짓는다.
  // 안 그러면 `await` 이 영영 안 풀려 호출부가 멈춘 채로 남는다.
  const settle = (v: boolean) => {
    resolver.current?.(v)
    resolver.current = null
    setOpts(null)
  }

  return (
    <Ctx.Provider value={ask}>
      {children}
      <Dialog open={opts !== null} onOpenChange={open => { if (!open) settle(false) }}>
        {/* 치수는 Figma Modal(392:108) 실측: 머리 아래 구분선 · 좌우 20 · 버튼 사이 8 */}
        {opts && (
          <DialogContent className="max-w-[560px] gap-0 p-0" onEscapeKeyDown={() => settle(false)}>
            <DialogHeader className="border-b border-border px-5 py-4">
              <DialogTitle className="text-base">{opts.title}</DialogTitle>
            </DialogHeader>
            <DialogDescription asChild>
              <div className="px-5 py-4 text-md text-muted-foreground">{opts.body}</div>
            </DialogDescription>
            <DialogFooter className="px-5 pb-4 sm:space-x-2">
              <Button variant="outline" onClick={() => settle(false)}>
                {opts.cancelLabel ?? '취소'}
              </Button>
              <Button variant={opts.tone === 'danger' ? 'destructive' : 'default'}
                      onClick={() => settle(true)} autoFocus>
                {opts.confirmLabel ?? '확인'}
              </Button>
            </DialogFooter>
          </DialogContent>
        )}
      </Dialog>
    </Ctx.Provider>
  )
}

/** `const confirm = useConfirm()` → `await confirm({ … })` */
export const useConfirm = () => useContext(Ctx)
