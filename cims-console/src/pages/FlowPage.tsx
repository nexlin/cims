import { useState, useEffect } from 'react'
import { flowApi, type FlowMessage } from '../api/flow'
import Modal from '../components/Modal'

// ── 컴포넌트 목록 (표시 순서) ─────────────────────────────────────────────
const ACTORS = ['ue', 'cwrtc', 'csp', 'cmp']
const ACTOR_LABEL: Record<string, string> = {
  ue: 'UE', cwrtc: 'CWRTC', csp: 'CSP', cmp: 'CMP',
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
}

function protoColor(proto: string) {
  return PROTO_COLOR[proto] ?? '#888'
}

function actorX(name: string): number {
  const idx = ACTORS.indexOf(name)
  return MARGIN_L + (idx < 0 ? 0 : idx) * COL_W
}

interface SequenceDiagramProps {
  messages: FlowMessage[]
  onSelect: (msg: FlowMessage) => void
  selectedIdx: number | null
}

function SequenceDiagram({ messages, onSelect, selectedIdx }: SequenceDiagramProps) {
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
        const x = actorX(a)
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
        const x1  = actorX(msg.from)
        const x2  = actorX(msg.to)
        const col = protoColor(msg.proto)
        const dir = x2 > x1 ? 1 : -1
        const arrowTip = x2 - dir * 10
        const isSelected = selectedIdx === i
        const bgColor = isSelected ? '#2d3f5a' : (i % 2 === 0 ? 'transparent' : '#171e2b')

        return (
          <g key={i} style={{ cursor: 'pointer' }} onClick={() => onSelect(msg)}>
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

// ── main ─────────────────────────────────────────────────────────────────

interface FlowPageProps {
  callId: string
  date?: string
  onClose: () => void
}

export default function FlowPage({ callId, date, onClose }: FlowPageProps) {
  const [messages, setMessages] = useState<FlowMessage[]>([])
  const [loading,  setLoading]  = useState(true)
  const [error,    setError]    = useState<string | null>(null)
  const [selected, setSelected] = useState<FlowMessage | null>(null)
  const [selIdx,   setSelIdx]   = useState<number | null>(null)

  useEffect(() => {
    setLoading(true)
    setError(null)
    flowApi.get(callId, date)
      .then(r => setMessages(r.messages))
      .catch(e => setError(String(e)))
      .finally(() => setLoading(false))
  }, [callId, date])

  function handleSelect(msg: FlowMessage, idx: number) {
    setSelected(msg)
    setSelIdx(idx)
  }

  return (
    <Modal
      title={`메시지 플로우 — ${callId}`}
      onClose={onClose}
      wide
    >
      {loading && <div className="empty">로딩 중…</div>}
      {error   && <div className="empty" style={{ color: '#e96' }}>오류: {error}</div>}

      {!loading && !error && messages.length === 0 && (
        <div className="empty">메시지 기록이 없습니다.</div>
      )}

      {!loading && !error && messages.length > 0 && (
        <div style={{ display: 'flex', gap: 16, alignItems: 'flex-start' }}>
          {/* 시퀀스 다이어그램 */}
          <div style={{ overflowX: 'auto', overflowY: 'auto', maxHeight: '70vh', flex: '0 0 auto' }}>
            <SequenceDiagram
              messages={messages}
              onSelect={(msg) => {
                const idx = messages.indexOf(msg)
                handleSelect(msg, idx)
              }}
              selectedIdx={selIdx}
            />
          </div>

          {/* 선택한 메시지 원문 */}
          {selected && (
            <div style={{ flex: 1, minWidth: 0 }}>
              <div style={{ marginBottom: 8, display: 'flex', gap: 8, alignItems: 'center' }}>
                <span className="badge" style={{ backgroundColor: protoColor(selected.proto), color: '#fff' }}>
                  {selected.proto}
                </span>
                <span style={{ fontWeight: 600 }}>{selected.label}</span>
                <span className="ts">{selected.from} → {selected.to}</span>
                <span className="ts" style={{ marginLeft: 'auto' }}>{selected.ts}</span>
              </div>
              <pre style={{
                background: '#0e1520',
                border: '1px solid #2a3a50',
                borderRadius: 6,
                padding: 12,
                overflowX: 'auto',
                overflowY: 'auto',
                maxHeight: '60vh',
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
          )}
        </div>
      )}

      <div className="modal-footer">
        <span className="ts" style={{ marginRight: 'auto' }}>
          {messages.length}건 · {date ?? '오늘'}
        </span>
        <button className="btn btn--ghost" onClick={onClose}>닫기</button>
      </div>
    </Modal>
  )
}
