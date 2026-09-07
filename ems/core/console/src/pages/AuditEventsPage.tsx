// 감사 이력 — kind=audit 이벤트(E-AUD-*) 전용 열람 (manager 이상, 라우트 requiredRole).
//   일반 이벤트 이력(AlertsPage EventsSection)과 분리하는 이유: 감사 기록은 "누가 무엇을 했나"의 통제 자료라
//   열람 권한이 다르고(alarm_catalog E-AUD 행 — 감청 수행 권한과 열람 권한 분리), 합법감청(call_monitored,
//   dispatch_center.md §5.7)은 params(감청자·관제 그룹·세션·대상·시간)를 열로 펼쳐 봐야 한다.
//   서버(OAM /events)가 manager 미만에게 audit 을 감추므로 이 화면은 manager 이상에서만 내용이 있다.
import { useState, useEffect, useCallback, useMemo } from 'react'
import { eventsApi, type EventRecord } from '../api/alerts'
import { useToast } from '../components/Toast'
import { Pager } from '../components/ListControls'
import { usePageParam } from '../widgets/pageParams'
import { eventTypeLabel, fmtTime, downloadCsv } from '../utils/alarmLabels'
import { RotateCw } from 'lucide-react'

const PAGE_SIZE = 20
const FETCH_LIMIT = 5000

// 합법감청 감사(E-AUD-016 call_monitored) params — CSP TasModule/GroupCallService 가 싣는 필드.
interface MonitoredParams {
  phase?: string        // started | ended | denied
  monitor?: string      // 감청자(관제사) id
  group?: string        // 관제 그룹 id
  session?: string      // relay session_id(통화) / PTT 그룹 id(청취)
  sesid?: string
  target_a?: string
  target_b?: string
  tap_mode?: string     // both | a | b | ptt_listen
  dur_ms?: number
}

const PHASE_LABEL: Record<string, string> = { started: '시작', ended: '종료', denied: '거절' }
const PHASE_BADGE: Record<string, string> = { started: 'badge--blue', ended: 'badge--gray', denied: 'badge--red' }
const TAP_LABEL: Record<string, string> = { both: '통화 양방향', a: '통화 발신측', b: '통화 착신측', ptt_listen: 'PTT 그룹콜 청취' }

function fmtDur(ms?: number): string {
  if (ms == null || ms < 0) return '-'
  const s = Math.round(ms / 1000)
  if (s < 60) return `${s}초`
  return `${Math.floor(s / 60)}분 ${s % 60}초`
}

export function AuditEventsSection() {
  const { show } = useToast()
  const [events, setEvents] = useState<EventRecord[]>([])
  const days = Number(usePageParam('days')[0]) || 7
  const [filterType, setFilterType] = useState('')
  const [filterPhase, setFilterPhase] = useState('')
  const [q, setQ] = useState('')
  const [loading, setLoading] = useState(false)
  const [forbidden, setForbidden] = useState(false)
  const [page, setPage] = useState(0)

  const load = useCallback(async () => {
    setLoading(true)
    setForbidden(false)
    try {
      const list = await eventsApi.list({ days, kind: 'audit', limit: FETCH_LIMIT })
      setEvents(list.events)
    } catch (e: unknown) {
      if (String(e).includes('403')) setForbidden(true)
      else show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [days, show])

  useEffect(() => { load() }, [load])
  useEffect(() => { setPage(0) }, [days, filterType, filterPhase, q])

  const types = useMemo(() => [...new Set(events.map(e => e.type).filter(Boolean))].sort(), [events])

  const filtered = useMemo(() => {
    const needle = q.trim().toLowerCase()
    return events.filter(e => {
      const p = (e.params || {}) as MonitoredParams
      if (filterType && e.type !== filterType) return false
      if (filterPhase && p.phase !== filterPhase) return false
      if (needle && ![e.code, e.type, e.message, p.monitor, p.group, p.session, p.target_a, p.target_b,
                      e.source?.mo_instance]
        .some(v => (v || '').toString().toLowerCase().includes(needle))) return false
      return true
    })
  }, [events, filterType, filterPhase, q])

  const pageStart = Math.min(page, Math.max(0, Math.ceil(filtered.length / PAGE_SIZE) - 1)) * PAGE_SIZE
  const pageRows = filtered.slice(pageStart, pageStart + PAGE_SIZE)

  const exportCsv = () => {
    downloadCsv(`audit_${days}d.csv`,
      ['시각', '코드', '유형', '단계', '행위자', '관제 그룹', '세션', '대상', '방식', '시간(ms)', '메시지'],
      filtered.map(e => {
        const p = (e.params || {}) as MonitoredParams
        return [e.ts, e.code || '', e.type, p.phase || '', p.monitor || '', p.group || '', p.session || '',
                [p.target_a, p.target_b].filter(Boolean).join('/'), p.tap_mode || '',
                p.dur_ms != null ? String(p.dur_ms) : '', e.message]
      }))
  }

  return (
    <>
      <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8, flex: 'none' }}>
        <select className="form-input" value={filterType} onChange={e => setFilterType(e.target.value)} style={{ width: 170 }}>
          <option value="">유형 전체</option>
          {types.map(t => <option key={t} value={t}>{eventTypeLabel(t)}</option>)}
        </select>
        <select className="form-input" value={filterPhase} onChange={e => setFilterPhase(e.target.value)} style={{ width: 110 }}>
          <option value="">단계 전체</option>
          <option value="started">시작</option>
          <option value="ended">종료</option>
          <option value="denied">거절</option>
        </select>
        <input className="search-input" style={{ width: 220 }} placeholder="행위자/그룹/세션/대상 검색"
               value={q} onChange={e => setQ(e.target.value)} />
        <button className="btn btn--ghost btn--sm" onClick={exportCsv} style={{ marginLeft: 'auto' }}
                disabled={filtered.length === 0}>CSV</button>
        <button className="btn btn--ghost btn--sm" onClick={load} title="새로고침"><RotateCw size={14} /></button>
      </div>

      <div className="panel" style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column' }}>
        <div style={{ padding: '10px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)', flex: 'none' }}>
          감사 이력 ({filtered.length}건)
          <span style={{ marginLeft: 8, fontSize: 11, fontWeight: 400, color: 'var(--muted-foreground)' }}>
            합법감청(E-AUD-016) 시작·종료·거절 — 열람은 운영 관리자 이상
          </span>
          {events.length >= FETCH_LIMIT && (
            <span style={{ marginLeft: 8, fontSize: 11, fontWeight: 400, color: 'var(--destructive)' }}>
              레코드 {FETCH_LIMIT}건 상한 도달 — 기간을 좁혀야 전체가 보입니다
            </span>
          )}
        </div>
        {loading ? (
          <div className="empty">로딩 중…</div>
        ) : forbidden ? (
          <div className="empty">감사 이력 열람 권한이 없습니다 (운영 관리자 이상)</div>
        ) : filtered.length === 0 ? (
          <div className="empty">기록된 감사 이벤트 없음</div>
        ) : (
          <>
            <div className="scroll-fill">
            <table className="data-table">
              <thead>
                <tr>
                  <th style={{ width: 150 }}>시각</th>
                  <th style={{ width: 130 }}>유형</th>
                  <th style={{ width: 70 }}>단계</th>
                  <th style={{ width: 150 }}>행위자(감청자)</th>
                  <th style={{ width: 130 }}>관제 그룹</th>
                  <th style={{ width: 170 }}>세션</th>
                  <th style={{ width: 200 }}>대상</th>
                  <th style={{ width: 120 }}>방식</th>
                  <th style={{ width: 90 }}>시간</th>
                  <th>메시지</th>
                </tr>
              </thead>
              <tbody>
                {pageRows.map((ev, i) => {
                  const p = (ev.params || {}) as MonitoredParams
                  const targets = [p.target_a, p.target_b].filter(Boolean).join(' / ')
                  return (
                    <tr key={`${ev.ts}-${pageStart + i}`}>
                      <td className="ts">{fmtTime(ev.ts)}</td>
                      <td>{eventTypeLabel(ev.type)}<div style={{ fontFamily: 'monospace', fontSize: 10, color: 'var(--muted-foreground)' }}>{ev.code || ''}</div></td>
                      <td>{p.phase
                        ? <span className={`badge ${PHASE_BADGE[p.phase] || 'badge--gray'}`}>{PHASE_LABEL[p.phase] || p.phase}</span>
                        : '-'}</td>
                      <td><code style={{ fontSize: 11 }}>{p.monitor || '-'}</code></td>
                      <td><code style={{ fontSize: 11 }}>{p.group || '-'}</code></td>
                      <td><code style={{ fontSize: 11 }} title={p.sesid}>{p.session || '-'}</code></td>
                      <td><code style={{ fontSize: 11 }}>{targets || '-'}</code></td>
                      <td>{p.tap_mode ? (TAP_LABEL[p.tap_mode] || p.tap_mode) : '-'}</td>
                      <td>{fmtDur(p.dur_ms)}</td>
                      <td title={ev.source?.mo_instance}>{ev.message}</td>
                    </tr>
                  )
                })}
              </tbody>
            </table>
            </div>
            <Pager page={page} count={filtered.length} pageSize={PAGE_SIZE} onPage={setPage} />
          </>
        )}
      </div>
    </>
  )
}
