// 알람·이벤트 통계 분석 — 기간 창의 코드별/유형별 집계·분포·추이 전용 뷰.
//   개별 라이프사이클 열람은 AlertsPage(이력), 활성 상태는 ActiveAlarmsPage 소관.
//   알람 탭은 /alerts/summary 집계만 사용(레코드 미수신 — 서버가 open/close 페어링),
//   이벤트 탭은 레코드를 받아 클라이언트에서 유형/소스별로 접는다.
//   화면 내 고정 레이아웃 — 지표/추이는 상단 고정, 분석 표는 내부 스크롤.
import { useState, useEffect, useCallback, useMemo, type ReactNode } from 'react'
import { alertsApi, eventsApi, type AlertSummaryByType, type EventRecord } from '../api/alerts'
import { useToast } from '../components/Toast'
import { DaysButtons } from '../components/ListControls'
import { SEV_COLOR, SEV_RANK } from '../widgets/useAlarms'
import {
  alarmTypeLabel, eventTypeLabel, EVENT_KIND_LABEL, sevBadgeClass,
  fmtTime, formatSec, downloadCsv,
} from '../utils/alarmLabels'

const FETCH_LIMIT = 5000   // 이벤트 탭 서버 상한 — 초과 시 최신순 절단(표기)

// ── 공통 표시 조각 ───────────────────────────────────────────────────────────
function Tile({ label, value, accent }: { label: string; value: ReactNode; accent?: boolean }) {
  return (
    <div style={{ flex: 1, minWidth: 110, background: 'var(--surface)', border: '1px solid var(--border)',
                  borderRadius: 'var(--radius)', padding: '10px 14px' }}>
      <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>{label}</div>
      <div style={{ fontSize: 22, fontWeight: 700, color: accent ? 'var(--danger)' : 'var(--text)' }}>{value}</div>
    </div>
  )
}

// 발생 비중 막대 + 수치 — 표 안 '발생' 열 공용
function ShareBar({ n, max, color = 'var(--primary)' }: { n: number; max: number; color?: string }) {
  const pct = max > 0 ? Math.round((n / max) * 100) : 0
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
      <div style={{ width: 64, height: 6, background: 'var(--border)', borderRadius: 3, flex: 'none' }}>
        <div style={{ width: `${pct}%`, height: '100%', background: color, borderRadius: 3, opacity: 0.85 }} />
      </div>
      <span>{n}</span>
    </div>
  )
}

function DailyBars({ data, height = 48 }: { data: { date: string; opens: number }[]; height?: number }) {
  if (data.length === 0) return <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>—</div>
  const max = Math.max(1, ...data.map(d => d.opens))
  return (
    <div style={{ display: 'flex', alignItems: 'flex-end', gap: 3, height: height + 14 }}>
      {data.map((d, i) => {
        const h = d.opens > 0 ? Math.max(2, Math.round((d.opens / max) * height)) : 1
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

// 심각도 분포 — 발생 건수 가중 누적 막대 + 범례
function SeverityDist({ bySev }: { bySev: Record<string, number> }) {
  const order = Object.keys(SEV_RANK).sort((a, b) => SEV_RANK[b] - SEV_RANK[a])
  const entries = order.filter(s => (bySev[s] || 0) > 0).map(s => [s, bySev[s]] as const)
  const total = entries.reduce((a, [, n]) => a + n, 0)
  if (total === 0) return <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>—</div>
  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
      <div style={{ display: 'flex', height: 10, borderRadius: 5, overflow: 'hidden' }}>
        {entries.map(([s, n]) => (
          <div key={s} title={`${s}: ${n}건`}
               style={{ width: `${(n / total) * 100}%`, background: SEV_COLOR[s] || 'var(--text-muted)' }} />
        ))}
      </div>
      <div style={{ display: 'flex', flexWrap: 'wrap', gap: '2px 12px', fontSize: 11, color: 'var(--text-muted)' }}>
        {entries.map(([s, n]) => (
          <span key={s} style={{ display: 'inline-flex', alignItems: 'center', gap: 4 }}>
            <span style={{ width: 8, height: 8, borderRadius: 2, background: SEV_COLOR[s] || 'var(--text-muted)' }} />
            {s} {n}
          </span>
        ))}
      </div>
    </div>
  )
}

// 분석 표를 담는 패널 골격 — 제목 고정 + 표 내부 스크롤
function TablePanel({ title, children }: { title: ReactNode; children: ReactNode }) {
  return (
    <div className="panel" style={{ flex: 1, minWidth: 0, minHeight: 0, display: 'flex', flexDirection: 'column' }}>
      <div style={{ padding: '10px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)', flex: 'none' }}>
        {title}
      </div>
      <div style={{ flex: 1, minHeight: 0, overflow: 'auto' }}>{children}</div>
    </div>
  )
}

// ── 알람 탭 ──────────────────────────────────────────────────────────────────
interface TypeAgg {
  type: string
  codes: Set<string>
  opens: number
  resolved: number
  open: number       // currently_open (코드·소스) 수
  last: string
}

function AlarmAnalysisSection() {
  const { show } = useToast()
  const [stats, setStats] = useState<AlertSummaryByType[]>([])
  const [daily, setDaily] = useState<{ date: string; opens: number }[]>([])
  const [days, setDays] = useState(7)
  const [loading, setLoading] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const sum = await alertsApi.summary(days)
      setStats(sum.by_type)
      setDaily(sum.daily)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [days, show])

  useEffect(() => { load() }, [load])

  const totals = useMemo(() => {
    let opens = 0, resolved = 0, open = 0, durSum = 0, durN = 0
    const bySev: Record<string, number> = {}
    for (const s of stats) {
      opens += s.opens
      resolved += s.resolved
      if (s.currently_open) open++
      if (s.avg_duration_sec != null && s.resolved > 0) { durSum += s.avg_duration_sec * s.resolved; durN += s.resolved }
      const sev = s.perceived_severity || 'indeterminate'
      bySev[sev] = (bySev[sev] || 0) + s.opens
    }
    return { opens, resolved, open, avgDur: durN > 0 ? durSum / durN : null, bySev }
  }, [stats])

  const byCode = useMemo(() =>
    [...stats].sort((a, b) => b.opens - a.opens || (b.last_ts || '').localeCompare(a.last_ts || '')), [stats])

  const byType = useMemo(() => {
    const m = new Map<string, TypeAgg>()
    for (const s of stats) {
      let t = m.get(s.type)
      if (!t) { t = { type: s.type, codes: new Set(), opens: 0, resolved: 0, open: 0, last: '' }; m.set(s.type, t) }
      if (s.code) t.codes.add(s.code)
      t.opens += s.opens
      t.resolved += s.resolved
      if (s.currently_open) t.open++
      if ((s.last_ts || '') > t.last) t.last = s.last_ts || ''
    }
    return [...m.values()].sort((a, b) => b.opens - a.opens || b.last.localeCompare(a.last))
  }, [stats])

  const maxCodeOpens = Math.max(1, ...byCode.map(s => s.opens))
  const maxTypeOpens = Math.max(1, ...byType.map(t => t.opens))

  const exportCsv = () => {
    downloadCsv(`alarm_stats_${days}d.csv`,
      ['코드', '클래스', '소스', '심각도', '발생', '해소', '현재 상태', '평균 지속(초)', '마지막 이벤트'],
      byCode.map(s => [
        s.code || '', s.type, s.mo_instance || '', s.perceived_severity || '',
        s.opens, s.resolved, s.currently_open ? 'OPEN' : '정상',
        s.avg_duration_sec != null ? Math.round(s.avg_duration_sec) : '', s.last_ts,
      ]))
  }

  return (
    <>
      <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8, flex: 'none' }}>
        <DaysButtons days={days} onChange={setDays} />
        <button className="btn btn--ghost btn--sm" onClick={exportCsv} style={{ marginLeft: 'auto' }}
                disabled={byCode.length === 0}>CSV</button>
        <button className="btn btn--ghost btn--sm" onClick={load}>↻</button>
      </div>

      <div style={{ display: 'flex', gap: 12, flex: 'none', flexWrap: 'wrap' }}>
        <Tile label="기간 내 발생" value={totals.opens} />
        <Tile label="해소" value={totals.resolved} />
        <Tile label="미해소 (코드·소스)" value={totals.open} accent={totals.open > 0} />
        <Tile label="평균 지속 (해소분)" value={totals.avgDur != null ? formatSec(Math.round(totals.avgDur)) : '—'} />
        <div style={{ flex: 2, minWidth: 220, background: 'var(--surface)', border: '1px solid var(--border)',
                      borderRadius: 'var(--radius)', padding: '10px 14px' }}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 6 }}>심각도 분포 (발생 기준)</div>
          <SeverityDist bySev={totals.bySev} />
        </div>
      </div>

      <div style={{ flex: 'none', background: 'var(--surface)', border: '1px solid var(--border)',
                    borderRadius: 'var(--radius)', padding: '10px 16px' }}>
        <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 6 }}>일별 발생량</div>
        <DailyBars data={daily} />
      </div>

      {loading ? (
        <div className="empty">로딩 중…</div>
      ) : (
        <div style={{ display: 'flex', gap: 16, flex: 1, minHeight: 0 }}>
          <div style={{ flex: 3, minWidth: 0, display: 'flex' }}>
            <TablePanel title={<>코드별 분석 ({byCode.length}종)</>}>
              {byCode.length === 0 ? <div className="empty">기간 내 알람 없음</div> : (
                <table className="data-table">
                  <thead>
                    <tr>
                      <th style={{ width: 110 }}>코드</th>
                      <th style={{ width: 120 }}>클래스</th>
                      <th style={{ width: 160 }}>소스</th>
                      <th style={{ width: 95 }}>심각도</th>
                      <th style={{ width: 130 }}>발생</th>
                      <th style={{ width: 60, textAlign: 'right' }}>해소</th>
                      <th style={{ width: 80 }}>현재 상태</th>
                      <th style={{ width: 110, textAlign: 'right' }}>평균 지속</th>
                      <th>마지막 이벤트</th>
                    </tr>
                  </thead>
                  <tbody>
                    {byCode.map((s, i) => (
                      <tr key={s.key || `${s.type}-${i}`}>
                        <td style={{ fontFamily: 'monospace', fontSize: 11 }}>{s.code || '-'}</td>
                        <td>{alarmTypeLabel(s.type)}</td>
                        <td><code style={{ fontSize: 11 }}>{s.mo_instance || '-'}</code></td>
                        <td>
                          {s.perceived_severity
                            ? <span className={`badge ${sevBadgeClass(s.perceived_severity)}`}>{s.perceived_severity}</span>
                            : '-'}
                        </td>
                        <td><ShareBar n={s.opens} max={maxCodeOpens} /></td>
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
            </TablePanel>
          </div>
          <div style={{ flex: 2, minWidth: 0, display: 'flex' }}>
            <TablePanel title={<>유형(클래스)별 분석 ({byType.length}종)</>}>
              {byType.length === 0 ? <div className="empty">기간 내 알람 없음</div> : (
                <table className="data-table">
                  <thead>
                    <tr>
                      <th>클래스</th>
                      <th style={{ width: 70, textAlign: 'right' }}>코드 수</th>
                      <th style={{ width: 130 }}>발생</th>
                      <th style={{ width: 60, textAlign: 'right' }}>해소</th>
                      <th style={{ width: 70, textAlign: 'right' }}>미해소</th>
                      <th style={{ width: 145 }}>마지막</th>
                    </tr>
                  </thead>
                  <tbody>
                    {byType.map(t => (
                      <tr key={t.type}>
                        <td>{alarmTypeLabel(t.type)}
                          <span style={{ marginLeft: 6, fontSize: 11, color: 'var(--text-muted)', fontFamily: 'monospace' }}>{t.type}</span>
                        </td>
                        <td style={{ textAlign: 'right' }}>{t.codes.size || '-'}</td>
                        <td><ShareBar n={t.opens} max={maxTypeOpens} /></td>
                        <td style={{ textAlign: 'right' }}>{t.resolved}</td>
                        <td style={{ textAlign: 'right', color: t.open > 0 ? 'var(--danger)' : undefined }}>{t.open}</td>
                        <td className="ts">{t.last ? fmtTime(t.last) : '-'}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              )}
            </TablePanel>
          </div>
        </div>
      )}
    </>
  )
}

// ── 이벤트 탭 ────────────────────────────────────────────────────────────────
interface EventAgg { key: string; kind?: string; code?: string; type: string; count: number; last: string }
interface SourceAgg { source: string; count: number; last: string }

function EventAnalysisSection() {
  const { show } = useToast()
  const [events, setEvents] = useState<EventRecord[]>([])
  const [days, setDays] = useState(7)
  const [loading, setLoading] = useState(false)

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

  const byKind = useMemo(() => {
    const m: Record<string, number> = {}
    for (const e of events) m[e.kind || '-'] = (m[e.kind || '-'] || 0) + 1
    return m
  }, [events])

  const byType = useMemo(() => {
    const m = new Map<string, EventAgg>()
    for (const e of events) {
      const k = `${e.kind || ''}|${e.code || ''}|${e.type}`
      let t = m.get(k)
      if (!t) { t = { key: k, kind: e.kind, code: e.code, type: e.type, count: 0, last: '' }; m.set(k, t) }
      t.count++
      if ((e.ts || '') > t.last) t.last = e.ts || ''
    }
    return [...m.values()].sort((a, b) => b.count - a.count || b.last.localeCompare(a.last))
  }, [events])

  const bySource = useMemo(() => {
    const m = new Map<string, SourceAgg>()
    for (const e of events) {
      const src = e.source?.mo_instance || '-'
      let t = m.get(src)
      if (!t) { t = { source: src, count: 0, last: '' }; m.set(src, t) }
      t.count++
      if ((e.ts || '') > t.last) t.last = e.ts || ''
    }
    return [...m.values()].sort((a, b) => b.count - a.count || b.last.localeCompare(a.last))
  }, [events])

  const daily = useMemo(() => {
    const m: Record<string, number> = {}
    for (const e of events) {
      const d = (e.ts || '').slice(0, 10)
      if (d) m[d] = (m[d] || 0) + 1
    }
    return Object.entries(m).sort(([a], [b]) => a.localeCompare(b)).map(([date, opens]) => ({ date, opens }))
  }, [events])

  const maxTypeCount = Math.max(1, ...byType.map(t => t.count))
  const maxSrcCount = Math.max(1, ...bySource.map(t => t.count))

  const exportCsv = () => {
    downloadCsv(`event_stats_${days}d.csv`,
      ['분류', '코드', '유형', '건수', '마지막'],
      byType.map(t => [EVENT_KIND_LABEL[t.kind || ''] || t.kind || '', t.code || '', t.type, t.count, t.last]))
  }

  return (
    <>
      <div className="toolbar" style={{ flexWrap: 'wrap', gap: 8, flex: 'none' }}>
        <DaysButtons days={days} onChange={setDays} />
        {events.length >= FETCH_LIMIT && (
          <span style={{ fontSize: 11, color: 'var(--danger)' }}>
            레코드 {FETCH_LIMIT}건 상한 도달 — 기간을 좁혀야 전체가 집계됩니다
          </span>
        )}
        <button className="btn btn--ghost btn--sm" onClick={exportCsv} style={{ marginLeft: 'auto' }}
                disabled={byType.length === 0}>CSV</button>
        <button className="btn btn--ghost btn--sm" onClick={load}>↻</button>
      </div>

      <div style={{ display: 'flex', gap: 12, flex: 'none', flexWrap: 'wrap' }}>
        <Tile label="총 통지" value={events.length} />
        <Tile label="상태 변화" value={byKind['stateChange'] || 0} />
        <Tile label="감사" value={byKind['audit'] || 0} />
        <Tile label="유형 수" value={byType.length} />
        <div style={{ flex: 3, minWidth: 260, background: 'var(--surface)', border: '1px solid var(--border)',
                      borderRadius: 'var(--radius)', padding: '10px 14px' }}>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 6 }}>일별 통지량</div>
          <DailyBars data={daily} height={30} />
        </div>
      </div>

      {loading ? (
        <div className="empty">로딩 중…</div>
      ) : (
        <div style={{ display: 'flex', gap: 16, flex: 1, minHeight: 0 }}>
          <div style={{ flex: 3, minWidth: 0, display: 'flex' }}>
            <TablePanel title={<>유형별 발생 ({byType.length}종)</>}>
              {byType.length === 0 ? <div className="empty">기간 내 이벤트 없음</div> : (
                <table className="data-table">
                  <thead>
                    <tr>
                      <th style={{ width: 90 }}>분류</th>
                      <th style={{ width: 110 }}>코드</th>
                      <th>유형</th>
                      <th style={{ width: 140 }}>건수</th>
                      <th style={{ width: 145 }}>마지막</th>
                    </tr>
                  </thead>
                  <tbody>
                    {byType.map(t => (
                      <tr key={t.key}>
                        <td>
                          <span className={`badge ${t.kind === 'audit' ? 'badge--gray' : 'badge--blue'}`}>
                            {EVENT_KIND_LABEL[t.kind || ''] || t.kind || '-'}
                          </span>
                        </td>
                        <td style={{ fontFamily: 'monospace', fontSize: 11 }}>{t.code || '-'}</td>
                        <td>{eventTypeLabel(t.type)}
                          <span style={{ marginLeft: 6, fontSize: 11, color: 'var(--text-muted)', fontFamily: 'monospace' }}>{t.type}</span>
                        </td>
                        <td><ShareBar n={t.count} max={maxTypeCount} /></td>
                        <td className="ts">{t.last ? fmtTime(t.last) : '-'}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              )}
            </TablePanel>
          </div>
          <div style={{ flex: 2, minWidth: 0, display: 'flex' }}>
            <TablePanel title={<>소스별 발생 ({bySource.length}곳)</>}>
              {bySource.length === 0 ? <div className="empty">기간 내 이벤트 없음</div> : (
                <table className="data-table">
                  <thead>
                    <tr>
                      <th>소스</th>
                      <th style={{ width: 140 }}>건수</th>
                      <th style={{ width: 145 }}>마지막</th>
                    </tr>
                  </thead>
                  <tbody>
                    {bySource.map(t => (
                      <tr key={t.source}>
                        <td><code style={{ fontSize: 11 }}>{t.source}</code></td>
                        <td><ShareBar n={t.count} max={maxSrcCount} /></td>
                        <td className="ts">{t.last ? fmtTime(t.last) : '-'}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              )}
            </TablePanel>
          </div>
        </div>
      )}
    </>
  )
}

// ── 페이지 ───────────────────────────────────────────────────────────────────
export default function AlarmAnalysisPage() {
  const [tab, setTab] = useState<'alarms' | 'events'>('alarms')
  return (
    <div className="page" style={{ height: 'calc(100vh - 135px)', minHeight: 520 }}>
      <div className="tab-nav" style={{ flex: 'none' }}>
        <button className={`tab-btn ${tab === 'alarms' ? 'tab-btn--active' : ''}`}
                onClick={() => setTab('alarms')}>알람</button>
        <button className={`tab-btn ${tab === 'events' ? 'tab-btn--active' : ''}`}
                onClick={() => setTab('events')}>이벤트</button>
      </div>
      {tab === 'alarms' ? <AlarmAnalysisSection /> : <EventAnalysisSection />}
    </div>
  )
}
