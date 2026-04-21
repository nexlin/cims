import { useState } from 'react'
import { AuthProvider, useAuth } from './contexts/AuthContext'
import { ToastProvider } from './components/Toast'
import Sidebar, { type PageId } from './components/Sidebar'
import LoginPage from './pages/LoginPage'
import DashboardPage from './pages/DashboardPage'
import OrganizationsPage from './pages/OrganizationsPage'
import MembersPage from './pages/MembersPage'
import SubscriptionsPage from './pages/SubscriptionsPage'
import PttGroupsPage from './pages/PttGroupsPage'
import ServiceStatusPage from './pages/ServiceStatusPage'
import VolteHistoryPage from './pages/VolteHistoryPage'
import PttHistoryPage from './pages/PttHistoryPage'
// RecordingsPage는 VoLTE/PTT 이력 상세에 통합
import StatsPage from './pages/StatsPage'
import StatsMessagesPage from './pages/StatsMessagesPage'
import VerificationPage from './pages/VerificationPage'
import SipListenersPage from './pages/SipListenersPage'
import SipTrunksPage from './pages/SipTrunksPage'
import SipRoutesPage from './pages/SipRoutesPage'
import SipAccessPage from './pages/SipAccessPage'
import SipServicesPage from './pages/SipServicesPage'
import ServicesPage from './pages/ServicesPage'
import DeploymentsDashboardPage from './pages/DeploymentsDashboardPage'
import DocsPage from './pages/DocsPage'
import { authApi } from './api/auth'
import './index.css'

const PAGE_TITLES: Record<PageId, string> = {
  'dashboard': '대시보드',
  'org': '조직 관리',
  'members': '구성원 관리',
  'subscriptions': 'VoLTE/PTT 번호 관리',
  'ptt-groups': 'PTT 그룹 관리',
  'service-status': '실시간 서비스 상태',
  'history-volte': 'VoLTE 서비스 이력',
  'history-ptt': 'PTT 서비스 이력',
  'stats-volte': 'VoLTE 통계',
  'stats-ptt': 'PTT 통계',
  'stats-sip': 'SIP 메시지 통계',
  'stats-cmp': 'CMP 인터페이스 통계',
  'stats-csc': 'CSC 인터페이스 통계',
  'stats-https': 'HTTPS 메시지 통계',
  'csp-listeners': 'SIP 리스너 (CSP)',
  'csp-trunks': 'SIP 트렁크 (CSP)',
  'csp-routes': 'SIP 라우팅 규칙 (CSP)',
  'csp-services': 'SIP 서비스 (CSP)',
  'csp-access': 'SIP 접근제어 (CSP)',
  'services':   '서비스 프로세스 제어',
  'deploy':       '배포 관리',
  'verification': '시스템 검증',
  'docs': '문서',
}

function Shell() {
  const { user, loading, logout, refresh } = useAuth()
  const [page, setPage] = useState<PageId>('dashboard')
  const [showChgPw, setShowChgPw] = useState(false)
  const [chgError, setChgError] = useState('')
  const [chgOk, setChgOk] = useState('')
  const [oldPw, setOldPw] = useState('')
  const [newPw, setNewPw] = useState('')
  const [newPw2, setNewPw2] = useState('')

  if (loading) return <div className="auth-loading">로딩 중...</div>
  if (!user) return <LoginPage />

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

  function renderPage() {
    if (!isAdmin && page !== 'docs') {
      return <div className="empty" style={{ marginTop: 80 }}>관리자 권한이 필요합니다</div>
    }
    switch (page) {
      case 'dashboard':       return <DashboardPage />
      case 'org':             return <OrganizationsPage />
      case 'members':         return <MembersPage />
      case 'subscriptions':   return <SubscriptionsPage />
      case 'ptt-groups':      return <PttGroupsPage />
      case 'service-status':  return <ServiceStatusPage />
      case 'history-volte':   return <VolteHistoryPage />
      case 'history-ptt':     return <PttHistoryPage />
      case 'stats-volte':     return <StatsPage />
      case 'stats-ptt':       return <StatsPage />
      case 'stats-sip':       return <StatsMessagesPage iface="sip" />
      case 'stats-cmp':       return <StatsMessagesPage iface="cmp" />
      case 'stats-csc':       return <StatsMessagesPage iface="csc" />
      case 'stats-https':     return <StatsMessagesPage iface="https" />
      case 'csp-listeners':   return <SipListenersPage />
      case 'csp-trunks':      return <SipTrunksPage />
      case 'csp-routes':      return <SipRoutesPage />
      case 'csp-services':    return <SipServicesPage />
      case 'csp-access':      return <SipAccessPage />
      case 'services':        return <ServicesPage />
      case 'deploy':          return <DeploymentsDashboardPage />
      case 'verification':    return <VerificationPage />
      case 'docs':            return <DocsPage />
      default:                return <DashboardPage />
    }
  }

  return (
    <ToastProvider>
      <div className="app-layout">
        <Sidebar
          current={page}
          onNavigate={setPage}
          userName={user.name}
          userRole={user.role}
          onLogout={logout}
          onChangePw={() => setShowChgPw(true)}
        />
        <div className="app-content">
          <div className="app-content-header">{PAGE_TITLES[page] || ''}</div>
          <div className="app-content-body">{renderPage()}</div>
        </div>
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
    <AuthProvider>
      <Shell />
    </AuthProvider>
  )
}
