import { useRef } from 'react'
import { X } from 'lucide-react'
import { cn } from '@core/lib/utils'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'

/**
 * 검색 칸 + 끝의 [×] 지우기. 시안 TextInput(17:58)에 ghost 아이콘 Button 을 얹은 **조합**이다 —
 * `02 Components` 에 「SearchInput」이 따로 있는 게 아니라 목록의 두 컴포넌트를 겹쳐 놓았다
 * (`prompt.tsx` 와 같은 방식). [×] 는 값이 있을 때만 보이고, 누르면 비운 뒤 칸에 포커스를 돌린다.
 *
 * `className` 은 바깥 틀(폭·flex)에 간다 — 기존 `<Input className="flex-1 w-[…]">` 자리에 그대로 바꿔 넣는다.
 */
export function SearchInput({ value, onChange, placeholder, className }: {
  value: string
  onChange: (v: string) => void
  placeholder?: string
  className?: string
}) {
  const ref = useRef<HTMLInputElement>(null)
  return (
    <div className={cn('relative', className)}>
      <Input ref={ref} className="pr-8" placeholder={placeholder} value={value}
             onChange={e => onChange(e.target.value)}
             onKeyDown={e => { if (e.key === 'Escape' && value) { e.preventDefault(); onChange('') } }}/>
      {value && (
        <Button variant="ghost" size="iconSm" aria-label="검색어 지우기" title="검색어 지우기"
                className="absolute right-1 top-1/2 -translate-y-1/2"
                onClick={() => { onChange(''); ref.current?.focus() }}>
          <X />
        </Button>
      )}
    </div>
  )
}
