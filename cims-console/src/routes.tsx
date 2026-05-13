import type { ComponentType } from 'react'
import type { LucideIcon } from 'lucide-react'
import {
  LayoutDashboard,
  Users,
  Radio,
  BarChart3,
  FlaskConical,
  Rocket,
  FileText,
} from 'lucide-react'

import DashboardPage from './pages/DashboardPage'
import OrganizationsPage from './pages/OrganizationsPage'
import MembersPage from './pages/MembersPage'
import SubscriptionsPage from './pages/SubscriptionsPage'
import PttGroupsPage from './pages/PttGroupsPage'
import ServiceStatusPage from './pages/ServiceStatusPage'
import VolteHistoryPage from './pages/VolteHistoryPage'
import PttHistoryPage from './pages/PttHistoryPage'
import StatsPage from './pages/StatsPage'
import StatsMessagesPage from './pages/StatsMessagesPage'
import ServicesPage from './pages/ServicesPage'
import PackagesPage from './pages/PackagesPage'
import ServersPage from './pages/ServersPage'
import HaServicesPage from './pages/HaServicesPage'
import VerificationV2Page from './pages/VerificationV2Page'
import VerificationHistoryPage from './pages/VerificationHistoryPage'
import DocsPage from './pages/DocsPage'

export type RouteDef = {
  path: string
  title: string
  component: ComponentType
  adminOnly?: boolean
}

export type RouteSection = {
  key: string
  label: string
  icon: LucideIcon
  basePath: string
  defaultPath: string
  routes: RouteDef[]
  // VITE_CONSOLE_TARGET=prod 빌드에서 숨김 (배포 콘솔 = 운영자용. 패키징 메뉴 불필요)
  prodHidden?: boolean
}

// 콘솔 타겟 — 'dev' (TB-Console / 검증용) | 'prod' (배포본 — 운영자용, 제한된 메뉴)
// vite build 시 VITE_CONSOLE_TARGET=prod 환경변수 주입으로 결정.
const CONSOLE_TARGET = ((import.meta as unknown as { env: Record<string, string> }).env?.VITE_CONSOLE_TARGET) || 'dev'
export const IS_PROD_CONSOLE = CONSOLE_TARGET === 'prod'

const volteStats = () => <StatsPage />
const pttStats = () => <StatsPage />
const sipStats = () => <StatsMessagesPage iface="sip" />
const cmpStats = () => <StatsMessagesPage iface="cmp" />
const cscStats = () => <StatsMessagesPage iface="csc" />
const httpsStats = () => <StatsMessagesPage iface="https" />

export const SECTIONS: RouteSection[] = [
  {
    key: 'dashboard',
    label: '대시보드',
    icon: LayoutDashboard,
    basePath: '/dashboard',
    defaultPath: '/dashboard',
    routes: [
      { path: '/dashboard', title: '대시보드', component: DashboardPage },
    ],
  },
  {
    key: 'subscribers',
    label: '가입자관리',
    icon: Users,
    basePath: '/subscribers',
    defaultPath: '/subscribers/organizations',
    routes: [
      { path: '/subscribers/organizations', title: '조직', component: OrganizationsPage, adminOnly: true },
      { path: '/subscribers/members',       title: '구성원', component: MembersPage, adminOnly: true },
      { path: '/subscribers/numbers',       title: 'VoLTE/PTT 번호', component: SubscriptionsPage, adminOnly: true },
      { path: '/subscribers/ptt-groups',    title: 'PTT 그룹', component: PttGroupsPage, adminOnly: true },
    ],
  },
  {
    key: 'service',
    label: '서비스',
    icon: Radio,
    basePath: '/service',
    defaultPath: '/service/status',
    routes: [
      { path: '/service/status',         title: '실시간 상태', component: ServiceStatusPage, adminOnly: true },
      { path: '/service/history/volte',  title: 'VoLTE 이력', component: VolteHistoryPage, adminOnly: true },
      { path: '/service/history/ptt',    title: 'PTT 이력',   component: PttHistoryPage, adminOnly: true },
    ],
  },
  {
    key: 'stats',
    label: '통계',
    icon: BarChart3,
    basePath: '/stats',
    defaultPath: '/stats/volte',
    routes: [
      { path: '/stats/volte', title: 'VoLTE', component: volteStats, adminOnly: true },
      { path: '/stats/ptt',   title: 'PTT',   component: pttStats,   adminOnly: true },
      { path: '/stats/sip',   title: 'SIP',   component: sipStats,   adminOnly: true },
      { path: '/stats/cmp',   title: 'CMP',   component: cmpStats,   adminOnly: true },
      { path: '/stats/csc',   title: 'CSC',   component: cscStats,   adminOnly: true },
      { path: '/stats/https', title: 'HTTPS', component: httpsStats, adminOnly: true },
    ],
  },
  {
    key: 'release',
    label: '패키징',
    icon: FlaskConical,
    basePath: '/release',
    defaultPath: '/release/verify',
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
    defaultPath: '/deploy/services',
    routes: [
      { path: '/deploy/services',  title: '서버 + HA',         component: HaServicesPage, adminOnly: true },
      { path: '/deploy/packages',  title: '패키지',            component: PackagesPage,   adminOnly: true },
      { path: '/deploy/servers',   title: '서버 Inspector',    component: ServersPage,    adminOnly: true },
    ],
  },
  {
    key: 'docs',
    label: '문서',
    icon: FileText,
    basePath: '/docs',
    defaultPath: '/docs',
    routes: [
      { path: '/docs', title: '문서', component: DocsPage },
    ],
  },
]

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
