import { useState } from 'react'
import { useNavigate, useLocation } from 'react-router-dom'
import { PanelLeftClose, PanelLeftOpen, SlidersHorizontal } from 'lucide-react'
import { findSectionByPath } from '../routes'
import { useAuth } from '../contexts/AuthContext'
import { useMenu } from '../contexts/MenuContext'
import { MenuEditorModal } from './MenuEditorModal'

interface SidebarProps {
  collapsed: boolean
  onToggle: () => void
}

export default function Sidebar({ collapsed, onToggle }: SidebarProps) {
  const navigate = useNavigate()
  const { pathname } = useLocation()
  const { user } = useAuth()
  const { sections } = useMenu()
  const [editing, setEditing] = useState(false)
  const currentSection = findSectionByPath(pathname)
  const isAdmin = user?.role === 'admin'

  return (
    <aside className={`sidebar ${collapsed ? 'sidebar--collapsed' : ''}`}>
      <nav className="sidebar-nav">
        {sections.map(section => {
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
        {isAdmin && (
          <button
            className="sidebar-item sidebar-toggle"
            onClick={() => setEditing(true)}
            title="메뉴 편집"
          >
            <SlidersHorizontal size={20} className="sidebar-item-icon" />
            {!collapsed && <span className="sidebar-item-label">메뉴 편집</span>}
          </button>
        )}
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
      {editing && <MenuEditorModal onClose={() => setEditing(false)} />}
    </aside>
  )
}
