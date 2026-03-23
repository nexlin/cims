import { useEffect, useRef, useState } from 'react'
import { PhoneClient } from '../lib/PhoneClient'
import type { PhoneState, IncomingInfo } from '../lib/PhoneClient'
import { useAuth } from '../contexts/AuthContext'
import type { Subscription } from '../api/users'

const STATE_LABEL: Record<PhoneState, string> = {
  disconnected: '연결 안됨',
  connecting:   '연결 중...',
  registering:  '등록 중...',
  registered:   '대기',
  calling:      '발신 중...',
  ringing:      '벨 울리는 중...',
  incoming:     '착신',
  active:       '통화 중',
}
const STATE_COLOR: Record<PhoneState, string> = {
  disconnected: '#6b7280', connecting:   '#d97706', registering:  '#d97706',
  registered:   '#15803d', calling:      '#2563eb', ringing:      '#2563eb',
  incoming:     '#dc2626', active:       '#15803d',
}

// cwrtc 서버 URL (환경에 따라 조정)
const CWRTC_WS = `ws://${window.location.hostname}:8080`

interface SubOption {
  type: 'call' | 'ptt'
  sub: Subscription
}

export default function PhonePage() {
  const { user } = useAuth()

  // 구독 번호 목록 (call + ptt)
  const subOptions: SubOption[] = [
    ...(user?.call_subscriptions ?? []).map(s => ({ type: 'call' as const, sub: s })),
    ...(user?.ptt_subscriptions  ?? []).map(s => ({ type: 'ptt'  as const, sub: s })),
  ]

  const [selectedIdx, setSelectedIdx] = useState(0)
  const [wsUrl,    setWsUrl]    = useState(CWRTC_WS)
  const [domain,   setDomain]   = useState('csp')
  const [dialTo,   setDialTo]   = useState('')

  const [state,    setState]    = useState<PhoneState>('disconnected')
  const [incoming, setIncoming] = useState<IncomingInfo | null>(null)
  const [activePtt, setActivePtt] = useState(false)
  const [error,    setError]    = useState('')
  const [pttHeld,  setPttHeld]  = useState(false)

  const clientRef = useRef<PhoneClient | null>(null)
  const audioRef  = useRef<HTMLAudioElement | null>(null)

  // PhoneClient 초기화
  useEffect(() => {
    const client = new PhoneClient({
      onState: (s) => {
        setState(s)
        if (s === 'registered' || s === 'disconnected') {
          setIncoming(null); setActivePtt(false); setPttHeld(false)
        }
      },
      onIncoming: (info) => setIncoming(info),
      onError: (msg) => { setError(msg); setTimeout(() => setError(''), 5000) },
    })
    clientRef.current = client
    if (audioRef.current) client.setAudioElement(audioRef.current)
    return () => { client.disconnect() }
  }, [])

  // 구독 번호가 있으면 자동 연결
  useEffect(() => {
    if (subOptions.length === 0) return
    const opt = subOptions[selectedIdx] ?? subOptions[0]
    const client = clientRef.current
    if (!client) return

    // 이미 연결/등록 중이면 먼저 끊기
    const st = client.getState()
    if (st !== 'disconnected') client.disconnect()

    // 잠깐 대기 후 연결 (disconnect 처리 시간)
    const tid = setTimeout(() => {
      setError('')
      client.connect(wsUrl, opt.sub.id, opt.sub.passwd ?? '', domain, opt.sub.auth_id)
    }, 200)
    return () => clearTimeout(tid)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selectedIdx, wsUrl, domain])

  const isConnected  = state !== 'disconnected' && state !== 'connecting'
  const selectedSub  = subOptions[selectedIdx]

  function handleManualConnect() {
    setError('')
    if (isConnected) {
      clientRef.current?.disconnect()
    } else if (selectedSub) {
      clientRef.current?.connect(wsUrl, selectedSub.sub.id, selectedSub.sub.passwd ?? '', domain, selectedSub.sub.auth_id)
    }
  }

  function handleCall() {
    if (!dialTo.trim()) { setError('전화번호를 입력하세요'); return }
    setError('')
    clientRef.current?.call(dialTo.trim())
  }

  function handleAnswer() {
    if (incoming?.ptt) setActivePtt(true)
    clientRef.current?.answer()
    setIncoming(null)
  }

  function handleReject() {
    clientRef.current?.reject()
    setIncoming(null)
  }

  function handleHangup() {
    clientRef.current?.hangup()
    setActivePtt(false); setPttHeld(false)
  }

  function handlePttDown() { setPttHeld(true);  clientRef.current?.setPttFloor(true) }
  function handlePttUp()   { setPttHeld(false); clientRef.current?.setPttFloor(false) }

  // 구독 번호가 없는 경우
  if (subOptions.length === 0) {
    return (
      <div className="page phone-page">
        <div className="panel">
          <div className="panel-header"><span className="panel-title">📱 소프트폰</span></div>
          <div className="empty">
            <div style={{ fontSize: 40, marginBottom: 12 }}>📵</div>
            <div>할당된 전화번호가 없습니다.</div>
            <div style={{ marginTop: 8, fontSize: 12, color: '#6b7280' }}>
              관리자에게 Call 또는 PTT 번호 등록을 요청하세요.
            </div>
          </div>
        </div>
      </div>
    )
  }

  return (
    <div className="page phone-page">
      <audio ref={audioRef} autoPlay style={{ display: 'none' }} />

      {/* ── 번호 선택 + 상태 ── */}
      <div className="panel">
        <div className="panel-header"><span className="panel-title">📱 소프트폰</span></div>
        <div className="phone-login">
          {/* 번호 선택 */}
          <div className="form-grid">
            <label>활성 번호</label>
            <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
              {subOptions.map((opt, i) => (
                <button
                  key={opt.sub.id}
                  className={`btn btn--sm ${i === selectedIdx ? 'btn--primary' : 'btn--outline'}`}
                  onClick={() => setSelectedIdx(i)}
                  title={opt.type === 'ptt' ? 'PTT 번호' : 'Call 번호'}
                >
                  {opt.type === 'ptt' ? '🎙' : '📞'} {opt.sub.id}
                </button>
              ))}
            </div>
            <label>cwrtc 서버</label>
            <input className="form-input" value={wsUrl} onChange={e => setWsUrl(e.target.value)} />
            <label>도메인</label>
            <input className="form-input" value={domain} onChange={e => setDomain(e.target.value)} />
          </div>
          <div className="phone-login-row">
            <div className="phone-status-dot" style={{ background: STATE_COLOR[state] }} />
            <span className="phone-status-label">{STATE_LABEL[state]}</span>
            <button
              className={`btn btn--sm ${isConnected ? 'btn--danger' : 'btn--outline'}`}
              onClick={handleManualConnect}
              disabled={state === 'connecting' || state === 'registering'}
            >
              {isConnected ? '연결 해제' : '재연결'}
            </button>
          </div>
          {error && (
            <div className="phone-error">
              {error}
              {error.includes('마이크') && (
                <div style={{ marginTop: 4, fontSize: 11 }}>
                  💡 Chrome: <code>chrome://flags/#unsafely-treat-insecure-origin-as-secure</code>에
                  <code> {window.location.origin}</code>을 추가하거나 HTTPS를 사용하세요.
                </div>
              )}
            </div>
          )}
        </div>
      </div>

      {/* ── 착신 알림 ── */}
      {state === 'incoming' && incoming && (
        <div className="panel phone-incoming">
          <div className="phone-incoming-title">📲 착신 통화</div>
          <div className="phone-incoming-from">
            <strong>{incoming.from}</strong>
            {incoming.ptt && <span className="badge badge--green" style={{ marginLeft: 8 }}>PTT</span>}
          </div>
          <div className="phone-incoming-btns">
            <button className="btn btn--primary phone-accept-btn" onClick={handleAnswer}>📞 수신</button>
            <button className="btn btn--danger" onClick={handleReject}>📵 거절</button>
          </div>
        </div>
      )}

      {/* ── PTT 통화 중 ── */}
      {state === 'active' && activePtt && (
        <div className="panel phone-ptt-panel">
          <div className="phone-ptt-title">🎙 PTT 그룹 통화 중</div>
          <button
            className={`phone-ptt-btn ${pttHeld ? 'phone-ptt-btn--active' : ''}`}
            onMouseDown={handlePttDown} onMouseUp={handlePttUp} onMouseLeave={handlePttUp}
            onTouchStart={handlePttDown} onTouchEnd={handlePttUp}
          >
            {pttHeld ? '🔴 송신 중' : '⚫ 눌러서 말하기'}
          </button>
          <button className="btn btn--danger" onClick={handleHangup} style={{ marginTop: 16 }}>
            📵 통화 종료
          </button>
        </div>
      )}

      {/* ── 발신/통화 중 ── */}
      {(state === 'calling' || state === 'ringing' || (state === 'active' && !activePtt)) && (
        <div className="panel phone-active">
          <div className="phone-active-state">{STATE_LABEL[state]}</div>
          {dialTo && <div className="phone-active-peer">→ {dialTo}</div>}
          <button className="btn btn--danger phone-hangup-btn" onClick={handleHangup}>
            📵 종료
          </button>
        </div>
      )}

      {/* ── 다이얼패드 ── */}
      {state === 'registered' && (
        <div className="panel">
          <div className="panel-header"><span className="panel-title">📞 발신</span></div>
          <div className="phone-dial">
            <input
              className="form-input phone-dial-input"
              value={dialTo} onChange={e => setDialTo(e.target.value)}
              onKeyDown={e => e.key === 'Enter' && handleCall()}
              placeholder="전화번호 입력"
            />
            <div className="phone-dial-pad">
              {['1','2','3','4','5','6','7','8','9','*','0','#'].map(d => (
                <button key={d} className="phone-key" onClick={() => setDialTo(p => p + d)}>{d}</button>
              ))}
            </div>
            <div className="phone-dial-row">
              <button className="btn btn--ghost phone-clear-btn"
                onClick={() => setDialTo(p => p.slice(0, -1))}>⌫</button>
              <button className="btn btn--primary phone-call-btn"
                onClick={handleCall} disabled={!dialTo.trim()}>📞 통화</button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
