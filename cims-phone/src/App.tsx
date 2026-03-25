import { AuthProvider, useAuth } from './contexts/AuthContext'
import { ToastProvider } from './components/Toast'
import PhonePage from './pages/PhonePage'
import LoginPage from './pages/LoginPage'
import './index.css'

function Shell() {
  const { user, loading, logout } = useAuth()

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
          <div className="app-user">
            <span className="app-user-name">
              {user.display_name} <small style={{ color: 'var(--text-muted)', fontWeight: 400 }}>{user.mcptt_id}</small>
            </span>
            <button className="btn btn--ghost btn--sm" onClick={logout}>로그아웃</button>
          </div>
        </header>

        <main className="app-main">
          <PhonePage />
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
