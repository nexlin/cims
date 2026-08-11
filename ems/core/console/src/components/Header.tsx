import { useState } from 'react'
import { KeyRound, LogOut, Radio, Sun, Moon, Code } from 'lucide-react'
import { useDevMode } from '../hooks/useDevMode'
import { setDevMode } from '../utils/devMode'
import { getInitialTheme, applyTheme, type Theme } from '../theme'
import { ROLE_LABELS, roleRank } from '../utils/permissions'
import type { Role } from '../api/auth'
import AlarmIndicator from './AlarmIndicator'

interface HeaderProps {
  userName: string
  userRole: string
  onLogout: () => void
  onChangePw: () => void
}

export default function Header({ userName, userRole, onLogout, onChangePw }: HeaderProps) {
  const [theme, setTheme] = useState<Theme>(getInitialTheme)
  const devMode = useDevMode()
  const isAdminRank = roleRank(userRole) >= roleRank('admin')
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
      {/* 페이지 위젯 편집 컨트롤 슬롯 — EditableLayout 이 portal 로 렌더 */}
      <div id="layout-edit-slot" className="app-header-editslot" />
      <div className="app-header-right">
        {/* 알람 인디케이터 — 셸 상주, 0건도 회색 배지 상시 (alarm_pipeline.md §8.2) */}
        <AlarmIndicator />
        <span className="app-header-user-name">{userName}</span>
        <span className={`badge ${roleRank(userRole) >= 4 ? 'badge--blue' : 'badge--gray'}`}>
          {ROLE_LABELS[userRole as Role] ?? userRole}
        </span>
        {isAdminRank && devMode && (
          <span className="badge" style={{ background: '#7c3aed', color: '#fff' }}>개발자 모드</span>
        )}
        {isAdminRank && (
          <button className="btn btn--ghost btn--sm" onClick={() => setDevMode(!devMode)}
                  title={devMode ? '개발자 모드 끄기 (릴리스 메뉴 숨김)' : '개발자 모드 켜기 — 빌드·검증·패키징 메뉴 노출'}
                  style={devMode ? { color: '#7c3aed' } : undefined}>
            <Code size={16} />
          </button>
        )}
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
