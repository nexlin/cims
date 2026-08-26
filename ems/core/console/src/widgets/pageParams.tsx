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
//   sev       = 심각도 필터 (심각도 요약 타일이 쓰고 알람 목록이 읽는다. 빈 값 = 전체)
//   days      = 조회 일수 (분석 화면의 [7일][30일][90일] 컨트롤이 소유)
//   atab      = 분석 대상 전환 (알람/이벤트) — placement.visibleWhen 이 읽어 탭처럼 동작
//   svc       = 선택된 서비스 정의 id (서비스 선택 컨트롤이 쓰고 모듈/규칙/소스 위젯이 읽는다)
export const PAGE_PARAM_KEYS = ['date', 'gran', 'sev', 'days', 'atab', 'svc'] as const
export type PageParamKey = (typeof PAGE_PARAM_KEYS)[number]

// 컨트롤 종류 — 어떤 파라미터 묶음을 누가 소유하는가.
//   'period' = date+gran (core.page-filter) / 'days' = 조회 일수 (core.days-filter)
export type PageControlKind = 'period' | 'days' | 'atab' | 'svc'

export const todayIso = () => new Date().toISOString().slice(0, 10)

// 집계 단위 라벨 — 컨트롤 위젯과 shape 위젯이 같은 표를 쓴다.
export const GRAN_LABELS: Record<string, string> = { '1h': '시간', '1d': '일', '1M': '월' }

const DEFAULTS: Record<PageParamKey, string> =
  { date: '', gran: '1h', sev: '', days: '7', atab: 'alarms', svc: '' }

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
  // URL 이 유일한 정본 — 별도 복제 상태를 두지 않는다(뒤로가기/링크 진입과 저절로 일치).
  const params = useMemo<Record<PageParamKey, string>>(() => ({
    date: search.get('date') || todayIso(),
    gran: search.get('gran') || DEFAULTS.gran,
    sev: search.get('sev') || DEFAULTS.sev,
    days: search.get('days') || DEFAULTS.days,
    atab: search.get('atab') || DEFAULTS.atab,
    svc: search.get('svc') || DEFAULTS.svc,
  }), [search])
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
  const [fallback, setFallback] = useState(() => (key === 'date' ? todayIso() : DEFAULTS[key]))
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
