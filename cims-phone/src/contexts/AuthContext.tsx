import { createContext, useContext, useEffect, useState } from 'react'
import type { ReactNode } from 'react'
import { authApi } from '../api/auth'
import type { CimsUser, Subscription } from '../api/auth'

// v3 (2026-04-22): Phone 은 /users/me + /users/me/subscriptions 2회 호출.
//   AuthContext 가 둘 다 로드해 보관.
interface AuthCtx {
  user:    CimsUser | null
  callSubscriptions: Subscription[]
  pttSubscriptions:  Subscription[]
  loading: boolean
  login:   (loginId: string, password: string) => Promise<void>
  logout:  () => void
}

const Ctx = createContext<AuthCtx>({
  user: null, callSubscriptions: [], pttSubscriptions: [],
  loading: true, login: async () => {}, logout: () => {},
})

const TOKEN_KEY = 'cims_token'

export function AuthProvider({ children }: { children: ReactNode }) {
  const [user,    setUser]    = useState<CimsUser | null>(null)
  const [callSubs, setCallSubs] = useState<Subscription[]>([])
  const [pttSubs,  setPttSubs]  = useState<Subscription[]>([])
  const [loading, setLoading] = useState(true)

  async function _loadProfileAndSubs() {
    const [u, subs] = await Promise.all([
      authApi.me(),
      authApi.mySubscriptions(),
    ])
    setUser(u)
    setCallSubs(subs.call_subscriptions || [])
    setPttSubs(subs.ptt_subscriptions || [])
  }

  useEffect(() => {
    const token = localStorage.getItem(TOKEN_KEY)
    if (!token) { setLoading(false); return }
    _loadProfileAndSubs()
      .catch(() => localStorage.removeItem(TOKEN_KEY))
      .finally(() => setLoading(false))
  }, [])

  async function login(loginId: string, password: string) {
    const { token, user } = await authApi.login(loginId, password)
    localStorage.setItem(TOKEN_KEY, token)
    setUser(user)
    // subscriptions 는 별도 호출
    const subs = await authApi.mySubscriptions()
    setCallSubs(subs.call_subscriptions || [])
    setPttSubs(subs.ptt_subscriptions || [])
  }

  function logout() {
    localStorage.removeItem(TOKEN_KEY)
    setUser(null)
    setCallSubs([])
    setPttSubs([])
  }

  return <Ctx.Provider value={{
    user, callSubscriptions: callSubs, pttSubscriptions: pttSubs,
    loading, login, logout,
  }}>{children}</Ctx.Provider>
}

export function useAuth() { return useContext(Ctx) }
export type { CimsUser }
