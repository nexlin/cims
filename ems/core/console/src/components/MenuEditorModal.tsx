// 메뉴 편집 모달 (admin) — 사이드바 구성 편집.
//  ① 영역(운용/관리 그룹핑): 라벨 변경, 커스텀 영역 추가/삭제(소속 메뉴는 관리로 이동)
//  ② 섹션: 순서/라벨/숨김/소속 영역 — 단, 시스템/릴리스는 잠금(편집 불가)
//  ③ 커스텀 메뉴 그룹 + 위젯 합성 페이지(/custom/<slug>) 추가/삭제
// 코어 라우트/아이콘/경로는 코드 SoT (링크는 안 깨짐).
import { useState } from 'react'
import { ChevronUp, ChevronDown, Lock, Plus, Trash2 } from 'lucide-react'
import Modal from './Modal'
import { useToast } from './Toast'
import { VISIBLE_SECTIONS } from '../routes'
import { useMenu } from '../contexts/MenuContext'
import { consoleApi } from '../api/console'
import {
  LOCKED_SECTION_KEYS, DEFAULT_AREAS,
  type MenuConfig, type MenuItemOverride, type MenuAreaDef,
  type CustomSectionDef, type CustomPageDef,
} from '../menu'

interface SectionRow {
  key: string
  label: string
  defaultLabel: string
  hidden: boolean
  area: string
  defaultArea: string
  locked: boolean
  custom: boolean
  pages: CustomPageDef[]   // custom 그룹만 사용
}

interface AreaRow {
  key: string
  label: string
  builtin: boolean   // ops/admin — 삭제 불가, 라벨만 변경
}

function newSlug(): string {
  return 'p' + Date.now().toString(36) + Math.floor(Math.random() * 1296).toString(36)
}

function buildAreaRows(cfg: MenuConfig | null): AreaRow[] {
  const rows: AreaRow[] = DEFAULT_AREAS.map(a => ({ key: a.key, label: a.label, builtin: true }))
  for (const a of cfg?.areas ?? []) {
    const b = rows.find(r => r.key === a.key)
    if (b) b.label = a.label || b.label
    else rows.push({ key: a.key, label: a.label, builtin: false })
  }
  return rows
}

function buildSectionRows(cfg: MenuConfig | null): SectionRow[] {
  const coreByKey = new Map(VISIBLE_SECTIONS.map(s => [s.key, s]))
  const customByKey = new Map((cfg?.customSections ?? []).map(cs => [cs.key, cs]))
  const rows: SectionRow[] = []
  const used = new Set<string>()
  const pushCore = (key: string, it?: MenuItemOverride) => {
    const s = coreByKey.get(key)
    if (!s) return
    const locked = LOCKED_SECTION_KEYS.has(key)
    rows.push({
      key, locked, custom: false, pages: [],
      label: (!locked && it?.label) || s.label,
      defaultLabel: s.label,
      hidden: !locked && !!it?.hidden,
      area: (!locked && it?.area) || s.area || 'admin',
      defaultArea: s.area || 'admin',
    })
  }
  const pushCustom = (cs: CustomSectionDef, it?: MenuItemOverride) => {
    rows.push({
      key: cs.key, locked: false, custom: true,
      label: cs.label, defaultLabel: cs.label,
      hidden: !!it?.hidden,
      area: cs.area || 'admin', defaultArea: 'admin',
      pages: cs.pages.map(p => ({ ...p })),
    })
  }
  for (const it of cfg?.items ?? []) {
    if (used.has(it.key)) continue
    if (coreByKey.has(it.key)) { used.add(it.key); pushCore(it.key, it) }
    else if (customByKey.has(it.key)) { used.add(it.key); pushCustom(customByKey.get(it.key)!, it) }
  }
  for (const s of VISIBLE_SECTIONS) if (!used.has(s.key)) { used.add(s.key); pushCore(s.key) }
  for (const cs of cfg?.customSections ?? []) if (!used.has(cs.key)) { used.add(cs.key); pushCustom(cs) }
  return rows
}

export function MenuEditorModal({ onClose }: { onClose: () => void }) {
  const { savedConfig, reload } = useMenu()
  const { show } = useToast()
  const [areas, setAreas] = useState<AreaRow[]>(() => buildAreaRows(savedConfig))
  const [rows, setRows] = useState<SectionRow[]>(() => buildSectionRows(savedConfig))
  const [saving, setSaving] = useState(false)

  // ── 영역 편집 ──
  const setAreaLabel = (key: string, v: string) =>
    setAreas(as => as.map(a => a.key === key ? { ...a, label: v } : a))
  const addArea = () => {
    const key = 'area:' + newSlug()
    setAreas(as => [...as, { key, label: '새 영역', builtin: false }])
  }
  const removeArea = (key: string) => {
    // 해당 영역의 메뉴는 관리(admin)로 이동
    setRows(rs => rs.map(r => r.area === key ? { ...r, area: 'admin' } : r))
    setAreas(as => as.filter(a => a.key !== key))
  }

  // ── 섹션 편집 ──
  const move = (i: number, dir: -1 | 1) => setRows(rs => {
    const j = i + dir
    if (j < 0 || j >= rs.length) return rs
    const c = [...rs]; [c[i], c[j]] = [c[j], c[i]]; return c
  })
  const patchRow = (i: number, p: Partial<SectionRow>) =>
    setRows(rs => rs.map((r, k) => k === i ? { ...r, ...p } : r))
  const removeCustom = (i: number) => setRows(rs => rs.filter((_, k) => k !== i))

  const addGroup = () => {
    const key = 'custom:' + newSlug()
    setRows(rs => [...rs, {
      key, label: '새 메뉴 그룹', defaultLabel: '새 메뉴 그룹', hidden: false,
      area: 'admin', defaultArea: 'admin', locked: false, custom: true, pages: [],
    }])
  }
  const addPage = (i: number) => setRows(rs => rs.map((r, k) => k === i
    ? { ...r, pages: [...r.pages, { slug: newSlug(), title: '새 페이지' }] } : r))
  const setPageTitle = (i: number, pi: number, v: string) => setRows(rs => rs.map((r, k) => k === i
    ? { ...r, pages: r.pages.map((p, x) => x === pi ? { ...p, title: v } : p) } : r))
  const removePage = (i: number, pi: number) => setRows(rs => rs.map((r, k) => k === i
    ? { ...r, pages: r.pages.filter((_, x) => x !== pi) } : r))

  const resetDefault = () => {
    setAreas(DEFAULT_AREAS.map(a => ({ key: a.key, label: a.label, builtin: true })))
    setRows(buildSectionRows(null))
  }

  const save = async () => {
    for (const r of rows) {
      if (r.custom && !r.label.trim()) { show('메뉴 그룹 이름을 입력하세요', 'err'); return }
      if (r.custom && r.pages.some(p => !p.title.trim())) { show('페이지 제목을 입력하세요', 'err'); return }
    }
    setSaving(true)
    try {
      const items: MenuItemOverride[] = rows.map(r => ({
        key: r.key,
        ...(!r.locked && !r.custom && r.label.trim() && r.label !== r.defaultLabel ? { label: r.label.trim() } : {}),
        ...(!r.locked && r.hidden ? { hidden: true } : {}),
        ...(!r.locked && !r.custom && r.area !== r.defaultArea ? { area: r.area } : {}),
      }))
      const customSections: CustomSectionDef[] = rows.filter(r => r.custom).map(r => ({
        key: r.key, label: r.label.trim(), area: r.area,
        pages: r.pages.map(p => ({ slug: p.slug, title: p.title.trim() })),
      }))
      const areaDefs: MenuAreaDef[] = areas
        .filter(a => !a.builtin || a.label !== DEFAULT_AREAS.find(d => d.key === a.key)?.label)
        .map(a => ({ key: a.key, label: a.label.trim() || a.key }))
      await consoleApi.putMenu({ items, custom_sections: customSections, areas: areaDefs })
      await reload()
      show('메뉴 저장됨', 'ok')
      onClose()
    } catch (e) { show((e as Error).message, 'err') }
    finally { setSaving(false) }
  }

  const muted = { fontSize: 11, color: 'var(--muted-foreground)' } as const
  return (
    <Modal title="메뉴 편집" onClose={onClose} width={760}>
      {/* 영역(그룹핑) */}
      <div style={{ marginBottom: 14 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
          <b style={{ fontSize: 13 }}>영역 (메뉴 그룹핑)</b>
          <span style={muted}>운용/관리처럼 사이드바를 크게 나누는 묶음 — 라벨 변경·영역 추가 가능</span>
          <button className="btn btn--sm btn--outline" style={{ marginLeft: 'auto' }} onClick={addArea}>
            <Plus size={13} /> 영역 추가
          </button>
        </div>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 8 }}>
          {areas.map(a => (
            <span key={a.key} style={{ display: 'inline-flex', alignItems: 'center', gap: 4 }}>
              <input className="form-input" style={{ width: 130 }} value={a.label}
                onChange={e => setAreaLabel(a.key, e.target.value)} />
              {!a.builtin && (
                <button className="btn btn--sm btn--outline" title="영역 삭제 (소속 메뉴는 관리로 이동)"
                  onClick={() => removeArea(a.key)}><Trash2 size={13} /></button>
              )}
            </span>
          ))}
        </div>
      </div>

      {/* 섹션 목록 */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
        <b style={{ fontSize: 13 }}>메뉴</b>
        <span style={muted}>시스템·릴리스는 잠금 (이름변경/숨김/이동 불가)</span>
        <button className="btn btn--sm btn--outline" style={{ marginLeft: 'auto' }} onClick={addGroup}>
          <Plus size={13} /> 메뉴 그룹 추가
        </button>
      </div>
      <div style={{ maxHeight: 420, overflowY: 'auto', border: '1px solid var(--border)', borderRadius: 'var(--radius)' }}>
        {rows.map((r, i) => (
          <div key={r.key} style={{
            borderBottom: '1px solid var(--border)', padding: '6px 8px',
            background: 'var(--muted)', opacity: r.hidden ? 0.55 : 1,
          }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
              <span style={{ display: 'inline-flex', flexDirection: 'column', gap: 2 }}>
                <button className="btn btn--sm btn--outline" style={{ padding: '0 4px', lineHeight: 1 }}
                  disabled={i === 0} onClick={() => move(i, -1)}><ChevronUp size={13} /></button>
                <button className="btn btn--sm btn--outline" style={{ padding: '0 4px', lineHeight: 1 }}
                  disabled={i === rows.length - 1} onClick={() => move(i, 1)}><ChevronDown size={13} /></button>
              </span>
              {r.locked
                ? <span title="잠금 — 시스템/릴리스 메뉴는 편집할 수 없습니다"
                    style={{ display: 'inline-flex', alignItems: 'center', gap: 5, width: 190, fontSize: 13 }}>
                    <Lock size={13} /> {r.defaultLabel}
                  </span>
                : <input className="form-input" style={{ width: 190 }} value={r.label}
                    placeholder={r.defaultLabel} onChange={e => patchRow(i, { label: e.target.value })} />}
              <select className="form-input" style={{ width: 120 }} value={r.area} disabled={r.locked}
                title="소속 영역" onChange={e => patchRow(i, { area: e.target.value })}>
                {areas.map(a => <option key={a.key} value={a.key}>{a.label}</option>)}
              </select>
              <label style={{ display: 'inline-flex', alignItems: 'center', gap: 4, fontSize: 12, opacity: r.locked ? 0.4 : 1 }}>
                <input type="checkbox" checked={r.hidden} disabled={r.locked}
                  onChange={() => patchRow(i, { hidden: !r.hidden })} /> 숨김
              </label>
              {r.custom ? (
                <>
                  <span style={{ ...muted, border: '1px solid var(--border)', borderRadius: 4, padding: '1px 5px' }}>커스텀</span>
                  <button className="btn btn--sm btn--outline" style={{ marginLeft: 'auto' }}
                    title="그룹 삭제" onClick={() => removeCustom(i)}><Trash2 size={13} /></button>
                </>
              ) : (!r.locked && r.label !== r.defaultLabel &&
                <span style={{ ...muted, marginLeft: 'auto' }}>기본: {r.defaultLabel}</span>
              )}
            </div>
            {r.custom && (
              <div style={{ margin: '6px 0 2px 34px' }}>
                {r.pages.map((p, pi) => (
                  <div key={p.slug} style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 4 }}>
                    <input className="form-input" style={{ width: 210 }} value={p.title}
                      onChange={e => setPageTitle(i, pi, e.target.value)} />
                    <code style={muted}>/custom/{p.slug}</code>
                    <button className="btn btn--sm btn--outline" title="페이지 삭제"
                      onClick={() => removePage(i, pi)}><Trash2 size={13} /></button>
                  </div>
                ))}
                <button className="btn btn--sm btn--outline" onClick={() => addPage(i)}>
                  <Plus size={13} /> 페이지 추가
                </button>
                <span style={{ ...muted, marginLeft: 8 }}>
                  페이지는 빈 위젯 보드로 생성 — 저장 후 해당 페이지에서 위젯을 배치하세요
                </span>
              </div>
            )}
          </div>
        ))}
      </div>

      <div className="modal-footer" style={{ marginTop: 14 }}>
        <button className="btn btn--outline" onClick={resetDefault} disabled={saving}
                style={{ marginRight: 'auto' }}>기본값으로</button>
        <button className="btn btn--outline" onClick={onClose} disabled={saving}>취소</button>
        <button className="btn btn--primary" onClick={save} disabled={saving}>
          {saving ? '저장 중...' : '저장'}
        </button>
      </div>
    </Modal>
  )
}
