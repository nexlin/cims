import { useNavigate, useLocation } from 'react-router-dom'
import { PanelLeftClose, PanelLeftOpen } from 'lucide-react'
import { VISIBLE_SECTIONS, findSectionByPath } from '../routes'

interface SidebarProps {
  collapsed: boolean
  onToggle: () => void
}

export default function Sidebar({ collapsed, onToggle }: SidebarProps) {
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
      <div className="sidebar-footer">
        <button
          className="sidebar-item sidebar-toggle"
          onClick={onToggle}
          title={collapsed ? '메뉴 펼치기' : '메뉴 접기'}
        >
          {collapsed
            ? <PanelLeftOpen size={20} className="sidebar-item-icon" />
            : <PanelLeftClose size={20} className="sidebar-item-icon" />}
          {!collapsed && <span className="sidebar-item-label">메뉴 접기</span>}
        </button>
      </div>
    </aside>
  )
}
