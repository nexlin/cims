import { useState } from 'react'
import { useNavigate, useLocation } from 'react-router-dom'
import { PanelLeftClose, PanelLeftOpen, SlidersHorizontal, ChevronDown, ChevronRight } from 'lucide-react'
import { useAuth } from '../contexts/AuthContext'
import { useMenu } from '../contexts/MenuContext'
import { MenuEditorModal } from './MenuEditorModal'
import { NAV_AREA_ORDER, NAV_AREA_LABELS, type NavArea, type RouteSection, type RouteDef } from '../nav-types'
import { hasRole, canAccessRoute } from '../utils/permissions'

interface SidebarProps {
  collapsed: boolean
  onToggle: () => void
}

// OAM 표준(FCAPS) + EMS(NetAct/U2000) 관례의 2-레벨 펼침형 사이드바:
//   [영역 헤더: 운용/관리] → [그룹(아이콘, 펼침)] → [하위 항목 링크]
export default function Sidebar({ collapsed, onToggle }: SidebarProps) {
  const navigate = useNavigate()
  const { pathname } = useLocation()
  const { user } = useAuth()
  const { sections } = useMenu()
  const [editing, setEditing] = useState(false)
  const [open, setOpen] = useState<Record<string, boolean>>({})
  const isAdmin = hasRole(user, 'admin')   // developer(admin 동급) 포함

  // leaf 가시성: hidden 아니고, 라우트 요구 역할 등급을 만족하는 사용자에게만 (RBAC).
  const visibleRoutes = (s: RouteSection): RouteDef[] =>
    s.routes.filter(r => !r.hidden && canAccessRoute(user, r))
  const isLeafActive = (r: RouteDef) => pathname === r.path || pathname.startsWith(r.path + '/')
  const isGroupActive = (s: RouteSection) => visibleRoutes(s).some(isLeafActive)
  // 기본 펼침 = 현재 활성 그룹. 사용자가 토글하면 override.
  const isExpanded = (s: RouteSection) => open[s.key] ?? isGroupActive(s)

  // 영역별 버킷 (보이는 leaf 가 있는 섹션만)
  const byArea: Record<NavArea, RouteSection[]> = { ops: [], admin: [] }
  for (const s of sections) {
    if (visibleRoutes(s).length === 0) continue
    byArea[(s.area ?? 'admin')].push(s)
  }

  return (
    <aside className={`sidebar ${collapsed ? 'sidebar--collapsed' : ''}`}>
      <nav className="sidebar-nav">
        {NAV_AREA_ORDER.map(area => {
          const groups = byArea[area]
          if (groups.length === 0) return null
          return (
            <div key={area} className="sidebar-area">
              {!collapsed && <div className="sidebar-area-label">{NAV_AREA_LABELS[area]}</div>}
              {groups.map(section => {
                const Icon = section.icon
                const leaves = visibleRoutes(section)
                const single = leaves.length === 1
                const groupActive = single ? isLeafActive(leaves[0]) : isGroupActive(section)
                const expanded = isExpanded(section)

                // 접힘 상태: 아이콘만 — 클릭 시 defaultPath 이동
                if (collapsed) {
                  return (
                    <button key={section.key}
                      className={`sidebar-item ${groupActive ? 'sidebar-item--active' : ''}`}
                      onClick={() => navigate(single ? leaves[0].path : section.defaultPath)}
                      title={section.label}>
                      <Icon size={20} className="sidebar-item-icon" />
                    </button>
                  )
                }
                return (
                  <div key={section.key} className="sidebar-group">
                    <button
                      className={`sidebar-item sidebar-group-head ${groupActive ? 'sidebar-item--active' : ''}`}
                      onClick={() => single
                        ? navigate(leaves[0].path)
                        : setOpen(o => ({ ...o, [section.key]: !expanded }))}>
                      <Icon size={18} className="sidebar-item-icon" />
                      <span className="sidebar-item-label">{section.label}</span>
                      {!single && (expanded
                        ? <ChevronDown size={14} className="sidebar-chevron" />
                        : <ChevronRight size={14} className="sidebar-chevron" />)}
                    </button>
                    {!single && expanded && (
                      <div className="sidebar-sub">
                        {leaves.map(r => (
                          <button key={r.path}
                            className={`sidebar-subitem ${isLeafActive(r) ? 'sidebar-subitem--active' : ''}`}
                            onClick={() => navigate(r.path)}>
                            {r.title}
                          </button>
                        ))}
                      </div>
                    )}
                  </div>
                )
              })}
            </div>
          )
        })}
      </nav>
      <div className="sidebar-footer">
        {isAdmin && (
          <button className="sidebar-item sidebar-toggle" onClick={() => setEditing(true)} title="메뉴 편집">
            <SlidersHorizontal size={20} className="sidebar-item-icon" />
            {!collapsed && <span className="sidebar-item-label">메뉴 편집</span>}
          </button>
        )}
        <button className="sidebar-item sidebar-toggle" onClick={onToggle} title={collapsed ? '메뉴 펼치기' : '메뉴 접기'}>
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
