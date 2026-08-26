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
import { GridEditor } from './GridEditor'
import { getWidget, splitFor, widgetsByCategory } from './registry'
import { flowToGrid, addToFirstFree, isGridLayout, ROW_H_VH, GRID_GAP } from './gridLayout'
import { PageParamsProvider, isPlacementVisible, usePageParams } from './pageParams'
import { expandSplits } from './legacySplit'
import type { PageLayout, WidgetPlacement } from './types'

// 편집 표면 — **화면에 보이는 것과 같은 배치만** 편집한다. 탭으로 갈아끼우는 위젯들은 같은 자리를
// 쓰므로, 다른 탭의 배치를 함께 늘어놓으면 편집 화면이 실제와 달라진다. 숨은 배치는 좌표를 그대로
// 보존한 채 뒤에 다시 붙인다(grid 배치는 절대 좌표라 배열 순서는 의미 없음).
// 컨트롤 위젯은 편집 중에도 눌리므로(GridEditor) 탭을 바꿔가며 각 탭의 배치를 편집할 수 있다.
function EditSurface({ widgets, gap, addId, preview, onAdded, onChange }: {
  widgets: WidgetPlacement[]
  gap: number
  addId: string
  preview: boolean
  onAdded: () => void
  onChange: (ws: WidgetPlacement[]) => void
}) {
  const params = usePageParams()
  const visible = widgets.filter(p => isPlacementVisible(p, params))
  const hidden = widgets.filter(p => !isPlacementVisible(p, params))

  // 위젯 추가도 "보이는 배치" 기준으로 자리를 잡는다 — 숨은 탭 아래로 밀려나지 않게.
  useEffect(() => {
    if (!addId) return
    const def = getWidget(addId)
    onChange([...addToFirstFree(visible, { widgetId: addId }, def?.defaultSize?.w, def?.defaultSize?.h), ...hidden])
    onAdded()
    // addId 가 설정된 순간에만 동작 — visible/hidden 은 그 시점 값을 쓴다.
  }, [addId])   // eslint-disable-line react-hooks/exhaustive-deps

  return (
    <GridEditor widgets={visible} gap={gap} preview={preview}
                onChange={ws => onChange([...ws, ...hidden])} />
  )
}

function clone(l: PageLayout): PageLayout {
  return JSON.parse(JSON.stringify(l))
}

// 레이아웃을 화면에 올리기 전 정규화 — 폐지된 묶음 위젯을 부품으로 펼친다(legacySplit).
// 저장본을 다시 쓰진 않는다(표시만 전개) — 운영자가 편집·저장하면 그때 펼친 형태로 굳는다.
function normalize(l: PageLayout): PageLayout {
  const widgets = expandSplits(l.widgets, splitFor)
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

  const editing = draft !== null
  // 편집 진입 — legacy(flow) 면 grid 로 migrate. vh→행 변환은 라이브 뷰포트 기준.
  // 높이가 지정되지 않은 배치(통짜 페이지 등)는 **지금 화면에 그려진 높이**를 재서 초기값으로 쓴다 —
  // 상수를 박으면 편집 카드가 실제 화면과 전혀 다른 크기로 잡힌다.
  const beginEdit = () => {
    const base = clone(layout)
    const rowPx = Math.max(1, (typeof window !== 'undefined' ? window.innerHeight : 800) * ROW_H_VH / 100)
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
          vh => Math.round(vh / ROW_H_VH),   // legacy vh → 행 (뷰포트 무관: 행=화면 2%)
          undefined, measureRows)
    setDraft({ ...base, widgets })
  }
  const cancelEdit = () => { setDraft(null); setAddId(''); setAddQuery(''); setPreview(false) }

  // 실제 배치는 EditSurface 가 한다 — 현재 탭에서 보이는 배치 기준으로 자리를 잡아야 하므로.
  const [pendingAdd, setPendingAdd] = useState('')
  const [preview, setPreview] = useState(false)   // 저장 후 모습 확인 — 제목줄·핸들을 감춘다
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

  const editControls = (
    <div className="layout-edit-headerbar">
      {!editing ? (
        <button className="btn btn--sm layout-edit-fab" onClick={beginEdit}
                title="이 페이지를 위젯으로 편집">✎ 편집</button>
      ) : (
        <>
          <span className="layout-edit-hint">편집 중</span>
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
                 onChange={e => { const g = parseInt(e.target.value); setDraft(d => d ? { ...d, gap: g } : d) }}
                 style={{ width: 64 }} title={`카드 사이 간격 ${draft?.gap ?? GRID_GAP}px`} />
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
          <EditSurface widgets={draft.widgets} gap={draft.gap ?? GRID_GAP} preview={preview}
                       addId={pendingAdd} onAdded={() => setPendingAdd('')}
                       onChange={ws => setDraft(d => d ? { ...d, widgets: ws } : d)} />
        )}
      </div>
    </PageParamsProvider>
  )
}
