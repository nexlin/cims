import React, { useState, useEffect, useCallback } from 'react'
import { orgApi, type Organization, type OrgInput } from '../../../api/organizations'
import { useToast } from '../../../components/Toast'

// ── 트리 빌더 ───────────────────────────────────────────────
interface TreeNode extends Organization {
  children: TreeNode[]
  depth: number
}

function buildTree(orgs: Organization[]): TreeNode[] {
  const map = new Map<number, TreeNode>()
  orgs.forEach(o => map.set(o.id, { ...o, children: [], depth: 0 }))
  const roots: TreeNode[] = []
  map.forEach(node => {
    if (node.parent_id && map.has(node.parent_id)) {
      map.get(node.parent_id)!.children.push(node)
    } else {
      roots.push(node)
    }
  })
  function setDepth(nodes: TreeNode[], d: number) {
    nodes.forEach(n => { n.depth = d; setDepth(n.children, d + 1) })
  }
  setDepth(roots, 0)
  return roots
}

function flattenTree(nodes: TreeNode[], expanded: Set<number>): TreeNode[] {
  const result: TreeNode[] = []
  function walk(list: TreeNode[]) {
    list.sort((a, b) => a.sort_order - b.sort_order || a.name.localeCompare(b.name))
    list.forEach(n => {
      result.push(n)
      if (expanded.has(n.id) && n.children.length > 0) walk(n.children)
    })
  }
  walk(nodes)
  return result
}

// ── 메인 ────────────────────────────────────────────────────
export default function OrganizationsPage() {
  const { show } = useToast()
  const [orgs, setOrgs] = useState<Organization[]>([])
  const [loading, setLoading] = useState(true)
  const [expanded, setExpanded] = useState<Set<number>>(new Set())

  // 인라인 편집
  const [editId, setEditId] = useState<number | null>(null)
  const [editForm, setEditForm] = useState<OrgInput>({ code: '', name: '', parent_id: null, sort_order: 0 })

  // 신규 추가 행: afterId = 어느 행 아래에 삽입할지 (null = 맨 아래)
  const [adding, setAdding] = useState(false)
  const [addAfterId, setAddAfterId] = useState<number | null>(null)
  const [addForm, setAddForm] = useState<OrgInput>({ code: '', name: '', parent_id: null, sort_order: 0 })

  // 다중 선택
  const [selected, setSelected] = useState<Set<number>>(new Set())

  // Excel import
  const [importOpen, setImportOpen] = useState(false)
  const [importResult, setImportResult] = useState<{created:number, updated:number, errors:Array<{row:number,error:string}>} | null>(null)
  const [importLoading, setImportLoading] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const data = await orgApi.list()
      setOrgs(data)
      // 최초 로드 시 전체 확장
      setExpanded(new Set(data.map(o => o.id)))
    }
    catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [show])

  useEffect(() => { load() }, [load])

  const tree = buildTree(orgs)
  const flat = flattenTree(tree, expanded)

  function toggleExpand(id: number) {
    setExpanded(prev => {
      const next = new Set(prev)
      if (next.has(id)) next.delete(id); else next.add(id)
      return next
    })
  }

  function toggleSelect(id: number) {
    setSelected(prev => { const n = new Set(prev); if (n.has(id)) n.delete(id); else n.add(id); return n })
  }

  // ── 인라인 편집 ──
  function startEdit(o: Organization) {
    setEditId(o.id)
    setEditForm({ code: o.code, name: o.name, parent_id: o.parent_id, sort_order: o.sort_order })
    setAdding(false)
  }

  function cancelEdit() { setEditId(null) }

  async function saveEdit() {
    if (!editId) return
    try {
      await orgApi.update(editId, editForm)
      show('수정 완료', 'ok')
      setEditId(null)
      load()
    } catch (e: unknown) { show(String(e), 'err') }
  }

  // ── 신규 추가 ──
  function startAdd(parentId: number | null = null, afterId: number | null = null) {
    setAdding(true)
    setAddAfterId(afterId)
    setAddForm({ code: '', name: '', parent_id: parentId, sort_order: 0 })
    setEditId(null)
  }

  function cancelAdd() { setAdding(false); setAddAfterId(null) }

  async function saveAdd() {
    if (!addForm.code || !addForm.name) { show('코드와 이름은 필수입니다', 'err'); return }
    try {
      await orgApi.create(addForm)
      show('생성 완료', 'ok')
      setAdding(false)
      load()
    } catch (e: unknown) { show(String(e), 'err') }
  }

  // ── 삭제 ──
  async function handleDelete(id: number) {
    if (!confirm('조직을 삭제합니다. 하위 조직은 상위로 이동됩니다.')) return
    try {
      await orgApi.delete(id)
      show('삭제 완료', 'ok')
      load()
    } catch (e: unknown) { show(String(e), 'err') }
  }

  async function handleBatchDelete() {
    if (selected.size === 0) return
    if (!confirm(`${selected.size}개 조직을 삭제합니다.`)) return
    try {
      const r = await orgApi.batchDelete(Array.from(selected))
      show(`${r.deleted}건 삭제`, 'ok')
      setSelected(new Set())
      load()
    } catch (e: unknown) { show(String(e), 'err') }
  }

  // ── Excel ──
  async function handleImport(e: React.ChangeEvent<HTMLInputElement>) {
    const file = e.target.files?.[0]
    if (!file) return
    setImportLoading(true); setImportResult(null)
    try {
      const buf = await file.arrayBuffer()
      const b64 = btoa(String.fromCharCode(...new Uint8Array(buf)))
      const r = await orgApi.importExcel(b64)
      setImportResult(r)
      if (r.created + r.updated > 0) load()
    } catch (err: unknown) { show(String(err), 'err') }
    finally { setImportLoading(false); e.target.value = '' }
  }

  // 부모 선택 옵션
  function parentOptions(excludeId?: number) {
    return orgs.filter(o => o.id !== excludeId)
  }

  return (
    <div className="page">
      {/* 툴바 */}
      <div className="toolbar">
        <button className="btn btn--outline" onClick={() => setImportOpen(true)}>Excel 가져오기</button>
        {selected.size > 0 && (
          <button className="btn btn--danger" onClick={handleBatchDelete}>
            선택 삭제 ({selected.size}건)
          </button>
        )}
        <button className="btn btn--ghost btn--sm" onClick={load}>↻</button>
      </div>

      {/* 테이블 */}
      {loading ? <div className="empty">로딩 중...</div> : (
        <div className="table-wrap">
          <table className="data-table">
            <thead>
              <tr>
                <th style={{ width: 36 }}>
                  <input type="checkbox"
                    checked={flat.length > 0 && flat.every(n => selected.has(n.id))}
                    onChange={() => {
                      if (flat.every(n => selected.has(n.id))) setSelected(new Set())
                      else setSelected(new Set(flat.map(n => n.id)))
                    }} />
                </th>
                <th>조직명</th>
                <th style={{ width: 120 }}>코드</th>
                <th style={{ width: 160 }}>상위 조직</th>
                <th style={{ width: 80 }}>정렬</th>
                <th style={{ width: 120 }}>작업</th>
              </tr>
            </thead>
            <tbody>
              {flat.length === 0 && !adding ? (
                <tr><td colSpan={6} className="empty-cell">조직이 없습니다</td></tr>
              ) : flat.map(n => {
                const isEditing = editId === n.id
                const hasChildren = n.children.length > 0
                const isExpanded = expanded.has(n.id)
                const addDepth = (n.depth || 0) + 1

                return (
                  <React.Fragment key={n.id}>
                  <tr style={selected.has(n.id) ? { background: 'rgba(74,144,217,0.08)' } : undefined}>
                    <td onClick={e => e.stopPropagation()}>
                      <input type="checkbox" checked={selected.has(n.id)} onChange={() => toggleSelect(n.id)} />
                    </td>

                    {/* 조직명 (트리 인덴트) */}
                    <td>
                      {isEditing ? (
                        <input className="form-input" value={editForm.name}
                          onChange={e => setEditForm({ ...editForm, name: e.target.value })}
                          style={{ width: '100%' }} autoFocus />
                      ) : (
                        <div style={{ paddingLeft: n.depth * 20, display: 'flex', alignItems: 'center', gap: 4 }}>
                          <span
                            style={{ width: 18, textAlign: 'center', cursor: hasChildren ? 'pointer' : 'default', userSelect: 'none', fontSize: 11 }}
                            onClick={() => { if (hasChildren) toggleExpand(n.id) }}
                          >
                            {hasChildren ? (isExpanded ? '▼' : '▶') : '●'}
                          </span>
                          <span style={{ fontWeight: 500 }}>{n.name}</span>
                        </div>
                      )}
                    </td>

                    {/* 코드 */}
                    <td>
                      {isEditing ? (
                        <input className="form-input" value={editForm.code} disabled
                          style={{ width: '100%', opacity: 0.6 }} />
                      ) : (
                        <span className="ts">{n.code}</span>
                      )}
                    </td>

                    {/* 상위 조직 */}
                    <td>
                      {isEditing ? (
                        <select className="form-input" value={editForm.parent_id ?? ''}
                          onChange={e => setEditForm({ ...editForm, parent_id: e.target.value ? Number(e.target.value) : null })}
                          style={{ width: '100%' }}>
                          <option value="">없음</option>
                          {parentOptions(n.id).map(o => <option key={o.id} value={o.id}>{o.name}</option>)}
                        </select>
                      ) : (
                        <span className="ts">{orgs.find(o => o.id === n.parent_id)?.name || '—'}</span>
                      )}
                    </td>

                    {/* 정렬 */}
                    <td>
                      {isEditing ? (
                        <input className="form-input" type="number" value={editForm.sort_order}
                          onChange={e => setEditForm({ ...editForm, sort_order: Number(e.target.value) })}
                          style={{ width: '100%' }} />
                      ) : (
                        <span className="ts">{n.sort_order}</span>
                      )}
                    </td>

                    {/* 작업 */}
                    <td className="actions">
                      {isEditing ? (
                        <>
                          <button className="btn btn--sm btn--primary" onClick={saveEdit}>저장</button>
                          <button className="btn btn--sm btn--ghost" onClick={cancelEdit}>취소</button>
                        </>
                      ) : (
                        <>
                          <button className="btn btn--sm btn--outline" onClick={() => startEdit(n)}>편집</button>
                          <button className="btn btn--sm btn--outline" onClick={() => startAdd(n.id, n.id)} title="하위 추가">＋</button>
                          <button className="btn btn--sm btn--danger" onClick={() => handleDelete(n.id)}>삭제</button>
                        </>
                      )}
                    </td>
                  </tr>
                  {/* 하위 추가 행: 이 행 바로 아래 */}
                  {adding && addAfterId === n.id && (
                    <tr style={{ background: 'rgba(74,144,217,0.08)' }}>
                      <td></td>
                      <td>
                        <div style={{ paddingLeft: addDepth * 20 }}>
                          <input className="form-input" placeholder="조직명 *" value={addForm.name}
                            onChange={e => setAddForm({ ...addForm, name: e.target.value })}
                            autoFocus style={{ width: '100%' }} />
                        </div>
                      </td>
                      <td><input className="form-input" placeholder="코드 *" value={addForm.code}
                        onChange={e => setAddForm({ ...addForm, code: e.target.value })} style={{ width: '100%' }} /></td>
                      <td><span className="ts">{n.name}</span></td>
                      <td><input className="form-input" type="number" value={addForm.sort_order}
                        onChange={e => setAddForm({ ...addForm, sort_order: Number(e.target.value) })} style={{ width: '100%' }} /></td>
                      <td className="actions">
                        <button className="btn btn--sm btn--primary" onClick={saveAdd}>저장</button>
                        <button className="btn btn--sm btn--ghost" onClick={cancelAdd}>취소</button>
                      </td>
                    </tr>
                  )}
                  </React.Fragment>
                )
              })}

              {/* 맨 아래: 최상위 추가 행 */}
              {adding && addAfterId === null ? (
                <tr style={{ background: 'rgba(74,144,217,0.08)' }}>
                  <td></td>
                  <td><input className="form-input" placeholder="조직명 *" value={addForm.name}
                    onChange={e => setAddForm({ ...addForm, name: e.target.value })}
                    autoFocus style={{ width: '100%' }} /></td>
                  <td><input className="form-input" placeholder="코드 *" value={addForm.code}
                    onChange={e => setAddForm({ ...addForm, code: e.target.value })} style={{ width: '100%' }} /></td>
                  <td>
                    <select className="form-input" value={addForm.parent_id ?? ''}
                      onChange={e => setAddForm({ ...addForm, parent_id: e.target.value ? Number(e.target.value) : null })}
                      style={{ width: '100%' }}>
                      <option value="">없음 (최상위)</option>
                      {orgs.map(o => <option key={o.id} value={o.id}>{o.name}</option>)}
                    </select>
                  </td>
                  <td><input className="form-input" type="number" value={addForm.sort_order}
                    onChange={e => setAddForm({ ...addForm, sort_order: Number(e.target.value) })} style={{ width: '100%' }} /></td>
                  <td className="actions">
                    <button className="btn btn--sm btn--primary" onClick={saveAdd}>저장</button>
                    <button className="btn btn--sm btn--ghost" onClick={cancelAdd}>취소</button>
                  </td>
                </tr>
              ) : !adding && (
                <tr>
                  <td colSpan={6} style={{ textAlign: 'center' }}>
                    <button className="btn btn--ghost btn--sm" onClick={() => startAdd(null, null)}
                      style={{ color: 'var(--primary)', fontSize: 12 }}>
                      ＋ 조직 추가
                    </button>
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      )}

      {/* Excel Import 모달 */}
      {importOpen && (
        <div className="modal-overlay" onClick={() => { setImportOpen(false); setImportResult(null) }}>
          <div className="modal-box" onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">조직 Excel 가져오기</span>
              <button className="modal-close" onClick={() => { setImportOpen(false); setImportResult(null) }}>✕</button>
            </div>
            <div className="modal-body">
              <p style={{ marginBottom: 12 }}>조직 계층을 Excel(.xlsx)로 일괄 등록합니다.</p>
              <div style={{ display: 'flex', gap: 12, alignItems: 'center', marginBottom: 16 }}>
                <label className="btn btn--primary" style={{ cursor: 'pointer' }}>
                  파일 선택
                  <input type="file" accept=".xlsx" onChange={handleImport} style={{ display: 'none' }} />
                </label>
                <a href={orgApi.templateUrl} className="btn btn--outline" download>템플릿 다운로드</a>
                {importLoading && <span className="ts">처리 중...</span>}
              </div>
              {importResult && (
                <div style={{ background: 'var(--surface)', borderRadius: 8, padding: 16 }}>
                  <div style={{ fontWeight: 600, marginBottom: 8 }}>결과</div>
                  <div style={{ fontSize: 14 }}>생성: <strong>{importResult.created}</strong>건, 수정: <strong>{importResult.updated}</strong>건</div>
                  {importResult.errors.length > 0 && (
                    <div style={{ marginTop: 8, color: 'var(--danger)', fontSize: 12 }}>
                      {importResult.errors.map((e, i) => <div key={i}>행 {e.row}: {e.error}</div>)}
                    </div>
                  )}
                </div>
              )}
            </div>
            <div className="modal-footer">
              <button className="btn btn--ghost" onClick={() => { setImportOpen(false); setImportResult(null) }}>닫기</button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
