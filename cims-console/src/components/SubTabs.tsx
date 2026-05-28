import { NavLink, useLocation } from 'react-router-dom'
import { findSectionByPath } from '../routes'

export default function SubTabs() {
  const { pathname } = useLocation()
  const section = findSectionByPath(pathname)
  if (!section) return null
  const visibleRoutes = section.routes.filter(r => !r.hidden)
  if (visibleRoutes.length <= 1) return null
  return (
    <nav className="subtabs">
      {visibleRoutes.map(r => (
        <NavLink
          key={r.path}
          to={r.path}
          className={({ isActive }) => `subtabs-item ${isActive ? 'subtabs-item--active' : ''}`}
        >
          {r.title}
        </NavLink>
      ))}
    </nav>
  )
}
