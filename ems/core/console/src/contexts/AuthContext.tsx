import { createContext, useContext, useEffect, useState } from 'react'
import type { ReactNode } from 'react'
import type { AuthUser } from '../api/auth'
import { authApi } from '../api/auth'

interface AuthCtx {
  user: AuthUser | null
  loading: boolean
  login: (token: string, user: AuthUser) => void
  logout: () => void
  refresh: () => Promise<void>
}

const AuthContext = createContext<AuthCtx>({
  user: null, loading: true,
  login: () => {}, logout: () => {}, refresh: async () => {},
})

export function AuthProvider({ children }: { children: ReactNode }) {
  const [user, setUser] = useState<AuthUser | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    const token = localStorage.getItem('cims_token')
    if (!token) { setLoading(false); return }
    authApi.me()
      .then(u => setUser(u))
      .catch(() => localStorage.removeItem('cims_token'))
      .finally(() => setLoading(false))
  }, [])

  function login(token: string, u: AuthUser) {
    localStorage.setItem('cims_token', token)
    setUser(u)
  }

  function logout() {
    localStorage.removeItem('cims_token')
    setUser(null)
  }

  async function refresh() {
    const u = await authApi.me()
    setUser(u)
  }

  return (
    <AuthContext.Provider value={{ user, loading, login, logout, refresh }}>
      {children}
    </AuthContext.Provider>
  )
}

export function useAuth() {
  return useContext(AuthContext)
}
