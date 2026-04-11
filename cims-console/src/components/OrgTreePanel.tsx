import { useState, useEffect, useCallback } from 'react'
import { orgApi, type Organization } from '../api/organizations'

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
    list.forEach(n => { result.push(n); if (expanded.has(n.id)) walk(n.children) })
  }
  walk(nodes)
  return result
}

interface OrgTreePanelProps {
  /** code_path로 필터 (startsWith 비교) */
  selectedPath: string | null
  onSelect: (codePath: string | null, name: string) => void
  style?: React.CSSProperties
}

export default function OrgTreePanel({ selectedPath, onSelect, style }: OrgTreePanelProps) {
  const [orgs, setOrgs] = useState<Organization[]>([])
  const [expanded, setExpanded] = useState<Set<number>>(new Set())

  const load = useCallback(async () => {
    try {
      const data = await orgApi.list()
      setOrgs(data)
      setExpanded(new Set(data.map(o => o.id)))
    } catch { /* ignore */ }
  }, [])

  useEffect(() => { load() }, [load])

  const tree = buildTree(orgs)
  const flat = flattenTree(tree, expanded)

  function toggleExpand(id: number) {
    setExpanded(prev => { const n = new Set(prev); if (n.has(id)) n.delete(id); else n.add(id); return n })
  }

  return (
    <div className="panel" style={{ minWidth: 150, maxWidth: 180, width: 150, ...style }}>
      <div style={{ padding: '10px 12px', fontWeight: 600, fontSize: 13, borderBottom: '1px solid var(--border)', display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
        <span>조직</span>
        <button className="btn btn--ghost btn--sm" style={{ fontSize: 11 }}
          onClick={() => { onSelect(null, '전체'); }}>전체</button>
      </div>
      <div style={{ maxHeight: 500, overflowY: 'auto' }}>
        {flat.map(n => {
          const hasChildren = n.children.length > 0
          const isExpanded = expanded.has(n.id)
          const isSelected = selectedPath === (n.code_path || n.code)
          return (
            <div key={n.id}
              style={{
                display: 'flex', alignItems: 'center', gap: 4,
                paddingLeft: 8 + n.depth * 16, paddingRight: 8, paddingTop: 5, paddingBottom: 5,
                background: isSelected ? 'rgba(74,144,217,0.15)' : undefined,
                cursor: 'pointer', fontSize: 12,
              }}
              onClick={() => onSelect(n.code_path || n.code, n.name)}
            >
              <span style={{ width: 14, textAlign: 'center', cursor: hasChildren ? 'pointer' : 'default', userSelect: 'none', fontSize: 10 }}
                onClick={e => { e.stopPropagation(); if (hasChildren) toggleExpand(n.id) }}>
                {hasChildren ? (isExpanded ? '▼' : '▶') : '●'}
              </span>
              <span style={{ fontWeight: isSelected ? 600 : 400 }}>{n.name}</span>
            </div>
          )
        })}
        {flat.length === 0 && <div style={{ padding: 12, fontSize: 12, color: 'var(--text-muted)' }}>조직 없음</div>}
      </div>
    </div>
  )
}
