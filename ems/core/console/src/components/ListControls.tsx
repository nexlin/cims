// 목록 페이지 공통 컨트롤 (AlertsPage / AlarmAnalysisPage 공유).
//   OAM 콘솔 목록은 페이지 스크롤 누적("더 보기") 대신 화면 내 고정 목록 + 페이저로
//   넘긴다 — 툴바·컬럼 헤더·페이저가 항상 보이고 표 영역만 페이지 단위로 교체.
//   조회 조건(알람/이벤트 전환·기간)은 페이지 파라미터를 읽고 쓰는 조각으로 둔다 — 이력·분석
//   위젯이 자기 안에 들이기도 하고, 컨트롤 위젯(widgets/core/faultWidgets.tsx)으로 따로 놓기도
//   하므로 소유 선언(usePageControl)은 여기서 하지 않는다.

import { usePageParam } from '../widgets/pageParams'
import { ChevronLeft, ChevronRight } from 'lucide-react'

export function DaysButtons({ days, onChange }: { days: number; onChange: (d: number) => void }) {
  return (
    <>
      <span style={{ fontSize: 13, color: 'var(--muted-foreground)' }}>기간:</span>
      {[1, 7, 30, 90].map(d => (
        <button key={d}
          className={`btn btn--sm ${days === d ? 'btn--primary' : 'btn--ghost'}`}
          onClick={() => onChange(d)}>
          {d === 1 ? '오늘' : `${d}일`}
        </button>
      ))}
    </>
  )
}

// 페이지 내비게이션 — «/» 는 처음/끝, Chevron 은 한 페이지 이동. count=0 이면 렌더 생략.
export function Pager({ page, count, pageSize, onPage, unit = '건' }: {
  page: number
  count: number
  pageSize: number
  onPage: (p: number) => void
  unit?: string
}) {
  if (count === 0) return null
  const totalPages = Math.max(1, Math.ceil(count / pageSize))
  const cur = Math.min(page, totalPages - 1)
  const from = cur * pageSize + 1
  const to = Math.min(count, (cur + 1) * pageSize)
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '6px 12px', fontSize: 12,
                  color: 'var(--muted-foreground)', borderTop: '1px solid var(--border)', flex: 'none' }}>
      <span>{from}–{to} / {count}{unit}</span>
      <span style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 4 }}>
        <button className="btn btn--ghost btn--sm" disabled={cur === 0} onClick={() => onPage(0)}>«</button>
        <button className="btn btn--ghost btn--sm" disabled={cur === 0} onClick={() => onPage(cur - 1)} title="이전 페이지"><ChevronLeft size={14} /></button>
        <span style={{ minWidth: 56, textAlign: 'center' }}>{cur + 1} / {totalPages}</span>
        <button className="btn btn--ghost btn--sm" disabled={cur >= totalPages - 1} onClick={() => onPage(cur + 1)} title="다음 페이지"><ChevronRight size={14} /></button>
        <button className="btn btn--ghost btn--sm" disabled={cur >= totalPages - 1} onClick={() => onPage(totalPages - 1)}>»</button>
      </span>
    </div>
  )
}

// 알람/이벤트 전환 — 기존 화면과 같은 탭 모양(카드 껍데기 없음). 파라미터 `atab` 을 읽고 쓴다.
export function AlarmEventTabs() {
  const [tab, setTab] = usePageParam('atab')
  const cur = tab || 'alarms'
  return (
    <div className="tab-nav">
      <button className={`tab-btn ${cur === 'alarms' ? 'tab-btn--active' : ''}`}
              onClick={() => setTab('alarms')}>알람</button>
      <button className={`tab-btn ${cur === 'events' ? 'tab-btn--active' : ''}`}
              onClick={() => setTab('events')}>이벤트</button>
    </div>
  )
}

// 기간 선택 — 파라미터 `days` 를 읽고 쓴다. 카드(panel)로 감싸지 않는다(툴바 바 모습 그대로).
export function PeriodDaysControl() {
  const [days, setDays] = usePageParam('days')
  return (
    <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
      <DaysButtons days={Number(days) || 7} onChange={d => setDays(String(d))} />
    </div>
  )
}
