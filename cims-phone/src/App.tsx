import { useState } from 'react'
import { AuthProvider, useAuth } from './contexts/AuthContext'
import { ToastProvider } from './components/Toast'
import PhonePage from './pages/PhonePage'
import LoginPage from './pages/LoginPage'
import DocsPage from './pages/DocsPage'
import './index.css'

type Tab = 'phone' | 'docs'

function Shell() {
  const { user, loading, logout } = useAuth()
  const [tab, setTab] = useState<Tab>('phone')

  if (loading) return <div className="auth-loading">로딩 중...</div>
  if (!user)   return <LoginPage />

  return (
    <ToastProvider>
      <div className="app">
        <header className="app-header">
          <div className="app-logo">
            <span className="app-logo-icon">📱</span>
            <span className="app-logo-text">CIMS Phone</span>
          </div>
          <nav className="tab-nav">
            <button className={`tab-btn${tab === 'phone' ? ' tab-btn--active' : ''}`} onClick={() => setTab('phone')}>
              전화/PTT
            </button>
            <button className={`tab-btn${tab === 'docs' ? ' tab-btn--active' : ''}`} onClick={() => setTab('docs')}>
              문서
            </button>
          </nav>
          <div className="app-user">
            <span className="app-user-name">
              {user.name} <small style={{ color: 'var(--text-muted)', fontWeight: 400 }}>{user.login_id}</small>
            </span>
            <button className="btn btn--ghost btn--sm" onClick={logout}>로그아웃</button>
          </div>
        </header>

        <main className="app-main">
          {tab === 'phone' && <PhonePage />}
          {tab === 'docs' && <DocsPage />}
        </main>
      </div>
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
