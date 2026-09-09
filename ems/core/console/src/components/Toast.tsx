import { X } from 'lucide-react'
import { createContext, useContext, useState, type ReactNode, useCallback } from 'react'
import { cn } from '@core/lib/utils'

/**
 * 우측 하단 알림. 정본 = Figma `02 Components` Sec/Toast (393:116) —
 * **좌측 3px 강조선 + 톤 점 + 제목/본문 + ×N 배지 + 닫기**, 폭 380, 그림자 `Elevation/lg`.
 * 계약 설명 그대로: *"같은 원인은 한 장으로 묶고 건수는 ×N 배지로 — 노드 수만큼 쌓지 않는다.
 * 화면에 최대 3장."*
 *
 * 옮기기 전에는 `.toast` CSS 한 덩어리였고 `toast--ok/err/alarm` 규칙이 **아예 없어서
 * 톤이 그려지지 않았다** — 성공도 실패도 같은 흰 상자였다.
 *
 * 라운드는 토큰(`--radius` 8)을 쓴다. 도안 인스턴스는 10 이지만 반경 스케일에 없는 값이라
 * 토큰을 따랐다(§3-1 hex·off-token 금지와 같은 판단).
 */
type Tone = 'ok' | 'err' | 'alarm'

interface ToastMsg {
  id: number
  text: string
  type: Tone
  count: number
  sticky?: boolean               // 수동 닫기 (알람 토스트 — alarm_pipeline.md §8.2)
  onClick?: () => void
}

interface ToastOpts { sticky?: boolean; onClick?: () => void }

interface ToastCtx { show: (text: string, type?: Tone, opts?: ToastOpts) => void }

const Ctx = createContext<ToastCtx>({ show: () => {} })

/** 톤 → 강조선·점 색 (DESIGN-RULES §2 고정 매핑). */
const ACCENT: Record<Tone, string> = {
  ok: 'bg-success',
  err: 'bg-destructive',
  alarm: 'bg-warning',
}

/** 화면에 동시에 띄우는 최대 장수 (계약 393:116). */
const MAX_VISIBLE = 3

let _seq = 0

export function ToastProvider({ children }: { children: ReactNode }) {
  const [msgs, setMsgs] = useState<ToastMsg[]>([])

  const show = useCallback((text: string, type: Tone = 'ok', opts?: ToastOpts) => {
    setMsgs(m => {
      // 같은 원인(같은 문구·톤)은 새 장을 쌓지 않고 **건수만 올린다**.
      const same = m.find(x => x.text === text && x.type === type)
      if (same) return m.map(x => x === same ? { ...x, count: x.count + 1 } : x)
      const id = ++_seq
      if (!opts?.sticky) setTimeout(() => setMsgs(q => q.filter(x => x.id !== id)), 3000)
      const next = [...m, { id, text, type, count: 1, sticky: opts?.sticky, onClick: opts?.onClick }]
      // 넘치면 **가장 오래된 것**부터 밀어낸다 — 방금 일어난 일이 항상 보여야 한다.
      return next.slice(-MAX_VISIBLE)
    })
  }, [])

  const dismiss = (id: number) => setMsgs(m => m.filter(x => x.id !== id))

  return (
    <Ctx.Provider value={{ show }}>
      {children}
      <div className="fixed bottom-6 right-6 z-[200] flex flex-col gap-2">
        {msgs.map(m => (
          <div key={m.id}
               className={cn('flex w-[380px] items-start overflow-hidden rounded-md border border-border',
                             'bg-card shadow-lg animate-in slide-in-from-right-10 fade-in duration-200',
                             m.onClick && 'cursor-pointer')}
               onClick={() => { m.onClick?.(); if (m.sticky) dismiss(m.id) }}>
            <span className={cn('w-[3px] shrink-0 self-stretch', ACCENT[m.type])} aria-hidden />
            <div className="flex min-w-0 flex-1 items-center gap-2.5 px-3 py-[11px]">
              <span className={cn('size-2 shrink-0 rounded-full', ACCENT[m.type])} aria-hidden />
              <span className="min-w-0 flex-1 text-sm font-semibold text-foreground">{m.text}</span>
              {m.count > 1 && (
                <span className="shrink-0 rounded-full bg-neutral-soft px-[7px] py-[3px] text-xs font-semibold text-neutral-on">
                  ×{m.count}
                </span>
              )}
              {m.sticky && (
                <button className="shrink-0 text-muted-foreground hover:text-foreground" aria-label="닫기"
                        onClick={e => { e.stopPropagation(); dismiss(m.id) }}><X size={14} /></button>
              )}
            </div>
          </div>
        ))}
      </div>
    </Ctx.Provider>
  )
}

export const useToast = () => useContext(Ctx)
