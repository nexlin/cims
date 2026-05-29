// 메뉴(상단 nav) 커스터마이즈 로직 — 섹션 단위.
// 저장 menu 는 코드 SECTIONS 위에 순서·라벨·표시여부만 override (라우트/아이콘/경로는 코드 SoT → 링크 안 깨짐).
import type { RouteSection } from './nav-types'

export interface MenuItemOverride {
  key: string        // RouteSection.key
  label?: string     // 라벨 override (없으면 코드 기본)
  hidden?: boolean   // nav 에서 숨김 (라우트는 유지 — 직접 URL 접근 가능)
}

// 코드 섹션에 저장 menu override 적용 → 실제 nav 섹션 목록.
// menu items 순서대로 정렬, 코드에만 있는 새 섹션은 뒤에 append (신규 기능 자동 노출).
export function applyMenu(base: RouteSection[], items: MenuItemOverride[] | null): RouteSection[] {
  if (!items || items.length === 0) return base
  const byKey = new Map(base.map(s => [s.key, s]))
  const out: RouteSection[] = []
  const used = new Set<string>()
  for (const it of items) {
    const s = byKey.get(it.key)
    if (!s || used.has(it.key)) continue
    used.add(it.key)
    if (it.hidden) continue
    out.push(it.label ? { ...s, label: it.label } : s)
  }
  for (const s of base) if (!used.has(s.key)) out.push(s)
  return out
}
