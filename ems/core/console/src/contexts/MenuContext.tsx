// 메뉴 컨텍스트 — 저장된 console_menu 를 로드해 effective nav (섹션+영역) 계산.
// Sidebar/CustomPageHost 가 소비. 편집 저장 후 reload() 로 즉시 반영.
//
// 서비스 게이팅 — `requiresService` 를 단 섹션·라우트는 서버가 알려주는 설치 서비스 집합
// (`GET /console/catalog`.installed_services = 게이트웨이 라우트 ∩ enabled)에 그 패키지가 있을 때만
// 보인다. 카탈로그를 아직 못 받았거나(부트스트랩·오류) 실패하면 게이팅 섹션은 숨긴다 — 미설치를
// 설치된 것처럼 보이는 쪽이 더 나쁘다(과거 폴백 버그: base 대시보드에 svc 위젯 노출).
import { createContext, useCallback, useContext, useEffect, useMemo, useState, type ReactNode } from 'react'
import { VISIBLE_SECTIONS } from '../routes'
import type { RouteSection } from '../nav-types'
import { consoleApi } from '../api/console'
import { consoleLayoutsApi } from '../api/consoleLayouts'
import { applyMenu, normalizeMenuConfig, DEFAULT_AREAS, type MenuConfig, type MenuAreaDef } from '../menu'

interface MenuCtx {
  sections: RouteSection[]
  areas: MenuAreaDef[]
  savedConfig: MenuConfig | null
  // 서버가 알려준 설치 서비스(패키지 id). null = 아직 모름.
  installedServices: Set<string> | null
  reload: () => Promise<void>
}

const Ctx = createContext<MenuCtx>({
  sections: VISIBLE_SECTIONS, areas: DEFAULT_AREAS, savedConfig: null, installedServices: null,
  reload: async () => {},
})

// 섹션 → 그 안의 라우트 순으로 같은 규칙을 적용한다(코어 섹션 안에서 한 화면만 서비스에 기대는 경우).
export function gateSectionsByService(sections: RouteSection[], installed: Set<string> | null): RouteSection[] {
  const ok = (req?: string) => !req || (installed?.has(req) ?? false)
  return sections
    .filter(s => ok(s.requiresService))
    .map(s => s.routes.some(r => r.requiresService)
      ? { ...s, routes: s.routes.filter(r => ok(r.requiresService)) }
      : s)
}

export function MenuProvider({ children }: { children: ReactNode }) {
  const [savedConfig, setSavedConfig] = useState<MenuConfig | null>(null)
  const [installed, setInstalled] = useState<Set<string> | null>(null)
  const reload = useCallback(async () => {
    try {
      const m = await consoleApi.getMenu()
      setSavedConfig(normalizeMenuConfig(m))
    } catch {
      setSavedConfig(null)   // 404/오류 → 코드 기본 섹션
    }
    try {
      const c = await consoleLayoutsApi.getCatalog()
      setInstalled(new Set((c.installed_services ?? []).map(x => String(x).toLowerCase())))
    } catch {
      setInstalled(null)     // 모름 → 게이팅 섹션 숨김
    }
  }, [])
  useEffect(() => { reload() }, [reload])
  const effective = useMemo(
    () => applyMenu(gateSectionsByService(VISIBLE_SECTIONS, installed), savedConfig),
    [savedConfig, installed])
  return (
    <Ctx.Provider value={{ sections: effective.sections, areas: effective.areas, savedConfig,
                           installedServices: installed, reload }}>
      {children}
    </Ctx.Provider>
  )
}

// eslint-disable-next-line react-refresh/only-export-components
export const useMenu = () => useContext(Ctx)
