// 자유 2D 그리드 편집기 — 편집 모드에서 각 위젯을 셀 그리드 카드로 렌더하고, 헤더 드래그(이동)와
// 8방향 핸들(리사이즈)을 포인터 이벤트(마우스+터치 통합)로 처리한다. 리사이즈는 귀퉁이(가로·세로 동시),
// 상/하 모서리(세로만), 좌/우 모서리(가로만) — 위/왼쪽에서 잡으면 위치(x/y)도 함께 이동한다.
// 드래그 중엔 placeholder ghost 로 착지 지점을 스냅해 보여주고, pointerup 에 gridLayout(moveItem/applyBox)
// 으로 커밋 → 겹침은 아래로 밀리고 빈칸은 위로 당겨진다(compaction). 배치 상태는 부모(EditableLayout) draft 소유.

import { useEffect, useRef, useState } from 'react'
import type { CSSProperties, PointerEvent as ReactPointerEvent } from 'react'
import WidgetApiBadge from '../components/WidgetApiBadge'
import { getWidget } from './registry'
import type { WidgetConfigField, WidgetDef, WidgetPlacement } from './types'
import {
  GRID_COLS, GRID_ROWS, GRID_GAP, MIN_ROWS,
  gridBox, moveItem, applyBox, removeAt, setConfigAt, setLockedAt,
  clampX, clampY, clampW, clampH, type GridBox, type MinRowsFn,
} from './gridLayout'

// 위젯별 최소 행 — 선언(WidgetDef.minSize.h)이 있으면 그것, 없으면 공통 하한.
// 자리를 뺏길 때 여기까지만 줄어든다(gridLayout.fitBudget).
const minRowsOf: MinRowsFn = p => getWidget(p.widgetId)?.minSize?.h ?? MIN_ROWS

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

// 배치 설정 패널 — `WidgetDef.configFields` 선언대로 그린 폼(소스·지표·제목 등).
// 위젯 본문은 편집 중 pointer-events 가 없으므로(미리보기), 설정은 이 카드 chrome 에서 만진다.
// 카드보다 클 수 있어(작은 지표 카드) 헤더 아래로 띄우는 popover — 여는 동안만 카드 overflow 해제
// (`.grid-widget--cfg`). 안 그러면 한 칸짜리 카드에서 패널이 잘려 안 보인다.
//
// **표시 이름(placement.title) 변경은 두지 않는다** — 관제 화면에서 위젯 이름을 바꾸면 그게 원래
// 무엇인지 가려져 위험하다(identifier_model: 동작은 id 로, 표시는 정의된 이름으로). 옛 저장본에
// 남은 이름은 계속 그려 주되(GridRenderer 캡션) 새로 붙이지는 않는다.
function WidgetConfigPanel({ placement, def, onConfig, onClose }: {
  placement: WidgetPlacement
  def?: WidgetDef
  onConfig: (patch: Record<string, unknown>) => void
  onClose: () => void
}) {
  const fields = def?.configFields ?? []
  const val = (f: WidgetConfigField) => placement.config?.[f.key]
  return (
    <div className="grid-config-panel" onPointerDown={e => e.stopPropagation()}>
      {fields.map(f => {
        const options = typeof f.options === 'function' ? f.options(placement.config) : (f.options ?? [])
        return (
          <label key={f.key} className="grid-config-row">
            <span>{f.label}</span>
            {f.type === 'select' ? (
              <select className="form-input" value={String(val(f) ?? '')}
                      onChange={e => onConfig({ [f.key]: e.target.value || undefined })}>
                <option value="">(기본값)</option>
                {options.map(o => <option key={o.value} value={o.value}>{o.label}</option>)}
              </select>
            ) : f.type === 'bool' ? (
              <input type="checkbox" checked={!!val(f)}
                     onChange={e => onConfig({ [f.key]: e.target.checked || undefined })} />
            ) : (
              <input className="form-input" type={f.type === 'number' ? 'number' : 'text'}
                     value={String(val(f) ?? '')} placeholder={f.placeholder}
                     onChange={e => {
                       const raw = e.target.value
                       if (!raw) return onConfig({ [f.key]: undefined })
                       onConfig({ [f.key]: f.type === 'number' ? Number(raw) : raw })
                     }} />
            )}
          </label>
        )
      })}
      {fields.length === 0 && (
        <div style={{ fontSize: 11, color: 'var(--text-muted)' }}>이 위젯은 배치 설정 항목이 없습니다.</div>
      )}
      <button className="btn btn--sm" onClick={onClose} style={{ alignSelf: 'flex-end' }}>닫기</button>
    </div>
  )
}

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

// 카드 안 배치 편집 상태 — **그 카드 자리에서 그대로** 편집한다(표면을 갈아끼우지 않는다).
// 편집 중인 카드만 본문이 중첩 편집기로 바뀌고, 나머지 카드는 흐려지며 조작이 잠긴다 —
// "지금 이 카드를, 이 영역 안에서 고치고 있다"가 화면으로 보이게 하는 게 목적.
export interface InsideEdit {
  key: number                                  // 편집 중인 카드의 배치 index
  layout: WidgetPlacement[]                    // 그 카드의 내부 배치
  onChange: (ws: WidgetPlacement[]) => void
  onUndo: () => void                           // 마지막 변경 한 수만 취소
  onReset: () => void                          // 이 카드의 기본 배치로 초기화
  onSave: () => void                           // 레이아웃 저장(= 편집 종료)
  onCancel: () => void                         // **이 카드 편집만** 취소(진입 시점으로 되돌림)
  onExit: () => void                           // 카드 편집만 끝내고 화면 편집으로
  canUndo?: boolean
  saving?: boolean
}

export function GridEditor({ widgets, gap = GRID_GAP, preview = false, nested = false,
                             onChange, onEditInside, inside = null }: {
  widgets: WidgetPlacement[]
  gap?: number
  preview?: boolean          // 미리보기 — 제목줄·핸들·점선을 감춰 저장 후 모습을 그대로 본다
  nested?: boolean           // 카드 안 편집기 — 캔버스 min-size 없이 그 카드 박스를 캔버스로 쓴다
  onChange: (widgets: WidgetPlacement[]) => void
  // 카드 안 배치 편집으로 들어가기 — `WidgetDef.cardLayout` 을 가진 카드에만 [⊞] 가 붙는다.
  onEditInside?: (index: number) => void
  inside?: InsideEdit | null
}) {
  const canvasRef = useRef<HTMLDivElement>(null)
  const cellWRef = useRef(0)                    // 측정된 1칸 폭(px)
  const cellHRef = useRef(0)                    // 측정된 1행 높이(px) — 캔버스가 고정 예산이라 실측이 정본
  const dragRef = useRef<DragState | null>(null)
  const cleanupRef = useRef<(() => void) | null>(null)
  const [drag, setDrag] = useState<DragState | null>(null)
  const [cfgOpen, setCfgOpen] = useState<number | null>(null)   // [⚙] 설정 패널이 열린 카드 index

  const measure = () => {
    const el = canvasRef.current
    // gap:0 트랙 — 셀 = 컨테이너 크기 / 칸수 (칸 사이 간격은 카드 margin 이라 트랙 계산에서 제외)
    if (el) {
      cellWRef.current = el.clientWidth / GRID_COLS
      cellHRef.current = el.clientHeight / GRID_ROWS
    }
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
    if (inside) return                          // 카드 안 편집 중 — 바깥 배치는 잠근다(한 번에 한 층)
    e.preventDefault()
    e.stopPropagation()                         // 핸들이 헤더(이동) onPointerDown 을 트리거하지 않게
    measure()                                   // ResizeObserver 미발화 대비 즉시 측정
    const start = gridBox(widgets[key])
    const st: DragState = { kind, key, edges, startPx: e.clientX, startPy: e.clientY, start, ghost: { ...start }, dx: 0, dy: 0 }
    dragRef.current = st
    setDrag(st)

    // gap:0 트랙 — pitch 에 간격 미포함. 행 높이는 캔버스 실측(= 콘텐츠 높이 / GRID_ROWS)이라
    // 창 크기와 무관하게 델타→행 변환이 정확하다.
    const pitchX = cellWRef.current || 1
    const pitchY = cellHRef.current || 1

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
          y: clampY(s.y + Math.round(dy / pitchY), s.h),
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
        if (ed.b) h = clampH(s.h + dRow, s.y)                                  // 하: y 고정, h 증감
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
      // 예산이 모자라면 gridLayout 이 원래 배치를 그대로 돌려준다(조작 거절) — 캔버스는 안 늘어난다.
      onChange(d.kind === 'move'
        ? moveItem(widgets, d.key, d.ghost.x, d.ghost.y, minRowsOf)
        : applyBox(widgets, d.key, d.ghost, minRowsOf))
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
    <div ref={canvasRef}
         className={`${nested ? 'card-canvas card-canvas--edit' : 'grid-canvas grid-canvas--edit'}`
                    + `${drag ? ' grid-canvas--dragging' : ''}`
                    + `${preview ? ' grid-canvas--preview' : ''}`
                    + `${inside ? ' grid-canvas--inside' : ''}`}
         style={{ display: 'grid', gridTemplateColumns: `repeat(${GRID_COLS}, 1fr)`,
                  gridTemplateRows: `repeat(${GRID_ROWS}, 1fr)`, gap: 0, alignItems: 'stretch',
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
        const editingInside = inside?.key === i
        const dimmed = !!inside && !editingInside
        // ⚙ 로 할 수 있는 일이 있는가 — 카드 안 편집 또는 배치 설정 항목.
        const hasCfg = !!def?.cardLayout || (def?.configFields?.length ?? 0) > 0
        return (
          <div key={`${p.widgetId}-${i}`}
               className={`grid-widget${isMove || isResize ? ' grid-widget--dragging' : ''}`
                          + `${cfgOpen === i ? ' grid-widget--cfg' : ''}`
                          + `${p.locked ? ' grid-widget--locked' : ''}`
                          + `${editingInside ? ' grid-widget--inside' : ''}`
                          + `${dimmed ? ' grid-widget--dimmed' : ''}`} style={style}>
            <div className="grid-drag-handle"
                 onPointerDown={e => { if (!p.locked && !inside) beginDrag('move', i, e) }}
                 title={inside ? '카드 안 편집 중 — 바깥 배치는 잠김'
                       : p.locked ? '잠김 — 이동하려면 자물쇠를 푸세요' : '드래그로 이동'}>
              <b className="grid-widget-title">{p.title || def?.title || p.widgetId}</b>
              <span className="grid-widget-id">({p.widgetId})</span>
              {p.visibleWhen && (
                <span className="grid-size-badge" style={{ color: 'var(--primary)' }}
                      title={`${p.visibleWhen.param}=${p.visibleWhen.equals} 일 때만 표시(탭)`}>
                  {p.visibleWhen.param}={p.visibleWhen.equals}
                </span>
              )}
              <span className="grid-size-badge" title="가로 % × 세로 % (세로는 화면 한 장 기준)">
                {Math.round(view.w / GRID_COLS * 100)}%×{Math.round(view.h / GRID_ROWS * 100)}%
              </span>
              {/* 사용 API — 개발자 모드에서만. 배치하면서 이 위젯이 뭘 부르는지 바로 확인. */}
              <WidgetApiBadge ids={def?.apis} sourceIds={def?.apiSources?.(p.config)} title={def?.title ?? p.widgetId} />
              {editingInside && inside && (
                <span style={{ marginLeft: 'auto', display: 'flex', gap: 4, position: 'relative', zIndex: 7 }}
                      onPointerDown={e => e.stopPropagation()}>
                  <button className="btn btn--sm" title="마지막 변경 한 수만 취소"
                          onClick={inside.onUndo} disabled={inside.saving || !inside.canUndo}>↶ 되돌리기</button>
                  <button className="btn btn--sm" title="이 카드의 기본 배치로 초기화"
                          onClick={inside.onReset} disabled={inside.saving}>초기화</button>
                  <button className="btn btn--sm btn--primary" title="레이아웃을 저장하고 편집을 끝낸다"
                          onClick={inside.onSave} disabled={inside.saving}>저장</button>
                  <button className="btn btn--sm" title="이 카드에서 한 편집만 버리고 화면 배치로 돌아간다"
                          onClick={inside.onCancel} disabled={inside.saving}>취소</button>
                  <button className="btn btn--sm" title="카드 편집을 끝내고 화면 배치로 돌아간다"
                          onClick={inside.onExit} disabled={inside.saving}>완료</button>
                </span>
              )}
              {/* 잠금 — 캔버스가 고정 예산이라 다른 위젯을 키우면 누군가는 줄어든다.
                  잠긴 카드는 그 대상에서 빠지고 자리도 고정된다. */}
              {!editingInside && <>
              <button className="btn btn--sm"
                      title={p.locked ? '잠금 해제 — 다른 위젯을 키울 때 이 카드가 줄어들 수 있음'
                                      : '잠금 — 위치·크기 고정(다른 위젯을 키워도 안 줄어듦)'}
                      style={{ ...(onEditInside && def?.cardLayout ? {} : { marginLeft: 'auto' }),
                               position: 'relative', zIndex: 7,
                               color: p.locked ? 'var(--primary)' : undefined }}
                      onPointerDown={e => e.stopPropagation()}
                      onClick={() => onChange(setLockedAt(widgets, i, !p.locked))}>{p.locked ? '🔒' : '🔓'}</button>
              {/* 카드(여러 블록을 담은 것)면 **바로 카드 안 편집으로 들어간다** — 중간에 패널을 한 번
                  더 거치게 하지 않는다. 그 밖의 위젯은 배치 설정 패널을 연다.
                  할 수 있는 일이 없으면 비활성 — "의미 없는 톱니바퀴"를 남기지 않는다.
                  (카드 위젯은 configFields 를 선언하지 않는다 — 선언하면 이 경로로 가려진다.) */}
              <button className="btn btn--sm"
                      title={def?.cardLayout ? '카드 안 블록 배치 편집'
                             : hasCfg ? '이 배치의 설정' : '설정 항목 없음'}
                      disabled={!hasCfg || dimmed}
                      style={{ position: 'relative', zIndex: 7 }}
                      onPointerDown={e => e.stopPropagation()}
                      onClick={() => {
                        if (def?.cardLayout && onEditInside) { setCfgOpen(null); onEditInside(i); return }
                        setCfgOpen(o => (o === i ? null : i))
                      }}>⚙</button>
              <button className="btn btn--sm" title="제거"
                      style={{ color: 'var(--danger)', position: 'relative', zIndex: 7 }}
                      onPointerDown={e => e.stopPropagation()}
                      onClick={() => { setCfgOpen(null); onChange(removeAt(widgets, i)) }}>✕</button>
              </>}
            </div>
            {cfgOpen === i && (
              <WidgetConfigPanel placement={p} def={def}
                onConfig={patch => onChange(setConfigAt(widgets, i, patch))}
                onClose={() => setCfgOpen(null)} />
            )}
            {/* 편집 중인 카드는 본문이 **그 자리에서** 중첩 편집기가 된다 — 카드 박스가 곧 편집
                가능한 영역이라 "이만큼 안에서 고치는 중"이 눈에 보인다.
                컨트롤 위젯은 편집 중에도 조작 가능(탭을 바꿔가며 그 탭의 배치를 편집).
                (나머지 위젯 본문은 미리보기라 pointer-events 없음) */}
            <div className={`grid-widget-body${def?.category === 'control' ? ' grid-widget-body--live' : ''}`}>
              {editingInside && inside ? (
                <GridEditor nested widgets={inside.layout} gap={gap} onChange={inside.onChange} />
              ) : Comp ? <Comp config={p.config} />
                    : <div style={{ color: 'var(--danger)', fontSize: 12, padding: 8 }}>알 수 없는 위젯: {p.widgetId}</div>}
            </div>
            {!p.locked && !inside && RESIZE_HANDLES.map(rh => (
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
          {nested ? '블록이 없습니다' : '위젯이 없습니다'} — 상단 [+ 위젯 추가]로 배치하세요.
        </div>
      )}
    </div>
  )
}
