// 알람·이벤트 통계 분석 블록 — 기간 창의 코드별/유형별 집계·분포·추이.
//   개별 라이프사이클 열람은 AlertsPage(이력), 활성 상태는 활성 알람 위젯 소관.
//   알람은 /alerts/summary 집계만 사용(레코드 미수신 — 서버가 open/close 페어링),
//   이벤트는 레코드를 받아 클라이언트에서 유형/소스별로 접는다.
//
// **화면 전체가 카드 하나**(`core.alarm-event-analysis`)다 — 전환 탭·기간 선택·블록들은 같은 기간
// 창을 여러 각도(요약 / 분포 / 추이 / 코드별 / 유형별)에서 보는 한 벌이라 함께 묶는다.
// 다만 카드 **안**은 바깥과 같은 48×48 셀 배치라(console_platform §3.0.1) 블록은 각각 위젯으로
// 등록돼 있고, 운영자가 카드 안에서 재배치·추가·제거할 수 있다.
// 알람/이벤트 전환은 배치의 `visibleWhen`(파라미터 `atab`)이 판정한다 — 카드 안에서도 같은 규칙.
// 조회 일수는 페이지 파라미터 `days` 로 두므로 딥링크가 화면을 재현하고,
// 같은 days 를 보는 블록이 여러 개여도 조회는 **1회**만 나간다(아래 공유 로더).
import { useMemo, type ReactNode } from 'react'
import { alertsApi, eventsApi, type AlertSummaryByType, type EventRecord } from '../api/alerts'
import { usePageParam } from '../widgets/pageParams'
import { makeSharedByKey } from '../widgets/sharedFetch'
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
function TablePanel({ title, action, children }: { title: ReactNode; action?: ReactNode; children: ReactNode }) {
  return (
    <div className="panel" style={{ flex: 1, minWidth: 0, minHeight: 0, display: 'flex', flexDirection: 'column' }}>
      <div style={{ padding: '10px 16px', fontWeight: 600, fontSize: 14, borderBottom: '1px solid var(--border)',
                    flex: 'none', display: 'flex', alignItems: 'center', gap: 8 }}>
        {title}
        {action && <span style={{ marginLeft: 'auto', fontWeight: 400 }}>{action}</span>}
      </div>
      <div className="scroll-fill">{children}</div>
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

interface TypeAgg { type: string; codes: Set<string>; opens: number; resolved: number; open: number; last: string }
interface EventAgg { key: string; kind?: string; code?: string; type: string; count: number; last: string }
interface SourceAgg { source: string; count: number; last: string }

const useAlarmSummaryRaw = makeSharedByKey(k => alertsApi.summary(Number(k)))
const useEventListRaw = makeSharedByKey(k => eventsApi.list({ days: Number(k), limit: FETCH_LIMIT }))

// 조회 일수 — 아직 아무도 고르지 않았으면 기본 7일.
function useDays(): string { return usePageParam('days')[0] || '7' }

function useAlarmSummary() {
  const days = useDays()
  const { data, loading, error, reload } = useAlarmSummaryRaw(days)
  const stats: AlertSummaryByType[] = useMemo(() => data?.by_type ?? [], [data])
  const daily = useMemo(() => data?.daily ?? [], [data])
  return { days, stats, daily, loading, error, reload }
}
function useEventList() {
  const days = useDays()
  const { data, loading, error, reload } = useEventListRaw(days)
  const events: EventRecord[] = useMemo(() => data?.events ?? [], [data])
  return { days, events, loading, error, reload }
}

// 블록 공통 껍데기 — 제목 + 오류/로딩 표기. 루트가 `.panel`(flex:1)이라 담긴 칸을 채운다.
function Block({ title, loading, error, children, pad = true }: {
  title?: ReactNode; loading?: boolean; error?: string; children: ReactNode; pad?: boolean
}) {
  return (
    <div className="panel" style={{ padding: pad ? '10px 16px' : 0, display: 'flex', flexDirection: 'column', minHeight: 0 }}>
      {title && (
        <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 6, flex: 'none',
                      padding: pad ? 0 : '10px 16px 6px' }}>
          {title}
          {loading && <span style={{ marginLeft: 6 }}>· 갱신 중…</span>}
          {error && <span style={{ marginLeft: 6, color: 'var(--danger)' }}>· 조회 실패</span>}
        </div>
      )}
      {children}
    </div>
  )
}

// ── 알람 블록 ────────────────────────────────────────────────────────────────
function useAlarmTotals(stats: AlertSummaryByType[]) {
  return useMemo(() => {
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
}

// 요약 타일 — 같은 기간의 한 묶음이라 타일끼리는 쪼개지 않는다.
export function AlarmTotalsBlock() {
  const { stats, loading, error } = useAlarmSummary()
  const totals = useAlarmTotals(stats)
  return (
    <Block loading={loading} error={error}>
      <div style={{ display: 'flex', gap: 12, flexWrap: 'wrap' }}>
        <Tile label="기간 내 발생" value={totals.opens} />
        <Tile label="해소" value={totals.resolved} />
        <Tile label="미해소 (코드·소스)" value={totals.open} accent={totals.open > 0} />
        <Tile label="평균 지속 (해소분)" value={totals.avgDur != null ? formatSec(Math.round(totals.avgDur)) : '—'} />
      </div>
    </Block>
  )
}

export function AlarmSeverityDistBlock() {
  const { stats, loading, error } = useAlarmSummary()
  const totals = useAlarmTotals(stats)
  return (
    <Block title="심각도 분포 (발생 기준)" loading={loading} error={error}>
      <SeverityDist bySev={totals.bySev} />
    </Block>
  )
}

export function AlarmDailyBlock() {
  const { daily, loading, error } = useAlarmSummary()
  return (
    <Block title="일별 발생량" loading={loading} error={error}>
      <DailyBars data={daily} />
    </Block>
  )
}

export function AlarmByCodeBlock() {
  const { days, stats, loading, error } = useAlarmSummary()
  const byCode = useMemo(() =>
    [...stats].sort((a, b) => b.opens - a.opens || (b.last_ts || '').localeCompare(a.last_ts || '')), [stats])
  const maxCodeOpens = Math.max(1, ...byCode.map(s => s.opens))
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
      {loading && byCode.length === 0 ? <div className="panel"><div className="empty">로딩 중…</div></div>
        : error ? <div className="panel"><div className="empty" style={{ color: 'var(--danger)' }}>조회 실패: {error}</div></div> : (
            <TablePanel title={<>코드별 분석 ({byCode.length}종)</>}
                        action={<button className="btn btn--ghost btn--sm" onClick={exportCsv}
                                        disabled={byCode.length === 0}>CSV</button>}>
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
      )}
    </>
  )
}

export function AlarmByTypeBlock() {
  const { stats, loading, error } = useAlarmSummary()
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
  const maxTypeOpens = Math.max(1, ...byType.map(t => t.opens))
  if (loading && byType.length === 0) return <div className="panel"><div className="empty">로딩 중…</div></div>
  if (error) return <div className="panel"><div className="empty" style={{ color: 'var(--danger)' }}>조회 실패: {error}</div></div>
  return (
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
  )
}

// ── 이벤트 블록 ──────────────────────────────────────────────────────────────
function useEventAggs() {
  const { days, events, loading, error } = useEventList()
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
  return { days, events, byKind, byType, bySource, daily, loading, error }
}

export function EventTotalsBlock() {
  const { events, byKind, byType, loading, error } = useEventAggs()
  return (
    <Block loading={loading} error={error}>
      <div style={{ display: 'flex', gap: 12, flexWrap: 'wrap' }}>
        <Tile label="총 통지" value={events.length} />
        <Tile label="상태 변화" value={byKind['stateChange'] || 0} />
        <Tile label="감사" value={byKind['audit'] || 0} />
        <Tile label="유형 수" value={byType.length} />
      </div>
      {events.length >= FETCH_LIMIT && (
        <div style={{ fontSize: 11, color: 'var(--danger)', marginTop: 6 }}>
          최신 {FETCH_LIMIT}건만 집계 (기간을 좁히세요)
        </div>
      )}
    </Block>
  )
}

export function EventDailyBlock() {
  const { daily, loading, error } = useEventAggs()
  return (
    <Block title="일별 통지량" loading={loading} error={error}>
      <DailyBars data={daily} height={30} />
    </Block>
  )
}

export function EventByTypeBlock() {
  const { days, byType, loading, error } = useEventAggs()
  const maxTypeCount = Math.max(1, ...byType.map(t => t.count))
  const exportCsv = () => {
    downloadCsv(`event_stats_${days}d.csv`,
      ['분류', '코드', '유형', '건수', '마지막'],
      byType.map(t => [EVENT_KIND_LABEL[t.kind || ''] || t.kind || '', t.code || '', t.type, t.count, t.last]))
  }
  if (loading && byType.length === 0) return <div className="panel"><div className="empty">로딩 중…</div></div>
  if (error) return <div className="panel"><div className="empty" style={{ color: 'var(--danger)' }}>조회 실패: {error}</div></div>
  return (
            <TablePanel title={<>유형별 발생 ({byType.length}종)</>}
                        action={<button className="btn btn--ghost btn--sm" onClick={exportCsv}
                                        disabled={byType.length === 0}>CSV</button>}>
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
  )
}

export function EventBySourceBlock() {
  const { bySource, loading, error } = useEventAggs()
  const maxSrcCount = Math.max(1, ...bySource.map(t => t.count))
  if (loading && bySource.length === 0) return <div className="panel"><div className="empty">로딩 중…</div></div>
  if (error) return <div className="panel"><div className="empty" style={{ color: 'var(--danger)' }}>조회 실패: {error}</div></div>
  return (
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
  )
}
