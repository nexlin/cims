// 왼쪽 목록 레일 — 결과 화면의 run 레일과 같은 꼴(검색·칩·행 목록). 토폴로지·시나리오 화면이 드롭다운 대신 이걸로 레코드를 고른다.
// 접으면 36px 띠(아이콘 + 세로 라벨)만 남고 상태는 localStorage 에 기억한다. 행 렌더는 호출부(children).
import { useState, type ReactNode } from 'react'
import { PanelLeftClose, PanelLeftOpen } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'

export default function ListRail({ storageKey, label, search, chips, action, children, width = 260 }: {
  storageKey: string
  label: string
  search?: { value: string; onChange: (v: string) => void; placeholder: string }
  chips?: ReactNode
  action?: ReactNode
  children: ReactNode
  width?: number
}) {
  const [open, setOpen] = useState(() => { try { return localStorage.getItem(storageKey) !== '0' } catch { return true } })
  const toggle = () => setOpen(o => { try { localStorage.setItem(storageKey, o ? '0' : '1') } catch { /* 무시 */ } return !o })
  if (!open) return (
    <aside className="flex w-[36px] shrink-0 flex-col items-center gap-2 border-r border-border bg-card py-2">
      <Button variant="ghost" size="iconSm" onClick={toggle} title={`${label} 목록 펼치기`}><PanelLeftOpen size={14} /></Button>
      <span className="text-[11px] text-muted-foreground [writing-mode:vertical-rl]">{label}</span>
    </aside>
  )
  return (
    <aside className="flex shrink-0 flex-col border-r border-border bg-card" style={{ width }}>
      <div className="flex flex-col gap-1.5 border-b border-border p-2">
        <div className="flex items-center gap-1">
          {search && <Input value={search.value} onChange={e => search.onChange(e.target.value)} placeholder={search.placeholder} className="h-[28px] min-w-0 flex-1 text-sm" />}
          {!search && <span className="text-sm font-semibold">{label}</span>}
          <Button variant="ghost" size="iconSm" onClick={toggle} title="목록 접기"><PanelLeftClose size={14} /></Button>
        </div>
        {chips && <div className="flex flex-wrap gap-1">{chips}</div>}
        {action}
      </div>
      <div className="min-h-0 flex-1 overflow-auto">{children}</div>
    </aside>
  )
}

/** 레일 행 — 선택 행은 accent, 두 줄(제목 줄·meta 줄) */
export function RailRow({ selected, onClick, top, bottom, title }: { selected: boolean; onClick: () => void; top: ReactNode; bottom?: ReactNode; title?: string }) {
  return (
    <button onClick={onClick} title={title} className={`flex w-full flex-col gap-0.5 border-b border-border px-3 py-1.5 text-left text-xs hover:bg-accent ${selected ? 'bg-accent' : ''}`}>
      <div className="flex w-full items-center gap-1.5">{top}</div>
      {bottom && <div className="flex w-full items-center gap-1.5 text-muted-foreground">{bottom}</div>}
    </button>
  )
}

/** 레일 그룹 머리(sticky) */
export function RailGroup({ children }: { children: ReactNode }) {
  return <div className="sticky top-0 z-[1] bg-muted px-3 py-1 text-[11px] font-semibold text-muted-foreground">{children}</div>
}
