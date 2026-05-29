// 저장된 layout 로드 → 없거나(404) 오류면 seed 유지. (5-3 step2-1: 읽기 전용. 편집/저장은 step2-2)
import { useState, useEffect } from 'react'
import { consoleApi } from '../api/console'
import type { PageLayout } from './types'

export function useStoredLayout(seed: PageLayout): PageLayout {
  const [layout, setLayout] = useState<PageLayout>(seed)
  useEffect(() => {
    let alive = true
    consoleApi.getLayout(seed.id)
      .then(l => { if (alive && l && Array.isArray(l.widgets)) setLayout(l) })
      .catch(() => { /* 404/오류 → seed 유지 */ })
    return () => { alive = false }
  }, [seed.id])
  return layout
}
