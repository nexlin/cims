import { KeyRound, Radio } from 'lucide-react'
import { useState } from 'react'
import { authApi } from '../api/auth'
import { useAuth } from '../contexts/AuthContext'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'

type Mode = 'login' | 'register' | 'change_pw'

export default function LoginPage() {
  const { login, user, refresh } = useAuth()
  const [mode,    setMode]    = useState<Mode>('login')
  const [name,    setName]    = useState('')
  const [loginId,   setLoginId]   = useState('')
  const [pw,      setPw]      = useState('')
  const [pw2,     setPw2]     = useState('')
  const [oldPw,   setOldPw]   = useState('')
  const [error,   setError]   = useState('')
  const [ok,      setOk]      = useState('')
  const [loading, setLoading] = useState(false)

  function reset() { setError(''); setOk('') }

  async function handleLogin(e: React.FormEvent) {
    e.preventDefault(); reset()
    if (!loginId || !pw) { setError('아이디와 비밀번호를 입력하세요'); return }
    setLoading(true)
    try {
      const res = await authApi.login(loginId, pw)
      login(res.token, res.user)
    } catch (err: unknown) {
      setError((err as Error).message)
    } finally { setLoading(false) }
  }

  async function handleRegister(e: React.FormEvent) {
    e.preventDefault(); reset()
    if (!name || !loginId || !pw) { setError('모든 항목을 입력하세요'); return }
    if (pw !== pw2) { setError('비밀번호가 일치하지 않습니다'); return }
    if (pw.length < 4) { setError('비밀번호는 4자 이상이어야 합니다'); return }
    setLoading(true)
    try {
      const res = await authApi.register(name, loginId, pw)
      login(res.token, res.user)
    } catch (err: unknown) {
      setError((err as Error).message)
    } finally { setLoading(false) }
  }

  async function handleChangePassword(e: React.FormEvent) {
    e.preventDefault(); reset()
    if (!oldPw || !pw) { setError('모든 항목을 입력하세요'); return }
    if (pw !== pw2) { setError('새 비밀번호가 일치하지 않습니다'); return }
    if (pw.length < 4) { setError('새 비밀번호는 4자 이상이어야 합니다'); return }
    setLoading(true)
    try {
      await authApi.changePassword(oldPw, pw)
      await refresh()
      setOk('비밀번호가 변경되었습니다')
      setOldPw(''); setPw(''); setPw2('')
      setTimeout(() => setMode('login'), 1500)
    } catch (err: unknown) {
      setError((err as Error).message)
    } finally { setLoading(false) }
  }

  // ── 비밀번호 변경 (로그인 상태에서) ──
  if (mode === 'change_pw' && user) {
    return (
      <div className="auth-wrap">
        <div className="auth-card">
          <div className="auth-logo"><KeyRound size={18} /> 비밀번호 변경</div>
          <form onSubmit={handleChangePassword} className="auth-form">
            <Input  type="password" placeholder="현재 비밀번호"
              value={oldPw} onChange={e => setOldPw(e.target.value)} />
            <Input  type="password" placeholder="새 비밀번호"
              value={pw} onChange={e => setPw(e.target.value)} />
            <Input  type="password" placeholder="새 비밀번호 확인"
              value={pw2} onChange={e => setPw2(e.target.value)} />
            {error && <div className="auth-error">{error}</div>}
            {ok    && <div className="auth-ok">{ok}</div>}
            <Button variant="default" size="default" className="w-full" type="submit" disabled={loading}>
              {loading ? '변경 중...' : '변경'}
            </Button>
            <Button variant="ghost" size="default" className="w-full" type="button" onClick={() => { reset(); setMode('login') }}>
              취소
            </Button>
          </form>
        </div>
      </div>
    )
  }

  // ── 로그인 상태 (헤더용, App.tsx에서 처리하지만 직접 접근 시 대비) ──
  if (user && mode === 'login') return null

  return (
    <div className="auth-wrap">
      <div className="auth-card">
        <div className="auth-logo">
          <Radio size={32} className="text-primary" />
          <span className="app-logo-text" style={{ fontSize: 22 }}>CIMS</span>
        </div>

        {mode === 'login' && (
          <form onSubmit={handleLogin} className="auth-form">
            <h2 className="auth-title">로그인</h2>
            <Input  placeholder="아이디"
              value={loginId} onChange={e => setLoginId(e.target.value)} autoFocus />
            <Input  type="password" placeholder="비밀번호"
              value={pw} onChange={e => setPw(e.target.value)} />
            {error && <div className="auth-error">{error}</div>}
            <Button variant="default" size="default" className="w-full" type="submit" disabled={loading}>
              {loading ? '로그인 중...' : '로그인'}
            </Button>
            <Button variant="link" size="default" className="auth-switch" type="button"
              onClick={() => { reset(); setMode('register') }}>
              계정이 없으신가요? 회원가입
            </Button>
          </form>
        )}

        {mode === 'register' && (
          <form onSubmit={handleRegister} className="auth-form">
            <h2 className="auth-title">회원가입</h2>
            <Input  placeholder="이름"
              value={name} onChange={e => setName(e.target.value)} autoFocus />
            <Input  placeholder="아이디"
              value={loginId} onChange={e => setLoginId(e.target.value)} />
            <Input  type="password" placeholder="비밀번호 (4자 이상)"
              value={pw} onChange={e => setPw(e.target.value)} />
            <Input  type="password" placeholder="비밀번호 확인"
              value={pw2} onChange={e => setPw2(e.target.value)} />
            {error && <div className="auth-error">{error}</div>}
            <Button variant="default" size="default" className="w-full" type="submit" disabled={loading}>
              {loading ? '가입 중...' : '가입하기'}
            </Button>
            <Button variant="link" size="default" className="auth-switch" type="button"
              onClick={() => { reset(); setMode('login') }}>
              이미 계정이 있으신가요? 로그인
            </Button>
          </form>
        )}
      </div>
    </div>
  )
}

export function useChangePw() {
  return useState<boolean>(false)
}
