// 데스크톱 폭 여부 훅 — 위젯 그리드 편집기(드래그/리사이즈)를 좁은 화면에서 숨기는 게이팅용.
// 뷰 모드는 좁은 화면에서도 단일열로 동작(index.css @media)하지만, 편집은 데스크톱 전용.
import { useEffect, useState } from 'react'

const QUERY = '(min-width: 900px)'   // GridRenderer 단일열 collapse 브레이크포인트와 일치

export function useIsDesktop(): boolean {
  const [desktop, setDesktop] = useState(() =>
    typeof window !== 'undefined' && typeof window.matchMedia === 'function'
      ? window.matchMedia(QUERY).matches
      : true)
  useEffect(() => {
    if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') return
    const mql = window.matchMedia(QUERY)
    const on = () => setDesktop(mql.matches)
    on()
    mql.addEventListener('change', on)
    return () => mql.removeEventListener('change', on)
  }, [])
  return desktop
}
