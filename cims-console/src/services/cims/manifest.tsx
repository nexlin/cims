// CIMS 서비스 pack — 콘솔 기여 매니페스트.
//
// 범용 OAM 코어(대시보드/패키징/배포/문서) 위에 CIMS 서비스 종속 nav 를 얹는다:
//   가입자관리(조직/구성원/번호/PTT그룹) · 서비스(상태/VoLTE·PTT 이력) · 통계(VoLTE/PTT/SIP/CMP/CSC/HTTPS).
// 다른 서비스를 붙이려면 같은 형태의 manifest 를 만들어 services/registry.ts 에 등록.

import { Users, TrendingUp, FileText } from 'lucide-react'
import type { ServiceManifest } from '../../nav-types'

import { healthDotsWidget } from './widgets/HealthDotsWidget'
import { kpiWidget } from './widgets/KpiWidget'
import { cspRolesWidget } from './widgets/CspRolesWidget'
import { alertBannerWidget } from './widgets/AlertBannerWidget'
import { activeAlarmsWidget } from './widgets/ActiveAlarmsWidget'
import { activeVoipWidget } from './widgets/ActiveVoipWidget'
import { activePttWidget } from './widgets/ActivePttWidget'
import { CIMS_OUTPUT_WIDGETS } from './widgets/outputWidgets'

import OrganizationsPage from './pages/OrganizationsPage'
import MembersPage from './pages/MembersPage'
import SubscriptionsPage from './pages/SubscriptionsPage'
import PttGroupsPage from './pages/PttGroupsPage'
import ServiceDescriptorsPage from '../../pages/ServiceDescriptorsPage'  // 코어 페이지 — '구성' 그룹에 배치

import LayoutRoute from '../../widgets/LayoutRoute'
import {
  SERVICE_STATUS_LAYOUT, SERVICE_HISTORY_VOLTE_LAYOUT, SERVICE_HISTORY_PTT_LAYOUT,
  STATS_VOLTE_LAYOUT, STATS_PTT_LAYOUT, STATS_SIP_LAYOUT,
  STATS_CMP_LAYOUT, STATS_CSC_LAYOUT, STATS_HTTPS_LAYOUT,
} from './layouts'

// 출력 섹션 route = 합성 가능한 레이아웃 (고정 페이지 대체). 각 route 는 layout_id 별 영속.
const lr = (layoutId: string, seed: typeof SERVICE_STATUS_LAYOUT) =>
  () => <LayoutRoute layoutId={layoutId} seed={seed} />

const serviceStatus = lr('service.status', SERVICE_STATUS_LAYOUT)
const volteHistory  = lr('service.history-volte', SERVICE_HISTORY_VOLTE_LAYOUT)
const pttHistory    = lr('service.history-ptt', SERVICE_HISTORY_PTT_LAYOUT)
const volteStats = lr('stats.volte', STATS_VOLTE_LAYOUT)
const pttStats   = lr('stats.ptt', STATS_PTT_LAYOUT)
const sipStats   = lr('stats.sip', STATS_SIP_LAYOUT)
const cmpStats   = lr('stats.cmp', STATS_CMP_LAYOUT)
const cscStats   = lr('stats.csc', STATS_CSC_LAYOUT)
const httpsStats = lr('stats.https', STATS_HTTPS_LAYOUT)

export const cimsManifest: ServiceManifest = {
  id: 'cims',
  label: 'CIMS',
  widgets: [
    healthDotsWidget, kpiWidget, cspRolesWidget,
    alertBannerWidget, activeAlarmsWidget, activeVoipWidget, activePttWidget,
    ...CIMS_OUTPUT_WIDGETS,
  ],
  sections: [
    // ── 성능 (ops) — 서비스 현황 + 통계(KPI/카운터). FCAPS Performance. ──
    {
      key: 'perf',
      label: '성능',
      icon: TrendingUp,
      area: 'ops',
      basePath: '/service',
      defaultPath: '/service/status',
      order: 30,
      routes: [
        { path: '/service/status', title: '서비스 현황', component: serviceStatus, adminOnly: true },
        { path: '/stats/volte', title: 'VoLTE 통계', component: volteStats, adminOnly: true },
        { path: '/stats/ptt',   title: 'PTT 통계',   component: pttStats,   adminOnly: true },
        { path: '/stats/sip',   title: 'SIP 통계',   component: sipStats,   adminOnly: true },
        { path: '/stats/cmp',   title: 'CMP 통계',   component: cmpStats,   adminOnly: true },
        { path: '/stats/csc',   title: 'CSC 통계',   component: cscStats,   adminOnly: true },
        { path: '/stats/https', title: 'HTTPS 통계', component: httpsStats, adminOnly: true },
      ],
    },
    // ── 기록 (ops) — 호·세션 이력(CDR 성격). FCAPS Accounting. ──
    {
      key: 'records',
      label: '기록',
      icon: FileText,
      area: 'ops',
      basePath: '/service/history',
      defaultPath: '/service/history/volte',
      order: 40,
      routes: [
        { path: '/service/history/volte',  title: 'VoLTE 호 이력',  component: volteHistory, adminOnly: true },
        { path: '/service/history/ptt',    title: 'PTT 세션 이력',  component: pttHistory, adminOnly: true },
      ],
    },
    // ── 구성 (admin) — 가입자 프로비저닝 + 서비스 정의. FCAPS Configuration. ──
    {
      key: 'config',
      label: '구성',
      icon: Users,
      area: 'admin',
      basePath: '/subscribers',
      defaultPath: '/subscribers/organizations',
      order: 50,
      routes: [
        { path: '/subscribers/organizations', title: '조직',           component: OrganizationsPage, adminOnly: true },
        { path: '/subscribers/members',       title: '사용자',         component: MembersPage, adminOnly: true },
        { path: '/subscribers/numbers',       title: '번호(VoLTE·PTT)', component: SubscriptionsPage, adminOnly: true },
        { path: '/subscribers/ptt-groups',    title: 'PTT 그룹',       component: PttGroupsPage, adminOnly: true },
        { path: '/deploy/service-defs',       title: '서비스 정의',     component: ServiceDescriptorsPage, adminOnly: true },
      ],
    },
  ],
}
