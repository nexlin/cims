/* PttHistoryPage — PTT 세션 이력 (세션 단일 평면)
 *
 * 기록 단위가 세션(녹취 세션 디렉터리)이므로 화면 단위도 세션이다. 그룹·1:1·임시가
 * 한 목록에 있고 종류·그룹·사람은 필터다. 축이 하나라 "어느 탭에 있나" 를 판단할 일이
 * 없고, 그룹 개명·삭제·재생성이 특수 케이스가 아니게 된다 — 세션은 그때의 사실이라
 * 나중에 변하지 않는다.
 *
 * "이 그룹의 최근 활동" 은 여기가 아니라 **PTT 그룹 › 활동** 탭에서 본다
 * (PttGroupsWorkbenchPage). 이력 페이지가 그룹 목록을 다시 그릴 이유가 없다.
 */
import { useState, useEffect, useCallback, useMemo, useRef, Fragment } from 'react'
import {
  pttApi, type PttSessionRow, type PttGroupSummary, type PttSessionKind,
} from '@core/api/ptt'
import { recordingsApi, type RecordingSegment } from '@core/api/recordings'
import type { FlowMessage } from '@core/api/flow'
import FlowPage from '@core/pages/FlowPage'
import SegmentPlayer from '@core/components/SegmentPlayer'
import { useInlineAudio, type InlineAudio } from '@core/components/useInlineAudio'
import { useToast } from '@core/components/Toast'
import { useDirectory, type Directory } from '@core/components/useDirectory'
import {
  SessionDetail, Person, recIdOf, dateOf, detailKey, fmtShortTime, fmtDur, fmtSpeechMs,
  type DetailState,
} from '@svc/components/pttSession'

// ── 종류 ────────────────────────────────────────────────────────
// group = TS 24.481 그룹 문서를 갖는 편성 엔티티, private = 1:1 (TS 24.379 §11.1),
// adhoc = 임시 그룹콜 (TS 22.179). 뒤 둘은 그룹 문서가 없는 개별 호다.
const KINDS: Array<{ id: PttSessionKind; label: string }> = [
  { id: 'group', label: '그룹' },
  { id: 'private', label: '1:1' },
  { id: 'adhoc', label: '임시' },
]
const ALL_KINDS = KINDS.map(k => k.id)
const KIND_BADGE: Record<string, string> = {
  group: 'badge--blue', private: 'badge--gray', adhoc: 'badge--yellow', unknown: 'badge--gray',
}
const KIND_LABEL: Record<string, string> = {
  group: '그룹', private: '1:1', adhoc: '임시', unknown: '미상',
}

type SortKey = 'start' | 'turns' | 'speech' | 'duration'
const SORT_LABEL: Record<SortKey, string> = {
  start: '시각', turns: '발언 턴', speech: '발화', duration: '길이',
}
const PAGE_SIZES = [20, 50, 100]

// 조회 기간 — 기본은 '오늘 고정'(관제·시험 확인이 대부분 당일이라). 최근 7일·기간 지정은
// P2 에서 연 days / from·to 파라미터를 쓴다.
type RangeId = 'day' | 'week' | 'custom'
const RANGES: Array<{ id: RangeId; label: string }> = [
  { id: 'day', label: '오늘' },
  { id: 'week', label: '최근 7일' },
  { id: 'custom', label: '기간 지정' },
]

const todayStr = () => {
  const d = new Date()
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`
}
const shiftDay = (day: string, n: number) => {
  const d = new Date(day + 'T00:00:00')
  d.setDate(d.getDate() + n)
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`
}

// 세션 하나를 가리키는 키 — 구 녹취는 세션키가 시간버킷이라 그룹키까지 묶어야 유일하다.
const rowKey = (r: PttSessionRow) => `${r.group_key}|${r.dir}`

// 목록/상세 경계 — 잡아끈 폭은 브라우저에 남긴다 (기본은 화면의 1/4)
const LIST_W_KEY = 'cims.ptt.history.listW'
const LIST_W_MIN = 240      // 요약 카드가 읽히는 최소 폭
const PANE_W_MIN = 420      // 타임바 레인·이벤트 한 줄이 읽히는 최소 폭

// 대상 = 그룹이면 그룹명, 1:1 이면 상대, 임시면 참여 요약.
// 사람은 이름으로 쓰고 번호는 hover 로 확인한다 (Person).
function Target({ r, names }: { r: PttSessionRow; names: Directory }) {
  if (r.kind === 'group') return <>{r.group_name || r.mcptt_group_id || r.group_key}</>
  const peers = r.peers || []
  if (r.kind === 'adhoc') return <><Person id={r.initiator} names={names} /> 외 {peers.length}명</>
  if (!peers.length) return <Person id={r.initiator} names={names} />
  return (
    <>
      <Person id={r.initiator} names={names} />{' ↔ '}
      {peers.map((p, i) => <Fragment key={p}>{i > 0 && ', '}<Person id={p} names={names} /></Fragment>)}
    </>
  )
}

export default function PttHistoryPage() {
  const { show } = useToast()
  const audio = useInlineAudio(useCallback((m: string) => show(m, 'err'), [show]))
  const names = useDirectory()

  // ── 조회 조건 ──
  const [date, setDate] = useState(todayStr())
  const [range, setRange] = useState<RangeId>('day')
  const [fromDate, setFrom] = useState(() => shiftDay(todayStr(), -6))
  const [toDate, setTo] = useState(todayStr())
  const [kinds, setKinds] = useState<Set<PttSessionKind>>(new Set(ALL_KINDS))
  const [groupKeys, setGroupKeys] = useState<Set<string>>(new Set())
  const [person, setPerson] = useState('')
  const [hour, setHour] = useState('')
  const [searchInput, setSearchInput] = useState('')
  const [q, setQ] = useState('')
  const [sort, setSort] = useState<SortKey>('start')
  const [order, setOrder] = useState<'asc' | 'desc'>('desc')
  const [page, setPage] = useState(0)
  const [ps, setPs] = useState(50)
  const [autoRefresh, setAR] = useState(false)
  const [dd, setDd] = useState<'group' | 'person' | null>(null)

  // ── 결과 ──
  const [rows, setRows] = useState<PttSessionRow[]>([])
  const [liveRows, setLiveRows] = useState<PttSessionRow[]>([])
  const [hours, setHours] = useState<Record<string, number>>({})
  const [total, setTotal] = useState(0)
  const [loading, setLoading] = useState(false)
  const [summaries, setSummaries] = useState<Record<string, PttGroupSummary>>({})

  const [open, setOpen] = useState<string | null>(null)
  const [detailByKey, setDetail] = useState<Map<string, DetailState>>(new Map())
  const [flow, setFlow] = useState<{ storeKey: string; date: string; nodes?: Record<string, FlowMessage[]>; messages?: FlowMessage[] } | null>(null)
  const [flowLoading, setFlowLoading] = useState(false)
  const [player, setPlayer] = useState<{ id: string; segments: RecordingSegment[]; title?: string } | null>(null)

  const kindParam = useMemo(
    () => (kinds.size === ALL_KINDS.length ? '' : [...kinds].join(',')),
    [kinds],
  )
  const groupParam = useMemo(() => [...groupKeys].join(','), [groupKeys])

  // 그룹 필터 후보 — 녹취가 있는 그룹 요약(가벼움). DB 목록이 아니라 녹취가 출처라
  // 삭제된 그룹의 과거 세션도 걸러 볼 수 있다.
  useEffect(() => {
    pttApi.summary().then(r => setSummaries(r.summaries || {})).catch(() => setSummaries({}))
  }, [])

  useEffect(() => {
    const t = setTimeout(() => { setQ(searchInput.trim()); setPage(0) }, 350)
    return () => clearTimeout(t)
  }, [searchInput])

  // 기간 프리셋 → API 파라미터. 셋 중 하나만 보낸다(서버 우선순위: from/to > date > days).
  const rangeParam = useMemo(() => (
    range === 'week' ? { days: 7 }
      : range === 'custom' ? { from: fromDate, to: toDate }
        : { date }
  ), [range, date, fromDate, toDate])

  const load = useCallback(async () => {
    setLoading(true)
    const common = { ...rangeParam, kind: kindParam, group_key: groupParam, person, q, hour }
    try {
      // 진행중은 페이징 밖 — 종료 목록을 아무리 넘겨도 '지금 열린 것' 은 늘 위에 있어야 한다.
      const [ended, live] = await Promise.all([
        pttApi.flat({ ...common, state: 'ended', sort, order, limit: ps, offset: page * ps }),
        pttApi.flat({ ...common, state: 'live', limit: 200 }),
      ])
      setRows(ended.items || [])
      setTotal(ended.total || 0)
      setHours(ended.hours || {})
      setLiveRows(live.items || [])
    } catch (e: unknown) {
      show(String(e), 'err')
      setRows([]); setLiveRows([]); setTotal(0); setHours({})
    } finally {
      setLoading(false)
    }
  }, [rangeParam, kindParam, groupParam, person, q, hour, sort, order, ps, page, show])

  useEffect(() => { load() }, [load])
  useEffect(() => {
    if (!autoRefresh) return
    const iv = setInterval(load, 15000)
    return () => clearInterval(iv)
  }, [autoRefresh, load])

  // 조건이 바뀌면 펼침·페이지를 초기화 — 다른 목록에 남은 펼침은 의미가 없다.
  const firstRun = useRef(true)
  useEffect(() => {
    if (firstRun.current) { firstRun.current = false; return }
    setOpen(null); setPage(0)
  }, [rangeParam, kindParam, groupParam, person, q, hour])

  // ── 세션 상세 lazy 로드 (그룹 활동 탭과 같은 규약) ──
  const loadDetail = useCallback(async (r: PttSessionRow) => {
    const dk = detailKey(r.group_key, r.dir)
    if (detailByKey.get(dk)?.loaded) return
    setDetail(prev => new Map(prev).set(dk, { events: [], participants: [], floor: [], segments: [], loading: true, loaded: false }))
    const recId = recIdOf(r.group_key, r.dir)
    const dt = dateOf(r.dir) || undefined
    try {
      const [ev, fl, rec] = await Promise.all([
        pttApi.events(r.group_key, r.dir, dt),
        pttApi.floor(r.group_key, r.dir, dt).catch(() => ({ floor: [] })),
        recId ? recordingsApi.get(recId).catch(() => ({ segments: [] })) : Promise.resolve({ segments: [] }),
      ])
      setDetail(prev => new Map(prev).set(dk, {
        events: ev.events || [], participants: ev.participants || [], floor: fl.floor || [],
        segments: (rec as { segments?: RecordingSegment[] }).segments || [],
        loading: false, loaded: true,
      }))
    } catch {
      setDetail(prev => new Map(prev).set(dk, { events: [], participants: [], floor: [], segments: [], loading: false, loaded: true }))
    }
  }, [detailByKey])

  const toggle = (r: PttSessionRow) => {
    const k = rowKey(r)
    setOpen(prev => {
      if (prev === k) return null
      loadDetail(r)
      return k
    })
  }

  // ── 녹취 · Flow ──
  const playAll = async (r: PttSessionRow) => {
    const recId = recIdOf(r.group_key, r.dir)
    if (!recId) { show('세션키가 올바르지 않습니다', 'err'); return }
    try {
      const rec = await recordingsApi.get(recId)
      if (rec.segments?.length) setPlayer({ id: recId, segments: rec.segments })
      else show('녹취 세그먼트가 없습니다', 'err')
    } catch (e: unknown) { show(String(e), 'err') }
  }
  const openFlow = async (r: PttSessionRow) => {
    setFlowLoading(true)
    const dt = dateOf(r.dir)
    try {
      const resp = await pttApi.flow(r.group_key, r.dir, dt || undefined)
      setFlow({ storeKey: r.group_key, date: dt, nodes: resp.nodes, messages: resp.messages })
    } catch (e: unknown) {
      show(String(e), 'err')
      setFlow({ storeKey: r.group_key, date: dt })
    } finally { setFlowLoading(false) }
  }

  const pages = Math.max(1, Math.ceil(total / ps))
  const speechSum = [...liveRows, ...rows].reduce((n, r) => n + (r.total_speech_ms ?? 0), 0)
  const peopleSeen = useMemo(
    () => [...new Set([...liveRows, ...rows].flatMap(r => r.people || []))].sort(),
    [rows, liveRows],
  )

  const cardProps = (r: PttSessionRow) => ({
    r,
    sel: open === rowKey(r),
    names,
    onSelect: () => toggle(r),
  })

  // ── 선택 세션 → 우측 상세 드로어 ──
  // 상세를 목록 행 안에서 펼치면 상세 높이(지표+타임바+이벤트)만큼 화면이 아래로 늘어나
  // 목록이 밀리고, 아래쪽 행을 펼치면 상세가 화면 밖에서 열린다. 목록은 그대로 두고
  // 상세만 고정 패널에 띄우면 스크롤 위치를 잃지 않고 세션을 바꿔 가며 볼 수 있다.
  const selRow = useMemo(
    () => [...liveRows, ...rows].find(r => rowKey(r) === open) || null,
    [liveRows, rows, open],
  )
  const selDetail = selRow ? detailByKey.get(detailKey(selRow.group_key, selRow.dir)) : undefined

  // 폭이 좁으면 나란히 두지 못한다 — 그때는 겹쳐 띄운다(오버레이).
  const bodyRef = useRef<HTMLDivElement>(null)
  const [wide, setWide] = useState(true)
  const [bodyW, setBodyW] = useState(0)
  useEffect(() => {
    const el = bodyRef.current
    if (!el || typeof ResizeObserver === 'undefined') return
    const measure = () => { setWide(el.clientWidth >= 1080); setBodyW(el.clientWidth) }
    measure()
    const ro = new ResizeObserver(measure)
    ro.observe(el)
    return () => ro.disconnect()
  }, [])

  // ── 목록/상세 경계 드래그 ──
  // 기본은 화면의 1/4 이지만 세션마다 읽고 싶은 게 달라(대상 이름이 길거나, 이벤트 사유가
  // 길거나) 고정폭은 늘 누군가에게 좁다. 잡아끈 폭은 브라우저에 남겨 다음에도 그대로 쓴다.
  const [listW, setListW] = useState<number | null>(() => {
    const v = Number(localStorage.getItem(LIST_W_KEY))
    return Number.isFinite(v) && v > 0 ? v : null
  })
  const listWidth = Math.max(
    LIST_W_MIN,
    Math.min(listW ?? (Math.round(bodyW * 0.25) || LIST_W_MIN), Math.max(LIST_W_MIN, bodyW - PANE_W_MIN)),
  )
  const dragRef = useRef<{ x: number; w: number } | null>(null)
  const onSplitDown = (e: React.MouseEvent) => {
    e.preventDefault()
    dragRef.current = { x: e.clientX, w: listWidth }
  }
  useEffect(() => {
    const onMove = (e: MouseEvent) => {
      const d = dragRef.current
      const el = bodyRef.current
      if (!d || !el) return
      const max = Math.max(LIST_W_MIN, el.clientWidth - PANE_W_MIN)
      setListW(Math.max(LIST_W_MIN, Math.min(max, d.w + (e.clientX - d.x))))
    }
    const onUp = () => {
      if (!dragRef.current) return
      dragRef.current = null
      setListW(w => { if (w) localStorage.setItem(LIST_W_KEY, String(w)); return w })
    }
    window.addEventListener('mousemove', onMove)
    window.addEventListener('mouseup', onUp)
    return () => {
      window.removeEventListener('mousemove', onMove)
      window.removeEventListener('mouseup', onUp)
    }
  }, [])
  // 더블클릭 = 기본(1/4) 복귀
  const resetSplit = () => { localStorage.removeItem(LIST_W_KEY); setListW(null) }

  // Esc = 닫기 (드로어 관례)
  useEffect(() => {
    if (!open) return
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') setOpen(null) }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [open])

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%', minHeight: 480 }}
         onClick={() => dd && setDd(null)}>

      {/* ── 툴바 1: 기간 · 검색 ── */}
      <div className="toolbar">
        <button className="btn btn--sm btn--ghost" disabled={range !== 'day'}
                onClick={() => setDate(d => shiftDay(d, -1))} title="이전 날">‹</button>
        <input type="date" className="form-input" value={date} style={{ width: 150 }}
               onChange={e => setDate(e.target.value)} aria-label="조회 날짜" />
        <button className="btn btn--sm btn--ghost" disabled={range !== 'day' || date >= todayStr()}
                onClick={() => setDate(d => shiftDay(d, 1))} title="다음 날">›</button>
        {/* 기간 프리셋 — P2 의 days/from·to 를 화면에서 쓰는 자리 */}
        {RANGES.map(r => (
          <button key={r.id} className={`btn btn--sm ${range === r.id ? 'btn--primary' : 'btn--outline'}`}
                  onClick={() => { setRange(r.id); if (r.id === 'day') setDate(todayStr()) }}>
            {r.label}
          </button>
        ))}
        {range === 'custom' && (
          <>
            <input type="date" className="form-input" value={fromDate} style={{ width: 150 }}
                   max={toDate} onChange={e => setFrom(e.target.value)} aria-label="시작 날짜" />
            <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>~</span>
            <input type="date" className="form-input" value={toDate} style={{ width: 150 }}
                   min={fromDate} max={todayStr()} onChange={e => setTo(e.target.value)} aria-label="종료 날짜" />
          </>
        )}
        <div style={{ width: 1, height: 20, background: 'var(--border)' }} />
        <input className="search-input" placeholder="그룹·번호·세션키 검색" style={{ maxWidth: 240 }}
               value={searchInput} onChange={e => setSearchInput(e.target.value)} />
        {q && <button className="btn btn--sm btn--ghost" onClick={() => setSearchInput('')}>검색 해제</button>}
        <button className="btn btn--primary btn--sm" onClick={load}>새로고침</button>
        <label style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 4, fontSize: 12, color: 'var(--text-muted)', cursor: 'pointer' }}>
          <input type="checkbox" checked={autoRefresh} onChange={e => setAR(e.target.checked)} />
          자동갱신
        </label>
      </div>

      {/* ── 툴바 2: 종류 · 그룹 · 사람 ── */}
      <div className="toolbar" style={{ borderTop: 'none' }}>
        <span style={{ fontSize: 11.5, color: 'var(--text-muted)' }}>종류</span>
        {KINDS.map(k => (
          <button key={k.id} className={`btn btn--sm ${kinds.has(k.id) ? 'btn--primary' : 'btn--outline'}`}
                  onClick={() => setKinds(prev => {
                    const n = new Set(prev)
                    if (n.has(k.id)) { if (n.size > 1) n.delete(k.id) } else n.add(k.id)
                    return n
                  })}>
            {k.label}
          </button>
        ))}
        <div style={{ width: 1, height: 20, background: 'var(--border)' }} />

        <GroupFilter summaries={summaries} selected={groupKeys} open={dd === 'group'}
                     onToggleMenu={() => setDd(v => (v === 'group' ? null : 'group'))}
                     onChange={setGroupKeys} />
        <PersonFilter value={person} candidates={peopleSeen} names={names} open={dd === 'person'}
                      onToggleMenu={() => setDd(v => (v === 'person' ? null : 'person'))}
                      onChange={v => { setPerson(v); setDd(null) }} />
        {hour && (
          <button className="btn btn--sm btn--primary" onClick={() => setHour('')}>{hour}시 ✕</button>
        )}

        <span style={{ marginLeft: 'auto', fontSize: 12, color: 'var(--text-muted)' }}>
          {loading ? '조회 중…' : <>
            <b style={{ color: 'var(--text)' }}>{total}</b>건
            {liveRows.length > 0 && <> · 진행중 <b style={{ color: 'var(--success)' }}>{liveRows.length}</b></>}
            {' · 발화 합 '}{fmtSpeechMs(speechSum)}
          </>}
        </span>
      </div>

      {/* ── 시간대 밴드 — 목록 위 전체 폭 (필터이자 그날의 분포) ── */}
      <div style={{ padding: '8px 14px', borderBottom: '1px solid var(--border)', background: 'var(--bg-soft)' }}>
        <HourHeatmap hours={hours} sel={hour} onPick={h => { setHour(prev => (prev === h ? '' : h)); setPage(0) }} />
      </div>

      {/* ── 본문 = 좌 세션 목록(요약 카드) · 우 선택 세션(발언 + 이벤트) ── */}
      <div ref={bodyRef} style={{ flex: 1, display: 'flex', minHeight: 0, position: 'relative' }}>
        <div style={{
          flex: wide ? `0 0 ${listWidth}px` : '0 0 100%', minWidth: 0,
          display: 'flex', flexDirection: 'column',
        }}>
          {/* 목록 머리 — 표 헤더가 없어진 자리의 정렬 컨트롤 */}
          <div style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '6px 10px', borderBottom: '1px solid var(--border)' }}>
            <span style={{ fontSize: 11.5, color: 'var(--text-muted)' }}>정렬</span>
            <select className="form-input" style={{ width: 96, padding: '2px 6px', fontSize: 12 }}
                    value={sort} onChange={e => { setSort(e.target.value as SortKey); setPage(0) }}>
              {(Object.keys(SORT_LABEL) as SortKey[]).map(k => <option key={k} value={k}>{SORT_LABEL[k]}</option>)}
            </select>
            <button className="btn btn--sm btn--ghost" title={order === 'desc' ? '내림차순' : '오름차순'}
                    onClick={() => setOrder(o => (o === 'desc' ? 'asc' : 'desc'))}>
              {order === 'desc' ? '▼' : '▲'}
            </button>
            <span style={{ marginLeft: 'auto', fontSize: 11.5, color: 'var(--text-muted)' }}>
              {loading ? '조회 중…' : `${total}건`}
            </span>
          </div>

          <div style={{ flex: 1, overflowY: 'auto', padding: '8px 10px', display: 'flex', flexDirection: 'column', gap: 6 }}>
            {(!rows.length && !liveRows.length && !loading) ? (
              <div className="empty" style={{ padding: 24 }}>조건에 맞는 세션이 없습니다</div>
            ) : (
              <>
                {/* 상시 세션(chat 그룹 등)은 종료가 없어 시각 정렬에서 늘 맨 아래로 밀린다 —
                    구역으로 고정해 '지금 열려 있는 것' 을 먼저 보게 한다. */}
                {liveRows.length > 0 && <SectionLabel label="진행중" n={liveRows.length} live />}
                {liveRows.map(r => <SessionCard key={rowKey(r)} {...cardProps(r)} />)}
                {liveRows.length > 0 && rows.length > 0 && <SectionLabel label="종료" n={total} />}
                {rows.map(r => <SessionCard key={rowKey(r)} {...cardProps(r)} />)}
              </>
            )}
          </div>

          <div style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '6px 10px', borderTop: '1px solid var(--border)', fontSize: 12, color: 'var(--text-muted)' }}>
            <button className="btn btn--sm btn--ghost" disabled={page <= 0} onClick={() => setPage(p => p - 1)}>‹</button>
            <span>{page + 1} / {pages}</span>
            <button className="btn btn--sm btn--ghost" disabled={page + 1 >= pages} onClick={() => setPage(p => p + 1)}>›</button>
            <select className="form-input" style={{ width: 78, marginLeft: 'auto', padding: '2px 6px', fontSize: 12 }} value={ps}
                    onChange={e => { setPs(Number(e.target.value)); setPage(0) }}>
              {PAGE_SIZES.map(n => <option key={n} value={n}>{n}개</option>)}
            </select>
          </div>
        </div>

        {/* 경계 — 잡아끌어 좌우 폭 조정, 더블클릭으로 기본(1/4) 복귀 */}
        {wide && (
          <div
            onMouseDown={onSplitDown}
            onDoubleClick={resetSplit}
            title="드래그로 폭 조정 · 더블클릭으로 기본 폭"
            style={{
              flex: '0 0 5px', cursor: 'col-resize', background: 'var(--border)',
              // 잡기 쉬우라고 실제 폭보다 넓게 — 가운데 1px 만 선으로 보인다
              backgroundClip: 'content-box', borderLeft: '2px solid var(--bg)', borderRight: '2px solid var(--bg)',
            }}
          />
        )}

        {/* 우 — 선택 세션. 지표는 왼쪽 카드가 이미 보여주므로 여기는 발언·이벤트만 둔다. */}
        {selRow
          ? <SessionPane
              r={selRow} detail={selDetail} names={names} audio={audio} overlay={!wide}
              flowLoading={flowLoading}
              onFlow={() => openFlow(selRow)} onPlayAll={() => playAll(selRow)}
              onClose={() => setOpen(null)}
            />
          : wide && (
            <div style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center', color: 'var(--text-muted)', fontSize: 12.5 }}>
              왼쪽에서 세션을 고르면 발언·이벤트가 여기 나옵니다
            </div>
          )}
      </div>

      {audio.node}

      {player && (
        <div className="modal-overlay" onClick={() => setPlayer(null)}>
          <div className="modal-box" style={{ width: 800, maxWidth: 'calc(100vw - 40px)' }} onClick={e => e.stopPropagation()}>
            {player.title && <div style={{ fontWeight: 600, fontSize: 13, marginBottom: 8 }}>{player.title}</div>}
            <SegmentPlayer segments={player.segments} recordingId={player.id} callType="ptt"
                           onClose={() => setPlayer(null)} />
          </div>
        </div>
      )}

      {flow && (
        <FlowPage callId={flow.storeKey} date={flow.date} callType="ptt"
                  onClose={() => setFlow(null)}
                  prefetchedNodes={flow.nodes} prefetchedMessages={flow.messages} />
      )}
    </div>
  )
}

// ════════════════════════════════════════════════════════════════
// 목록 조각
// ════════════════════════════════════════════════════════════════
function SectionLabel({ label, n, live }: { label: string; n: number; live?: boolean }) {
  return (
    <div style={{
      display: 'flex', alignItems: 'center', gap: 5, padding: '4px 2px 1px',
      fontSize: 10.5, fontWeight: 700, letterSpacing: '.08em',
      textTransform: 'uppercase', color: 'var(--text-muted)',
    }}>
      {live && <span style={{ width: 6, height: 6, borderRadius: '50%', background: 'var(--success)' }} />}
      {label}
      <span style={{ fontWeight: 600, letterSpacing: 0, textTransform: 'none', opacity: .75 }}>{n}</span>
    </div>
  )
}

// ── 세션 요약 카드 (좌측 목록의 한 항목) ──────────────────────
// 표가 아니라 카드인 이유: 목록 폭이 화면의 1/4 이라 열을 늘어놓을 자리가 없고, 세션에서
// 먼저 읽어야 할 것은 열의 값이 아니라 "무엇을·언제·얼마나" 한 덩어리이기 때문이다.
// 카드는 선택돼도 내용·크기가 변하지 않는다 — 동작(Flow·재생)은 우측 패널 머리가 맡아
// 목록에서 선택을 옮겨도 카드가 자라며 아래 목록을 밀지 않는다.
function SessionCard({ r, sel, names, onSelect }: {
  r: PttSessionRow
  sel: boolean
  names: Directory
  onSelect: () => void
}) {
  // floor 축은 세션 당시 스냅샷이 정본 — private 은 통화마다 SDP 협상으로 달라진다.
  const duplex = r.floor_control === 'off'
  const live = r.state === 'active'
  const dur = r.end_time && r.start_time
    ? Math.max(0, Math.round((Date.parse(r.end_time) - Date.parse(r.start_time)) / 1000))
    : null
  const maxCon = r.max_concurrent ?? 0
  return (
    <div onClick={onSelect} style={{
      cursor: 'pointer', padding: '7px 9px', borderRadius: 8,
      border: `1px solid ${sel ? 'var(--primary)' : 'var(--border)'}`,
      background: sel ? 'var(--primary-soft)' : 'var(--surface)',
      display: 'flex', flexDirection: 'column', gap: 4,
    }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 5, flexWrap: 'wrap' }}>
        <span className={`badge ${KIND_BADGE[r.kind] || 'badge--gray'}`}>{KIND_LABEL[r.kind] || r.kind}</span>
        {duplex && <span className="badge badge--blue">전이중</span>}
        {/* 상태는 카드마다 명시 — 구역 라벨은 스크롤하면 시야에서 사라진다 */}
        <span className={`badge ${live ? 'badge--green' : 'badge--gray'}`}>{live ? '진행중' : '종료'}</span>
        <span style={{ fontWeight: 600, fontSize: 12.5, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
          <Target r={r} names={names} />
        </span>
      </div>

      <div className="ts" style={{ fontSize: 11.5 }}>
        {fmtShortTime(r.start_time)} ~ {live ? '진행중' : fmtShortTime(r.end_time)}
        {dur != null && <> · {fmtDur(dur)}</>}
        {r.kind === 'group' && r.mcptt_group_id && <> · {r.mcptt_group_id}</>}
      </div>

      <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap', fontSize: 11, color: 'var(--text-muted)' }}>
        <span>턴 <b style={{ color: 'var(--text)' }}>{r.turn_count ?? r.segment_count ?? 0}</b></span>
        <span>화자 <b style={{ color: 'var(--text)' }}>{r.speaker_count ?? ((r.people || []).length || 0)}</b></span>
        <span>발화 <b style={{ color: 'var(--text)' }}>{fmtSpeechMs(r.total_speech_ms)}</b></span>
        {maxCon > 1 && <span className="badge badge--blue" style={{ fontSize: 9 }}>동시 {maxCon}</span>}
        {r.initiator && <span style={{ marginLeft: 'auto' }}>개시 <Person id={r.initiator} names={names} /></span>}
      </div>
    </div>
  )
}

// ════════════════════════════════════════════════════════════════
// 선택 세션 — 머리(정체성·동작) + 참여자/발언/이벤트 3구획(경계 드래그로 높이 조절).
// 지표는 왼쪽 카드가 맡는다. 좁은 화면(<1080px)에서는 목록 위에 겹쳐 뜬다.
// ════════════════════════════════════════════════════════════════
const PANE_W = 560

function SessionPane({ r, detail, names, audio, overlay, flowLoading, onFlow, onPlayAll, onClose }: {
  r: PttSessionRow
  detail: DetailState | undefined
  names: Directory
  audio: InlineAudio
  overlay: boolean
  flowLoading: boolean
  onFlow: () => void
  onPlayAll: () => void
  onClose: () => void
}) {
  const duplex = r.floor_control === 'off'
  const live = r.state === 'active'

  const body = (
    <section
      onClick={e => e.stopPropagation()}
      style={{
        background: 'var(--bg-soft)', display: 'flex', flexDirection: 'column', minHeight: 0,
        ...(overlay
          ? {
              position: 'absolute', top: 0, right: 0, bottom: 0, width: `min(${PANE_W}px, 100%)`,
              zIndex: 21, boxShadow: 'var(--shadow-lg)', borderLeft: '1px solid var(--border)',
            }
          : { flex: 1 }),
      }}
    >
      {/* 머리 — 세션 정체성 + 동작. 동작을 카드가 아니라 여기에 두는 것은 카드 크기를
          선택 여부와 무관하게 고정하기 위해서다 (좁은 화면에선 카드가 가려지기도 한다). */}
      <div style={{
        display: 'flex', alignItems: 'center', gap: 6, padding: '8px 12px',
        borderBottom: '1px solid var(--border)', background: 'var(--surface)',
      }}>
        <span className={`badge ${KIND_BADGE[r.kind] || 'badge--gray'}`}>{KIND_LABEL[r.kind] || r.kind}</span>
        {duplex && <span className="badge badge--blue">전이중</span>}
        <span className={`badge ${live ? 'badge--green' : 'badge--gray'}`}>{live ? '진행중' : '종료'}</span>
        <span style={{ fontWeight: 600, fontSize: 12.5, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
          <Target r={r} names={names} />
        </span>
        {r.floor_control === 'on' && r.floor_policy && (
          <span className="badge badge--gray" style={{ fontSize: 9, flex: '0 0 auto' }}
                title="세션 당시 동시 발언 정책 (TS 24.380)">
            {r.floor_policy === 'multi' ? `multi · 최대 ${r.max_talkers || '?'}명`
              : r.floor_policy === 'dual' ? 'dual · 2명' : 'single'}
          </span>
        )}
        <span style={{ marginLeft: 'auto', display: 'inline-flex', gap: 4, flex: '0 0 auto' }}>
          <button className="btn btn--sm btn--outline" disabled={flowLoading} onClick={onFlow}>Flow</button>
          <button className="btn btn--sm btn--outline" onClick={onPlayAll}>&#9654; 전체</button>
          <button className="btn btn--sm btn--ghost" onClick={onClose} title="닫기 (Esc)">✕</button>
        </span>
      </div>

      {/* 본문 — 구획별 스크롤·높이 조절은 SessionDetail(panel) 이 맡는다 */}
      {!detail || detail.loading ? (
        <div className="empty" style={{ padding: 16 }}>상세 로딩 중...</div>
      ) : (
        <SessionDetail
          detail={detail} sess={r} recId={recIdOf(r.group_key, r.dir)}
          isDuplex={duplex} audio={audio} layout="panel"
        />
      )}
    </section>
  )

  if (!overlay) return body
  return (
    <>
      <div onClick={onClose}
           style={{ position: 'absolute', inset: 0, background: 'rgba(0,0,0,.35)', zIndex: 20 }} />
      {body}
    </>
  )
}

// 시간대 히트맵 — 세션 건수 기준 (hour 필터 이전 값이라 이동의 근거가 된다)
function HourHeatmap({ hours, sel, onPick }: {
  hours: Record<string, number>; sel: string; onPick: (h: string) => void
}) {
  const cells = Array.from({ length: 24 }, (_, h) => {
    const k = String(h).padStart(2, '0')
    return { h: k, v: hours[k] || 0 }
  })
  const max = Math.max(1, ...cells.map(c => c.v))
  return (
    <div>
      <div style={{ fontSize: 11.5, color: 'var(--text-muted)', marginBottom: 5 }}>
        시간대별 세션 <span style={{ opacity: .75 }}>· 색 진할수록 많음 · 클릭 → 그 시간대만</span>
      </div>
      <div style={{ display: 'flex', gap: 2 }}>
        {cells.map(c => {
          const ratio = c.v > 0 ? 0.18 + 0.82 * (c.v / max) : 0
          const on = sel === c.h
          return (
            <div key={c.h} onClick={() => c.v > 0 && onPick(c.h)} title={`${c.h}시 · ${c.v}건`}
                 style={{
                   flex: 1, cursor: c.v > 0 ? 'pointer' : 'default', textAlign: 'center',
                   borderRadius: 4, padding: on ? '2px 0' : '3px 0',
                   border: on ? '2px solid var(--primary)' : '1px solid var(--border)',
                   background: c.v > 0 ? `color-mix(in srgb, var(--primary) ${Math.round(ratio * 100)}%, var(--surface))` : 'var(--bg-soft)',
                   color: ratio > 0.55 ? '#fff' : 'var(--text)',
                 }}>
              <div style={{ fontSize: 11.5, fontWeight: 600, lineHeight: 1.3, height: 16, fontVariantNumeric: 'tabular-nums' }}>
                {c.v > 0 ? c.v : ' '}
              </div>
              <div style={{ fontSize: 9, lineHeight: 1.3, height: 12, opacity: .8, fontVariantNumeric: 'tabular-nums' }}>{c.h}</div>
            </div>
          )
        })}
      </div>
    </div>
  )
}

// ── 그룹 / 사람 필터 ────────────────────────────────────────────
function GroupFilter({ summaries, selected, open, onToggleMenu, onChange }: {
  summaries: Record<string, PttGroupSummary>
  selected: Set<string>
  open: boolean
  onToggleMenu: () => void
  onChange: (s: Set<string>) => void
}) {
  // 그룹 세션이 있는 키만 후보 — 1:1·임시는 그룹이 아니라 여기에 오지 않는다.
  const opts = Object.entries(summaries)
    .filter(([, s]) => (s.kind || 'group') === 'group')
    .sort((a, b) => (b[1].last_window || '').localeCompare(a[1].last_window || ''))
  return (
    <div style={{ position: 'relative' }} onClick={e => e.stopPropagation()}>
      <button className={`btn btn--sm ${selected.size ? 'btn--primary' : 'btn--outline'}`} onClick={onToggleMenu}>
        {selected.size ? `그룹 ${selected.size} ▾` : '그룹 ▾'}
      </button>
      {open && (
        <div style={{
          position: 'absolute', zIndex: 30, top: 'calc(100% + 4px)', left: 0, minWidth: 240, maxHeight: 320,
          overflowY: 'auto', padding: 6, background: 'var(--surface)', border: '1px solid var(--border)',
          borderRadius: 8, boxShadow: 'var(--shadow-lg)',
        }}>
          {opts.length === 0 && <div className="empty" style={{ padding: 12, fontSize: 12 }}>녹취가 있는 그룹이 없습니다</div>}
          {opts.map(([key, s]) => (
            <label key={key} style={{ display: 'flex', alignItems: 'center', gap: 7, padding: '5px 8px', borderRadius: 6, fontSize: 12.5, cursor: 'pointer' }}>
              <input type="checkbox" checked={selected.has(key)} onChange={() => {
                const n = new Set(selected)
                if (n.has(key)) n.delete(key); else n.add(key)
                onChange(n)
              }} />
              <span>{s.name || s.mcptt_group_id || key}</span>
              <span className="ts" style={{ color: 'var(--text-muted)' }}>{s.mcptt_group_id}</span>
            </label>
          ))}
          {selected.size > 0 && (
            <div style={{ padding: '4px 8px' }}>
              <button className="link-btn" style={{ fontSize: 12 }} onClick={() => onChange(new Set())}>전체 해제</button>
            </div>
          )}
        </div>
      )}
    </div>
  )
}

function PersonFilter({ value, candidates, names, open, onToggleMenu, onChange }: {
  value: string
  candidates: string[]
  names: Directory
  open: boolean
  onToggleMenu: () => void
  onChange: (v: string) => void
}) {
  const [input, setInput] = useState('')
  // 이름으로도 찾는다 — 목록은 이름으로 읽히는데 검색만 번호면 앞뒤가 맞지 않는다.
  // (서버 필터는 번호가 계약이라 고른 결과는 번호로 보낸다)
  const shown = useMemo(() => {
    const s = input.trim().toLowerCase()
    if (!s) return candidates
    return candidates.filter(p => p.toLowerCase().includes(s) || names.nameOf(p).toLowerCase().includes(s))
  }, [candidates, input, names])
  return (
    <div style={{ position: 'relative' }} onClick={e => e.stopPropagation()}>
      <button className={`btn btn--sm ${value ? 'btn--primary' : 'btn--outline'}`}
              title={value ? names.tipOf(value) : undefined}
              onClick={() => (value ? onChange('') : onToggleMenu())}>
        {value ? `사람 ${names.nameOf(value)} ✕` : '사람 ▾'}
      </button>
      {open && !value && (
        <div style={{
          position: 'absolute', zIndex: 30, top: 'calc(100% + 4px)', left: 0, minWidth: 230, maxHeight: 320,
          overflowY: 'auto', padding: 6, background: 'var(--surface)', border: '1px solid var(--border)',
          borderRadius: 8, boxShadow: 'var(--shadow-lg)',
        }}>
          <div style={{ padding: '2px 4px 6px' }}>
            <input className="form-input" autoFocus placeholder="이름·번호로 찾기 (Enter=번호 직접)"
                   value={input} onChange={e => setInput(e.target.value)}
                   onKeyDown={e => { if (e.key === 'Enter' && input.trim()) onChange(input.trim()) }} />
          </div>
          {/* 현재 목록에 등장한 참여자 — 발언하지 않은 참가자도 포함된다 */}
          {shown.slice(0, 40).map(p => (
            <div key={p} onClick={() => onChange(p)} title={names.tipOf(p)}
                 style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '5px 8px', borderRadius: 6, fontSize: 12.5, cursor: 'pointer' }}>
              <span>{names.nameOf(p)}</span>
              {names.person(p) && <span className="ts" style={{ color: 'var(--text-muted)', fontSize: 11 }}>{p}</span>}
            </div>
          ))}
          {shown.length === 0 && <div className="empty" style={{ padding: 10, fontSize: 12 }}>표시할 참여자가 없습니다</div>}
        </div>
      )}
    </div>
  )
}
