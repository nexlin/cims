import { useEffect, useRef, useState } from 'react'
import { PhoneClient } from '../lib/PhoneClient'
import type { PhoneState, IncomingInfo } from '../lib/PhoneClient'
import { useAuth } from '../contexts/AuthContext'
import type { Subscription } from '../api/auth'
import { idmsLogin } from '../api/idms'
import { listMyGroups } from '../api/gms'
import type { GmsGroup } from '../api/gms'

// Vite 프록시 /cwrtc → ws(s)://host/cwrtc
const CWRTC_WS = `${window.location.protocol === 'https:' ? 'wss' : 'ws'}://${window.location.host}/cwrtc`

// auth_id (e.g. "450033@ims.example.com") 에서 도메인 추출
function domainOf(authId: string): string {
  return authId.includes('@') ? authId.split('@')[1] : authId
}

const STATE_LABEL: Record<PhoneState, string> = {
  disconnected: '연결 안됨',  connecting:  '연결 중...',
  registering:  '등록 중...', registered:  '대기',
  calling:      '발신 중...', ringing:     '벨 울리는 중...',
  incoming:     '착신',       active:      '통화 중',
}
const STATE_COLOR: Record<PhoneState, string> = {
  disconnected: '#9ca3af', connecting:  '#d97706', registering: '#d97706',
  registered:   '#16a34a', calling:     '#2563eb', ringing:     '#2563eb',
  incoming:     '#dc2626', active:      '#16a34a',
}

// ── Call Panel ────────────────────────────────────────────────────────────────

function CallPanel({ sub }: { sub: Subscription }) {
  const [state,    setState]    = useState<PhoneState>('disconnected')
  const [incoming, setIncoming] = useState<IncomingInfo | null>(null)
  const [dialTo,   setDialTo]   = useState('')
  const [activeTo, setActiveTo] = useState('')
  const [error,    setError]    = useState('')

  const clientRef = useRef<PhoneClient | null>(null)
  const audioRef  = useRef<HTMLAudioElement | null>(null)

  useEffect(() => {
    const client = new PhoneClient({
      onState: (s) => {
        setState(s)
        if (s === 'registered' || s === 'disconnected') setIncoming(null)
        if (s === 'registered') setActiveTo('')
      },
      onIncoming: (info) => setIncoming(info),
      onError:    (msg)  => { setError(msg); setTimeout(() => setError(''), 6000) },
    })
    clientRef.current = client
    if (audioRef.current) client.setAudioElement(audioRef.current)
    client.connect(CWRTC_WS, sub.id, sub.passwd, domainOf(sub.auth_id), sub.auth_id)
    return () => { client.disconnect() }
  }, [sub.id, sub.passwd, sub.auth_id])

  function doCall(to: string) {
    const t = to.trim()
    if (!t) { setError('전화번호를 입력하세요'); return }
    setError(''); setActiveTo(t)
    clientRef.current?.call(t)
  }

  const isBusy = state === 'calling' || state === 'ringing' || state === 'active'

  return (
    <div className="sp-panel">
      <audio ref={audioRef} autoPlay style={{ display: 'none' }} />

      <div className="sp-header">
        <span className="sp-badge sp-badge--call">📞 통화</span>
        <span className="sp-number">{sub.id}</span>
        <div className="sp-conn">
          <span className="sp-dot" style={{ background: STATE_COLOR[state] }} />
          <span className="sp-state">{STATE_LABEL[state]}</span>
        </div>
      </div>

      {error && <div className="sp-error">{error}</div>}

      {state === 'incoming' && incoming && (
        <div className="sp-incoming">
          <div className="sp-incoming-from">📲 {incoming.from}</div>
          <div className="sp-incoming-btns">
            <button className="btn btn--primary"
              onClick={() => { clientRef.current?.answer(); setIncoming(null) }}>📞 수신</button>
            <button className="btn btn--danger"
              onClick={() => { clientRef.current?.reject(); setIncoming(null) }}>📵 거절</button>
          </div>
        </div>
      )}

      {isBusy && (
        <div className="sp-active">
          <div className="sp-active-state">{STATE_LABEL[state]}</div>
          <div className="sp-active-peer">{activeTo}</div>
          <button className="btn btn--danger" onClick={() => clientRef.current?.hangup()}>📵 종료</button>
        </div>
      )}

      {!isBusy && state !== 'incoming' && (
        <>
          <div className="sp-section">전화 걸기</div>
          <div className="sp-dialpad">
            <div className="sp-dial-row">
              <input
                className="form-input sp-dial-input"
                value={dialTo}
                onChange={e => setDialTo(e.target.value)}
                onKeyDown={e => e.key === 'Enter' && state === 'registered' && doCall(dialTo)}
                placeholder="번호 입력"
              />
              <button className="btn btn--ghost sp-del"
                onClick={() => setDialTo(p => p.slice(0, -1))}>⌫</button>
            </div>
            <div className="sp-keypad">
              {['1','2','3','4','5','6','7','8','9','*','0','#'].map(d => (
                <button key={d} className="phone-key"
                  onClick={() => setDialTo(p => p + d)}>{d}</button>
              ))}
            </div>
            <button className="btn btn--primary sp-call-btn"
              onClick={() => doCall(dialTo)}
              disabled={state !== 'registered' || !dialTo.trim()}>
              📞 통화
            </button>
          </div>
        </>
      )}
    </div>
  )
}

// ── PTT Panel ─────────────────────────────────────────────────────────────────

interface PttMember {
  uri: string; priority: number; name: string; connected: boolean
}

function PttPanel({ sub }: { sub: Subscription }) {
  const [authStatus,    setAuthStatus]    = useState<'pending'|'ok'|'fail'>('pending')
  const [accessToken,   setAccessToken]   = useState('')
  const [state,         setState]         = useState<PhoneState>('disconnected')
  const [error,         setError]         = useState('')
  const [groups,        setGroups]        = useState<GmsGroup[]>([])
  const [activeGroupId, setActiveGroupId] = useState<string | null>(null)
  const [members,       setMembers]       = useState<PttMember[]>([])
  const [speaker,       setSpeaker]       = useState<string | null>(null)
  const [floorOn,       setFloorOn]       = useState(false)

  const clientRef      = useRef<PhoneClient | null>(null)
  const audioRef       = useRef<HTMLAudioElement | null>(null)
  const activeGroupRef = useRef<string | null>(null)
  const groupsRef      = useRef<GmsGroup[]>([])
  const membersRef     = useRef<PttMember[]>([])
  const floorOnRef     = useRef(false)
  const stateRef       = useRef<PhoneState>('disconnected')

  useEffect(() => { activeGroupRef.current = activeGroupId }, [activeGroupId])
  useEffect(() => { groupsRef.current = groups }, [groups])
  useEffect(() => { membersRef.current = members }, [members])
  useEffect(() => { floorOnRef.current = floorOn }, [floorOn])
  useEffect(() => { stateRef.current = state }, [state])

  // Step 1: MCPTT IdMs 인증
  useEffect(() => {
    const mcpttId = sub.id.startsWith('tel:') ? sub.id : `tel:${sub.id}`
    idmsLogin(mcpttId, sub.passwd)
      .then(tokens => {
        setAccessToken(tokens.access_token)
        setAuthStatus('ok')
      })
      .catch(err => {
        setError(`IdMs 인증 실패: ${(err as Error).message}`)
        setAuthStatus('fail')
      })
  }, [sub.id, sub.passwd])

  // Step 2: PhoneClient 연결 (IdMs 인증 완료 후)
  useEffect(() => {
    if (authStatus !== 'ok') return

    const client = new PhoneClient({
      onState: (s) => {
        setState(s)
        if (s === 'disconnected') {
          setActiveGroupId(null); activeGroupRef.current = null
          setSpeaker(null); setFloorOn(false); floorOnRef.current = false
          setMembers([]); membersRef.current = []
        }
        if (s === 'registered') {
          setSpeaker(null); setFloorOn(false); floorOnRef.current = false
          setMembers(ms => ms.map(m => ({ ...m, connected: false })))
        }
      },
      onIncoming: (info) => {
        if (info.ptt) {
          // PTT 자동 수락
          client.answer().catch(() => {})
          // group_id 우선, 없으면 from 필드에서 추출
          const gid = info.groupId || info.from.replace(/^sip:/, '').replace(/^tel:/, '').split('@')[0]
          setActiveGroupId(gid); activeGroupRef.current = gid
        }
      },
      onError: (msg) => { setError(msg); setTimeout(() => setError(''), 6000) },
      onFloor: (spk) => setSpeaker(spk),
      onMemberStatus: (userId, connected) => {
        // GMS uri = "tel:+82...", cwrtc user_id = "+82..." → 정규화 비교
        const bare = (id: string) => id.replace(/^tel:/, '').replace(/^sip:/, '').split('@')[0]
        const uid = bare(userId)
        setMembers(ms => ms.map(m => bare(m.uri) === uid ? { ...m, connected } : m))
      },
    })
    clientRef.current = client
    if (audioRef.current) client.setAudioElement(audioRef.current)
    client.connect(CWRTC_WS, sub.id, sub.passwd, domainOf(sub.auth_id), sub.auth_id)
    return () => { client.disconnect() }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [authStatus])

  // Step 3: 그룹 목록 로드 (access_token 확보 후)
  useEffect(() => {
    if (!accessToken) return
    const mcpttId = sub.id.startsWith('tel:') ? sub.id : `tel:${sub.id}`
    listMyGroups(mcpttId, accessToken)
      .then(all => setGroups(all))
      .catch(() => {})
  }, [accessToken, sub.id])

  // 활성 그룹 멤버 목록 갱신 (activeGroupId 또는 groups 변경 시 재실행)
  useEffect(() => {
    if (!activeGroupId) { setMembers([]); return }
    const g = groups.find(g => g.id === activeGroupId)
    if (!g) { setMembers([]); return }
    const bare = (id: string) => id.replace(/^tel:/, '').replace(/^sip:/, '').split('@')[0]
    const myId = bare(sub.id)
    const isActive = stateRef.current === 'active'
    const ms = g.members.slice().sort((a, b) => a.priority - b.priority)
      .map(m => {
        const prev = membersRef.current.find(em => em.uri === m.uri)
        // 본인이고 active 상태면 connected=true, 아니면 기존 상태 보존
        const connected = (isActive && bare(m.uri) === myId) || (prev?.connected ?? false)
        return { uri: m.uri, priority: m.priority, name: m.name || m.uri, connected }
      })
    setMembers(ms); membersRef.current = ms
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeGroupId, groups])

  // state → active/registered 전환 시 본인 connected 상태 동기화
  useEffect(() => {
    const bare = (id: string) => id.replace(/^tel:/, '').replace(/^sip:/, '').split('@')[0]
    const myId = bare(sub.id)
    if (state === 'active') {
      setMembers(ms => ms.map(m => bare(m.uri) === myId ? { ...m, connected: true } : m))
    }
    // registered 전환(통화 종료)은 onState 핸들러에서 전체 false 처리
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state])

  // 고우선순위 화자 발언 시 내 플로어 자동 해제
  useEffect(() => {
    if (!speaker || speaker === sub.id) return
    if (!floorOnRef.current) return
    const spkMember = membersRef.current.find(m => m.uri === speaker)
    const myMember  = membersRef.current.find(m => m.uri === sub.id || m.uri === `tel:${sub.id}`)
    if (spkMember && myMember && spkMember.priority < myMember.priority) {
      setFloorOn(false); floorOnRef.current = false
      clientRef.current?.setPttFloor(false)
    }
  }, [speaker, sub.id])

  function handleLeave() {
    clientRef.current?.hangup()
    setActiveGroupId(null); activeGroupRef.current = null
    setSpeaker(null); setFloorOn(false); floorOnRef.current = false
  }

  function handleFloorToggle() {
    const next = !floorOn
    setFloorOn(next); floorOnRef.current = next
    clientRef.current?.setPttFloor(next)
  }

  const inCall = state === 'active' || state === 'calling' || state === 'ringing'

  // SIP 등록 완료 여부 (통화 중이어도 등록 상태는 유지됨)
  const isSipRegistered = state !== 'disconnected' && state !== 'connecting' && state !== 'registering'
  const regDotColor  = isSipRegistered ? '#16a34a' : (state === 'disconnected' ? '#9ca3af' : '#d97706')
  const regLabel     = isSipRegistered ? '등록됨'  : STATE_LABEL[state]

  return (
    <div className="sp-panel">
      <audio ref={audioRef} autoPlay style={{ display: 'none' }} />

      <div className="sp-header">
        <span className="sp-badge sp-badge--ptt">🎙 PTT</span>
        <span className="sp-number">{sub.id}</span>
        <div className="sp-conn">
          {authStatus === 'pending'
            ? <span className="sp-state" style={{ color: '#d97706' }}>인증 중...</span>
            : authStatus === 'fail'
            ? <span className="sp-state" style={{ color: '#dc2626' }}>인증 실패</span>
            : <>
                <span className="sp-dot" style={{ background: regDotColor }} />
                <span className="sp-state">{regLabel}</span>
              </>
          }
        </div>
      </div>

      {error && <div className="sp-error">{error}</div>}

      {authStatus === 'ok' && (
        <>
          <div className="sp-section">PTT 그룹</div>
          <div className="sp-groups">
            {groups.length === 0
              ? <div className="sp-empty-hint">소속 그룹 없음</div>
              : groups.map(g => {
                const isActive = g.id === activeGroupId
                const totalCount = g.members.length || g.member_count
                const connectedCount = isActive ? members.filter(m => m.connected).length : 0
                return (
                  <div key={g.id}>
                    <div
                      className={`sp-group${isActive ? ' sp-group--active' : ''}`}
                      style={{ cursor: 'default' }}
                    >
                      <span className="sp-group-dot"
                        style={{ background: isActive && state === 'active' ? '#16a34a' : isActive ? '#d97706' : '#d1d5db' }} />
                      <div className="sp-group-info">
                        <span className="sp-group-name">{g.display_name}</span>
                        <span className="sp-group-id">{g.id}</span>
                      </div>
                      <span className="sp-group-state">
                        {isActive && state === 'active'
                          ? `${connectedCount}/${totalCount}명`
                          : `0/${totalCount}명`}
                      </span>
                      {isActive && inCall && (
                        <button className="btn btn--sm btn--danger sp-join-btn"
                          onClick={e => { e.stopPropagation(); handleLeave() }}>나가기</button>
                      )}
                    </div>

                    {isActive && members.length > 0 && (
                      <div className="sp-members">
                        <div className="sp-members-head">
                          <span className="sp-mh-pri">우선</span>
                          <span className="sp-mh-name">이름 / 번호</span>
                          <span className="sp-mh-status">상태</span>
                        </div>
                        {members.map(m => {
                          const isSpeaker = m.uri === speaker
                          const isMe      = m.uri === sub.id || m.uri === `tel:${sub.id}`
                          return (
                            <div key={m.uri}
                              className={`sp-member${isSpeaker ? ' sp-member--speaker' : ''}${isMe ? ' sp-member--me' : ''}`}
                            >
                              <span className="sp-m-pri">{m.priority}</span>
                              <div className="sp-m-info">
                                <span className="sp-m-name">
                                  {m.name}{isMe && <span className="sp-me-tag">나</span>}
                                </span>
                                <span className="sp-m-id">{m.uri}</span>
                              </div>
                              <div className="sp-m-status">
                                {isSpeaker
                                  ? <span className="sp-speaker-badge">🎤 화자</span>
                                  : <span className="sp-conn-dot"
                                      style={{ background: m.connected ? '#16a34a' : '#d1d5db' }} />
                                }
                              </div>
                            </div>
                          )
                        })}
                      </div>
                    )}
                  </div>
                )
              })
            }
          </div>

          <div className="sp-ptt-footer">
            {state === 'active' && activeGroupId
              ? <button
                  className={`sp-push-btn${floorOn ? ' sp-push-btn--on' : ''}`}
                  onClick={handleFloorToggle}
                >
                  {floorOn ? '🔴 PUSH (송신 중)' : '⚫ PUSH'}
                </button>
              : state === 'incoming'
              ? <div className="sp-ptt-waiting">PTT 연결 중...</div>
              : (state === 'calling' || state === 'ringing')
              ? <div className="sp-ptt-waiting">{STATE_LABEL[state]}</div>
              : <div className="sp-ptt-waiting">착신 대기 중</div>
            }
          </div>
        </>
      )}
    </div>
  )
}

// ── Main Page ─────────────────────────────────────────────────────────────────

export default function PhonePage() {
  const { user } = useAuth()
  if (!user) return null

  const hasCall = user.call_subscriptions.length > 0
  const hasPtt  = user.ptt_subscriptions.length  > 0

  if (!hasCall && !hasPtt) {
    return (
      <div className="sp-page">
        <div className="sp-empty-hint" style={{ padding: '2rem' }}>
          등록된 VoIP / PTT 번호가 없습니다.
        </div>
      </div>
    )
  }

  return (
    <div className="sp-page">
      {user.call_subscriptions.map(sub => (
        <CallPanel key={sub.id} sub={sub} />
      ))}
      {user.ptt_subscriptions.map(sub => (
        <PttPanel key={sub.id} sub={sub} />
      ))}
    </div>
  )
}
