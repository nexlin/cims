// 하단 드로어 높이 — 손잡이를 끌어 조절, localStorage 에 기억(편집기마다 키 하나). 반환 = [높이, 손잡이 pointerdown 핸들러].
import { useCallback, useEffect, useRef, useState } from 'react'

export function useDrawerHeight(key: string, initial = 240, min = 120, max = 640): [number, (e: React.PointerEvent) => void] {
  const [h, setH] = useState(() => { try { const v = Number(localStorage.getItem(key)); return v >= min && v <= max ? v : initial } catch { return initial } })
  const drag = useRef<{ y: number; h: number } | null>(null)
  useEffect(() => { try { localStorage.setItem(key, String(h)) } catch { /* 무시 */ } }, [key, h])
  const onDown = useCallback((e: React.PointerEvent) => {
    e.preventDefault(); drag.current = { y: e.clientY, h }
    const move = (ev: PointerEvent) => { const d = drag.current; if (!d) return; setH(Math.max(min, Math.min(max, d.h + (d.y - ev.clientY)))) }
    const up = () => { drag.current = null; window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up) }
    window.addEventListener('pointermove', move); window.addEventListener('pointerup', up)
  }, [h, min, max])
  return [h, onDown]
}

/** 옆 패널 폭 — 왼쪽 가장자리 손잡이를 끌어 조절, localStorage 에 기억. 반환 = [폭, 손잡이 pointerdown 핸들러]. 손잡이가 패널 왼쪽에 있어 왼쪽으로 끌면 넓어진다 */
export function usePanelWidth(key: string, initial = 320, min = 260, max = 600): [number, (e: React.PointerEvent) => void] {
  const [w, setW] = useState(() => { try { const v = Number(localStorage.getItem(key)); return v >= min && v <= max ? v : initial } catch { return initial } })
  const drag = useRef<{ x: number; w: number } | null>(null)
  useEffect(() => { try { localStorage.setItem(key, String(w)) } catch { /* 무시 */ } }, [key, w])
  const onDown = useCallback((e: React.PointerEvent) => {
    e.preventDefault(); drag.current = { x: e.clientX, w }
    const move = (ev: PointerEvent) => { const d = drag.current; if (!d) return; setW(Math.max(min, Math.min(max, d.w + (d.x - ev.clientX)))) }
    const up = () => { drag.current = null; window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up) }
    window.addEventListener('pointermove', move); window.addEventListener('pointerup', up)
  }, [w, min, max])
  return [w, onDown]
}
