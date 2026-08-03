// 편집 가능한 위젯 레이아웃 — 저장본 로드 → admin 이 [✎ 편집] 으로 위젯 추가/제거 및 드래그·리사이즈로
// 2D 배치 후 저장. view 모드: GridRenderer. edit 모드: GridEditor(포인터 드래그/리사이즈 + compaction).
// 편집 진입 시 legacy(flow) 레이아웃은 grid 로 1회 migrate(flowToGrid). 좁은 화면에선 편집 비활성.
// 영속: OAM /console/layouts/<id> (PUT 저장 / DELETE seed 리셋). 없으면 seed.

import { useState, useEffect } from 'react'
import { createPortal } from 'react-dom'
import { useAuth } from '../contexts/AuthContext'
import { hasRole } from '../utils/permissions'
import { useToast } from '../components/Toast'
import { useIsDesktop } from '../hooks/useIsDesktop'
import { consoleApi } from '../api/console'
import { GridRenderer } from './GridRenderer'
import { GridEditor } from './GridEditor'
import { getWidget, widgetsByCategory } from './registry'
import { flowToGrid, addToFirstFree, isGridLayout, ROW_H_VH, GRID_GAP } from './gridLayout'
import type { PageLayout } from './types'

function clone(l: PageLayout): PageLayout {
  return JSON.parse(JSON.stringify(l))
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

export function EditableLayout({ layoutId, seed }: { layoutId: string; seed: PageLayout }) {
  const { user } = useAuth()
  const { show } = useToast()
  const isAdmin = hasRole(user, 'admin')   // developer(admin 동급) 포함
  const isDesktop = useIsDesktop()          // 편집(드래그/리사이즈)은 데스크톱 전용 — 뷰는 좁은 화면도 단일열 동작
  const [layout, setLayout] = useState<PageLayout>(() => _readCache(layoutId) || seed)
  const [draft, setDraft] = useState<PageLayout | null>(null)
  const [addId, setAddId] = useState('')
  const [saving, setSaving] = useState(false)
  // 편집 컨트롤은 글로벌 헤더의 슬롯(#layout-edit-slot)으로 portal — 콘텐츠/위젯 컨트롤과의 겹침 원천 차단.
  const [editSlot, setEditSlot] = useState<HTMLElement | null>(null)
  useEffect(() => { setEditSlot(document.getElementById('layout-edit-slot')) }, [])

  useEffect(() => {
    let alive = true
    consoleApi.getLayout(layoutId)
      .then(l => { if (alive && l && Array.isArray(l.widgets)) { setLayout(l); _writeCache(layoutId, l) } })
      .catch(() => { /* 404/오류 → 캐시/seed 유지 */ })
    return () => { alive = false }
  }, [layoutId])

  const editing = draft !== null
  // 편집 진입 — legacy(flow) 면 grid 로 migrate. vh→행 변환은 라이브 뷰포트 기준.
  const beginEdit = () => {
    const base = clone(layout)
    const widgets = isGridLayout(base.widgets)
      ? base.widgets
      : flowToGrid(base.widgets,
          id => getWidget(id)?.defaultSize?.w,
          vh => Math.round(vh / ROW_H_VH))   // legacy vh → 행 (뷰포트 무관: 행=화면 5%)
    setDraft({ ...base, widgets })
  }
  const cancelEdit = () => { setDraft(null); setAddId('') }

  const addWidget = () => {
    if (!addId) return
    const def = getWidget(addId)
    setDraft(d => d
      ? { ...d, widgets: addToFirstFree(d.widgets, { widgetId: addId }, def?.defaultSize?.w, def?.defaultSize?.h) }
      : d)
    setAddId('')
  }

  const saveLayout = async () => {
    if (!draft) return
    setSaving(true)
    try {
      const saved = await consoleApi.putLayout(layoutId, draft)
      const next = saved && Array.isArray(saved.widgets) ? saved : draft
      setLayout(next); _writeCache(layoutId, next)
      setDraft(null); setAddId('')
      show('레이아웃 저장됨', 'ok')
    } catch (e) { show((e as Error).message, 'err') }
    finally { setSaving(false) }
  }
  const resetLayout = async () => {
    if (!confirm('저장된 레이아웃을 삭제하고 기본값(seed)으로 되돌릴까요?')) return
    setSaving(true)
    try {
      await consoleApi.deleteLayout(layoutId)
      setLayout(seed); _writeCache(layoutId, seed); setDraft(null); setAddId('')
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
          <select className="form-input" value={addId} onChange={e => setAddId(e.target.value)}
                  style={{ width: 180, fontSize: 12 }}>
            <option value="">+ 위젯 추가…</option>
            {widgetsByCategory().map(g => (
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
          <button className="btn btn--sm btn--primary" onClick={saveLayout} disabled={saving}>저장</button>
          <button className="btn btn--sm" onClick={cancelEdit} disabled={saving}>취소</button>
          <button className="btn btn--sm" onClick={resetLayout} disabled={saving}
                  title="저장본 삭제 → 기본값 복귀">초기화</button>
        </>
      )}
    </div>
  )

  return (
    <div className="layout-host">
      {/* 편집 컨트롤 — 헤더 슬롯에 portal. FAB 는 데스크톱에서만, 편집 중이면 계속 노출(완료 보장). */}
      {isAdmin && editSlot && (isDesktop || editing) && createPortal(editControls, editSlot)}

      {!editing ? (
        layout.widgets.length === 0 ? (
          <div className="empty" style={{ padding: 40, textAlign: 'center' }}>
            아직 위젯이 없습니다{isAdmin ? ' — 상단 [✎ 편집]으로 위젯을 배치하세요.' : '.'}
          </div>
        ) : <GridRenderer layout={layout} />
      ) : draft && (
        <GridEditor widgets={draft.widgets} gap={draft.gap ?? GRID_GAP}
                    onChange={ws => setDraft(d => d ? { ...d, widgets: ws } : d)} />
      )}
    </div>
  )
}
