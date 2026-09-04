// 대시보드 서비스 요약 카드 — VoLTE 한 장 · PTT 한 장.
//
// 서비스 현황(`cims.svc-volte-kpi`)과 **다른 위젯**이다. 그쪽은 그 화면에서 서비스를 파고들 때
// 필요한 값을 다 펼치고, 여기는 대시보드에서 한눈에 훑는 3개만 카드로 세운다 — 대시보드는
// "지금 서비스가 도는가"만 답하면 되고, 자세히는 카드의 [서비스 현황] 으로 건너간다.
//
// **배치 단위는 카드 하나**지만 카드 안도 바깥과 같은 48×48 셀이라(console_platform §3.0.1)
// 머리줄·타일이 각각 블록 위젯이고, 운영자가 `[⚙]` 으로 카드 안에서 자리·크기를 바꿀 수 있다.
// 두 카드의 타일 순서는 **등록 · 진행 중 · 규모** 로 맞춰 둔다 — 같은 항목(등록)이 같은 자리에
// 있어야 두 카드를 나란히 놓고 비교할 수 있다.
//
// 타일 모양은 statCards 의 `StatCard` 를 그대로 쓴다(대시보드의 다른 지표 카드와 같은 모양).
// 데이터는 서비스 현황과 같은 공유 폴러(`stats.service.live`)라 카드를 몇 장 띄워도 조회는 1회.
import { ArrowRight } from 'lucide-react'
import { useNavigate } from 'react-router-dom'
import type { ServiceLive } from '@core/api/stats'
import { makeCardWidget } from '@core/widgets/CardLayout'
import type { WidgetDef, WidgetPlacement } from '@core/widgets/types'
import { useServiceLive } from '../pages/ServiceStatusPage'
import { StatCard } from './statCards'

const API = ['stats.service.live']
const pct = (used: number, total: number) => (total > 0 ? Math.round((used / total) * 100) : 0)

// 타일 1개 선언. label=타일에 보이는 이름, title=편집 목록에서 고를 때의 이름,
// hint=라벨만으로 헷갈리는 지표의 뜻(툴팁).
interface SummaryTile {
  key: string
  label: string
  title: string
  unit?: string
  hint?: string
  value: (l: ServiceLive) => string | number
  sub?: (l: ServiceLive) => string
}

const VOLTE_TILES: SummaryTile[] = [
  { key: 'registered', label: '등록', title: 'VoLTE 요약 — 등록',
    hint: '지금 등록(REGISTER)된 VoLTE 번호 / 프로비저닝된 전체 번호',
    value: l => `${l.volte.kpi.registered}/${l.volte.kpi.numbers}`, sub: () => '등록/전체' },
  { key: 'active', label: '통화 중', title: 'VoLTE 요약 — 통화 중', unit: '건',
    hint: '진행 중인 VoLTE 통화 (호출 중은 제외)',
    value: l => l.volte.kpi.active },
  { key: 'rtp', label: 'RTP 풀', title: 'VoLTE 요약 — RTP 풀',
    hint: 'CMP VoIP RTP 포트 — 사용 / 전체',
    value: l => `${l.capacity.volte_rtp.used}/${l.capacity.volte_rtp.total}`,
    sub: l => `사용 ${pct(l.capacity.volte_rtp.used, l.capacity.volte_rtp.total)}%` },
]

const PTT_TILES: SummaryTile[] = [
  // 등록은 **번호(단말)** 축, 그룹은 **통화** 축 — 세는 대상이 다르다. 라벨만으로 헷갈려 hint 를 단다.
  { key: 'registered', label: '등록', title: 'PTT 요약 — 등록',
    hint: '지금 등록(REGISTER)된 PTT 번호 / 프로비저닝된 전체 번호',
    value: l => `${l.ptt.kpi.registered}/${l.ptt.kpi.numbers}`, sub: () => '등록/전체' },
  { key: 'groups', label: 'PTT 그룹', title: 'PTT 요약 — 그룹',
    hint: '지금 통화(그룹콜) 중인 그룹 / 프로비저닝된 전체 그룹',
    value: l => `${l.ptt.kpi.active_groups}/${l.ptt.kpi.total_groups}`, sub: () => '통화 중/전체' },
  { key: 'participants', label: '참여', title: 'PTT 요약 — 참여', unit: '명',
    hint: '통화 중인 그룹에 들어와 있는 참여자 총수',
    value: l => l.ptt.kpi.participants },
]

function TileBlock({ tile }: { tile: SummaryTile }) {
  const live = useServiceLive()
  // 아직 응답 전이면 값 자리는 em dash — 0 으로 보여 오해를 주지 않는다.
  return <StatCard label={tile.label} hint={tile.hint} unit={live ? tile.unit : undefined}
                   value={live ? tile.value(live) : '—'} sub={live ? tile.sub?.(live) : undefined} />
}

// 머리줄 — 대상 배지 + 상세(서비스 현황)로 가는 길.
function HeadBlock({ badge, badgeClass }: { badge: string; badgeClass: string }) {
  const navigate = useNavigate()
  return (
    <div style={{ flex: 1, minHeight: 0, display: 'flex', alignItems: 'center', gap: 8 }}>
      <span className={`badge ${badgeClass}`}>{badge}</span>
      <button className="link-btn" title="서비스 현황으로 이동"
              onClick={() => navigate('/service/status')}
              style={{ marginLeft: 'auto', display: 'inline-flex', alignItems: 'center', gap: 4, fontSize: 12 }}>
        서비스 현황 <ArrowRight size={14} />
      </button>
    </div>
  )
}

// ── 블록 위젯 등록 — 카드 안에서 id 로 찾아 그린다(낱개 배치도 그대로 성립) ─────────────
const tileWidget = (prefix: string, t: SummaryTile): WidgetDef => ({
  id: `${prefix}.${t.key}`, title: t.title, category: 'metric', apis: API,
  component: () => <TileBlock tile={t} />, defaultSize: { w: 2, h: 6 }, minSize: { h: 4 },
})
const headWidget = (prefix: string, title: string, badge: string, badgeClass: string): WidgetDef => ({
  id: `${prefix}.head`, title, category: 'control', apis: API,
  component: () => <HeadBlock badge={badge} badgeClass={badgeClass} />,
  defaultSize: { w: 6, h: 3 }, minSize: { h: 2 },
})

// 카드 안 기본 배치 — 머리줄 한 줄 + 타일 3장이 48칸을 16·16·16 으로 나눈다.
const cardLayout = (prefix: string, tiles: SummaryTile[]): WidgetPlacement[] => [
  { widgetId: `${prefix}.head`, x: 0, y: 0, w: 48, h: 9 },
  ...tiles.map((t, i) => ({ widgetId: `${prefix}.${t.key}`, x: i * 16, y: 9, w: 16, h: 39 })),
]

const VOLTE = 'cims.volte-summary'
const PTT = 'cims.ptt-summary'

export const volteSummaryWidget: WidgetDef = makeCardWidget({
  id: VOLTE, title: 'VoLTE 요약 (등록·통화·RTP)', category: 'service',
  defaultSize: { w: 6, h: 9 }, minSize: { h: 6 }, layout: cardLayout(VOLTE, VOLTE_TILES),
})
export const pttSummaryWidget: WidgetDef = makeCardWidget({
  id: PTT, title: 'PTT 요약 (등록·그룹·참여)', category: 'service',
  defaultSize: { w: 6, h: 9 }, minSize: { h: 6 }, layout: cardLayout(PTT, PTT_TILES),
})

export const DASHBOARD_CARD_WIDGETS: WidgetDef[] = [
  volteSummaryWidget, headWidget(VOLTE, 'VoLTE 요약 — 머리줄', 'VoLTE', 'badge--blue'),
  ...VOLTE_TILES.map(t => tileWidget(VOLTE, t)),
  pttSummaryWidget, headWidget(PTT, 'PTT 요약 — 머리줄', 'PTT', 'badge--green'),
  ...PTT_TILES.map(t => tileWidget(PTT, t)),
]
