// CIMS 위젯 — 활성 알람 (재정의된 알람/이벤트 모델 기반).
//   상단 severity 요약 타일(6단계 색 체계) + 아래 활성 알람 목록. 알람 표준화(X.733/32.111):
//   A-* code · perceived_severity · mo 소유 주체(source.mo_instance) · ×N 재통지 · ack 라이프사이클.
//   이벤트(정상 동작 통지)는 별도 스트림이라 여기 표시하지 않는다(표준화 §3.6 — 이벤트는
//   헤더 드로어/이력 탭). 배너 역할(critical/major 강조)을 흡수 — 심각 알람 행에 좌측 강조선.
//   데이터는 전역 알람 store 구독 1원화(alarm_pipeline.md §8.2, 개별 fetch 없음). 폴링 실패는
//   "표시 없음 ≠ 정상" 을 위해 명시(error). 타일 클릭 = 해당 severity 로 목록 필터.
import { useMemo, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { alertsApi } from '@core/api/alerts'
import { SEV_COLOR, refreshAlarms, severityOf, useAlarms } from '@core/widgets/useAlarms'
import { useToast } from '@core/components/Toast'
import type { WidgetDef } from '@core/widgets/types'

// 요약 타일에 항상 노출하는 상위 4단계(고정 순서). indeterminate/cleared 는 건수 있을 때만.
const TILE_ORDER = ['critical', 'major', 'minor', 'warning'] as const
const SEV_LABEL: Record<string, string> = {
  critical: 'CRIT', major: 'MAJOR', minor: 'MINOR',
  warning: 'WARN', indeterminate: 'IND', cleared: 'CLR',
}
const SEVERE = new Set(['critical', 'major'])   // 배너 흡수 — 강조 대상

function Dot({ sev, size = 9 }: { sev: string; size?: number }) {
  return <span style={{ width: size, height: size, borderRadius: '50%',
                        background: SEV_COLOR[sev] || 'var(--muted-foreground)', display: 'inline-block', flex: 'none' }} />
}

function ActiveAlarmsWidget() {
  const navigate = useNavigate()
  const { active, loaded, error } = useAlarms()
  const { show } = useToast()
  const [filter, setFilter] = useState<string | null>(null)

  const counts = useMemo(() => {
    const c: Record<string, number> = {}
    for (const a of active) { const s = severityOf(a); c[s] = (c[s] || 0) + 1 }
    return c
  }, [active])

  const tiles: string[] = [
    ...TILE_ORDER,
    ...(counts.indeterminate ? ['indeterminate'] : []),
  ]
  const rows = filter ? active.filter(a => severityOf(a) === filter) : active

  const ack = async (id?: string) => {
    if (!id) return
    try { await alertsApi.ack(id); show('알람 승인됨', 'ok'); refreshAlarms() }
    catch (e) { show(`승인 실패: ${(e as Error).message}`, 'err') }
  }

  return (
    <div className="panel">
      {/* 헤더 — 총 건수 + 폴링 실패 표기 + 이력/카탈로그 이동 */}
      <div style={{ padding: '12px 16px', borderBottom: '1px solid var(--border)', background: 'var(--muted)',
                    display: 'flex', alignItems: 'center', gap: 10 }}>
        <span style={{ fontWeight: 600, fontSize: 14 }}>활성 알람 ({active.length})</span>
        {error && (
          <span title="알람 조회 실패 — 표시가 최신이 아닐 수 있음 (표시 없음 ≠ 정상)"
                style={{ fontSize: 12, color: 'var(--destructive)', fontWeight: 600 }}>⚠ 조회 실패</span>
        )}
        <span style={{ marginLeft: 'auto', display: 'flex', gap: 12 }}>
          <a href="#" onClick={e => { e.preventDefault(); navigate('/alerts/active') }}
             style={{ fontSize: 12, fontWeight: 500 }}>활성 전체 →</a>
          <a href="#" onClick={e => { e.preventDefault(); navigate('/alerts/catalog') }}
             style={{ fontSize: 12, fontWeight: 500, color: 'var(--muted-foreground)' }}>카탈로그</a>
        </span>
      </div>

      {/* severity 요약 타일 — 클릭 시 해당 심각도로 필터(재클릭 해제) */}
      <div style={{ display: 'flex', gap: 8, padding: '10px 16px', flexWrap: 'wrap',
                    borderBottom: '1px solid var(--border)' }}>
        {tiles.map(sev => {
          const n = counts[sev] || 0
          const sel = filter === sev
          return (
            <button key={sev} onClick={() => setFilter(sel ? null : sev)}
                    title={`${SEV_LABEL[sev]} ${n}건${sel ? ' — 필터 해제' : n ? ' — 이 심각도만' : ''}`}
                    style={{
                      display: 'flex', alignItems: 'center', gap: 7, cursor: 'pointer',
                      padding: '6px 12px', borderRadius: 'var(--radius)',
                      border: `1px solid ${sel ? SEV_COLOR[sev] : 'var(--border)'}`,
                      background: sel ? `color-mix(in srgb, ${SEV_COLOR[sev]} 12%, var(--card))` : 'var(--card)',
                      opacity: n === 0 && !sel ? 0.5 : 1,
                    }}>
              <Dot sev={sev} />
              <span style={{ fontSize: 18, fontWeight: 700, fontVariantNumeric: 'tabular-nums', minWidth: 14 }}>{n}</span>
              <span style={{ fontSize: 11, fontWeight: 600, color: 'var(--muted-foreground)', letterSpacing: '.3px' }}>{SEV_LABEL[sev]}</span>
            </button>
          )
        })}
        {filter && (
          <button onClick={() => setFilter(null)} className="btn btn--ghost btn--sm"
                  style={{ marginLeft: 'auto', alignSelf: 'center' }}>전체 보기</button>
        )}
      </div>

      {/* 활성 알람 목록 */}
      {!loaded ? (
        <div className="empty">로딩 중…</div>
      ) : active.length === 0 ? (
        <div className="empty" style={{ color: 'var(--cims-success)' }}>
          ✓ 활성 알람 없음{error ? ' (단, 마지막 조회 실패 — 최신이 아닐 수 있음)' : ''}
        </div>
      ) : rows.length === 0 ? (
        <div className="empty">해당 심각도의 활성 알람 없음</div>
      ) : (
        <div className="table-wrap">
          <table className="data-table" style={{ fontSize: 13 }}>
            <thead>
              <tr>
                <th style={{ width: 92 }}>심각도</th>
                <th style={{ width: 118 }}>코드</th>
                <th style={{ width: 168 }}>소스(MO)</th>
                <th>메시지</th>
                <th style={{ width: 96 }}>승인</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((a, i) => {
                const sev = severityOf(a)
                const severe = SEVERE.has(sev) && !a.acked
                return (
                  <tr key={`${a.alarm_id || a.type}-${i}`}
                      style={severe ? { background: `color-mix(in srgb, ${SEV_COLOR[sev]} 7%, transparent)` } : undefined}>
                    <td style={severe ? { boxShadow: `inset 3px 0 0 ${SEV_COLOR[sev]}` } : undefined}>
                      <span style={{ display: 'inline-flex', alignItems: 'center', gap: 6 }}>
                        <Dot sev={sev} />
                        <span style={{ fontSize: 11, fontWeight: 600 }}>{SEV_LABEL[sev] || sev}</span>
                      </span>
                    </td>
                    <td>
                      <span style={{ display: 'inline-flex', alignItems: 'center', gap: 5 }}>
                        <code style={{ fontSize: 11 }}>{a.code || a.type}</code>
                        {(a.occurrences || 1) > 1 && <span className="badge badge--gray">×{a.occurrences}</span>}
                      </span>
                    </td>
                    <td><code style={{ fontSize: 11, color: 'var(--muted-foreground)' }}>{a.source?.mo_instance || '-'}</code></td>
                    <td>{a.message}</td>
                    <td>
                      {a.acked
                        ? <span style={{ fontSize: 11, color: 'var(--muted-foreground)' }}>✓ {a.ackUser || '승인'}</span>
                        : <button className="btn btn--ghost btn--sm" onClick={() => ack(a.alarm_id)}>승인</button>}
                    </td>
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

export const activeAlarmsWidget: WidgetDef = {
  id: 'cims.active-alarms',
  title: '활성 알람',
  category: 'event',
  component: ActiveAlarmsWidget,
  apis: ['alerts.list'],
  defaultSize: { w: 12 },
}
