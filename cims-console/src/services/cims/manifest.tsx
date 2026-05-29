// CIMS 서비스 pack — 콘솔 기여 매니페스트.
//
// 범용 OAM 코어(대시보드/패키징/배포/문서) 위에 CIMS 서비스 종속 nav 를 얹는다:
//   가입자관리(조직/구성원/번호/PTT그룹) · 서비스(상태/VoLTE·PTT 이력) · 통계(VoLTE/PTT/SIP/CMP/CSC/HTTPS).
// 다른 서비스를 붙이려면 같은 형태의 manifest 를 만들어 services/registry.ts 에 등록.

import { Users, Radio, BarChart3 } from 'lucide-react'
import type { ServiceManifest } from '../../nav-types'

import { healthDotsWidget } from './widgets/HealthDotsWidget'
import { kpiWidget } from './widgets/KpiWidget'
import { cspRolesWidget } from './widgets/CspRolesWidget'
import { alertBannerWidget } from './widgets/AlertBannerWidget'
import { activeVoipWidget } from './widgets/ActiveVoipWidget'
import { activePttWidget } from './widgets/ActivePttWidget'

import OrganizationsPage from './pages/OrganizationsPage'
import MembersPage from './pages/MembersPage'
import SubscriptionsPage from './pages/SubscriptionsPage'
import PttGroupsPage from './pages/PttGroupsPage'
import ServiceStatusPage from './pages/ServiceStatusPage'
import VolteHistoryPage from './pages/VolteHistoryPage'
import PttHistoryPage from './pages/PttHistoryPage'
import StatsPage from './pages/StatsPage'
import StatsMessagesPage from './pages/StatsMessagesPage'

const volteStats = () => <StatsPage />
const pttStats = () => <StatsPage />
const sipStats = () => <StatsMessagesPage iface="sip" />
const cmpStats = () => <StatsMessagesPage iface="cmp" />
const cscStats = () => <StatsMessagesPage iface="csc" />
const httpsStats = () => <StatsMessagesPage iface="https" />

export const cimsManifest: ServiceManifest = {
  id: 'cims',
  label: 'CIMS',
  widgets: [
    healthDotsWidget, kpiWidget, cspRolesWidget,
    alertBannerWidget, activeVoipWidget, activePttWidget,
  ],
  sections: [
    {
      key: 'subscribers',
      label: '가입자관리',
      icon: Users,
      basePath: '/subscribers',
      defaultPath: '/subscribers/organizations',
      order: 20,
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
      order: 30,
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
      order: 40,
      routes: [
        { path: '/stats/volte', title: 'VoLTE', component: volteStats, adminOnly: true },
        { path: '/stats/ptt',   title: 'PTT',   component: pttStats,   adminOnly: true },
        { path: '/stats/sip',   title: 'SIP',   component: sipStats,   adminOnly: true },
        { path: '/stats/cmp',   title: 'CMP',   component: cmpStats,   adminOnly: true },
        { path: '/stats/csc',   title: 'CSC',   component: cscStats,   adminOnly: true },
        { path: '/stats/https', title: 'HTTPS', component: httpsStats, adminOnly: true },
      ],
    },
  ],
}
