import { useState, useEffect, useCallback, useReducer, Fragment } from 'react'
import { useSearchParams, useNavigate } from 'react-router-dom'
import {
  statsApi, type Subscriber, type SubscribersResponse,
  type ServiceLive, type ServiceTrend, type Pool, type VolteCall, type PttGroup, type Anomaly,
  type ServiceEvent, type OrgStat, type PttMembersResponse,
} from '../../../api/stats'
import { useToast } from '../../../components/Toast'

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
  if (pct >= 60) return 'var(--warning, #d98e00)'
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
const useServiceTrend = makeSharedPoll<ServiceTrend>(() => statsApi.serviceTrend(30))

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
const BARS = ['▁', '▂', '▃', '▄', '▅', '▆', '▇', '█']
function Sparkline({ values, color }: { values: number[]; color?: string }) {
  const max = Math.max(1, ...values)
  const txt = values.map(v => v <= 0 ? '▁' : BARS[Math.min(BARS.length - 1, Math.max(0, Math.round((v / max) * (BARS.length - 1))))]).join('')
  return <span style={{ fontFamily: 'monospace', fontSize: 15, letterSpacing: '-1px', color: color || 'var(--primary)' }}>{txt}</span>
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

// ── 위젯: 동시 사용량 추세 ────────────────────────────────
export function TrendCard() {
  const trend = useServiceTrend()
  if (!trend) return <div className="panel" style={{ padding: 14 }}><Loading /></div>
  return (
    <div className="panel" style={{ padding: '10px 14px' }}>
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
  const now = useNowTick()
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
                    <td colSpan={8} style={{ background: 'var(--bg-subtle, rgba(0,0,0,.02))' }}>
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
export function OrgStatsCard() {
  const { show } = useToast()
  const live = useServiceLive()
  const now = useNowTick()
  const [orgs, setOrgs] = useState<OrgStat[]>([])
  const [loading, setLoading] = useState(true)
  const [sel, setSel] = useState<string | null>(null)
  const load = useCallback(async () => {
    try { setOrgs((await statsApi.serviceOrg()).orgs) }
    catch (e: unknown) { show(String(e), 'err') } finally { setLoading(false) }
  }, [show])
  useEffect(() => { load(); const iv = setInterval(load, 5000); return () => clearInterval(iv) }, [load])
  if (loading) return <div className="panel"><Loading /></div>
  if (orgs.length === 0) return <div className="empty">조직 정보가 없습니다</div>

  const selOrg = orgs.find(o => o.org === sel) || null
  // 조직 가입자 활동 기준 (그룹 귀속이 아니라 가입자 활동)
  const calls = (live?.volte.calls ?? []).filter(c => c.org === sel)
  const talkers = (live?.ptt.talkers ?? []).filter(t => t.org === sel)

  return (
    <div className="panel">
      <table className="data-table">
        <thead><tr><th>조직</th><th>VoLTE 등록</th><th>PTT 등록</th><th>VoLTE 통화</th><th>PTT 발언</th><th>PTT 참여</th></tr></thead>
        <tbody>
          {orgs.map(o => (
            <tr key={o.org} onClick={() => setSel(sel === o.org ? null : o.org)}
              style={{ cursor: 'pointer', ...(sel === o.org ? { background: 'rgba(80,120,255,.1)' } : {}) }}>
              <td style={{ fontWeight: 600 }}>{sel === o.org ? '▾ ' : '▸ '}{o.name}</td>
              <td><RegBar reg={o.volte_reg} num={o.volte_num} /></td>
              <td><RegBar reg={o.ptt_reg} num={o.ptt_num} /></td>
              <td>{o.active_volte > 0 ? <span className="badge badge--blue">{o.active_volte}</span> : <span className="ts">0</span>}</td>
              <td>{o.ptt_talking > 0 ? <span className="badge badge--green">🎤 {o.ptt_talking}</span> : <span className="ts">0</span>}</td>
              <td className="ts">{o.active_ptt}</td>
            </tr>
          ))}
        </tbody>
      </table>

      {selOrg && (
        <div style={{ borderTop: '1px solid var(--border)', padding: '10px 12px', background: 'var(--bg-subtle, rgba(0,0,0,.02))' }}>
          <div style={{ fontWeight: 600, fontSize: 13, marginBottom: 8 }}>{selOrg.name} · 가입자 활동</div>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 4 }}>VoLTE 통화 중 ({calls.length})</div>
          {calls.length === 0 ? <div className="ts" style={{ marginBottom: 8 }}>없음</div>
            : <div style={{ marginBottom: 10 }}>{calls.map(c => (
                <div key={c.call_id} style={{ fontSize: 13, padding: '2px 0' }}>
                  <span className={`badge ${c.state === 'ringing' ? 'badge--blue' : 'badge--green'}`}>{c.state === 'ringing' ? '호출' : '통화'}</span>
                  {' '}<b>{c.caller}</b> → {c.callee} <span className="ts">· {c.video ? '영상' : '음성'} · {fmtDur(elapsedSec(c.invite_time, now, c.duration_sec))} · {c.media_node || '-'}</span>
                </div>
              ))}</div>}
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 4 }}>PTT 발언 중 ({talkers.length}) · 참여 {selOrg.active_ptt}명</div>
          {talkers.length === 0 ? <div className="ts">발언 중인 가입자 없음</div>
            : talkers.map((t, i) => (
                <div key={i} style={{ fontSize: 13, padding: '2px 0' }}>
                  <span style={{ color: 'var(--primary)', fontWeight: 600 }}>🎤 {t.msisdn}</span>
                  <span className="ts"> → {t.group_name}</span>
                </div>
              ))}
        </div>
      )}
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

// ── 기본 페이지: 위젯을 합성(섹션 헤더 + 카드) ────────────
function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div style={{ marginBottom: 14 }}>
      <div style={{ fontSize: 13, fontWeight: 600, color: 'var(--text-muted)', margin: '0 0 6px 2px' }}>{title}</div>
      {children}
    </div>
  )
}
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
      <Section title="VoLTE 활성 호"><VolteCallsCard /></Section>
      <Section title="PTT 활성 그룹"><PttGroupsCard /></Section>
      <Section title="라이브 이벤트"><EventFeedCard /></Section>
      <Section title="조직별 집계"><OrgStatsCard /></Section>
      <Section title="가입자 조회"><SubscriberLookup /></Section>
    </div>
  )
}
