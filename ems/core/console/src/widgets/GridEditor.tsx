// 자유 2D 그리드 편집기 — 편집 모드에서 각 위젯을 셀 그리드 카드로 렌더하고, 드래그(이동)와
// 우하단 핸들(리사이즈)을 포인터 이벤트(마우스+터치 통합)로 처리한다. 드래그 중엔 placeholder ghost 로
// 착지 지점만 스냅해 보여주고, pointerup 에 gridLayout(moveItem/resizeItem) 으로 커밋 → 겹침은
// 아래로 밀리고 빈칸은 위로 당겨진다(compaction). 배치 상태는 부모(EditableLayout)의 draft 가 소유.

import { useEffect, useRef, useState } from 'react'
import type { CSSProperties, PointerEvent as ReactPointerEvent } from 'react'
import { getWidget } from './registry'
import type { WidgetPlacement } from './types'
import {
  GRID_COLS, GRID_GAP, ROW_H_VH,
  gridBox, moveItem, resizeItem, removeAt,
  clampX, clampY, clampW, clampH, type GridBox,
} from './gridLayout'

interface DragState {
  kind: 'move' | 'resize'
  key: number
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

  function beginDrag(kind: 'move' | 'resize', key: number, e: ReactPointerEvent) {
    if (e.button !== 0) return                  // 주버튼/터치만
    e.preventDefault()
    measure()                                   // ResizeObserver 미발화 대비 즉시 측정
    const start = gridBox(widgets[key])
    const st: DragState = { kind, key, startPx: e.clientX, startPy: e.clientY, start, ghost: { ...start }, dx: 0, dy: 0 }
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
      let ghost: GridBox
      if (d.kind === 'move') {
        ghost = {
          x: clampX(d.start.x + Math.round(dx / pitchX), d.start.w),
          y: clampY(d.start.y + Math.round(dy / pitchY)),
          w: d.start.w, h: d.start.h,
        }
      } else {
        ghost = {
          x: d.start.x, y: d.start.y,
          w: clampW(d.start.w + Math.round(dx / pitchX), d.start.x),
          h: clampH(d.start.h + Math.round(dy / pitchY)),
        }
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
        : resizeItem(widgets, d.key, d.ghost.w, d.ghost.h))
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
        const span = isResize && drag ? drag.ghost : b
        const style: CSSProperties = {
          gridColumn: `${b.x + 1} / span ${span.w}`,
          gridRow: `${b.y + 1} / span ${span.h}`,
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
                {Math.round(span.w / GRID_COLS * 100)}%×{Math.round(span.h * ROW_H_VH)}%
              </span>
              <button className="btn btn--sm" title="제거" style={{ color: 'var(--danger)', marginLeft: 'auto' }}
                      onPointerDown={e => e.stopPropagation()} onClick={() => onChange(removeAt(widgets, i))}>✕</button>
            </div>
            <div className="grid-widget-body">
              {Comp ? <Comp config={p.config} />
                    : <div style={{ color: 'var(--danger)', fontSize: 12, padding: 8 }}>알 수 없는 위젯: {p.widgetId}</div>}
            </div>
            <div className="grid-resize-handle" onPointerDown={e => beginDrag('resize', i, e)} title="크기 조절" />
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
