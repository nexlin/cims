import React, { useMemo, useState } from 'react'

// ── 공통 데이터 테이블 ────────────────────────────────────────
//  구성(조직/사용자/번호/PTT그룹) 4페이지의 중복 테이블 로직을 흡수하는 단일 컴포넌트.
//  - 고정 헤더(sticky) + 열 정렬(asc/desc/off) + 다중 선택 + 클라 페이징 + 활성행 하이라이트
//  - 인라인 편집은 render() 안에서 자유롭게(편집행은 호출부가 결정), 본 컴포넌트는 표 골격만 제공.

export interface Column<T> {
  key: string
  header: React.ReactNode
  width?: number | string
  align?: 'left' | 'center' | 'right'
  sortable?: boolean
  /** 셀 렌더. 미지정 시 (row as any)[key] 표시 */
  render?: (row: T) => React.ReactNode
  /** 정렬 기준 값(미지정 시 render 불가 → key 원시값) */
  sortValue?: (row: T) => string | number
}

interface DataTableProps<T> {
  columns: Column<T>[]
  rows: T[]
  rowKey: (row: T) => string | number
  loading?: boolean
  emptyText?: string

  // 선택
  selectable?: boolean
  selected?: Set<string | number>
  onSelectChange?: (next: Set<string | number>) => void

  // 행 클릭(상세 드로어 열기 등)
  onRowClick?: (row: T) => void
  activeRowKey?: string | number | null

  // 행 확장 — key 가 일치하는 행 바로 아래에 상세/편집을 인라인 렌더
  expandedKey?: string | number | null
  renderExpanded?: (row: T) => React.ReactNode

  // 페이징 (0/미지정 = 페이징 없음)
  pageSize?: number

  // 표 아래 좌측에 끼워넣을 추가 노드(예: '＋ 추가' 행)
  footer?: React.ReactNode
}

type SortDir = 'asc' | 'desc' | null

export function DataTable<T>(props: DataTableProps<T>) {
  const {
    columns, rows, rowKey, loading, emptyText = '항목이 없습니다',
    selectable, selected, onSelectChange, onRowClick, activeRowKey,
    expandedKey, renderExpanded,
    pageSize = 0, footer,
  } = props

  const [sortKey, setSortKey] = useState<string | null>(null)
  const [sortDir, setSortDir] = useState<SortDir>(null)
  const [page, setPage] = useState(0)

  // rows 가 줄면 현재 페이지가 범위를 벗어날 수 있어 보정
  const sorted = useMemo(() => {
    if (!sortKey || !sortDir) return rows
    const col = columns.find(c => c.key === sortKey)
    if (!col) return rows
    const val = col.sortValue ?? ((r: T) => {
      const v = (r as Record<string, unknown>)[sortKey]
      return (typeof v === 'number' ? v : String(v ?? '')) as string | number
    })
    const arr = [...rows]
    arr.sort((a, b) => {
      const av = val(a), bv = val(b)
      let c: number
      if (typeof av === 'number' && typeof bv === 'number') c = av - bv
      else c = String(av).localeCompare(String(bv))
      return sortDir === 'asc' ? c : -c
    })
    return arr
  }, [rows, columns, sortKey, sortDir])

  const totalPages = pageSize > 0 ? Math.max(1, Math.ceil(sorted.length / pageSize)) : 1
  // rows 가 줄어 현재 page 가 범위를 벗어나면 파생값으로 클램프(effect 불필요)
  const safePage = Math.min(page, totalPages - 1)
  const pageRows = pageSize > 0 ? sorted.slice(safePage * pageSize, safePage * pageSize + pageSize) : sorted

  function toggleSort(c: Column<T>) {
    if (!c.sortable) return
    if (sortKey !== c.key) { setSortKey(c.key); setSortDir('asc'); return }
    setSortDir(d => d === 'asc' ? 'desc' : d === 'desc' ? null : 'asc')
    if (sortDir === 'desc') setSortKey(null)
  }

  const allOnPageSelected = selectable && pageRows.length > 0 && pageRows.every(r => selected?.has(rowKey(r)))
  function toggleSelectAll() {
    if (!onSelectChange) return
    const next = new Set(selected)
    if (allOnPageSelected) pageRows.forEach(r => next.delete(rowKey(r)))
    else pageRows.forEach(r => next.add(rowKey(r)))
    onSelectChange(next)
  }
  function toggleSelectOne(k: string | number) {
    if (!onSelectChange) return
    const next = new Set(selected)
    if (next.has(k)) next.delete(k); else next.add(k)
    onSelectChange(next)
  }

  const colCount = columns.length + (selectable ? 1 : 0)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', minHeight: 0, flex: 1 }}>
      <div className="table-wrap" style={{ overflowY: 'auto', flex: 1, minHeight: 0 }}>
      <table className="data-table">
        <thead>
          <tr>
            {selectable && (
              <th style={{ width: 36, position: 'sticky', top: 0, zIndex: 2 }}>
                <input type="checkbox" checked={!!allOnPageSelected} onChange={toggleSelectAll} />
              </th>
            )}
            {columns.map(c => (
              <th key={c.key}
                style={{ width: c.width, textAlign: c.align, cursor: c.sortable ? 'pointer' : undefined, userSelect: 'none', position: 'sticky', top: 0, zIndex: 2 }}
                onClick={() => toggleSort(c)}>
                {c.header}
                {c.sortable && sortKey === c.key && (
                  <span style={{ marginLeft: 4, fontSize: 10 }}>{sortDir === 'asc' ? '▲' : sortDir === 'desc' ? '▼' : ''}</span>
                )}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {loading ? (
            <tr><td colSpan={colCount} className="empty-cell" style={{ textAlign: 'center', padding: 24 }}>로딩 중...</td></tr>
          ) : pageRows.length === 0 ? (
            <tr><td colSpan={colCount} className="empty-cell" style={{ textAlign: 'center', padding: 24 }}>{emptyText}</td></tr>
          ) : pageRows.map(r => {
            const k = rowKey(r)
            const isActive = activeRowKey != null && activeRowKey === k
            const isSel = selected?.has(k)
            const isExpanded = expandedKey != null && expandedKey === k && !!renderExpanded
            return (
              <React.Fragment key={k}>
              <tr
                onClick={onRowClick ? () => onRowClick(r) : undefined}
                style={{
                  cursor: onRowClick ? 'pointer' : undefined,
                  background: (isActive || isExpanded) ? 'rgba(74,144,217,0.15)' : isSel ? 'rgba(74,144,217,0.08)' : undefined,
                }}>
                {selectable && (
                  <td onClick={e => e.stopPropagation()}>
                    <input type="checkbox" checked={!!isSel} onChange={() => toggleSelectOne(k)} />
                  </td>
                )}
                {columns.map(c => (
                  <td key={c.key} style={{ textAlign: c.align }}>
                    {c.render ? c.render(r) : String((r as Record<string, unknown>)[c.key] ?? '')}
                  </td>
                ))}
              </tr>
              {isExpanded && (
                <tr className="row--expanded">
                  <td colSpan={colCount} style={{ padding: 0, background: 'var(--muted)', boxShadow: 'inset 0 4px 6px -5px rgba(0,0,0,0.35)' }}>
                    {renderExpanded!(r)}
                  </td>
                </tr>
              )}
              </React.Fragment>
            )
          })}
          {footer}
        </tbody>
      </table>
      </div>

      {pageSize > 0 && sorted.length > pageSize && (
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '8px 12px', fontSize: 12, color: 'var(--muted-foreground)', borderTop: '1px solid var(--border)' }}>
          <span>{sorted.length}건</span>
          <span style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 6 }}>
            <button className="btn btn--ghost btn--sm" disabled={safePage === 0} onClick={() => setPage(Math.max(0, safePage - 1))}>◀</button>
            <span>{safePage + 1} / {totalPages}</span>
            <button className="btn btn--ghost btn--sm" disabled={safePage >= totalPages - 1} onClick={() => setPage(Math.min(totalPages - 1, safePage + 1))}>▶</button>
          </span>
        </div>
      )}
    </div>
  )
}

export default DataTable
