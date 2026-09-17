// 편집 이력 — 캔버스 편집기(토폴로지·시나리오)의 문서 스냅샷 스택. 문서는 페이지가 소유하고, 캔버스는 바뀐 사본을 올려보낸다.
// `set(next)` 가 한 걸음, `set(next, true)` 는 이어지는 걸음(영역 끌기·크기 조절처럼 pointermove 마다 오는 변경 — 첫 걸음만 쌓고
// 다음은 덮어쓴다; `commit()` 이 그 묶음을 닫는다). `reset` 은 레코드 전환·저장 뒤 — 이력을 비운다. Ctrl+Z / Ctrl+Y·Ctrl+Shift+Z 는 페이지가 건다.
import { useCallback, useEffect, useRef, useState } from 'react'

const LIMIT = 100

export function useDocHistory<T>() {
  const [st, setSt] = useState<{ past: string[]; now: T | null; future: string[] }>({ past: [], now: null, future: [] })
  const open = useRef(false)      // 이어지는 걸음 묶음이 열려 있는가

  const reset = useCallback((d: T | null) => { open.current = false; setSt({ past: [], now: d, future: [] }) }, [])
  const set = useCallback((d: T, transient = false) => {
    setSt(s => {
      if (s.now == null) return { past: [], now: d, future: [] }
      if (transient && open.current) return { ...s, now: d }
      open.current = transient
      return { past: [...s.past.slice(-(LIMIT - 1)), JSON.stringify(s.now)], now: d, future: [] }
    })
  }, [])
  /** 배치 전용 변경(영역 자동 확장) — 이력에 안 쌓고 현재만 바꾼다 */
  const replace = useCallback((d: T) => setSt(s => ({ ...s, now: d })), [])
  const commit = useCallback(() => { open.current = false }, [])
  const undo = useCallback(() => { open.current = false; setSt(s => s.past.length ? { past: s.past.slice(0, -1), now: JSON.parse(s.past[s.past.length - 1]) as T, future: [JSON.stringify(s.now), ...s.future].slice(0, LIMIT) } : s) }, [])
  const redo = useCallback(() => { open.current = false; setSt(s => s.future.length ? { past: [...s.past, JSON.stringify(s.now)].slice(-LIMIT), now: JSON.parse(s.future[0]) as T, future: s.future.slice(1) } : s) }, [])

  return { doc: st.now, set, replace, commit, reset, undo, redo, canUndo: st.past.length > 0, canRedo: st.future.length > 0 }
}

/** Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z — 입력칸 안에서는 브라우저 기본(글자 되돌리기)에 맡긴다 */
export function useUndoKeys(undo: () => void, redo: () => void, enabled = true) {
  useEffect(() => {
    if (!enabled) return
    const kd = (e: KeyboardEvent) => {
      if (!(e.ctrlKey || e.metaKey)) return
      if ((e.target as HTMLElement).closest('input,select,textarea,[contenteditable]')) return
      const k = e.key.toLowerCase()
      if (k === 'z' && !e.shiftKey) { e.preventDefault(); undo() }
      else if (k === 'y' || (k === 'z' && e.shiftKey)) { e.preventDefault(); redo() }
    }
    window.addEventListener('keydown', kd); return () => window.removeEventListener('keydown', kd)
  }, [undo, redo, enabled])
}

/** 저장 안 한 변경이 있을 때 탭 닫기·새로고침 경고 */
export function useUnsavedGuard(dirty: boolean) {
  useEffect(() => {
    if (!dirty) return
    const h = (e: BeforeUnloadEvent) => { e.preventDefault(); e.returnValue = '' }
    window.addEventListener('beforeunload', h); return () => window.removeEventListener('beforeunload', h)
  }, [dirty])
}
