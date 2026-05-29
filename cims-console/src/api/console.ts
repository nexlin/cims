// 콘솔 layout/menu 영속 API (OAM /api/v1/console). 저장본 없으면 404 → 프론트 seed fallback.
import { api } from './client'
import type { PageLayout } from '../widgets/types'

export interface LayoutSummary {
  id: string
  title?: string
  widget_count: number
  update_time?: string
}

export interface ConsoleMenu {
  items: unknown[]
}

export const consoleApi = {
  listLayouts: () => api.get<{ layouts: LayoutSummary[] }>('/console/layouts'),
  getLayout:   (id: string) => api.get<PageLayout>(`/console/layouts/${encodeURIComponent(id)}`),
  putLayout:   (id: string, layout: PageLayout) =>
    api.put<PageLayout>(`/console/layouts/${encodeURIComponent(id)}`, layout),
  deleteLayout: (id: string) => api.delete<{ deleted: boolean }>(`/console/layouts/${encodeURIComponent(id)}`),
  getMenu:     () => api.get<ConsoleMenu>('/console/menu'),
  putMenu:     (items: unknown[]) => api.put<ConsoleMenu>('/console/menu', { items }),
}
