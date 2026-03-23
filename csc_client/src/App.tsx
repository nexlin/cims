import { useState } from 'react'
import { ToastProvider } from './components/Toast'
import UsersPage from './pages/UsersPage'
import GroupsPage from './pages/GroupsPage'
import './index.css'

type Tab = 'users' | 'groups'

export default function App() {
  const [tab, setTab] = useState<Tab>('users')

  return (
    <ToastProvider>
      <div className="app">
        <header className="app-header">
          <div className="app-logo">
            <span className="app-logo-icon">📡</span>
            <span className="app-logo-text">CIMS Admin</span>
          </div>
          <nav className="tab-nav">
            <button
              className={`tab-btn${tab === 'users' ? ' tab-btn--active' : ''}`}
              onClick={() => setTab('users')}
            >
              👤 가입자 관리
            </button>
            <button
              className={`tab-btn${tab === 'groups' ? ' tab-btn--active' : ''}`}
              onClick={() => setTab('groups')}
            >
              📢 PTT 그룹 관리
            </button>
          </nav>
        </header>

        <main className="app-main">
          {tab === 'users'  && <UsersPage />}
          {tab === 'groups' && <GroupsPage />}
        </main>
      </div>
    </ToastProvider>
  )
}
