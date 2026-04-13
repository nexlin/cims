import { type ReactNode, useEffect } from 'react'

interface Props {
  title: string
  onClose: () => void
  children: ReactNode
  wide?: boolean
  fullscreen?: boolean
}

export default function Modal({ title, onClose, children, wide, fullscreen }: Props) {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && onClose()
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [onClose])

  if (fullscreen) {
    // 사이드바 메뉴를 제외한 콘텐츠 영역 전체 사용 (position: absolute within app-content)
    return (
      <div style={{
        position: 'absolute', top: 0, left: 0, right: 0, bottom: 0,
        zIndex: 100, background: 'var(--bg)', display: 'flex', flexDirection: 'column',
      }}>
        <div className="modal-header" style={{ flex: '0 0 auto' }}>
          <span className="modal-title">{title}</span>
          <button className="modal-close" onClick={onClose}>✕</button>
        </div>
        <div style={{ flex: 1, overflow: 'hidden' }}>{children}</div>
      </div>
    )
  }

  return (
    <div className="modal-overlay" onClick={onClose}>
      <div
        className={`modal-box${wide ? ' modal-box--wide' : ''}`}
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
