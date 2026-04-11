import { useState, useEffect } from 'react'
import { flowApi, type FlowMessage } from '../api/flow'
import Modal from '../components/Modal'

// ── 컴포넌트 목록 ─────────────────────────────────────────────
const ACTOR_LABEL: Record<string, string> = {
  ue: 'UE', ue_o: 'UE\u1d3c', ue_t: 'UE\u1d40', cwrtc: 'CWRTC', csp: 'CSP', cmp: 'CMP',
}

// 메시지에서 사용된 actor를 순서대로 추출
function deriveActors(messages: FlowMessage[]): string[] {
  const ORDER = ['ue_o', 'ue', 'cwrtc', 'csp', 'cmp', 'ue_t']
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
  WS:   '#57b65a',
  RTP:  '#d94bbf',
  RTCP: '#9b59b6',
}

function protoColor(proto: string) {
  return PROTO_COLOR[proto] ?? '#888'
}

function actorX(name: string, actors: string[]): number {
  const idx = actors.indexOf(name)
  return MARGIN_L + (idx < 0 ? 0 : idx) * COL_W
}

interface SequenceDiagramProps {
  messages: FlowMessage[]
  onSelect: (idx: number) => void
  selectedIdx: number | null
}

function SequenceDiagram({ messages, onSelect, selectedIdx }: SequenceDiagramProps) {
  const ACTORS = deriveActors(messages)
  const height = HEAD_H + messages.length * ROW_H + 20
  const width  = MARGIN_L * 2 + (ACTORS.length - 1) * COL_W

  return (
    <svg
      width={width}
      height={height}
      style={{ fontFamily: 'monospace', fontSize: 12, userSelect: 'none', cursor: 'default' }}
    >
      {/* ── 헤더: actor 이름 + 수직선 ── */}
      {ACTORS.map(a => {
        const x = actorX(a, ACTORS)
        return (
          <g key={a}>
            <rect x={x - 36} y={8} width={72} height={28} rx={4}
              fill="#1e2635" stroke="#4a90d9" strokeWidth={1.5} />
            <text x={x} y={26} textAnchor="middle" fill="#c8d8f0" fontWeight="bold">
              {ACTOR_LABEL[a] ?? a}
            </text>
            {/* 수직 생명선 */}
            <line x1={x} y1={38} x2={x} y2={height - 10}
              stroke="#3a4a5f" strokeWidth={1} strokeDasharray="4 3" />
          </g>
        )
      })}

      {/* ── 메시지 화살표 ── */}
      {messages.map((msg, i) => {
        const y   = HEAD_H + i * ROW_H
        const x1  = actorX(msg.from, ACTORS)
        const x2  = actorX(msg.to, ACTORS)
        const col = protoColor(msg.proto)
        const dir = x2 > x1 ? 1 : -1
        const arrowTip = x2 - dir * 10
        const isSelected = selectedIdx === i
        const bgColor = isSelected ? '#2d3f5a' : (i % 2 === 0 ? 'transparent' : '#171e2b')

        return (
          <g key={i} style={{ cursor: 'pointer' }} onClick={() => onSelect(i)}>
            {/* 행 배경 */}
            <rect x={0} y={y} width={width} height={ROW_H}
              fill={bgColor} opacity={isSelected ? 1 : 0.7} />

            {/* 타임스탬프 */}
            <text x={4} y={y + 14} fill="#7a8fa8" fontSize={10}>{msg.ts.slice(0, 12)}</text>

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
      maxHeight: '30vh',
      border: '1px solid #2a3a50',
      borderRadius: 6,
      background: '#0e1520',
    }}>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12, fontFamily: 'monospace' }}>
        <thead>
          <tr style={{ position: 'sticky', top: 0, background: '#1a2333', zIndex: 1 }}>
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
                  background: isSelected ? '#2d3f5a' : (i % 2 === 0 ? 'transparent' : '#131b27'),
                }}
              >
                <td style={tdStyle}>{i + 1}</td>
                <td style={tdStyle}>{msg.ts.slice(0, 12)}</td>
                <td style={{ ...tdStyle, color: '#c8d8f0' }}>{ACTOR_LABEL[msg.from] ?? msg.from}</td>
                <td style={{ ...tdStyle, color: '#7a8fa8' }}>{'\u2192'}</td>
                <td style={{ ...tdStyle, color: '#c8d8f0' }}>{ACTOR_LABEL[msg.to] ?? msg.to}</td>
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
  color: '#7a8fa8',
  fontWeight: 600,
  borderBottom: '1px solid #2a3a50',
  whiteSpace: 'nowrap',
}

const tdStyle: React.CSSProperties = {
  padding: '4px 8px',
  color: '#8a9ab0',
  whiteSpace: 'nowrap',
  borderBottom: '1px solid #1a2333',
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
  const [showDetail, setShowDetail] = useState(false)

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
    if (selIdx === idx) {
      // 같은 메시지 다시 클릭 → 상세 토글
      if (showDetail) {
        setShowDetail(false)
        setSelIdx(null)
      } else {
        setShowDetail(true)
      }
    } else {
      setSelIdx(idx)
      setShowDetail(true)
    }
  }

  function handleBackToList() {
    setShowDetail(false)
    setSelIdx(null)
  }

  const selected = selIdx !== null ? messages[selIdx] : null

  return (
    <Modal
      title={`\uba54\uc2dc\uc9c0 \ud50c\ub85c\uc6b0 \u2014 ${callId}`}
      onClose={onClose}
      wide
    >
      {loading && <div className="empty">{'\ub85c\ub529 \uc911\u2026'}</div>}
      {error   && <div className="empty" style={{ color: '#e96' }}>{'\uc624\ub958'}: {error}</div>}

      {!loading && !error && messages.length === 0 && (
        <div className="empty">{'\uba54\uc2dc\uc9c0 \uae30\ub85d\uc774 \uc5c6\uc2b5\ub2c8\ub2e4.'}</div>
      )}

      {!loading && !error && messages.length > 0 && (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
          {/* 상단: 시퀀스 다이어그램 */}
          <div style={{ overflowX: 'auto', overflowY: 'auto', maxHeight: '45vh', flex: '0 0 auto' }}>
            <SequenceDiagram
              messages={messages}
              onSelect={handleSelect}
              selectedIdx={selIdx}
            />
          </div>

          {/* 하단: 메시지 목록 또는 상세 */}
          {showDetail && selected ? (
            <div>
              <div style={{ marginBottom: 8, display: 'flex', gap: 8, alignItems: 'center' }}>
                <button
                  className="btn btn--ghost"
                  style={{ fontSize: 11, padding: '2px 8px' }}
                  onClick={handleBackToList}
                >
                  {'\uc804\uccb4'}
                </button>
                <span className="badge" style={{ backgroundColor: protoColor(selected.proto), color: '#fff' }}>
                  {selected.proto}
                </span>
                <span style={{ fontWeight: 600 }}>{selected.label}</span>
                <span className="ts">{ACTOR_LABEL[selected.from] ?? selected.from} {'\u2192'} {ACTOR_LABEL[selected.to] ?? selected.to}</span>
                <span className="ts" style={{ marginLeft: 'auto' }}>{selected.ts}</span>
              </div>
              <pre style={{
                background: '#0e1520',
                border: '1px solid #2a3a50',
                borderRadius: 6,
                padding: 12,
                overflowX: 'auto',
                overflowY: 'auto',
                maxHeight: '30vh',
                fontSize: 12,
                lineHeight: 1.5,
                color: '#c8d8f0',
                whiteSpace: 'pre-wrap',
                wordBreak: 'break-all',
                margin: 0,
              }}>
                {selected.body}
              </pre>
            </div>
          ) : (
            <MessageList
              messages={messages}
              selectedIdx={selIdx}
              onSelect={handleSelect}
            />
          )}
        </div>
      )}

      <div className="modal-footer">
        <span className="ts" style={{ marginRight: 'auto' }}>
          {messages.length}{'\uac74'} {'\u00b7'} {date ?? '\uc624\ub298'}
        </span>
        <button className="btn btn--ghost" onClick={onClose}>{'\ub2eb\uae30'}</button>
      </div>
    </Modal>
  )
}
