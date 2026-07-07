import { createContext, useContext, useState, type ReactNode, useCallback } from 'react'

interface ToastMsg { id: number; text: string; type: 'ok' | 'err' }

interface ToastCtx { show: (text: string, type?: 'ok' | 'err') => void }

const Ctx = createContext<ToastCtx>({ show: () => {} })

export function ToastProvider({ children }: { children: ReactNode }) {
  const [msgs, setMsgs] = useState<ToastMsg[]>([])

  const show = useCallback((text: string, type: 'ok' | 'err' = 'ok') => {
    const id = Date.now()
    setMsgs(m => [...m, { id, text, type }])
    setTimeout(() => setMsgs(m => m.filter(x => x.id !== id)), 3000)
  }, [])

  return (
    <Ctx.Provider value={{ show }}>
      {children}
      <div className="toast-stack">
        {msgs.map(m => (
          <div key={m.id} className={`toast toast--${m.type}`}>{m.text}</div>
        ))}
      </div>
    </Ctx.Provider>
  )
}

export const useToast = () => useContext(Ctx)
