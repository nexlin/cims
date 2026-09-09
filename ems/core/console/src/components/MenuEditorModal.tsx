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
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { fromSel, toSel } from '@core/components/custom/select-value'
import { Checkbox } from '@core/components/ui/checkbox'

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

  return (
    <Modal title="메뉴 편집" onClose={onClose} width={760}>
      {/* 영역(그룹핑) */}
      <div className="mb-3.5">
        <div className="flex items-center gap-2 mb-1.5">
          <b className="text-md">영역 (메뉴 그룹핑)</b>
          <span className="text-xs text-muted-foreground">운용/관리처럼 사이드바를 크게 나누는 묶음 — 라벨 변경·영역 추가 가능</span>
          <Button className="ml-auto" onClick={addArea}>
            <Plus size={13} /> 영역 추가
          </Button>
        </div>
        <div className="flex flex-wrap gap-2">
          {areas.map(a => (
            <span className="inline-flex items-center gap-1" key={a.key}>
              <Input className="w-[130px]" value={a.label}
                onChange={e => setAreaLabel(a.key, e.target.value)}/>
              {!a.builtin && (
                <Button title="영역 삭제 (소속 메뉴는 관리로 이동)"
                  onClick={() => removeArea(a.key)}><Trash2 size={13} /></Button>
              )}
            </span>
          ))}
        </div>
      </div>

      {/* 섹션 목록 */}
      <div className="flex items-center gap-2 mb-1.5">
        <b className="text-md">메뉴</b>
        <span className="text-xs text-muted-foreground">시스템·릴리스는 잠금 (이름변경/숨김/이동 불가)</span>
        <Button className="ml-auto" onClick={addGroup}>
          <Plus size={13} /> 메뉴 그룹 추가
        </Button>
      </div>
      <div className="max-h-[420px] overflow-y-auto border border-border rounded-md">
        {rows.map((r, i) => (
          <div key={r.key} style={{
            borderBottom: '1px solid var(--border)', padding: '6px 8px',
            background: 'var(--muted)', opacity: r.hidden ? 0.55 : 1,
          }}>
            <div className="flex items-center gap-1.5">
              <span className="inline-flex flex-col gap-0.5">
                <Button className="py-0 px-1 leading-none"
                  disabled={i === 0} onClick={() => move(i, -1)}><ChevronUp size={13} /></Button>
                <Button className="py-0 px-1 leading-none"
                  disabled={i === rows.length - 1} onClick={() => move(i, 1)}><ChevronDown size={13} /></Button>
              </span>
              {r.locked
                ? <span className="inline-flex items-center gap-[5px] w-[190px] text-md" title="잠금 — 시스템/릴리스 메뉴는 편집할 수 없습니다">
                    <Lock size={13} /> {r.defaultLabel}
                  </span>
                : <Input className="w-[190px]" value={r.label}
                    placeholder={r.defaultLabel} onChange={e => patchRow(i, { label: e.target.value })}/>}
              <Select value={toSel(r.area)} onValueChange={(v: string) => patchRow(i, { area: fromSel(v) })} disabled={r.locked}>
                <SelectTrigger className="w-[120px]" title="소속 영역"><SelectValue /></SelectTrigger>
                <SelectContent>
                  {areas.map(a => <SelectItem key={a.key} value={a.key}>{a.label}</SelectItem>)}
                </SelectContent>
              </Select>
              <label className={`inline-flex items-center gap-1 text-sm ${r.locked ? 'opacity-40' : ''}`}>
                <Checkbox  checked={r.hidden} disabled={r.locked} onCheckedChange={() => patchRow(i, { hidden: !r.hidden })} /> 숨김
              </label>
              {r.custom ? (
                <>
                  <span className="rounded-sm border border-border px-1.5 py-px text-xs text-muted-foreground">커스텀</span>
                  <Button className="ml-auto"
                    title="그룹 삭제" onClick={() => removeCustom(i)}><Trash2 size={13} /></Button>
                </>
              ) : (!r.locked && r.label !== r.defaultLabel &&
                <span className="ml-auto text-xs text-muted-foreground">기본: {r.defaultLabel}</span>
              )}
            </div>
            {r.custom && (
              <div className="mt-1.5 mr-0 mb-0.5 ml-[34px]">
                {r.pages.map((p, pi) => (
                  <div className="flex items-center gap-1.5 mb-1" key={p.slug}>
                    <Input className="w-[210px]" value={p.title}
                      onChange={e => setPageTitle(i, pi, e.target.value)}/>
                    <code className="text-xs text-muted-foreground">/custom/{p.slug}</code>
                    <Button title="페이지 삭제"
                      onClick={() => removePage(i, pi)}><Trash2 size={13} /></Button>
                  </div>
                ))}
                <Button onClick={() => addPage(i)}>
                  <Plus size={13} /> 페이지 추가
                </Button>
                <span className="ml-2 text-xs text-muted-foreground">
                  페이지는 빈 위젯 보드로 생성 — 저장 후 해당 페이지에서 위젯을 배치하세요
                </span>
              </div>
            )}
          </div>
        ))}
      </div>

      <div className="flex justify-end gap-2.5 pt-5 mt-3.5">
        <Button className="mr-auto" size="default" onClick={resetDefault} disabled={saving}>기본값으로</Button>
        <Button size="default" onClick={onClose} disabled={saving}>취소</Button>
        <Button variant="default" size="default" onClick={save} disabled={saving}>
          {saving ? '저장 중...' : '저장'}
        </Button>
      </div>
    </Modal>
  )
}
