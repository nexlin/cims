// shape별 순수 렌더러 — shape 데이터만 받아 그린다 (소스/fetch 무관). 테마 토큰 사용.
//
// **차트 높이는 담긴 칸을 따라간다** — px 를 박아 두면 카드를 키워도 여백만 생기고 줄이면 잘린다.
// 캔버스가 고정 예산(화면 한 장)이라 카드 크기가 배치마다 다르므로, 막대 높이는 플롯 영역 대비
// **비율(%)** 로 그린다(플롯 영역은 flex:1 로 남은 높이를 전부 차지).
import { useRef, useState } from 'react'
import type { CSSProperties } from 'react'
import type { TimeBarData, SeriesBarData, KpiData, DistributionData, TableData, MatrixData } from './types'
import { DataTable, Th, Td } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'

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

export function TimeBarChart({ data }: { data: TimeBarData }) {
  const { buckets, unit } = data
  const vals = buckets.map(b => b.value)
  const max = Math.max(...vals, 1)
  if (buckets.length === 0) return <EmptyState title="데이터 없음" />
  const labels = compactLabels(buckets.map(b => b.label))
  // 라벨·값은 몇 칸 걸러 하나만 — 막대는 다 보이되 글자만 솎는다(겹쳐 뭉개지는 것보다 낫다).
  const every = Math.ceil(buckets.length / 24)
  return (
    <div style={{ flex: 1, minHeight: 0, display: 'flex', alignItems: 'flex-end', gap: 2, padding: '0 4px' }}>
      {buckets.map((b, i) => (
        <div className="flex-1 min-w-0 h-full flex flex-col items-center" key={i}>
          <div className="flex-none text-[10px] text-muted-foreground mb-0.5">
            {b.value > 0 && i % every === 0 ? b.value : ''}
          </div>
          {/* 막대 영역 — 남은 높이 전부. 막대는 그 안에서 값 비율만큼 차지한다. */}
          <div className="flex-1 min-h-0 w-full flex items-end justify-center">
            <div title={`${b.label}: ${b.value}${unit || ''}`}
                 style={{ width: '100%', maxWidth: 32, height: `${(b.value / max) * 100}%`, minHeight: 2,
                          background: 'var(--primary)', borderRadius: '2px 2px 0 0' }} />
          </div>
          {/* 라벨은 몇 칸 걸러 하나만 — 막대는 다 보이되 글자만 솎는다(겹쳐 뭉개지는 것보다 낫다). */}
          <div className="flex-none text-[10px] text-muted-foreground mt-0.5">
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
  const [hover, setHover] = useState<
    { x: number; y: number; bucket: string; key: string; total: number } | null>(null)
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
    const r = wrap.current?.getBoundingClientRect()
    if (!r) return
    setHover({ x: e.clientX - r.left, y: e.clientY - r.top, bucket, key, total })
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
      <div style={{ flex: 1, minHeight: 0, display: 'flex', alignItems: 'flex-end', gap: 2, padding: '0 4px' }}>
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
          position: 'absolute', left: Math.min(hover.x + 12, (wrap.current?.clientWidth ?? 0) - 190),
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
        <div key={i} style={{ flex: '1 1 120px', background: 'var(--card)', border: '1px solid var(--border)',
                              borderRadius: 'var(--radius)', padding: '14px 16px', textAlign: 'center' }}>
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
  if (!k) return <EmptyState title="지표 없음" className="text-[12px]" />
  return (
    <div className="flex-auto min-h-0 flex flex-col justify-center items-center text-center">
      <div className="text-sm text-muted-foreground mb-1">{k.label}</div>
      <div style={{ fontSize: 24, fontWeight: 700, lineHeight: 1.1 }}>
        {k.value}<span className="text-sm text-muted-foreground ml-0.5">{k.unit}</span>
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
            <div className="flex-1 bg-secondary rounded-[4px] h-[18px] flex overflow-hidden">
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
  const cellBg = (key: string, v: number) => {
    if (!v) return undefined
    const a = Math.min(0.42, 0.06 + 0.36 * (v / (colMax.get(key) || 1)))
    return `color-mix(in srgb, var(--primary) ${Math.round(a * 100)}%, transparent)`
  }
  const numTd = (v: number, bg?: string): CSSProperties => ({
    textAlign: 'right', fontVariantNumeric: 'tabular-nums',
    color: v ? 'var(--foreground)' : 'var(--muted-foreground)', opacity: v ? 1 : 0.45,
    fontWeight: v ? 600 : 400, background: bg, whiteSpace: 'nowrap',
  })
  const stickyL: CSSProperties = {
    position: 'sticky', left: 0, background: 'var(--card)', zIndex: 1, whiteSpace: 'nowrap',
  }
  const stickyR: CSSProperties = {
    position: 'sticky', right: 0, background: 'var(--card)', zIndex: 1,
  }

  if (data.rows.length === 0 || data.columns.length === 0) {
    return <EmptyState title="데이터 없음" />
  }
  return (
    <div className="overflow-auto max-h-full">
      <DataTable sticky className="[&_td]:text-sm">
        <thead>
          <tr>
            <Th style={{ ...stickyL, zIndex: 2 }}>시각</Th>
            {data.columns.map(c => (
              <Th className="text-right whitespace-nowrap" key={c.key}
                  title={`전 구간 ${c.total}${c.unit ?? data.unit ?? '건'}`}>
                {c.label}{c.unit === '%' ? ' (%)' : ''}
              </Th>
            ))}
            {data.rowTotal && <Th style={{ ...stickyR, zIndex: 2, textAlign: 'right' }}>합계</Th>}
          </tr>
        </thead>
        <tbody>
          {data.rows.map(r => (
            <tr key={r.label}>
              <Td style={stickyL}>{r.label}</Td>
              {data.columns.map(c => {
                const v = r.cells[c.key] ?? 0
                return <Td key={c.key} style={numTd(v, cellBg(c.key, v))}>{v}</Td>
              })}
              {data.rowTotal &&
                <Td style={{ ...stickyR, ...numTd(r.total), fontWeight: 700 }}>{r.total}</Td>}
            </tr>
          ))}
        </tbody>
        <tfoot>
          <tr>
            <Td style={{ ...stickyL, fontWeight: 700 }}>{data.rowTotal ? '합계' : '전 구간'}</Td>
            {data.columns.map(c => (
              <Td key={c.key} style={{ ...numTd(c.total), fontWeight: 700 }}>{c.total}</Td>
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
