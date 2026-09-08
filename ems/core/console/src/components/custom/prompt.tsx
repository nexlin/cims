import { createContext, useCallback, useContext, useEffect, useRef, useState, type ReactNode } from 'react'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import {
  Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle,
} from '@core/components/ui/dialog'

/**
 * 값 하나를 받는 대화상자. `confirm.tsx` 와 **같은 방식** — 시안 Modal(392:108) 껍데기에
 * TextInput(17:58)을 한 칸 넣은 조합이다. `02 Components` 에 「Prompt」 컴포넌트가 따로
 * 있는 게 아니라, 목록에 있는 두 컴포넌트를 조합한 것이다(새 컴포넌트를 만든 게 아니다).
 *
 * 브라우저 네이티브 `window.prompt()` 를 대체한다. 네이티브는 토큰·포커스 링이 안 먹고
 * 입력 종류(비밀번호)도 못 정하며 문구에 서식을 넣을 수 없다.
 * **어느 액션에 붙일지와 문구는 현행 유지** — 시안이 정하지 않았으므로 옮기기만 한다.
 *
 *   const prompt = usePrompt()
 *   const v = await prompt({ title: '서버 이름', defaultValue: a.name })
 *   if (v === null) return          // 취소 — 네이티브와 같은 규약
 */
export interface PromptOptions {
  title: string
  /** 본문 설명. 줄바꿈이 필요하면 노드로 넘긴다 (네이티브의 `\n` 대체). */
  body?: ReactNode
  defaultValue?: string
  placeholder?: string
  /** 비밀번호를 받을 때 `password`. */
  type?: 'text' | 'password' | 'number'
  confirmLabel?: string
  cancelLabel?: string
}

type Ask = (o: PromptOptions) => Promise<string | null>

const Ctx = createContext<Ask>(async () => null)

export function PromptProvider({ children }: { children: ReactNode }) {
  const [opts, setOpts] = useState<PromptOptions | null>(null)
  const [value, setValue] = useState('')
  const resolver = useRef<((v: string | null) => void) | null>(null)

  const ask = useCallback<Ask>((o) => {
    setOpts(o)
    setValue(o.defaultValue ?? '')
    return new Promise<string | null>(resolve => { resolver.current = resolve })
  }, [])

  // 닫히는 경로가 셋이다(취소·확인·바깥/Esc) — 어느 쪽이든 약속을 매듭짓는다.
  // 안 그러면 `await` 이 영영 안 풀려 호출부가 멈춘 채로 남는다.
  const settle = (v: string | null) => {
    resolver.current?.(v)
    resolver.current = null
    setOpts(null)
  }

  // 열릴 때마다 입력칸을 잡아 준다 — 네이티브 prompt 는 바로 타이핑할 수 있었다.
  const inputRef = useRef<HTMLInputElement>(null)
  useEffect(() => { if (opts) inputRef.current?.focus() }, [opts])

  return (
    <Ctx.Provider value={ask}>
      {children}
      <Dialog open={opts !== null} onOpenChange={open => { if (!open) settle(null) }}>
        {opts && (
          <DialogContent onEscapeKeyDown={() => settle(null)}>
            <DialogHeader>
              <DialogTitle>{opts.title}</DialogTitle>
            </DialogHeader>
            <DialogDescription asChild>
              <div className="flex flex-col gap-2 px-5 py-4">
                {opts.body && <div className="text-md text-muted-foreground">{opts.body}</div>}
                <Input ref={inputRef} type={opts.type ?? 'text'} value={value}
                       placeholder={opts.placeholder}
                       onChange={e => setValue(e.target.value)}
                       onKeyDown={e => { if (e.key === 'Enter') settle(value) }} />
              </div>
            </DialogDescription>
            <DialogFooter>
              <Button variant="outline" onClick={() => settle(null)}>
                {opts.cancelLabel ?? '취소'}
              </Button>
              <Button variant="default" onClick={() => settle(value)}>
                {opts.confirmLabel ?? '확인'}
              </Button>
            </DialogFooter>
          </DialogContent>
        )}
      </Dialog>
    </Ctx.Provider>
  )
}

/** `const prompt = usePrompt()` → `await prompt({ … })` (취소면 `null`) */
export const usePrompt = () => useContext(Ctx)
