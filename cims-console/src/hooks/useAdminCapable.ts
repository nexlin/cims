// useAdminCapable — admin 세션이거나 admin 승격(sudo) 활성인지.
// 시스템/인프라 탭1·2 의 편집 가능 여부 판정에 사용.
import { useEffect, useReducer } from 'react'
import { useAuth } from '../contexts/AuthContext'
import { elevationActive, onElevationChange } from '../api/client'

export function useAdminCapable(): boolean {
  const { user } = useAuth()
  const [, force] = useReducer((x: number) => x + 1, 0)
  useEffect(() => onElevationChange(force), [])
  return user?.role === 'admin' || elevationActive()
}
