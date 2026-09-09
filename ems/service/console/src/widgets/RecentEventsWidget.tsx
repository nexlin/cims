// CIMS 위젯 — 최근 이벤트 (재정의된 알람/이벤트 모델 — 알람과 분리된 스트림).
//   정상 동작 통지: X.730/731 stateChange(STC) · X.740 audit(AUD). severity/ack 없음(알람 아님).
//   상단 kind 요약 타일(STC/AUD) + 아래 이벤트 목록(코드 E-* · 소스 MO · 메시지 · 시각).
//   데이터는 전역 알람 store 구독 1원화(alarm_pipeline.md §8.2 — recentEvents, 개별 fetch 없음).
//   타일 클릭 = 해당 kind 로 목록 필터. 이벤트는 토스트/배너 대상이 아니다(§8.2 소음 통제).
import { AlertTriangle, ArrowRight } from 'lucide-react'
import { useMemo, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAlarms } from '@core/widgets/useAlarms'
import type { EventRecord } from '@core/api/alerts'
import type { WidgetDef } from '@core/widgets/types'
import { Button } from '@core/components/ui/button'
import { DataTable, Th, Td } from '@core/components/custom/data-table'
import { Badge } from '@core/components/ui/badge'
import type { BadgeTone } from '@core/components/ui/badge'
import { EmptyState } from '@core/components/custom/empty-state'

// kind = 이벤트 스트림의 1차 축 (표준화 §3.6 — DOMAIN 약어 STC/AUD). 고정 순서.
const KIND_ORDER = ['stateChange', 'audit'] as const
const KIND_LABEL: Record<string, string> = { stateChange: '상태변경', audit: '감사' }
const KIND_ABBR: Record<string, string> = { stateChange: 'STC', audit: 'AUD' }
const KIND_BADGE: Record<string, BadgeTone> = { stateChange: 'brandSoft', audit: 'warningSoft' }

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
    <div className="panel flex flex-1 flex-col overflow-hidden rounded-md border border-border bg-card">
      {/* 헤더 — 총 건수(24h) + 이력 이동 */}
      <div className="py-3 px-4 border-b border-border bg-muted flex items-center gap-2.5">
        <span className="font-semibold text-base">최근 이벤트 ({recentEvents.length})</span>
        <span className="text-xs text-muted-foreground">최근 24시간 · 정상 동작 통지</span>
        {error && (
          <span title="조회 실패 — 표시가 최신이 아닐 수 있음"
                className="inline-flex items-center gap-1 text-sm font-semibold text-destructive">
            <AlertTriangle size={13} /> 조회 실패</span>
        )}
        <a className="ml-auto inline-flex items-center gap-1 text-sm font-medium" href="#" onClick={e => { e.preventDefault(); navigate('/alerts/history') }}>이력 <ArrowRight size={13} /></a>
      </div>

      {/* kind 요약 타일 — 클릭 시 필터(재클릭 해제) */}
      <div className="flex gap-2 py-2.5 px-4 flex-wrap border-b border-border">
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
                      background: sel ? 'var(--cims-brand-soft)' : 'var(--card)',
                      opacity: n === 0 && !sel ? 0.5 : 1,
                    }}>
              <span className="min-w-3.5 text-xl font-bold [font-variant-numeric:tabular-nums]">{n}</span>
              <span style={{ fontSize: 11, fontWeight: 600, color: 'var(--muted-foreground)', letterSpacing: '.3px' }}>
                {label} <span className="opacity-70">{KIND_ABBR[kind] || ''}</span>
              </span>
            </button>
          )
        })}
        {filter && (
          <Button className="ml-auto self-center" variant="ghost" onClick={() => setFilter(null)}>전체 보기</Button>
        )}
      </div>

      {/* 이벤트 목록 */}
      {!loaded && recentEvents.length === 0 ? (
        <div className="flex min-h-0 flex-1 items-center justify-center p-8 text-center text-muted-foreground">로딩 중…</div>
      ) : recentEvents.length === 0 ? (
        <EmptyState title="최근 24시간 이벤트 없음" />
      ) : rows.length === 0 ? (
        <EmptyState title="해당 종류의 이벤트 없음" />
      ) : (
        <div className="flex-1 overflow-x-auto">
          <DataTable sticky>
            <thead>
              <tr>
                <Th className="w-[96px]">구분</Th>
                <Th className="w-[118px]">코드</Th>
                <Th className="w-[168px]">소스(MO)</Th>
                <Th>메시지</Th>
                <Th className="w-[150px]">시각</Th>
              </tr>
            </thead>
            <tbody>
              {rows.map((e, i) => {
                const kind = kindOf(e)
                return (
                  <tr key={`${e.code || e.type}-${e.ts}-${i}`}>
                    <Td><Badge variant={KIND_BADGE[kind] || 'neutralSoft'} >{KIND_LABEL[kind] || kind}</Badge></Td>
                    <Td><code className="text-xs">{e.code || e.type}</code></Td>
                    <Td><code className="text-xs text-muted-foreground">{e.source?.mo_instance || '-'}</code></Td>
                    <Td>{e.message}</Td>
                    <Td className="text-sm text-muted-foreground">{e.ts}</Td>
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

export const recentEventsWidget: WidgetDef = {
  id: 'cims.recent-events',
  title: '최근 이벤트',
  category: 'event',
  component: RecentEventsWidget,
  apis: ['events.list'],
  defaultSize: { w: 12 },
}
