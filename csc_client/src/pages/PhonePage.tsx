import { useEffect, useRef, useState } from 'react'
import { PhoneClient } from '../lib/PhoneClient'
import type { PhoneState, IncomingInfo } from '../lib/PhoneClient'
import { useAuth } from '../contexts/AuthContext'
import type { Subscription } from '../api/users'
import { usersApi } from '../api/users'
import { groupsApi } from '../api/groups'
import type { Group } from '../api/groups'

// Vite 프록시 /cwrtc → ws://127.0.0.1:8080 (cwrtc WebSocket)
// 페이지와 동일한 origin 사용 → HTTP/HTTPS 환경 무관
const CWRTC_WS   = `${window.location.protocol === 'https:' ? 'wss' : 'ws'}://${window.location.host}/cwrtc`
const CALL_DOMAIN = 'ims.mnc033.mcc450.3gppnetwork.org'
const PTT_DOMAIN  = 'ptt.mnc033.mcc450.3gppnetwork.org'

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
  disconnected: '#9ca3af', connecting:  '#d97706', registering: '#d97706',
  registered:   '#16a34a', calling:     '#2563eb', ringing:     '#2563eb',
  incoming:     '#dc2626', active:      '#16a34a',
}

// ── Call Panel ───────────────────────────────────────────────────────────────

interface Contact { name: string; msisdn: string }

function CallPanel({ sub }: { sub: Subscription }) {
  const [state,    setState]    = useState<PhoneState>('disconnected')
  const [incoming, setIncoming] = useState<IncomingInfo | null>(null)
  const [dialTo,   setDialTo]   = useState('')
  const [activeTo, setActiveTo] = useState('')
  const [error,    setError]    = useState('')
  const [contacts, setContacts] = useState<Contact[]>([])

  const clientRef = useRef<PhoneClient | null>(null)
  const audioRef  = useRef<HTMLAudioElement | null>(null)

  // Init PhoneClient and auto-connect
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
    client.connect(CWRTC_WS, sub.id, sub.passwd ?? '', CALL_DOMAIN, sub.auth_id)
    return () => { client.disconnect() }
  }, [sub.id, sub.auth_id, sub.passwd])

  // Load contacts (all users' call numbers except self)
  useEffect(() => {
    usersApi.list().then(users => {
      const list: Contact[] = []
      for (const u of users)
        for (const cs of u.call_subscriptions)
          if (cs.id !== sub.id) list.push({ name: u.name, msisdn: cs.id })
      setContacts(list)
    }).catch(() => {})
  }, [sub.id])

  function doCall(to: string) {
    const t = to.trim()
    if (!t) { setError('전화번호를 입력하세요'); return }
    setError(''); setActiveTo(t)
    clientRef.current?.call(t)
  }

  function handleAnswer() { clientRef.current?.answer(); setIncoming(null) }
  function handleReject() { clientRef.current?.reject(); setIncoming(null) }
  function handleHangup() { clientRef.current?.hangup() }

  const isBusy = state === 'calling' || state === 'ringing' || state === 'active'

  return (
    <div className="sp-panel">
      <audio ref={audioRef} autoPlay style={{ display: 'none' }} />

      {/* ── Header ── */}
      <div className="sp-header">
        <span className="sp-badge sp-badge--call">📞 통화</span>
        <span className="sp-number">{sub.id}</span>
        <div className="sp-conn">
          <span className="sp-dot" style={{ background: STATE_COLOR[state] }} />
          <span className="sp-state">{STATE_LABEL[state]}</span>
        </div>
      </div>

      {error && <div className="sp-error">{error}</div>}

      {/* ── 착신 ── */}
      {state === 'incoming' && incoming && (
        <div className="sp-incoming">
          <div className="sp-incoming-from">📲 {incoming.from}</div>
          <div className="sp-incoming-btns">
            <button className="btn btn--primary" onClick={handleAnswer}>📞 수신</button>
            <button className="btn btn--danger"  onClick={handleReject}>📵 거절</button>
          </div>
        </div>
      )}

      {/* ── 통화 중 ── */}
      {isBusy && (
        <div className="sp-active">
          <div className="sp-active-state">{STATE_LABEL[state]}</div>
          <div className="sp-active-peer">{activeTo}</div>
          <button className="btn btn--danger" onClick={handleHangup}>📵 종료</button>
        </div>
      )}

      {/* ── 대기 중: 연락처 + 다이얼패드 ── */}
      {!isBusy && state !== 'incoming' && (
        <>
          <div className="sp-section">연락처</div>
          <div className="sp-contacts">
            {contacts.length === 0
              ? <div className="sp-empty-hint">연락처 없음</div>
              : contacts.map(c => (
                <div key={c.msisdn}
                  className="sp-contact"
                  onClick={() => state === 'registered' && doCall(c.msisdn)}
                >
                  <span className="sp-contact-name">{c.name}</span>
                  <span className="sp-contact-num">{c.msisdn}</span>
                  {state === 'registered' && (
                    <button className="sp-contact-btn"
                      onClick={e => { e.stopPropagation(); doCall(c.msisdn) }}>📞</button>
                  )}
                </div>
              ))
            }
          </div>

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

// ── PTT Panel ────────────────────────────────────────────────────────────────

interface PttMember {
  user_id:   string
  priority:  number
  name:      string    // 사용자 이름 (usersApi에서 조회)
  connected: boolean   // cwrtc 멤버 상태 메시지
}

interface PttGroup extends Group { }

function PttPanel({ sub }: { sub: Subscription }) {
  const [state,         setState]         = useState<PhoneState>('disconnected')
  const [error,         setError]         = useState('')
  const [groups,        setGroups]        = useState<PttGroup[]>([])
  const [activeGroupId, setActiveGroupId] = useState<string | null>(null)
  const [members,       setMembers]       = useState<PttMember[]>([])  // active group 멤버
  const [speaker,       setSpeaker]       = useState<string | null>(null)  // speaker의 user_id
  const [floorOn,       setFloorOn]       = useState(false)   // 내가 화자인지 (Toggle)

  const clientRef      = useRef<PhoneClient | null>(null)
  const audioRef       = useRef<HTMLAudioElement | null>(null)
  const activeGroupRef = useRef<string | null>(null)
  const groupsRef      = useRef<PttGroup[]>([])
  const membersRef     = useRef<PttMember[]>([])
  const floorOnRef     = useRef(false)

  useEffect(() => { activeGroupRef.current = activeGroupId }, [activeGroupId])
  useEffect(() => { groupsRef.current = groups }, [groups])
  useEffect(() => { membersRef.current = members }, [members])
  useEffect(() => { floorOnRef.current = floorOn }, [floorOn])

  // 화자 변경 → 우선순위가 높은 사람이 발언 시 내 플로어 자동 해제
  useEffect(() => {
    if (!speaker || speaker === sub.id) return
    if (!floorOnRef.current) return
    const speakerMember = membersRef.current.find(m => m.user_id === speaker)
    const myMember      = membersRef.current.find(m => m.user_id === sub.id)
    if (speakerMember && myMember && speakerMember.priority < myMember.priority) {
      // 더 높은 우선순위(낮은 숫자)가 발언 → 내 toggle 자동 해제
      setFloorOn(false)
      floorOnRef.current = false
      clientRef.current?.setPttFloor(false)
    }
  }, [speaker, sub.id])

  // Init PhoneClient
  useEffect(() => {
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
          client.answer().catch(() => {})
          const gid = info.from.replace(/^sip:/, '').split('@')[0]
          setActiveGroupId(gid); activeGroupRef.current = gid
        }
      },
      onError: (msg) => { setError(msg); setTimeout(() => setError(''), 6000) },
      onFloor: (spk) => setSpeaker(spk),
      onMemberStatus: (userId, connected) => {
        setMembers(ms => ms.map(m => m.user_id === userId ? { ...m, connected } : m))
      },
    })
    clientRef.current = client
    if (audioRef.current) client.setAudioElement(audioRef.current)
    client.connect(CWRTC_WS, sub.id, sub.passwd ?? '', PTT_DOMAIN, sub.auth_id)
    return () => { client.disconnect() }
  }, [sub.id, sub.auth_id, sub.passwd])

  // usersApi에서 PTT MSISDN → 이름 매핑 테이블 로드
  const [nameMap, setNameMap] = useState<Record<string, string>>({})
  useEffect(() => {
    usersApi.list().then(users => {
      const map: Record<string, string> = {}
      for (const u of users)
        for (const ps of u.ptt_subscriptions) map[ps.id] = u.name
      setNameMap(map)
    }).catch(() => {})
  }, [])

  // 내가 속한 그룹 목록 로드
  useEffect(() => {
    groupsApi.list().then(all => {
      setGroups(all.filter(g => g.members.some(m => m.user_id === sub.id)))
    }).catch(() => {})
  }, [sub.id])

  // 활성 그룹 멤버 목록 갱신
  useEffect(() => {
    if (!activeGroupId) { setMembers([]); return }
    const g = groupsRef.current.find(g => g.id === activeGroupId)
    if (!g) { setMembers([]); return }
    const ms: PttMember[] = g.members
      .slice()
      .sort((a, b) => a.priority - b.priority)
      .map(m => ({ user_id: m.user_id, priority: m.priority, name: nameMap[m.user_id] ?? m.user_id, connected: false }))
    setMembers(ms)
    membersRef.current = ms
  }, [activeGroupId, nameMap])

  // 등록 완료 후 첫 번째 그룹 자동 접속
  useEffect(() => {
    if (state === 'registered' && groupsRef.current.length > 0 && !activeGroupRef.current) {
      doJoin(groupsRef.current[0].id)
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state])

  function doJoin(groupId: string) {
    const client = clientRef.current
    if (!client) return
    const cur = client.getState()
    if (cur === 'active' || cur === 'calling' || cur === 'ringing') {
      client.hangup()
      setTimeout(() => { setActiveGroupId(groupId); activeGroupRef.current = groupId; setSpeaker(null); setFloorOn(false); client.call(groupId) }, 300)
    } else if (cur === 'registered') {
      setActiveGroupId(groupId); activeGroupRef.current = groupId; setSpeaker(null); setFloorOn(false)
      client.call(groupId)
    }
  }

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

  return (
    <div className="sp-panel">
      <audio ref={audioRef} autoPlay style={{ display: 'none' }} />

      {/* ── Header ── */}
      <div className="sp-header">
        <span className="sp-badge sp-badge--ptt">🎙 PTT</span>
        <span className="sp-number">{sub.id}</span>
        <div className="sp-conn">
          <span className="sp-dot" style={{ background: STATE_COLOR[state] }} />
          <span className="sp-state">{STATE_LABEL[state]}</span>
        </div>
      </div>

      {error && <div className="sp-error">{error}</div>}

      {/* ── 그룹 목록 ── */}
      <div className="sp-section">PTT 그룹</div>
      <div className="sp-groups">
        {groups.length === 0
          ? <div className="sp-empty-hint">소속 그룹 없음</div>
          : groups.map(g => {
            const isActive = g.id === activeGroupId
            return (
              <div key={g.id}>
                {/* 그룹 헤더 행 */}
                <div
                  className={`sp-group${isActive ? ' sp-group--active' : ''}`}
                  onClick={() => !isActive && state === 'registered' && doJoin(g.id)}
                  style={{ cursor: !isActive && state === 'registered' ? 'pointer' : 'default' }}
                >
                  <span className="sp-group-dot"
                    style={{ background: isActive ? STATE_COLOR[state] : '#d1d5db' }} />
                  <div className="sp-group-info">
                    <span className="sp-group-name">{g.name}</span>
                    <span className="sp-group-id">{g.id}</span>
                  </div>
                  <span className="sp-group-state">
                    {isActive ? STATE_LABEL[state] : '미접속'}
                  </span>
                  {!isActive && state === 'registered' && (
                    <button className="btn btn--sm btn--outline sp-join-btn"
                      onClick={e => { e.stopPropagation(); doJoin(g.id) }}>접속</button>
                  )}
                  {isActive && inCall && (
                    <button className="btn btn--sm btn--danger sp-join-btn"
                      onClick={e => { e.stopPropagation(); handleLeave() }}>나가기</button>
                  )}
                </div>

                {/* 활성 그룹 멤버 목록 */}
                {isActive && members.length > 0 && (
                  <div className="sp-members">
                    <div className="sp-members-head">
                      <span className="sp-mh-pri">우선</span>
                      <span className="sp-mh-name">이름 / 번호</span>
                      <span className="sp-mh-status">상태</span>
                    </div>
                    {members.map(m => {
                      const isSpeaker = m.user_id === speaker
                      const isMe      = m.user_id === sub.id
                      return (
                        <div key={m.user_id}
                          className={`sp-member${isSpeaker ? ' sp-member--speaker' : ''}${isMe ? ' sp-member--me' : ''}`}
                        >
                          <span className="sp-m-pri">{m.priority}</span>
                          <div className="sp-m-info">
                            <span className="sp-m-name">
                              {m.name}{isMe && <span className="sp-me-tag">나</span>}
                            </span>
                            <span className="sp-m-id">{m.user_id}</span>
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

      {/* ── PTT Toggle 버튼 ── */}
      <div className="sp-ptt-footer">
        {state === 'active' && activeGroupId ? (
          <button
            className={`sp-push-btn${floorOn ? ' sp-push-btn--on' : ''}`}
            onClick={handleFloorToggle}
          >
            {floorOn ? '🔴 PUSH (송신 중)' : '⚫ PUSH'}
          </button>
        ) : (state === 'calling' || state === 'ringing') ? (
          <div className="sp-ptt-waiting">{STATE_LABEL[state]}</div>
        ) : (
          <div className="sp-ptt-waiting">그룹에 접속하세요</div>
        )}
      </div>
    </div>
  )
}

// ── Main Page ────────────────────────────────────────────────────────────────

export default function PhonePage() {
  const { user } = useAuth()

  const hasAny = (user?.call_subscriptions.length ?? 0) + (user?.ptt_subscriptions.length ?? 0) > 0

  if (!hasAny) {
    return (
      <div className="page">
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
    <div className="sp-page">
      {user?.call_subscriptions.map(sub => <CallPanel key={sub.id} sub={sub} />)}
      {user?.ptt_subscriptions.map(sub  => <PttPanel  key={sub.id} sub={sub} />)}
    </div>
  )
}
