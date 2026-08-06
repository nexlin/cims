/* useDirectory — 번호(MSISDN) → 사람 사전
 *
 * 이력·녹취 화면은 번호가 아니라 사람으로 읽혀야 한다. 표시는 이름, 번호는 hover 로
 * 확인한다(tip). 사전의 출처는 가입자 목록 한 벌(`/users`)이고 모듈 캐시에 둔다 —
 * 여러 화면·여러 컴포넌트가 같은 사전을 보므로 요청은 페이지 로드당 한 번이다.
 *
 * 사전에 없는 번호(퇴사자·타 시스템·미등록 단말)는 번호를 그대로 보여준다 — 이력은
 * 그때의 사실이라 지금 사전에 없다고 표시가 비면 안 된다.
 */
import { useEffect, useMemo, useState } from 'react'
import { usersApi, type UserSummary } from '../api/users'

export interface DirPerson {
  userId: number
  name: string
  title?: string      // 직함 (예: 팀장)
  org?: string        // 조직 코드
}

export interface Directory {
  /** 사전 적재 완료 여부 (미완이면 번호로 폴백 중) */
  ready: boolean
  /** 번호 → 가입자. 미등록 번호는 undefined */
  person: (msisdn: string) => DirPerson | undefined
  /** 표시명 — 사전에 없으면 번호 그대로 */
  nameOf: (msisdn: string) => string
  /** hover 툴팁 — '이름(직함) · 번호'. 사전에 없으면 번호만 */
  tipOf: (msisdn: string) => string
}

type DirMap = Map<string, DirPerson>

let cached: DirMap | null = null
let inflight: Promise<DirMap> | null = null

function build(users: UserSummary[]): DirMap {
  const m: DirMap = new Map()
  for (const u of users) {
    const p: DirPerson = {
      userId: u.id, name: u.name,
      title: u.title || undefined, org: u.org_id || undefined,
    }
    for (const s of [...(u.ptt_subscriptions || []), ...(u.call_subscriptions || [])]) {
      if (s.id && !m.has(s.id)) m.set(s.id, p)
    }
  }
  return m
}

/** 사전 적재 (모듈 캐시 · 동시 호출은 한 요청으로 합류) */
export function loadDirectory(): Promise<DirMap> {
  if (cached) return Promise.resolve(cached)
  if (!inflight) {
    inflight = usersApi.list()
      // 권한이 없거나(가입자 조회 불가 롤) 조회가 실패하면 빈 사전 — 화면은 번호로 산다.
      .catch(() => [] as UserSummary[])
      .then(us => { cached = build(us); inflight = null; return cached })
  }
  return inflight
}

/** 가입자 편집 후 등 사전을 다시 읽어야 할 때 */
export function resetDirectory() {
  cached = null
  inflight = null
}

export function directoryOf(map: DirMap | null): Directory {
  const person = (msisdn: string) => (msisdn ? map?.get(msisdn) : undefined)
  return {
    ready: !!map,
    person,
    nameOf: (msisdn: string) => person(msisdn)?.name || msisdn || '—',
    tipOf: (msisdn: string) => {
      const p = person(msisdn)
      if (!p) return msisdn || ''
      return `${p.name}${p.title ? `(${p.title})` : ''} · ${msisdn}`
    },
  }
}

export function useDirectory(): Directory {
  const [map, setMap] = useState<DirMap | null>(cached)
  useEffect(() => {
    if (map) return
    let alive = true
    loadDirectory().then(m => { if (alive) setMap(m) })
    return () => { alive = false }
  }, [map])
  return useMemo(() => directoryOf(map), [map])
}
