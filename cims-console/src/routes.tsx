// 콘솔 라우트 = [범용 OAM 코어 섹션] + [서비스 pack 기여 섹션] 합성.
//   - CORE_SECTIONS: 대시보드 / 장애 / 시스템 / 릴리스 / 문서 — 서비스 무지(범용 OAM).
//   - SERVICE_MANIFESTS: 구성(가입자)/성능/기록 등 — services/registry.ts 의 서비스 pack 이 기여.
// 메뉴는 OAM 표준(FCAPS) + EMS(NetAct/ENM/U2000) 관례에 맞춰 운용(ops)/관리(admin) 2대영역 그룹.
// nav 타입은 ./nav-types, CIMS 기여는 ./services/cims/manifest.
import {
  LayoutDashboard,
  Bell,
  Server,
  Rocket,
  BookOpen,
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
const CONSOLE_TARGET = ((import.meta as unknown as { env: Record<string, string> }).env?.VITE_CONSOLE_TARGET) || 'dev'
export const IS_PROD_CONSOLE = CONSOLE_TARGET === 'prod'

// 활성 알람 뷰 (해소된 알람 숨김). 이력 뷰는 AlertsPage 기본.
const ActiveAlarmsPage = () => <AlertsPage openOnly />

// ── 범용 OAM 코어 섹션 (서비스 무지) ──────────────────────────────
const CORE_SECTIONS: RouteSection[] = [
  // ── 운용(ops) ──
  {
    key: 'dashboard',
    label: '대시보드',
    icon: LayoutDashboard,
    area: 'ops',
    basePath: '/dashboard',
    defaultPath: '/dashboard',
    order: 10,
    routes: [
      { path: '/dashboard', title: '종합 현황', component: DashboardPage },
    ],
  },
  {
    key: 'fault',
    label: '장애',
    icon: Bell,
    area: 'ops',
    basePath: '/alerts',
    defaultPath: '/alerts/active',
    order: 20,
    routes: [
      { path: '/alerts/active',  title: '활성 알람',        component: ActiveAlarmsPage },
      { path: '/alerts/history', title: '알람·이벤트 이력', component: AlertsPage },
    ],
  },
  // ── 관리(admin) ──
  {
    key: 'system',
    label: '시스템',
    icon: Server,
    area: 'admin',
    basePath: '/deploy',
    defaultPath: '/deploy/servers',
    order: 60,
    routes: [
      { path: '/deploy/servers',   title: '시스템/인프라', component: ServersPage,     adminOnly: true },
      { path: '/deploy/services',  title: 'HA 서비스',     component: HaServicesPage,  adminOnly: true },
      { path: '/deploy/packages',  title: '패키지',        component: PackagesPage,    adminOnly: true },
    ],
  },
  {
    key: 'release',
    label: '릴리스',
    icon: Rocket,
    area: 'admin',
    basePath: '/release',
    defaultPath: '/release/verify',
    order: 70,
    prodHidden: true,                    // 배포 콘솔에서 숨김 (운영자는 패키징/검증 불필요)
    routes: [
      { path: '/release/verify',          title: '검증',         component: VerificationV2Page, adminOnly: true },
      { path: '/release/verify-history',  title: '검증 이력',     component: VerificationHistoryPage, adminOnly: true },
      { path: '/release/package',         title: '패키징',       component: ServicesPage, adminOnly: true },
    ],
  },
  {
    key: 'docs',
    label: '문서',
    icon: BookOpen,
    area: 'admin',
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

// 평탄화된 라우트 리스트 — <Routes> 렌더용
export const FLAT_ROUTES: RouteDef[] = VISIBLE_SECTIONS.flatMap(s => s.routes)

// 경로 → section/route 조회. 섹션이 자기 basePath 밖의 route 도 가질 수 있어(예: 구성↔서비스정의)
// basePath prefix 가 아니라 route 멤버십으로 매칭한다.
export function findSectionByPath(pathname: string): RouteSection | undefined {
  return SECTIONS.find(s => s.routes.some(r => pathname === r.path || pathname.startsWith(r.path + '/')))
}

export function findRouteByPath(pathname: string): RouteDef | undefined {
  return FLAT_ROUTES.find(r => r.path === pathname)
}
