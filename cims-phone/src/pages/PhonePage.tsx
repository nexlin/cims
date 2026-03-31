import { useEffect, useRef, useState, useCallback } from 'react'
import { PhoneClient } from '../lib/PhoneClient'
import type { PhoneState, IncomingInfo } from '../lib/PhoneClient'
import { useAuth } from '../contexts/AuthContext'
import type { Subscription } from '../api/auth'
import { idmsLogin } from '../api/idms'
import { listMyGroups } from '../api/gms'
import type { GmsGroup } from '../api/gms'

// Vite 프록시 /cwrtc → ws(s)://host/cwrtc
const CWRTC_WS = `${window.location.protocol === 'https:' ? 'wss' : 'ws'}://${window.location.host}/cwrtc`

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

const bare = (id: string) => id.replace(/^tel:/, '').replace(/^sip:/, '').split('@')[0]

// ── Call Panel (VoIP 1:1) ────────────────────────────────────────────────────

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
              <input className="form-input sp-dial-input" value={dialTo}
                onChange={e => setDialTo(e.target.value)}
                onKeyDown={e => e.key === 'Enter' && state === 'registered' && doCall(dialTo)}
                placeholder="번호 입력" />
              <button className="btn btn--ghost sp-del" onClick={() => setDialTo(p => p.slice(0, -1))}>⌫</button>
            </div>
            <div className="sp-keypad">
              {['1','2','3','4','5','6','7','8','9','*','0','#'].map(d => (
                <button key={d} className="phone-key" onClick={() => setDialTo(p => p + d)}>{d}</button>
              ))}
            </div>
            <button className="btn btn--primary sp-call-btn"
              onClick={() => doCall(dialTo)} disabled={state !== 'registered' || !dialTo.trim()}>
              📞 통화
            </button>
          </div>
        </>
      )}
    </div>
  )
}

// ── Per-Group State ──────────────────────────────────────────────────────────

interface GroupSession {
  callId: string
  speaker: string | null
  floorOn: boolean
  members: PttMember[]
  videoRotation: number
  volume: number
}

interface PttMember {
  uri: string; priority: number; name: string; connected: boolean
}

// ── PTT Panel (다중 그룹) ────────────────────────────────────────────────────

function PttPanel({ sub }: { sub: Subscription }) {
  const [authStatus,  setAuthStatus]  = useState<'pending'|'ok'|'fail'>('pending')
  const [accessToken, setAccessToken] = useState('')
  const [state,       setState]       = useState<PhoneState>('disconnected')
  const [error,       setError]       = useState('')
  const [groups,      setGroups]      = useState<GmsGroup[]>([])
  // 그룹별 세션: key = groupId
  const [sessions, setSessions] = useState<Record<string, GroupSession>>({})

  const clientRef = useRef<PhoneClient | null>(null)
  const audioRef  = useRef<HTMLAudioElement | null>(null)
  // 그룹별 video ref: key = groupId
  const videoRefs = useRef<Record<string, HTMLVideoElement | null>>({})
  // 그룹별 audio ref (볼륨 조절용)
  const groupAudioRefs = useRef<Record<string, HTMLAudioElement | null>>({})

  // Step 1: MCPTT IdMs 인증
  useEffect(() => {
    const mcpttId = sub.id.startsWith('tel:') ? sub.id : `tel:${sub.id}`
    idmsLogin(mcpttId, sub.passwd)
      .then(tokens => { setAccessToken(tokens.access_token); setAuthStatus('ok') })
      .catch(err => { setError(`IdMs 인증 실패: ${(err as Error).message}`); setAuthStatus('fail') })
  }, [sub.id, sub.passwd])

  // Step 2: PhoneClient 연결
  useEffect(() => {
    if (authStatus !== 'ok') return
    const client = new PhoneClient({
      onState: (s) => {
        setState(s)
        if (s === 'disconnected' || s === 'registered') {
          setSessions({})
        }
      },
      onIncoming: (info) => {
        if (!info.ptt) return
        const gid = info.groupId || info.from.replace(/^sip:/, '').replace(/^tel:/, '').split('@')[0]
        const hasVideo = info.sdp.includes('m=video')
        client.setVideoEnabled(hasVideo)
        client.answer().catch(() => {})
        setSessions(prev => ({
          ...prev,
          [gid]: { callId: info.callId, speaker: null, floorOn: false,
                   members: [], videoRotation: 0, volume: 100 }
        }))
      },
      onError: (msg) => { setError(msg); setTimeout(() => setError(''), 6000) },
      onFloor: (spk) => {
        // callId로 그룹 찾기
        setSessions(prev => {
          const next = { ...prev }
          for (const gid in next) {
            // floor 이벤트는 현재 활성 callId에 대해 옴
            next[gid] = { ...next[gid], speaker: spk }
          }
          return next
        })
      },
      onFloorReject: () => {
        setSessions(prev => {
          const next = { ...prev }
          for (const gid in next) {
            if (next[gid].floorOn) {
              next[gid] = { ...next[gid], floorOn: false }
              client.setPttFloor(false)
            }
          }
          return next
        })
      },
      onLocalVideo: (stream) => {
        // 화자 자신의 카메라 프리뷰 → 활성 비디오 그룹의 video 엘리먼트에 표시
        for (const gid in videoRefs.current) {
          const el = videoRefs.current[gid]
          if (el) {
            if (stream) {
              el.srcObject = stream
              el.muted = true
            }
            // stream===null이면 리모트 비디오가 다시 표시됨 (ontrack에서)
          }
        }
      },
      onMemberStatus: (userId, connected) => {
        const uid = bare(userId)
        setSessions(prev => {
          const next = { ...prev }
          for (const gid in next) {
            next[gid] = {
              ...next[gid],
              members: next[gid].members.map(m => bare(m.uri) === uid ? { ...m, connected } : m)
            }
          }
          return next
        })
      },
    })
    clientRef.current = client
    if (audioRef.current) client.setAudioElement(audioRef.current)
    client.connect(CWRTC_WS, sub.id, sub.passwd, domainOf(sub.auth_id), sub.auth_id)
    return () => { client.disconnect() }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [authStatus])

  // Step 3: 그룹 목록 로드
  useEffect(() => {
    if (!accessToken) return
    const mcpttId = sub.id.startsWith('tel:') ? sub.id : `tel:${sub.id}`
    listMyGroups(mcpttId, accessToken).then(all => setGroups(all)).catch(() => {})
  }, [accessToken, sub.id])

  // 세션 있는 그룹의 멤버 목록 동기화
  useEffect(() => {
    setSessions(prev => {
      const next = { ...prev }
      let changed = false
      for (const gid in next) {
        const g = groups.find(g => g.id === gid)
        if (!g) continue
        if (next[gid].members.length === 0 && g.members.length > 0) {
          const myId = bare(sub.id)
          next[gid] = {
            ...next[gid],
            members: g.members.slice().sort((a, b) => a.priority - b.priority)
              .map(m => ({ uri: m.uri, priority: m.priority, name: m.name || m.uri,
                           connected: bare(m.uri) === myId }))
          }
          changed = true
        }
      }
      return changed ? next : prev
    })
  }, [sessions, groups, sub.id])

  // videoEl ref 연결
  const setVideoRef = useCallback((gid: string, el: HTMLVideoElement | null) => {
    videoRefs.current[gid] = el
    // 첫 번째 비디오 활성 그룹의 videoEl을 client에 연결
    if (el && clientRef.current) clientRef.current.setVideoElement(el)
  }, [])

  // 볼륨 변경
  const handleVolumeChange = useCallback((gid: string, vol: number) => {
    setSessions(prev => ({ ...prev, [gid]: { ...prev[gid], volume: vol } }))
    const audioEl = groupAudioRefs.current[gid]
    if (audioEl) audioEl.volume = vol / 100
    // 메인 audio도 조절
    if (audioRef.current) audioRef.current.volume = vol / 100
  }, [])

  // PUSH 토글
  const handleFloorToggle = useCallback((gid: string) => {
    setSessions(prev => {
      const sess = prev[gid]
      if (!sess) return prev
      const next = !sess.floorOn
      clientRef.current?.setPttFloor(next)
      return { ...prev, [gid]: { ...sess, floorOn: next } }
    })
  }, [])

  // 비디오 회전
  const handleRotate = useCallback((gid: string) => {
    setSessions(prev => {
      const sess = prev[gid]
      if (!sess) return prev
      return { ...prev, [gid]: { ...sess, videoRotation: (sess.videoRotation + 90) % 360 } }
    })
  }, [])

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
                const sess = sessions[g.id]
                const isActive = !!sess
                const totalCount = g.members.length || g.member_count
                const connectedCount = isActive ? sess.members.filter(m => m.connected).length : 0

                return (
                  <div key={g.id} className="sp-group-card">
                    {/* 그룹 헤더 */}
                    <div className={`sp-group${isActive ? ' sp-group--active' : ''}`}
                      style={{ cursor: 'default' }}>
                      <span className="sp-group-dot"
                        style={{ background: isActive ? '#16a34a' : '#d1d5db' }} />
                      <div className="sp-group-info">
                        <span className="sp-group-name">
                          {g.display_name}
                          {g.video_enabled && <span style={{ marginLeft: 4, fontSize: '0.75em', color: '#2563eb' }}>[Video]</span>}
                        </span>
                        <span className="sp-group-id">{g.id}</span>
                      </div>
                      <span className="sp-group-state">
                        {isActive ? `${connectedCount}/${totalCount}명` : `0/${totalCount}명`}
                      </span>
                    </div>

                    {/* 비디오 영역 (video_enabled 그룹만) */}
                    {isActive && g.video_enabled && (
                      <div style={{ position: 'relative', background: '#000', borderRadius: 8,
                                    overflow: 'hidden', margin: '4px 0', aspectRatio: '4/3' }}>
                        <video
                          ref={el => setVideoRef(g.id, el)}
                          autoPlay playsInline muted
                          style={{
                            width: '100%', height: '100%', objectFit: 'contain',
                            transform: `rotate(${sess.videoRotation}deg)`,
                          }} />
                        <button onClick={() => handleRotate(g.id)}
                          style={{ position: 'absolute', top: 4, right: 4, background: 'rgba(0,0,0,0.5)',
                                   color: '#fff', border: 'none', borderRadius: 4, padding: '2px 8px',
                                   cursor: 'pointer', fontSize: 14 }}>
                          ↻ 회전
                        </button>
                        {sess.floorOn && (
                          <span style={{ position: 'absolute', top: 4, left: 4, background: 'rgba(220,38,38,0.8)',
                                         color: '#fff', borderRadius: 4, padding: '2px 6px', fontSize: 11 }}>
                            송신 중
                          </span>
                        )}
                      </div>
                    )}

                    {/* 멤버 목록 */}
                    {isActive && sess.members.length > 0 && (
                      <div className="sp-members">
                        <div className="sp-members-head">
                          <span className="sp-mh-pri">우선</span>
                          <span className="sp-mh-name">이름 / 번호</span>
                          <span className="sp-mh-status">상태</span>
                        </div>
                        {sess.members.map(m => {
                          const isSpeaker = sess.speaker ? bare(m.uri) === bare(sess.speaker) : false
                          const isMe = m.uri === sub.id || m.uri === `tel:${sub.id}`
                          return (
                            <div key={m.uri}
                              className={`sp-member${isSpeaker ? ' sp-member--speaker' : ''}${isMe ? ' sp-member--me' : ''}`}>
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
                                      style={{ background: m.connected ? '#16a34a' : '#d1d5db' }} />}
                              </div>
                            </div>
                          )
                        })}
                      </div>
                    )}

                    {/* 그룹별 PUSH 버튼 + 볼륨 */}
                    {isActive && (
                      <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '6px 0' }}>
                        <button
                          className={`sp-push-btn${sess.floorOn ? ' sp-push-btn--on' : ''}`}
                          onClick={() => handleFloorToggle(g.id)}
                          style={{ flex: 1 }}>
                          {sess.floorOn ? '🔴 PUSH (송신 중)' : '⚫ PUSH'}
                        </button>
                        <div style={{ display: 'flex', alignItems: 'center', gap: 4, minWidth: 90 }}>
                          <span style={{ fontSize: 12 }}>🔊</span>
                          <input type="range" min={0} max={100} value={sess.volume}
                            onChange={e => handleVolumeChange(g.id, Number(e.target.value))}
                            style={{ width: 70, accentColor: '#2563eb' }} />
                        </div>
                      </div>
                    )}

                    {/* 비활성 그룹 상태 */}
                    {!isActive && (
                      <div style={{ textAlign: 'center', padding: '4px 0', fontSize: 12, color: '#9ca3af' }}>
                        착신 대기 중
                      </div>
                    )}
                  </div>
                )
              })
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
