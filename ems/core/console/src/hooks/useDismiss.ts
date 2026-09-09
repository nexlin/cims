import { useEffect, type RefObject } from 'react'

/**
 * 바깥 클릭 · Esc 로 닫는다.
 *
 * 헤더의 톱니바퀴·계정 메뉴는 Radix `DropdownMenu` 라 이 동작을 공짜로 얻는다.
 * 시트 2 에 **Drawer/Popover 가 없어서** 직접 만든 패널(알람 드로어 · ⓘ 말풍선)은
 * 그 동작을 스스로 갖춰야 한다 — 안 그러면 열어 둔 채 다른 조작을 하게 되고 시야를 가린다.
 * (알람 드로어가 실제로 그랬다 — 벨을 다시 누르거나 ✕ 를 눌러야만 닫혔다.)
 *
 * `box` 안의 클릭은 무시한다. 트리거도 같은 `box` 안에 두면 「다시 눌러 닫기」가
 * 두 번 처리되지 않는다.
 */
export function useDismiss(open: boolean, box: RefObject<HTMLElement | null>, close: () => void) {
  useEffect(() => {
    if (!open) return
    const onDown = (e: MouseEvent) => {
      if (box.current && !box.current.contains(e.target as Node)) close()
    }
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') close() }
    document.addEventListener('mousedown', onDown)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', onDown)
      document.removeEventListener('keydown', onKey)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open])
}
