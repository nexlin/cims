// 왼쪽 목록 레일 — 결과 화면의 run 레일과 같은 꼴(검색·칩·행 목록). 토폴로지·시나리오 화면이 드롭다운 대신 이걸로 레코드를 고른다.
// 접으면 36px 띠(아이콘 + 세로 라벨)만 남고 상태는 localStorage 에 기억한다. 행 렌더는 호출부(children).
// `tabs` 를 주면 한 열에 탭 여러 개 — 첫 탭(main)은 목록(검색·칩·action·children), 나머지는 호출부가 준 content. 접은 띠에는 탭마다
// 아이콘 하나(누르면 그 탭으로 펼친다). 토폴로지 화면이 [토폴로지 | 팔레트] 로 쓴다 — 접는 띠가 둘 나란히 서지 않게.
import { useState, type ReactNode } from 'react'
import { PanelLeftClose, PanelLeftOpen } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'

export interface RailTab { key: string; label: string; icon: ReactNode; badge?: ReactNode; content?: ReactNode }

export default function ListRail({ storageKey, label, search, chips, action, children, width = 260, tabs, tab, onTab }: {
  storageKey: string
  label: string
  search?: { value: string; onChange: (v: string) => void; placeholder: string }
  chips?: ReactNode
  action?: ReactNode
  children: ReactNode
  width?: number
  /** 탭 여러 개 — 첫 항목이 목록 탭(content 없음), 나머지는 content 를 그린다 */
  tabs?: RailTab[]
  tab?: string
  onTab?: (key: string) => void
}) {
  const [open, setOpen] = useState(() => { try { return localStorage.getItem(storageKey) !== '0' } catch { return true } })
  const setOpenKeep = (o: boolean) => { try { localStorage.setItem(storageKey, o ? '1' : '0') } catch { /* 무시 */ } setOpen(o) }
  const toggle = () => setOpenKeep(!open)
  const [innerTab, setInnerTab] = useState(tabs?.[0]?.key ?? '')
  const cur = tab ?? innerTab
  const pickTab = (k: string) => { setInnerTab(k); onTab?.(k) }
  const active = tabs?.find(t => t.key === cur) ?? tabs?.[0]
  const isMain = !tabs || !active || active === tabs[0]

  if (!open) return (
    <aside className="flex w-[36px] shrink-0 flex-col items-center gap-1 border-r border-border bg-card py-2">
      {tabs ? tabs.map(t => (
        <Button key={t.key} variant="ghost" size="iconSm" onClick={() => { pickTab(t.key); setOpenKeep(true) }} title={`${t.label} 펼치기`}>{t.icon}</Button>
      )) : <Button variant="ghost" size="iconSm" onClick={toggle} title={`${label} 목록 펼치기`}><PanelLeftOpen size={14} /></Button>}
      <span className="mt-1 text-[11px] text-muted-foreground [writing-mode:vertical-rl]">{tabs ? tabs.map(t => t.label).join(' · ') : label}</span>
    </aside>
  )
  return (
    <aside className="flex shrink-0 flex-col border-r border-border bg-card" style={{ width }}>
      {tabs && (
        <div className="flex items-stretch border-b border-border">
          {tabs.map(t => (
            <button key={t.key} onClick={() => pickTab(t.key)}
                    className={`flex flex-1 items-center justify-center gap-1.5 border-b-2 px-2 py-1.5 text-xs ${t.key === active?.key ? 'border-primary font-medium text-foreground' : 'border-transparent text-muted-foreground hover:bg-accent'}`}>
              {t.icon}{t.label}{t.badge}
            </button>
          ))}
          <Button variant="ghost" size="iconSm" className="m-0.5 shrink-0" onClick={toggle} title="접기"><PanelLeftClose size={14} /></Button>
        </div>
      )}
      {isMain ? (
        <>
          <div className="flex flex-col gap-1.5 border-b border-border p-2">
            <div className="flex items-center gap-1">
              {search && <Input value={search.value} onChange={e => search.onChange(e.target.value)} placeholder={search.placeholder} className="h-[28px] min-w-0 flex-1 text-sm" />}
              {!search && <span className="text-sm font-semibold">{label}</span>}
              {!tabs && <Button variant="ghost" size="iconSm" onClick={toggle} title="목록 접기"><PanelLeftClose size={14} /></Button>}
            </div>
            {chips && <div className="flex flex-wrap gap-1">{chips}</div>}
            {action}
          </div>
          <div className="min-h-0 flex-1 overflow-auto">{children}</div>
        </>
      ) : (
        <div className="flex min-h-0 flex-1 flex-col overflow-auto">{active?.content}</div>
      )}
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
