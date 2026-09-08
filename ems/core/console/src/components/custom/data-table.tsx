import React, { type ReactNode, type ThHTMLAttributes, type TdHTMLAttributes } from 'react'
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
export function DataTable({ children, className, sticky }: {
  children: ReactNode
  className?: string
  /**
   * 바깥(위젯 패널·`.scroll-fill`)이 세로로 스크롤하는 표 — 헤더를 위에 붙여 둔다.
   * 이때 감싼 div 에 `overflow` 를 주면 **그 div 가 스크롤포트가 되어** sticky 가
   * 바깥 스크롤을 못 따라간다. 그래서 가로 스크롤을 포기하고 모서리만 헤더 셀에서 둥글린다.
   */
  sticky?: boolean
}) {
  return (
    // 라운드 모서리가 헤더 배경을 잘라내야 해서 overflow-hidden 이 필요하다.
    // 컬럼이 좁아지지 않게 표는 넘치면 가로 스크롤한다 (본문 세로 스크롤과 섞이지 않게 여기서만).
    <div className={cn('rounded-sm border border-border-strong', !sticky && 'overflow-x-auto', className)}>
      <table className={cn('w-full border-collapse',
                           sticky && '[&_th]:sticky [&_th]:top-0 [&_th]:z-[2] [&_th:first-child]:rounded-tl-sm [&_th:last-child]:rounded-tr-sm')}>
        {children}
      </table>
    </div>
  )
}

/** 헤더 셀. `align="right"` 은 수치·버전처럼 우측 정렬하는 컬럼에.
 *  `sticky` 는 스크롤 컨테이너가 표 바깥(위젯 패널)일 때 헤더를 붙여 둔다. */
export function Th({ children, className, align = 'left', width, sticky, style, ...rest }: {
  children?: ReactNode
  align?: 'left' | 'right' | 'center'
  width?: number
  sticky?: boolean
} & Omit<ThHTMLAttributes<HTMLTableCellElement>, 'align' | 'width' | 'children'>) {
  return (
    <th {...rest} style={width !== undefined ? { width, ...style } : style}
        className={cn('h-[37px] whitespace-nowrap bg-neutral-soft px-3.5 text-sm font-semibold text-muted-foreground',
                      align === 'right' ? 'text-right' : align === 'center' ? 'text-center' : 'text-left',
                      sticky && 'sticky top-0 z-[2]',
                      className)}>
      {children}
    </th>
  )
}

/** 본문 셀. `mono` 는 IP·버전·경로처럼 자리수를 맞춰 읽는 값에. */
export function Td({ children, className, align = 'left', mono, ...rest }: {
  children?: ReactNode
  align?: 'left' | 'right' | 'center'
  mono?: boolean
} & Omit<TdHTMLAttributes<HTMLTableCellElement>, 'align' | 'children'>) {
  return (
    <td {...rest}
        className={cn('h-10 border-t border-border px-3.5 text-md font-medium text-foreground',
                      mono && 'font-mono text-sm font-normal',
                      align === 'right' ? 'text-right' : align === 'center' ? 'text-center' : 'text-left',
                      className)}>
      {children}
    </td>
  )
}

/** 클릭으로 상세를 여는 행. 시안이 상태를 행 배경으로 칠하지 말라고 해서(§Table) 여기 tint 는
 *  **선택·hover 피드백 전용**이다 — 값의 뜻을 나타내지 않는다. */
export function TrLink({ children, selected, className, ...rest }: {
  children?: ReactNode
  selected?: boolean
} & Omit<React.HTMLAttributes<HTMLTableRowElement>, 'children'>) {
  return (
    <tr {...rest}
        className={cn('cursor-pointer hover:bg-accent', selected && 'bg-brandsoft', className)}>
      {children}
    </tr>
  )
}

/** 값이 없는 셀은 표 전체에서 같은 기호로 — 계약이 정한 `—`(muted). */
export function orDash(v: string | number | null | undefined) {
  return v === null || v === undefined || v === '' ? <span className="text-muted-foreground">—</span> : v
}
