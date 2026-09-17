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
