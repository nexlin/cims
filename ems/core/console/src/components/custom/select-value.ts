/**
 * Radix Select 는 **빈 문자열 value 를 못 쓴다** — 빈 값은 placeholder 전용이라
 * `<SelectItem value="">` 은 예외를 던진다. 그런데 콘솔의 필터·선택 폼은 `''` 를
 * 「전체 / 미지정」의 값으로 쓰는 곳이 많다(네이티브 `<option value="">전체</option>`).
 *
 * 상태의 뜻은 그대로 두고 **컴포넌트 경계에서만** 표식으로 바꿔 끼운다 —
 * 저장되는 값도, API 로 나가는 값도 여전히 `''` 다.
 *
 *   <Select value={toSel(f.code)} onValueChange={v => set({ code: fromSel(v) })}>
 *     <SelectItem value={NONE}>전체</SelectItem>
 */
export const NONE = '__none__'

/** 상태값 → Select 의 value. `''`·`null`·`undefined` 를 표식으로 바꾼다. */
export const toSel = (v: string | null | undefined): string => (v == null || v === '' ? NONE : v)

/** Select 가 준 value → 상태값. 표식을 다시 `''` 로 되돌린다. */
export const fromSel = (v: string): string => (v === NONE ? '' : v)
