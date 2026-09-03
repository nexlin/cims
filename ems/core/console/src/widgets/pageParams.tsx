// 페이지 파라미터 버스 — 한 레이아웃(page) 안의 위젯들이 조회 조건을 공유한다.
//
// 위젯을 잘게 분해하면 같은 조건(기간·단위)을 여러 위젯이 함께 봐야 한다. 위젯마다 자기 컨트롤을
// 들면 한 화면에 날짜 선택기가 3개 생기고 운영자가 3번 바꿔야 한다. 그래서 조건은 **레이아웃 단위**로
// 한 곳에 둔다:
//   · 컨트롤 위젯(`core.page-filter`)이 값을 쓰고 — 마운트 시 소유를 선언(usePageControl),
//   · 데이터 위젯이 usePageParam 으로 읽는다.
//   · 컨트롤 위젯이 그 페이지에 **없으면** 데이터 위젯은 예전처럼 자기 컨트롤을 쓴다(hasControl=false).
// 컨트롤 유무로 동작이 갈리므로 위젯에 설정을 추가하지 않아도 두 배치가 모두 성립한다.
//
// URL 쿼리와 양방향 — 딥링크/북마크가 조건까지 재현한다. **버스가 소유한 키만** 건드리므로
// 페이지가 쓰는 `?group=`·`?agent=`·`?t=`·`?q=` 와 충돌하지 않는다.

import { createContext, useCallback, useContext, useEffect, useMemo, useState } from 'react'
import type { ReactNode } from 'react'
import { useSearchParams } from 'react-router-dom'

// 버스가 소유하는 URL 쿼리 키. 새 파라미터를 늘릴 때 이 표에만 추가한다.
//   date/gran = 조회 기간·집계 단위 (core.page-filter 가 소유)
//   sev       = 심각도 필터 (심각도 타일이 쓰고 알람 목록이 읽는다. 빈 값 = 전체)
//   days      = 조회 일수 ([오늘][7일][30일][90일] — 이력·분석 위젯 안의 컨트롤, 또는 core.days-filter)
//   atab      = 알람/이벤트 전환 — 이력·분석 위젯이 읽어 본문을 갈아끼운다
//               (배치의 visibleWhen 으로 탭을 구성하는 레이아웃도 같은 값을 본다)
//   svc       = 선택된 서비스 정의 id (서비스 선택 컨트롤이 쓰고 모듈/규칙/소스 위젯이 읽는다)
//   src       = 선택된 데이터 소스 id (소스 선택 컨트롤이 쓰고 shape 위젯들이 읽는다 —
//               한 화면의 차트·표가 같은 대상을 함께 본다)
//   series    = 차트에 표시할 계열 키 목록(쉼표). 비어 있으면 전 계열 — 계열 선택 타일이 소유.
//   from/to   = 조회 구간(YYYY-MM-DD HH:MM). `date`(기준일 하나)를 대체한다 —
//               "8/28 에 '월' 단위" 처럼 기준일+단위 조합은 무엇을 보는지 모호했다.
//               구간을 먼저 정하고 그 안을 gran 단위로 쪼갠다.
export const PAGE_PARAM_KEYS =
  ['date', 'gran', 'sev', 'days', 'atab', 'svc', 'src', 'series', 'from', 'to'] as const
export type PageParamKey = (typeof PAGE_PARAM_KEYS)[number]

// 컨트롤 종류 — 어떤 파라미터 묶음을 누가 소유하는가.
//   'period' = date+gran (core.page-filter) / 'days' = 조회 일수 (core.days-filter)
//   'atab'   = 알람/이벤트 전환 (core.alarm-event-tabs)
//   'source' = 데이터 소스 (core.source-picker) — 있으면 shape 위젯이 자기 배치 소스 대신 이 값을 쓴다
//   'series' = 표시 계열 (core.series-select) — 있으면 계열 차트가 고른 것만 그린다
export type PageControlKind = 'period' | 'days' | 'atab' | 'svc' | 'source' | 'series'

export const todayIso = () => new Date().toISOString().slice(0, 10)

// 집계 단위 라벨 — 컨트롤 위젯과 shape 위젯이 같은 표를 쓴다.
export const GRAN_LABELS: Record<string, string> =
  { '5m': '5분', '10m': '10분', '1h': '1시간', '1d': '일', '1M': '월', '1y': '년' }

// 단위별 **최대 조회 범위(일)** — 버킷이 수천 개가 되면 차트도 못 읽고 스캔 비용만 는다.
// 기준은 버킷 800개 근처지만, 상한은 거기서 **사람이 말하는 창**으로 반올림했다(3일·일주일·한 달·2년).
// 그래서 실제 버킷 수는 기준을 조금 넘기도 한다: 5m=864 / 10m=1008 / 1h=720 / 1d=730.
// 서버도 **같은 표**를 적용한다 (handlers/stats.py `_GRAN_MAX_DAYS`) — 한쪽만 바꾸면 어긋난다.
// 1M/1y 는 사실상 무제한.
export const GRAN_MAX_DAYS: Record<string, number> =
  { '5m': 3, '10m': 7, '1h': 30, '1d': 730 }

// 구간이 이 단위로 감당 가능한가 — 컨트롤이 버튼 활성/비활성을 이걸로 정한다.
export function granFits(from: string, to: string, gran: string): boolean {
  const lim = GRAN_MAX_DAYS[gran]
  if (!lim) return true
  const f = Date.parse(from.replace(' ', 'T')), t = Date.parse(to.replace(' ', 'T'))
  if (!Number.isFinite(f) || !Number.isFinite(t)) return true
  return (t - f) / 86400000 <= lim
}

// 구간에 맞는 가장 세밀한 단위 — 프리셋 버튼이 범위를 바꿀 때 단위를 자동 승격한다.
export function bestGran(from: string, to: string): string {
  for (const g of ['5m', '10m', '1h', '1d', '1M', '1y']) {
    if (granFits(from, to, g)) return g
  }
  return '1y'
}

// 'YYYY-MM-DD HH:MM' — datetime-local 입력과 URL 양쪽에 쓰는 표기.
export const fmtDt = (d: Date) =>
  `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}` +
  ` ${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`

// 기본 구간 = 오늘 00:00 ~ 지금.
export function defaultRange(): { from: string; to: string } {
  const now = new Date()
  const midnight = new Date(now.getFullYear(), now.getMonth(), now.getDate())
  return { from: fmtDt(midnight), to: fmtDt(now) }
}

const DEFAULTS: Record<PageParamKey, string> =
  { date: '', gran: '1h', sev: '', days: '7', atab: 'alarms', svc: '', src: '', series: '', from: '', to: '' }

interface Ctx {
  params: Record<PageParamKey, string>
  setParam: (k: PageParamKey, v: string) => void
  controls: Record<string, number>            // kind → 마운트된 컨트롤 위젯 수
  acquire: (kind: PageControlKind) => void
  release: (kind: PageControlKind) => void
}

const PageParamsCtx = createContext<Ctx | null>(null)

export function PageParamsProvider({ children }: { children: ReactNode }) {
  const [search, setSearch] = useSearchParams()
  // 구간 기본값은 **마운트 때 한 번** 정한다. 매번 새로 계산하면 `지금`이 계속 움직여서,
  // 계열을 켜고 끄는 것처럼 표시만 바꾸는 조작이 조회 창까지 슬쩍 바꾼다 — 그러면 같은 화면의
  // 지표 타일과 차트가 서로 다른 끝시각으로 조회해 숫자가 어긋날 수 있다.
  // 창을 지금으로 당기는 건 컨트롤의 [오늘]·[↺] 가 명시적으로 한다(URL 에 값을 쓴다).
  const [fallbackRange] = useState(defaultRange)
  // URL 이 유일한 정본 — 별도 복제 상태를 두지 않는다(뒤로가기/링크 진입과 저절로 일치).
  const params = useMemo<Record<PageParamKey, string>>(() => ({
    date: search.get('date') || todayIso(),
    gran: search.get('gran') || DEFAULTS.gran,
    sev: search.get('sev') || DEFAULTS.sev,
    days: search.get('days') || DEFAULTS.days,
    atab: search.get('atab') || DEFAULTS.atab,
    series: search.get('series') ?? DEFAULTS.series,
    svc: search.get('svc') || DEFAULTS.svc,
    src: search.get('src') || DEFAULTS.src,
    // 구간은 URL 이 비면 기본값(오늘 00:00~지금)으로 채워 내려준다.
    from: search.get('from') || fallbackRange.from,
    to: search.get('to') || fallbackRange.to,
  }), [search, fallbackRange])
  const [controls, setControls] = useState<Record<string, number>>({})

  const setParam = useCallback((k: PageParamKey, v: string) => {
    // 버스 소유 키만 갱신 — 나머지 쿼리는 보존. replace 로 히스토리 오염 방지.
    setSearch(prev => {
      const next = new URLSearchParams(prev)
      if (v) next.set(k, v)
      else next.delete(k)
      return next
    }, { replace: true })
  }, [setSearch])

  const acquire = useCallback((kind: PageControlKind) => {
    setControls(c => ({ ...c, [kind]: (c[kind] ?? 0) + 1 }))
  }, [])
  const release = useCallback((kind: PageControlKind) => {
    setControls(c => ({ ...c, [kind]: Math.max(0, (c[kind] ?? 0) - 1) }))
  }, [])

  const value = useMemo<Ctx>(() => ({ params, setParam, controls, acquire, release }),
    [params, setParam, controls, acquire, release])
  return <PageParamsCtx.Provider value={value}>{children}</PageParamsCtx.Provider>
}

// 파라미터 1개 읽기/쓰기. Provider 밖(예: 단독 렌더)에서도 안전하게 동작하도록 로컬 폴백.
export function usePageParam(key: PageParamKey): [string, (v: string) => void] {
  const ctx = useContext(PageParamsCtx)
  const [fallback, setFallback] = useState(() =>
    key === 'date' ? todayIso()
      : key === 'from' ? defaultRange().from
      : key === 'to' ? defaultRange().to
      : DEFAULTS[key])
  if (!ctx) return [fallback, setFallback]
  return [ctx.params[key] ?? DEFAULTS[key], v => ctx.setParam(key, v)]
}

// 배치가 지금 보여야 하는가 — placement.visibleWhen 판정. 렌더러와 편집기가 같은 규칙을 쓴다.
export function isPlacementVisible(
  p: { visibleWhen?: { param: string; equals: string } }, params: Record<string, string>,
): boolean {
  return !p.visibleWhen || params[p.visibleWhen.param] === p.visibleWhen.equals
}

// 파라미터 전체 — 렌더러의 조건부 표시(placement.visibleWhen) 판정용.
export function usePageParams(): Record<string, string> {
  const ctx = useContext(PageParamsCtx)
  return ctx ? ctx.params : (DEFAULTS as Record<string, string>)
}

// 이 페이지에 해당 종류의 컨트롤 위젯이 있는가 — 데이터 위젯이 자기 컨트롤을 접을지 판단한다.
export function useHasPageControl(kind: PageControlKind): boolean {
  const ctx = useContext(PageParamsCtx)
  return !!ctx && (ctx.controls[kind] ?? 0) > 0
}

// 컨트롤 위젯이 자기 소유를 선언 — 마운트 동안만 유효(제거하면 데이터 위젯이 자기 컨트롤 복원).
export function usePageControl(kind: PageControlKind) {
  const ctx = useContext(PageParamsCtx)
  useEffect(() => {
    if (!ctx) return
    ctx.acquire(kind)
    return () => ctx.release(kind)
    // acquire/release 는 안정(useCallback) — kind 만 의존.
  }, [ctx?.acquire, ctx?.release, kind])   // eslint-disable-line react-hooks/exhaustive-deps
}
