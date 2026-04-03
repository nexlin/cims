import { useState } from 'react'
import { useAuth } from '../contexts/AuthContext'

export default function LoginPage() {
  const { login } = useAuth()
  const [loginId, setLoginId] = useState('')
  const [pw,      setPw]      = useState('')
  const [error,   setError]   = useState('')
  const [loading, setLoading] = useState(false)

  async function handleLogin(e: React.FormEvent) {
    e.preventDefault()
    setError('')
    if (!loginId || !pw) { setError('아이디와 비밀번호를 입력하세요'); return }
    setLoading(true)
    try {
      await login(loginId, pw)
    } catch (err: unknown) {
      setError((err as Error).message)
    } finally { setLoading(false) }
  }

  return (
    <div className="auth-wrap">
      <div className="auth-card">
        <div className="auth-logo">
          <span style={{ fontSize: 32 }}>📱</span>
          <span className="app-logo-text" style={{ fontSize: 22 }}>CIMS Phone</span>
        </div>

        <form onSubmit={handleLogin} className="auth-form">
          <h2 className="auth-title">로그인</h2>
          <input
            className="form-input"
            placeholder="아이디"
            value={loginId}
            onChange={e => setLoginId(e.target.value)}
            autoFocus
          />
          <input
            className="form-input"
            type="password"
            placeholder="비밀번호"
            value={pw}
            onChange={e => setPw(e.target.value)}
          />
          {error && <div className="auth-error">{error}</div>}
          <button className="btn btn--primary" type="submit" disabled={loading}>
            {loading ? '로그인 중...' : '로그인'}
          </button>
        </form>
      </div>
    </div>
  )
}
