import { useState, useEffect, useCallback, useReducer, Fragment, type CSSProperties } from 'react'
import { useSearchParams, useNavigate } from 'react-router-dom'
import {
  statsApi, type Subscriber, type SubscribersResponse,
  type ServiceLive, type ServiceTrend, type Pool, type VolteCall, type PttGroup, type Anomaly,
  type ServiceEvent, type OrgStat, type PttMembersResponse, type TrendPoint, type TrendMetric,
} from '@core/api/stats'
import { useToast } from '@core/components/Toast'

// ── 공통 유틸 ─────────────────────────────────────────────
export function fmtDur(sec: number): string {
  if (sec < 0) sec = 0
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60
  const mm = String(m).padStart(2, '0'), ss = String(s).padStart(2, '0')
  return h > 0 ? `${h}:${mm}:${ss}` : `${m}:${ss}`
}
function elapsedSec(iso: string | null, now: number, fallback = 0): number {
  if (!iso) return fallback
  const t = Date.parse(iso)
  if (isNaN(t)) return fallback
  return Math.max(0, Math.floor((now - t) / 1000))
}
function poolColor(pct: number): string {
  if (pct >= 80) return 'var(--danger)'
  if (pct >= 60) return 'var(--warning)'
  return 'var(--success)'
}

export function useNowTick(periodMs = 1000): number {
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => { const iv = setInterval(() => setNow(Date.now()), periodMs); return () => clearInterval(iv) }, [periodMs])
  return now
}

// ── 공유 라이브 폴러 (위젯이 여러 개여도 /service/live·/trend 는 각 1회만 폴링) ──
function makeSharedPoll<T>(fetcher: () => Promise<T>, periodMs = 5000) {
  let state: T | null = null
  const subs = new Set<() => void>()
  let timer: ReturnType<typeof setInterval> | null = null
  const tick = async () => { try { state = await fetcher(); subs.forEach(f => f()) } catch { /* keep last */ } }
  return function useShared(): T | null {
    const [, force] = useReducer((x: number) => x + 1, 0)
    useEffect(() => {
      subs.add(force)
      if (!timer) { tick(); timer = setInterval(tick, periodMs) }
      return () => {
        subs.delete(force)
        if (subs.size === 0 && timer) { clearInterval(timer); timer = null }
      }
    }, [])
    return state
  }
}
const useServiceLive = makeSharedPoll<ServiceLive>(() => statsApi.serviceLive())

export function usePins(key: string): { pins: Set<string>; toggle: (id: string) => void } {
  const [pins, setPins] = useState<Set<string>>(() => {
    try { return new Set(JSON.parse(localStorage.getItem(key) || '[]')) } catch { return new Set() }
  })
  const toggle = (id: string) => setPins(prev => {
    const n = new Set(prev); if (n.has(id)) n.delete(id); else n.add(id)
    localStorage.setItem(key, JSON.stringify([...n])); return n
  })
  return { pins, toggle }
}

function OnlineDot({ on }: { on: boolean }) {
  return <span style={{ display: 'inline-block', width: 8, height: 8, borderRadius: '50%', background: on ? 'var(--success)' : 'var(--text-muted)', marginRight: 6 }} />
}
function PinBtn({ on, onClick }: { on: boolean; onClick: (e: React.MouseEvent) => void }) {
  return <button className="btn btn--sm btn--ghost" title={on ? '고정 해제' : '고정'} onClick={onClick} style={{ padding: '0 6px', opacity: on ? 1 : 0.35 }}>📌</button>
}
function Gauge({ label, pool }: { label: string; pool: Pool }) {
  const total = pool.total || 0, used = pool.used || 0
  const pct = total > 0 ? Math.round((used / total) * 100) : 0
  return (
    <div style={{ minWidth: 200 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: 12, color: 'var(--text-muted)', marginBottom: 3 }}>
        <span>{label}</span><span>{used} / {total} ({pct}%)</span>
      </div>
      <div style={{ height: 8, borderRadius: 4, background: 'var(--border)', overflow: 'hidden' }}>
        <div style={{ width: `${Math.min(100, pct)}%`, height: '100%', background: poolColor(pct), transition: 'width .3s' }} />
      </div>
    </div>
  )
}
function Kpi({ label, value, sub }: { label: string; value: React.ReactNode; sub?: string }) {
  return (
    <div style={{ display: 'flex', flexDirection: 'column', minWidth: 80 }}>
      <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>{label}</span>
      <span style={{ fontSize: 20, fontWeight: 700, lineHeight: 1.2 }}>{value}</span>
      {sub && <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>{sub}</span>}
    </div>
  )
}
function Loading() { return <div className="empty">로딩 중...</div> }

// ── 위젯: VoLTE 요약 ──────────────────────────────────────
export function VolteKpiCard() {
  const live = useServiceLive()
  const v = live?.volte.kpi
  return (
    <div className="panel" style={{ padding: 14 }}>
      <div style={{ display: 'flex', flexWrap: 'wrap', gap: 22, alignItems: 'center' }}>
        <span className="badge badge--blue" style={{ alignSelf: 'flex-start' }}>VoLTE</span>
        <Kpi label="통화 중" value={v?.active ?? '-'} />
        <Kpi label="호출 중" value={v?.ringing ?? '-'} />
        <Kpi label="평균 통화" value={v ? fmtDur(v.avg_duration_sec) : '-'} />
        <Kpi label="등록" value={v?.registered ?? '-'} sub={v ? `/ ${v.numbers}` : ''} />
        {live && <Gauge label="RTP 풀" pool={live.capacity.volte_rtp} />}
      </div>
    </div>
  )
}

// ── 위젯: PTT 요약 + 미디어 노드 분산 ─────────────────────
export function PttKpiCard() {
  const live = useServiceLive()
  const p = live?.ptt.kpi
  return (
    <div className="panel" style={{ padding: 14 }}>
      <div style={{ display: 'flex', flexWrap: 'wrap', gap: 22, alignItems: 'center' }}>
        <span className="badge badge--green" style={{ alignSelf: 'flex-start' }}>PTT</span>
        <Kpi label="발언 중" value={p?.talking ?? '-'} sub="그룹" />
        <Kpi label="최근 5분 발언" value={p?.recent_active ?? '-'} sub="그룹" />
        <Kpi label="전체 그룹" value={p?.total_groups ?? '-'} />
        <Kpi label="참여(세션)" value={p?.participants ?? '-'} sub="명" />
        <Kpi label="등록" value={p?.registered ?? '-'} sub={p ? `/ ${p.numbers}` : ''} />
        {live && <Gauge label="PTT 그룹 풀(동시 그룹·floor)" pool={live.capacity.ptt_rtp} />}
      </div>
      {live && live.capacity.nodes.length > 1 && (
        <div style={{ borderTop: '1px dashed var(--border)', marginTop: 10, paddingTop: 8, display: 'flex', flexWrap: 'wrap', gap: 18, fontSize: 12, color: 'var(--text-muted)' }}>
          <span>미디어 노드 분산:</span>
          {live.capacity.nodes.map(n => (
            <span key={n.host}>
              <span style={{ display: 'inline-block', width: 7, height: 7, borderRadius: '50%', background: n.up ? 'var(--success)' : 'var(--danger)', marginRight: 4 }} />
              {n.host} · VoLTE {n.volte_rtp.used}/{n.volte_rtp.total} · PTT {n.ptt_rtp.used}/{n.ptt_rtp.total} · 그룹 {n.groups}
            </span>
          ))}
        </div>
      )}
    </div>
  )
}

// ── 위젯: 사용량 추세 (윈도우 선택 + 24구간 + 다지표) ─────
const TREND_WINS: { k: string; label: string }[] = [
  { k: '2h', label: '2시간' }, { k: '4h', label: '4시간' }, { k: '8h', label: '8시간' },
  { k: '16h', label: '16시간' }, { k: '24h', label: '24시간' },
]
const TREND_SERIES: { key: TrendMetric; label: string; rgb: string }[] = [
  { key: 'volte_active', label: 'VoLTE 동시통화', rgb: '37,99,235' },
  { key: 'volte_calls', label: 'VoLTE 발생 호', rgb: '59,130,246' },
  { key: 'ptt_grants', label: 'PTT 발언 수', rgb: '22,163,74' },
  { key: 'ptt_speakers', label: 'PTT 발언자', rgb: '5,150,105' },
  { key: 'ptt_groups', label: 'PTT 활성그룹', rgb: '217,142,0' },
]
function clockOf(t: number) { return new Date(t * 1000).toLocaleString('ko-KR', { month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit' }) }
function bucketLabel(sec: number) { return sec >= 3600 ? `${Math.round(sec / 3600)}시간 간격` : `${Math.round(sec / 60)}분 간격` }
const AXIS_W = 92   // 좌측 지표 라벨 폭 — 히트맵 셀 영역과 시간축 정렬용
function HeatRow({ label, points, metric, rgb }: { label: string; points: TrendPoint[]; metric: TrendMetric; rgb: string }) {
  const max = Math.max(1, ...points.map(p => p[metric]))
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 3 }}>
      <span style={{ width: AXIS_W, fontSize: 12, color: 'var(--text-muted)', textAlign: 'right', flexShrink: 0 }}>{label}</span>
      <div style={{ display: 'flex', gap: 1, flex: 1 }}>
        {points.map((p, i) => {
          const v = p[metric]
          const ratio = v > 0 ? 0.18 + 0.82 * (v / max) : 0
          return (
            <div key={i} title={`${clockOf(p.t)} · ${v}`}
              style={{
                flex: 1, height: 20, display: 'flex', alignItems: 'center', justifyContent: 'center',
                fontSize: 10, lineHeight: 1, borderRadius: 2,
                background: v > 0 ? `rgba(${rgb},${ratio.toFixed(3)})` : 'var(--border)',
                color: ratio > 0.55 ? '#fff' : 'var(--text)',
              }}>
              {v > 0 ? v : ''}
            </div>
          )
        })}
      </div>
    </div>
  )
}
// 히트맵 하단 시간축 — 셀 경계마다 시각 눈금. 첫 눈금 또는 날짜가 바뀌는 눈금엔 날짜도 표시.
function TrendAxis({ points }: { points: TrendPoint[] }) {
  const n = points.length
  if (n === 0) return null
  const dayKey = (t: number) => { const d = new Date(t * 1000); return `${d.getFullYear()}-${d.getMonth()}-${d.getDate()}` }
  const md = (t: number) => { const d = new Date(t * 1000); return `${d.getMonth() + 1}/${d.getDate()}` }
  const hm = (t: number) => new Date(t * 1000).toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', hour12: false })
  const step = n <= 12 ? 2 : 4   // 약 6~7눈금
  const ticks: number[] = []
  for (let i = 0; i < n; i += step) ticks.push(i)
  if (ticks[ticks.length - 1] !== n - 1) ticks.push(n - 1)
  return (
    <div style={{ display: 'flex', gap: 8, marginTop: 2 }}>
      <span style={{ width: AXIS_W, flexShrink: 0 }} />
      <div style={{ position: 'relative', flex: 1, height: 14 }}>
        {ticks.map((i, idx) => {
          const t = points[i].t
          const prevT = idx > 0 ? points[ticks[idx - 1]].t : null
          const showDate = prevT === null || dayKey(prevT) !== dayKey(t)   // 첫 눈금 또는 날짜 변경
          const pct = (i + 0.5) / n * 100
          const isFirst = i === 0, isLast = i === n - 1
          const style: CSSProperties = {
            position: 'absolute', top: 0, fontSize: 10, color: 'var(--text-muted)', whiteSpace: 'nowrap',
          }
          if (isFirst) style.left = 0
          else if (isLast) style.right = 0
          else { style.left = `${pct}%`; style.transform = 'translateX(-50%)' }
          return (
            <span key={i} style={style}>
              {showDate && <span style={{ color: 'var(--text)', fontWeight: 600, marginRight: 3 }}>{md(t)}</span>}
              {hm(t)}
            </span>
          )
        })}
      </div>
    </div>
  )
}
export function TrendCard() {
  const { show } = useToast()
  const [win, setWin] = useState('8h')
  const [data, setData] = useState<ServiceTrend | null>(null)
  useEffect(() => {
    let alive = true
    const load = () => statsApi.serviceTrend(win).then(d => { if (alive) setData(d) }).catch(e => show(String(e), 'err'))
    load(); const iv = setInterval(load, 15000); return () => { alive = false; clearInterval(iv) }
  }, [win, show])
  const points = data?.points ?? []
  return (
    <div className="panel" style={{ padding: '10px 14px' }}>
      <div className="toolbar" style={{ marginBottom: 8 }}>
        <span style={{ fontSize: 13, fontWeight: 600 }}>사용량 추세</span>
        <span style={{ fontSize: 12, color: 'var(--text-muted)', marginLeft: 4 }}>최근</span>
        {TREND_WINS.map(w => (
          <button key={w.k} className={`btn btn--sm ${win === w.k ? 'btn--primary' : 'btn--ghost'}`} onClick={() => setWin(w.k)}>{w.label}</button>
        ))}
        <span style={{ marginLeft: 'auto', fontSize: 11, color: 'var(--text-muted)' }}>
          {points.length ? `${clockOf(points[0].t)} ~ ${clockOf(points[points.length - 1].t)} · ${points.length}구간 (${bucketLabel(data?.bucket_sec ?? 0)})` : ''}
        </span>
      </div>
      {!data ? <Loading /> : <>
        {TREND_SERIES.map(s => (
          <HeatRow key={s.key} label={s.label} points={points} metric={s.key} rgb={s.rgb} />
        ))}
        <TrendAxis points={points} />
      </>}
    </div>
  )
}

// ── 위젯: 이상 징후 ───────────────────────────────────────
export function AnomalyCard() {
  const live = useServiceLive()
  const anomalies: Anomaly[] = live?.anomalies ?? []
  return (
    <div className="panel" style={{ padding: '10px 14px', borderLeft: `3px solid ${anomalies.length ? 'var(--danger)' : 'var(--success)'}` }}>
      <div style={{ fontSize: 13, fontWeight: 600, marginBottom: anomalies.length ? 6 : 0, color: anomalies.length ? 'var(--danger)' : 'var(--success)' }}>
        {anomalies.length ? `⚠ 이상 징후 (${anomalies.length})` : '✓ 이상 징후 없음'}
      </div>
      {anomalies.map((a, i) => (
        <div key={i} style={{ display: 'flex', alignItems: 'center', gap: 8, fontSize: 13, marginBottom: 3 }}>
          <span className="badge badge--red">{a.kind === 'volte' ? 'VoLTE' : 'PTT'}</span>
          <span>{a.detail}</span>
          <span style={{ color: 'var(--text-muted)' }}>{a.label}</span>
        </div>
      ))}
    </div>
  )
}

// ── 위젯: VoLTE 활성 호 ───────────────────────────────────
export function VolteCallsCard() {
  const live = useServiceLive()
  const navigate = useNavigate()
  const now = useNowTick()
  const { pins, toggle } = usePins('svc.pins.volte')
  const calls = live?.volte.calls ?? []
  if (!live) return <div className="panel"><Loading /></div>
  if (calls.length === 0) return <div className="empty">현재 통화 중인 호가 없습니다</div>
  const sorted = [...calls].sort((a, b) => (pins.has(b.call_id) ? 1 : 0) - (pins.has(a.call_id) ? 1 : 0))
  return (
    <div className="panel">
      <table className="data-table">
        <thead><tr><th></th><th>상태</th><th>발신 → 착신</th><th>유형</th><th>경과</th><th>미디어 노드</th><th>호 ID</th><th></th></tr></thead>
        <tbody>
          {sorted.map((c: VolteCall) => {
            const ring = c.state === 'ringing', warn = c.anomalies.length > 0, pinned = pins.has(c.call_id)
            return (
              <tr key={c.call_id} style={pinned ? { background: 'rgba(80,120,255,.08)' } : warn ? { background: 'rgba(220,50,50,.06)' } : undefined}>
                <td><PinBtn on={pinned} onClick={e => { e.stopPropagation(); toggle(c.call_id) }} /></td>
                <td><span className={`badge ${ring ? 'badge--blue' : 'badge--green'}`}>{ring ? '호출 중' : '통화 중'}</span>{warn && <span title={c.anomalies.map(a => a.detail).join(', ')}> ⚠</span>}</td>
                <td><b>{c.caller || '-'}</b> <span style={{ color: 'var(--text-muted)' }}>→</span> {c.callee || '-'}</td>
                <td>{c.video ? '영상' : '음성'}</td>
                <td className="ts">{fmtDur(elapsedSec(c.invite_time, now, c.duration_sec))}</td>
                <td className="ts">{c.media_node || '-'}</td>
                <td className="ts" title={c.call_id} style={{ maxWidth: 140, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{c.call_id}</td>
                <td><button className="btn btn--sm btn--ghost" onClick={() => navigate('/service/history/volte')}>이력 ▸</button></td>
              </tr>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}

// ── 위젯: PTT 활성 그룹 ───────────────────────────────────
// 멤버 drill 패널 (그룹당 100~200명 → 페이지네이션)
const MEMBER_LIMIT = 30
function MemberDrill({ group }: { group: string }) {
  const { show } = useToast()
  const [data, setData] = useState<PttMembersResponse | null>(null)
  const [page, setPage] = useState(1)
  useEffect(() => {
    let live = true
    statsApi.pttMembers(group, page, MEMBER_LIMIT).then(d => { if (live) setData(d) }).catch(e => show(String(e), 'err'))
    return () => { live = false }
  }, [group, page, show])
  if (!data) return <div className="ts" style={{ padding: 8 }}>멤버 로딩...</div>
  const pages = Math.max(1, Math.ceil(data.total / MEMBER_LIMIT))
  return (
    <div style={{ padding: '6px 4px' }}>
      <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 4 }}>
        멤버 {data.total}명 · 현재 참여 {data.active_count}명 · {page}/{pages} 페이지
      </div>
      <div style={{ display: 'flex', flexWrap: 'wrap', gap: '2px 14px' }}>
        {data.members.map(m => (
          <span key={m.msisdn} style={{ fontSize: 12, minWidth: 200, color: m.active ? 'var(--text)' : 'var(--text-muted)' }}>
            {m.talking ? '🎤 ' : m.active ? '🟢 ' : '· '}{m.name || m.msisdn}
            {m.role !== 'participant' && m.role !== 'member' && <span className="ts"> ({m.role})</span>}
          </span>
        ))}
      </div>
      {pages > 1 && (
        <div style={{ marginTop: 6 }}>
          <button className="btn btn--sm btn--ghost" disabled={page <= 1} onClick={e => { e.stopPropagation(); setPage(p => p - 1) }}>이전</button>
          <button className="btn btn--sm btn--ghost" disabled={page >= pages} onClick={e => { e.stopPropagation(); setPage(p => p + 1) }}>다음</button>
        </div>
      )}
    </div>
  )
}

export function PttGroupsCard() {
  const live = useServiceLive()
  const navigate = useNavigate()
  useNowTick()   // 주기적 리렌더 구독(반환값 미사용)
  const { pins, toggle } = usePins('svc.pins.ptt')
  const [open, setOpen] = useState<string | null>(null)   // 멤버 drill 열린 그룹(1개)
  const groups = live?.ptt.groups ?? []   // 백엔드가 발언 활동순으로 정렬
  if (!live) return <div className="panel"><Loading /></div>
  if (groups.length === 0) return <div className="empty">현재 발언 중이거나 최근 활동한 그룹이 없습니다</div>
  const typeLabel = (t: string) => t || '-'
  const sorted = [...groups].sort((a, b) => (pins.has(b.group_id) ? 1 : 0) - (pins.has(a.group_id) ? 1 : 0))
  return (
    <div className="panel">
      <table className="data-table">
        <thead><tr><th></th><th>그룹</th><th>유형</th><th>참여</th><th>현재 화자</th><th>최근 발언</th><th>발언수(5m)</th><th></th></tr></thead>
        <tbody>
          {sorted.map((g: PttGroup) => {
            const isOpen = open === g.group_id, warn = g.anomalies.length > 0, pinned = pins.has(g.group_id)
            return (
              <Fragment key={g.group_id}>
                <tr style={{ cursor: 'pointer', ...(pinned ? { background: 'rgba(80,120,255,.08)' } : warn ? { background: 'rgba(220,50,50,.06)' } : {}) }} onClick={() => setOpen(isOpen ? null : g.group_id)}>
                  <td><PinBtn on={pinned} onClick={e => { e.stopPropagation(); toggle(g.group_id) }} /></td>
                  <td><span style={{ color: 'var(--text-muted)' }}>{isOpen ? '▾' : '▸'}</span> <b>{g.name}</b> <span className="ts">{g.group_id !== g.name ? `(${g.group_id})` : ''}</span></td>
                  <td><span className="badge">{typeLabel(g.type)}</span></td>
                  <td className="ts">{g.active_members} / {g.total_members}</td>
                  <td>{g.floor_holder ? <span style={{ color: 'var(--primary)', fontWeight: 600 }}>🎤 {g.floor_holder}</span> : <span className="ts">(없음)</span>}{warn && <span title={g.anomalies.map(a => a.detail).join(', ')}> ⚠</span>}</td>
                  <td className="ts">{g.last_floor ? new Date(g.last_floor).toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', second: '2-digit' }) : '-'}</td>
                  <td className="ts">{g.floor_count ?? 0}</td>
                  <td><button className="btn btn--sm btn--ghost" onClick={e => { e.stopPropagation(); navigate('/service/history/ptt') }}>이력 ▸</button></td>
                </tr>
                {isOpen && (
                  <tr>
                    <td colSpan={8} style={{ background: 'var(--hover)' }}>
                      <MemberDrill group={g.group_id} />
                      {g.floor_held_sec !== undefined && <span style={{ margin: '0 8px', color: 'var(--danger)', fontSize: 12 }}>⚠ floor {fmtDur(g.floor_held_sec)} 점유</span>}
                    </td>
                  </tr>
                )}
              </Fragment>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}

// ── 위젯: 라이브 이벤트 ───────────────────────────────────
const EV_ICON: Record<string, string> = {
  call_start: '📞', call_end: '📵', floor_grant: '🎤', floor_release: '🔇',
  floor_reject: '⛔', member_join: '➕', member_leave: '➖',
}
export function EventFeedCard() {
  const { show } = useToast()
  const [events, setEvents] = useState<ServiceEvent[]>([])
  const [loading, setLoading] = useState(true)
  const load = useCallback(async () => {
    try { setEvents((await statsApi.serviceEvents(80)).events) }
    catch (e: unknown) { show(String(e), 'err') } finally { setLoading(false) }
  }, [show])
  useEffect(() => { load(); const iv = setInterval(load, 5000); return () => clearInterval(iv) }, [load])
  if (loading) return <div className="panel"><Loading /></div>
  if (events.length === 0) return <div className="empty">최근 이벤트가 없습니다</div>
  return (
    <div className="panel">
      <table className="data-table">
        <thead><tr><th style={{ width: 96 }}>시각</th><th style={{ width: 70 }}>구분</th><th>이벤트</th></tr></thead>
        <tbody>
          {events.map((e, i) => (
            <tr key={i}>
              <td className="ts">{new Date(e.ts).toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })}</td>
              <td><span className={`badge ${e.kind === 'volte' ? 'badge--blue' : 'badge--green'}`}>{e.kind === 'volte' ? 'VoLTE' : 'PTT'}</span></td>
              <td>{EV_ICON[e.type] || '•'} {e.detail}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

// ── 위젯: 조직별 집계 ─────────────────────────────────────
// 구성원 로스터 표 (가입자 상태) — 부서 선택/검색 결과 공용
function SubscriberRows({ subs }: { subs: Subscriber[] }) {
  return (
    <table className="data-table">
      <thead><tr><th>이름</th><th>부서</th><th>VoLTE 번호</th><th>VoLTE</th><th>VoLTE 통화</th><th>PTT 번호</th><th>PTT</th><th>PTT 서비스</th></tr></thead>
      <tbody>
        {subs.map(s => (
          <tr key={s.person_id}>
            <td style={{ fontWeight: 600 }}>{s.name}</td>
            <td className="ts" style={{ whiteSpace: 'nowrap' }}>{s.org_path || '-'}</td>
            <td className="ts">{s.volte?.msisdn || '-'}</td>
            <td>{s.volte ? <><OnlineDot on={s.volte.online} />{s.volte.online ? '접속' : '미접속'}</> : <span className="ts">-</span>}</td>
            <td>{s.volte?.calls && s.volte.calls.length > 0
              ? s.volte.calls.map((c, i) => <span key={i} className={`badge ${c.state === 'active' ? 'badge--green' : 'badge--blue'}`} style={{ marginRight: 4 }}>{c.state === 'active' ? '통화' : '호출'} {c.role === 'caller' ? '→' : '←'} {c.peer}</span>)
              : <span className="ts">{s.volte?.online ? '대기' : '-'}</span>}</td>
            <td className="ts">{s.ptt?.msisdn || '-'}</td>
            <td>{s.ptt ? <><OnlineDot on={s.ptt.online} />{s.ptt.online ? '접속' : '미접속'}</> : <span className="ts">-</span>}</td>
            <td>{s.ptt?.groups && s.ptt.groups.length > 0
              ? s.ptt.groups.map((g, i) => <span key={i} className="badge badge--green" style={{ marginRight: 4 }}>🎤 {g.group_id}</span>)
              : <span className="ts">{s.ptt?.online ? '대기' : '-'}</span>}</td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}

// 조직별(부서) — 상단 검색 + 좌측 부서 트리 + 우측 구성원 로스터
const PAGE_SIZES = [5, 10, 20, 50, 100]
export function OrgStatsCard() {
  const { show } = useToast()
  const [orgs, setOrgs] = useState<OrgStat[]>([])
  const [dbDegraded, setDbDegraded] = useState(false) // DB 집계 실패 강등 (구성원/등록 수 0)
  const [sel, setSel] = useState<string>('')          // 선택 부서 코드
  const [searchInput, setSearchInput] = useState('')
  const [q, setQ] = useState('')
  const [page, setPage] = useState(1)
  const [limit, setLimit] = useState(20)   // 기본 20명, 5/10/20/50/100 선택
  const [roster, setRoster] = useState<SubscribersResponse | null>(null)

  // 부서 트리 (10초)
  useEffect(() => {
    let alive = true
    const load = () => statsApi.serviceOrg().then(r => {
      if (!alive) return
      setOrgs(r.orgs)
      setDbDegraded(!!r.db_degraded)
      setSel(s => s || r.orgs[0]?.code || '')   // 기본=최상위
    }).catch(e => show(String(e), 'err'))
    load(); const iv = setInterval(load, 10000); return () => { alive = false; clearInterval(iv) }
  }, [show])

  // 검색 디바운스
  useEffect(() => { const t = setTimeout(() => { setQ(searchInput.trim()); setPage(1) }, 350); return () => clearTimeout(t) }, [searchInput])

  // 로스터: 선택 부서(org) AND 검색어(q) 조합. 둘 다 비면 skip.
  useEffect(() => {
    if (!q && !sel) return
    let alive = true
    const load = () => statsApi.subscribers({ org: sel || undefined, q: q || undefined, status: 'all', page, limit })
      .then(r => { if (alive) setRoster(r) }).catch(e => show(String(e), 'err'))
    load(); const iv = setInterval(load, 5000); return () => { alive = false; clearInterval(iv) }
  }, [q, sel, page, limit, show])

  const selNode = orgs.find(o => o.code === sel)
  const total = roster?.total ?? 0
  const totalPages = Math.max(1, Math.ceil(total / limit))

  return (
    <div className="panel" style={{ padding: 10 }}>
      {dbDegraded && (
        <div style={{ marginBottom: 8, padding: '6px 10px', borderRadius: 4, fontSize: 12,
          background: 'var(--warn-soft)', color: 'var(--warning)', border: '1px solid var(--border)' }}>
          DB 조회 실패 — 구성원/등록 수는 표시되지 않습니다 (활성 세션·발언자는 정상). 상세는 OAM 로그 참조.
        </div>
      )}
      <div className="toolbar" style={{ marginBottom: 8 }}>
        <input className="search-input" placeholder="가입자 이름/번호 검색 (전체)" value={searchInput}
          onChange={e => setSearchInput(e.target.value)} style={{ maxWidth: 280 }} />
        {q && <button className="btn btn--sm btn--ghost" onClick={() => setSearchInput('')}>검색 해제</button>}
        <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>
          {selNode ? `부서: ${selNode.name} (${selNode.members}명)` : ''}{selNode && q ? '  &  ' : ''}{q ? `검색: "${q}"` : ''}
        </span>
        <label style={{ marginLeft: 'auto', fontSize: 12, color: 'var(--text-muted)' }}>
          표시{' '}
          <select className="form-input" value={limit} onChange={e => { setLimit(Number(e.target.value)); setPage(1) }}
            style={{ width: 'auto', padding: '2px 6px', display: 'inline-block' }}>
            {PAGE_SIZES.map(n => <option key={n} value={n}>{n}명</option>)}
          </select>
        </label>
      </div>
      <div style={{ display: 'flex', gap: 10, alignItems: 'flex-start' }}>
        {/* 부서 트리 */}
        <div style={{ flex: '0 0 230px', maxHeight: 520, overflow: 'auto', borderRight: '1px solid var(--border)', paddingRight: 6 }}>
          {orgs.length === 0 ? <Loading /> : orgs.map(o => (
            <div key={o.code} onClick={() => { setSel(o.code); setPage(1) }}
              style={{ cursor: 'pointer', padding: '4px 6px', paddingLeft: 6 + o.depth * 16, borderRadius: 4, fontSize: 13,
                background: sel === o.code ? 'rgba(80,120,255,.12)' : undefined,
                fontWeight: o.depth === 0 ? 700 : o.depth === 1 ? 600 : 400 }}>
              {o.name} <span className="ts">({o.members})</span>
              {o.active_volte > 0 && <span className="badge badge--blue" style={{ marginLeft: 4 }}>📞{o.active_volte}</span>}
              {o.ptt_talking > 0 && <span className="badge badge--green" style={{ marginLeft: 4 }}>🎤{o.ptt_talking}</span>}
            </div>
          ))}
        </div>
        {/* 구성원 로스터 — 헤더(고정)·본문(스크롤)·페이지(고정) */}
        <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column', maxHeight: 520 }}>
          <div className="scroll-fill">
            {!roster ? <Loading />
              : roster.subscribers.length === 0 ? <div className="empty">{q ? '검색 결과 없음' : '구성원 없음'}</div>
              : <SubscriberRows subs={roster.subscribers} />}
          </div>
          {roster && roster.subscribers.length > 0 && (
            <div className="toolbar" style={{ justifyContent: 'flex-end', borderTop: '1px solid var(--border)', flexShrink: 0 }}>
              <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>총 {total.toLocaleString()}명 · {page}/{totalPages}</span>
              <button className="btn btn--sm btn--ghost" disabled={page <= 1} onClick={() => setPage(p => Math.max(1, p - 1))}>이전</button>
              <button className="btn btn--sm btn--ghost" disabled={page >= totalPages} onClick={() => setPage(p => Math.min(totalPages, p + 1))}>다음</button>
            </div>
          )}
        </div>
      </div>
    </div>
  )
}

// ── 위젯: 가입자 조회 (서버사이드 active/online/all + 검색 + 페이지) ──
const LOOKUP_LIMIT = 50
export function SubscriberLookup() {
  const { show } = useToast()
  const [searchParams, setSearchParams] = useSearchParams()
  const initialQ = searchParams.get('q') || ''
  const [status, setStatus] = useState<'active' | 'online' | 'all'>(initialQ ? 'all' : 'active')
  const [searchInput, setSearchInput] = useState(initialQ)
  const [q, setQ] = useState(initialQ)
  const [page, setPage] = useState(1)
  const [data, setData] = useState<SubscribersResponse | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    const t = setTimeout(() => {
      setQ(searchInput.trim()); setPage(1)
      if (searchInput.trim()) setSearchParams({ q: searchInput.trim() }, { replace: true })
      else { searchParams.delete('q'); setSearchParams(searchParams, { replace: true }) }
    }, 350)
    return () => clearTimeout(t)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [searchInput])

  const load = useCallback(async (spin: boolean) => {
    if (spin) setLoading(true)
    try { setData(await statsApi.subscribers({ status, q, page, limit: LOOKUP_LIMIT })) }
    catch (e: unknown) { show(String(e), 'err') } finally { setLoading(false) }
  }, [status, q, page, show])
  useEffect(() => { load(true); const iv = setInterval(() => load(false), 5000); return () => clearInterval(iv) }, [load])

  const counts = data?.counts ?? { all: 0, online: 0, active: 0 }
  const subs: Subscriber[] = data?.subscribers ?? []
  const total = data?.total ?? 0
  const totalPages = Math.max(1, Math.ceil(total / LOOKUP_LIMIT))
  const tabBtn = (s: 'active' | 'online' | 'all', label: string, n: number) => (
    <button className={`btn btn--sm ${status === s ? 'btn--primary' : 'btn--ghost'}`} onClick={() => { setStatus(s); setPage(1) }}>{label} ({n})</button>
  )
  return (
    <div>
      <div className="toolbar">
        {tabBtn('active', '이용 중', counts.active)}
        {tabBtn('online', '접속 중', counts.online)}
        {tabBtn('all', '전체', counts.all)}
        <input className="search-input" placeholder="이름/번호 검색" value={searchInput} onChange={e => setSearchInput(e.target.value)} style={{ maxWidth: 200 }} />
      </div>
      {loading ? <Loading />
        : subs.length === 0 ? <div className="empty">{status === 'active' ? '이용 중인 가입자가 없습니다' : q ? '검색 결과가 없습니다' : '가입자가 없습니다'}</div>
        : (
          <div className="panel">
            <table className="data-table">
              <thead><tr><th>이름</th><th>VoLTE 번호</th><th>VoLTE 접속</th><th>VoLTE 통화</th><th>PTT 번호</th><th>PTT 접속</th><th>PTT 서비스</th></tr></thead>
              <tbody>
                {subs.map(s => (
                  <tr key={s.person_id}>
                    <td style={{ fontWeight: 600 }}>{s.name}</td>
                    <td className="ts">{s.volte?.msisdn || '-'}</td>
                    <td>{s.volte ? <><OnlineDot on={s.volte.online} />{s.volte.online ? '접속' : '미접속'}</> : <span className="ts">-</span>}</td>
                    <td>{s.volte?.calls && s.volte.calls.length > 0
                      ? s.volte.calls.map((c, i) => <span key={i} className={`badge ${c.state === 'active' ? 'badge--green' : 'badge--blue'}`} style={{ marginRight: 4 }}>{c.state === 'active' ? '통화 중' : '호출 중'} {c.role === 'caller' ? '→' : '←'} {c.peer}</span>)
                      : <span className="ts">{s.volte?.online ? '대기' : '-'}</span>}</td>
                    <td className="ts">{s.ptt?.msisdn || '-'}</td>
                    <td>{s.ptt ? <><OnlineDot on={s.ptt.online} />{s.ptt.online ? '접속' : '미접속'}</> : <span className="ts">-</span>}</td>
                    <td>{s.ptt?.groups && s.ptt.groups.length > 0
                      ? s.ptt.groups.map((g, i) => <span key={i} className="badge badge--green" style={{ marginRight: 4 }}>참여 그룹 {g.group_id} ({g.active_members}/{g.total_members})</span>)
                      : <span className="ts">{s.ptt?.online ? '대기' : '-'}</span>}</td>
                  </tr>
                ))}
              </tbody>
            </table>
            {totalPages > 1 && (
              <div className="toolbar" style={{ justifyContent: 'flex-end', borderTop: '1px solid var(--border)' }}>
                <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>총 {total.toLocaleString()}건 · {page}/{totalPages}</span>
                <button className="btn btn--sm btn--ghost" disabled={page <= 1} onClick={() => setPage(p => Math.max(1, p - 1))}>이전</button>
                <button className="btn btn--sm btn--ghost" disabled={page >= totalPages} onClick={() => setPage(p => Math.min(totalPages, p + 1))}>다음</button>
              </div>
            )}
          </div>
        )}
    </div>
  )
}

// ── 위젯: 서비스 상세 (탭 통합 — 호·그룹·이벤트·조직·조회) ──
// 루트에 .widget-stack — 위젯으로 배치됐을 때 칸을 채운다(없으면 내용 높이만 차지).
export function ServiceDetailTabs() {
  const live = useServiceLive()
  const [tab, setTab] = useState<'events' | 'org' | 'volte' | 'ptt'>('org')
  const v = live?.volte.kpi
  const p = live?.ptt.kpi
  const tb = (t: typeof tab, label: string, n?: number) => (
    <button className={`btn btn--sm ${tab === t ? 'btn--primary' : 'btn--ghost'}`} onClick={() => setTab(t)}>
      {label}{n !== undefined && n !== null ? ` (${n})` : ''}
    </button>
  )
  return (
    <div className="widget-stack">
      <div className="toolbar" style={{ flexWrap: 'wrap' }}>
        {tb('events', '라이브 이벤트')}
        {tb('org', '부서별')}
        {tb('volte', 'VoLTE 호', v?.active)}
        {tb('ptt', 'PTT 그룹', p?.recent_active)}
        <span style={{ marginLeft: 'auto', color: 'var(--text-muted)', fontSize: 12 }}>5초 자동 갱신{live?.ts ? ` · ${new Date(live.ts).toLocaleTimeString('ko-KR')}` : ''}</span>
      </div>
      {tab === 'events' && <EventFeedCard />}
      {tab === 'org' && <OrgStatsCard />}
      {tab === 'volte' && <VolteCallsCard />}
      {tab === 'ptt' && <PttGroupsCard />}
    </div>
  )
}

// ── 기본 페이지: 위젯을 합성(섹션 헤더 + 카드) ────────────
export default function ServiceStatusPage() {
  return (
    <div>
      <div style={{ display: 'flex', gap: 10, flexWrap: 'wrap', marginBottom: 14 }}>
        <div style={{ flex: '1 1 460px' }}><VolteKpiCard /></div>
        <div style={{ flex: '1 1 460px' }}><PttKpiCard /></div>
      </div>
      <div style={{ display: 'flex', gap: 10, flexWrap: 'wrap', marginBottom: 14 }}>
        <div style={{ flex: '1 1 460px' }}><TrendCard /></div>
        <div style={{ flex: '1 1 460px' }}><AnomalyCard /></div>
      </div>
      <ServiceDetailTabs />
    </div>
  )
}
