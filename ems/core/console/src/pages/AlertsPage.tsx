// 알람·이벤트 이력 — 기록 탐색기 (alarm_pipeline.md §8.3 판독 규율로 open/close 페어링).
//   활성 알람 뷰는 ActiveAlarmsPage(store 라이브), 코드 사전·평가 규칙은 AlarmCatalogPage,
//   코드별/유형별 집계·분포는 AlarmAnalysisPage 소관 — 여기는 기간 창 안의 알람
//   라이프사이클(발생→변경→해소)과 이벤트 스트림 열람 전용.
//   **화면 전체가 카드 하나**(`core.alarm-event-history`)다 — 전환 탭·기간 선택·이력 표는 따로
//   떼면 말이 되지 않는 한 조작 단위(탭을 고르고 기간을 좁혀 표를 읽는다)라 함께 묶는다.
//   카드 **안**은 바깥과 같은 48×48 셀 배치라 블록이 각각 위젯이고 재배치할 수 있다(§3.0.1).
//   알람/이벤트 전환은 배치의 `visibleWhen`(파라미터 `atab`)이 판정한다.
//   조건은 그대로 페이지 파라미터(`atab`/`days`)여서 딥링크가 화면을 재현한다.
//   목록은 화면 내 고정 높이 + 페이지 내비게이션(Pager)으로 넘긴다 — 페이지 스크롤 누적 없음.
//   필터는 전부 클라이언트에서 건다 — 서버 type 필터는 type 필드가 없는 ack/comment
//   레코드를 떨어뜨려 승인·코멘트 표시가 소실되기 때문(전 레코드 수신 후 행 단위 필터).
import { useState, useCallback, useMemo } from 'react'
import { alertsApi, eventsApi, type AlertEvent, type EventRecord } from '../api/alerts'
import { useToast } from '../components/Toast'
import { DaysButtons, Pager } from '../components/ListControls'
import { makeSharedByKey } from '../widgets/sharedFetch'
import { usePageParam } from '../widgets/pageParams'
import { alertsFilter, useAlarmFilter, useEventFilter } from './alertsHistoryStore'
import {
  alarmTypeLabel, eventTypeLabel, EVENT_KIND_LABEL, sevBadgeClass, severityOf,
  fmtTime, durationBetween, downloadCsv,
} from '../utils/alarmLabels'

const PAGE_SIZE = 20
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
// ── 공유 조회 ────────────────────────────────────────────────────────────────
// 필터 블록과 표 블록이 **같은 응답**을 봐야 하므로 days 를 키로 공유한다(요청 1회).
const useAlertsRaw = makeSharedByKey(k => alertsApi.list({ days: Number(k), limit: FETCH_LIMIT }))
const useEventsRaw = makeSharedByKey(k => eventsApi.list({ days: Number(k), limit: FETCH_LIMIT }))

function useAlarmHistory() {
  // 기간은 이 화면이 소유하지 않는다 — 기간 선택 컨트롤이 쓰는 페이지 파라미터를 읽는다.
  const days = Number(usePageParam('days')[0]) || 7
  const { data, loading, error, reload } = useAlertsRaw(String(days))
  const events = useMemo(() => data?.events ?? [], [data])
  const allRows = useMemo(() => pairEvents(events), [events])
  const f = useAlarmFilter()
  const rows = useMemo(() => {
    const needle = f.q.trim().toLowerCase()
    return allRows.filter(r => {
      if (!f.showResolved && r.resolved_at) return false
      if (f.sev && severityOf(r) !== f.sev) return false
      if (f.code && r.code !== f.code) return false
      if (f.type && r.type !== f.type) return false
      if (needle && ![r.code, r.type, r.message, r.source?.mo_instance, r.source?.mo_label, r.source?.detected_by]
        .some(v => (v || '').toLowerCase().includes(needle))) return false
      return true
    })
  }, [allRows, f])
  return { days, events, allRows, rows, f, loading, error, reload }
}

// ── 알람 조회 조건 ───────────────────────────────────────────────────────────
// 기간 선택 옆줄에 놓는 컨트롤 — 표는 아래 공간을 전부 쓴다.
export function AlarmHistoryFilter() {
  const { days, allRows, rows, f, reload } = useAlarmHistory()
  const setDays = usePageParam('days')[1]
  const codes = useMemo(() => [...new Set(allRows.map(r => r.code).filter(Boolean) as string[])].sort(), [allRows])
  const types = useMemo(() => [...new Set(allRows.map(r => r.type).filter(Boolean))].sort(), [allRows])
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
    <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
      {/* 기간과 필터는 **한 줄 한 블록** — 조회 조건이 두 덩어리로 갈려 보이지 않게. */}
      <DaysButtons days={days} onChange={d => setDays(String(d))} />
      <span style={{ width: 1, alignSelf: 'stretch', margin: '0 4px', background: 'var(--border)' }} />
      <select className="form-input" value={f.sev} style={{ width: 108 }}
              onChange={e => alertsFilter.setAlarm({ sev: e.target.value })}>
        <option value="">심각도 전체</option>
        {['critical', 'major', 'minor', 'warning', 'indeterminate'].map(s => <option key={s} value={s}>{s}</option>)}
      </select>
      <select className="form-input" value={f.code} style={{ width: 124 }}
              onChange={e => alertsFilter.setAlarm({ code: e.target.value })}>
        <option value="">코드 전체</option>
        {codes.map(c => <option key={c} value={c}>{c}</option>)}
      </select>
      <select className="form-input" value={f.type} style={{ width: 132 }}
              onChange={e => alertsFilter.setAlarm({ type: e.target.value })}>
        <option value="">클래스 전체</option>
        {types.map(t => <option key={t} value={t}>{alarmTypeLabel(t)}</option>)}
      </select>
      <input className="search-input" style={{ width: 170 }} placeholder="소스/메시지 검색"
             value={f.q} onChange={e => alertsFilter.setAlarm({ q: e.target.value })} />
      <label style={{ display: 'flex', alignItems: 'center', gap: 4, fontSize: 13, whiteSpace: 'nowrap' }}>
        <input type="checkbox" checked={f.showResolved}
               onChange={e => alertsFilter.setAlarm({ showResolved: e.target.checked })} />
        해소 포함
      </label>
      <button className="btn btn--ghost btn--sm" onClick={exportCsv} disabled={rows.length === 0}>CSV</button>
      <button className="btn btn--ghost btn--sm" onClick={reload}>↻</button>
    </div>
  )
}

// ── 알람 이력 표 ─────────────────────────────────────────────────────────────
export function AlarmsSection() {
  const { show } = useToast()
  const { events, rows, f, loading, reload } = useAlarmHistory()
  const [expanded, setExpanded] = useState<string | null>(null)

  const openCount = rows.filter(r => r.action === 'open' && !r.resolved_at).length
  const pageStart = Math.min(f.page, Math.max(0, Math.ceil(rows.length / PAGE_SIZE) - 1)) * PAGE_SIZE
  const pageRows = rows.slice(pageStart, pageStart + PAGE_SIZE)

  const ackAlarm = useCallback(async (alarmId?: string) => {
    if (!alarmId) return
    try { await alertsApi.ack(alarmId); show('알람 승인됨', 'ok'); reload() }
    catch (e) { show((e as Error).message, 'err') }
  }, [reload, show])

  const commentAlarm = useCallback(async (alarmId: string | undefined, text: string) => {
    if (!alarmId) return
    try { await alertsApi.comment(alarmId, text); show('코멘트 기록됨', 'ok'); reload() }
    catch (e) { show((e as Error).message, 'err') }
  }, [reload, show])

  return (
      <div className="panel" style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column' }}>
        <div style={{ padding: '10px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)', flex: 'none' }}>
          알람 이력 ({rows.length}건{openCount > 0 && <span style={{ color: 'var(--danger)' }}> · 미해소 {openCount}</span>})
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
            <div className="scroll-fill">
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
                {pageRows.map((r, i) => {
                  const isOpen = r.action === 'open' && !r.resolved_at
                  const sev = severityOf(r)
                  const key = `${r.ts}-${r.alarm_id || r.type}-${pageStart + i}`
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
                      <td><code style={{ fontSize: 11 }} title={r.source?.mo_instance || ''}>
                        {r.source?.mo_label || r.source?.mo_instance || '-'}</code></td>
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
                          <span style={{ marginLeft: 6, fontSize: 11, color: 'var(--success)' }}>✓</span>
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
            </div>
            <Pager page={f.page} count={rows.length} pageSize={PAGE_SIZE}
                   onPage={pg => alertsFilter.setAlarm({ page: pg })} />
          </>
        )}
      </div>
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

function useEventHistory() {
  const days = Number(usePageParam('days')[0]) || 7
  const { data, loading, error, reload } = useEventsRaw(String(days))
  const events = useMemo(() => data?.events ?? [], [data])
  const f = useEventFilter()
  const filtered = useMemo(() => {
    const needle = f.q.trim().toLowerCase()
    return events.filter(e => {
      if (f.type && e.type !== f.type) return false
      if (f.kind && e.kind !== f.kind) return false
      if (needle && ![e.code, e.type, e.message, e.source?.mo_instance]
        .some(v => (v || '').toLowerCase().includes(needle))) return false
      return true
    })
  }, [events, f])
  const groups = useMemo(() => groupEvents(filtered), [filtered])
  return { days, events, filtered, groups, f, loading, error, reload }
}

// ── 이벤트 조회 조건 ─────────────────────────────────────────────────────────
export function EventHistoryFilter() {
  const { days, events, filtered, f, reload } = useEventHistory()
  const setDays = usePageParam('days')[1]
  const types = useMemo(() => [...new Set(events.map(e => e.type).filter(Boolean))].sort(), [events])
  const exportCsv = () => {
    downloadCsv(`events_${days}d.csv`,
      ['시각', '분류', '코드', '유형', '소스', '메시지'],
      filtered.map(e => [e.ts, e.kind || '', e.code || '', e.type, e.source?.mo_instance || '', e.message]))
  }
  return (
    <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8 }}>
      <DaysButtons days={days} onChange={d => setDays(String(d))} />
      <span style={{ width: 1, alignSelf: 'stretch', margin: '0 4px', background: 'var(--border)' }} />
      <select className="form-input" value={f.kind} style={{ width: 116 }}
              onChange={e => alertsFilter.setEvent({ kind: e.target.value })}>
        <option value="">분류 전체</option>
        <option value="stateChange">상태 변화</option>
        <option value="audit">감사</option>
      </select>
      <select className="form-input" value={f.type} style={{ width: 152 }}
              onChange={e => alertsFilter.setEvent({ type: e.target.value })}>
        <option value="">유형 전체</option>
        {types.map(t => <option key={t} value={t}>{eventTypeLabel(t)}</option>)}
      </select>
      <input className="search-input" style={{ width: 180 }} placeholder="코드/소스/메시지 검색"
             value={f.q} onChange={e => alertsFilter.setEvent({ q: e.target.value })} />
      <button className="btn btn--ghost btn--sm" onClick={exportCsv} disabled={filtered.length === 0}>CSV</button>
      <button className="btn btn--ghost btn--sm" onClick={reload}>↻</button>
    </div>
  )
}

// ── 이벤트 이력 표 ───────────────────────────────────────────────────────────
export function EventsSection() {
  const { events, filtered, groups, f, loading } = useEventHistory()
  const [expanded, setExpanded] = useState<string | null>(null)
  const pageStart = Math.min(f.page, Math.max(0, Math.ceil(groups.length / PAGE_SIZE) - 1)) * PAGE_SIZE
  const pageGroups = groups.slice(pageStart, pageStart + PAGE_SIZE)

  return (
      <div className="panel" style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column' }}>
        <div style={{ padding: '10px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)', flex: 'none' }}>
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
            <div className="scroll-fill">
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
                {pageGroups.map((g, gi) => {
                  const n = g.items.length
                  const key = `${g.key}-${g.first.ts}-${pageStart + gi}`
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
            </div>
            <Pager page={f.page} count={groups.length} pageSize={PAGE_SIZE} unit="묶음"
                   onPage={pg => alertsFilter.setEvent({ page: pg })} />
          </>
        )}
      </div>
  )
}
