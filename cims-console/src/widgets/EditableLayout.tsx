// 편집 가능한 위젯 레이아웃 — 저장본 로드 → admin 이 [✎ 편집] 으로 위젯 추가/제거/순서/폭 조정 후 저장.
// view 모드: GridRenderer 그대로. edit 모드: 각 위젯에 컨트롤 오버레이 + 위젯 추가 + 저장/취소/초기화.
// 영속: OAM /console/layouts/<id> (PUT 저장 / DELETE seed 리셋). 없으면 seed.

import { useState, useEffect } from 'react'
import { useAuth } from '../contexts/AuthContext'
import { useToast } from '../components/Toast'
import { consoleApi } from '../api/console'
import { GridRenderer } from './GridRenderer'
import { getWidget, widgetsByCategory } from './registry'
import type { PageLayout, WidgetPlacement } from './types'

const WIDTH_OPTS: { v: number; label: string }[] = [
  { v: 12, label: '전체' }, { v: 6, label: '1/2' }, { v: 4, label: '1/3' }, { v: 3, label: '1/4' },
]
const HEIGHT_OPTS: { v: number; label: string }[] = [
  { v: 0, label: '높이:자동' }, { v: 240, label: '높이:240' }, { v: 360, label: '높이:360' },
  { v: 480, label: '높이:480' }, { v: 640, label: '높이:640' },
]

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
  const isAdmin = user?.role === 'admin'
  const [layout, setLayout] = useState<PageLayout>(() => _readCache(layoutId) || seed)
  const [draft, setDraft] = useState<PageLayout | null>(null)
  const [addId, setAddId] = useState('')
  const [saving, setSaving] = useState(false)

  useEffect(() => {
    let alive = true
    consoleApi.getLayout(layoutId)
      .then(l => { if (alive && l && Array.isArray(l.widgets)) { setLayout(l); _writeCache(layoutId, l) } })
      .catch(() => { /* 404/오류 → 캐시/seed 유지 */ })
    return () => { alive = false }
  }, [layoutId])

  const editing = draft !== null
  const beginEdit = () => setDraft(clone(layout))
  const cancelEdit = () => { setDraft(null); setAddId('') }

  const mutate = (fn: (ws: WidgetPlacement[]) => WidgetPlacement[]) =>
    setDraft(d => d ? { ...d, widgets: fn([...d.widgets]) } : d)

  const move = (i: number, dir: -1 | 1) => mutate(ws => {
    const j = i + dir
    if (j < 0 || j >= ws.length) return ws
    ;[ws[i], ws[j]] = [ws[j], ws[i]]
    return ws
  })
  const remove = (i: number) => mutate(ws => ws.filter((_, k) => k !== i))
  const setWidth = (i: number, w: number) => mutate(ws => ws.map((p, k) => k === i ? { ...p, w } : p))
  const setHeight = (i: number, h: number) => mutate(ws => ws.map((p, k) => k === i ? { ...p, h: h || undefined } : p))
  const addWidget = () => {
    if (!addId) return
    const def = getWidget(addId)
    mutate(ws => [...ws, { widgetId: addId, w: def?.defaultSize?.w ?? 12 }])
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

  return (
    <div>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 12 }}>
        {editing && <span style={{ fontSize: 12, color: 'var(--primary)', fontWeight: 600 }}>편집 중 — 저장 전까지 반영 안 됨</span>}
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 6, alignItems: 'center' }}>
          {!editing && isAdmin && (
            <button className="btn btn--sm" onClick={beginEdit}>✎ 편집</button>
          )}
          {editing && (
            <>
              <select className="form-input" value={addId} onChange={e => setAddId(e.target.value)}
                      style={{ width: 200, fontSize: 12 }}>
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
              <button className="btn btn--sm btn--primary" onClick={saveLayout} disabled={saving}>저장</button>
              <button className="btn btn--sm" onClick={cancelEdit} disabled={saving}>취소</button>
              <button className="btn btn--sm" onClick={resetLayout} disabled={saving}
                      title="저장본 삭제 → 기본값 복귀">초기화</button>
            </>
          )}
        </div>
      </div>

      {!editing ? (
        <GridRenderer layout={layout} />
      ) : draft && (
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(12, 1fr)', gap: 12, alignItems: 'start' }}>
          {draft.widgets.map((p, i) => {
            const def = getWidget(p.widgetId)
            const span = Math.min(Math.max(p.w ?? def?.defaultSize?.w ?? 12, 1), 12)
            const Comp = def?.component
            return (
              <div key={`${p.widgetId}-${i}`}
                   style={{ gridColumn: `span ${span}`, minWidth: 0, border: '1px dashed var(--primary)', borderRadius: 6, padding: 6 }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 4, marginBottom: 6, fontSize: 11 }}>
                  <b style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                    {def?.title ?? p.widgetId}
                  </b>
                  <span style={{ color: 'var(--text-muted)' }}>({p.widgetId})</span>
                  <div style={{ marginLeft: 'auto', display: 'flex', gap: 3, alignItems: 'center' }}>
                    <button className="btn btn--sm" onClick={() => move(i, -1)} disabled={i === 0} title="위로">↑</button>
                    <button className="btn btn--sm" onClick={() => move(i, 1)} disabled={i === draft.widgets.length - 1} title="아래로">↓</button>
                    <select value={span} onChange={e => setWidth(i, parseInt(e.target.value))}
                            style={{ fontSize: 11, padding: '1px 2px' }} title="폭">
                      {WIDTH_OPTS.map(o => <option key={o.v} value={o.v}>{o.label}</option>)}
                    </select>
                    <select value={p.h ?? 0} onChange={e => setHeight(i, parseInt(e.target.value))}
                            style={{ fontSize: 11, padding: '1px 2px' }} title="높이">
                      {HEIGHT_OPTS.map(o => <option key={o.v} value={o.v}>{o.label}</option>)}
                    </select>
                    <button className="btn btn--sm" onClick={() => remove(i)} title="제거"
                            style={{ color: 'var(--danger)' }}>✕</button>
                  </div>
                </div>
                <div style={{ pointerEvents: 'none', opacity: 0.85, ...(p.h ? { maxHeight: p.h, overflowY: 'auto' as const } : {}) }}>
                  {Comp ? <Comp config={p.config} /> : <div style={{ color: 'var(--danger)', fontSize: 12 }}>알 수 없는 위젯: {p.widgetId}</div>}
                </div>
              </div>
            )
          })}
          {draft.widgets.length === 0 && (
            <div style={{ gridColumn: 'span 12', color: 'var(--text-muted)', fontSize: 13, padding: 20, textAlign: 'center' }}>
              위젯이 없습니다 — 상단 [+ 위젯 추가]로 배치하세요.
            </div>
          )}
        </div>
      )}
    </div>
  )
}
