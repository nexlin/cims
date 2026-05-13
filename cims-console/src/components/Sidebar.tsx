import { useNavigate, useLocation } from 'react-router-dom'
import { VISIBLE_SECTIONS, findSectionByPath } from '../routes'

interface SidebarProps {
  collapsed: boolean
}

export default function Sidebar({ collapsed }: SidebarProps) {
  const navigate = useNavigate()
  const { pathname } = useLocation()
  const currentSection = findSectionByPath(pathname)

  return (
    <aside className={`sidebar ${collapsed ? 'sidebar--collapsed' : ''}`}>
      <nav className="sidebar-nav">
        {VISIBLE_SECTIONS.map(section => {
          const Icon = section.icon
          const active = currentSection?.key === section.key
          return (
            <button
              key={section.key}
              className={`sidebar-item ${active ? 'sidebar-item--active' : ''}`}
              onClick={() => navigate(section.defaultPath)}
              title={collapsed ? section.label : undefined}
            >
              <Icon size={20} className="sidebar-item-icon" />
              {!collapsed && <span className="sidebar-item-label">{section.label}</span>}
            </button>
          )
        })}
      </nav>
    </aside>
  )
}
