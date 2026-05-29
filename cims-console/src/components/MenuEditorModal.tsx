// 메뉴 편집 모달 (admin) — 상단 nav 섹션의 순서·이름·표시여부 조정. 라우트/아이콘은 고정.
import { useState } from 'react'
import { ChevronUp, ChevronDown } from 'lucide-react'
import Modal from './Modal'
import { useToast } from './Toast'
import { VISIBLE_SECTIONS } from '../routes'
import { useMenu } from '../contexts/MenuContext'
import { consoleApi } from '../api/console'
import type { MenuItemOverride } from '../menu'

interface Row { key: string; label: string; defaultLabel: string; hidden: boolean }

function buildRows(saved: MenuItemOverride[] | null): Row[] {
  const byKey = new Map(VISIBLE_SECTIONS.map(s => [s.key, s]))
  const rows: Row[] = []
  const used = new Set<string>()
  if (saved && saved.length) {
    for (const it of saved) {
      const s = byKey.get(it.key)
      if (!s || used.has(it.key)) continue
      used.add(it.key)
      rows.push({ key: s.key, label: it.label ?? s.label, defaultLabel: s.label, hidden: !!it.hidden })
    }
  }
  for (const s of VISIBLE_SECTIONS) {
    if (used.has(s.key)) continue
    rows.push({ key: s.key, label: s.label, defaultLabel: s.label, hidden: false })
  }
  return rows
}

export function MenuEditorModal({ onClose }: { onClose: () => void }) {
  const { savedItems, reload } = useMenu()
  const { show } = useToast()
  const [rows, setRows] = useState<Row[]>(() => buildRows(savedItems))
  const [saving, setSaving] = useState(false)

  const move = (i: number, dir: -1 | 1) => setRows(rs => {
    const j = i + dir
    if (j < 0 || j >= rs.length) return rs
    const c = [...rs]; [c[i], c[j]] = [c[j], c[i]]; return c
  })
  const setLabel = (i: number, v: string) => setRows(rs => rs.map((r, k) => k === i ? { ...r, label: v } : r))
  const toggleHidden = (i: number) => setRows(rs => rs.map((r, k) => k === i ? { ...r, hidden: !r.hidden } : r))
  const resetDefault = () => setRows(VISIBLE_SECTIONS.map(s => ({ key: s.key, label: s.label, defaultLabel: s.label, hidden: false })))

  const save = async () => {
    setSaving(true)
    try {
      const items: MenuItemOverride[] = rows.map(r => ({
        key: r.key,
        ...(r.label !== r.defaultLabel && r.label.trim() ? { label: r.label.trim() } : {}),
        ...(r.hidden ? { hidden: true } : {}),
      }))
      await consoleApi.putMenu(items)
      await reload()
      show('메뉴 저장됨', 'ok')
      onClose()
    } catch (e) { show((e as Error).message, 'err') }
    finally { setSaving(false) }
  }

  return (
    <Modal title="메뉴 편집" onClose={onClose} width={480}>
      <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 12 }}>
        상단 메뉴의 순서·이름·표시 여부를 조정합니다. 라우트/아이콘은 고정이라 링크는 안 깨집니다.
      </div>
      <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
        {rows.map((r, i) => (
          <div key={r.key} style={{
            display: 'flex', alignItems: 'center', gap: 8, padding: '6px 8px',
            border: '1px solid var(--border)', borderRadius: 'var(--radius)',
            background: 'var(--bg-soft)', opacity: r.hidden ? 0.55 : 1,
          }}>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
              <button className="btn btn--sm btn--outline" onClick={() => move(i, -1)} disabled={i === 0}
                      style={{ padding: '0 4px', lineHeight: 1 }}><ChevronUp size={13} /></button>
              <button className="btn btn--sm btn--outline" onClick={() => move(i, 1)} disabled={i === rows.length - 1}
                      style={{ padding: '0 4px', lineHeight: 1 }}><ChevronDown size={13} /></button>
            </div>
            <input className="form-input" value={r.label} onChange={e => setLabel(i, e.target.value)} style={{ flex: 1 }} />
            <code style={{ fontSize: 11, color: 'var(--text-muted)', width: 78 }}>{r.key}</code>
            <label className="toggle" title={r.hidden ? '숨김 (클릭 시 표시)' : '표시 (클릭 시 숨김)'}>
              <input type="checkbox" checked={!r.hidden} onChange={() => toggleHidden(i)} />
              <span className="toggle-track" />
            </label>
          </div>
        ))}
      </div>
      <div className="modal-footer">
        <button className="btn btn--outline" onClick={resetDefault} disabled={saving}
                style={{ marginRight: 'auto' }}>기본값</button>
        <button className="btn btn--outline" onClick={onClose} disabled={saving}>취소</button>
        <button className="btn btn--primary" onClick={save} disabled={saving}>저장</button>
      </div>
    </Modal>
  )
}
