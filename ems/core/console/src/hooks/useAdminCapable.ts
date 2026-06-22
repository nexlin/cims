// useAdminCapable — admin 세션이거나 admin 승격(sudo) 활성인지.
// 시스템/인프라 탭1·2 의 편집 가능 여부 판정에 사용.
import { useEffect, useReducer } from 'react'
import { useAuth } from '../contexts/AuthContext'
import { elevationActive, onElevationChange } from '../api/client'
import { hasRole } from '../utils/permissions'

export function useAdminCapable(): boolean {
  const { user } = useAuth()
  const [, force] = useReducer((x: number) => x + 1, 0)
  useEffect(() => onElevationChange(force), [])
  // rank 기반 — developer(공급사 개발 계정, admin 동급) 포함
  return hasRole(user, 'admin') || elevationActive()
}
