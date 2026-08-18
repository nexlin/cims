// 목록 페이지 공통 컨트롤 (AlertsPage / AlarmAnalysisPage 공유).
//   OAM 콘솔 목록은 페이지 스크롤 누적("더 보기") 대신 화면 내 고정 목록 + 페이저로
//   넘긴다 — 툴바·컬럼 헤더·페이저가 항상 보이고 표 영역만 페이지 단위로 교체.

export function DaysButtons({ days, onChange }: { days: number; onChange: (d: number) => void }) {
  return (
    <>
      <span style={{ fontSize: 13, color: 'var(--text-muted)' }}>기간:</span>
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

// 페이지 내비게이션 — «/» 는 처음/끝, ◀/▶ 는 한 페이지 이동. count=0 이면 렌더 생략.
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
                  color: 'var(--text-muted)', borderTop: '1px solid var(--border)', flex: 'none' }}>
      <span>{from}–{to} / {count}{unit}</span>
      <span style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 4 }}>
        <button className="btn btn--ghost btn--sm" disabled={cur === 0} onClick={() => onPage(0)}>«</button>
        <button className="btn btn--ghost btn--sm" disabled={cur === 0} onClick={() => onPage(cur - 1)}>◀</button>
        <span style={{ minWidth: 56, textAlign: 'center' }}>{cur + 1} / {totalPages}</span>
        <button className="btn btn--ghost btn--sm" disabled={cur >= totalPages - 1} onClick={() => onPage(cur + 1)}>▶</button>
        <button className="btn btn--ghost btn--sm" disabled={cur >= totalPages - 1} onClick={() => onPage(totalPages - 1)}>»</button>
      </span>
    </div>
  )
}
