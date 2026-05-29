// 메뉴 컨텍스트 — 저장된 console_menu 를 로드해 effective nav 섹션 계산 (OAM 플랫폼화 5-3 step2-3).
// Sidebar 가 소비. 편집 저장 후 reload() 로 즉시 반영.
import { createContext, useCallback, useContext, useEffect, useMemo, useState, type ReactNode } from 'react'
import { VISIBLE_SECTIONS } from '../routes'
import type { RouteSection } from '../nav-types'
import { consoleApi } from '../api/console'
import { applyMenu, type MenuItemOverride } from '../menu'

interface MenuCtx {
  sections: RouteSection[]
  savedItems: MenuItemOverride[] | null
  reload: () => Promise<void>
}

const Ctx = createContext<MenuCtx>({ sections: VISIBLE_SECTIONS, savedItems: null, reload: async () => {} })

export function MenuProvider({ children }: { children: ReactNode }) {
  const [savedItems, setSavedItems] = useState<MenuItemOverride[] | null>(null)
  const reload = useCallback(async () => {
    try {
      const m = await consoleApi.getMenu()
      setSavedItems(Array.isArray(m.items) ? (m.items as MenuItemOverride[]) : null)
    } catch {
      setSavedItems(null)   // 404/오류 → 코드 기본 섹션
    }
  }, [])
  useEffect(() => { reload() }, [reload])
  const sections = useMemo(() => applyMenu(VISIBLE_SECTIONS, savedItems), [savedItems])
  return <Ctx.Provider value={{ sections, savedItems, reload }}>{children}</Ctx.Provider>
}

// eslint-disable-next-line react-refresh/only-export-components
export const useMenu = () => useContext(Ctx)
