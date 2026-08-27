/**
 * AdminElevateDialog — admin 패스워드 인증으로 한시 승격 (sudo 모드)
 *
 * 시스템/인프라의 탭1(시스템/서버 구성)·탭2(패키지 설치) 변이 작업은 admin 전용.
 * 비-admin(operator+) 세션은 본 다이얼로그로 admin 자격을 검증해 30분 승격 —
 * 승격 토큰은 메모리에만 보관(새로고침 시 소멸), 활성 동안 API 가 admin JWT 사용.
 * 백엔드(OAM _console_rbac)가 최종 게이트이므로 UI 우회는 403.
 */
import { useState } from 'react'
import { authApi } from '../api/auth'
import { setElevatedToken } from '../api/client'
import { roleRank } from '../utils/permissions'

export default function AdminElevateDialog({ onClose, onElevated }: {
  onClose: () => void
  onElevated?: () => void
}) {
  const [loginId, setLoginId] = useState('admin')
  const [password, setPassword] = useState('')
  const [working, setWorking] = useState(false)
  const [error, setError] = useState('')

  async function submit() {
    if (!loginId || !password) { setError('계정/패스워드를 입력하세요'); return }
    setWorking(true)
    setError('')
    try {
      const r = await authApi.login(loginId, password)
      if (roleRank(r.user?.role) < roleRank('admin')) {
        setError('admin 권한 계정이 아닙니다')
        return
      }
      setElevatedToken(r.token)
      onElevated?.()
      onClose()
    } catch (e) {
      setError(`인증 실패: ${(e as Error).message}`)
    } finally {
      setWorking(false)
    }
  }

  return (
    <div style={{
      position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.45)', zIndex: 1200,
      display: 'flex', alignItems: 'center', justifyContent: 'center',
    }} onClick={onClose}>
      <div style={{ background: 'var(--surface)', borderRadius: 6, width: 380, padding: 24 }}
           onClick={e => e.stopPropagation()}>
        <h3 style={{ margin: '0 0 6px', fontSize: 16 }}>🔐 관리자 인증 (승격)</h3>
        <p style={{ fontSize: 12, color: 'var(--text-muted)', margin: '0 0 14px' }}>
          시스템/서버 구성·패키지 설치 변경은 admin 권한이 필요합니다.
          admin 계정으로 인증하면 <b>30분간</b> 이 브라우저 탭에서 변경이 허용됩니다.
        </p>
        <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
          <input className="form-input" placeholder="admin 계정 ID" value={loginId}
                 onChange={e => setLoginId(e.target.value)} disabled={working} />
          <input className="form-input" type="password" placeholder="패스워드" value={password}
                 onChange={e => setPassword(e.target.value)} disabled={working}
                 onKeyDown={e => { if (e.key === 'Enter') void submit() }}
                 autoFocus />
        </div>
        {error && <div style={{ color: '#e74c3c', fontSize: 12, marginTop: 8 }}>{error}</div>}
        <div style={{ display: 'flex', justifyContent: 'flex-end', gap: 8, marginTop: 16 }}>
          <button className="btn btn--outline" onClick={onClose} disabled={working}>취소</button>
          <button className="btn btn--primary" onClick={() => void submit()} disabled={working}>
            {working ? '인증 중…' : '인증'}
          </button>
        </div>
      </div>
    </div>
  )
}
