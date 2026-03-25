import { createContext, useContext, useEffect, useState } from 'react'
import type { ReactNode } from 'react'
import { idmsLogin, idmsRefresh } from '../api/idms'
import { getUserProfile } from '../api/cms'

export interface McpttUser {
  mcptt_id:     string   // "tel:+82571900001"
  phone_number: string   // "+82571900001"
  display_name: string
  access_token:  string
  refresh_token: string
  password:      string  // kept for SIP registration
}

interface AuthCtx {
  user:    McpttUser | null
  loading: boolean
  login:   (phoneNumber: string, password: string) => Promise<void>
  logout:  () => void
}

const Ctx = createContext<AuthCtx>({ user: null, loading: true, login: async () => {}, logout: () => {} })

const SESSION_KEY = 'mcptt_session'

export function AuthProvider({ children }: { children: ReactNode }) {
  const [user,    setUser]    = useState<McpttUser | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    const raw = localStorage.getItem(SESSION_KEY)
    if (!raw) { setLoading(false); return }
    try {
      const session: McpttUser = JSON.parse(raw)
      idmsRefresh(session.refresh_token)
        .then(tokens => {
          const updated = { ...session, access_token: tokens.access_token, refresh_token: tokens.refresh_token }
          localStorage.setItem(SESSION_KEY, JSON.stringify(updated))
          setUser(updated)
        })
        .catch(() => localStorage.removeItem(SESSION_KEY))
        .finally(() => setLoading(false))
    } catch {
      localStorage.removeItem(SESSION_KEY)
      setLoading(false)
    }
  }, [])

  async function login(phoneNumber: string, password: string) {
    const tokens = await idmsLogin(phoneNumber, password)
    const phone  = tokens.mcptt_id.replace(/^tel:/, '')

    // Try to get display name from CMS; fall back to phone number
    let display_name = phone
    try {
      const profile = await getUserProfile(tokens.mcptt_id, tokens.access_token)
      display_name = profile.display_name || phone
    } catch { /* ignore */ }

    const mcpttUser: McpttUser = {
      mcptt_id:     tokens.mcptt_id,
      phone_number: phone,
      display_name,
      access_token:  tokens.access_token,
      refresh_token: tokens.refresh_token,
      password,
    }
    localStorage.setItem(SESSION_KEY, JSON.stringify(mcpttUser))
    setUser(mcpttUser)
  }

  function logout() {
    localStorage.removeItem(SESSION_KEY)
    setUser(null)
  }

  return <Ctx.Provider value={{ user, loading, login, logout }}>{children}</Ctx.Provider>
}

export function useAuth() { return useContext(Ctx) }
