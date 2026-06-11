// 메뉴(사이드바 nav) 커스터마이즈 로직.
// 저장 menu 는 코드 SECTIONS 위에 순서·라벨·표시여부·영역(area) override 를 적용하고,
// 여기에 더해 ① 커스텀 영역(운용/관리 같은 그룹핑) 추가/이름변경 ② 커스텀 메뉴 그룹 +
// 위젯 합성 페이지(/custom/<slug>) 추가를 지원한다.
// 코어 라우트/아이콘/경로는 코드가 SoT (링크 안 깨짐). 시스템/릴리스 섹션은 편집 잠금.
import { LayoutGrid } from 'lucide-react'
import type { RouteSection } from './nav-types'
import { NAV_AREA_LABELS } from './nav-types'

export interface MenuItemOverride {
  key: string        // RouteSection.key (코어 'system'… 또는 커스텀 'custom:<id>')
  label?: string     // 라벨 override (없으면 코드/정의 기본)
  hidden?: boolean   // nav 에서 숨김 (라우트는 유지 — 직접 URL 접근 가능)
  area?: string      // 영역 이동 ('ops' | 'admin' | 커스텀 영역 key)
}

// 커스텀 메뉴 그룹이 담는 위젯 합성 페이지 — /custom/<slug> 에서 EditableLayout 으로 렌더.
export interface CustomPageDef {
  slug: string       // URL/레이아웃 키 (생성 시 고정 — 변경하면 저장된 위젯 배치를 잃음)
  title: string
}

export interface CustomSectionDef {
  key: string        // 'custom:<id>'
  label: string
  area?: string      // 소속 영역 (기본 'admin')
  pages: CustomPageDef[]
}

export interface MenuAreaDef {
  key: string        // 'ops' | 'admin' | 'area:<id>'
  label: string
}

// 저장 문서 전체 (OAM /console/menu). 구버전 저장본은 items 만 가짐 — normalize 로 흡수.
export interface MenuConfig {
  items: MenuItemOverride[]
  customSections: CustomSectionDef[]
  areas: MenuAreaDef[]   // ops/admin 라벨 override + 커스텀 영역 추가분
}

export interface EffectiveMenu {
  sections: RouteSection[]
  areas: MenuAreaDef[]   // 렌더 순서대로 (ops, admin, 커스텀…)
}

// 편집 잠금 섹션 — 상용 base 콘솔의 생명선(시스템 구성·릴리스). 이름변경/숨김/영역이동 불가.
export const LOCKED_SECTION_KEYS: ReadonlySet<string> = new Set(['system', 'release'])

export const DEFAULT_AREAS: MenuAreaDef[] = [
  { key: 'ops', label: NAV_AREA_LABELS.ops },
  { key: 'admin', label: NAV_AREA_LABELS.admin },
]

// 저장 문서(임의 JSON) → MenuConfig. 구버전({items:[…]}만)·결손 필드 모두 흡수.
export function normalizeMenuConfig(doc: unknown): MenuConfig | null {
  if (!doc || typeof doc !== 'object') return null
  const d = doc as Record<string, unknown>
  const items = Array.isArray(d.items) ? (d.items as MenuItemOverride[]) : []
  const customSections = Array.isArray(d.custom_sections) ? (d.custom_sections as CustomSectionDef[]) : []
  const areas = Array.isArray(d.areas) ? (d.areas as MenuAreaDef[]) : []
  if (items.length === 0 && customSections.length === 0 && areas.length === 0) return null
  return {
    items: items.filter(it => it && typeof it.key === 'string'),
    customSections: customSections.filter(cs => cs && typeof cs.key === 'string' && Array.isArray(cs.pages)),
    areas: areas.filter(a => a && typeof a.key === 'string' && typeof a.label === 'string'),
  }
}

// 커스텀 그룹 → RouteSection (라우트는 /custom/<slug> 합성 페이지, 빈 위젯 seed).
export function customSectionToRouteSection(cs: CustomSectionDef): RouteSection {
  const routes = cs.pages.map(p => ({
    path: `/custom/${p.slug}`,
    title: p.title,
    layoutId: `custom.${p.slug}`,
    layout: { id: `custom.${p.slug}`, title: p.title, widgets: [] },
  }))
  return {
    key: cs.key,
    label: cs.label,
    icon: LayoutGrid,
    area: cs.area || 'admin',
    basePath: routes[0]?.path ?? '/custom',
    defaultPath: routes[0]?.path ?? '/custom',
    routes,
  }
}

// 코드 섹션 + 저장 메뉴 → 실제 nav (섹션 순서/라벨/숨김/영역 + 커스텀 그룹/영역).
// 코드에만 있는 새 섹션은 뒤에 append (신규 기능 자동 노출). 잠금 섹션의 override 는 무시.
export function applyMenu(base: RouteSection[], cfg: MenuConfig | null): EffectiveMenu {
  // 영역: ops/admin 기본 + 저장본의 라벨 override·커스텀 영역 append (저장 순서 유지)
  const areas: MenuAreaDef[] = DEFAULT_AREAS.map(a => ({ ...a }))
  if (cfg) {
    for (const a of cfg.areas) {
      const found = areas.find(x => x.key === a.key)
      if (found) found.label = a.label || found.label
      else areas.push({ key: a.key, label: a.label })
    }
  }

  if (!cfg) return { sections: base, areas }

  const customs = cfg.customSections.map(customSectionToRouteSection)
  const byKey = new Map([...base, ...customs].map(s => [s.key, s] as const))
  const out: RouteSection[] = []
  const used = new Set<string>()
  for (const it of cfg.items) {
    const s = byKey.get(it.key)
    if (!s || used.has(it.key)) continue
    used.add(it.key)
    if (LOCKED_SECTION_KEYS.has(it.key)) { out.push(s); continue }  // 잠금 — override 무시
    if (it.hidden) continue
    out.push({
      ...s,
      ...(it.label ? { label: it.label } : {}),
      ...(it.area ? { area: it.area } : {}),
    })
  }
  for (const s of base) if (!used.has(s.key)) out.push(s)
  for (const s of customs) if (!used.has(s.key)) out.push(s)
  return { sections: out, areas }
}

// 커스텀 페이지 slug 룩업 — /custom/:slug 라우트 호스트가 제목 표시에 사용.
export function findCustomPage(cfg: MenuConfig | null, slug: string): { page: CustomPageDef; section: CustomSectionDef } | null {
  if (!cfg) return null
  for (const cs of cfg.customSections) {
    const p = cs.pages.find(x => x.slug === slug)
    if (p) return { page: p, section: cs }
  }
  return null
}
