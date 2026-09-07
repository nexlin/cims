import { useState } from 'react'
import { useNavigate, useLocation } from 'react-router-dom'
import { PanelLeftClose, PanelLeftOpen, SlidersHorizontal, ChevronDown, ChevronRight } from 'lucide-react'
import { useAuth } from '../contexts/AuthContext'
import { useDevMode } from '../hooks/useDevMode'
import { useMenu } from '../contexts/MenuContext'
import { MenuEditorModal } from './MenuEditorModal'
import type { RouteSection, RouteDef } from '../nav-types'
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
  const { sections, areas } = useMenu()
  const [editing, setEditing] = useState(false)
  const [open, setOpen] = useState<Record<string, boolean>>({})
  const isAdmin = hasRole(user, 'admin')   // developer(admin 동급) 포함

  // leaf 가시성: hidden 아니고, 라우트 요구 역할 등급을 만족하는 사용자에게만 (RBAC).
  const devMode = useDevMode()
  const visibleRoutes = (s: RouteSection): RouteDef[] =>
    s.routes.filter(r => !r.hidden && canAccessRoute(user, r) && (!r.devOnly || devMode))
  const isLeafActive = (r: RouteDef) => pathname === r.path || pathname.startsWith(r.path + '/')
  const isGroupActive = (s: RouteSection) => visibleRoutes(s).some(isLeafActive)
  // 기본 펼침 = 현재 활성 그룹. 사용자가 토글하면 override.
  const isExpanded = (s: RouteSection) => open[s.key] ?? isGroupActive(s)

  // 영역별 버킷 (보이는 leaf 가 있는 섹션만). 영역 목록은 메뉴 편집으로 추가/이름변경 가능 —
  // 알 수 없는 area 값(영역 삭제 등)은 'admin' 으로 수용.
  const areaKeys = new Set(areas.map(a => a.key))
  const byArea: Record<string, RouteSection[]> = {}
  for (const a of areas) byArea[a.key] = []
  for (const s of sections) {
    if (visibleRoutes(s).length === 0) continue
    const key = s.area && areaKeys.has(s.area) ? s.area : 'admin'
    ;(byArea[key] ??= []).push(s)
  }

  // 치수는 Figma Sec/Sidebar(457:5484) 실측: 폭 232 · 항목 212×36(간격 2) ·
  // 항목 안 좌 10 / 아이콘 18 / 아이콘→라벨 8 / caret 우 10 · 하위 들여쓰기 26 ·
  // 영역 사이 12. 접힘 상태는 시안이 다루지 않아 현행 유지한다.
  const ITEM = 'flex h-9 w-full items-center gap-2 rounded-md px-2.5 text-left text-md ' +
               'font-medium text-sidebar-foreground transition-colors ' +
               'hover:bg-sidebar-accent hover:text-sidebar-primary'
  const ITEM_C = 'flex h-9 w-full items-center justify-center rounded-md text-sidebar-foreground ' +
                 'transition-colors hover:bg-sidebar-accent hover:text-sidebar-primary'

  return (
    // `sidebar` / `sidebar--collapsed` 이름은 남긴다 — 그리드 배치와 인쇄 시 숨김 규칙이 쓴다.
    <aside className={`sidebar ${collapsed ? 'sidebar--collapsed' : ''} flex flex-col overflow-y-auto overflow-x-hidden border-r border-sidebar-border bg-sidebar`}>
      <nav className={`flex flex-1 flex-col gap-0.5 ${collapsed ? 'px-1.5 py-2' : 'p-2.5'}`}>
        {areas.map(area => {
          const groups = byArea[area.key] ?? []
          if (groups.length === 0) return null
          return (
            <div key={area.key} className="flex flex-col gap-0.5 [&+&]:mt-3">
              {!collapsed && (
                <div className="px-2.5 pb-0.5 pt-2 text-xs text-muted-foreground">{area.label}</div>
              )}
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
                      className={`${ITEM_C} ${groupActive ? 'bg-sidebar-accent text-sidebar-primary' : ''}`}
                      onClick={() => navigate(single ? leaves[0].path : section.defaultPath)}
                      title={section.label}>
                      <Icon size={20} className="shrink-0" />
                    </button>
                  )
                }
                return (
                  <div key={section.key} className="flex flex-col">
                    {/* 그룹 헤드 — 활성일 때 **글자색만** 바뀐다. 배경은 실제로 열려 있는
                        하위 항목(잎)에만 깔린다(시안 Sec/Sidebar). 단일 잎 섹션은 헤드가
                        곧 잎이므로 배경도 준다. */}
                    <button
                      className={`${ITEM} ${groupActive
                        ? (single ? 'bg-sidebar-accent text-sidebar-primary font-semibold'
                                  : 'text-sidebar-primary font-semibold')
                        : ''}`}
                      onClick={() => single
                        ? navigate(leaves[0].path)
                        : setOpen(o => ({ ...o, [section.key]: !expanded }))}>
                      <Icon size={18} className="shrink-0 opacity-90" />
                      <span className="flex-1 truncate">{section.label}</span>
                      {!single && (expanded
                        ? <ChevronDown size={14} className="shrink-0 opacity-55" />
                        : <ChevronRight size={14} className="shrink-0 opacity-55" />)}
                    </button>
                    {!single && expanded && (
                      <div className="mb-0.5 mt-px flex flex-col gap-px">
                        {leaves.map(r => (
                          <button key={r.path}
                            className={`flex h-9 w-full items-center rounded-md pl-[26px] pr-2.5 text-left text-md
                              font-medium transition-colors hover:bg-sidebar-accent hover:text-sidebar-primary
                              ${isLeafActive(r)
                                ? 'bg-sidebar-accent font-semibold text-sidebar-primary'
                                : 'text-sidebar-foreground opacity-85'}`}
                            onClick={() => navigate(r.path)}>
                            <span className="truncate">{r.title}</span>
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
      {/* 하단 고정 — 시안에는 구분선이 없다 */}
      <div className={`flex flex-col gap-0.5 ${collapsed ? 'px-1.5 pb-3.5 pt-2' : 'px-2.5 pb-3.5 pt-2'}`}>
        {isAdmin && (
          <button className={`${collapsed ? ITEM_C : ITEM} text-neutral`}
                  onClick={() => setEditing(true)} title="메뉴 편집">
            <SlidersHorizontal size={20} className="shrink-0" />
            {!collapsed && <span className="flex-1 truncate">메뉴 편집</span>}
          </button>
        )}
        <button className={`${collapsed ? ITEM_C : ITEM} text-neutral`}
                onClick={onToggle} title={collapsed ? '메뉴 펼치기' : '메뉴 접기'}>
          {collapsed
            ? <PanelLeftOpen size={20} className="shrink-0" />
            : <PanelLeftClose size={20} className="shrink-0" />}
          {!collapsed && <span className="flex-1 truncate">메뉴 접기</span>}
        </button>
      </div>
      {editing && <MenuEditorModal onClose={() => setEditing(false)} />}
    </aside>
  )
}
