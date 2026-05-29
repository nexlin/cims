// 콘솔 라우트 = [범용 OAM 코어 섹션] + [서비스 pack 기여 섹션] 합성.
//   - CORE_SECTIONS: 대시보드 / 패키징 / 배포 / 문서 — 서비스 무지(범용 OAM).
//   - SERVICE_MANIFESTS: 가입자/서비스/통계 등 — services/registry.ts 의 서비스 pack 이 기여.
// nav 타입은 ./nav-types, CIMS 기여는 ./services/cims/manifest.
import {
  LayoutDashboard,
  FlaskConical,
  Rocket,
  FileText,
} from 'lucide-react'

import type { RouteDef, RouteSection } from './nav-types'
import { SERVICE_MANIFESTS } from './services/registry'

import DashboardPage from './pages/DashboardPage'
import AlertsPage from './pages/AlertsPage'
import ServicesPage from './pages/ServicesPage'
import PackagesPage from './pages/PackagesPage'
import ServersPage from './pages/ServersPage'
import HaServicesPage from './pages/HaServicesPage'
import VerificationV2Page from './pages/VerificationV2Page'
import VerificationHistoryPage from './pages/VerificationHistoryPage'
import DocsPage from './pages/DocsPage'

export type { RouteDef, RouteSection } from './nav-types'

// 콘솔 타겟 — 'dev' (TB-Console / 검증용) | 'prod' (배포본 — 운영자용, 제한된 메뉴)
// vite build 시 VITE_CONSOLE_TARGET=prod 환경변수 주입으로 결정.
const CONSOLE_TARGET = ((import.meta as unknown as { env: Record<string, string> }).env?.VITE_CONSOLE_TARGET) || 'dev'
export const IS_PROD_CONSOLE = CONSOLE_TARGET === 'prod'

// ── 범용 OAM 코어 섹션 (서비스 무지) ──────────────────────────────
const CORE_SECTIONS: RouteSection[] = [
  {
    key: 'dashboard',
    label: '대시보드',
    icon: LayoutDashboard,
    basePath: '/dashboard',
    defaultPath: '/dashboard',
    order: 10,
    routes: [
      { path: '/dashboard',        title: '실시간',     component: DashboardPage },
      { path: '/dashboard/alerts', title: '알람 이력', component: AlertsPage },
    ],
  },
  {
    key: 'release',
    label: '패키징',
    icon: FlaskConical,
    basePath: '/release',
    defaultPath: '/release/verify',
    order: 60,
    prodHidden: true,                    // 배포 콘솔에서 숨김 (운영자는 패키징/검증 불필요)
    routes: [
      { path: '/release/verify',          title: '검증 실행',     component: VerificationV2Page, adminOnly: true },
      { path: '/release/verify-history',  title: '검증 이력',     component: VerificationHistoryPage, adminOnly: true },
      { path: '/release/package',         title: '패키징',       component: ServicesPage, adminOnly: true },
    ],
  },
  {
    key: 'deploy',
    label: '배포',
    icon: Rocket,
    basePath: '/deploy',
    defaultPath: '/deploy/servers',
    order: 70,
    routes: [
      { path: '/deploy/servers',   title: '시스템/인프라',      component: ServersPage,    adminOnly: true },
      { path: '/deploy/packages',  title: '패키지',            component: PackagesPage,   adminOnly: true },
      { path: '/deploy/services',  title: 'HA 상세 편집',      component: HaServicesPage, adminOnly: true, hidden: true },
    ],
  },
  {
    key: 'docs',
    label: '문서',
    icon: FileText,
    basePath: '/docs',
    defaultPath: '/docs',
    order: 90,
    routes: [
      { path: '/docs', title: '문서', component: DocsPage },
    ],
  },
]

// 코어 + 서비스 pack 섹션 병합 → order 기준 정렬. 서비스 섹션엔 serviceId 태깅.
const SERVICE_SECTIONS: RouteSection[] = SERVICE_MANIFESTS.flatMap(
  m => m.sections.map(s => ({ ...s, serviceId: s.serviceId ?? m.id }))
)

export const SECTIONS: RouteSection[] = [...CORE_SECTIONS, ...SERVICE_SECTIONS]
  .sort((a, b) => (a.order ?? 50) - (b.order ?? 50))

// IS_PROD_CONSOLE 일 때 prodHidden=true 섹션 제거. dev 빌드는 모두 노출.
export const VISIBLE_SECTIONS: RouteSection[] = SECTIONS.filter(s => !IS_PROD_CONSOLE || !s.prodHidden)

// 평탄화된 라우트 리스트 — <Routes> 렌더용 (prod 에서는 숨김 섹션 라우팅도 제거 → /dashboard 로 redirect)
export const FLAT_ROUTES: RouteDef[] = VISIBLE_SECTIONS.flatMap(s => s.routes)

// 경로 → section/route 조회
export function findSectionByPath(pathname: string): RouteSection | undefined {
  return SECTIONS.find(s => pathname === s.basePath || pathname.startsWith(s.basePath + '/'))
}

export function findRouteByPath(pathname: string): RouteDef | undefined {
  return FLAT_ROUTES.find(r => r.path === pathname)
}
