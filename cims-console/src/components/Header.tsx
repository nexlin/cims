import { Menu, KeyRound, LogOut, Radio } from 'lucide-react'

interface HeaderProps {
  userName: string
  userRole: string
  onToggleSidebar: () => void
  onLogout: () => void
  onChangePw: () => void
}

export default function Header({ userName, userRole, onToggleSidebar, onLogout, onChangePw }: HeaderProps) {
  return (
    <header className="app-header">
      <div className="app-header-left">
        <button className="app-header-toggle" onClick={onToggleSidebar} aria-label="사이드바 토글">
          <Menu size={20} />
        </button>
        <div className="app-header-logo">
          <Radio size={20} />
          <span className="app-header-logo-text">CIMS</span>
        </div>
      </div>
      <div className="app-header-right">
        <span className="app-header-user-name">{userName}</span>
        <span className={`badge ${userRole === 'admin' ? 'badge--blue' : 'badge--gray'}`}>
          {userRole === 'admin' ? '관리자' : '사용자'}
        </span>
        <button className="btn btn--ghost btn--sm" onClick={onChangePw} title="비밀번호 변경">
          <KeyRound size={16} />
        </button>
        <button className="btn btn--ghost btn--sm" onClick={onLogout} title="로그아웃">
          <LogOut size={16} />
          <span style={{ marginLeft: 4 }}>로그아웃</span>
        </button>
      </div>
    </header>
  )
}
