// 콘솔 D1 — 위젯 카탈로그 / 프로파일 / 사용자별 레이아웃 API (OAM /api/v1/console/*).
// oam_base_service_split §6. 서버(base OAM)가 RBAC 필터 + 서비스 가용성을 권위로 내려준다(D7):
//  - catalog.widgets = (RBAC 허용) 만, 각 항목에 available(서비스 설치/가용) 플래그.
//  - layouts/me PUT 은 권한 밖/미존재 위젯이면 서버가 403/400 으로 거부(레이아웃은 보안 아님).
import { api } from './client'
import type { Role } from './auth'

export type WidgetArea = 'ops' | 'admin'

// 카탈로그 1개 — 서버 정책(RBAC 통과분)만 내려오며 available 로 서비스 가용성 표기.
export interface CatalogWidget {
  id: string
  title: string
  area: WidgetArea
  requires_service: string | null   // null=base · 'csc' · 'svc-mgmt'
  default_w: number
  available: boolean                 // false = 서비스 미설치/불가 → "설치 후 사용 가능" 표기
}

export interface CatalogResponse {
  role: Role
  installed_services: string[]
  widgets: CatalogWidget[]
}

export interface ProfileTemplate {
  id: string
  label: string
  dashboard: string[]                // 위젯 id 목록
}

export interface ProfilesResponse {
  role: Role
  default: string                    // 계정/역할 기본 프로파일 id
  profiles: ProfileTemplate[]
}

// 사용자 레이아웃 — 페이지별 위젯 배치 + 대시보드 위젯 목록(개인화 override).
export interface UserLayout {
  pages?: { slug: string; widgets: string[] }[]
  widgets?: Record<string, string[]>
}

export interface MyLayoutResponse {
  login_id: string
  role: Role
  base_profile: string
  source: 'override' | 'profile'     // override=개인 저장본, profile=템플릿 기본
  layout: UserLayout
  updated_at?: string
}

export const consoleLayoutsApi = {
  // 카탈로그(RBAC∩설치서비스 서버필터) — 위젯 추가 UI 가 사용.
  getCatalog: () => api.get<CatalogResponse>('/console/catalog'),
  // 프로파일 템플릿 목록(역할 허용분) + 기본.
  getProfiles: () => api.get<ProfilesResponse>('/console/profiles'),
  // 본인 레이아웃(없으면 프로파일 기본 = source:'profile').
  getMyLayout: () => api.get<MyLayoutResponse>('/console/layouts/me'),
  // 개인화 저장(서버가 RBAC 강제 → 권한 밖 위젯이면 reject).
  saveMyLayout: (body: { base_profile?: string; layout: UserLayout }) =>
    api.put<{ saved: boolean; login_id: string }>('/console/layouts/me', body),
  // 프로파일 기본으로 리셋.
  resetMyLayout: () => api.delete<{ reset: boolean; login_id: string }>('/console/layouts/me'),
}

// 위젯이 요구하는 서비스가 미설치/불가일 때 빈 화면 대신 안내(장애격리 UX, D1).
// 컴포넌트 쪽에서 catalog.available 로 분기할 때 쓰는 헬퍼.
export function widgetUnavailableNote(w: CatalogWidget): string | null {
  if (w.available) return null
  return w.requires_service
    ? `'${w.requires_service}' 서비스 미설치/일시 불가 — 설치 후 사용 가능`
    : '일시적으로 사용할 수 없습니다'
}
