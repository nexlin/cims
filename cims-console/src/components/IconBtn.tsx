// 공용 아이콘 버튼 — 테이블 행 액션(편집/삭제/추가 등)용 소형 버튼.
// 워크벤치들(ProvisioningWorkbench/PttGroupsWorkbench)에 중복 정의돼 있던 것을 통합.
import React from 'react'

export default function IconBtn({ title, onClick, tone, disabled, children }: {
  title: string
  onClick: () => void
  tone?: 'primary' | 'danger' | 'default'
  disabled?: boolean
  children: React.ReactNode
}) {
  // danger 는 솔리드 빨강 대신 ghost+빨간 아이콘 — 행마다 반복되는 위험 액션이
  // 화면을 지배하지 않도록 (hover 시에만 배경 강조).
  const cls = tone === 'primary' ? 'btn--primary' : 'btn--ghost'
  return (
    <button title={title} aria-label={title} onClick={onClick} disabled={disabled}
      className={`btn btn--sm ${cls}${tone === 'danger' ? ' icon-btn--danger' : ''}`}
      style={{ padding: '3px 6px', display: 'inline-flex', alignItems: 'center', lineHeight: 0,
               ...(tone === 'danger' ? { color: 'var(--danger, #c0392b)' } : {}) }}>
      {children}
    </button>
  )
}
