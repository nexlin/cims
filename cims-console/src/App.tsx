import { useState } from 'react'
import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom'
import { AuthProvider, useAuth } from './contexts/AuthContext'
import { MenuProvider } from './contexts/MenuContext'
import { ToastProvider } from './components/Toast'
import Sidebar from './components/Sidebar'
import Header from './components/Header'
import LoginPage from './pages/LoginPage'
import { FLAT_ROUTES } from './routes'
import { authApi } from './api/auth'
import './index.css'

const SIDEBAR_COLLAPSED_KEY = 'cims_sidebar_collapsed'

function RouteGuard({ children, adminOnly }: { children: React.ReactNode; adminOnly?: boolean }) {
  const { user } = useAuth()
  if (adminOnly && user?.role !== 'admin') {
    return <div className="empty" style={{ marginTop: 80 }}>관리자 권한이 필요합니다</div>
  }
  return <>{children}</>
}

function Shell() {
  const { user, loading, logout, refresh } = useAuth()
  const [collapsed, setCollapsed] = useState<boolean>(() => {
    return localStorage.getItem(SIDEBAR_COLLAPSED_KEY) === '1'
  })
  const [showChgPw, setShowChgPw] = useState(false)
  const [chgError, setChgError] = useState('')
  const [chgOk, setChgOk] = useState('')
  const [oldPw, setOldPw] = useState('')
  const [newPw, setNewPw] = useState('')
  const [newPw2, setNewPw2] = useState('')

  if (loading) return <div className="auth-loading">로딩 중...</div>
  if (!user) return <LoginPage />

  function toggleSidebar() {
    setCollapsed(prev => {
      const next = !prev
      localStorage.setItem(SIDEBAR_COLLAPSED_KEY, next ? '1' : '0')
      return next
    })
  }

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
     <MenuProvider>
      <div className={`app-layout ${collapsed ? 'app-layout--collapsed' : ''}`}>
        <Header
          userName={user.name}
          userRole={user.role}
          onLogout={logout}
          onChangePw={() => setShowChgPw(true)}
        />
        <Sidebar collapsed={collapsed} onToggle={toggleSidebar} />
        <main className="app-content">
          <div className="app-content-body">
            <Routes>
              <Route path="/" element={<Navigate to="/dashboard" replace />} />
              {/* 옛 경로 호환 — 알람 이력은 /alerts/history 로 이전 */}
              <Route path="/dashboard/alerts" element={<Navigate to="/alerts/history" replace />} />
              {FLAT_ROUTES.map(r => {
                const Comp = r.component
                return (
                  <Route
                    key={r.path}
                    path={r.path}
                    element={<RouteGuard adminOnly={r.adminOnly}><Comp /></RouteGuard>}
                  />
                )
              })}
              <Route path="*" element={<Navigate to="/dashboard" replace />} />
            </Routes>
          </div>
        </main>
      </div>
     </MenuProvider>

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
                  <input className="form-input" type="password" value={oldPw} onChange={e => setOldPw(e.target.value)} />
                  <label>새 비밀번호</label>
                  <input className="form-input" type="password" value={newPw} onChange={e => setNewPw(e.target.value)} />
                  <label>새 비밀번호 확인</label>
                  <input className="form-input" type="password" value={newPw2} onChange={e => setNewPw2(e.target.value)} />
                </div>
                {chgError && <div className="auth-error" style={{ marginTop: 12 }}>{chgError}</div>}
                {chgOk && <div className="auth-ok" style={{ marginTop: 12 }}>{chgOk}</div>}
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
    <BrowserRouter>
      <AuthProvider>
        <Shell />
      </AuthProvider>
    </BrowserRouter>
  )
}
