import { useState, useEffect, useRef, useCallback } from 'react'
import { flowApi, type FlowMessage } from '../api/flow'
import Modal from '../components/Modal'

/** ts "HH:MM:SS.uuuuuu" 에서 hour 추출 */
function hourFromTs(ts: string): string | undefined {
  if (ts && ts.length >= 2) return ts.slice(0, 2)
  return undefined
}

/** dir 추론: from=csp이면 TX, 아니면 RX (flow.jsonl의 원래 dir 복원) */
function inferDir(msg: FlowMessage): string {
  // csp에서 나가면 TX, csp로 들어오면 RX
  if (msg.from === 'csp') return 'TX'
  if (msg.to === 'csp') return 'RX'
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
  SIP:  '#4b8cda',
  JSON: '#e6832a',
  CSC:  '#2ecc71',
  WS:   '#57b65a',
  RTP:  '#d94bbf',
  RTCP: '#9b59b6',
}

function protoColor(proto: string) {
  return PROTO_COLOR[proto] ?? '#888'
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
          const col = protoColor(msg.proto)
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
                  stroke={col} strokeWidth={isSelected ? 2 : 1.5} />
                <polygon
                  points={`${x2},${y + ARROW_Y_OFFSET} ${x2 - dir * 8},${y + ARROW_Y_OFFSET - 5} ${x2 - dir * 8},${y + ARROW_Y_OFFSET + 5}`}
                  fill={col} />
              </>}
              <text x={(x1 + x2) / 2} y={y + ARROW_Y_OFFSET - 4}
                textAnchor="middle" fill={col} fontSize={11} fontWeight="bold">
                {msg.label}
              </text>
            </g>
          )
        })}
      </svg>
    </div>
  )
}

// ── SequenceDiagram (legacy, unused but kept) ──

interface SequenceDiagramProps {
  messages: FlowMessage[]
  onSelect: (idx: number) => void
  selectedIdx: number | null
}

export function SequenceDiagram({ messages: rawMessages, onSelect, selectedIdx }: SequenceDiagramProps) {
  // ue(+번호) actor들을 'ue' 하나로 통합 (다이어그램 표시용)
  const messages = normalizeMessages(rawMessages)
  const ACTORS = deriveActors(messages)
  // actor 수에 따라 열 간격 동적 조정
  const colW = ACTORS.length > 6 ? 130 : ACTORS.length > 4 ? 150 : COL_W
  const height = HEAD_H + messages.length * ROW_H + 20
  const width  = MARGIN_L * 2 + (ACTORS.length - 1) * colW

  return (
    <svg
      width={width}
      height={height}
      style={{ fontFamily: 'monospace', fontSize: 12, userSelect: 'none', cursor: 'default' }}
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
        const col = protoColor(msg.proto)
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

            {/* 화살선 */}
            <line x1={x1} y1={y + ARROW_Y_OFFSET} x2={arrowTip} y2={y + ARROW_Y_OFFSET}
              stroke={col} strokeWidth={isSelected ? 2 : 1.5} />
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
                  {msg.label}
                </text>
              )
            })()}
          </g>
        )
      })}
    </svg>
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
            <th style={thStyle}>시각</th>
            <th style={thStyle}>From</th>
            <th style={thStyle}></th>
            <th style={thStyle}>To</th>
            <th style={thStyle}>Proto</th>
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
                <td style={{ ...tdStyle, color: '#1a1d2e' }}>{actorLabel(msg.from)}</td>
                <td style={{ ...tdStyle, color: '#7a8fa8' }}>{'\u2192'}</td>
                <td style={{ ...tdStyle, color: '#1a1d2e' }}>{actorLabel(msg.to)}</td>
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
                <td style={{ ...tdStyle, fontWeight: 600, color: protoColor(msg.proto) }}>{msg.label}</td>
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
  onClose: () => void
  /** Pre-fetched messages. When provided, skips the API call. */
  prefetchedMessages?: FlowMessage[]
}

export default function FlowPage({ callId, date, onClose, prefetchedMessages }: FlowPageProps) {
  const [messages, setMessages] = useState<FlowMessage[]>(prefetchedMessages ?? [])
  const [loading,  setLoading]  = useState(!prefetchedMessages)
  const [error,    setError]    = useState<string | null>(null)
  const [selIdx,   setSelIdx]   = useState<number | null>(null)
  const [bodyText, setBodyText] = useState<string | null>(null)
  const [bodyLoading, setBodyLoading] = useState(false)

  useEffect(() => {
    if (prefetchedMessages) {
      setMessages(prefetchedMessages)
      setLoading(false)
      return
    }
    setLoading(true)
    setError(null)
    flowApi.get(callId, date)
      .then(r => setMessages(r.messages))
      .catch(e => setError(String(e)))
      .finally(() => setLoading(false))
  }, [callId, date, prefetchedMessages])

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

      setBodyLoading(true)
      flowApi.getBody(d, hour, seq, msg.ts, dir, proto, iface)
        .then(r => setBodyText(r.body || ''))
        .catch(() => setBodyText('(body 조회 실패)'))
        .finally(() => setBodyLoading(false))
    }
  }

  const selected = selIdx !== null ? messages[selIdx] : null

  // 정규화된 메시지 (다이어그램용)
  const normalizedMsgs = normalizeMessages(messages)

  return (
    <Modal
      title={`메시지 플로우 — ${callId}`}
      onClose={onClose}
      fullscreen
    >
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
    </Modal>
  )
}
