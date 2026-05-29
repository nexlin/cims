import { useState } from 'react'
import { KeyRound, LogOut, Radio, Sun, Moon } from 'lucide-react'
import { getInitialTheme, applyTheme, type Theme } from '../theme'

interface HeaderProps {
  userName: string
  userRole: string
  onLogout: () => void
  onChangePw: () => void
}

export default function Header({ userName, userRole, onLogout, onChangePw }: HeaderProps) {
  const [theme, setTheme] = useState<Theme>(getInitialTheme)
  const toggleTheme = () => {
    const next: Theme = theme === 'dark' ? 'light' : 'dark'
    applyTheme(next); setTheme(next)
  }
  return (
    <header className="app-header">
      <div className="app-header-left">
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
        <button className="btn btn--ghost btn--sm" onClick={toggleTheme}
                title={theme === 'dark' ? '라이트 모드로' : '다크 모드로'}>
          {theme === 'dark' ? <Sun size={16} /> : <Moon size={16} />}
        </button>
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
