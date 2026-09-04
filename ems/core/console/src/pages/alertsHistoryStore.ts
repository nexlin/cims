// 알람·이벤트 이력 — 조회 필터 store (모듈 싱글톤).
//
// 화면이 카드 하나로 묶여 있어도 **안의 블록은 각각 위젯**이다(console_platform §3.0.1).
// 필터 툴바와 표를 다른 블록으로 두면(운영자 요청: 필터는 기간 선택 옆줄, 표는 아래를 다 쓴다)
// 둘이 같은 필터를 봐야 하므로 상태를 여기로 끌어올린다.
// 조회 자체는 `days` 를 키로 공유하므로(makeSharedByKey) 블록이 몇 개든 요청은 1회다.
import { useEffect, useState } from 'react'

// `live` 는 조회 **조건**이 아니라 보기 방식이다 — 켜고 끄는 것으로 페이지가 처음으로
// 돌아가면 3쪽을 보다 켠 운영자가 자리를 잃는다. page 와 함께 리셋 대상에서 뺀다.
export interface AlarmFilter {
  sev: string; code: string; type: string; q: string; showResolved: boolean; page: number
  live: boolean
}
export interface EventFilter {
  kind: string; type: string; q: string; page: number; live: boolean
}

const INIT_ALARM: AlarmFilter = { sev: '', code: '', type: '', q: '', showResolved: true, page: 0,
                                  live: false }
const INIT_EVENT: EventFilter = { kind: '', type: '', q: '', page: 0, live: false }

const VIEW_KEYS = ['page', 'live'] as const
const keepsPage = (patch: object) => VIEW_KEYS.some(k => k in patch)

let alarmF: AlarmFilter = INIT_ALARM
let eventF: EventFilter = INIT_EVENT
const subs = new Set<() => void>()
const notify = () => subs.forEach(fn => fn())

export const alertsFilter = {
  alarm: () => alarmF,
  event: () => eventF,
  // 필터를 건드리면 페이지는 처음으로 — 3쪽을 보다 조건을 바꾸면 빈 쪽이 나온다.
  setAlarm(patch: Partial<AlarmFilter>) {
    alarmF = { ...alarmF, ...patch, ...(keepsPage(patch) ? {} : { page: 0 }) }
    notify()
  },
  setEvent(patch: Partial<EventFilter>) {
    eventF = { ...eventF, ...patch, ...(keepsPage(patch) ? {} : { page: 0 }) }
    notify()
  },
}

function useFilterTick() {
  const [, tick] = useState(0)
  useEffect(() => {
    const fn = () => tick(t => t + 1)
    subs.add(fn)
    return () => { subs.delete(fn) }
  }, [])
}

export function useAlarmFilter(): AlarmFilter { useFilterTick(); return alarmF }
export function useEventFilter(): EventFilter { useFilterTick(); return eventF }
