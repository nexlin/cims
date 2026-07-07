// 메뉴 컨텍스트 — 저장된 console_menu 를 로드해 effective nav (섹션+영역) 계산.
// Sidebar/CustomPageHost 가 소비. 편집 저장 후 reload() 로 즉시 반영.
import { createContext, useCallback, useContext, useEffect, useMemo, useState, type ReactNode } from 'react'
import { VISIBLE_SECTIONS } from '../routes'
import type { RouteSection } from '../nav-types'
import { consoleApi } from '../api/console'
import { applyMenu, normalizeMenuConfig, DEFAULT_AREAS, type MenuConfig, type MenuAreaDef } from '../menu'

interface MenuCtx {
  sections: RouteSection[]
  areas: MenuAreaDef[]
  savedConfig: MenuConfig | null
  reload: () => Promise<void>
}

const Ctx = createContext<MenuCtx>({
  sections: VISIBLE_SECTIONS, areas: DEFAULT_AREAS, savedConfig: null, reload: async () => {},
})

export function MenuProvider({ children }: { children: ReactNode }) {
  const [savedConfig, setSavedConfig] = useState<MenuConfig | null>(null)
  const reload = useCallback(async () => {
    try {
      const m = await consoleApi.getMenu()
      setSavedConfig(normalizeMenuConfig(m))
    } catch {
      setSavedConfig(null)   // 404/오류 → 코드 기본 섹션
    }
  }, [])
  useEffect(() => { reload() }, [reload])
  const effective = useMemo(() => applyMenu(VISIBLE_SECTIONS, savedConfig), [savedConfig])
  return (
    <Ctx.Provider value={{ sections: effective.sections, areas: effective.areas, savedConfig, reload }}>
      {children}
    </Ctx.Provider>
  )
}

// eslint-disable-next-line react-refresh/only-export-components
export const useMenu = () => useContext(Ctx)
