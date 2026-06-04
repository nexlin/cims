import { useState, useEffect, useCallback, Fragment } from 'react'
import { useSearchParams } from 'react-router-dom'
import { useNavigate } from 'react-router-dom'
import {
  statsApi, type Subscriber, type SubscribersResponse,
  type ServiceLive, type ServiceTrend, type Pool, type VolteCall, type PttGroup, type Anomaly,
  type ServiceEvent, type OrgStat,
} from '../../../api/stats'
import { useToast } from '../../../components/Toast'

type Tab = 'volte' | 'ptt' | 'events' | 'org' | 'subscribers'

// 고정(watch) — localStorage 영속
function usePins(key: string): { pins: Set<string>; toggle: (id: string) => void } {
  const [pins, setPins] = useState<Set<string>>(() => {
    try { return new Set(JSON.parse(localStorage.getItem(key) || '[]')) } catch { return new Set() }
  })
  const toggle = (id: string) => setPins(prev => {
    const n = new Set(prev)
    if (n.has(id)) n.delete(id); else n.add(id)
    localStorage.setItem(key, JSON.stringify([...n]))
    return n
  })
  return { pins, toggle }
}

function PinBtn({ on, onClick }: { on: boolean; onClick: (e: React.MouseEvent) => void }) {
  return <button className="btn btn--sm btn--ghost" title={on ? '고정 해제' : '고정'} onClick={onClick}
    style={{ padding: '0 6px', opacity: on ? 1 : 0.35 }}>📌</button>
}

// ── 공통 유틸 ─────────────────────────────────────────────
function fmtDur(sec: number): string {
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
  if (pct >= 60) return 'var(--warning, #d98e00)'
  return 'var(--success)'
}

// 1초 틱 (경과시간 라이브 카운트)
function useNowTick(periodMs = 1000): number {
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => {
    const iv = setInterval(() => setNow(Date.now()), periodMs)
    return () => clearInterval(iv)
  }, [periodMs])
  return now
}

function OnlineDot({ on }: { on: boolean }) {
  return <span style={{ display: 'inline-block', width: 8, height: 8, borderRadius: '50%', background: on ? 'var(--success)' : 'var(--text-muted)', marginRight: 6 }} />
}

// ── 용량 게이지 ───────────────────────────────────────────
function Gauge({ label, pool }: { label: string; pool: Pool }) {
  const total = pool.total || 0
  const used = pool.used || 0
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

// ── KPI 칩 ────────────────────────────────────────────────
function Kpi({ label, value, sub }: { label: string; value: React.ReactNode; sub?: string }) {
  return (
    <div style={{ display: 'flex', flexDirection: 'column', minWidth: 86 }}>
      <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>{label}</span>
      <span style={{ fontSize: 20, fontWeight: 700, lineHeight: 1.2 }}>{value}</span>
      {sub && <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>{sub}</span>}
    </div>
  )
}

// ── 스파크라인 (unicode 블록) ─────────────────────────────
const BARS = ['▁', '▂', '▃', '▄', '▅', '▆', '▇', '█']
function Sparkline({ values, color }: { values: number[]; color?: string }) {
  const max = Math.max(1, ...values)
  const txt = values.map(v => {
    if (v <= 0) return '▁'
    const idx = Math.min(BARS.length - 1, Math.max(0, Math.round((v / max) * (BARS.length - 1))))
    return BARS[idx]
  }).join('')
  return <span style={{ fontFamily: 'monospace', fontSize: 15, letterSpacing: '-1px', color: color || 'var(--primary)' }}>{txt}</span>
}

// ── 상단: KPI + 게이지 + 추세 + 이상징후 ──────────────────
function LiveHeader({ live, trend, onJump }: { live: ServiceLive | null; trend: ServiceTrend | null; onJump: (a: Anomaly) => void }) {
  const v = live?.volte.kpi
  const p = live?.ptt.kpi
  return (
    <>
      <div className="panel" style={{ padding: 14, marginBottom: 10 }}>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 24, alignItems: 'center' }}>
          <span className="badge badge--blue" style={{ alignSelf: 'flex-start' }}>VoLTE</span>
          <Kpi label="통화 중" value={v?.active ?? '-'} />
          <Kpi label="호출 중" value={v?.ringing ?? '-'} />
          <Kpi label="평균 통화" value={v ? fmtDur(v.avg_duration_sec) : '-'} />
          <Kpi label="등록" value={v?.registered ?? '-'} sub={v ? `/ ${v.numbers}` : ''} />
          {live && <Gauge label="RTP 풀" pool={live.capacity.volte_rtp} />}
        </div>
        <div style={{ borderTop: '1px solid var(--border)', margin: '12px 0' }} />
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 24, alignItems: 'center' }}>
          <span className="badge badge--green" style={{ alignSelf: 'flex-start' }}>PTT</span>
          <Kpi label="활성 그룹" value={p?.active_groups ?? '-'} />
          <Kpi label="발언 중" value={p?.talking ?? '-'} />
          <Kpi label="참여자" value={p?.participants ?? '-'} />
          <Kpi label="등록" value={p?.registered ?? '-'} sub={p ? `/ ${p.numbers}` : ''} />
          {live && <Gauge label="PTT RTP 풀" pool={live.capacity.ptt_rtp} />}
        </div>
      </div>

      {trend && (
        <div className="panel" style={{ padding: '10px 14px', marginBottom: 10 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 4 }}>
            <span style={{ fontSize: 12, color: 'var(--text-muted)', width: 64 }}>통화 수</span>
            <Sparkline values={trend.points.map(pt => pt.volte)} />
            <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>now {trend.volte_now} · peak {trend.volte_peak}</span>
          </div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
            <span style={{ fontSize: 12, color: 'var(--text-muted)', width: 64 }}>그룹 수</span>
            <Sparkline values={trend.points.map(pt => pt.ptt)} color="var(--success)" />
            <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>now {trend.ptt_now} · peak {trend.ptt_peak}</span>
          </div>
          <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>최근 {trend.window_min}분</span>
        </div>
      )}

      {live && live.anomalies.length > 0 && (
        <div className="panel" style={{ padding: '10px 14px', marginBottom: 10, borderLeft: '3px solid var(--danger)' }}>
          <div style={{ fontSize: 13, fontWeight: 600, marginBottom: 6, color: 'var(--danger)' }}>⚠ 이상 징후 ({live.anomalies.length})</div>
          {live.anomalies.map((a, i) => (
            <div key={i} style={{ display: 'flex', alignItems: 'center', gap: 8, fontSize: 13, marginBottom: 3 }}>
              <span className="badge badge--red">{a.kind === 'volte' ? 'VoLTE' : 'PTT'}</span>
              <span>{a.detail}</span>
              <span style={{ color: 'var(--text-muted)' }}>{a.label}</span>
              <button className="btn btn--sm btn--ghost" style={{ marginLeft: 'auto' }} onClick={() => onJump(a)}>이동 ▸</button>
            </div>
          ))}
        </div>
      )}
    </>
  )
}

// ── VoLTE 활성 호 ─────────────────────────────────────────
function VolteCalls({ calls, now, onHistory, pins, togglePin }: { calls: VolteCall[]; now: number; onHistory: () => void; pins: Set<string>; togglePin: (id: string) => void }) {
  if (calls.length === 0) return <div className="empty">현재 통화 중인 호가 없습니다</div>
  const sorted = [...calls].sort((a, b) => (pins.has(b.call_id) ? 1 : 0) - (pins.has(a.call_id) ? 1 : 0))
  return (
    <div className="panel">
      <table className="data-table">
        <thead><tr><th></th><th>상태</th><th>발신 → 착신</th><th>유형</th><th>경과</th><th>호 ID</th><th></th></tr></thead>
        <tbody>
          {sorted.map(c => {
            const ring = c.state === 'ringing'
            const warn = c.anomalies.length > 0
            const pinned = pins.has(c.call_id)
            return (
              <tr key={c.call_id} style={pinned ? { background: 'rgba(80,120,255,.08)' } : warn ? { background: 'rgba(220,50,50,.06)' } : undefined}>
                <td><PinBtn on={pinned} onClick={e => { e.stopPropagation(); togglePin(c.call_id) }} /></td>
                <td><span className={`badge ${ring ? 'badge--blue' : 'badge--green'}`}>{ring ? '호출 중' : '통화 중'}</span>{warn && <span title={c.anomalies.map(a => a.detail).join(', ')}> ⚠</span>}</td>
                <td><b>{c.caller || '-'}</b> <span style={{ color: 'var(--text-muted)' }}>→</span> {c.callee || '-'}</td>
                <td>{c.video ? '영상' : '음성'}</td>
                <td className="ts">{fmtDur(elapsedSec(c.invite_time, now, c.duration_sec))}</td>
                <td className="ts" title={c.call_id} style={{ maxWidth: 140, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{c.call_id}</td>
                <td><button className="btn btn--sm btn--ghost" onClick={onHistory}>이력 ▸</button></td>
              </tr>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}

// ── PTT 활성 그룹 (펼침: 멤버 + floor) ────────────────────
function PttGroups({ groups, now, onHistory, pins, togglePin }: { groups: PttGroup[]; now: number; onHistory: () => void; pins: Set<string>; togglePin: (id: string) => void }) {
  const [open, setOpen] = useState<Record<string, boolean>>({})
  if (groups.length === 0) return <div className="empty">현재 활성 그룹이 없습니다</div>
  const typeLabel = (t: string) => t === 'broadcast' ? 'broadcast' : t === 'chat' ? 'chat' : t === 'prearranged' ? 'prearranged' : (t || '-')
  const sorted = [...groups].sort((a, b) => (pins.has(b.group_id) ? 1 : 0) - (pins.has(a.group_id) ? 1 : 0))
  return (
    <div className="panel">
      <table className="data-table">
        <thead><tr><th></th><th>그룹</th><th>유형</th><th>참여</th><th>현재 화자</th><th>경과</th><th></th></tr></thead>
        <tbody>
          {sorted.map(g => {
            const isOpen = !!open[g.group_id]
            const warn = g.anomalies.length > 0
            const pinned = pins.has(g.group_id)
            return (
              <Fragment key={g.group_id}>
                <tr style={{ cursor: 'pointer', ...(pinned ? { background: 'rgba(80,120,255,.08)' } : warn ? { background: 'rgba(220,50,50,.06)' } : {}) }} onClick={() => setOpen(o => ({ ...o, [g.group_id]: !isOpen }))}>
                  <td><PinBtn on={pinned} onClick={e => { e.stopPropagation(); togglePin(g.group_id) }} /></td>
                  <td><span style={{ color: 'var(--text-muted)' }}>{isOpen ? '▾' : '▸'}</span> <b>{g.name}</b> <span className="ts">{g.group_id !== g.name ? `(${g.group_id})` : ''}</span></td>
                  <td><span className="badge">{typeLabel(g.type)}</span></td>
                  <td className="ts">{g.active_members} / {g.total_members}</td>
                  <td>{g.floor_holder ? <span style={{ color: 'var(--primary)', fontWeight: 600 }}>🎤 {g.floor_holder}</span> : <span className="ts">(없음)</span>}{warn && <span title={g.anomalies.map(a => a.detail).join(', ')}> ⚠</span>}</td>
                  <td className="ts">{fmtDur(elapsedSec(g.invite_time, now, g.duration_sec))}</td>
                  <td><button className="btn btn--sm btn--ghost" onClick={e => { e.stopPropagation(); onHistory() }}>이력 ▸</button></td>
                </tr>
                {isOpen && (
                  <tr>
                    <td colSpan={7} style={{ background: 'var(--bg-subtle, rgba(0,0,0,.02))', fontSize: 12 }}>
                      <span style={{ color: 'var(--text-muted)' }}>참여자: </span>
                      {g.members.map((m, i) => (
                        <span key={i} style={{ marginRight: 10 }}>
                          {m.subscriber_id}
                          {m.role === 'initiator' && <span className="ts"> (개시자)</span>}
                          {m.subscriber_id === g.floor_holder && ' 🎤'}
                        </span>
                      ))}
                      {g.floor_held_sec !== undefined && <span style={{ marginLeft: 8, color: 'var(--danger)' }}>· floor {fmtDur(g.floor_held_sec)} 점유</span>}
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

// ── 가입자 조회 (서버사이드 active/online/all + 검색 + 페이지) ──
const LOOKUP_LIMIT = 50
function SubscriberLookup() {
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
    catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
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
      {loading ? <div className="empty">로딩 중...</div>
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
      <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 6 }}>특정 가입자의 현재 상태를 이름/번호로 조회합니다. 5초 자동 갱신.</div>
    </div>
  )
}

// ── ④ 라이브 이벤트 피드 ──────────────────────────────────
const EV_ICON: Record<string, string> = {
  call_start: '📞', call_end: '📵', floor_grant: '🎤', floor_release: '🔇',
  floor_reject: '⛔', member_join: '➕', member_leave: '➖',
}
function EventFeed() {
  const { show } = useToast()
  const [events, setEvents] = useState<ServiceEvent[]>([])
  const [loading, setLoading] = useState(true)
  const load = useCallback(async () => {
    try { setEvents((await statsApi.serviceEvents(80)).events) }
    catch (e: unknown) { show(String(e), 'err') } finally { setLoading(false) }
  }, [show])
  useEffect(() => { load(); const iv = setInterval(load, 5000); return () => clearInterval(iv) }, [load])
  if (loading) return <div className="empty">로딩 중...</div>
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

// ── ⑥ 조직별 집계 ─────────────────────────────────────────
function RegBar({ reg, num }: { reg: number; num: number }) {
  const pct = num > 0 ? Math.round((reg / num) * 100) : 0
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
      <div style={{ width: 70, height: 6, borderRadius: 3, background: 'var(--border)', overflow: 'hidden' }}>
        <div style={{ width: `${pct}%`, height: '100%', background: 'var(--success)' }} />
      </div>
      <span className="ts">{reg}/{num}</span>
    </div>
  )
}
function OrgStats() {
  const { show } = useToast()
  const [orgs, setOrgs] = useState<OrgStat[]>([])
  const [loading, setLoading] = useState(true)
  const load = useCallback(async () => {
    try { setOrgs((await statsApi.serviceOrg()).orgs) }
    catch (e: unknown) { show(String(e), 'err') } finally { setLoading(false) }
  }, [show])
  useEffect(() => { load(); const iv = setInterval(load, 5000); return () => clearInterval(iv) }, [load])
  if (loading) return <div className="empty">로딩 중...</div>
  if (orgs.length === 0) return <div className="empty">조직 정보가 없습니다</div>
  return (
    <div className="panel">
      <table className="data-table">
        <thead><tr><th>조직</th><th>VoLTE 등록</th><th>PTT 등록</th><th>이용 중(VoLTE)</th><th>이용 중(PTT)</th></tr></thead>
        <tbody>
          {orgs.map(o => (
            <tr key={o.org}>
              <td style={{ fontWeight: 600 }}>{o.name}</td>
              <td><RegBar reg={o.volte_reg} num={o.volte_num} /></td>
              <td><RegBar reg={o.ptt_reg} num={o.ptt_num} /></td>
              <td>{o.active_volte > 0 ? <span className="badge badge--blue">{o.active_volte}</span> : <span className="ts">0</span>}</td>
              <td>{o.active_ptt > 0 ? <span className="badge badge--green">{o.active_ptt}</span> : <span className="ts">0</span>}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

// ── 메인 ──────────────────────────────────────────────────
export default function ServiceStatusPage() {
  const { show } = useToast()
  const navigate = useNavigate()
  const [tab, setTab] = useState<Tab>('volte')
  const [live, setLive] = useState<ServiceLive | null>(null)
  const [trend, setTrend] = useState<ServiceTrend | null>(null)
  const now = useNowTick()
  const voltePins = usePins('svc.pins.volte')
  const pttPins = usePins('svc.pins.ptt')

  const loadLive = useCallback(async () => {
    try {
      const [l, t] = await Promise.all([statsApi.serviceLive(), statsApi.serviceTrend(30)])
      setLive(l); setTrend(t)
    } catch (e: unknown) { show(String(e), 'err') }
  }, [show])

  useEffect(() => { loadLive(); const iv = setInterval(() => loadLive(), 5000); return () => clearInterval(iv) }, [loadLive])

  const jump = (a: Anomaly) => { setTab(a.kind === 'volte' ? 'volte' : 'ptt') }
  const gotoHistory = (kind: 'volte' | 'ptt') => navigate(kind === 'volte' ? '/service/history/volte' : '/service/history/ptt')

  const tabBtn = (t: Tab, label: string, n?: number) => (
    <button className={`btn btn--sm ${tab === t ? 'btn--primary' : 'btn--ghost'}`} onClick={() => setTab(t)}>{label}{n !== undefined ? ` (${n})` : ''}</button>
  )

  return (
    <div>
      <LiveHeader live={live} trend={trend} onJump={jump} />

      <div className="toolbar" style={{ marginTop: 4 }}>
        {tabBtn('volte', 'VoLTE 호', live?.volte.kpi.active)}
        {tabBtn('ptt', 'PTT 그룹', live?.ptt.kpi.active_groups)}
        {tabBtn('events', '라이브 이벤트')}
        {tabBtn('org', '조직별')}
        {tabBtn('subscribers', '가입자 조회')}
        <span style={{ marginLeft: 'auto', color: 'var(--text-muted)', fontSize: 12 }}>5초 자동 갱신{live?.ts ? ` · ${new Date(live.ts).toLocaleTimeString('ko-KR')}` : ''}</span>
      </div>

      {tab === 'volte' && <VolteCalls calls={live?.volte.calls ?? []} now={now} onHistory={() => gotoHistory('volte')} pins={voltePins.pins} togglePin={voltePins.toggle} />}
      {tab === 'ptt' && <PttGroups groups={live?.ptt.groups ?? []} now={now} onHistory={() => gotoHistory('ptt')} pins={pttPins.pins} togglePin={pttPins.toggle} />}
      {tab === 'events' && <EventFeed />}
      {tab === 'org' && <OrgStats />}
      {tab === 'subscribers' && <SubscriberLookup />}
    </div>
  )
}
