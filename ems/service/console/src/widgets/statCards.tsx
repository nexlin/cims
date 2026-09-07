// CIMS 현황 지표 카드 — 카드 1장 = 위젯 1개. 지표 목록은 아래 HEALTH_METRICS 선언 표 하나가 정본이고,
// 위젯은 makeMetricWidget 팩토리가 표에서 생성한다(지표별 컴포넌트 없음).
//
// 용어: 이 카드들은 **KPI 가 아니다.** 시점 현재값 — 재고(가입자·번호·그룹 총수, FCAPS 의 Configuration)와
// 실시간 점유(등록 단말·활성 호·RTP 포트 사용률)다. KPI(성능지표: 성공률·시도수·평균 통화시간)는
// 기간에 대한 측정치이고 성능 메뉴(`/stats`) 와 `shape.kpi` 가 담당한다.
//
// 데이터는 전부 `stats.health` 응답 1개에서 나온다. useSharedHealth 가 모듈 싱글톤 폴러라 카드를
// 몇 장 띄워도 호출은 5초당 1회로 동일하다(분해에 따른 부하 증가 없음).
import type { CSSProperties } from 'react'
import { useSharedHealth, type HistorySample } from '@core/widgets/useSharedHealth'
import type { HealthResponse } from '@core/api/stats'
import type { WidgetDef } from '@core/widgets/types'
import type { SplitFn } from '@core/widgets/legacyLayout'
import { Sparkline } from './shared'

// 카드 상자 — flex 컬럼(내용 세로 중앙). 위젯 1장으로 배치되면 grid 칸을 채우고(flex:1),
// 묶음(StatCardsRow)에서는 grid 자식이라 flex 속성이 무시돼 같은 모양이 된다.
const CARD_BOX: CSSProperties = {
  flex: '1 1 auto', minHeight: 0, display: 'flex', flexDirection: 'column',
  background: 'var(--card)', border: '1px solid var(--border)', borderRadius: 'var(--radius)',
  padding: '10px 12px', textAlign: 'center', boxSizing: 'border-box',
}

// hint = 이 수치가 무엇을 세는지(툴팁). 라벨만으로는 헷갈리는 지표에만 단다.
export function StatCard({ label, value, sub, unit, series, hint }: {
  label: string; value: string | number; sub?: string; unit?: string; series?: number[]; hint?: string
}) {
  return (
    <div style={{ ...CARD_BOX, justifyContent: 'center' }} title={hint}>
      <div style={{ fontSize: 11, color: 'var(--muted-foreground)', marginBottom: 2 }}>{label}</div>
      <div style={{ fontSize: 20, fontWeight: 700, lineHeight: 1.15 }}>
        {value}
        {unit && <span style={{ fontSize: 11, color: 'var(--muted-foreground)', marginLeft: 3 }}>{unit}</span>}
      </div>
      {sub && <div style={{ fontSize: 10, color: 'var(--muted-foreground)', marginTop: 1 }}>{sub}</div>}
      {series && <Sparkline data={series} height={16} />}
    </div>
  )
}

const pct = (used: number, total: number) => (total > 0 ? Math.round(used / total * 100) : 0)

// 지표 1개 선언. label=카드에 보이는 이름(=사실상 위젯 제목), title=편집 목록에서 고를 때의 이름.
interface MetricDef {
  key: string                                   // 위젯 id 접미 (`cims.stat.<key>`)
  label: string
  title: string
  unit?: string
  value: (h: HealthResponse) => string | number
  sub?: (h: HealthResponse) => string | undefined
  series?: (hist: HistorySample[]) => number[]
  apis?: string[]
}

export const HEALTH_METRICS: MetricDef[] = [
  { key: 'subscribers', label: '가입자', title: '가입자 수', unit: '명',
    value: h => h.csp.subscribers_total ?? '–' },
  { key: 'volte-numbers', label: 'VoLTE 번호', title: 'VoLTE 번호 (등록/전체)',
    value: h => `${h.csp.volte_registered ?? 0}/${h.csp.volte_numbers ?? 0}`,
    sub: () => '등록/전체', series: hist => hist.map(s => s.volte_registered) },
  { key: 'ptt-numbers', label: 'PTT 번호', title: 'PTT 번호 (등록/전체)',
    value: h => `${h.csp.ptt_registered ?? 0}/${h.csp.ptt_numbers ?? 0}`,
    sub: () => '등록/전체', series: hist => hist.map(s => s.ptt_registered) },
  { key: 'active-calls', label: 'VoIP 활성 호', title: 'VoIP 활성 호', unit: '건',
    value: h => h.csp.active_calls, series: hist => hist.map(s => s.active_calls) },
  { key: 'ptt-groups', label: 'PTT 그룹', title: 'PTT 그룹 (활성/전체)',
    value: h => `${h.cmp.groups}/${h.csp.ptt_groups_total ?? 0}`,
    sub: () => '활성/전체', series: hist => hist.map(s => s.ptt_groups) },
  { key: 'rtp-voip', label: 'RTP VoIP', title: 'RTP 포트 — VoIP 점유',
    value: h => `${h.cmp.rtp_ports.used}/${h.cmp.rtp_ports.total}`,
    sub: h => `사용 ${pct(h.cmp.rtp_ports.used, h.cmp.rtp_ports.total)}%`,
    series: hist => hist.map(s => s.rtp_used) },
  { key: 'rtp-ptt', label: 'RTP PTT', title: 'RTP 포트 — PTT 점유',
    value: h => `${h.cmp.rtp_ports_ptt?.used ?? 0}/${h.cmp.rtp_ports_ptt?.total ?? 0}`,
    sub: h => `사용 ${pct(h.cmp.rtp_ports_ptt?.used ?? 0, h.cmp.rtp_ports_ptt?.total ?? 0)}%`,
    series: hist => hist.map(s => s.rtp_ptt_used) },
]

// 로딩 중 자리 확보(팝인 방지) — 카드 1장 크기의 스켈레톤.
function CardSkeleton({ label }: { label: string }) {
  return (
    <div style={{ ...CARD_BOX, justifyContent: 'center' }}>
      <div style={{ fontSize: 11, color: 'var(--muted-foreground)', marginBottom: 6 }}>{label}</div>
      <div style={{ height: 20, background: 'var(--secondary)', borderRadius: 4, opacity: 0.6 }} />
    </div>
  )
}

function MetricBody({ m }: { m: MetricDef }) {
  const { data, history } = useSharedHealth()
  if (!data) return <CardSkeleton label={m.label} />
  return <StatCard label={m.label} value={m.value(data)} unit={m.unit}
                   sub={m.sub?.(data)} series={m.series?.(history)} />
}

function makeMetricWidget(m: MetricDef): WidgetDef {
  return {
    id: `cims.stat.${m.key}`,
    title: m.title,
    category: 'metric',
    apis: m.apis ?? ['stats.health'],
    component: () => <MetricBody m={m} />,
    defaultSize: { w: 2, h: 6 },      // 12-칸 기준 폭 2(≈⅙) × 6행(=12vh) — 카드 1장 크기
  }
}

export const STAT_CARD_WIDGETS: WidgetDef[] = HEALTH_METRICS.map(makeMetricWidget)

// 구 묶음 위젯 `cims.kpi`("KPI (가입자/번호/호/그룹/RTP)" — 7개 지표가 한 카드) → 지표별 위젯 전개.
// 묶음 위젯 자체는 없앴다. 이미 저장된 레이아웃만 로드 시 펼쳐 준다(legacyLayout.ts).
export const STAT_CARD_SPLITS: Record<string, SplitFn> = {
  'cims.kpi': () => HEALTH_METRICS.map(m => ({ widgetId: `cims.stat.${m.key}` })),
}
