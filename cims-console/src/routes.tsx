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
}

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
    key: 'testbed',
    label: '빌드 · 검증',
    icon: FlaskConical,
    basePath: '/testbed',
    defaultPath: '/testbed/modules',
    routes: [
      { path: '/testbed/modules',          title: '빌드',         component: ServicesPage, adminOnly: true },
      { path: '/testbed/verify-v2',        title: '검증 실행',     component: VerificationV2Page, adminOnly: true },
      { path: '/testbed/verify-history',   title: '검증 이력',     component: VerificationHistoryPage, adminOnly: true },
    ],
  },
  {
    key: 'deploy',
    label: '배포',
    icon: Rocket,
    basePath: '/deploy',
    defaultPath: '/deploy/packages',
    routes: [
      { path: '/deploy/packages', title: '패키지', component: PackagesPage, adminOnly: true },
      { path: '/deploy/servers',  title: '서버',   component: ServersPage,  adminOnly: true },
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

// 평탄화된 라우트 리스트 — <Routes> 렌더용
export const FLAT_ROUTES: RouteDef[] = SECTIONS.flatMap(s => s.routes)

// 경로 → section/route 조회
export function findSectionByPath(pathname: string): RouteSection | undefined {
  return SECTIONS.find(s => pathname === s.basePath || pathname.startsWith(s.basePath + '/'))
}

export function findRouteByPath(pathname: string): RouteDef | undefined {
  return FLAT_ROUTES.find(r => r.path === pathname)
}
