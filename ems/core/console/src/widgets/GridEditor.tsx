// 자유 2D 그리드 편집기 — 편집 모드에서 각 위젯을 셀 그리드 카드로 렌더하고, 헤더 드래그(이동)와
// 8방향 핸들(리사이즈)을 포인터 이벤트(마우스+터치 통합)로 처리한다. 리사이즈는 귀퉁이(가로·세로 동시),
// 상/하 모서리(세로만), 좌/우 모서리(가로만) — 위/왼쪽에서 잡으면 위치(x/y)도 함께 이동한다.
// 드래그 중엔 placeholder ghost 로 착지 지점을 스냅해 보여주고, pointerup 에 gridLayout(moveItem/applyBox)
// 으로 커밋 → 겹침은 아래로 밀리고 빈칸은 위로 당겨진다(compaction). 배치 상태는 부모(EditableLayout) draft 소유.

import { useEffect, useRef, useState } from 'react'
import type { CSSProperties, PointerEvent as ReactPointerEvent } from 'react'
import { getWidget } from './registry'
import type { WidgetPlacement } from './types'
import {
  GRID_COLS, GRID_GAP, ROW_H_VH,
  gridBox, moveItem, applyBox, removeAt,
  clampX, clampY, clampW, clampH, type GridBox,
} from './gridLayout'

// 리사이즈 방향 — 어느 모서리(edge)를 잡아 끄는가. l/r=좌/우, t/b=상/하.
interface Edges { l?: boolean; r?: boolean; t?: boolean; b?: boolean }

// 8개 핸들 정의(모서리 4 + 귀퉁이 4). cls 로 위치/커서 스타일(index.css) 지정.
const RESIZE_HANDLES: { dir: string; edges: Edges; cls: string }[] = [
  { dir: 'n',  edges: { t: true },          cls: 'grid-rz grid-rz-n' },
  { dir: 's',  edges: { b: true },          cls: 'grid-rz grid-rz-s' },
  { dir: 'e',  edges: { r: true },          cls: 'grid-rz grid-rz-e' },
  { dir: 'w',  edges: { l: true },          cls: 'grid-rz grid-rz-w' },
  { dir: 'ne', edges: { t: true, r: true }, cls: 'grid-rz grid-rz-ne' },
  { dir: 'nw', edges: { t: true, l: true }, cls: 'grid-rz grid-rz-nw' },
  { dir: 'se', edges: { b: true, r: true }, cls: 'grid-rz grid-rz-se' },
  { dir: 'sw', edges: { b: true, l: true }, cls: 'grid-rz grid-rz-sw' },
]

interface DragState {
  kind: 'move' | 'resize'
  key: number
  edges?: Edges         // resize 시 잡은 모서리
  startPx: number
  startPy: number
  start: GridBox        // 드래그 시작 시점의 박스
  ghost: GridBox        // 현재 스냅된 착지 박스
  dx: number            // 원시 픽셀 델타 (이동 카드 커서 추종용)
  dy: number
}

export function GridEditor({ widgets, gap = GRID_GAP, onChange }: {
  widgets: WidgetPlacement[]
  gap?: number
  onChange: (widgets: WidgetPlacement[]) => void
}) {
  const canvasRef = useRef<HTMLDivElement>(null)
  const cellWRef = useRef(0)                    // 측정된 1칸 폭(px)
  const dragRef = useRef<DragState | null>(null)
  const cleanupRef = useRef<(() => void) | null>(null)
  const [drag, setDrag] = useState<DragState | null>(null)

  const measure = () => {
    const el = canvasRef.current
    // gap:0 트랙 — 셀 폭 = 컨테이너 폭 / 칸수 (칸 사이 간격은 카드 margin 이라 트랙 계산에서 제외)
    if (el) cellWRef.current = el.clientWidth / GRID_COLS
  }
  useEffect(() => {
    measure()
    const el = canvasRef.current
    if (!el || typeof ResizeObserver === 'undefined') return
    const ro = new ResizeObserver(measure)
    ro.observe(el)
    return () => ro.disconnect()
  }, [])
  // 언마운트 중 드래그면 window 리스너 정리.
  useEffect(() => () => { cleanupRef.current?.() }, [])

  function beginDrag(kind: 'move' | 'resize', key: number, e: ReactPointerEvent, edges?: Edges) {
    if (e.button !== 0) return                  // 주버튼/터치만
    e.preventDefault()
    e.stopPropagation()                         // 핸들이 헤더(이동) onPointerDown 을 트리거하지 않게
    measure()                                   // ResizeObserver 미발화 대비 즉시 측정
    const start = gridBox(widgets[key])
    const st: DragState = { kind, key, edges, startPx: e.clientX, startPy: e.clientY, start, ghost: { ...start }, dx: 0, dy: 0 }
    dragRef.current = st
    setDrag(st)

    // gap:0 트랙 — pitch 에 간격 미포함. pitchX=셀 폭, pitchY=행 높이(px).
    const pitchX = cellWRef.current || 1
    // 행 높이(px) = 뷰포트 세로 × ROW_H_VH% — grid-auto-rows 의 vh 와 동일 기준이라 델타→행 변환이 정확.
    const rowHpx = (typeof window !== 'undefined' ? window.innerHeight : 800) * ROW_H_VH / 100
    const pitchY = rowHpx

    const onMove = (ev: PointerEvent) => {
      const d = dragRef.current
      if (!d) return
      const dx = ev.clientX - d.startPx
      const dy = ev.clientY - d.startPy
      const s = d.start
      let ghost: GridBox
      if (d.kind === 'move') {
        ghost = {
          x: clampX(s.x + Math.round(dx / pitchX), s.w),
          y: clampY(s.y + Math.round(dy / pitchY)),
          w: s.w, h: s.h,
        }
      } else {
        // 리사이즈 — 잡은 모서리만 이동. 반대 모서리는 고정.
        const ed = d.edges ?? {}
        const dCol = Math.round(dx / pitchX)
        const dRow = Math.round(dy / pitchY)
        const right0 = s.x + s.w
        const bottom0 = s.y + s.h
        let x = s.x, y = s.y, w = s.w, h = s.h
        if (ed.r) w = clampW(s.w + dCol, s.x)                                  // 우: x 고정, w 증감
        if (ed.l) { x = Math.max(0, Math.min(right0 - 1, s.x + dCol)); w = right0 - x }  // 좌: 우변 고정
        if (ed.b) h = clampH(s.h + dRow)                                       // 하: y 고정, h 증감
        if (ed.t) { y = Math.max(0, Math.min(bottom0 - 1, s.y + dRow)); h = bottom0 - y } // 상: 하변 고정
        ghost = { x, y, w, h }
      }
      const next: DragState = { ...d, dx, dy, ghost }
      dragRef.current = next
      setDrag(next)
    }
    const finish = () => {
      const d = dragRef.current
      cleanup()
      dragRef.current = null
      setDrag(null)
      if (!d) return
      onChange(d.kind === 'move'
        ? moveItem(widgets, d.key, d.ghost.x, d.ghost.y)
        : applyBox(widgets, d.key, d.ghost))
    }
    const cancel = () => { cleanup(); dragRef.current = null; setDrag(null) }
    const onKey = (ev: KeyboardEvent) => { if (ev.key === 'Escape') cancel() }
    const cleanup = () => {
      window.removeEventListener('pointermove', onMove)
      window.removeEventListener('pointerup', finish)
      window.removeEventListener('pointercancel', cancel)
      window.removeEventListener('keydown', onKey)
      cleanupRef.current = null
    }
    cleanupRef.current = cleanup
    window.addEventListener('pointermove', onMove)
    window.addEventListener('pointerup', finish)
    window.addEventListener('pointercancel', cancel)
    window.addEventListener('keydown', onKey)
  }

  const activeMove = drag?.kind === 'move' ? drag.key : -1
  const activeResize = drag?.kind === 'resize' ? drag.key : -1

  return (
    <div ref={canvasRef} className={`grid-canvas grid-canvas--edit${drag ? ' grid-canvas--dragging' : ''}`}
         style={{ display: 'grid', gridTemplateColumns: `repeat(${GRID_COLS}, 1fr)`,
                  gridAutoRows: `${ROW_H_VH}vh`, gap: 0, alignItems: 'stretch',
                  ['--card-gap']: `${gap}px` } as CSSProperties}>
      {widgets.map((p, i) => {
        const def = getWidget(p.widgetId)
        const Comp = def?.component
        const b = gridBox(p)
        const isMove = i === activeMove
        const isResize = i === activeResize
        // 리사이즈 중엔 위치·크기 모두 ghost 로(위/왼쪽 리사이즈는 위치도 이동). 이동 중엔 시작 위치 + translate.
        const view = isResize && drag ? drag.ghost : b
        const style: CSSProperties = {
          gridColumn: `${view.x + 1} / span ${view.w}`,
          gridRow: `${view.y + 1} / span ${view.h}`,
          minWidth: 0,
        }
        if (isMove && drag) { style.transform = `translate(${drag.dx}px, ${drag.dy}px)`; style.zIndex = 20 }
        if (isResize) style.zIndex = 20
        return (
          <div key={`${p.widgetId}-${i}`}
               className={`grid-widget${isMove || isResize ? ' grid-widget--dragging' : ''}`} style={style}>
            <div className="grid-drag-handle" onPointerDown={e => beginDrag('move', i, e)} title="드래그로 이동">
              <b style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{def?.title ?? p.widgetId}</b>
              <span style={{ color: 'var(--text-muted)', fontSize: 11 }}>({p.widgetId})</span>
              <span className="grid-size-badge" title="가로 % × 세로 %">
                {Math.round(view.w / GRID_COLS * 100)}%×{Math.round(view.h * ROW_H_VH)}%
              </span>
              <button className="btn btn--sm" title="제거"
                      style={{ color: 'var(--danger)', marginLeft: 'auto', position: 'relative', zIndex: 7 }}
                      onPointerDown={e => e.stopPropagation()} onClick={() => onChange(removeAt(widgets, i))}>✕</button>
            </div>
            <div className="grid-widget-body">
              {Comp ? <Comp config={p.config} />
                    : <div style={{ color: 'var(--danger)', fontSize: 12, padding: 8 }}>알 수 없는 위젯: {p.widgetId}</div>}
            </div>
            {RESIZE_HANDLES.map(rh => (
              <div key={rh.dir} className={rh.cls} title="크기 조절"
                   onPointerDown={e => beginDrag('resize', i, e, rh.edges)} />
            ))}
          </div>
        )
      })}

      {drag && (
        <div className="grid-placeholder" aria-hidden
             style={{ gridColumn: `${drag.ghost.x + 1} / span ${drag.ghost.w}`,
                      gridRow: `${drag.ghost.y + 1} / span ${drag.ghost.h}` }} />
      )}

      {widgets.length === 0 && (
        <div style={{ gridColumn: '1 / span 12', color: 'var(--text-muted)', fontSize: 13, padding: 20, textAlign: 'center' }}>
          위젯이 없습니다 — 상단 [+ 위젯 추가]로 배치하세요.
        </div>
      )}
    </div>
  )
}
