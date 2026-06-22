import { useEffect, useReducer } from 'react'
import { devModeActive, onDevModeChange } from '../utils/devMode'

export function useDevMode(): boolean {
  const [, force] = useReducer((x: number) => x + 1, 0)
  useEffect(() => onDevModeChange(force), [])
  return devModeActive()
}
