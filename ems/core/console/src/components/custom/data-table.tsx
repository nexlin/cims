import type { ReactNode } from 'react'
import { cn } from '@core/lib/utils'

/**
 * 시안 표 껍데기. 정본 = Figma `02 Components` TableHeaderCell/Table +
 * `cims-design-handoff/components/contracts.md` §Table.
 *
 * 계약 4가지 — **세로 칼럼 구분선 없음** · 외곽 `border-strong` > 내부 rule `border` ·
 * 헤더는 `whitespace-nowrap`(좁아서 글자가 쪼개지지 않게 컬럼 최소폭을 잡는다) ·
 * **빈 셀은 `—`**(muted). 행 전체 배경 tint 로 상태를 나타내지 않는다 — 배지·셀 표시로 한다.
 *
 * 치수는 Figma G1 멤버 표(43:624) 실측: 헤더 37 · 행 40 · 좌우 여백 14 ·
 * 헤더 12px SemiBold muted on `neutral-soft` · 본문 13px Medium.
 * 기존 `.data-table`(index.css) 은 헤더를 `uppercase` 로 만들어 `Agent` 가 `AGENT` 가 되고
 * 헤더 배경도 `--muted` 로 더 옅다 — 시안 표는 이 컴포넌트를 쓴다.
 */
export function DataTable({ children, className }: { children: ReactNode; className?: string }) {
  return (
    // 라운드 모서리가 헤더 배경을 잘라내야 해서 overflow-hidden 이 필요하다.
    // 컬럼이 좁아지지 않게 표는 넘치면 가로 스크롤한다 (본문 세로 스크롤과 섞이지 않게 여기서만).
    <div className={cn('overflow-x-auto rounded-sm border border-border-strong', className)}>
      <table className="w-full border-collapse">{children}</table>
    </div>
  )
}

/** 헤더 셀. `align="right"` 은 수치·버전처럼 우측 정렬하는 컬럼에. */
export function Th({ children, className, align = 'left', width, title }: {
  children?: ReactNode
  className?: string
  align?: 'left' | 'right' | 'center'
  width?: number
  title?: string
}) {
  return (
    <th title={title} style={width !== undefined ? { width } : undefined}
        className={cn('h-[37px] whitespace-nowrap bg-neutral-soft px-3.5 text-sm font-semibold text-muted-foreground',
                      align === 'right' ? 'text-right' : align === 'center' ? 'text-center' : 'text-left',
                      className)}>
      {children}
    </th>
  )
}

/** 본문 셀. `mono` 는 IP·버전·경로처럼 자리수를 맞춰 읽는 값에. */
export function Td({ children, className, align = 'left', mono, colSpan, onClick, title }: {
  children?: ReactNode
  className?: string
  align?: 'left' | 'right' | 'center'
  mono?: boolean
  colSpan?: number
  onClick?: () => void
  title?: string
}) {
  return (
    <td colSpan={colSpan} onClick={onClick} title={title}
        className={cn('h-10 border-t border-border px-3.5 text-md font-medium text-foreground',
                      mono && 'font-mono text-sm font-normal',
                      align === 'right' ? 'text-right' : align === 'center' ? 'text-center' : 'text-left',
                      className)}>
      {children}
    </td>
  )
}

/** 값이 없는 셀은 표 전체에서 같은 기호로 — 계약이 정한 `—`(muted). */
export function orDash(v: string | number | null | undefined) {
  return v === null || v === undefined || v === '' ? <span className="text-muted-foreground">—</span> : v
}
