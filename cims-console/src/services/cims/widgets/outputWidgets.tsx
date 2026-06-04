// CIMS 출력(read-only) 위젯 — 서비스 상태/통계/이력 페이지 본문을 위젯으로 노출.
// 페이지(고정 화면)였던 것을 위젯화 → 운영자가 섹션 레이아웃(EditableLayout)에 자유 배치.
// 본문 로직은 기존 page 컴포넌트 그대로 재사용(중복 0). config 로 인스턴스 파라미터 주입.
import type { WidgetDef, WidgetProps } from '../../../widgets/types'
import {
  VolteKpiCard, PttKpiCard, TrendCard, AnomalyCard,
  VolteCallsCard, PttGroupsCard, EventFeedCard, OrgStatsCard, SubscriberLookup,
} from '../pages/ServiceStatusPage'
import StatsPage from '../pages/StatsPage'
import StatsMessagesPage from '../pages/StatsMessagesPage'
import VolteHistoryPage from '../pages/VolteHistoryPage'
import PttHistoryPage from '../pages/PttHistoryPage'

// 서비스 현황 — 섹션별 개별 위젯 (각 위젯 자체 데이터 페칭, /service/live·/trend 는 공유 폴러)
export const svcVolteKpiWidget: WidgetDef = { id: 'cims.svc-volte-kpi', title: 'VoLTE 요약', category: 'service', component: VolteKpiCard, defaultSize: { w: 6 }, adminOnly: true }
export const svcPttKpiWidget: WidgetDef = { id: 'cims.svc-ptt-kpi', title: 'PTT 요약 · 노드 분산', category: 'service', component: PttKpiCard, defaultSize: { w: 6 }, adminOnly: true }
export const svcTrendWidget: WidgetDef = { id: 'cims.svc-trend', title: '동시 사용량 추세', category: 'service', component: TrendCard, defaultSize: { w: 6 }, adminOnly: true }
export const svcAnomalyWidget: WidgetDef = { id: 'cims.svc-anomaly', title: '서비스 이상 징후', category: 'service', component: AnomalyCard, defaultSize: { w: 6 }, adminOnly: true }
export const svcVolteCallsWidget: WidgetDef = { id: 'cims.svc-volte-calls', title: 'VoLTE 활성 호', category: 'service', component: VolteCallsCard, defaultSize: { w: 6 }, adminOnly: true }
export const svcPttGroupsWidget: WidgetDef = { id: 'cims.svc-ptt-groups', title: 'PTT 활성 그룹', category: 'service', component: PttGroupsCard, defaultSize: { w: 6 }, adminOnly: true }
export const svcEventsWidget: WidgetDef = { id: 'cims.svc-events', title: '라이브 이벤트', category: 'event', component: EventFeedCard, defaultSize: { w: 6 }, adminOnly: true }
export const svcOrgWidget: WidgetDef = { id: 'cims.svc-org', title: '조직별 서비스 이용', category: 'service', component: OrgStatsCard, defaultSize: { w: 12 }, adminOnly: true }

// 가입자 조회 (특정 가입자 현재 상태) — 기존 id 유지(하위호환), 내용은 조회 위젯으로 정정
export const subscriberStatusWidget: WidgetDef = {
  id: 'cims.subscriber-status',
  title: '가입자 조회',
  category: 'service',
  component: SubscriberLookup,
  defaultSize: { w: 12 },
  adminOnly: true,
}

export const serviceStatsWidget: WidgetDef = {
  id: 'cims.service-stats',
  title: '서비스 통계 (VoLTE/PTT)',
  category: 'stats',
  component: (p: WidgetProps) => {
    const svc = p.config?.svcType === 'ptt' ? 'ptt' : 'volte'
    return <StatsPage initialSvcType={svc} />
  },
  defaultSize: { w: 12 },
  adminOnly: true,
}

export const messageStatsWidget: WidgetDef = {
  id: 'cims.message-stats',
  title: '메시지 통계 (iface)',
  category: 'stats',
  component: (p: WidgetProps) => {
    const iface = typeof p.config?.iface === 'string' ? p.config.iface : 'sip'
    return <StatsMessagesPage iface={iface} />
  },
  defaultSize: { w: 12 },
  adminOnly: true,
}

export const volteHistoryWidget: WidgetDef = {
  id: 'cims.volte-history',
  title: 'VoLTE 이력',
  category: 'event',
  component: VolteHistoryPage,
  defaultSize: { w: 12 },
  adminOnly: true,
}

export const pttHistoryWidget: WidgetDef = {
  id: 'cims.ptt-history',
  title: 'PTT 이력',
  category: 'event',
  component: PttHistoryPage,
  defaultSize: { w: 12 },
  adminOnly: true,
}

export const CIMS_OUTPUT_WIDGETS: WidgetDef[] = [
  svcVolteKpiWidget, svcPttKpiWidget, svcTrendWidget, svcAnomalyWidget,
  svcVolteCallsWidget, svcPttGroupsWidget, svcEventsWidget, svcOrgWidget,
  subscriberStatusWidget, serviceStatsWidget, messageStatsWidget,
  volteHistoryWidget, pttHistoryWidget,
]
