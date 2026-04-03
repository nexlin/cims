import { useState } from 'react'
import { AuthProvider, useAuth } from './contexts/AuthContext'
import { ToastProvider } from './components/Toast'
import UsersPage from './pages/UsersPage'
import GroupsPage from './pages/GroupsPage'
import CallLogsPage from './pages/CallLogsPage'
import LoginPage from './pages/LoginPage'
import DocsPage from './pages/DocsPage'
import DashboardPage from './pages/DashboardPage'
import ServiceStatusPage from './pages/ServiceStatusPage'
import StatsPage from './pages/StatsPage'
import { authApi } from './api/auth'
import './index.css'

type Tab = 'dashboard' | 'users' | 'groups' | 'service' | 'history' | 'stats' | 'docs'

function Shell() {
  const { user, loading, logout, refresh } = useAuth()
  const [tab,       setTab]       = useState<Tab>('dashboard')
  const [showChgPw, setShowChgPw] = useState(false)
  const [chgError,  setChgError]  = useState('')
  const [chgOk,     setChgOk]     = useState('')
  const [oldPw, setOldPw] = useState('')
  const [newPw, setNewPw] = useState('')
  const [newPw2, setNewPw2] = useState('')

  if (loading) return <div className="auth-loading">로딩 중...</div>
  if (!user)   return <LoginPage />

  const isAdmin = user.role === 'admin'

  async function handleChangePassword(e: React.FormEvent) {
    e.preventDefault()
    setChgError(''); setChgOk('')
    if (newPw !== newPw2) { setChgError('새 비밀번호가 일치하지 않습니다'); return }
    if (newPw.length < 4) { setChgError('4자 이상이어야 합니다'); return }
    try {
      await authApi.changePassword(oldPw, newPw)
      await refresh()
      setChgOk('비밀번호가 변경되었습니다')
      setOldPw(''); setNewPw(''); setNewPw2('')
      setTimeout(() => { setShowChgPw(false); setChgOk('') }, 1500)
    } catch (err: unknown) {
      setChgError((err as Error).message)
    }
  }

  return (
    <ToastProvider>
      <div className="app">
        <header className="app-header">
          <div className="app-logo">
            <span className="app-logo-icon">📡</span>
            <span className="app-logo-text">CIMS Console</span>
          </div>
          <nav className="tab-nav">
            {isAdmin && (
              <>
                <button className={`tab-btn${tab === 'dashboard' ? ' tab-btn--active' : ''}`} onClick={() => setTab('dashboard')}>
                  대시보드
                </button>
                <button className={`tab-btn${tab === 'users'  ? ' tab-btn--active' : ''}`} onClick={() => setTab('users')}>
                  가입자 관리
                </button>
                <button className={`tab-btn${tab === 'groups' ? ' tab-btn--active' : ''}`} onClick={() => setTab('groups')}>
                  PTT 그룹
                </button>
                <button className={`tab-btn${tab === 'service' ? ' tab-btn--active' : ''}`} onClick={() => setTab('service')}>
                  서비스 상태
                </button>
                <button className={`tab-btn${tab === 'history' ? ' tab-btn--active' : ''}`} onClick={() => setTab('history')}>
                  서비스 이력
                </button>
                <button className={`tab-btn${tab === 'stats' ? ' tab-btn--active' : ''}`} onClick={() => setTab('stats')}>
                  통계
                </button>
              </>
            )}
            <button className={`tab-btn${tab === 'docs' ? ' tab-btn--active' : ''}`} onClick={() => setTab('docs')}>
              문서
            </button>
          </nav>
          <div className="app-user">
            <span className="app-user-name">{user.name}</span>
            <span className={`badge ${user.role === 'admin' ? 'badge--blue' : 'badge--gray'}`}>
              {user.role === 'admin' ? '관리자' : '사용자'}
            </span>
            <button className="btn btn--ghost btn--sm" onClick={() => setShowChgPw(true)}>🔑</button>
            <button className="btn btn--ghost btn--sm" onClick={logout}>로그아웃</button>
          </div>
        </header>

        <main className="app-main">
          {!isAdmin ? (
            <div className="empty" style={{ marginTop: 80, fontSize: 16 }}>
              관리자 권한이 필요합니다
            </div>
          ) : (
            <>
              {tab === 'dashboard' && <DashboardPage />}
              {tab === 'users'     && <UsersPage />}
              {tab === 'groups'    && <GroupsPage />}
              {tab === 'service'   && <ServiceStatusPage />}
              {tab === 'history'   && <CallLogsPage />}
              {tab === 'stats'     && <StatsPage />}
              {tab === 'docs'      && <DocsPage />}
            </>
          )}
        </main>
      </div>

      {showChgPw && (
        <div className="modal-overlay" onClick={() => setShowChgPw(false)}>
          <div className="modal-box" onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">🔑 비밀번호 변경</span>
              <button className="modal-close" onClick={() => setShowChgPw(false)}>✕</button>
            </div>
            <form onSubmit={handleChangePassword}>
              <div className="modal-body">
                <div className="form-grid">
                  <label>현재 비밀번호</label>
                  <input className="form-input" type="password" value={oldPw}
                    onChange={e => setOldPw(e.target.value)} />
                  <label>새 비밀번호</label>
                  <input className="form-input" type="password" value={newPw}
                    onChange={e => setNewPw(e.target.value)} />
                  <label>새 비밀번호 확인</label>
                  <input className="form-input" type="password" value={newPw2}
                    onChange={e => setNewPw2(e.target.value)} />
                </div>
                {chgError && <div className="auth-error" style={{ marginTop: 12 }}>{chgError}</div>}
                {chgOk    && <div className="auth-ok"    style={{ marginTop: 12 }}>{chgOk}</div>}
              </div>
              <div className="modal-footer">
                <button className="btn btn--outline" type="button" onClick={() => setShowChgPw(false)}>취소</button>
                <button className="btn btn--primary" type="submit">변경</button>
              </div>
            </form>
          </div>
        </div>
      )}
    </ToastProvider>
  )
}

export default function App() {
  return (
    <AuthProvider>
      <Shell />
    </AuthProvider>
  )
}
