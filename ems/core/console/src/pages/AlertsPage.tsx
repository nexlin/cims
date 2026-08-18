// 알람·이벤트 이력 — 기록 탐색기 (alarm_pipeline.md §8.3 판독 규율로 open/close 페어링).
//   활성 알람 뷰는 ActiveAlarmsPage(store 라이브), 코드 사전·평가 규칙은 AlarmCatalogPage 소관 —
//   여기는 기간 창 안의 알람 라이프사이클(발생→변경→해소)과 이벤트 스트림 열람 전용.
//   필터는 전부 클라이언트에서 건다 — 서버 type 필터는 type 필드가 없는 ack/comment
//   레코드를 떨어뜨려 승인·코멘트 표시가 소실되기 때문(전 레코드 수신 후 행 단위 필터).
import { useState, useEffect, useCallback, useMemo } from 'react'
import { alertsApi, eventsApi, type AlertEvent, type AlertSummaryResponse, type EventRecord } from '../api/alerts'
import { useToast } from '../components/Toast'
import {
  alarmTypeLabel, eventTypeLabel, EVENT_KIND_LABEL, sevBadgeClass, severityOf,
  fmtTime, formatSec, durationBetween, downloadCsv,
} from '../utils/alarmLabels'

const PAGE_SIZE = 100
const FETCH_LIMIT = 5000   // 서버 상한 — 창 안 레코드가 이보다 많으면 최신순 절단(표기)

// ── 알람 스트림 접기 (§8.3) ──────────────────────────────────────────────────
interface SevChange { ts: string; from?: string; to?: string; trend?: string }

interface AlertRow extends AlertEvent {
  resolved_at?: string
  duration?: string
  occurrences?: number        // clear 없이 반복 수신된 open 수 (최초 포함)
  last_open_ts?: string
  comments?: { text: string; user?: string; ts: string }[]
  changes?: SevChange[]       // severity 승격/완화 이력 (notifyChangedAlarm)
  preWindow?: boolean         // 창 이전에 열린 알람의 해소만 창에 잡힌 행
}

// 활성 식별 키 = alarm_id 의 occurrence epoch 제거(code@mo) / 구 레코드는 type.
function akey(ev: AlertEvent): string {
  if (ev.alarm_id) return ev.alarm_id.replace(/@\d+$/, '')
  return ev.type
}

/**
 * open/close 페어링 + ack/change/comment 를 해당 open 행에 누적.
 * clear 없이 같은 key 의 open 재수신 = 같은 알람의 재통지 → 기존 행 갱신(행 추가 X,
 * alarm_standardization.md §3.4). 미지 action 은 무시(전방 호환).
 */
function pairEvents(events: AlertEvent[]): AlertRow[] {
  const sortedAsc = [...events].sort((a, b) => (a.ts || '').localeCompare(b.ts || ''))
  const rows: AlertRow[] = []
  const openByKey: Record<string, AlertRow> = {}
  for (const ev of sortedAsc) {
    const k = akey(ev)
    if (ev.action === 'open') {
      const prev = openByKey[k]
      if (prev) {
        prev.occurrences = (prev.occurrences ?? 1) + 1
        prev.last_open_ts = ev.ts
        prev.message = ev.message ?? prev.message
        continue
      }
      const row: AlertRow = { ...ev, occurrences: 1 }
      rows.push(row)
      openByKey[k] = row
    } else if (ev.action === 'ack') {
      const open = openByKey[k]
      if (open) {
        open.ack_state = 'acknowledged'
        open.ack_user = ev.ack_user
        open.ack_time = ev.ts
      }
    } else if (ev.action === 'change') {
      const open = openByKey[k]
      if (open) {
        const from = severityOf(open)
        open.changes = [...(open.changes ?? []),
          { ts: ev.ts, from, to: ev.perceived_severity, trend: ev.trend_indication }]
        open.perceived_severity = ev.perceived_severity ?? open.perceived_severity
        open.severity = ev.perceived_severity ?? open.severity
        open.message = ev.message ?? open.message
      }
    } else if (ev.action === 'comment') {
      const open = openByKey[k]
      if (open && ev.comment) {
        open.comments = [...(open.comments ?? []), { text: ev.comment, user: ev.comment_user, ts: ev.ts }]
      }
    } else if (ev.action === 'close') {
      const open = openByKey[k]
      if (open) {
        open.resolved_at = ev.ts
        open.duration = durationBetween(open.ts, ev.ts)
        delete openByKey[k]
      } else {
        // 짝 open 이 창 밖 — 해소 사실만 있는 행 (발생 시각 미상 표기)
        rows.push({ ...ev, resolved_at: ev.ts, preWindow: true })
      }
    }
  }
  return rows.sort((a, b) => (b.ts || '').localeCompare(a.ts || ''))
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

function DaysButtons({ days, onChange }: { days: number; onChange: (d: number) => void }) {
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

// 상세 항목 한 줄 — 값 없으면 렌더 생략
function DetailItem({ label, value }: { label: string; value?: string | null }) {
  if (!value) return null
  return (
    <div style={{ display: 'flex', gap: 8, fontSize: 12 }}>
      <span style={{ color: 'var(--text-muted)', minWidth: 90, flexShrink: 0 }}>{label}</span>
      <span>{value}</span>
    </div>
  )
}

// ── 알람 탭 ──────────────────────────────────────────────────────────────────
function AlarmsSection() {
  const { show } = useToast()
  const [events, setEvents] = useState<AlertEvent[]>([])
  const [summary, setSummary] = useState<AlertSummaryResponse | null>(null)
  const [days, setDays] = useState(7)
  const [sevFilter, setSevFilter] = useState('')
  const [codeFilter, setCodeFilter] = useState('')
  const [typeFilter, setTypeFilter] = useState('')
  const [q, setQ] = useState('')
  const [showResolved, setShowResolved] = useState(true)
  const [showStats, setShowStats] = useState(false)
  const [loading, setLoading] = useState(false)
  const [expanded, setExpanded] = useState<string | null>(null)
  const [visible, setVisible] = useState(PAGE_SIZE)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [list, sum] = await Promise.all([
        alertsApi.list({ days, limit: FETCH_LIMIT }),
        alertsApi.summary(days),
      ])
      setEvents(list.events)
      setSummary(sum)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [days, show])

  useEffect(() => { load() }, [load])
  useEffect(() => { setVisible(PAGE_SIZE) }, [days, sevFilter, codeFilter, typeFilter, q, showResolved])

  const allRows = useMemo(() => pairEvents(events), [events])
  const codes = useMemo(() => [...new Set(allRows.map(r => r.code).filter(Boolean) as string[])].sort(), [allRows])
  const types = useMemo(() => [...new Set(allRows.map(r => r.type).filter(Boolean))].sort(), [allRows])

  const rows = useMemo(() => {
    const needle = q.trim().toLowerCase()
    return allRows.filter(r => {
      if (!showResolved && r.resolved_at) return false
      if (sevFilter && severityOf(r) !== sevFilter) return false
      if (codeFilter && r.code !== codeFilter) return false
      if (typeFilter && r.type !== typeFilter) return false
      if (needle && ![r.code, r.type, r.message, r.source?.mo_instance, r.source?.detected_by]
        .some(v => (v || '').toLowerCase().includes(needle))) return false
      return true
    })
  }, [allRows, sevFilter, codeFilter, typeFilter, q, showResolved])

  const openCount = rows.filter(r => r.action === 'open' && !r.resolved_at).length
  const occurredCount = rows.filter(r => !r.preWindow).length

  const ackAlarm = useCallback(async (alarmId?: string) => {
    if (!alarmId) return
    try { await alertsApi.ack(alarmId); show('알람 승인됨', 'ok'); load() }
    catch (e) { show((e as Error).message, 'err') }
  }, [load, show])

  const commentAlarm = useCallback(async (alarmId: string | undefined, text: string) => {
    if (!alarmId) return
    try { await alertsApi.comment(alarmId, text); show('코멘트 기록됨', 'ok'); load() }
    catch (e) { show((e as Error).message, 'err') }
  }, [load, show])

  const exportCsv = () => {
    downloadCsv(`alarms_${days}d.csv`,
      ['발생', '해제', '지속(초)', '심각도', '코드', '클래스', '소스', '감지', '메시지', '재통지', '승인자'],
      rows.map(r => [
        r.ts, r.resolved_at || '', r.resolved_at ? Math.round((new Date(r.resolved_at).getTime() - new Date(r.ts).getTime()) / 1000) : '',
        severityOf(r), r.code || '', r.type, r.source?.mo_instance || '', r.source?.detected_by || '',
        r.message, r.occurrences ?? 1, r.ack_user || '',
      ]))
  }

  return (
    <>
      <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
        <DaysButtons days={days} onChange={setDays} />
        <div style={{ width: 1, height: 24, background: 'var(--border)', margin: '0 4px' }} />
        <select className="form-input" value={sevFilter} onChange={e => setSevFilter(e.target.value)} style={{ width: 110 }}>
          <option value="">심각도 전체</option>
          {['critical', 'major', 'minor', 'warning', 'indeterminate'].map(s => <option key={s} value={s}>{s}</option>)}
        </select>
        <select className="form-input" value={codeFilter} onChange={e => setCodeFilter(e.target.value)} style={{ width: 130 }}>
          <option value="">코드 전체</option>
          {codes.map(c => <option key={c} value={c}>{c}</option>)}
        </select>
        <select className="form-input" value={typeFilter} onChange={e => setTypeFilter(e.target.value)} style={{ width: 140 }}>
          <option value="">클래스 전체</option>
          {types.map(t => <option key={t} value={t}>{alarmTypeLabel(t)}</option>)}
        </select>
        <input className="search-input" style={{ width: 200 }} placeholder="소스/메시지 검색"
               value={q} onChange={e => setQ(e.target.value)} />
        <label style={{ display: 'flex', alignItems: 'center', gap: 4, fontSize: 13 }}>
          <input type="checkbox" checked={showResolved}
            onChange={e => setShowResolved(e.target.checked)} />
          해소 포함
        </label>
        <button className="btn btn--ghost btn--sm" onClick={exportCsv} style={{ marginLeft: 'auto' }}
                disabled={rows.length === 0}>CSV</button>
        <button className="btn btn--ghost btn--sm" onClick={load}>↻</button>
      </div>

      <div style={{ display: 'flex', gap: 12 }}>
        <div style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '12px 16px' }}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>기간 내 발생</div>
          <div style={{ fontSize: 24, fontWeight: 700 }}>{occurredCount}</div>
        </div>
        <div style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '12px 16px' }}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>미해소 (창 기준)</div>
          <div style={{ fontSize: 24, fontWeight: 700, color: openCount > 0 ? 'var(--danger)' : 'var(--text)' }}>{openCount}</div>
        </div>
        <div style={{ flex: 2, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: '12px 16px' }}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 6 }}>일별 발생량</div>
          <DailyBars data={summary?.daily || []} />
        </div>
      </div>

      {summary && summary.by_type.length > 0 && (
        <div className="panel">
          <button className="btn btn--ghost btn--sm"
                  style={{ width: '100%', textAlign: 'left', padding: '10px 16px', fontWeight: 600, fontSize: 13 }}
                  onClick={() => setShowStats(v => !v)}>
            {showStats ? '▾' : '▸'} 코드별 통계 (최근 {summary.days}일 · {summary.by_type.length}종)
          </button>
          {showStats && (
            <table className="data-table">
              <thead>
                <tr>
                  <th style={{ width: 110 }}>코드</th>
                  <th style={{ width: 130 }}>클래스</th>
                  <th style={{ width: 170 }}>소스</th>
                  <th style={{ width: 70, textAlign: 'right' }}>발생</th>
                  <th style={{ width: 70, textAlign: 'right' }}>해소</th>
                  <th style={{ width: 90 }}>현재 상태</th>
                  <th style={{ width: 130, textAlign: 'right' }}>평균 지속</th>
                  <th>마지막 이벤트</th>
                </tr>
              </thead>
              <tbody>
                {summary.by_type.map((s, i) => (
                  <tr key={s.key || `${s.type}-${i}`}>
                    <td style={{ fontFamily: 'monospace', fontSize: 11 }}>{s.code || '-'}</td>
                    <td>{alarmTypeLabel(s.type)}</td>
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
          )}
        </div>
      )}

      <div className="panel">
        <div style={{ padding: '12px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)' }}>
          알람 이력 ({rows.length}건)
          {events.length >= FETCH_LIMIT && (
            <span style={{ marginLeft: 8, fontSize: 11, fontWeight: 400, color: 'var(--danger)' }}>
              레코드 {FETCH_LIMIT}건 상한 도달 — 기간을 좁혀야 전체가 보입니다
            </span>
          )}
        </div>
        {loading ? (
          <div className="empty">로딩 중…</div>
        ) : rows.length === 0 ? (
          <div className="empty">기록된 알람 없음</div>
        ) : (
          <>
            <table className="data-table">
              <thead>
                <tr>
                  <th style={{ width: 90 }}>심각도</th>
                  <th style={{ width: 100 }}>코드</th>
                  <th style={{ width: 120 }}>클래스</th>
                  <th style={{ width: 160 }}>소스</th>
                  <th style={{ width: 80 }}>감지</th>
                  <th>메시지</th>
                  <th style={{ width: 145 }}>발생 시각</th>
                  <th style={{ width: 145 }}>해제 시각</th>
                  <th style={{ width: 90 }}>지속</th>
                </tr>
              </thead>
              <tbody>
                {rows.slice(0, visible).map((r, i) => {
                  const isOpen = r.action === 'open' && !r.resolved_at
                  const sev = severityOf(r)
                  const key = `${r.ts}-${r.alarm_id || r.type}-${i}`
                  const open = expanded === key
                  const lastChange = r.changes?.[r.changes.length - 1]
                  return [
                    <tr key={key} onClick={() => setExpanded(open ? null : key)}
                        style={{ cursor: 'pointer',
                                 background: open ? 'var(--hover)' : isOpen ? 'rgba(220, 53, 69, 0.08)' : undefined }}>
                      <td>
                        <span className={`badge ${sevBadgeClass(sev)}`}>{sev}</span>
                        {lastChange && (
                          <span title={`severity 변경 ${r.changes!.length}회 — 상세는 행 클릭`}
                                style={{ marginLeft: 4, fontSize: 11,
                                         color: lastChange.trend === 'moreSevere' ? 'var(--danger)' : 'var(--text-muted)' }}>
                            {lastChange.trend === 'moreSevere' ? '↑' : '↓'}{r.changes!.length}
                          </span>
                        )}
                      </td>
                      <td style={{ fontFamily: 'monospace', fontSize: 11 }}>{r.code || '-'}</td>
                      <td>{alarmTypeLabel(r.type)}</td>
                      <td><code style={{ fontSize: 11 }}>{r.source?.mo_instance || '-'}</code></td>
                      <td style={{ fontSize: 11, color: 'var(--text-muted)' }}>{r.source?.detected_by || '-'}</td>
                      <td>
                        {r.message}
                        {isOpen && <span style={{ marginLeft: 8, color: 'var(--danger)', fontSize: 11, fontWeight: 600 }}>OPEN</span>}
                        {(r.occurrences ?? 1) > 1 && (
                          <span style={{ marginLeft: 6, fontSize: 10, fontWeight: 600, color: 'var(--text-muted)',
                                         border: '1px solid var(--border)', borderRadius: 3, padding: '0 3px' }}
                                title={`해제 없이 ${r.occurrences}회 재통지 — 최근 ${r.last_open_ts ? fmtTime(r.last_open_ts) : ''}`}>
                            ×{r.occurrences}
                          </span>
                        )}
                        {(r.comments?.length ?? 0) > 0 && (
                          <span style={{ marginLeft: 6, fontSize: 11, color: 'var(--text-muted)' }}>💬{r.comments!.length}</span>
                        )}
                        {r.ack_state === 'acknowledged' && (
                          <span style={{ marginLeft: 6, fontSize: 11, color: 'var(--success, #16a34a)' }}>✓</span>
                        )}
                      </td>
                      <td className="ts">
                        {r.preWindow
                          ? <span style={{ color: 'var(--text-muted)' }}>창 이전</span>
                          : fmtTime(r.ts)}
                      </td>
                      <td className="ts">{r.resolved_at ? fmtTime(r.resolved_at) : '—'}</td>
                      <td>{r.duration || (isOpen ? '진행 중' : '-')}</td>
                    </tr>,
                    open && (
                      <tr key={`${key}-detail`}>
                        <td colSpan={9} style={{ padding: 0, background: 'var(--hover)' }}>
                          <AlarmHistoryDetail r={r} isOpen={isOpen} onAck={ackAlarm} onComment={commentAlarm} />
                        </td>
                      </tr>
                    ),
                  ]
                })}
              </tbody>
            </table>
            {rows.length > visible && (
              <div style={{ padding: 10, textAlign: 'center' }}>
                <button className="btn btn--ghost btn--sm" onClick={() => setVisible(v => v + PAGE_SIZE * 2)}>
                  더 보기 ({rows.length - visible}건 남음)
                </button>
              </div>
            )}
          </>
        )}
      </div>
    </>
  )
}

function AlarmHistoryDetail({ r, isOpen, onAck, onComment }: {
  r: AlertRow
  isOpen: boolean
  onAck: (id?: string) => void
  onComment: (id: string | undefined, text: string) => void
}) {
  const [text, setText] = useState('')
  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 6, padding: '10px 16px 12px' }}>
      <DetailItem label="alarm_id" value={r.alarm_id} />
      <DetailItem label="eventType" value={r.event_type} />
      <DetailItem label="probableCause" value={r.probable_cause} />
      <DetailItem label="영향" value={r.effect} />
      <DetailItem label="권장 조치" value={r.recommended_action} />
      {r.threshold_info && (
        <DetailItem label="관측값"
          value={`${r.threshold_info.observed}${r.threshold_info.unit || ''} (임계 ${r.threshold_info.threshold}${r.threshold_info.unit || ''})`} />
      )}
      {(r.occurrences ?? 1) > 1 && (
        <DetailItem label="재통지" value={`해제 없이 ${r.occurrences}회 — 최근 ${fmtTime(r.last_open_ts)}`} />
      )}
      {r.preWindow && <DetailItem label="비고" value="발생 시각이 조회 기간 밖 — 해소 기록만 표시" />}
      {(r.changes?.length ?? 0) > 0 && (
        <div style={{ fontSize: 12 }}>
          <div style={{ color: 'var(--text-muted)', marginBottom: 2 }}>severity 변경 이력</div>
          {r.changes!.map((c, i) => (
            <div key={i} style={{ padding: '2px 0 2px 8px', borderLeft: '2px solid var(--border)' }}>
              <span className="ts">{fmtTime(c.ts)}</span> — {c.from} → {c.to}
              {c.trend && (
                <span style={{ marginLeft: 6, color: c.trend === 'moreSevere' ? 'var(--danger)' : 'var(--text-muted)' }}>
                  ({c.trend === 'moreSevere' ? '승격' : '완화'})
                </span>
              )}
            </div>
          ))}
        </div>
      )}
      {r.ack_state === 'acknowledged' && (
        <DetailItem label="승인" value={`${r.ack_user || ''} ${r.ack_time ? fmtTime(r.ack_time) : ''}`} />
      )}
      {(r.comments?.length ?? 0) > 0 && (
        <div style={{ fontSize: 12 }}>
          <div style={{ color: 'var(--text-muted)', marginBottom: 2 }}>코멘트</div>
          {r.comments!.map((c, i) => (
            <div key={i} style={{ padding: '2px 0 2px 8px', borderLeft: '2px solid var(--border)' }}>
              <span style={{ color: 'var(--text-muted)' }}>{c.user || ''} {fmtTime(c.ts)}</span> — {c.text}
            </div>
          ))}
        </div>
      )}
      {isOpen && (
        <div style={{ display: 'flex', gap: 6, alignItems: 'center', marginTop: 2 }}>
          {r.ack_state !== 'acknowledged' && (
            <button className="btn btn--sm btn--outline" disabled={!r.alarm_id} onClick={() => onAck(r.alarm_id)}>승인</button>
          )}
          <input className="form-input" style={{ width: 280 }} placeholder="코멘트 입력 후 Enter"
                 value={text} onChange={e => setText(e.target.value)}
                 onKeyDown={e => {
                   if (e.key === 'Enter' && text.trim()) { onComment(r.alarm_id, text.trim()); setText('') }
                 }} />
        </div>
      )}
    </div>
  )
}

// ── 이벤트 탭 ────────────────────────────────────────────────────────────────
//   정상 동작 통지(stateChange/audit) 스트림 — 알람과 모델 분리(표준화 §3.6).
//   같은 (type, 소스, 분류) 의 연속 발생은 한 행으로 접는다 — 반복 통지가 스트림을
//   도배해도 다른 이벤트가 묻히지 않게. 펼치면 개별 통지를 보여준다.
interface EventGroup {
  key: string
  first: EventRecord            // 그룹 내 최신(목록이 최신순이므로 first=최근)
  last: EventRecord             // 그룹 내 최고(最古)
  items: EventRecord[]
}

function groupEvents(events: EventRecord[]): EventGroup[] {
  const sortedDesc = [...events].sort((a, b) => (b.ts || '').localeCompare(a.ts || ''))
  const groups: EventGroup[] = []
  for (const ev of sortedDesc) {
    const gk = `${ev.type}|${ev.kind || ''}|${ev.source?.mo_instance || ''}`
    const cur = groups[groups.length - 1]
    if (cur && cur.key === gk) {
      cur.items.push(ev)
      cur.last = ev
    } else {
      groups.push({ key: gk, first: ev, last: ev, items: [ev] })
    }
  }
  return groups
}

function EventsSection() {
  const { show } = useToast()
  const [events, setEvents] = useState<EventRecord[]>([])
  const [days, setDays] = useState(7)
  const [filterType, setFilterType] = useState('')
  const [filterKind, setFilterKind] = useState('')
  const [q, setQ] = useState('')
  const [loading, setLoading] = useState(false)
  const [expanded, setExpanded] = useState<string | null>(null)
  const [visible, setVisible] = useState(PAGE_SIZE)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const list = await eventsApi.list({ days, limit: FETCH_LIMIT })
      setEvents(list.events)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [days, show])

  useEffect(() => { load() }, [load])
  useEffect(() => { setVisible(PAGE_SIZE) }, [days, filterType, filterKind, q])

  const types = useMemo(() => [...new Set(events.map(e => e.type).filter(Boolean))].sort(), [events])

  const filtered = useMemo(() => {
    const needle = q.trim().toLowerCase()
    return events.filter(e => {
      if (filterType && e.type !== filterType) return false
      if (filterKind && e.kind !== filterKind) return false
      if (needle && ![e.code, e.type, e.message, e.source?.mo_instance]
        .some(v => (v || '').toLowerCase().includes(needle))) return false
      return true
    })
  }, [events, filterType, filterKind, q])

  const groups = useMemo(() => groupEvents(filtered), [filtered])

  const exportCsv = () => {
    downloadCsv(`events_${days}d.csv`,
      ['시각', '분류', '코드', '유형', '소스', '메시지'],
      filtered.map(e => [e.ts, e.kind || '', e.code || '', e.type, e.source?.mo_instance || '', e.message]))
  }

  return (
    <>
      <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
        <DaysButtons days={days} onChange={setDays} />
        <div style={{ width: 1, height: 24, background: 'var(--border)', margin: '0 4px' }} />
        <select className="form-input" value={filterKind} onChange={e => setFilterKind(e.target.value)} style={{ width: 120 }}>
          <option value="">분류 전체</option>
          <option value="stateChange">상태 변화</option>
          <option value="audit">감사</option>
        </select>
        <select className="form-input" value={filterType} onChange={e => setFilterType(e.target.value)} style={{ width: 160 }}>
          <option value="">유형 전체</option>
          {types.map(t => <option key={t} value={t}>{eventTypeLabel(t)}</option>)}
        </select>
        <input className="search-input" style={{ width: 200 }} placeholder="코드/소스/메시지 검색"
               value={q} onChange={e => setQ(e.target.value)} />
        <button className="btn btn--ghost btn--sm" onClick={exportCsv} style={{ marginLeft: 'auto' }}
                disabled={filtered.length === 0}>CSV</button>
        <button className="btn btn--ghost btn--sm" onClick={load}>↻</button>
      </div>

      <div className="panel">
        <div style={{ padding: '12px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)' }}>
          이벤트 이력 ({filtered.length}건 · {groups.length}묶음)
          {events.length >= FETCH_LIMIT && (
            <span style={{ marginLeft: 8, fontSize: 11, fontWeight: 400, color: 'var(--danger)' }}>
              레코드 {FETCH_LIMIT}건 상한 도달 — 기간을 좁혀야 전체가 보입니다
            </span>
          )}
        </div>
        {loading ? (
          <div className="empty">로딩 중…</div>
        ) : groups.length === 0 ? (
          <div className="empty">기록된 이벤트 없음</div>
        ) : (
          <>
            <table className="data-table">
              <thead>
                <tr>
                  <th style={{ width: 230 }}>시각</th>
                  <th style={{ width: 90 }}>분류</th>
                  <th style={{ width: 100 }}>코드</th>
                  <th style={{ width: 140 }}>유형</th>
                  <th style={{ width: 170 }}>소스</th>
                  <th>메시지</th>
                </tr>
              </thead>
              <tbody>
                {groups.slice(0, visible).map((g, gi) => {
                  const n = g.items.length
                  const key = `${g.key}-${g.first.ts}-${gi}`
                  const open = expanded === key
                  const ev = g.first
                  return [
                    <tr key={key} onClick={() => n > 1 && setExpanded(open ? null : key)}
                        style={{ cursor: n > 1 ? 'pointer' : undefined, background: open ? 'var(--hover)' : undefined }}>
                      <td className="ts">
                        {n > 1
                          ? <>{fmtTime(g.last.ts)} ~ {fmtTime(ev.ts)}</>
                          : fmtTime(ev.ts)}
                      </td>
                      <td>
                        <span className={`badge ${ev.kind === 'audit' ? 'badge--gray' : 'badge--blue'}`}>
                          {EVENT_KIND_LABEL[ev.kind || ''] || ev.kind || '-'}
                        </span>
                      </td>
                      <td style={{ fontFamily: 'monospace', fontSize: 11 }}>{ev.code || '-'}</td>
                      <td>{eventTypeLabel(ev.type)}</td>
                      <td><code style={{ fontSize: 11 }}>{ev.source?.mo_instance || '-'}</code></td>
                      <td title={ev.source?.detected_by}>
                        {ev.message}
                        {n > 1 && (
                          <span style={{ marginLeft: 6, fontSize: 10, fontWeight: 600, color: 'var(--text-muted)',
                                         border: '1px solid var(--border)', borderRadius: 3, padding: '0 3px' }}
                                title="연속 반복 — 클릭해 개별 통지 열람">
                            ×{n}
                          </span>
                        )}
                      </td>
                    </tr>,
                    open && (
                      <tr key={`${key}-detail`}>
                        <td colSpan={6} style={{ padding: 0, background: 'var(--hover)' }}>
                          <div style={{ padding: '8px 16px 10px', display: 'flex', flexDirection: 'column', gap: 2 }}>
                            {g.items.slice(0, 100).map((e2, i2) => (
                              <div key={i2} style={{ fontSize: 12, padding: '2px 0 2px 8px', borderLeft: '2px solid var(--border)' }}>
                                <span className="ts">{fmtTime(e2.ts)}</span> — {e2.message}
                                {e2.params && Object.keys(e2.params).length > 0 && (
                                  <code style={{ marginLeft: 8, fontSize: 11, color: 'var(--text-muted)' }}>
                                    {JSON.stringify(e2.params)}
                                  </code>
                                )}
                              </div>
                            ))}
                            {n > 100 && (
                              <div style={{ fontSize: 11, color: 'var(--text-muted)' }}>… 외 {n - 100}건 (CSV 로 전체 내보내기)</div>
                            )}
                          </div>
                        </td>
                      </tr>
                    ),
                  ]
                })}
              </tbody>
            </table>
            {groups.length > visible && (
              <div style={{ padding: 10, textAlign: 'center' }}>
                <button className="btn btn--ghost btn--sm" onClick={() => setVisible(v => v + PAGE_SIZE * 2)}>
                  더 보기 ({groups.length - visible}묶음 남음)
                </button>
              </div>
            )}
          </>
        )}
      </div>
    </>
  )
}

// ── 페이지 ───────────────────────────────────────────────────────────────────
export default function AlertsPage() {
  const [tab, setTab] = useState<'alarms' | 'events'>('alarms')
  return (
    <div className="page">
      <div className="tab-nav">
        <button className={`tab-btn ${tab === 'alarms' ? 'tab-btn--active' : ''}`}
                onClick={() => setTab('alarms')}>알람</button>
        <button className={`tab-btn ${tab === 'events' ? 'tab-btn--active' : ''}`}
                onClick={() => setTab('events')}>이벤트</button>
      </div>
      {tab === 'alarms' ? <AlarmsSection /> : <EventsSection />}
    </div>
  )
}
