import { type ReactNode, useEffect } from 'react'

interface Props {
  title: string
  onClose: () => void
  children: ReactNode
  wide?: boolean
  fullscreen?: boolean
  width?: number | string
}

export default function Modal({ title, onClose, children, wide, fullscreen, width }: Props) {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && onClose()
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [onClose])

  if (fullscreen) {
    // position: fixed — 상단 헤더 높이와 왼쪽 사이드바 폭은 CSS 변수
    // (--header-h, --sidebar-w) 로 주입. app-content 의 스크롤에 영향받지 않음.
    return (
      <div className="modal-fullscreen">
        <div className="modal-header">
          <span className="modal-title">{title}</span>
          <button className="modal-close" onClick={onClose}>✕</button>
        </div>
        <div className="modal-body-area">{children}</div>
      </div>
    )
  }

  return (
    <div className="modal-overlay" onClick={onClose}>
      <div
        className={`modal-box${wide ? ' modal-box--wide' : ''}`}
        style={width ? { width } : undefined}
        onClick={e => e.stopPropagation()}
      >
        <div className="modal-header">
          <span className="modal-title">{title}</span>
          <button className="modal-close" onClick={onClose}>✕</button>
        </div>
        <div className="modal-body">{children}</div>
      </div>
    </div>
  )
}
