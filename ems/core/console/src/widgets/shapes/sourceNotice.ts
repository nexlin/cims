// 조회가 "못 본 구간" 을 알리는 통로 — 데이터 소스 → 페이지 상단 띠.
//
// 서버는 구간 조회에 `warning` 과 `coverage.missing_days` 를 실어 보낸다: 집계도 원본도 없어
// 제외된 날들이다(sip_statistics.md §7.2). 표는 그 날의 행을 `—` 로 그리지만(§2.1c), **표를
// 훑어야 보인다** — 지표 타일만 보고 "이 구간 성공률 91.7%" 로 읽는 사람은 그 값이 18일 중
// 10일치라는 것을 모른다. 그래서 화면 위쪽에 한 줄로 올린다.
//
// 띠를 그리는 곳(페이지 렌더러)과 사실을 아는 곳(소스를 fetch 하는 shape 위젯)이 떨어져 있어
// 모듈 단위로 공유한다 — 같은 조건을 보는 블록이 여럿이어도 문구는 하나다(중복 제거).
//
// **조회 조건이 키의 일부**다. 구간을 바꾸면 옛 조건의 문구는 저절로 화면에서 빠진다 —
// 지운 시점을 따로 관리하지 않아도 되고, 갱신 중에 옛 경고가 남아 있는 상태가 생기지 않는다.
import { useEffect, useReducer } from 'react'

const notices = new Map<string, string>()
const subs = new Set<() => void>()
const MAX_KEYS = 40          // 조건을 바꿔 가며 보는 세션에서 무한정 쌓이지 않게

export function noticeScope(from: string, to: string, gran: string): string {
  return `${from}|${to}|${gran}`
}

/** 소스 응답의 경고를 싣는다(없으면 지운다). `key` = `${scope}|${sourceId}`. */
export function publishNotice(key: string, text: string | null): void {
  const cur = notices.get(key)
  if (text) {
    if (cur === text) return
    notices.set(key, text)
  } else {
    if (cur === undefined) return
    notices.delete(key)
  }
  while (notices.size > MAX_KEYS) {
    const oldest = notices.keys().next().value
    if (oldest === undefined) break
    notices.delete(oldest)
  }
  subs.forEach(f => f())
}

/** 그 조회 조건에 달린 경고들(중복 제거) — 순수 계산. 화면은 아래 훅을 쓴다. */
export function noticesFor(scope: string): string[] {
  const out: string[] = []
  for (const [k, v] of notices) {
    if (k.startsWith(`${scope}|`) && !out.includes(v)) out.push(v)
  }
  return out
}

/** 지금 조회 조건에 달린 경고들. 갱신되면 다시 그린다. */
export function useNotices(scope: string): string[] {
  const [, bump] = useReducer((x: number) => x + 1, 0)
  useEffect(() => {
    subs.add(bump)
    return () => { subs.delete(bump) }
  }, [])
  return noticesFor(scope)
}

/** 응답에서 띠에 쓸 문구를 만든다 — 없으면 null. 빠진 날짜를 함께 적는다(어느 날인지가 조치다). */
export function noticeOf(raw: unknown): string | null {
  const r = raw as { warning?: unknown; coverage?: { missing_days?: unknown } } | null
  const warning = typeof r?.warning === 'string' ? r.warning : ''
  if (!warning) return null
  const days = Array.isArray(r?.coverage?.missing_days) ? r.coverage.missing_days as unknown[] : []
  if (days.length === 0) return warning
  // 날짜는 월-일만 적는다(구간이 이미 화면에 있다). 너무 길면 뒤를 접는다.
  const shown = days.slice(0, 12).map(d => String(d).slice(5)).join(' · ')
  const more = days.length > 12 ? ` 외 ${days.length - 12}일` : ''
  return `${warning} (${shown}${more})`
}
