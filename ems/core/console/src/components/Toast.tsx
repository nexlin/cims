import { createContext, useContext, useState, type ReactNode, useCallback } from 'react'

interface ToastMsg {
  id: number
  text: string
  type: 'ok' | 'err' | 'alarm'
  sticky?: boolean               // 수동 닫기 (알람 토스트 — alarm_pipeline.md §8.2)
  onClick?: () => void
}

interface ToastOpts { sticky?: boolean; onClick?: () => void }

interface ToastCtx { show: (text: string, type?: 'ok' | 'err' | 'alarm', opts?: ToastOpts) => void }

const Ctx = createContext<ToastCtx>({ show: () => {} })

let _seq = 0

export function ToastProvider({ children }: { children: ReactNode }) {
  const [msgs, setMsgs] = useState<ToastMsg[]>([])

  const show = useCallback((text: string, type: 'ok' | 'err' | 'alarm' = 'ok', opts?: ToastOpts) => {
    const id = ++_seq
    setMsgs(m => [...m, { id, text, type, sticky: opts?.sticky, onClick: opts?.onClick }])
    if (!opts?.sticky) setTimeout(() => setMsgs(m => m.filter(x => x.id !== id)), 3000)
  }, [])

  const dismiss = (id: number) => setMsgs(m => m.filter(x => x.id !== id))

  return (
    <Ctx.Provider value={{ show }}>
      {children}
      <div className="toast-stack">
        {msgs.map(m => (
          <div key={m.id} className={`toast toast--${m.type}`}
               style={m.onClick ? { cursor: 'pointer' } : undefined}
               onClick={() => { m.onClick?.(); if (m.sticky) dismiss(m.id) }}>
            {m.text}
            {m.sticky && (
              <button className="toast-close" aria-label="닫기"
                      onClick={e => { e.stopPropagation(); dismiss(m.id) }}>✕</button>
            )}
          </div>
        ))}
      </div>
    </Ctx.Provider>
  )
}

export const useToast = () => useContext(Ctx)
