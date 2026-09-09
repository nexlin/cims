// CIMS 위젯 — 활성 알람 (재정의된 알람/이벤트 모델 기반).
//   상단 severity 요약 타일(6단계 색 체계) + 아래 활성 알람 목록. 알람 표준화(X.733/32.111):
//   A-* code · perceived_severity · mo 소유 주체(source.mo_instance) · ×N 재통지 · ack 라이프사이클.
//   이벤트(정상 동작 통지)는 별도 스트림이라 여기 표시하지 않는다(표준화 §3.6 — 이벤트는
//   헤더 드로어/이력 탭). 배너 역할(critical/major 강조)을 흡수 — 심각 알람 행에 좌측 강조선.
//   데이터는 전역 알람 store 구독 1원화(alarm_pipeline.md §8.2, 개별 fetch 없음). 폴링 실패는
//   "표시 없음 ≠ 정상" 을 위해 명시(error). 타일 클릭 = 해당 severity 로 목록 필터.
import { AlertTriangle, ArrowRight, Check } from 'lucide-react'
import { useMemo, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { alertsApi } from '@core/api/alerts'
import { SEV_COLOR, refreshAlarms, severityOf, useAlarms } from '@core/widgets/useAlarms'
import { useToast } from '@core/components/Toast'
import type { WidgetDef } from '@core/widgets/types'
import { Button } from '@core/components/ui/button'
import { DataTable, Th, Td } from '@core/components/custom/data-table'
import { Badge } from '@core/components/ui/badge'
import { EmptyState } from '@core/components/custom/empty-state'

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
      <div className="py-3 px-4 border-b border-border bg-muted flex items-center gap-2.5">
        <span className="font-semibold text-base">활성 알람 ({active.length})</span>
        {error && (
          <span title="알람 조회 실패 — 표시가 최신이 아닐 수 있음 (표시 없음 ≠ 정상)"
                className="inline-flex items-center gap-1 text-sm font-semibold text-destructive">
            <AlertTriangle size={13} /> 조회 실패</span>
        )}
        <span className="ml-auto flex gap-3">
          <a className="inline-flex items-center gap-1 text-sm font-medium" href="#" onClick={e => { e.preventDefault(); navigate('/alerts/active') }}>활성 전체 <ArrowRight size={13} /></a>
          <a className="text-sm font-medium text-muted-foreground" href="#" onClick={e => { e.preventDefault(); navigate('/alerts/catalog') }}>카탈로그</a>
        </span>
      </div>

      {/* severity 요약 타일 — 클릭 시 해당 심각도로 필터(재클릭 해제) */}
      <div className="flex gap-2 py-2.5 px-4 flex-wrap border-b border-border">
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
          <Button className="ml-auto self-center" variant="ghost" onClick={() => setFilter(null)}>전체 보기</Button>
        )}
      </div>

      {/* 활성 알람 목록 */}
      {!loaded ? (
        <div className="flex min-h-0 flex-1 items-center justify-center p-8 text-center text-muted-foreground">로딩 중…</div>
      ) : active.length === 0 ? (
        <EmptyState title={<span className="text-success"><Check size={13} className="inline align-[-2px]" /> 활성 알람 없음{error ? ' (단, 마지막 조회 실패 — 최신이 아닐 수 있음)' : ''}</span>} />
      ) : rows.length === 0 ? (
        <EmptyState title="해당 심각도의 활성 알람 없음" />
      ) : (
        <div className="flex-1 overflow-x-auto">
          <DataTable sticky>
            <thead>
              <tr>
                <Th className="w-[92px]">심각도</Th>
                <Th className="w-[118px]">코드</Th>
                <Th className="w-[168px]">소스(MO)</Th>
                <Th>메시지</Th>
                <Th className="w-[96px]">승인</Th>
              </tr>
            </thead>
            <tbody>
              {rows.map((a, i) => {
                const sev = severityOf(a)
                const severe = SEVERE.has(sev) && !a.acked
                return (
                  <tr key={`${a.alarm_id || a.type}-${i}`}
                      style={severe ? { background: `color-mix(in srgb, ${SEV_COLOR[sev]} 7%, transparent)` } : undefined}>
                    <Td style={severe ? { boxShadow: `inset 3px 0 0 ${SEV_COLOR[sev]}` } : undefined}>
                      <span className="inline-flex items-center gap-1.5">
                        <Dot sev={sev} />
                        <span className="text-xs font-semibold">{SEV_LABEL[sev] || sev}</span>
                      </span>
                    </Td>
                    <Td>
                      <span className="inline-flex items-center gap-[5px]">
                        <code className="text-xs">{a.code || a.type}</code>
                        {(a.occurrences || 1) > 1 && <Badge variant="neutralSoft" >×{a.occurrences}</Badge>}
                      </span>
                    </Td>
                    <Td><code className="text-xs text-muted-foreground">{a.source?.mo_instance || '-'}</code></Td>
                    <Td>{a.message}</Td>
                    <Td>
                      {a.acked
                        ? <span className="inline-flex items-center gap-1 text-xs text-muted-foreground">
                      <Check size={12} /> {a.ackUser || '승인'}</span>
                        : <Button variant="ghost" onClick={() => ack(a.alarm_id)}>승인</Button>}
                    </Td>
                  </tr>
                )
              })}
            </tbody>
          </DataTable>
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
