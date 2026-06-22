// 콘솔 layout/menu 영속 API (OAM /api/v1/console). 저장본 없으면 404 → 프론트 seed fallback.
import { api } from './client'
import type { PageLayout } from '../widgets/types'

export interface LayoutSummary {
  id: string
  title?: string
  widget_count: number
  update_time?: string
}

// 저장 메뉴 문서 — items(섹션 override) + custom_sections(커스텀 그룹/페이지) + areas(영역).
// 구버전 저장본은 items 만 가질 수 있음 (menu.ts normalizeMenuConfig 가 흡수).
export interface ConsoleMenu {
  items: unknown[]
  custom_sections?: unknown[]
  areas?: unknown[]
  found?: boolean
}

export const consoleApi = {
  listLayouts: () => api.get<{ layouts: LayoutSummary[] }>('/console/layouts'),
  getLayout:   (id: string) => api.get<PageLayout>(`/console/layouts/${encodeURIComponent(id)}`),
  putLayout:   (id: string, layout: PageLayout) =>
    api.put<PageLayout>(`/console/layouts/${encodeURIComponent(id)}`, layout),
  deleteLayout: (id: string) => api.delete<{ deleted: boolean }>(`/console/layouts/${encodeURIComponent(id)}`),
  getMenu:     () => api.get<ConsoleMenu>('/console/menu'),
  putMenu:     (menu: { items: unknown[]; custom_sections?: unknown[]; areas?: unknown[] }) =>
    api.put<ConsoleMenu>('/console/menu', menu),
}
