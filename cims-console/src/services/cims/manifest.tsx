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

import ProvisioningWorkbenchPage from './pages/ProvisioningWorkbenchPage'
import OrganizationsPage from './pages/OrganizationsPage'
import MembersPage from './pages/MembersPage'
import SubscriptionsPage from './pages/SubscriptionsPage'
import PttGroupsPage from './pages/PttGroupsPage'
import LeakReclaimsPage from './pages/LeakReclaimsPage'
import AbnormalSessionsPage from './pages/AbnormalSessionsPage'
import ServiceDescriptorsPage from '../../pages/ServiceDescriptorsPage'  // 코어 페이지 — '구성' 그룹에 배치

import {
  SERVICE_STATUS_LAYOUT, SERVICE_HISTORY_VOLTE_LAYOUT, SERVICE_HISTORY_PTT_LAYOUT,
  STATS_VOLTE_LAYOUT, STATS_PTT_LAYOUT, STATS_SIP_LAYOUT,
  STATS_CMP_LAYOUT, STATS_CSC_LAYOUT, STATS_HTTPS_LAYOUT,
} from './layouts'

// 출력 섹션 route = 합성 가능한 레이아웃 (고정 페이지 대체). 각 route 는 layout(seed) + layoutId 영속.
// App 의 중앙 EditablePageHost 가 layout 지정 라우트를 EditableLayout 으로 렌더한다.

export const cimsManifest: ServiceManifest = {
  id: 'cims',
  label: 'CIMS',
  widgets: [
    healthDotsWidget, kpiWidget, cspRolesWidget,
    alertBannerWidget, activeAlarmsWidget, activeVoipWidget, activePttWidget,
    ...CIMS_OUTPUT_WIDGETS,
  ],
  sections: [
    // ── 서비스 (ops) — 서비스 이용 현황(실시간) + 호·세션 이력. 대상(서비스 호/세션) 기준 묶음. ──
    {
      key: 'service',
      label: '서비스',
      icon: FileText,
      area: 'ops',
      basePath: '/service',
      defaultPath: '/service/status',
      order: 30,
      routes: [
        { path: '/service/status',         title: '서비스 현황',    layout: SERVICE_STATUS_LAYOUT,        layoutId: 'service.status',        requiredRole: 'monitor' },
        { path: '/service/history/volte',  title: 'VoLTE 호 이력',  layout: SERVICE_HISTORY_VOLTE_LAYOUT, layoutId: 'service.history-volte', requiredRole: 'monitor' },
        { path: '/service/history/ptt',    title: 'PTT 세션 이력',  layout: SERVICE_HISTORY_PTT_LAYOUT,   layoutId: 'service.history-ptt',   requiredRole: 'monitor' },
        { path: '/service/abnormal-sessions', title: '비정상 세션 이력', component: AbnormalSessionsPage, requiredRole: 'monitor' },
      ],
    },
    // ── 성능 (ops) — 통계(KPI/카운터). FCAPS Performance. ──
    {
      key: 'perf',
      label: '성능',
      icon: TrendingUp,
      area: 'ops',
      basePath: '/stats',
      defaultPath: '/stats/volte',
      order: 35,
      routes: [
        { path: '/stats/volte', title: 'VoLTE 통계', layout: STATS_VOLTE_LAYOUT, layoutId: 'stats.volte', requiredRole: 'monitor' },
        { path: '/stats/ptt',   title: 'PTT 통계',   layout: STATS_PTT_LAYOUT,   layoutId: 'stats.ptt',   requiredRole: 'monitor' },
        { path: '/stats/sip',   title: 'SIP 통계',   layout: STATS_SIP_LAYOUT,   layoutId: 'stats.sip',   requiredRole: 'monitor' },
        { path: '/stats/cmp',   title: 'CMP 통계',   layout: STATS_CMP_LAYOUT,   layoutId: 'stats.cmp',   requiredRole: 'monitor' },
        { path: '/stats/csc',   title: 'CSC 통계',   layout: STATS_CSC_LAYOUT,   layoutId: 'stats.csc',   requiredRole: 'monitor' },
        { path: '/stats/https', title: 'HTTPS 통계', layout: STATS_HTTPS_LAYOUT, layoutId: 'stats.https', requiredRole: 'monitor' },
        { path: '/stats/leak-reclaims', title: '누수 회수(sweeper)', component: LeakReclaimsPage, requiredRole: 'monitor' },
      ],
    },
    // ── 구성 (admin) — 가입자 프로비저닝 + 서비스 정의. FCAPS Configuration. ──
    {
      key: 'config',
      label: '구성',
      icon: Users,
      area: 'admin',
      basePath: '/subscribers',
      defaultPath: '/subscribers/workbench',
      order: 50,
      routes: [
        { path: '/subscribers/workbench',     title: '통합 관리',       component: ProvisioningWorkbenchPage, requiredRole: 'monitor' },
        { path: '/subscribers/organizations', title: '조직',           component: OrganizationsPage, requiredRole: 'monitor' },
        { path: '/subscribers/members',       title: '사용자',         component: MembersPage, requiredRole: 'monitor' },
        { path: '/subscribers/numbers',       title: '번호(VoLTE·PTT)', component: SubscriptionsPage, requiredRole: 'monitor' },
        { path: '/subscribers/ptt-groups',    title: 'PTT 그룹',       component: PttGroupsPage, requiredRole: 'monitor' },
        { path: '/deploy/service-defs',       title: '서비스 정의',     component: ServiceDescriptorsPage, adminOnly: true },
      ],
    },
  ],
}
