import { useEffect, useRef, useState } from 'react'
import { PhoneClient } from '../lib/PhoneClient'
import type { PhoneState, IncomingInfo } from '../lib/PhoneClient'
import { useAuth } from '../contexts/AuthContext'
import type { McpttUser } from '../contexts/AuthContext'
import { listMyGroups } from '../api/gms'
import type { GmsGroup, GmsMember } from '../api/gms'

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

function CallPanel({ user }: { user: McpttUser }) {
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
    client.connect(CWRTC_WS, user.phone_number, user.password, CALL_DOMAIN, user.phone_number)
    return () => { client.disconnect() }
  }, [user.phone_number, user.password])

  // Load contacts from GMS: collect unique members across all groups, excluding self
  useEffect(() => {
    listMyGroups(user.mcptt_id, user.access_token).then(groups => {
      const seen = new Set<string>()
      const list: Contact[] = []
      for (const g of groups) {
        for (const m of g.members) {
          if (m.uri === user.mcptt_id) continue
          const msisdn = m.uri.replace(/^tel:/, '')
          if (!seen.has(msisdn)) {
            seen.add(msisdn)
            list.push({ name: m.name, msisdn })
          }
        }
      }
      setContacts(list)
    }).catch(() => {})
  }, [user.mcptt_id, user.access_token])

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
        <span className="sp-number">{user.phone_number}</span>
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
  uri:       string   // full tel: URI
  priority:  number
  name:      string
  connected: boolean
}

type PttGroup = GmsGroup

function PttPanel({ user }: { user: McpttUser }) {
  const [state,         setState]         = useState<PhoneState>('disconnected')
  const [error,         setError]         = useState('')
  const [groups,        setGroups]        = useState<PttGroup[]>([])
  const [activeGroupId, setActiveGroupId] = useState<string | null>(null)
  const [members,       setMembers]       = useState<PttMember[]>([])
  const [speaker,       setSpeaker]       = useState<string | null>(null)
  const [floorOn,       setFloorOn]       = useState(false)

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
    if (!speaker || speaker === user.mcptt_id) return
    if (!floorOnRef.current) return
    const speakerMember = membersRef.current.find(m => m.uri === speaker)
    const myMember      = membersRef.current.find(m => m.uri === user.mcptt_id)
    if (speakerMember && myMember && speakerMember.priority < myMember.priority) {
      setFloorOn(false)
      floorOnRef.current = false
      clientRef.current?.setPttFloor(false)
    }
  }, [speaker, user.mcptt_id])

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
        setMembers(ms => ms.map(m => m.uri === userId ? { ...m, connected } : m))
      },
    })
    clientRef.current = client
    if (audioRef.current) client.setAudioElement(audioRef.current)
    client.connect(CWRTC_WS, user.phone_number, user.password, PTT_DOMAIN, user.phone_number)
    return () => { client.disconnect() }
  }, [user.phone_number, user.password])

  // Load groups from GMS; filter to groups where self is a member
  useEffect(() => {
    listMyGroups(user.mcptt_id, user.access_token).then(all => {
      setGroups(all.filter(g => g.members.some((m: GmsMember) => m.uri === user.mcptt_id)))
    }).catch(() => {})
  }, [user.mcptt_id, user.access_token])

  // 활성 그룹 멤버 목록 갱신
  useEffect(() => {
    if (!activeGroupId) { setMembers([]); return }
    const g = groupsRef.current.find(g => g.id === activeGroupId)
    if (!g) { setMembers([]); return }
    const ms: PttMember[] = g.members
      .slice()
      .sort((a, b) => a.priority - b.priority)
      .map(m => ({ uri: m.uri, priority: m.priority, name: m.name || m.uri, connected: false }))
    setMembers(ms)
    membersRef.current = ms
  }, [activeGroupId])

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
        <span className="sp-number">{user.phone_number}</span>
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
                    <span className="sp-group-name">{g.display_name}</span>
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
                      const isSpeaker = m.uri === speaker
                      const isMe      = m.uri === user.mcptt_id
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
  if (!user) return null

  return (
    <div className="sp-page">
      <CallPanel user={user} />
      <PttPanel  user={user} />
    </div>
  )
}
