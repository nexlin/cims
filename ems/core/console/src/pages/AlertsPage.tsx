import { useState, useEffect, useCallback } from 'react'
import { alertsApi, eventsApi, type AlertEvent, type AlertSummaryResponse, type AlertRulesResponse, type EventRecord } from '../api/alerts'
import { useToast } from '../components/Toast'

const TYPE_LABEL: Record<string, string> = {
  // 조건 클래스 (표준)
  process_down: '프로세스 다운',
  connection_lost: '연결 끊김',
  threshold_crossed: '임계 초과',
  // 구 type (하위호환 표시)
  csp_down: '프로세스 다운', cmp_down: '프로세스 다운', module_down: '프로세스 다운',
  db_down: '연결 끊김', rtp_high: '임계 초과', disk_high: '임계 초과',
}

function typeLabel(t: string): string {
  return TYPE_LABEL[t] || t
}

// X.733 perceived severity → 배지 클래스 (6단계).
function sevBadge(sev?: string): string {
  switch (sev) {
    case 'critical': return 'badge--red'
    case 'major': return 'badge--red'
    case 'minor': return 'badge--yellow'
    case 'warning': return 'badge--yellow'
    case 'indeterminate': return 'badge--blue'
    case 'cleared': return 'badge--gray'
    default: return 'badge--blue'
  }
}
function evSev(e: { perceived_severity?: string; severity?: string }): string {
  return e.perceived_severity || e.severity || 'warning'
}

function fmtTime(iso: string): string {
  if (!iso) return '-'
  return new Date(iso).toLocaleString('ko-KR', {
    year: '2-digit', month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit', second: '2-digit',
  })
}

function durationBetween(openTs: string, closeTs: string): string {
  const o = new Date(openTs).getTime()
  const c = new Date(closeTs).getTime()
  if (isNaN(o) || isNaN(c) || c < o) return '-'
  const sec = Math.round((c - o) / 1000)
  return formatSec(sec)
}

function formatSec(sec: number): string {
  if (sec < 0) return '-'
  if (sec < 60) return `${sec}초`
  if (sec < 3600) return `${Math.floor(sec / 60)}분 ${sec % 60}초`
  return `${Math.floor(sec / 3600)}시간 ${Math.floor((sec % 3600) / 60)}분`
}

interface AlertRow extends AlertEvent {
  resolved_at?: string  // close 이벤트가 있으면 그 시각
  duration?: string
  occurrences?: number  // 재통지 횟수 (clear 없이 반복 수신된 open 수, 최초 포함)
  last_open_ts?: string // 마지막 재통지 시각 (occurrences > 1 일 때만 의미)
}

function DailyBars({ data }: { data: { date: string; opens: number }[] }) {
  if (data.length === 0) return <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>—</div>
  const max = Math.max(1, ...data.map(d => d.opens))
  const H = 36
  return (
    <div style={{ display: 'flex', alignItems: 'flex-end', gap: 3, height: H + 14 }}>
      {data.map((d, i) => {
        const h = d.opens > 0 ? Math.max(2, Math.round((d.opens / max) * H)) : 1
        const mmdd = d.date.slice(5).replace('-', '/')
        // 30/90일 범위에서 모든 날짜 라벨을 찍으면 겹쳐 읽을 수 없음 — 적정 간격만 표기
        const labelEvery = data.length > 60 ? 7 : data.length > 21 ? 3 : 1
        const showLabel = i % labelEvery === 0 || i === data.length - 1
        return (
          <div key={d.date} style={{ flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center', minWidth: 0 }}
            title={`${d.date}: ${d.opens}건`}>
            <div style={{
              width: '100%',
              height: h,
              background: d.opens > 0 ? 'var(--danger)' : 'var(--border)',
              borderRadius: 2,
              opacity: d.opens > 0 ? 0.85 : 0.4,
            }} />
            <div style={{ fontSize: 9, color: 'var(--text-muted)', marginTop: 2, whiteSpace: 'nowrap',
                          overflow: 'visible', visibility: showLabel ? 'visible' : 'hidden' }}>
              {mmdd}
            </div>
          </div>
        )
      })}
    </div>
  )
}

/**
 * open/close 페어링: 같은 type 의 가장 가까운 후속 close 를 찾아 매칭.
 * 매칭 안 된 open 은 currently open 으로 표시. close 는 standalone 으로도 표시.
 *
 * clear 없이 같은 key 의 open 이 다시 오면 **같은 알람의 재통지**로 보고 기존 행을
 * 갱신한다 (새 행 X). alarm_standardization.md §3.4 — 새 occurrence 는 clear 후
 * 재open 일 때만 성립하므로, 연속 open 을 별개 행으로 만들면 뒤따르는 close 1건이
 * 마지막 행만 닫고 앞선 행은 영구 미해소로 남는다(활성 알람 유령).
 */
// 활성 식별 키 = alarm_id 의 occurrence epoch 제거(code@mo) / 구 레코드는 type.
function akey(ev: AlertEvent): string {
  if (ev.alarm_id) return ev.alarm_id.replace(/@\d+$/, '')
  return ev.type
}

function pairEvents(events: AlertEvent[]): AlertRow[] {
  const sortedAsc = [...events].sort((a, b) => (a.ts || '').localeCompare(b.ts || ''))
  const rows: AlertRow[] = []
  const openByKey: Record<string, AlertRow> = {}
  for (const ev of sortedAsc) {
    const k = akey(ev)
    if (ev.action === 'open') {
      const prev = openByKey[k]
      if (prev) {                      // 미해소 open 이 이미 있음 → 재통지, 행 추가 X
        prev.occurrences = (prev.occurrences ?? 1) + 1
        prev.last_open_ts = ev.ts
        prev.message = ev.message ?? prev.message
        continue
      }
      const row: AlertRow = { ...ev, occurrences: 1 }
      rows.push(row)
      openByKey[k] = row
    } else if (ev.action === 'ack') {  // 승인 — 해당 open 행에 ack 상태 주석 (행 추가 X)
      const open = openByKey[k]
      if (open) {
        open.ack_state = 'acknowledged'
        open.ack_user = ev.ack_user
        open.ack_time = ev.ts
      }
    } else {  // close
      const open = openByKey[k]
      if (open) {
        open.resolved_at = ev.ts
        open.duration = durationBetween(open.ts, ev.ts)
        delete openByKey[k]
      } else {
        rows.push({ ...ev })
      }
    }
  }
  return rows.sort((a, b) => (b.ts || '').localeCompare(a.ts || ''))
}

// 이벤트 탭 — 정상 동작 통지(stateChange/audit) 스트림. 알람과 모델 분리(§3.6),
// 표시단에서도 스트림 구분 (alarm_self_reporting.md §6).
const EVENT_TYPE_LABEL: Record<string, string> = {
  process_started: '프로세스 기동',
  process_stopping: '프로세스 종료',
  config_reloaded: '설정 재적재',
  catalog_registered: '카탈로그 등록',
}
const KIND_LABEL: Record<string, string> = { stateChange: '상태 변화', audit: '감사' }

function EventsSection() {
  const { show } = useToast()
  const [events, setEvents] = useState<EventRecord[]>([])
  const [types, setTypes] = useState<string[]>([])
  const [days, setDays] = useState(7)
  const [filterType, setFilterType] = useState('')
  const [filterKind, setFilterKind] = useState('')
  const [loading, setLoading] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [list, typeList] = await Promise.all([
        eventsApi.list({ days, type: filterType || undefined, kind: filterKind || undefined, limit: 2000 }),
        eventsApi.types(),
      ])
      setEvents(list.events)
      setTypes(typeList.types)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [days, filterType, filterKind, show])

  useEffect(() => { load() }, [load])

  return (
    <>
      <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
        <span style={{ fontSize: 13, color: 'var(--text-muted)' }}>기간:</span>
        {[1, 7, 30, 90].map(d => (
          <button key={d}
            className={`btn btn--sm ${days === d ? 'btn--primary' : 'btn--ghost'}`}
            onClick={() => setDays(d)}>
            {d === 1 ? '오늘' : `${d}일`}
          </button>
        ))}
        <div style={{ width: 1, height: 24, background: 'var(--border)', margin: '0 8px' }} />
        <span style={{ fontSize: 13, color: 'var(--text-muted)' }}>유형:</span>
        <select className="form-input" value={filterType}
          onChange={e => setFilterType(e.target.value)} style={{ width: 160 }}>
          <option value="">전체</option>
          {types.map(t => <option key={t} value={t}>{EVENT_TYPE_LABEL[t] || t}</option>)}
        </select>
        <span style={{ fontSize: 13, color: 'var(--text-muted)' }}>분류:</span>
        <select className="form-input" value={filterKind}
          onChange={e => setFilterKind(e.target.value)} style={{ width: 120 }}>
          <option value="">전체</option>
          <option value="stateChange">상태 변화</option>
          <option value="audit">감사</option>
        </select>
        <button className="btn btn--ghost btn--sm" onClick={load} style={{ marginLeft: 'auto' }}>↻</button>
      </div>

      <div className="panel">
        <div style={{ padding: '12px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)' }}>
          이벤트 이력 ({events.length}건)
        </div>
        {loading ? (
          <div className="empty">로딩 중…</div>
        ) : events.length === 0 ? (
          <div className="empty">기록된 이벤트 없음</div>
        ) : (
          <table className="data-table">
            <thead>
              <tr>
                <th style={{ width: 150 }}>시각</th>
                <th style={{ width: 90 }}>분류</th>
                <th style={{ width: 140 }}>유형</th>
                <th style={{ width: 170 }}>소스</th>
                <th>메시지</th>
              </tr>
            </thead>
            <tbody>
              {events.map((ev, i) => (
                <tr key={`${ev.ts}-${ev.type}-${i}`}>
                  <td className="ts">{fmtTime(ev.ts)}</td>
                  <td>
                    <span className={`badge ${ev.kind === 'audit' ? 'badge--gray' : 'badge--blue'}`}>
                      {KIND_LABEL[ev.kind || ''] || ev.kind || '-'}
                    </span>
                  </td>
                  <td>{EVENT_TYPE_LABEL[ev.type] || ev.type}</td>
                  <td><code style={{ fontSize: 11 }}>{ev.source?.mo_instance || '-'}</code></td>
                  <td title={ev.source?.detected_by}>{ev.message}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
    </>
  )
}

// openOnly=true → 활성 알람 뷰(해소된 알람 숨김 기본), false → 알람·이벤트 이력 전체.
export default function AlertsPage({ openOnly = false }: { openOnly?: boolean } = {}) {
  const { show } = useToast()
  const [events, setEvents] = useState<AlertEvent[]>([])
  const [types, setTypes] = useState<string[]>([])
  const [summary, setSummary] = useState<AlertSummaryResponse | null>(null)
  const [rules, setRules] = useState<AlertRulesResponse | null>(null)
  const [days, setDays] = useState(openOnly ? 90 : 7)
  const [filterType, setFilterType] = useState('')
  const [showResolved, setShowResolved] = useState(!openOnly)
  const [loading, setLoading] = useState(false)
  // 알람/이벤트 스트림 탭 — 활성 알람 뷰(openOnly)에는 이벤트 탭 없음.
  const [tab, setTab] = useState<'alarms' | 'events'>('alarms')

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [list, typeList, sum] = await Promise.all([
        alertsApi.list({ days, type: filterType || undefined, limit: 2000 }),
        alertsApi.types(),
        alertsApi.summary(days),
      ])
      setEvents(list.events)
      setTypes(typeList.types)
      setSummary(sum)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [days, filterType, show])

  useEffect(() => { load() }, [load])
  // 활성 알림 규칙 — days 와 무관, 1회 로드. 실패해도 이력 화면엔 영향 없음.
  useEffect(() => { alertsApi.rules().then(setRules).catch(() => setRules(null)) }, [])

  const ackAlarm = useCallback(async (alarmId?: string) => {
    if (!alarmId) return
    try { await alertsApi.ack(alarmId); show('알람 승인됨', 'ok'); load() }
    catch (e) { show((e as Error).message, 'err') }
  }, [load, show])

  const rows = pairEvents(events).filter(r => showResolved || !r.resolved_at)
  const openCount = rows.filter(r => r.action === 'open' && !r.resolved_at).length

  if (!openOnly && tab === 'events') {
    return (
      <div className="page">
        <div className="tab-nav">
          <button className="tab-btn" onClick={() => setTab('alarms')}>알람</button>
          <button className="tab-btn tab-btn--active">이벤트</button>
        </div>
        <EventsSection />
      </div>
    )
  }

  return (
    <div className="page">
      {!openOnly && (
        <div className="tab-nav">
          <button className="tab-btn tab-btn--active">알람</button>
          <button className="tab-btn" onClick={() => setTab('events')}>이벤트</button>
        </div>
      )}
      <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
        <span style={{ fontSize: 13, color: 'var(--text-muted)' }}>기간:</span>
        {[1, 7, 30, 90].map(d => (
          <button key={d}
            className={`btn btn--sm ${days === d ? 'btn--primary' : 'btn--ghost'}`}
            onClick={() => setDays(d)}>
            {d === 1 ? '오늘' : `${d}일`}
          </button>
        ))}
        <div style={{ width: 1, height: 24, background: 'var(--border)', margin: '0 8px' }} />
        <span style={{ fontSize: 13, color: 'var(--text-muted)' }}>유형:</span>
        <select className="form-input" value={filterType}
          onChange={e => setFilterType(e.target.value)} style={{ width: 160 }}>
          <option value="">전체</option>
          {types.map(t => <option key={t} value={t}>{typeLabel(t)}</option>)}
        </select>
        <label style={{ display: 'flex', alignItems: 'center', gap: 4, fontSize: 13 }}>
          <input type="checkbox" checked={showResolved}
            onChange={e => setShowResolved(e.target.checked)} />
          해제된 알람 포함
        </label>
        <button className="btn btn--ghost btn--sm" onClick={load} style={{ marginLeft: 'auto' }}>↻</button>
      </div>

      <div style={{ display: 'flex', gap: 12 }}>
        <div style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '12px 16px' }}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>현재 열린 알람</div>
          <div style={{ fontSize: 24, fontWeight: 700, color: openCount > 0 ? 'var(--danger)' : 'var(--text)' }}>{openCount}</div>
        </div>
        <div style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '12px 16px' }}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>최근 {days}일 발생</div>
          <div style={{ fontSize: 24, fontWeight: 700 }}>{rows.filter(r => r.action === 'open').length}</div>
        </div>
        <div style={{ flex: 2, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '12px 16px' }}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 6 }}>일별 발생량</div>
          <DailyBars data={summary?.daily || []} />
        </div>
      </div>

      {rules && rules.rules.length > 0 && (
        <div className="panel">
          <div style={{ padding: '10px 16px', fontWeight: 600, fontSize: 13, borderBottom: '1px solid var(--border)',
                        display: 'flex', alignItems: 'center', gap: 8 }}>
            알림 규칙 ({rules.rules.length})
            <span style={{ fontSize: 11, fontWeight: 400, color: 'var(--text-muted)' }}>
              점검 주기 {rules.sweep_sec}초 · {rules.editable ? '편집 가능' : '읽기 전용 (oam.json 설정 기반)'}
            </span>
          </div>
          <table className="data-table">
            <thead>
              <tr>
                <th style={{ width: 80 }}>심각도</th>
                <th style={{ width: 110 }}>코드</th>
                <th style={{ width: 120 }}>클래스</th>
                <th style={{ width: 130 }}>event type</th>
                <th>대상 (지표·소스)</th>
                <th style={{ width: 180 }}>발생 조건</th>
              </tr>
            </thead>
            <tbody>
              {rules.rules.map((r, i) => (
                  <tr key={`${r.code}-${r.mo_instance || r.scope}-${i}`}
                      title={[r.effect && `영향: ${r.effect}`, r.recommended_action && `조치: ${r.recommended_action}`].filter(Boolean).join('\n')}>
                    <td><span className={`badge ${sevBadge(evSev(r))}`}>{evSev(r)}</span></td>
                    <td style={{ fontFamily: 'monospace', fontSize: 11 }}>{r.code || '-'}</td>
                    <td>{typeLabel(r.type)}</td>
                    <td style={{ fontSize: 12, color: 'var(--text-muted)' }}>{r.event_type || '-'}</td>
                    <td>{r.metric}{r.mo_instance && <code style={{ marginLeft: 6, fontSize: 11, color: 'var(--text-muted)' }}>{r.mo_instance}</code>}</td>
                    <td style={{ fontFamily: 'monospace', fontSize: 12 }}>
                      {r.condition}
                      {r.threshold != null && (
                        <span style={{ marginLeft: 6, color: 'var(--text-muted)', fontFamily: 'inherit' }}>
                          (threshold {r.threshold}{r.unit || ''})
                        </span>
                      )}
                    </td>
                  </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

      {summary && summary.by_type.length > 0 && (
        <div className="panel">
          <div style={{ padding: '10px 16px', fontWeight: 600, fontSize: 13, borderBottom: '1px solid var(--border)' }}>
            유형별 통계 (최근 {summary.days}일)
          </div>
          <table className="data-table">
            <thead>
              <tr>
                <th style={{ width: 120 }}>클래스</th>
                <th style={{ width: 160 }}>소스</th>
                <th style={{ width: 80, textAlign: 'right' }}>발생</th>
                <th style={{ width: 80, textAlign: 'right' }}>해제</th>
                <th style={{ width: 100 }}>현재 상태</th>
                <th style={{ width: 140, textAlign: 'right' }}>평균 지속시간</th>
                <th>마지막 이벤트</th>
              </tr>
            </thead>
            <tbody>
              {summary.by_type.map((s, i) => (
                <tr key={s.key || `${s.type}-${i}`}>
                  <td>{typeLabel(s.type)}</td>
                  <td><code style={{ fontSize: 11 }}>{s.mo_instance || '-'}</code></td>
                  <td style={{ textAlign: 'right' }}>{s.opens}</td>
                  <td style={{ textAlign: 'right' }}>{s.resolved}</td>
                  <td>
                    {s.currently_open
                      ? <span className="badge badge--red">OPEN</span>
                      : <span style={{ color: 'var(--text-muted)' }}>정상</span>}
                  </td>
                  <td style={{ textAlign: 'right' }}>
                    {s.avg_duration_sec != null ? formatSec(Math.round(s.avg_duration_sec)) : '-'}
                  </td>
                  <td className="ts">{s.last_ts ? fmtTime(s.last_ts) : '-'}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

      <div className="panel">
        <div style={{ padding: '12px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)' }}>
          알람 이력 ({rows.length}건)
        </div>
        {loading ? (
          <div className="empty">로딩 중…</div>
        ) : rows.length === 0 ? (
          <div className="empty">기록된 알람 없음</div>
        ) : (
          <table className="data-table">
            <thead>
              <tr>
                <th style={{ width: 80 }}>심각도</th>
                <th style={{ width: 110 }}>클래스</th>
                <th style={{ width: 150 }}>소스</th>
                <th>메시지</th>
                <th style={{ width: 150 }}>발생 시각</th>
                <th style={{ width: 150 }}>해제 시각</th>
                <th style={{ width: 90 }}>지속 시간</th>
                <th style={{ width: 120 }}>승인</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((r, i) => {
                const isOpen = r.action === 'open' && !r.resolved_at
                const sev = evSev(r)
                const tip = [r.code && `code: ${r.code}`, r.event_type && `eventType: ${r.event_type}`,
                             r.probable_cause && `cause: ${r.probable_cause}`, r.effect && `영향: ${r.effect}`,
                             r.recommended_action && `조치: ${r.recommended_action}`].filter(Boolean).join('\n')
                return (
                  <tr key={`${r.ts}-${r.alarm_id || r.type}-${i}`} title={tip || undefined}
                      style={isOpen ? { background: 'rgba(220, 53, 69, 0.08)' } : undefined}>
                    <td><span className={`badge ${sevBadge(sev)}`}>{sev}</span></td>
                    <td>{typeLabel(r.type)}</td>
                    <td><code style={{ fontSize: 11 }}>{r.source?.mo_instance || '-'}</code></td>
                    <td>{r.message}{isOpen && <span style={{ marginLeft: 8, color: 'var(--danger)', fontSize: 11, fontWeight: 600 }}>OPEN</span>}</td>
                    <td className="ts">
                      {fmtTime(r.ts)}
                      {(r.occurrences ?? 1) > 1 && (
                        <span style={{ marginLeft: 6, fontSize: 10, fontWeight: 600,
                                       color: 'var(--text-muted)', border: '1px solid var(--border)',
                                       borderRadius: 3, padding: '0 3px' }}
                              title={`해제 없이 ${r.occurrences}회 재통지 — 최근 ${r.last_open_ts ? fmtTime(r.last_open_ts) : ''}`}>
                          ×{r.occurrences}
                        </span>
                      )}
                    </td>
                    <td className="ts">{r.resolved_at ? fmtTime(r.resolved_at) : (r.action === 'open' ? '—' : fmtTime(r.ts))}</td>
                    <td>{r.duration || (isOpen ? '진행 중' : '-')}</td>
                    <td>
                      {r.ack_state === 'acknowledged'
                        ? <span style={{ fontSize: 11, color: 'var(--success, #16a34a)' }} title={r.ack_time ? fmtTime(r.ack_time) : ''}>✓ {r.ack_user || '승인'}</span>
                        : isOpen
                          ? <button className="btn btn--sm btn--outline" onClick={() => ackAlarm(r.alarm_id)} disabled={!r.alarm_id}>승인</button>
                          : <span style={{ color: 'var(--text-muted)' }}>-</span>}
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        )}
      </div>
    </div>
  )
}
