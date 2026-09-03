// 내 대시보드 구성 — 편집 상태 store (모듈 싱글톤).
//
// 화면이 카드 하나로 묶여 있어도 **안의 블록은 각각 위젯**이라(console_platform §3.0.1) 상태를
// 컴포넌트 하나가 쥐고 있을 수 없다. 프로파일 블록이 고른 값을 위젯 목록 블록이 보고, 저장 버튼은
// 또 다른 블록에 있기 때문. 그래서 편집 중인 초안(dashboard/dirty/…)을 모듈 store 로 끌어올린다.
// (조회만 하는 화면은 makeSharedByKey 로 충분하지만, 여기는 **가변 초안**을 공유해야 한다.)
import { useEffect, useState } from 'react'
import {
  consoleLayoutsApi, type CatalogWidget, type ProfileTemplate,
} from '../api/consoleLayouts'

export interface MyLayoutState {
  loading: boolean
  catalog: CatalogWidget[]
  installed: string[]
  profiles: ProfileTemplate[]
  baseProfile: string
  source: 'override' | 'profile'
  dashboard: string[]      // 편집 중인 위젯 id 순서
  dirty: boolean
  saving: boolean
}

const INIT: MyLayoutState = {
  loading: true, catalog: [], installed: [], profiles: [],
  baseProfile: '', source: 'profile', dashboard: [], dirty: false, saving: false,
}

let state: MyLayoutState = INIT
const subs = new Set<() => void>()
function set(patch: Partial<MyLayoutState>) {
  state = { ...state, ...patch }
  subs.forEach(fn => fn())
}

// 블록이 몇 개든 조회는 1회 — 첫 구독자만 로드를 건다.
let started = false

export type Notify = (msg: string, kind?: 'ok' | 'err') => void

export const myLayout = {
  get: () => state,

  async load(notify?: Notify) {
    set({ loading: true })
    try {
      const [cat, prof, mine] = await Promise.all([
        consoleLayoutsApi.getCatalog(),
        consoleLayoutsApi.getProfiles(),
        consoleLayoutsApi.getMyLayout(),
      ])
      // 응답 필드가 비어 와도 화면은 살아 있어야 한다 — 카탈로그/프로파일이 없으면 "없음"으로
      // 보이면 될 뿐, 렌더가 죽으면 되돌릴 방법까지 사라진다.
      set({
        catalog: cat.widgets ?? [], installed: cat.installed_services ?? [],
        profiles: prof.profiles ?? [],
        baseProfile: mine.base_profile ?? '', source: mine.source ?? 'profile',
        dashboard: mine.layout?.widgets?.dashboard ?? mine.layout?.pages?.[0]?.widgets ?? [],
        dirty: false,
      })
    } catch (e) {
      notify?.((e as Error).message, 'err')
    } finally { set({ loading: false }) }
  },

  setBaseProfile: (id: string) => set({ baseProfile: id }),

  // 프로파일 적용 = 그 템플릿의 위젯 세트로 교체(초안만 바뀐다 — 저장은 별도).
  applyProfile(id: string) {
    const p = state.profiles.find(x => x.id === id)
    if (!p) return
    set({ baseProfile: id, dashboard: [...p.dashboard], dirty: true })
  },

  add(id: string) {
    if (!id || state.dashboard.includes(id)) return
    set({ dashboard: [...state.dashboard, id], dirty: true })
  },
  remove(i: number) {
    set({ dashboard: state.dashboard.filter((_, k) => k !== i), dirty: true })
  },
  move(i: number, dir: -1 | 1) {
    const j = i + dir
    if (j < 0 || j >= state.dashboard.length) return
    const next = [...state.dashboard]
    ;[next[i], next[j]] = [next[j], next[i]]
    set({ dashboard: next, dirty: true })
  },

  async save(notify?: Notify) {
    set({ saving: true })
    try {
      await consoleLayoutsApi.saveMyLayout({
        base_profile: state.baseProfile,
        layout: { pages: [{ slug: '/dashboard', widgets: state.dashboard }],
                  widgets: { dashboard: state.dashboard } },
      })
      notify?.('내 대시보드 구성 저장됨', 'ok')
      set({ source: 'override', dirty: false })
    } catch (e) {
      notify?.((e as Error).message, 'err')   // 서버 RBAC 거부(403)/미존재(400) 포함
    } finally { set({ saving: false }) }
  },

  async reset(notify?: Notify) {
    set({ saving: true })
    try {
      await consoleLayoutsApi.resetMyLayout()
      notify?.('프로파일 기본값으로 초기화됨', 'ok')
      await myLayout.load(notify)
    } catch (e) {
      notify?.((e as Error).message, 'err')
    } finally { set({ saving: false }) }
  },
}

// 구독 훅 — 첫 사용 시 1회 로드.
export function useMyLayout(notify?: Notify): MyLayoutState {
  const [, tick] = useState(0)
  useEffect(() => {
    const fn = () => tick(t => t + 1)
    subs.add(fn)
    if (!started) { started = true; void myLayout.load(notify) }
    return () => { subs.delete(fn) }
    // notify 는 첫 로드에만 쓰인다 — 재구독 이유가 아니다.
  }, [])   // eslint-disable-line react-hooks/exhaustive-deps
  return state
}
