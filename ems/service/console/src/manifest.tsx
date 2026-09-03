// CIMS 서비스 pack — 콘솔 기여 매니페스트.
//
// 범용 OAM 코어(대시보드/패키징/배포/문서) 위에 CIMS 서비스 종속 nav 를 얹는다:
//   가입자관리(조직/구성원/번호/PTT그룹) · 서비스(상태/VoLTE·PTT 이력) · 성능(VoLTE/PTT/메시지 통계).
// 다른 서비스를 붙이려면 같은 형태의 manifest 를 만들어 services/registry.ts 에 등록.

import { Users, TrendingUp, FileText } from 'lucide-react'
import type { ServiceManifest } from '@core/nav-types'

import { healthDotsWidget } from './widgets/HealthDotsWidget'
import { STAT_CARD_WIDGETS, STAT_CARD_SPLITS } from './widgets/statCards'
import { LEAK_RECLAIM_WIDGETS } from './widgets/leakReclaimWidgets'
import { cspRolesWidget } from './widgets/CspRolesWidget'
import { alertBannerWidget } from './widgets/AlertBannerWidget'
import { activeAlarmsWidget } from './widgets/ActiveAlarmsWidget'
import { recentEventsWidget } from './widgets/RecentEventsWidget'
import { activeVoipWidget } from './widgets/ActiveVoipWidget'
import { activePttWidget } from './widgets/ActivePttWidget'
import { CIMS_OUTPUT_WIDGETS } from './widgets/outputWidgets'
import { STATS_SCREEN_WIDGETS } from './widgets/statsScreens'
import { ABNORMAL_WIDGETS } from './widgets/abnormalWidgets'

import ProvisioningWorkbenchPage from './pages/ProvisioningWorkbenchPage'
import OrganizationsPage from './pages/OrganizationsPage'
import PttGroupsWorkbenchPage from './pages/PttGroupsWorkbenchPage'
import DispatchGroupsPage from './pages/DispatchGroupsPage'
import McpttPolicyPage from './pages/McpttPolicyPage'
import RegisterFlowPage from './pages/RegisterFlowPage'
import LeakReclaimsPage from './pages/LeakReclaimsPage'
import { SERVICE_DEFS_LAYOUT } from '@core/widgets/layouts'  // 코어 레이아웃 — '구성' 그룹에 배치

import {
  SERVICE_STATUS_LAYOUT, SERVICE_HISTORY_VOLTE_LAYOUT, SERVICE_HISTORY_PTT_LAYOUT,
  STATS_VOLTE_LAYOUT, STATS_PTT_LAYOUT, STATS_IFACE_LAYOUT, ABNORMAL_SESSIONS_LAYOUT,
} from './layouts'

// 출력 섹션 route = 합성 가능한 레이아웃 (고정 페이지 대체). 각 route 는 layout(seed) + layoutId 영속.
// App 의 중앙 EditablePageHost 가 layout 지정 라우트를 EditableLayout 으로 렌더한다.

export const cimsManifest: ServiceManifest = {
  id: 'cims',
  label: 'CIMS',
  widgets: [
    ...STAT_CARD_WIDGETS,          // 대시보드 현황 지표 — 서로 다른 축이라 지표 1개 = 위젯 1개
    ...LEAK_RECLAIM_WIDGETS,       // 누수 회수(sweeper) 블록
    healthDotsWidget, cspRolesWidget,
    alertBannerWidget, activeAlarmsWidget, recentEventsWidget, activeVoipWidget, activePttWidget,
    ...CIMS_OUTPUT_WIDGETS,
    ...STATS_SCREEN_WIDGETS,       // 성능 통계 — 화면 하나 = 카드 하나(카드 안 블록은 코어 위젯 그대로)
    ...ABNORMAL_WIDGETS,           // 비정상 세션 이력 — 카드 + 블록 4
  ],
  // 폐지한 묶음 위젯 → 부품 전개 (저장본이 옛 id 를 참조할 때만 쓰인다)
  splits: { ...STAT_CARD_SPLITS },
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
        { path: '/service/abnormal-sessions', title: '비정상 세션 이력', requiredRole: 'monitor',
          layout: ABNORMAL_SESSIONS_LAYOUT, layoutId: 'service.abnormal-sessions' },
        { path: '/service/register-flow',     title: '메세지 이력', component: RegisterFlowPage,     requiredRole: 'monitor',
          apis: ['flow.user', 'flow.register', 'flow.register.list', 'flow.body'] },
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
        { path: '/stats/interfaces', title: '인터페이스 통계', layout: STATS_IFACE_LAYOUT,
          layoutId: 'stats.interfaces', requiredRole: 'monitor' },
        { path: '/stats/leak-reclaims', title: '누수 회수(sweeper)', component: LeakReclaimsPage, requiredRole: 'monitor',
          apis: ['stats.leak-reclaims'] },
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
        { path: '/subscribers/organizations', title: '조직',           component: OrganizationsPage, requiredRole: 'monitor',
          apis: ['csc.orgs.list', 'csc.orgs.get', 'csc.orgs.create', 'csc.orgs.update',
                 'csc.orgs.delete', 'csc.orgs.batch-delete', 'csc.orgs.users'] },
        { path: '/subscribers/workbench',     title: '사용자',         component: ProvisioningWorkbenchPage, requiredRole: 'monitor',
          apis: ['csc.users.list', 'csc.users.get', 'csc.users.create', 'csc.users.update',
                 'csc.users.delete', 'csc.users.batch-delete', 'csc.users.subs.list',
                 'csc.users.subs.add', 'csc.users.subs.update', 'csc.users.subs.delete',
                 'csc.orgs.list'] },
        { path: '/subscribers/ptt-groups',    title: 'PTT 그룹',       component: PttGroupsWorkbenchPage, requiredRole: 'monitor',
          apis: ['csc.ptt-groups.list', 'csc.ptt-groups.get', 'csc.ptt-groups.create',
                 'csc.ptt-groups.update', 'csc.ptt-groups.delete',
                 'csc.ptt-groups.members.list', 'csc.ptt-groups.members.add',
                 'csc.ptt-groups.members.remove', 'csc.users.list', 'csc.orgs.list'] },
        { path: '/subscribers/dispatch-groups', title: '관제 그룹',    component: DispatchGroupsPage, requiredRole: 'monitor',
          apis: ['csc.dispatch-groups.list', 'csc.dispatch-groups.get', 'csc.dispatch-groups.create',
                 'csc.dispatch-groups.update', 'csc.dispatch-groups.delete',
                 'csc.dispatch-groups.members.list', 'csc.dispatch-groups.members.add',
                 'csc.dispatch-groups.members.remove', 'csc.dispatch-groups.monitor-targets.put',
                 'csc.dispatch-groups.ptt-targets.put', 'csc.users.list', 'csc.orgs.list', 'csc.ptt-groups.list'] },
        { path: '/subscribers/mcptt-policy',  title: 'MCPTT 정책',     component: McpttPolicyPage, requiredRole: 'monitor',
          apis: ['csc.mcptt.service-config.get', 'csc.mcptt.service-config.update'] },
        { path: '/deploy/service-defs',       title: '서비스 정의',
          layout: SERVICE_DEFS_LAYOUT, layoutId: 'deploy.service-defs', adminOnly: true },
      ],
    },
  ],
}
