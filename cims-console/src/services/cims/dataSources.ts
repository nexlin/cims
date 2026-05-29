// CIMS 데이터 소스 — 범용 shape 위젯(차트/표/KPI/분포)이 선택지로 쓰는 통계 소스.
// 같은 shape 에 소스만 다른 출력(메시지 iface별, 서비스 volte/ptt)을 위젯 추가 없이 소스 등록으로 확장.
import { api } from '../../api/client'
import { statsApi, type ServiceStatsResponse, type MessagesResponse } from '../../api/stats'
import type { DataSource } from '../../widgets/shapes/types'

function fmtDuration(sec: number): string {
  const m = Math.floor(sec / 60), s = Math.round(sec % 60)
  return `${m}:${String(s).padStart(2, '0')}`
}

// 메시지 iface별 통계 — /stats/messages/{iface} (hour 버킷 + 메서드 카운트).
interface IfaceMsgStats { date: string; total: number; buckets: { hour: number; count: number }[]; method_counts: Record<string, number> }

const msgSource = (iface: string, label: string): DataSource<IfaceMsgStats> => ({
  id: `cims.msg.${iface}`, label, shapes: ['time-bar', 'table'],
  load: p => api.get<IfaceMsgStats>(`/stats/messages/${iface}?date=${p.date}`),
  toTimeBar: raw => ({ buckets: raw.buckets.map(b => ({ label: b.hour, value: b.count })) }),
  toTable: raw => ({ columns: ['메서드', '건수'], rows: Object.entries(raw.method_counts).map(([k, v]) => ({ key: k, value: v })) }),
})

const svcVolte: DataSource<ServiceStatsResponse> = {
  id: 'cims.svc.volte', label: 'VoLTE 서비스', shapes: ['kpi', 'time-bar', 'distribution'],
  load: p => statsApi.service('volte', { date: p.date, granularity: p.granularity }),
  toKpi: raw => ({ items: raw.voip ? [
    { label: '호 시도', value: raw.voip.total_attempts, unit: '건' },
    { label: '호 성공률', value: raw.voip.success_rate, unit: '%' },
    { label: '평균 통화시간', value: fmtDuration(raw.voip.avg_duration_sec) },
    { label: '성공', value: raw.voip.total_success, unit: '건' },
  ] : [] }),
  toTimeBar: raw => ({ buckets: (raw.voip?.buckets ?? []).map(b => ({ label: b.hour ?? b.date ?? '', value: b.attempts })) }),
  toDistribution: raw => ({ total: raw.voip?.total_attempts ?? 0,
    items: Object.entries(raw.voip?.end_reasons ?? {}).map(([k, v]) => ({ label: k, value: v })) }),
}

const svcPtt: DataSource<ServiceStatsResponse> = {
  id: 'cims.svc.ptt', label: 'PTT 서비스', shapes: ['kpi', 'time-bar', 'distribution'],
  load: p => statsApi.service('ptt', { date: p.date, granularity: p.granularity }),
  toKpi: raw => ({ items: raw.ptt ? [
    { label: '그룹콜 수', value: raw.ptt.total_calls, unit: '건' },
    { label: '평균 세션 시간', value: fmtDuration(raw.ptt.avg_duration_sec) },
  ] : [] }),
  toTimeBar: raw => ({ buckets: (raw.ptt?.buckets ?? []).map(b => ({ label: b.hour ?? b.date ?? '', value: b.calls })) }),
  toDistribution: raw => ({ total: raw.ptt?.total_calls ?? 0,
    items: Object.entries(raw.ptt?.by_group ?? {}).map(([k, v]) => ({ label: `그룹 ${k}`, value: v })) }),
}

const msgSummary: DataSource<MessagesResponse> = {
  id: 'cims.msg-summary', label: '메시지 합계 (VoIP+PTT)', shapes: ['time-bar'],
  load: p => statsApi.messages({ date: p.date, granularity: p.granularity }),
  toTimeBar: raw => ({ buckets: raw.buckets.map(b => ({ label: b.hour ?? '', value: b.total })) }),
}

export const CIMS_DATA_SOURCES: DataSource[] = [
  msgSource('sip', 'SIP 메시지'),
  msgSource('cmp', 'CMP 메시지'),
  msgSource('csc', 'CSC 메시지'),
  msgSource('https', 'HTTPS 메시지'),
  svcVolte, svcPtt, msgSummary,
] as DataSource[]
