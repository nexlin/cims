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
  const cls = tone === 'danger' ? 'btn--danger' : tone === 'primary' ? 'btn--primary' : 'btn--ghost'
  return (
    <button title={title} aria-label={title} onClick={onClick} disabled={disabled}
      className={`btn btn--sm ${cls}`}
      style={{ padding: '3px 6px', display: 'inline-flex', alignItems: 'center', lineHeight: 0 }}>
      {children}
    </button>
  )
}
