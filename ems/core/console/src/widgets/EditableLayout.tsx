// 편집 가능한 위젯 레이아웃 — 저장본 로드 → admin 이 [✎ 편집] 으로 위젯 추가/제거 및 드래그·리사이즈로
// 2D 배치 후 저장. view 모드: GridRenderer. edit 모드: GridEditor(포인터 드래그/리사이즈 + compaction).
// 편집 진입 시 legacy(flow) 레이아웃은 grid 로 1회 migrate(flowToGrid). 좁은 화면에선 편집 비활성.
// 영속: OAM /console/layouts/<id> (PUT 저장 / DELETE seed 리셋). 없으면 seed.

import { useState, useEffect, useRef } from 'react'
import { createPortal } from 'react-dom'
import { useAuth } from '../contexts/AuthContext'
import { hasRole } from '../utils/permissions'
import { useToast } from '../components/Toast'
import { useIsDesktop } from '../hooks/useIsDesktop'
import { consoleApi } from '../api/console'
import { GridRenderer } from './GridRenderer'
import { GridEditor, type InsideEdit } from './GridEditor'
import { getWidget, mergeFor, splitFor, widgetsByCategory } from './registry'
import { flowToGrid, addToFirstFree, isGridLayout, usedRows, fitToBudget, setConfigAt,
         GRID_ROWS, GRID_GAP, MIN_ROWS, NOMINAL_ROW_VH } from './gridLayout'
import { PageParamsProvider, isPlacementVisible, usePageParams } from './pageParams'
import { collapseMerges, expandSplits } from './legacyLayout'
import { resolveCardLayout } from './CardLayout'
import type { PageLayout, WidgetPlacement } from './types'

// 편집 표면 — **화면에 보이는 것과 같은 배치만** 편집한다. 탭으로 갈아끼우는 위젯들은 같은 자리를
// 쓰므로, 다른 탭의 배치를 함께 늘어놓으면 편집 화면이 실제와 달라진다. 숨은 배치는 좌표를 그대로
// 보존한 채 뒤에 다시 붙인다(grid 배치는 절대 좌표라 배열 순서는 의미 없음).
// 컨트롤 위젯은 편집 중에도 눌리므로(GridEditor) 탭을 바꿔가며 각 탭의 배치를 편집할 수 있다.
function EditSurface({ widgets, gap, addId, preview, onAdded, onFull, onChange, onEditInside, inside }: {
  widgets: WidgetPlacement[]
  gap: number
  addId: string
  preview: boolean
  onAdded: () => void
  onFull: () => void          // 세로 예산이 없어 추가를 거절했음 — 부모가 알린다
  onChange: (ws: WidgetPlacement[]) => void
  onEditInside?: (index: number) => void
  inside?: InsideEdit | null               // 카드 안 편집 중이면 그 카드 자리에서 중첩 편집기가 뜬다
}) {
  const params = usePageParams()
  const visible = widgets.filter(p => isPlacementVisible(p, params))
  const hidden = widgets.filter(p => !isPlacementVisible(p, params))

  // 위젯 추가도 "보이는 배치" 기준으로 자리를 잡는다 — 숨은 탭 아래로 밀려나지 않게.
  useEffect(() => {
    if (!addId) return
    const def = getWidget(addId)
    // 예산이 없으면 addToFirstFree 가 원본을 그대로 돌려준다(추가 거절) — 캔버스는 안 늘어난다.
    const next = addToFirstFree(visible, { widgetId: addId }, def?.defaultSize?.w, def?.defaultSize?.h,
                                p => getWidget(p.widgetId)?.minSize?.h ?? MIN_ROWS)
    if (next.length === visible.length) onFull()
    else onChange([...next, ...hidden])
    onAdded()
    // addId 가 설정된 순간에만 동작 — visible/hidden 은 그 시점 값을 쓴다.
  }, [addId])   // eslint-disable-line react-hooks/exhaustive-deps

  return (
    <GridEditor widgets={visible} gap={gap} preview={preview}
                onEditInside={onEditInside && (i => onEditInside(widgets.indexOf(visible[i])))}
                inside={inside ? { ...inside, key: visible.indexOf(widgets[inside.key]) } : null}
                onChange={ws => onChange([...ws, ...hidden])} />
  )
}

function clone(l: PageLayout): PageLayout {
  return JSON.parse(JSON.stringify(l))
}

// 레이아웃을 화면에 올리기 전 정규화 —
//  ① 폐지된 위젯 id 를 지금 쓰는 것으로 갈아끼운다(legacyLayout: 묶음은 부품으로 펼치고, 낱개로
//     흩어졌던 것은 대체 위젯 하나로 접는다).
//  ② **세로 예산(GRID_ROWS)을 넘는 옛 배치를 줄여 맞춘다** — 세로가 무한이던 시절의 저장본은
//     그대로 두면 캔버스 밖으로 흘러나간다(§3.0). 잠긴 위젯은 건드리지 않는다.
// 저장본을 다시 쓰진 않는다(표시만) — 운영자가 편집·저장하면 그때 굳는다.
function normalize(l: PageLayout): PageLayout {
  const merged = collapseMerges(expandSplits(l.widgets, splitFor), mergeFor)
  const widgets = isGridLayout(merged)
    ? fitToBudget(merged, p => getWidget(p.widgetId)?.minSize?.h ?? MIN_ROWS)
    : merged
  return widgets === l.widgets ? l : { ...l, widgets }
}

// 마지막으로 본 레이아웃을 localStorage 에 캐시 — 다음 진입 시 저장본을 즉시 렌더(seed flash·서버
// 조회 지연 제거), 백그라운드로 서버에서 갱신. (저장 레이아웃 GET 이 느린 환경에서도 바로 표시.)
const _cacheKey = (id: string) => `cims_layout_${id}`
function _readCache(id: string): PageLayout | null {
  try {
    const s = localStorage.getItem(_cacheKey(id))
    if (s) { const o = JSON.parse(s); if (o && Array.isArray(o.widgets)) return o }
  } catch { /* ignore */ }
  return null
}
function _writeCache(id: string, l: PageLayout) {
  try { localStorage.setItem(_cacheKey(id), JSON.stringify(l)) } catch { /* ignore */ }
}

// seed 세대 안내를 "이 배치 유지"로 닫은 기록 — 같은 세대에선 다시 띄우지 않는다(계정/브라우저 로컬).
const _dismissKey = (id: string) => `cims_seedver_dismissed_${id}`
function _readDismissed(id: string): number {
  try { return parseInt(localStorage.getItem(_dismissKey(id)) || '0', 10) || 0 } catch { return 0 }
}

export function EditableLayout({ layoutId, seed }: { layoutId: string; seed: PageLayout }) {
  const { user } = useAuth()
  const { show } = useToast()
  const isAdmin = hasRole(user, 'admin')   // developer(admin 동급) 포함
  const isDesktop = useIsDesktop()          // 편집(드래그/리사이즈)은 데스크톱 전용 — 뷰는 좁은 화면도 단일열 동작
  const [layout, setLayout] = useState<PageLayout>(() => normalize(_readCache(layoutId) || seed))
  const [draft, setDraft] = useState<PageLayout | null>(null)
  // 되돌리기 — 편집 중 배치 변경 직전 스냅샷을 쌓는다(초기화와 다르다: 마지막 한 수만 취소).
  // 화면 배치든 카드 안 배치든 모두 draft 를 거치므로 스택 하나로 두 층이 함께 덮인다.
  const [undoStack, setUndoStack] = useState<PageLayout[]>([])
  const [addId, setAddId] = useState('')
  const [addQuery, setAddQuery] = useState('')     // 위젯 검색 — 분해로 위젯 수가 늘어 목록 스캔이 힘들다
  const [saving, setSaving] = useState(false)
  const [dismissedSeedVer, setDismissedSeedVer] = useState(() => _readDismissed(layoutId))
  // 뷰 렌더 DOM — 편집 진입 시 "지금 보이는 높이"를 재려면 필요하다(높이 미지정 배치의 초기값).
  const hostRef = useRef<HTMLDivElement>(null)
  // 편집 컨트롤은 글로벌 헤더의 슬롯(#layout-edit-slot)으로 portal — 콘텐츠/위젯 컨트롤과의 겹침 원천 차단.
  const [editSlot, setEditSlot] = useState<HTMLElement | null>(null)
  useEffect(() => { setEditSlot(document.getElementById('layout-edit-slot')) }, [])

  useEffect(() => {
    let alive = true
    consoleApi.getLayout(layoutId)
      .then(l => { if (alive && l && Array.isArray(l.widgets)) { setLayout(normalize(l)); _writeCache(layoutId, l) } })
      .catch(() => { /* 404/오류 → 캐시/seed 유지 */ })
    return () => { alive = false }
  }, [layoutId])

  // 배치를 바꾸는 유일한 통로 — 바꾸기 직전 상태를 undo 스택에 쌓는다(최근 UNDO_MAX 개).
  const UNDO_MAX = 50
  const applyDraft = (next: (d: PageLayout) => PageLayout) => {
    setDraft(d => {
      if (!d) return d
      const n = next(d)
      if (n === d) return d
      setUndoStack(st => [...st.slice(-(UNDO_MAX - 1)), d])
      return n
    })
  }
  const undo = () => {
    setUndoStack(st => {
      if (st.length === 0) return st
      setDraft(st[st.length - 1])
      return st.slice(0, -1)
    })
  }

  const editing = draft !== null
  // 편집 진입 — legacy(flow) 면 grid 로 migrate. vh→행 변환은 라이브 뷰포트 기준.
  // 높이가 지정되지 않은 배치(통짜 페이지 등)는 **지금 화면에 그려진 높이**를 재서 초기값으로 쓴다 —
  // 상수를 박으면 편집 카드가 실제 화면과 전혀 다른 크기로 잡힌다.
  const beginEdit = () => {
    const base = clone(layout)
    // 행 높이(px) = 지금 그려진 캔버스 높이 / 행 예산. 캔버스가 콘텐츠 영역을 꽉 채우므로
    // 이 값이 곧 한 행의 실제 크기다(vh 상수를 쓰면 창 크기에 따라 어긋난다).
    const canvasEl = hostRef.current?.querySelector('.grid-canvas') as HTMLElement | null
    const canvasH = canvasEl?.clientHeight || (typeof window !== 'undefined' ? window.innerHeight * 0.9 : 900)
    const rowPx = Math.max(1, canvasH / GRID_ROWS)
    const gapPx = base.gap ?? GRID_GAP     // grid 카드는 위아래 margin(gap)을 쓰므로 그만큼 더해서 환산
    const rendered = hostRef.current
      ? Array.from(hostRef.current.querySelectorAll(':scope > * > .widget-api-host'))
      : []
    const measureRows = (i: number) => {
      const el = rendered[i]
      if (!el) return undefined
      const h = el.getBoundingClientRect().height
      return h > 0 ? Math.max(1, Math.round((h + gapPx) / rowPx)) : undefined
    }
    const widgets = isGridLayout(base.widgets)
      ? base.widgets
      : flowToGrid(base.widgets,
          id => getWidget(id)?.defaultSize?.w,
          vh => Math.round(vh / NOMINAL_ROW_VH),   // legacy vh → 행 (명목 환산)
          undefined, measureRows)
    setDraft({ ...base, widgets })
    setUndoStack([])
  }
  const cancelEdit = () => {
    setDraft(null); setAddId(''); setAddQuery(''); setPreview(false)
    setInsideKey(null); setInsideBase(undefined); setUndoStack([])
  }

  // 실제 배치는 EditSurface 가 한다 — 현재 탭에서 보이는 배치 기준으로 자리를 잡아야 하므로.
  const [pendingAdd, setPendingAdd] = useState('')
  const [preview, setPreview] = useState(false)   // 저장 후 모습 확인 — 제목줄·핸들을 감춘다
  // 카드 안 배치 편집 — 그 카드는 자기 자리에 그대로 두고 본문만 중첩 편집기가 된다.
  // 좌표계가 바깥과 같은 48×48 이라 같은 GridEditor 를 그대로 겨눈다.
  const [insideKey, setInsideKey] = useState<number | null>(null)
  // 카드 편집 **진입 시점**의 그 카드 배치 — 카드 안 [취소] 는 여기로만 되돌린다
  // (화면 편집 전체를 버리지 않는다. 전역 [취소]와 범위가 다르다).
  const [insideBase, setInsideBase] = useState<WidgetPlacement[] | undefined>(undefined)
  const addWidget = () => { if (addId) { setPendingAdd(addId); setAddId('') } }



  const saveLayout = async () => {
    if (!draft) return
    setSaving(true)
    try {
      // 저장본에 "어느 seed 세대를 기준으로 만든 배치인가"를 각인 — 이후 seed 가 개편되면
      // (seedVersion 상승) 이 값과 비교해 안내를 띄운다.
      const body = { ...draft, seedVersion: seed.seedVersion }
      const saved = await consoleApi.putLayout(layoutId, body)
      // seedVersion 은 서버 응답에 없을 수 있다(구 OAM 은 top-level 화이트리스트에서 탈락) →
      // 방금 저장한 값으로 덮어 각인 유지. 없으면 저장 직후에 안내 배너가 다시 뜬다.
      const next = saved && Array.isArray(saved.widgets)
        ? { ...saved, seedVersion: seed.seedVersion } : body
      setLayout(next); _writeCache(layoutId, next)
      setDraft(null); setAddId(''); setAddQuery(''); setPreview(false)
      setInsideKey(null); setInsideBase(undefined); setUndoStack([])
      show('레이아웃 저장됨', 'ok')
    } catch (e) { show((e as Error).message, 'err') }
    finally { setSaving(false) }
  }
  const resetLayout = async () => {
    if (!confirm('저장된 레이아웃을 삭제하고 기본값(seed)으로 되돌릴까요?')) return
    setSaving(true)
    try {
      await consoleApi.deleteLayout(layoutId)
      setLayout(normalize(seed)); _writeCache(layoutId, seed); setDraft(null); setAddId(''); setAddQuery('')
      try { localStorage.removeItem(_dismissKey(layoutId)) } catch { /* ignore */ }
      setDismissedSeedVer(0)
      show('기본 레이아웃으로 초기화됨', 'ok')
    } catch (e) { show((e as Error).message, 'err') }
    finally { setSaving(false) }
  }

  // ── 카드 안 편집 ─────────────────────────────────────────────────────────
  // 표면을 그 카드의 내부 배치로 갈아끼운다 — 좌표계가 바깥과 같은 48×48 이라 같은 GridEditor·
  // 같은 예산/잠금 규칙이 그대로 적용된다(중첩 드래그를 만들지 않는 이유이기도 하다).
  const insideAt = insideKey !== null ? draft?.widgets[insideKey] : undefined
  const insideDef = insideAt ? getWidget(insideAt.widgetId) : undefined
  const insideLayout = insideDef?.cardLayout
    ? resolveCardLayout(insideDef.cardLayout, insideAt?.config)
    : null
  const insideLayoutRef = useRef<WidgetPlacement[] | null>(null)
  insideLayoutRef.current = insideLayout
  const setInsideLayout = (ws: WidgetPlacement[]) => {
    if (insideKey === null) return
    applyDraft(d => ({ ...d, widgets: setConfigAt(d.widgets, insideKey, { layout: ws }) }))
  }
  // 카드 안 기본 배치로 되돌리기 — 저장본(config.layout)만 지운다.
  const resetInside = () => {
    if (insideKey === null) return
    applyDraft(d => ({ ...d, widgets: setConfigAt(d.widgets, insideKey, { layout: undefined }) }))
  }
  // 카드 편집 진입 — 되돌릴 기준점(그 시점의 config.layout)을 붙잡아 둔다.
  const enterInside = (i: number) => {
    const saved = draft?.widgets[i]?.config?.layout
    setInsideBase(Array.isArray(saved) ? (saved as WidgetPlacement[]) : undefined)
    setInsideKey(i)
  }
  const exitInside = () => { setInsideKey(null); setInsideBase(undefined) }
  // 카드 안 [취소] — **이 카드 편집만** 진입 시점으로 되돌리고 화면 편집으로 나간다.
  // 전역 [취소](편집 전체 버리기)와 구분된다.
  const cancelInside = () => {
    if (insideKey === null) return
    applyDraft(d => ({ ...d, widgets: setConfigAt(d.widgets, insideKey, { layout: insideBase }) }))
    exitInside()
  }

  // 카드 안 편집 중의 '+ 위젯 추가' 는 **그 카드 안**으로 들어간다(바깥 배치는 잠겨 있으므로).
  useEffect(() => {
    if (!pendingAdd || insideKey === null || !insideLayoutRef.current) return
    const def = getWidget(pendingAdd)
    const next = addToFirstFree(insideLayoutRef.current, { widgetId: pendingAdd },
                                def?.defaultSize?.w, def?.defaultSize?.h,
                                p => getWidget(p.widgetId)?.minSize?.h ?? MIN_ROWS)
    if (next.length === insideLayoutRef.current.length) {
      show(`카드 세로 예산(${GRID_ROWS}행)이 꽉 찼습니다 — 블록을 줄이거나 제거하세요`, 'err')
    } else {
      setInsideLayout(next)
    }
    setPendingAdd('')
    // pendingAdd 가 설정된 순간에만 동작.
  }, [pendingAdd])   // eslint-disable-line react-hooks/exhaustive-deps

  // 남은 세로 예산 — 편집 툴바 표시용. 카드 안 편집 중이면 그 카드의 예산을 센다.
  const budgetOf = insideLayout ?? draft?.widgets
  const freeRows = budgetOf ? Math.max(0, GRID_ROWS - usedRows(budgetOf)) : GRID_ROWS

  const editControls = (
    <div className="layout-edit-headerbar">
      {!editing ? (
        <button className="btn btn--sm layout-edit-fab" onClick={beginEdit}
                title="이 페이지를 위젯으로 편집">✎ 편집</button>
      ) : (
        <>
          <span className="layout-edit-hint">
            {insideLayout
              ? `카드 안 편집: ${insideAt?.title || insideDef?.title || insideAt?.widgetId}`
              : '편집 중'}
          </span>
          <input className="form-input" value={addQuery} onChange={e => setAddQuery(e.target.value)}
                 placeholder="위젯 검색" style={{ width: 96, fontSize: 12 }}
                 title="제목/id 부분일치로 아래 목록을 좁힌다" />
          <select className="form-input" value={addId} onChange={e => setAddId(e.target.value)}
                  style={{ width: 180, fontSize: 12 }}>
            <option value="">+ 위젯 추가…</option>
            {widgetsByCategory(addQuery).map(g => (
              <optgroup key={g.category} label={g.label}>
                {g.widgets.map(w => (
                  <option key={w.id} value={w.id}>{w.title} ({w.serviceId || 'core'})</option>
                ))}
              </optgroup>
            ))}
          </select>
          <button className="btn btn--sm" onClick={addWidget} disabled={!addId}>추가</button>
          <span style={{ fontSize: 11, color: 'var(--text-muted)', whiteSpace: 'nowrap' }}
                title="카드 사이 간격(px)">간격</span>
          <input type="range" min={0} max={40} step={2} value={draft?.gap ?? GRID_GAP}
                 onChange={e => { const g = parseInt(e.target.value); applyDraft(d => ({ ...d, gap: g })) }}
                 style={{ width: 64 }} title={`카드 사이 간격 ${draft?.gap ?? GRID_GAP}px`} />
          {/* 세로 예산 — 캔버스가 화면 한 장이라 남은 행이 곧 배치 여력이다. */}
          <span className="layout-edit-budget"
                title={`화면 한 장 = ${GRID_ROWS}행. 위젯을 키우면 잠기지 않은 위젯이 그만큼 줄어든다.`}
                style={{ color: freeRows === 0 ? 'var(--danger)' : undefined }}>
            남은 세로 {freeRows}/{GRID_ROWS}행
          </span>
          <button className="btn btn--sm" onClick={undo} disabled={undoStack.length === 0}
                  title={undoStack.length ? `마지막 변경 취소 (${undoStack.length}단계 남음)`
                                          : '되돌릴 변경 없음'}>↶ 되돌리기</button>
          <button className={`btn btn--sm ${preview ? 'btn--primary' : ''}`}
                  title="저장 후 모습 보기 — 제목줄·핸들을 감춘다(조작 잠김)"
                  onClick={() => setPreview(p => !p)}>👁 미리보기</button>
          <button className="btn btn--sm btn--primary" onClick={saveLayout} disabled={saving}>저장</button>
          <button className="btn btn--sm" onClick={cancelEdit} disabled={saving}>취소</button>
          <button className="btn btn--sm" onClick={resetLayout} disabled={saving}
                  title="저장본 삭제 → 기본값 복귀">초기화</button>
        </>
      )}
    </div>
  )

  // seed 개편 안내 — 저장본이 옛 seed 세대 기준이면(= 저장본이 개편된 기본 배치를 가리고 있으면)
  // 그 사실을 알린다. 저장본을 자동으로 버리지는 않는다(운영자가 만든 배치이므로).
  const seedVer = seed.seedVersion ?? 0
  const staleSeed = !editing && isAdmin && seedVer > 0
    && seedVer > (layout.seedVersion ?? 0) && seedVer > dismissedSeedVer
  const applySeed = async () => {
    setSaving(true)
    try {
      await consoleApi.deleteLayout(layoutId)
      setLayout(normalize(seed)); _writeCache(layoutId, seed)
      show('기본 레이아웃(신규)을 적용했습니다', 'ok')
    } catch (e) { show((e as Error).message, 'err') }
    finally { setSaving(false) }
  }
  const keepLayout = () => {
    try { localStorage.setItem(_dismissKey(layoutId), String(seedVer)) } catch { /* ignore */ }
    setDismissedSeedVer(seedVer)
  }

  return (
    <PageParamsProvider>
      <div className="layout-host" ref={hostRef}>
        {/* 편집 컨트롤 — 헤더 슬롯에 portal. FAB 는 데스크톱에서만, 편집 중이면 계속 노출(완료 보장). */}
        {isAdmin && editSlot && (isDesktop || editing) && createPortal(editControls, editSlot)}

        {staleSeed && (
          <div className="seed-update-banner">
            <span>이 페이지의 <b>기본 위젯 배치가 갱신</b>되었습니다 (저장된 배치가 이전 구성을 유지 중).</span>
            <button className="btn btn--sm btn--primary" onClick={applySeed} disabled={saving}>기본값 적용</button>
            <button className="btn btn--sm" onClick={keepLayout} disabled={saving}>이 배치 유지</button>
          </div>
        )}

        {!editing ? (
          layout.widgets.length === 0 ? (
            <div className="empty" style={{ padding: 40, textAlign: 'center' }}>
              아직 위젯이 없습니다{isAdmin ? ' — 상단 [✎ 편집]으로 위젯을 배치하세요.' : '.'}
            </div>
          ) : <GridRenderer layout={layout} />
        ) : draft && (
          // 카드 안 편집도 **이 표면 위에서** 한다 — 그 카드는 자기 자리에 그대로 있고 본문만
          // 중첩 편집기가 된다(§3.0.1). 표면을 갈아끼우면 "어디를 편집 중인지"가 사라진다.
          <EditSurface widgets={draft.widgets} gap={draft.gap ?? GRID_GAP} preview={preview}
                       addId={insideKey === null ? pendingAdd : ''} onAdded={() => setPendingAdd('')}
                       onFull={() => show(`세로 예산(${GRID_ROWS}행)이 꽉 찼습니다 — 다른 위젯을 줄이거나 제거하세요`, 'err')}
                       onEditInside={enterInside}
                       inside={insideLayout && insideKey !== null ? {
                         key: insideKey, layout: insideLayout, saving,
                         canUndo: undoStack.length > 0,
                         onChange: setInsideLayout, onUndo: undo, onReset: resetInside,
                         onSave: () => { void saveLayout() },
                         onCancel: cancelInside, onExit: exitInside,
                       } : null}
                       onChange={ws => applyDraft(d => ({ ...d, widgets: ws }))} />
        )}
      </div>
    </PageParamsProvider>
  )
}
