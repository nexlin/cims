import { useState } from 'react'

export type PageId =
  | 'dashboard'
  | 'org' | 'members' | 'subscriptions' | 'ptt-groups'
  | 'service-status'
  | 'history-volte' | 'history-ptt'
  | 'stats-volte' | 'stats-ptt' | 'stats-sip' | 'stats-cmp' | 'stats-csc' | 'stats-https'
  | 'csp-listeners' | 'csp-trunks' | 'csp-routes' | 'csp-access'
  | 'verification'
  | 'docs'

interface MenuItem {
  id?: PageId
  label: string
  children?: { id: PageId; label: string }[]
}

const MENU: MenuItem[] = [
  { id: 'dashboard', label: '대시보드' },
  {
    label: '가입자관리',
    children: [
      { id: 'org', label: '조직' },
      { id: 'members', label: '구성원' },
      { id: 'subscriptions', label: 'VoLTE/PTT 번호' },
      { id: 'ptt-groups', label: 'PTT 그룹' },
    ],
  },
  { id: 'service-status', label: '실시간 상태' },
  {
    label: '서비스 이력',
    children: [
      { id: 'history-volte', label: 'VoLTE' },
      { id: 'history-ptt', label: 'PTT' },
    ],
  },
  {
    label: '통계',
    children: [
      { id: 'stats-volte', label: 'VoLTE' },
      { id: 'stats-ptt', label: 'PTT' },
      { id: 'stats-sip', label: 'SIP' },
      { id: 'stats-cmp', label: 'CMP' },
      { id: 'stats-csc', label: 'CSC' },
      { id: 'stats-https', label: 'HTTPS' },
    ],
  },
  {
    label: 'CSP 설정',
    children: [
      { id: 'csp-listeners', label: 'SIP 리스너' },
      { id: 'csp-trunks',    label: 'SIP 트렁크' },
      { id: 'csp-routes',    label: '라우팅 규칙' },
      { id: 'csp-access',    label: '접근제어' },
    ],
  },
  { id: 'verification', label: '검증' },
  { id: 'docs', label: '문서' },
]

interface SidebarProps {
  current: PageId
  onNavigate: (id: PageId) => void
  userName: string
  userRole: string
  onLogout: () => void
  onChangePw: () => void
}

export default function Sidebar({ current, onNavigate, userName, userRole, onLogout, onChangePw }: SidebarProps) {
  const [expanded, setExpanded] = useState<Set<string>>(new Set(['가입자관리', '서비스 이력', '통계']))

  function toggleGroup(label: string) {
    setExpanded(prev => {
      const next = new Set(prev)
      if (next.has(label)) next.delete(label); else next.add(label)
      return next
    })
  }

  function isActive(id?: PageId) {
    return id === current
  }

  function isGroupActive(children?: { id: PageId }[]) {
    return children?.some(c => c.id === current)
  }

  return (
    <aside className="sidebar">
      <div className="sidebar-logo">
        <span className="sidebar-logo-icon">📡</span>
        <span className="sidebar-logo-text">CIMS</span>
      </div>

      <nav className="sidebar-nav">
        {MENU.map((item, i) => {
          if (item.children) {
            const open = expanded.has(item.label)
            const groupActive = isGroupActive(item.children)
            return (
              <div key={i} className="sidebar-group">
                <button
                  className={`sidebar-group-btn ${groupActive ? 'sidebar-group-btn--active' : ''}`}
                  onClick={() => toggleGroup(item.label)}
                >
                  <span>{item.label}</span>
                  <span className="sidebar-arrow">{open ? '▾' : '▸'}</span>
                </button>
                {open && (
                  <div className="sidebar-children">
                    {item.children.map(child => (
                      <button
                        key={child.id}
                        className={`sidebar-item ${isActive(child.id) ? 'sidebar-item--active' : ''}`}
                        onClick={() => onNavigate(child.id)}
                      >
                        {child.label}
                      </button>
                    ))}
                  </div>
                )}
              </div>
            )
          }
          return (
            <button
              key={item.id}
              className={`sidebar-item ${isActive(item.id) ? 'sidebar-item--active' : ''}`}
              onClick={() => item.id && onNavigate(item.id)}
            >
              {item.label}
            </button>
          )
        })}
      </nav>

      <div className="sidebar-footer">
        <div className="sidebar-user">
          <span className="sidebar-user-name">{userName}</span>
          <span className={`badge ${userRole === 'admin' ? 'badge--blue' : 'badge--gray'}`}>
            {userRole === 'admin' ? '관리자' : '사용자'}
          </span>
        </div>
        <div className="sidebar-actions">
          <button className="btn btn--ghost btn--sm" onClick={onChangePw}>🔑</button>
          <button className="btn btn--ghost btn--sm" onClick={onLogout}>로그아웃</button>
        </div>
      </div>
    </aside>
  )
}
