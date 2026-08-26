// CIMS 위젯 — 최근 이벤트 (재정의된 알람/이벤트 모델 — 알람과 분리된 스트림).
//   정상 동작 통지: X.730/731 stateChange(STC) · X.740 audit(AUD). severity/ack 없음(알람 아님).
//   상단 kind 요약 타일(STC/AUD) + 아래 이벤트 목록(코드 E-* · 소스 MO · 메시지 · 시각).
//   데이터는 전역 알람 store 구독 1원화(alarm_pipeline.md §8.2 — recentEvents, 개별 fetch 없음).
//   타일 클릭 = 해당 kind 로 목록 필터. 이벤트는 토스트/배너 대상이 아니다(§8.2 소음 통제).
import { useMemo, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAlarms } from '@core/widgets/useAlarms'
import type { EventRecord } from '@core/api/alerts'
import type { WidgetDef } from '@core/widgets/types'

// kind = 이벤트 스트림의 1차 축 (표준화 §3.6 — DOMAIN 약어 STC/AUD). 고정 순서.
const KIND_ORDER = ['stateChange', 'audit'] as const
const KIND_LABEL: Record<string, string> = { stateChange: '상태변경', audit: '감사' }
const KIND_ABBR: Record<string, string> = { stateChange: 'STC', audit: 'AUD' }
const KIND_BADGE: Record<string, string> = { stateChange: 'badge--blue', audit: 'badge--yellow' }

function kindOf(e: EventRecord): string { return e.kind || 'stateChange' }

function RecentEventsWidget() {
  const navigate = useNavigate()
  const { recentEvents, loaded, error } = useAlarms()
  const [filter, setFilter] = useState<string | null>(null)

  const counts = useMemo(() => {
    const c: Record<string, number> = {}
    for (const e of recentEvents) { const k = kindOf(e); c[k] = (c[k] || 0) + 1 }
    return c
  }, [recentEvents])

  // 알려진 kind 외 값이 있으면 타일에 추가 노출(전방 호환)
  const extraKinds = Object.keys(counts).filter(k => !KIND_ORDER.includes(k as typeof KIND_ORDER[number]))
  const tiles: string[] = [...KIND_ORDER, ...extraKinds]
  const rows = filter ? recentEvents.filter(e => kindOf(e) === filter) : recentEvents

  return (
    <div className="panel">
      {/* 헤더 — 총 건수(24h) + 이력 이동 */}
      <div style={{ padding: '12px 16px', borderBottom: '1px solid var(--border)', background: 'var(--bg-soft)',
                    display: 'flex', alignItems: 'center', gap: 10 }}>
        <span style={{ fontWeight: 600, fontSize: 14 }}>최근 이벤트 ({recentEvents.length})</span>
        <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>최근 24시간 · 정상 동작 통지</span>
        {error && (
          <span title="조회 실패 — 표시가 최신이 아닐 수 있음"
                style={{ fontSize: 12, color: 'var(--danger)', fontWeight: 600 }}>⚠ 조회 실패</span>
        )}
        <a href="#" onClick={e => { e.preventDefault(); navigate('/alerts/history') }}
           style={{ marginLeft: 'auto', fontSize: 12, fontWeight: 500 }}>이력 →</a>
      </div>

      {/* kind 요약 타일 — 클릭 시 필터(재클릭 해제) */}
      <div style={{ display: 'flex', gap: 8, padding: '10px 16px', flexWrap: 'wrap',
                    borderBottom: '1px solid var(--border)' }}>
        {tiles.map(kind => {
          const n = counts[kind] || 0
          const sel = filter === kind
          const label = KIND_LABEL[kind] || kind
          return (
            <button key={kind} onClick={() => setFilter(sel ? null : kind)}
                    title={`${label} ${n}건${sel ? ' — 필터 해제' : n ? ' — 이 종류만' : ''}`}
                    style={{
                      display: 'flex', alignItems: 'center', gap: 8, cursor: 'pointer',
                      padding: '6px 12px', borderRadius: 'var(--radius)',
                      border: `1px solid ${sel ? 'var(--primary)' : 'var(--border)'}`,
                      background: sel ? 'var(--primary-soft)' : 'var(--surface)',
                      opacity: n === 0 && !sel ? 0.5 : 1,
                    }}>
              <span style={{ fontSize: 18, fontWeight: 700, fontVariantNumeric: 'tabular-nums', minWidth: 14 }}>{n}</span>
              <span style={{ fontSize: 11, fontWeight: 600, color: 'var(--text-muted)', letterSpacing: '.3px' }}>
                {label} <span style={{ opacity: .7 }}>{KIND_ABBR[kind] || ''}</span>
              </span>
            </button>
          )
        })}
        {filter && (
          <button onClick={() => setFilter(null)} className="btn btn--ghost btn--sm"
                  style={{ marginLeft: 'auto', alignSelf: 'center' }}>전체 보기</button>
        )}
      </div>

      {/* 이벤트 목록 */}
      {!loaded && recentEvents.length === 0 ? (
        <div className="empty">로딩 중…</div>
      ) : recentEvents.length === 0 ? (
        <div className="empty">최근 24시간 이벤트 없음</div>
      ) : rows.length === 0 ? (
        <div className="empty">해당 종류의 이벤트 없음</div>
      ) : (
        <div className="table-wrap">
          <table className="data-table" style={{ fontSize: 13 }}>
            <thead>
              <tr>
                <th style={{ width: 96 }}>구분</th>
                <th style={{ width: 118 }}>코드</th>
                <th style={{ width: 168 }}>소스(MO)</th>
                <th>메시지</th>
                <th style={{ width: 150 }}>시각</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((e, i) => {
                const kind = kindOf(e)
                return (
                  <tr key={`${e.code || e.type}-${e.ts}-${i}`}>
                    <td><span className={`badge ${KIND_BADGE[kind] || 'badge--gray'}`}>{KIND_LABEL[kind] || kind}</span></td>
                    <td><code style={{ fontSize: 11 }}>{e.code || e.type}</code></td>
                    <td><code style={{ fontSize: 11, color: 'var(--text-muted)' }}>{e.source?.mo_instance || '-'}</code></td>
                    <td>{e.message}</td>
                    <td className="ts">{e.ts}</td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}

export const recentEventsWidget: WidgetDef = {
  id: 'cims.recent-events',
  title: '최근 이벤트',
  category: 'event',
  component: RecentEventsWidget,
  apis: ['events.list'],
  defaultSize: { w: 12 },
}
