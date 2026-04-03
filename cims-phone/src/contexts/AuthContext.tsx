import { createContext, useContext, useEffect, useState } from 'react'
import type { ReactNode } from 'react'
import { authApi } from '../api/auth'
import type { CimsUser } from '../api/auth'

interface AuthCtx {
  user:    CimsUser | null
  loading: boolean
  login:   (loginId: string, password: string) => Promise<void>
  logout:  () => void
}

const Ctx = createContext<AuthCtx>({
  user: null, loading: true, login: async () => {}, logout: () => {},
})

const TOKEN_KEY = 'cims_token'

export function AuthProvider({ children }: { children: ReactNode }) {
  const [user,    setUser]    = useState<CimsUser | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    const token = localStorage.getItem(TOKEN_KEY)
    if (!token) { setLoading(false); return }
    authApi.me()
      .then(u => setUser(u))
      .catch(() => localStorage.removeItem(TOKEN_KEY))
      .finally(() => setLoading(false))
  }, [])

  async function login(loginId: string, password: string) {
    const { token, user } = await authApi.login(loginId, password)
    localStorage.setItem(TOKEN_KEY, token)
    setUser(user)
  }

  function logout() {
    localStorage.removeItem(TOKEN_KEY)
    setUser(null)
  }

  return <Ctx.Provider value={{ user, loading, login, logout }}>{children}</Ctx.Provider>
}

export function useAuth() { return useContext(Ctx) }
export type { CimsUser }
