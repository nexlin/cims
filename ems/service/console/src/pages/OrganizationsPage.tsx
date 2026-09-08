import { useConfirm } from '@core/components/custom/confirm'
import React, { useState, useEffect, useCallback } from 'react'
import { ChevronDown, ChevronRight, Dot, Pencil, Plus, RotateCw, Trash2, X } from 'lucide-react'
import IconBtn from '@core/components/IconBtn'
import { orgApi, type Organization, type OrgInput } from '@core/api/organizations'
import { useToast } from '@core/components/Toast'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { NONE, fromSel, toSel } from '@core/components/custom/select-value'
import { DataTable, Th, Td } from '@core/components/custom/data-table'

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
  const confirm = useConfirm()
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
    if (!await confirm({ title: '조직 삭제', tone: 'danger', confirmLabel: '삭제',
      body: '조직을 삭제합니다. 하위 조직은 상위로 이동됩니다.' })) return
    try {
      await orgApi.delete(id)
      show('삭제 완료', 'ok')
      load()
    } catch (e: unknown) { show(String(e), 'err') }
  }

  async function handleBatchDelete() {
    if (selected.size === 0) return
    if (!await confirm({ title: '조직 일괄 삭제', tone: 'danger', confirmLabel: '삭제',
      body: `${selected.size}개 조직을 삭제합니다.` })) return
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
        <Button size="default" onClick={() => setImportOpen(true)}>Excel 가져오기</Button>
        {selected.size > 0 && (
          <Button variant="destructive" size="default" onClick={handleBatchDelete}>
            선택 삭제 ({selected.size}건)
          </Button>
        )}
        <Button variant="ghost" onClick={load} title="새로고침"><RotateCw size={14} /></Button>
      </div>

      {/* 테이블 */}
      {loading ? <div className="flex min-h-0 flex-1 items-center justify-center p-8 text-center text-muted-foreground">로딩 중...</div> : (
        <div className="flex-1 overflow-x-auto">
          <DataTable sticky>
            <thead>
              <tr>
                <Th style={{ width: 36 }}>
                  <input type="checkbox"
                    checked={flat.length > 0 && flat.every(n => selected.has(n.id))}
                    onChange={() => {
                      if (flat.every(n => selected.has(n.id))) setSelected(new Set())
                      else setSelected(new Set(flat.map(n => n.id)))
                    }} />
                </Th>
                <Th>조직명</Th>
                <Th style={{ width: 120 }}>코드</Th>
                <Th style={{ width: 160 }}>상위 조직</Th>
                <Th style={{ width: 80 }}>정렬</Th>
                <Th style={{ width: 120 }}>작업</Th>
              </tr>
            </thead>
            <tbody>
              {flat.length === 0 && !adding ? (
                <tr><Td colSpan={6} className="py-8 text-center text-muted-foreground">조직이 없습니다</Td></tr>
              ) : flat.map(n => {
                const isEditing = editId === n.id
                const hasChildren = n.children.length > 0
                const isExpanded = expanded.has(n.id)
                const addDepth = (n.depth || 0) + 1

                return (
                  <React.Fragment key={n.id}>
                  <tr style={selected.has(n.id) ? { background: 'rgba(74,144,217,0.08)' } : undefined}>
                    <Td onClick={e => e.stopPropagation()}>
                      <input type="checkbox" checked={selected.has(n.id)} onChange={() => toggleSelect(n.id)} />
                    </Td>

                    {/* 조직명 (트리 인덴트) */}
                    <Td>
                      {isEditing ? (
                        <Input  value={editForm.name}
                          onChange={e => setEditForm({ ...editForm, name: e.target.value })}
                          style={{ width: '100%' }} autoFocus />
                      ) : (
                        <div style={{ paddingLeft: n.depth * 20, display: 'flex', alignItems: 'center', gap: 4 }}>
                          <span
                            style={{ width: 18, textAlign: 'center', cursor: hasChildren ? 'pointer' : 'default', userSelect: 'none', fontSize: 11 }}
                            onClick={() => { if (hasChildren) toggleExpand(n.id) }}
                          >
                            {hasChildren
              ? (isExpanded ? <ChevronDown size={12} /> : <ChevronRight size={12} />)
              : <Dot size={12} className="text-muted-foreground" />}
                          </span>
                          <span style={{ fontWeight: 500 }}>{n.name}</span>
                        </div>
                      )}
                    </Td>

                    {/* 코드 */}
                    <Td>
                      {isEditing ? (
                        <Input  value={editForm.code} disabled
                          style={{ width: '100%', opacity: 0.6 }} />
                      ) : (
                        <span className="text-sm text-muted-foreground">{n.code}</span>
                      )}
                    </Td>

                    {/* 상위 조직 */}
                    <Td>
                      {isEditing ? (
                        <Select value={toSel(editForm.parent_id == null ? '' : String(editForm.parent_id))} onValueChange={(v: string) => setEditForm({ ...editForm, parent_id: fromSel(v) ? Number(fromSel(v)) : null })}>
                          <SelectTrigger style={{ width: '100%' }}><SelectValue /></SelectTrigger>
                          <SelectContent>
                            <SelectItem value={NONE}>없음</SelectItem>
                            {parentOptions(n.id).map(o => <SelectItem key={o.id} value={String(o.id)}>{o.name}</SelectItem>)}
                          </SelectContent>
                        </Select>
                      ) : (
                        <span className="text-sm text-muted-foreground">{orgs.find(o => o.id === n.parent_id)?.name || '—'}</span>
                      )}
                    </Td>

                    {/* 정렬 */}
                    <Td>
                      {isEditing ? (
                        <Input  type="number" value={editForm.sort_order}
                          onChange={e => setEditForm({ ...editForm, sort_order: Number(e.target.value) })}
                          style={{ width: '100%' }} />
                      ) : (
                        <span className="text-sm text-muted-foreground">{n.sort_order}</span>
                      )}
                    </Td>

                    {/* 작업 */}
                    <Td className="flex gap-1.5">
                      {isEditing ? (
                        <>
                          <Button variant="default" onClick={saveEdit}>저장</Button>
                          <Button variant="ghost" onClick={cancelEdit}>취소</Button>
                        </>
                      ) : (
                        <>
                          {/* 행마다 빨간 [삭제] 텍스트버튼이 반복돼 시각 소음 — 워크벤치와 동일한 아이콘 버튼으로 통일 */}
                          <IconBtn title="편집" onClick={() => startEdit(n)}><Pencil size={14} /></IconBtn>
                          <IconBtn title="하위 조직 추가" onClick={() => startAdd(n.id, n.id)}><Plus size={14} /></IconBtn>
                          <IconBtn title="삭제" tone="danger" onClick={() => handleDelete(n.id)}><Trash2 size={14} /></IconBtn>
                        </>
                      )}
                    </Td>
                  </tr>
                  {/* 하위 추가 행: 이 행 바로 아래 */}
                  {adding && addAfterId === n.id && (
                    <tr style={{ background: 'rgba(74,144,217,0.08)' }}>
                      <Td></Td>
                      <Td>
                        <div style={{ paddingLeft: addDepth * 20 }}>
                          <Input  placeholder="조직명 *" value={addForm.name}
                            onChange={e => setAddForm({ ...addForm, name: e.target.value })}
                            autoFocus style={{ width: '100%' }} />
                        </div>
                      </Td>
                      <Td><Input  placeholder="코드 *" value={addForm.code}
                        onChange={e => setAddForm({ ...addForm, code: e.target.value })} style={{ width: '100%' }} /></Td>
                      <Td><span className="text-sm text-muted-foreground">{n.name}</span></Td>
                      <Td><Input  type="number" value={addForm.sort_order}
                        onChange={e => setAddForm({ ...addForm, sort_order: Number(e.target.value) })} style={{ width: '100%' }} /></Td>
                      <Td className="flex gap-1.5">
                        <Button variant="default" onClick={saveAdd}>저장</Button>
                        <Button variant="ghost" onClick={cancelAdd}>취소</Button>
                      </Td>
                    </tr>
                  )}
                  </React.Fragment>
                )
              })}

              {/* 맨 아래: 최상위 추가 행 */}
              {adding && addAfterId === null ? (
                <tr style={{ background: 'rgba(74,144,217,0.08)' }}>
                  <Td></Td>
                  <Td><Input  placeholder="조직명 *" value={addForm.name}
                    onChange={e => setAddForm({ ...addForm, name: e.target.value })}
                    autoFocus style={{ width: '100%' }} /></Td>
                  <Td><Input  placeholder="코드 *" value={addForm.code}
                    onChange={e => setAddForm({ ...addForm, code: e.target.value })} style={{ width: '100%' }} /></Td>
                  <Td>
                    <Select value={toSel(addForm.parent_id == null ? '' : String(addForm.parent_id))} onValueChange={(v: string) => setAddForm({ ...addForm, parent_id: fromSel(v) ? Number(fromSel(v)) : null })}>
                      <SelectTrigger style={{ width: '100%' }}><SelectValue /></SelectTrigger>
                      <SelectContent>
                        <SelectItem value={NONE}>없음 (최상위)</SelectItem>
                        {orgs.map(o => <SelectItem key={o.id} value={String(o.id)}>{o.name}</SelectItem>)}
                      </SelectContent>
                    </Select>
                  </Td>
                  <Td><Input  type="number" value={addForm.sort_order}
                    onChange={e => setAddForm({ ...addForm, sort_order: Number(e.target.value) })} style={{ width: '100%' }} /></Td>
                  <Td className="flex gap-1.5">
                    <Button variant="default" onClick={saveAdd}>저장</Button>
                    <Button variant="ghost" onClick={cancelAdd}>취소</Button>
                  </Td>
                </tr>
              ) : !adding && (
                <tr>
                  <Td colSpan={6} style={{ textAlign: 'center' }}>
                    <Button variant="ghost" onClick={() => startAdd(null, null)}
                      style={{ color: 'var(--primary)', fontSize: 12 }}>
                      <Plus size={13} /> 조직 추가
                    </Button>
                  </Td>
                </tr>
              )}
            </tbody>
          </DataTable>
        </div>
      )}

      {/* Excel Import 모달 */}
      {importOpen && (
        <div className="modal-overlay" onClick={() => { setImportOpen(false); setImportResult(null) }}>
          <div className="modal-box" onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">조직 Excel 가져오기</span>
              <button className="modal-close" aria-label="닫기"
              onClick={() => { setImportOpen(false); setImportResult(null) }}><X size={16} /></button>
            </div>
            <div className="modal-body">
              <p style={{ marginBottom: 12 }}>조직 계층을 Excel(.xlsx)로 일괄 등록합니다.</p>
              <div style={{ display: 'flex', gap: 12, alignItems: 'center', marginBottom: 16 }}>
                <Button asChild variant="default" size="default">
                  <label className="cursor-pointer">
                    파일 선택
                    <input type="file" accept=".xlsx" onChange={handleImport} style={{ display: 'none' }} />
                  </label>
                </Button>
                <Button asChild size="default"><a href={orgApi.templateUrl} download>템플릿 다운로드</a></Button>
                {importLoading && <span className="text-sm text-muted-foreground">처리 중...</span>}
              </div>
              {importResult && (
                <div style={{ background: 'var(--card)', borderRadius: 8, padding: 16 }}>
                  <div style={{ fontWeight: 600, marginBottom: 8 }}>결과</div>
                  <div style={{ fontSize: 14 }}>생성: <strong>{importResult.created}</strong>건, 수정: <strong>{importResult.updated}</strong>건</div>
                  {importResult.errors.length > 0 && (
                    <div style={{ marginTop: 8, color: 'var(--destructive)', fontSize: 12 }}>
                      {importResult.errors.map((e, i) => <div key={i}>행 {e.row}: {e.error}</div>)}
                    </div>
                  )}
                </div>
              )}
            </div>
            <div className="modal-footer">
              <Button variant="ghost" size="default" onClick={() => { setImportOpen(false); setImportResult(null) }}>닫기</Button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
