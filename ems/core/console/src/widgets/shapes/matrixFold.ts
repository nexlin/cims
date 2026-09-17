// 교차표의 **값 없는 구간 접기** — 표시 로직이지만 규칙이라 따로 두고 시험으로 덮는다.
import type { MatrixData } from './types'

/** 이보다 짧은 빈 구간은 접지 않는다 — 접은 줄이 원래 줄보다 길어지고, 몇 줄 건너뛰는 것은
 *  눈으로 따라가는 데 방해가 안 된다. */
export const EMPTY_RUN_MIN = 10

export interface MatrixChunk {
  /** 이 묶음이 전부 빈 행인가 */
  blank: boolean
  /** 접어서 한 줄로 낼 묶음인가 (빈 행이 EMPTY_RUN_MIN 이상 이어질 때) */
  fold: boolean
  rows: MatrixData['rows']
}

/**
 * 행을 **연속된 빈 구간 / 값 있는 구간**으로 묶는다.
 *
 * 시간축을 구간 전체로 채우면 축은 균일해지지만(sip_statistics.md §2.1c) 1분 단위 하루
 * 조회는 900행이 되고 그중 대부분이 0 이라, 값이 있는 몇 줄이 빈 줄 사이에 파묻힌다.
 * 긴 빈 구간을 한 줄로 접으면 값 있는 행만 남아 표가 읽힌다 — 접힌 줄은 **조회 직후 기본
 * 상태**이고, 그 구간을 실제로 들여다볼 때만 펼친다.
 *
 * "빈 행" 의 기준은 **모든 열이 0 이거나 값 없음**이다. 0 은 사실이지만(그 구간에 아무 일도
 * 없었다) 전부 0 인 행은 읽을 것이 없다.
 */
export function foldBlankRuns(rows: MatrixData['rows'], columns: MatrixData['columns'],
                              min: number = EMPTY_RUN_MIN): MatrixChunk[] {
  const blank = (r: MatrixData['rows'][number]) =>
    columns.every(c => {
      const v = r.cells[c.key]
      return v === null || v === undefined || v === 0
    })
  const out: MatrixChunk[] = []
  for (const r of rows) {
    const b = blank(r)
    const last = out[out.length - 1]
    if (last && last.blank === b) last.rows.push(r)
    else out.push({ blank: b, fold: false, rows: [r] })
  }
  for (const g of out) g.fold = g.blank && g.rows.length >= min
  return out
}

/**
 * 접힌 구간의 범위 표기 — `13:24 ~ 13:33`.
 *
 * 같은 날 안이면 날짜를 뗀다(1분·5분·시간 단위는 한 화면이 대개 하루 안이라 날짜가 되풀이되면
 * 읽는 데 방해만 된다). 자정을 넘거나 일·주·월·연 단위처럼 라벨이 날짜면 **그대로** 낸다 —
 * 떼면 `11 ~ 14` 가 되어 무엇인지 알 수 없다.
 */
export function blankRange(rows: MatrixData['rows']): string {
  const a = String(rows[0]?.label ?? '')
  const b = String(rows[rows.length - 1]?.label ?? '')
  const day = /^(\d{4}-\d{2}-\d{2}) /
  const ma = a.match(day), mb = b.match(day)
  if (ma && mb && ma[1] === mb[1]) return `${a.slice(11)} ~ ${b.slice(11)}`
  return a === b ? a : `${a} ~ ${b}`
}
