import { useState, useEffect, useRef, useCallback } from 'react'
import { flowApi, type FlowMessage } from '../api/flow'
import Modal from '../components/Modal'

/** ts "HH:MM:SS.uuuuuu" 에서 hour 추출 */
function hourFromTs(ts: string): string | undefined {
  if (ts && ts.length >= 2) return ts.slice(0, 2)
  return undefined
}

/** dir 추론 — 기록 주체(nodeId, 예: cmp_01) 관점의 TX/RX.
 *  같은 메시지라도 CSP 기록분은 TX(송신), CMP 기록분은 RX(수신)로 갈린다 —
 *  msg 원문 파일의 dir 필드와 동일 관점이라 원문 역조회 dir 매칭에도 그대로 쓴다. */
export function inferDir(msg: FlowMessage): string {
  const nid = (msg.nodeId || msg.node || '').replace(/_\d+$/, '') || 'csp'
  if (msg.from === nid) return msg.to === nid ? '' : 'TX'   // from==to==자기 = 내부(INT) 이벤트
  if (msg.to === nid) return 'RX'
  return 'TX'
}

/** proto 변환: JSON→JSON(CMP), CSC→CSC, 기본 SIP */
function detailProto(msg: FlowMessage): string {
  return msg.proto || 'SIP'
}

// ── 컴포넌트 목록 ─────────────────────────────────────────────
const ACTOR_LABEL: Record<string, string> = {
  ue: 'UE', ue_o: 'UEᴼ', ue_t: 'UEᵀ', cwrtc: 'CWRTC', csc: 'CSC', csp: 'CSP', cmp: 'CMP',
}

// actor 표시명 (하단 목록용 — 번호 포함)
function actorLabel(a: string): string {
  if (ACTOR_LABEL[a]) return ACTOR_LABEL[a]
  const m = a.match(/^ue\((.+)\)$/)
  if (m) return `UE(${m[1]})`
  return a.toUpperCase()
}

// ue(+82571900001) → 번호 부분 추출
function extractUeNumber(actor: string): string {
  const m = actor.match(/^ue\((.+)\)$/)
  return m ? m[1] : ''
}

// ue(+번호) actor를 'ue' 하나로 통합, 라벨에 번호 표시
function normalizeMessages(messages: FlowMessage[]): FlowMessage[] {
  return messages.map(m => {
    let from = m.from, to = m.to, label = m.label
    const fromNum = extractUeNumber(from)
    const toNum = extractUeNumber(to)
    // ue(+번호) → 'ue' 노드로 통합, 라벨에 번호 축약 추가
    if (fromNum) {
      from = 'ue'
      const short = fromNum.length > 4 ? '..' + fromNum.slice(-4) : fromNum
      label = `${label}(${short})`
    }
    if (toNum) {
      to = 'ue'
      const short = toNum.length > 4 ? '..' + toNum.slice(-4) : toNum
      if (!fromNum) label = `${label}(${short})`  // from이 이미 번호를 포함하면 중복 방지
    }
    return { ...m, from, to, label }
  })
}

// 메시지에서 사용된 actor를 순서대로 추출
function deriveActors(messages: FlowMessage[]): string[] {
  // VoLTE: ue_o → csp → cmp → ue_t, PTT: csc → csp → cmp → ue
  const ORDER = ['csc', 'ue_o', 'cwrtc', 'csp', 'cmp', 'ue_t', 'ue']
  const used = new Set<string>()
  messages.forEach(m => { used.add(m.from); used.add(m.to) })
  return ORDER.filter(a => used.has(a))
}

// ── SVG 상수 ─────────────────────────────────────────────────────────────
const COL_W    = 160  // actor 열 간격
const MARGIN_L = 80   // 왼쪽 여백
const HEAD_H   = 50   // 헤더 영역 높이
const ROW_H    = 32   // 메시지 한 줄 높이
const ARROW_Y_OFFSET = 16  // 텍스트 기준선 아래 화살표 위치

// proto별 색상
const PROTO_COLOR: Record<string, string> = {
  SIP:   '#4b8cda',
  JSON:  '#e6832a',   // CSP↔CMP 제어
  CSC:   '#2ecc71',
  HTTPS: '#2ecc71',   // UE↔CSC (XCAP/IdMS)
  WS:    '#57b65a',
  RTP:   '#d94bbf',
  RTCP:  '#9b59b6',
  MCPTT: '#b9770e',   // MCPTT floor control (RTCP APP)
  DTMF:  '#c0392b',   // RFC 2833/4733 telephone-event
  INT:   '#7f8c8d',   // CMP 내부 이벤트
}

function protoColor(proto: string) {
  return PROTO_COLOR[proto] ?? '#888'
}

// SIP 메서드 계열별 색상 — 전부 파란색으로 뭉치지 않게 계열을 나눈다.
const SIP_METHOD_COLOR: Record<string, string> = {
  REGISTER:  '#4b8cda',  // 등록 (파랑)
  SUBSCRIBE: '#8e5ad8',  // 구독/알림 (보라)
  NOTIFY:    '#8e5ad8',
  PUBLISH:   '#a44bd9',
  INVITE:    '#0d9488',  // 호 제어 (청록)
  ACK:       '#0d9488',
  BYE:       '#0d9488',
  CANCEL:    '#0d9488',
  UPDATE:    '#0d9488',
  PRACK:     '#0d9488',
  REFER:     '#0d9488',
  MESSAGE:   '#0d9488',
  INFO:      '#0d9488',
  OPTIONS:   '#0d9488',
}

const SIP_FAIL_COLOR = '#d64545'  // 4xx~6xx — 실패는 계열과 무관하게 빨강 유지

/** 색상/응답 주석이 붙은 메시지 */
type ColoredMsg = FlowMessage & {
  _color?: string      // 표시 색 (요청: 계열색, 응답: 대응 요청의 계열색)
  _resp?: boolean      // 응답 여부 (점선 화살표)
  _reqMethod?: string  // 응답이 대응하는 요청 메서드 (라벨 표기)
}

/** 요청-응답 매칭 색상 부여: 응답은 "무엇에 대한 응답인지" 를 색과 라벨로 보여준다.
 *  같은 sesid 안에서 (from→to) 방향별 직전 요청 메서드를 기억해두고,
 *  역방향 응답이 오면 그 요청의 계열색을 물려받는다. (실패 응답은 빨강 고정)
 */
function annotateColors(messages: FlowMessage[]): ColoredMsg[] {
  const lastReq: Record<string, string> = {}  // `${sesid}|${from}>${to}` → 요청 메서드
  return messages.map(m => {
    const label = (m.label || '').trim()
    if (m.proto === 'SIP') {
      const head = label.split(/[ (]/)[0]
      if (SIP_METHOD_COLOR[head]) {
        lastReq[`${m.sesid}|${m.from}>${m.to}`] = head
        return { ...m, _color: SIP_METHOD_COLOR[head], _resp: false }
      }
      const code = parseInt(label, 10)
      if (!isNaN(code)) {
        const reqMethod = lastReq[`${m.sesid}|${m.to}>${m.from}`]
        const famColor = (reqMethod && SIP_METHOD_COLOR[reqMethod]) || PROTO_COLOR.SIP
        return {
          ...m,
          _color: code >= 400 ? SIP_FAIL_COLOR : famColor,
          _resp: true,
          _reqMethod: reqMethod,
        }
      }
      return { ...m, _color: PROTO_COLOR.SIP, _resp: false }
    }
    // CMP JSON: 명령(주황) / OK 응답은 직전 명령과 짝 (같은 mid)
    if (m.proto === 'JSON') {
      if (label === 'OK' || label === 'ERROR') {
        const reqMethod = lastReq[`${m.sesid}|${m.to}>${m.from}|${m.mid || ''}`]
        return {
          ...m,
          _color: label === 'ERROR' ? SIP_FAIL_COLOR : protoColor('JSON'),
          _resp: true,
          _reqMethod: reqMethod,
        }
      }
      lastReq[`${m.sesid}|${m.from}>${m.to}|${m.mid || ''}`] = label
      return { ...m, _color: protoColor('JSON'), _resp: false }
    }
    return { ...m, _color: protoColor(m.proto), _resp: false }
  })
}

/** 메시지별 표시 색 (annotateColors 주석 우선, 없으면 proto 색) */
function msgColor(msg: ColoredMsg): string {
  return msg._color ?? protoColor(msg.proto)
}

/** 응답 라벨: "200 ← REGISTER" 형태로 어떤 요청의 응답인지 표기 */
function msgLabel(msg: ColoredMsg): string {
  const base = msg.label || ''
  if (msg._resp && msg._reqMethod) return `${base} ‹${msg._reqMethod}›`
  return base
}

function actorX(name: string, actors: string[], colW: number = COL_W): number {
  const idx = actors.indexOf(name)
  return MARGIN_L + (idx < 0 ? 0 : idx) * colW
}

// ── FlowDiagram: 좌측 패널 (컨테이너 너비에 맞게 노드 간격 자동 조정) ──

interface FlowDiagramProps {
  actors: string[]
  messages: FlowMessage[]
  selIdx: number | null
  onSelect: (idx: number) => void
}

function FlowDiagram({ actors, messages, selIdx, onSelect }: FlowDiagramProps) {
  const containerRef = useRef<HTMLDivElement>(null)
  const [containerW, setContainerW] = useState(0)

  const measure = useCallback(() => {
    if (containerRef.current) setContainerW(containerRef.current.clientWidth)
  }, [])

  useEffect(() => {
    measure()
    const obs = new ResizeObserver(measure)
    if (containerRef.current) obs.observe(containerRef.current)
    return () => obs.disconnect()
  }, [measure])

  // 컨테이너 너비에 맞게 colW 계산 (최소 100, 여백 포함)
  const minColW = 100
  const colW = containerW > 0 && actors.length > 1
    ? Math.max(minColW, Math.floor((containerW - MARGIN_L * 2) / (actors.length - 1)))
    : COL_W
  const svgWidth = containerW > 0 ? Math.max(containerW, MARGIN_L * 2 + (actors.length - 1) * colW) : MARGIN_L * 2 + (actors.length - 1) * colW

  return (
    <div ref={containerRef} style={{ flex: '1 1 50%', overflow: 'auto', minWidth: 0, borderRight: '1px solid var(--border)' }}>
      {/* 노드 헤더 (sticky) */}
      <div style={{ position: 'sticky', top: 0, zIndex: 10, background: '#ffffff', borderBottom: '1px solid #e0e2ea' }}>
        <svg width={svgWidth} height={HEAD_H} style={{ fontFamily: 'monospace', fontSize: 12, display: 'block' }}>
          {actors.map(a => {
            const x = actorX(a, actors, colW)
            return (
              <g key={a}>
                <rect x={x - 45} y={8} width={90} height={28} rx={4}
                  fill="#ffffff" stroke="#2563eb" strokeWidth={1.5} />
                <text x={x} y={26} textAnchor="middle" fill="#1a1d2e" fontWeight="bold" fontSize={11}>
                  {actorLabel(a)}
                </text>
              </g>
            )
          })}
        </svg>
      </div>
      {/* 메시지 Flow */}
      <svg
        width={svgWidth}
        height={messages.length * ROW_H + 20}
        style={{ fontFamily: 'monospace', fontSize: 12, userSelect: 'none', cursor: 'default', display: 'block' }}
      >
        {actors.map(a => {
          const x = actorX(a, actors, colW)
          return <line key={a} x1={x} y1={0} x2={x} y2={messages.length * ROW_H + 10}
            stroke="#d0d5dd" strokeWidth={1} strokeDasharray="4 3" />
        })}
        {messages.map((msg, i) => {
          const y   = i * ROW_H
          const x1  = actorX(msg.from, actors, colW)
          const x2  = actorX(msg.to, actors, colW)
          const col = msgColor(msg)
          const dir = x2 > x1 ? 1 : x2 < x1 ? -1 : 0
          const arrowTip = dir !== 0 ? x2 - dir * 10 : x2
          const isSelected = selIdx === i
          return (
            <g key={i} style={{ cursor: 'pointer' }} onClick={() => onSelect(i)}>
              <rect x={0} y={y} width={svgWidth} height={ROW_H}
                fill={isSelected ? '#dbeafe' : 'transparent'} />
              <text x={4} y={y + 14} fill="#6b7280" fontSize={10}>{msg.ts.slice(0, 12)}</text>
              {dir !== 0 && <>
                <line x1={x1} y1={y + ARROW_Y_OFFSET} x2={arrowTip} y2={y + ARROW_Y_OFFSET}
                  stroke={col} strokeWidth={isSelected ? 2 : 1.5}
                  strokeDasharray={(msg as ColoredMsg)._resp ? '5 3' : undefined} />
                <polygon
                  points={`${x2},${y + ARROW_Y_OFFSET} ${x2 - dir * 8},${y + ARROW_Y_OFFSET - 5} ${x2 - dir * 8},${y + ARROW_Y_OFFSET + 5}`}
                  fill={col} />
              </>}
              <text x={(x1 + x2) / 2} y={y + ARROW_Y_OFFSET - 4}
                textAnchor="middle" fill={col} fontSize={11} fontWeight="bold">
                {msgLabel(msg as ColoredMsg)}{msg.detail ? `(${msg.detail})` : ''}
              </text>
            </g>
          )
        })}
      </svg>
    </div>
  )
}

// ── SequenceDiagram (VoLTE 호 이력 인라인 패널 등 임베드용) ──

interface SequenceDiagramProps {
  messages: FlowMessage[]
  onSelect: (idx: number) => void
  selectedIdx: number | null
}

export function SequenceDiagram({ messages: rawMessages, onSelect, selectedIdx }: SequenceDiagramProps) {
  // ue(+번호) actor들을 'ue' 하나로 통합 (다이어그램 표시용)
  const messages = normalizeMessages(rawMessages)
  const ACTORS = deriveActors(messages)

  // 컨테이너 폭 측정 — 노드(actor)를 좌우 가득 고르게 배치 (FlowDiagram 과 동일 방식).
  // 폭이 좁으면 최소 간격을 지키고 가로 스크롤로 넘긴다.
  const containerRef = useRef<HTMLDivElement>(null)
  const [containerW, setContainerW] = useState(0)
  const measure = useCallback(() => {
    if (containerRef.current) setContainerW(containerRef.current.clientWidth)
  }, [])
  useEffect(() => {
    measure()
    const obs = new ResizeObserver(measure)
    if (containerRef.current) obs.observe(containerRef.current)
    return () => obs.disconnect()
  }, [measure])

  const minColW = 100
  const colW = containerW > 0 && ACTORS.length > 1
    ? Math.max(minColW, Math.floor((containerW - MARGIN_L * 2) / (ACTORS.length - 1)))
    : (ACTORS.length > 6 ? 130 : ACTORS.length > 4 ? 150 : COL_W)
  const height = HEAD_H + messages.length * ROW_H + 20
  const width = containerW > 0
    ? Math.max(containerW, MARGIN_L * 2 + (ACTORS.length - 1) * colW)
    : MARGIN_L * 2 + (ACTORS.length - 1) * colW

  return (
    <div ref={containerRef} style={{ width: '100%', overflowX: 'auto' }}>
    <svg
      width={width}
      height={height}
      style={{ fontFamily: 'monospace', fontSize: 12, userSelect: 'none', cursor: 'default', display: 'block' }}
    >
      {/* ── 헤더: actor 이름 + 수직선 ── */}
      {ACTORS.map(a => {
        const x = actorX(a, ACTORS, colW)
        return (
          <g key={a}>
            <rect x={x - 45} y={8} width={90} height={28} rx={4}
              fill="#ffffff" stroke="#2563eb" strokeWidth={1.5} />
            <text x={x} y={26} textAnchor="middle" fill="#1a1d2e" fontWeight="bold">
              {actorLabel(a)}
            </text>
            {/* 수직 생명선 */}
            <line x1={x} y1={38} x2={x} y2={height - 10}
              stroke="#d0d5dd" strokeWidth={1} strokeDasharray="4 3" />
          </g>
        )
      })}

      {/* ── 메시지 화살표 ── */}
      {messages.map((msg, i) => {
        const y   = HEAD_H + i * ROW_H
        const x1  = actorX(msg.from, ACTORS, colW)
        const x2  = actorX(msg.to, ACTORS, colW)
        const col = msgColor(msg)
        const dir = x2 > x1 ? 1 : -1
        const arrowTip = x2 - dir * 10
        const isSelected = selectedIdx === i
        const bgColor = isSelected ? '#dbeafe' : 'transparent'

        return (
          <g key={i} style={{ cursor: 'pointer' }} onClick={() => onSelect(i)}>
            {/* 행 배경 */}
            <rect x={0} y={y} width={width} height={ROW_H}
              fill={bgColor} opacity={1} />

            {/* 타임스탬프 */}
            <text x={4} y={y + 14} fill="#6b7280" fontSize={10}>{msg.ts.slice(0, 12)}</text>

            {/* 화살선 (응답은 점선) */}
            <line x1={x1} y1={y + ARROW_Y_OFFSET} x2={arrowTip} y2={y + ARROW_Y_OFFSET}
              stroke={col} strokeWidth={isSelected ? 2 : 1.5}
              strokeDasharray={(msg as ColoredMsg)._resp ? '5 3' : undefined} />
            {/* 화살촉 */}
            <polygon
              points={`${x2},${y + ARROW_Y_OFFSET} ${x2 - dir * 8},${y + ARROW_Y_OFFSET - 5} ${x2 - dir * 8},${y + ARROW_Y_OFFSET + 5}`}
              fill={col} />

            {/* 라벨 (선 위) */}
            {(() => {
              const mx = (x1 + x2) / 2
              const labelY = y + ARROW_Y_OFFSET - 4
              return (
                <text x={mx} y={labelY} textAnchor="middle" fill={col} fontSize={11} fontWeight="bold">
                  {msgLabel(msg as ColoredMsg)}{msg.detail ? `(${msg.detail})` : ''}
                </text>
              )
            })()}
          </g>
        )
      })}
    </svg>
    </div>
  )
}

// ── 메시지 목록 테이블 ──────────────────────────────────────────────────

interface MessageListProps {
  messages: FlowMessage[]
  selectedIdx: number | null
  onSelect: (idx: number) => void
}

function MessageList({ messages, selectedIdx, onSelect }: MessageListProps) {
  return (
    <div style={{
      overflowY: 'auto',
      height: '100%',
      border: '1px solid #e0e2ea',
      borderRadius: 6,
      background: '#f8f9fa',
    }}>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12, fontFamily: 'monospace' }}>
        <thead>
          <tr style={{ position: 'sticky', top: 0, background: '#f0f1f3', zIndex: 1 }}>
            <th style={thStyle}>#</th>
            <th style={thStyle}>시간</th>
            <th style={thStyle}>From→To</th>
            <th style={thStyle}>모듈</th>
            <th style={thStyle}>TX/RX</th>
            <th style={thStyle}>프로토콜</th>
            <th style={thStyle}>Method</th>
          </tr>
        </thead>
        <tbody>
          {messages.map((msg, i) => {
            const isSelected = selectedIdx === i
            return (
              <tr
                key={i}
                onClick={() => onSelect(i)}
                style={{
                  cursor: 'pointer',
                  background: isSelected ? '#dbeafe' : 'transparent',
                }}
              >
                <td style={tdStyle}>{i + 1}</td>
                <td style={tdStyle}>{msg.ts.slice(0, 12)}</td>
                <td style={{ ...tdStyle, color: '#1a1d2e' }}>
                  {actorLabel(msg.from)}<span style={{ color: '#7a8fa8' }}>{'\u2192'}</span>{actorLabel(msg.to)}
                </td>
                <td style={{ ...tdStyle, color: 'var(--text-muted)', fontSize: 10 }}>
                  {/* \uae30\ub85d \uc8fc\uccb4 \ud504\ub85c\uc138\uc2a4\uba85+ID (flow \ud30c\uc77c \uc18c\uc720\uc790, \uc608: CSP_01) \u2014 nodeId \uc5c6\uc73c\uba74(\uad6c \uc751\ub2f5) node \ub85c \ud3f4\ubc31 */}
                  {(msg.nodeId || msg.node || '').toUpperCase()}
                </td>
                <td style={tdStyle}>
                  {(() => {
                    const d = inferDir(msg)
                    return d ? (
                      <span style={{ display: 'inline-block', padding: '1px 5px', borderRadius: 3,
                        fontSize: 9, fontWeight: 700, color: '#fff',
                        background: d === 'TX' ? '#2563eb' : '#16a34a' }}>{d}</span>
                    ) : <span style={{ color: 'var(--text-muted)' }}>\u2014</span>
                  })()}
                </td>
                <td style={tdStyle}>
                  <span style={{
                    display: 'inline-block',
                    padding: '1px 6px',
                    borderRadius: 3,
                    fontSize: 10,
                    fontWeight: 600,
                    color: '#fff',
                    background: protoColor(msg.proto),
                  }}>
                    {msg.proto}
                  </span>
                </td>
                <td style={{ ...tdStyle, fontWeight: 600, color: msgColor(msg) }}>
                  {msgLabel(msg as ColoredMsg)}{msg.detail ? <span style={{ fontWeight: 400, color: 'var(--text-muted)' }}>({msg.detail})</span> : ''}
                </td>
              </tr>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}

const thStyle: React.CSSProperties = {
  textAlign: 'left',
  padding: '6px 8px',
  color: '#6b7280',
  fontWeight: 600,
  borderBottom: '1px solid #e0e2ea',
  whiteSpace: 'nowrap',
}

const tdStyle: React.CSSProperties = {
  padding: '4px 8px',
  color: '#8a9ab0',
  whiteSpace: 'nowrap',
  borderBottom: '1px solid #e0e2ea',
}

// ── main ─────────────────────────────────────────────────────────────────

interface FlowPageProps {
  callId: string
  date?: string
  callType?: 'volte' | 'ptt'
  onClose: () => void
  prefetchedNodes?: Record<string, FlowMessage[]>
  prefetchedMessages?: FlowMessage[]
  /** true 면 Modal 없이 페이지 안에 바로 렌더 (메세지 이력 페이지 등 임베드용) */
  inline?: boolean
}

export default function FlowPage({ callId, date, callType, onClose, prefetchedNodes, prefetchedMessages, inline }: FlowPageProps) {
  const [allNodes, setAllNodes] = useState<Record<string, FlowMessage[]>>({})
  const [enabledNodes, setEnabledNodes] = useState<Set<string>>(new Set())
  const [loading,  setLoading]  = useState(!prefetchedMessages)
  const [error,    setError]    = useState<string | null>(null)
  const [selIdx,   setSelIdx]   = useState<number | null>(null)
  const [bodyText, setBodyText] = useState<string | null>(null)
  const [bodyLoading, setBodyLoading] = useState(false)

  // nodes 구조 또는 messages 배열을 allNodes로 변환
  const applyResponse = useCallback((r: { nodes?: Record<string, FlowMessage[]>; messages?: FlowMessage[] }) => {
    if (r.nodes) {
      // 기록 주체(nodeId) 기준 재그룹 — 백엔드 표시 그룹(_flow_node_of)은 CSP 가 기록한
      // CMP 제어 TX 도 'cmp' 로 묶는다. 노드 토글은 "누가 기록했나"가 기준이어야
      // CSP 단독 선택 시 CSP 송신 기록이 보인다. nodeId 없으면(구 응답) 백엔드 그룹 유지.
      const processed: Record<string, FlowMessage[]> = {}
      for (const [node, msgs] of Object.entries(r.nodes)) {
        for (const m of msgs) {
          const key = (m.nodeId || '').replace(/_\d+$/, '') || node
          if (!processed[key]) processed[key] = []
          processed[key].push({ ...m, node: m.node || key })
        }
      }
      for (const k of Object.keys(processed)) {
        processed[k].sort((a, b) => (a.ts || '').localeCompare(b.ts || ''))
      }
      setAllNodes(processed)
      setEnabledNodes(new Set(Object.keys(processed)))
    } else if (r.messages) {
      // 레거시: 단일 배열 → 'all' 노드
      setAllNodes({ all: r.messages })
      setEnabledNodes(new Set(['all']))
    }
  }, [])

  useEffect(() => {
    if (prefetchedNodes) {
      applyResponse({ nodes: prefetchedNodes })
      setLoading(false)
      return
    }
    if (prefetchedMessages) {
      applyResponse({ messages: prefetchedMessages })
      setLoading(false)
      return
    }
    setLoading(true)
    setError(null)
    flowApi.get(callId, date, callType)
      .then(r => applyResponse(r))
      .catch(e => setError(String(e)))
      .finally(() => setLoading(false))
  }, [callId, date, callType, prefetchedNodes, prefetchedMessages, applyResponse])

  // 선택된 노드의 메시지를 합쳐서 시간순 정렬 → 요청-응답 매칭 색상 부여
  const messages = annotateColors(
    Object.entries(allNodes)
      .filter(([node]) => enabledNodes.has(node))
      .flatMap(([, msgs]) => msgs)
      .sort((a, b) => (a.ts || '').localeCompare(b.ts || ''))
  )

  function handleSelect(idx: number) {
    const newIdx = selIdx === idx ? null : idx
    setSelIdx(newIdx)
    setBodyText(null)

    if (newIdx !== null) {
      const msg = messages[newIdx]
      // body가 이미 있으면 그대로 사용 (prefetched 또는 기존 형식)
      if (msg.body) {
        setBodyText(msg.body)
        return
      }
      // body 없으면 API로 조회 (seq+iface 우선, fallback ts+dir)
      const d = date || new Date().toISOString().slice(0, 10)
      const hour = hourFromTs(msg.ts)
      const seq = msg.seq
      const dir = inferDir(msg)
      const proto = detailProto(msg)
      const iface = msg.iface

      // node: 기록 주체(nodeId) 우선 — 원문은 기록 주체의 msg 파일에 있다 (cmp_01 → cmp)
      const nodeStr = (msg.nodeId || msg.node || '').replace(/_\d+$/, '')

      setBodyLoading(true)
      flowApi.getBody(d, hour, seq, msg.ts, dir, proto, iface, nodeStr, msg.sesid, msg.mid)
        .then(r => setBodyText(r.body || ''))
        .catch(() => setBodyText('(body 조회 실패)'))
        .finally(() => setBodyLoading(false))
    }
  }

  const selected = selIdx !== null ? messages[selIdx] : null

  // 정규화된 메시지 (다이어그램용)
  const normalizedMsgs = normalizeMessages(messages)

  const inner = (
    <>
      {/* 노드 필터 */}
      {Object.keys(allNodes).length > 0 && (
        <div style={{ display: 'flex', gap: 12, padding: '6px 16px', borderBottom: '1px solid var(--border)', fontSize: 13, alignItems: 'center' }}>
          <span style={{ color: 'var(--text-muted)', fontWeight: 500 }}>노드:</span>
          {Object.keys(allNodes).map(node => (
            <label key={node} style={{ display: 'flex', alignItems: 'center', gap: 4, cursor: 'pointer' }}>
              <input type="checkbox" checked={enabledNodes.has(node)}
                onChange={() => setEnabledNodes(prev => {
                  const next = new Set(prev)
                  if (next.has(node)) next.delete(node); else next.add(node)
                  return next
                })} />
              {(node || '').toUpperCase()}
              <span style={{ color: 'var(--text-muted)', fontSize: 11 }}>({allNodes[node]?.length || 0})</span>
            </label>
          ))}

          {/* 색상 범례 — 응답은 요청과 같은 색(점선 화살표), 실패(4xx+)만 빨강 */}
          <span style={{ marginLeft: 'auto', display: 'flex', gap: 10, fontSize: 11, color: 'var(--text-muted)', flexWrap: 'wrap', alignItems: 'center' }}>
            {[
              ['등록', '#4b8cda'], ['구독/알림', '#8e5ad8'], ['호 제어', '#0d9488'],
              ['실패응답', '#d64545'],
              ['CMP제어', '#e6832a'], ['CSC(XCAP)', '#2ecc71'],
            ].map(([name, c]) => (
              <span key={name} style={{ display: 'flex', alignItems: 'center', gap: 3 }}>
                <span style={{ width: 10, height: 10, borderRadius: 2, background: c, display: 'inline-block' }} />
                {name}
              </span>
            ))}
            <span style={{ display: 'flex', alignItems: 'center', gap: 3 }}>
              <svg width={22} height={10}><line x1={0} y1={5} x2={22} y2={5} stroke="#8a9ab0" strokeWidth={1.5} strokeDasharray="5 3" /></svg>
              응답 (요청과 같은 색)
            </span>
          </span>
        </div>
      )}

      {loading && <div className="empty">로딩 중…</div>}
      {error   && <div className="empty" style={{ color: '#e96' }}>오류: {error}</div>}

      {!loading && !error && messages.length === 0 && (
        <div className="empty">메시지 기록이 없습니다.</div>
      )}

      {!loading && !error && messages.length > 0 && (() => {
        const ACTORS = deriveActors(normalizedMsgs)

        return (
          <div style={{ display: 'flex', height: '100%', gap: 0 }}>
            {/* ── 좌측: 시퀀스 다이어그램 ── */}
            <FlowDiagram actors={ACTORS} messages={normalizedMsgs} selIdx={selIdx} onSelect={handleSelect} />

            {/* ── 우측: 메시지 목록(상단) + 상세(하단) 분할 ── */}
            <div style={{ flex: '1 1 50%', display: 'flex', flexDirection: 'column', minWidth: 0, padding: '0 12px', overflow: 'hidden' }}>
              {/* 메시지 목록 (상단 50%) */}
              <div style={{ flex: '1 1 50%', overflow: 'auto', minHeight: 0 }}>
                <MessageList messages={messages} selectedIdx={selIdx} onSelect={handleSelect} />
              </div>
              {/* 메시지 상세 (하단 50%) */}
              <div style={{ flex: '1 1 50%', display: 'flex', flexDirection: 'column', overflow: 'hidden', borderTop: '1px solid #e0e2ea', minHeight: 0 }}>
                {selected ? (
                  <>
                    <div style={{ flex: '0 0 auto', padding: '8px 12px', display: 'flex', gap: 8, alignItems: 'center', borderBottom: '1px solid var(--border)', background: '#f0f1f3' }}>
                      <span className="badge" style={{ backgroundColor: protoColor(selected.proto), color: '#fff' }}>{selected.proto}</span>
                      <span style={{ fontWeight: 600, fontSize: 12 }}>{selected.label}</span>
                      <span className="ts">{actorLabel(selected.from)} {'\u2192'} {actorLabel(selected.to)}</span>
                      <span className="ts" style={{ marginLeft: 'auto' }}>{selected.ts}</span>
                    </div>
                    <pre style={{
                      flex: 1, margin: 0, padding: 12, overflow: 'auto',
                      background: '#f8f9fa', fontSize: 12, lineHeight: 1.5,
                      color: '#1a1d2e', whiteSpace: 'pre-wrap', wordBreak: 'break-all',
                      minHeight: 0,
                    }}>
                      {bodyLoading ? '...' : (() => {
                        // JSON이면 indent 2로 포맷팅
                        const b = bodyText ?? ''
                        if (b && (b.startsWith('{') || b.startsWith('['))) {
                          try { return JSON.stringify(JSON.parse(b), null, 2) } catch {}
                        }
                        return b || '(body 없음)'
                      })()}
                    </pre>
                  </>
                ) : (
                  <div style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center', color: '#8a9ab0', fontSize: 13 }}>
                    메시지를 선택하세요
                  </div>
                )}
              </div>
            </div>
          </div>
        )
      })()}
    </>
  )

  // inline: Modal 래핑 없이 페이지 안에 바로 렌더
  if (inline) {
    return (
      <div style={{ flex: 1, minHeight: 0, height: '100%', display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
        {inner}
      </div>
    )
  }

  return (
    <Modal
      title={`메시지 플로우 — ${callId}`}
      onClose={onClose}
      fullscreen
    >
      {inner}
    </Modal>
  )
}
