// CIMS 출력 섹션의 기본 레이아웃 seed — 각 출력 route 가 EditableLayout 으로 합성.
// layout_id 는 route 경로에서 파생(예: '/stats/volte' → 'stats.volte'). 저장본 없으면 이 seed.
// 운영자는 [✎ 편집]으로 어느 페이지에든 위젯을 추가/제거/배치할 수 있다.
import type { PageLayout } from '@core/widgets/types'

const one = (id: string, title: string, widgetId: string,
             config?: Record<string, unknown>): PageLayout => ({
  id, title, widgets: [{ widgetId, config }],
})

// 서비스 섹션 — 서비스 현황은 섹션별 개별 위젯을 합성(운영자가 ✎ 편집으로 재배치 가능)
export const SERVICE_STATUS_LAYOUT: PageLayout = {
  id: 'service.status', title: '서비스 현황',
  widgets: [
    // 요약(작은 위젯) + 상세(탭 통합 위젯) — 빈 위젯/헤더 스크롤 완화
    { widgetId: 'cims.svc-volte-kpi', w: 6 }, { widgetId: 'cims.svc-ptt-kpi', w: 6 },
    { widgetId: 'cims.svc-trend', w: 6 }, { widgetId: 'cims.svc-anomaly', w: 6 },
    { widgetId: 'cims.svc-detail', w: 12, h: 2 },
  ],
}
export const SERVICE_HISTORY_VOLTE_LAYOUT = one('service.history-volte', 'VoLTE 이력', 'cims.volte-history')
export const SERVICE_HISTORY_PTT_LAYOUT   = one('service.history-ptt', 'PTT 이력', 'cims.ptt-history')

// 통계 섹션
export const STATS_VOLTE_LAYOUT = one('stats.volte', 'VoLTE 통계', 'cims.service-stats', { svcType: 'volte' })
export const STATS_PTT_LAYOUT   = one('stats.ptt', 'PTT 통계', 'cims.service-stats', { svcType: 'ptt' })
export const STATS_SIP_LAYOUT   = one('stats.sip', 'SIP 메시지', 'cims.message-stats', { iface: 'sip' })
export const STATS_CMP_LAYOUT   = one('stats.cmp', 'CMP 메시지', 'cims.message-stats', { iface: 'cmp' })
export const STATS_CSC_LAYOUT   = one('stats.csc', 'CSC 메시지', 'cims.message-stats', { iface: 'csc' })
export const STATS_HTTPS_LAYOUT = one('stats.https', 'HTTPS 메시지', 'cims.message-stats', { iface: 'https' })
