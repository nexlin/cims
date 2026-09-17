// shape별 순수 렌더러 — shape 데이터만 받아 그린다 (소스/fetch 무관). 테마 토큰 사용.
//
// **차트 높이는 담긴 칸을 따라간다** — px 를 박아 두면 카드를 키워도 여백만 생기고 줄이면 잘린다.
// 캔버스가 고정 예산(화면 한 장)이라 카드 크기가 배치마다 다르므로, 막대 높이는 플롯 영역 대비
// **비율(%)** 로 그린다(플롯 영역은 flex:1 로 남은 높이를 전부 차지).
import { Fragment, useMemo, useRef, useState } from 'react'
import type { CSSProperties } from 'react'
import type { TimeBarData, SeriesBarData, KpiData, DistributionData, TableData, MatrixData } from './types'
import { DataTable, Th, Td } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { ChevronDown, ChevronRight } from 'lucide-react'
import { NO_VALUE } from './dataSourceSpec'
import { blankRange, foldBlankRuns } from './matrixFold'

// 집계 불가 표시 — 서버가 `null` 로 내린 값. 0 과 구별해 빈 자리로 그리고, 왜 비었는지
// 말해 준다(분모가 원천에 없는 기간이 섞였다 — sip_statistics.md §2.1).
const NO_VALUE_TITLE = '값을 낼 수 없습니다 — 자료가 없거나, 이 구간에 분모(시도) 원천이 없는 기간이 섞여 있습니다'
const naCell = (
  <span title={NO_VALUE_TITLE} className="cursor-help text-muted-foreground">{NO_VALUE}</span>
)

// ── 시간축 공용 ────────────────────────────────────────────────────────────

/**
 * 라벨 압축 — 촘촘한 축에서 반복되는 연·월을 지운다. 전체 값은 tooltip 이 갖는다.
 *   'YYYY-MM-DD HH:MM' → 'HH:MM' (날이 바뀌는 칸만 'MM-DD HH:MM')
 *   'YYYY-MM-DD'       → 'MM-DD'
 *   'YYYY-MM' · 'YYYY' → 그대로 (버킷 수가 적어 압축할 이유가 없다)
 */
function compactLabels(labels: (string | number)[]): string[] {
  const out: string[] = []
  let prevDay = ''
  for (const raw of labels) {
    const v = String(raw)
    const m = /^(\d{4})-(\d{2})-(\d{2})(?:[ T](\d{2}:\d{2}))?$/.exec(v)
    if (!m) { out.push(v); prevDay = ''; continue }
    const [, , mo, dd, hm] = m
    if (!hm) { out.push(`${mo}-${dd}`); prevDay = `${mo}-${dd}`; continue }
    const day = `${mo}-${dd}`
    out.push(day === prevDay ? hm : `${day} ${hm}`)
    prevDay = day
  }
  return out
}

/**
 * 버킷이 이보다 많으면 **막대 대신 선**으로 그린다.
 *
 * 막대는 칸마다 폭을 나눠 가져야 한다 — 추이 카드 폭(그리드 26칸 ≈ 864px)에 1분 단위 하루치
 * 900버킷을 넣으면 한 칸이 1px 미만이 되어 **그래프가 안 보인다**(실측 2026-09-16).
 * 선은 폭이 필요 없어서 버킷이 아무리 많아도 모양이 남는다 — 자료를 묶어 버리지 않고(압축)
 * 해상도를 그대로 둔 채 조회 구간 전체가 한 화면에 들어온다.
 *
 * 반대로 성길 때는 막대가 낫다 — 칸 경계가 보여서 구간끼리 비교가 쉽다. 그래서 자동 전환한다.
 */
const LINE_MIN_BUCKETS = 120

export function TimeBarChart({ data }: { data: TimeBarData }) {
  const { buckets, unit } = data
  const [hover, setHover] = useState<number | null>(null)
  const wrap = useRef<HTMLDivElement>(null)
  const vals = buckets.map(b => b.value)
  const max = Math.max(...vals, 1)
  if (buckets.length === 0) return <EmptyState title="데이터 없음" />
  const labels = compactLabels(buckets.map(b => b.label))

  if (buckets.length > LINE_MIN_BUCKETS) {
    // ── 선 모드 ───────────────────────────────────────────────────────────
    // 점은 찍지 않는다: 이 밀도에서 점 간격은 1~2px 라 점끼리 붙어 **선이 두꺼워진 것처럼만**
    // 보이고(값 위치를 짚어 주는 역할을 못 한다), SVG 노드가 버킷 수만큼 늘어난다.
    const W = 1000, H = 100
    const x = (i: number) => (i / Math.max(1, buckets.length - 1)) * W
    const y = (v: number) => H - (v / max) * (H - 4) - 2
    const line = vals.map((v, i) => `${i ? 'L' : 'M'} ${x(i).toFixed(1)} ${y(v).toFixed(1)}`).join(' ')
    const area = `${line} L ${W} ${H} L 0 ${H} Z`
    // 가로축 눈금은 **축에 직접** 둔다 — 칸 안에 넣으면 1px 칸에서 글자가 서로 겹친다.
    const ticks = [0, 0.25, 0.5, 0.75, 1].map(f => Math.round((buckets.length - 1) * f))
    const at = (e: React.MouseEvent) => {
      const r = wrap.current?.getBoundingClientRect()
      if (!r || r.width === 0) return
      const i = Math.round(((e.clientX - r.left) / r.width) * (buckets.length - 1))
      setHover(Math.max(0, Math.min(buckets.length - 1, i)))
    }
    const h = hover === null ? null : buckets[hover]
    return (
      <div className="flex-1 min-h-0 flex flex-col">
        <div className="flex-none h-4 text-xs text-muted-foreground text-right pr-1 tabular-nums">
          {h ? `${labels[hover!]} · ${h.value}${unit || ''}` : `최대 ${max}${unit || ''}`}
        </div>
        <div ref={wrap} className="flex-1 min-h-0 relative" onMouseMove={at}
             onMouseLeave={() => setHover(null)}>
          <svg viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" width="100%" height="100%"
               role="img" aria-label={`추이 ${buckets.length}구간, 최대 ${max}${unit || ''}`}>
            <path d={area} fill="color-mix(in srgb, var(--primary) 16%, transparent)" />
            <path d={line} fill="none" stroke="var(--primary)" strokeWidth={1.2}
                  vectorEffect="non-scaling-stroke" strokeLinejoin="round" />
            {hover !== null && (
              <line x1={x(hover)} x2={x(hover)} y1={0} y2={H} stroke="var(--muted-foreground)"
                    strokeWidth={1} vectorEffect="non-scaling-stroke" strokeDasharray="3 3" />
            )}
          </svg>
        </div>
        <div className="flex-none flex justify-between text-xs text-muted-foreground pt-1 tabular-nums">
          {ticks.map(i => <span key={i}>{labels[i]}</span>)}
        </div>
      </div>
    )
  }

  // ── 막대 모드 (성길 때) ─────────────────────────────────────────────────
  // 라벨·값은 몇 칸 걸러 하나만 — 막대는 다 보이되 글자만 솎는다(겹쳐 뭉개지는 것보다 낫다).
  const every = Math.ceil(buckets.length / 24)
  return (
    <div className="flex-1 min-h-0 flex items-end py-0 px-1 gap-0.5">
      {buckets.map((b, i) => (
        <div className="flex-1 min-w-0 h-full flex flex-col items-center" key={i}>
          <div className="flex-none text-xs text-muted-foreground mb-0.5">
            {b.value > 0 && i % every === 0 ? b.value : ''}
          </div>
          {/* 막대 영역 — 남은 높이 전부. 막대는 그 안에서 값 비율만큼 차지한다. */}
          <div className="flex-1 min-h-0 w-full flex items-end justify-center">
            <div title={`${b.label}: ${b.value}${unit || ''}`}
                 style={{ width: '100%', maxWidth: 32, height: `${(b.value / max) * 100}%`, minHeight: 2,
                          background: 'var(--primary)', borderRadius: '2px 2px 0 0' }} />
          </div>
          <div className="flex-none text-xs text-muted-foreground mt-0.5">
            {i % every === 0 ? labels[i] : ''}
          </div>
        </div>
      ))}
    </div>
  )
}

// 계열 시계열 — 한 버킷에 **막대 하나**, 고른 계열을 색으로 **쌓아** 올린다(아래→위 = 선언 순서).
// 쌓기는 "부분의 합"을 뜻하므로 막대 높이가 곧 고른 계열의 합계다 — VoLTE 4 위에 PTT 4 를 얹으면
// 8 짜리 막대가 된다. 포함관계인 계열(전체 ⊃ VoLTE)을 같이 켜면 그 합이 중복이라, 그럴 때만
// 범례 아래에 한 줄로 알린다(선택을 막지는 않는다 — 참조선으로 겹쳐 보고 싶을 수 있다).
export function SeriesBarChart({ data }: { data: SeriesBarData }) {
  const { buckets, series, unit } = data
  // `w` = 툴팁을 가둘 폭. **렌더 중에는 ref 를 읽을 수 없으므로**(React 규약 — 그 값으로 다시 그리지
  // 않아 위치가 낡는다) 마우스 이벤트에서 재어 함께 담는다.
  const [hover, setHover] = useState<
    { x: number; y: number; w: number; bucket: string; key: string; total: number } | null>(null)
  const wrap = useRef<HTMLDivElement>(null)

  if (series.length === 0) return <EmptyState title="표시할 계열을 선택하세요" />
  if (buckets.length === 0) return <EmptyState title="데이터 없음" />

  const sum = (b: typeof buckets[number]) => series.reduce((a, sp) => a + (b.values[sp.key] || 0), 0)
  const max = Math.max(1, ...buckets.map(sum))
  // 라벨이 촘촘하면(버킷이 많으면) 몇 칸 걸러 하나만 적는다 — 겹쳐 뭉개지는 것보다 낫다.
  const every = Math.ceil(buckets.length / 24)
  const labels = compactLabels(buckets.map(b => b.label))
  // 고른 계열 중 포함관계로 겹치는 쌍이 있으면 합계가 중복된다.
  const shownKeys = new Set(series.map(sp => sp.key))
  const overlap = series.filter(sp => (sp.includes ?? []).some(k => shownKeys.has(k)))

  const move = (e: React.MouseEvent, bucket: string, key: string, total: number) => {
    const el = wrap.current
    if (!el) return
    const r = el.getBoundingClientRect()
    setHover({ x: e.clientX - r.left, y: e.clientY - r.top, w: el.clientWidth, bucket, key, total })
  }
  const hoveredBucket = hover ? buckets.find(b => String(b.label) === hover.bucket) : undefined

  return (
    <div className="relative flex-1 min-h-0 flex flex-col" ref={wrap}
         onMouseLeave={() => setHover(null)}>
      <div className="flex-none flex gap-3.5 flex-wrap mb-2">
        {series.map(sp => (
          <span className="flex items-center gap-[5px] text-sm" key={sp.key}>
            <span style={{ width: 10, height: 10, borderRadius: 2, background: sp.color }} />
            {sp.label}
          </span>
        ))}
      </div>
      {overlap.length > 0 && (
        <div className="flex-none text-xs text-muted-foreground mb-2">
          ※ {overlap.map(sp => sp.label).join(' · ')} 은(는) 다른 계열을 포함합니다 — 함께 쌓으면 합계가 중복됩니다.
        </div>
      )}
      <div className="flex-1 min-h-0 flex items-end gap-0.5 py-0 px-1">
        {buckets.map((b, i) => {
          const total = sum(b)
          const on = hover?.bucket === String(b.label)
          return (
            <div className="flex-1 min-w-0 flex flex-col items-center h-full" key={i}>
              <div className="flex-1 w-full min-h-0 flex flex-col-reverse items-center justify-start">
                {/* column-reverse — 선언 순서 첫 계열이 바닥에 깔린다 */}
                {series.map(sp => {
                  const v = b.values[sp.key] || 0
                  if (v <= 0) return null
                  return (
                    <div key={sp.key}
                         onMouseMove={e => move(e, String(b.label), sp.key, total)}
                         style={{ width: '100%', maxWidth: 26, height: `${(v / max) * 100}%`, minHeight: 2,
                                  background: sp.color,
                                  opacity: !hover || hover.key === sp.key ? 1 : 0.45,
                                  cursor: 'default' }} />
                  )
                })}
                {total === 0 && (
                  <div className="w-full max-w-[26px] h-[2px] bg-border" onMouseMove={e => move(e, String(b.label), '', 0)}/>
                )}
              </div>
              <div style={{ flex: 'none', fontSize: 10, marginTop: 2, whiteSpace: 'nowrap', overflow: 'hidden',
                            color: on ? 'var(--foreground)' : 'var(--muted-foreground)',
                            fontWeight: on ? 600 : 400 }}>
                {i % every === 0 || on ? labels[i] : ''}
              </div>
            </div>
          )
        })}
      </div>
      {hover && hoveredBucket && (
        <div style={{
          position: 'absolute', left: Math.min(hover.x + 12, hover.w - 190),
          top: Math.max(hover.y - 12, 0), zIndex: 30, pointerEvents: 'none', width: 178,
          background: 'var(--card)', border: '1px solid var(--border)',
          borderRadius: 'var(--radius)', boxShadow: 'var(--cims-elevation-lg)', padding: '8px 10px', fontSize: 12,
        }}>
          <div className="text-muted-foreground mb-[5px]">{hover.bucket}</div>
          {series.map(sp => {
            const v = hoveredBucket.values[sp.key] || 0
            const cur = sp.key === hover.key
            return (
              <div key={sp.key} style={{ display: 'flex', alignItems: 'center', gap: 6, marginTop: 2,
                                         fontWeight: cur ? 700 : 400, opacity: cur || v > 0 ? 1 : 0.5 }}>
                <span style={{ width: 8, height: 8, borderRadius: 2, background: sp.color, flex: 'none' }} />
                <span className="flex-1 overflow-hidden text-ellipsis whitespace-nowrap">
                  {sp.label}
                </span>
                <span>{v}{unit || ''}</span>
              </div>
            )
          })}
          {series.length > 1 && (
            <div className="flex justify-between mt-1.5 pt-[5px] border-t border-border text-muted-foreground">
              <span>합계</span><span className="font-bold text-foreground">{hover.total}{unit || ''}</span>
            </div>
          )}
        </div>
      )}
    </div>
  )
}

export function KpiCards({ data }: { data: KpiData }) {
  return (
    <div className="flex gap-3 flex-wrap">
      {data.items.map((k, i) => (
        <div className="flex-[1_1_120px] bg-card border border-border rounded-md py-3.5 px-4 text-center" key={i}>
          <div className="text-sm text-muted-foreground mb-1">{k.label}</div>
          <div className="text-3xl font-bold">
            {k.value}<span className="text-sm text-muted-foreground ml-0.5">{k.unit}</span>
          </div>
        </div>
      ))}
    </div>
  )
}

// 지표 카드 — 값 하나. 카드가 자기 칸을 채우고 값은 세로 중앙.
export function StatValue({ data }: { data: KpiData }) {
  const k = data.items[0]
  if (!k) return <EmptyState title="지표 없음" className="text-sm" />
  return (
    <div className="flex-auto min-h-0 flex flex-col justify-center items-center text-center">
      <div className="text-sm text-muted-foreground mb-1">{k.label}</div>
      <div className="text-3xl font-bold leading-[1.1]">
        {k.value === NO_VALUE
          ? <span title={NO_VALUE_TITLE} className="cursor-help text-muted-foreground">{NO_VALUE}</span>
          : <>{k.value}<span className="text-sm text-muted-foreground ml-0.5">{k.unit}</span></>}
      </div>
    </div>
  )
}

export function DistributionBars({ data }: { data: DistributionData }) {
  const { items, total, series } = data
  const [hover, setHover] = useState<{ i: number; key: string } | null>(null)
  if (items.length === 0) return <EmptyState title="데이터 없음" />
  // 계열이 선언돼 있으면 막대 하나를 계열별 조각으로 나눠 색칠한다(시계열 차트와 같은 색).
  const seg = (series ?? []).length > 0
  return (
    <div onMouseLeave={() => setHover(null)}>
      {seg && (
        <div className="flex gap-3.5 flex-wrap mb-2">
          {series!.map(sp => (
            <span className="flex items-center gap-[5px] text-sm" key={sp.key}>
              <span style={{ width: 10, height: 10, borderRadius: 2, background: sp.color }} />
              {sp.label}
            </span>
          ))}
        </div>
      )}
      {items.slice().sort((a, b) => b.value - a.value).map((it, i) => {
        const pct = total > 0 ? Math.round(it.value / total * 100) : 0
        const on = hover?.i === i
        return (
          <div className="flex items-center gap-2 mb-1.5" key={i}>
            <div className="w-[90px] text-md overflow-hidden text-ellipsis whitespace-nowrap">{it.label || 'unknown'}</div>
            <div className="flex-1 bg-secondary rounded-sm h-[18px] flex overflow-hidden">
              {seg ? series!.map(sp => {
                const v = it.parts?.[sp.key] || 0
                if (v <= 0) return null
                const w = total > 0 ? v / total * 100 : 0
                return (
                  <div key={sp.key}
                       title={`${it.label} · ${sp.label}: ${v}`}
                       onMouseEnter={() => setHover({ i, key: sp.key })}
                       style={{ width: `${w}%`, minWidth: 3, background: sp.color, height: 18,
                                opacity: !hover || hover.key === sp.key ? 1 : 0.45 }} />
                )
              }) : (
                <div style={{ width: `${pct}%`, background: 'var(--primary)', borderRadius: 4, height: 18, minWidth: pct > 0 ? 4 : 0 }} />
              )}
            </div>
            <div style={{ width: 78, fontSize: 12, textAlign: 'right',
                          color: on ? 'var(--foreground)' : 'var(--muted-foreground)', fontWeight: on ? 600 : 400 }}>
              {on && hover ? `${hover.key}: ${it.parts?.[hover.key] ?? 0}` : `${it.value} (${pct}%)`}
            </div>
          </div>
        )
      })}
    </div>
  )
}

export function KvTable({ data }: { data: TableData }) {
  return (
    <DataTable sticky className="[&_td]:text-sm">
      <thead><tr><Th>{data.columns[0]}</Th><Th className="w-[90px] text-right">{data.columns[1]}</Th></tr></thead>
      <tbody>
        {data.rows.length === 0 ? <tr><Td colSpan={2} className="py-8 text-center text-muted-foreground">데이터 없음</Td></tr>
          : data.rows.map((r, i) => (
            <tr key={i}><Td>{r.key}</Td><Td className="text-right font-semibold">{r.value}</Td></tr>
          ))}
      </tbody>
    </DataTable>
  )
}

/**
 * 교차표 — 행=시간 버킷, 열=항목(SIP 메서드 등).
 *
 * 가시성 장치 세 가지:
 *  1) **0 은 흐리게.** 통계표는 대부분 칸이 0 이라, 0 이 진하면 값이 있는 칸이 묻힌다.
 *  2) **칸 배경 농도 = 그 열 안에서의 상대 크기.** 같은 메시지의 시간대별 증감을 색으로 먼저
 *     읽고 숫자로 확인하게 한다. 열마다 정규화하는 이유는 메시지별 자릿수가 크게 달라
 *     (200 은 수백, CANCEL 은 한 자리) 표 전체 기준으로는 작은 열이 전부 흰칸이 되기 때문이다.
 *  3) **합계 행·열 고정.** 가로 스크롤이 생겨도 시각(첫 열)과 합계는 늘 보이게 sticky.
 */
export function MatrixTable({ data }: { data: MatrixData }) {
  const colMax = new Map(data.columns.map(c => [
    c.key, Math.max(1, ...data.rows.map(r => r.cells[c.key] ?? 0)),
  ]))
  /**
   * 칸 색 — 값에 비례한 농도(큰 값이 진하다). **0 만 예외**로 고정 농도로 칠한다:
   * 높을수록 좋은 비율에서 0 은 "전부 실패" 인데 비례 색칠로는 가장 흐려져 **가장 나쁜 값이
   * 가장 안 보인다.** 색은 전부 같은 계열(`--primary`)로 통일한다 — 색으로 성격을 나누지
   * 않고, 왜 그런지는 실패 사유 칸과 그 툴팁이 말한다.
   * 분모가 없는 구간은 서버가 `null` 로 내려 `—` 이므로, 여기 0 은 "호는 있었고 하나도 안
   * 됐다" 만 뜻한다(§2.1a).
   */
  const paintZero = new Map(data.columns.map(c => [c.key, c.paintZero === true]))
  const cellBg = (key: string, v: number | null) => {
    if (v === 0 && paintZero.get(key)) {
      return 'color-mix(in srgb, var(--primary) 22%, transparent)'
    }
    if (!v) return undefined
    const a = Math.min(0.42, 0.06 + 0.36 * (v / (colMax.get(key) || 1)))
    return `color-mix(in srgb, var(--primary) ${Math.round(a * 100)}%, transparent)`
  }
  const numTd = (v: number | null, bg?: string, key?: string): CSSProperties => {
    // 칠한 0 은 흐리게 두지 않는다 — 읽혀야 하는 값이다.
    const painted = v === 0 && !!key && paintZero.get(key)
    return {
      textAlign: 'right', fontVariantNumeric: 'tabular-nums',
      color: (v || painted) ? 'var(--foreground)' : 'var(--muted-foreground)',
      opacity: (v || painted) ? 1 : 0.45,
      fontWeight: (v || painted) ? 600 : 400, background: bg, whiteSpace: 'nowrap',
    }
  }
  const stickyR: CSSProperties = {
    position: 'sticky', right: 0, background: 'var(--card)', zIndex: 1,
  }
  // 값 없는 구간 접기 — 규칙은 `matrixFold.ts` 가 정본이다(시험으로 덮는다).
  const chunks = useMemo(() => foldBlankRuns(data.rows, data.columns), [data.rows, data.columns])
  const [opened, setOpened] = useState<Record<string, boolean>>({})
  // 칸 상세 — 건수만으로는 원인을 못 답하는 열(실패 사유 등)에 소스가 붙여 준다.
  // **점선 밑줄을 둔다**: 마우스를 올려야 보이는 것은 올릴 이유가 보여야 쓰인다. 값이 0 인
  // 칸에는 붙이지 않는다(볼 게 없는데 밑줄이 있으면 빈 풍선을 열게 된다).
  const withDetail = (v: number | null, detail: string | undefined) => (
    v === null ? naCell
      : v && detail
        ? <span title={detail} className="cursor-help underline decoration-dotted underline-offset-2
                                          decoration-muted-foreground">{v}</span>
        : v
  )

  if (data.rows.length === 0 || data.columns.length === 0) {
    return <EmptyState title="데이터 없음" />
  }
  return (
    <div className="overflow-auto max-h-full">
      <DataTable sticky className="[&_td]:text-sm">
        <thead>
          <tr>
            <Th className="sticky left-0 z-[1] whitespace-nowrap bg-card z-[2]">시각</Th>
            {data.columns.map(c => (
              <Th className="text-right whitespace-nowrap" key={c.key}
                  title={`전 구간 ${c.total}${c.unit ?? data.unit ?? '건'}`}>
                {c.label}{c.unit === '%' ? ' (%)' : ''}
              </Th>
            ))}
            {data.rowTotal && <Th align="right" className="sticky right-0 z-[1] bg-card z-[2]">합계</Th>}
          </tr>
        </thead>
        <tbody>
          {chunks.map((g, gi) => {
            const dataRow = (r: MatrixData['rows'][number]) => (
              <tr key={r.label}>
                <Td className="sticky left-0 z-[1] whitespace-nowrap bg-card">{r.label}</Td>
                {data.columns.map(c => {
                  const v = r.cells[c.key] ?? null
                  return <Td key={c.key} style={numTd(v, cellBg(c.key, v), c.key)}>
                    {withDetail(v, r.details?.[c.key])}
                  </Td>
                })}
                {data.rowTotal &&
                  <Td className="sticky right-0 z-[1] bg-card font-bold" style={{ ...numTd(r.total) }}>{r.total}</Td>}
              </tr>
            )
            if (!g.fold) return <Fragment key={gi}>{g.rows.map(dataRow)}</Fragment>
            const id = String(g.rows[0].label)
            const on = opened[id] === true
            const span = data.columns.length + 1 + (data.rowTotal ? 1 : 0)
            return (
              <Fragment key={gi}>
                <tr>
                  <Td colSpan={span} className="bg-neutral-soft p-0">
                    <button type="button" aria-expanded={on}
                            onClick={() => setOpened(o => ({ ...o, [id]: !on }))}
                            className="flex w-full items-center gap-1.5 px-3.5 py-1.5 text-sm
                                       text-muted-foreground hover:text-foreground">
                      {on ? <ChevronDown size={13} /> : <ChevronRight size={13} />}
                      <span>{data.blankLabel ?? '자료가 없는 시간'}</span>
                      <span className="opacity-70">({blankRange(g.rows)})</span>
                      <span className="ml-auto">{on ? '접기' : '펼치기'}</span>
                    </button>
                  </Td>
                </tr>
                {on && g.rows.map(dataRow)}
              </Fragment>
            )
          })}
        </tbody>
        <tfoot>
          <tr>
            <Td className="sticky left-0 z-[1] whitespace-nowrap bg-card font-bold">{data.rowTotal ? '합계' : '전 구간'}</Td>
            {data.columns.map(c => (
              <Td key={c.key} style={{ ...numTd(c.total, cellBg(c.key, c.total), c.key),
                                       fontWeight: 700 }}>
                {withDetail(c.total, c.detail)}
              </Td>
            ))}
            {data.rowTotal &&
              <Td style={{ ...stickyR, ...numTd(data.grandTotal), fontWeight: 700 }}>{data.grandTotal}</Td>}
          </tr>
        </tfoot>
      </DataTable>
      {/* 각주 — 비율 열은 이름만으로 분자·분모를 알 수 없다. 표를 보는 자리에서 바로 읽히게
          표 바로 아래 둔다(별도 도움말로 빼면 아무도 찾아가지 않는다). */}
      {data.notes?.length ? (
        <div className="mt-2 text-xs text-muted-foreground leading-[1.7]">
          {data.notes.map(n => <div key={n}>{n}</div>)}
        </div>
      ) : null}
    </div>
  )
}
