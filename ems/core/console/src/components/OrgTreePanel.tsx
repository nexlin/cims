import { useState, useEffect, useCallback } from 'react'
import { orgApi, type Organization } from '../api/organizations'
import { ChevronDown, ChevronRight, Dot } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { cn } from '@core/lib/utils'

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
  /** 호출부가 폭을 정한다 — 화면마다 좌측 트리 폭이 다르다. */
  className?: string
  /** true = 부모 flex 높이를 가득 채움(워크벤치). false(기본) = maxHeight 500 박스. */
  fill?: boolean
}

export default function OrgTreePanel({ selectedPath, onSelect, style, className, fill }: OrgTreePanelProps) {
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
    // 기본 폭은 **클래스로** 둔다 — 인라인 style 에 두면 호출부가 className 으로 넘기는 폭이
    // 항상 진다(twMerge 는 클래스끼리만 판정한다). 실제로 200px 지정이 150px 로 죽었다.
    <div className={cn('panel min-w-[150px] w-[150px] max-w-[180px]', fill && 'h-full', className)}
         style={style}>
      <div className="flex items-center justify-between border-b border-border bg-muted px-4 py-3">
        <span className="font-semibold">조직</span>
        <Button className="text-xs" variant="ghost"
          onClick={() => { onSelect(null, '전체'); }}>전체</Button>
      </div>
      <div style={{ overflowY: 'auto', ...(fill ? { flex: 1, minHeight: 0 } : { maxHeight: 500 }) }}>
        {flat.map(n => {
          const hasChildren = n.children.length > 0
          const isExpanded = expanded.has(n.id)
          const isSelected = selectedPath === (n.code_path || n.code)
          return (
            <div key={n.id}
              style={{
                display: 'flex', alignItems: 'center', gap: 4,
                paddingLeft: 8 + n.depth * 16, paddingRight: 8, paddingTop: 5, paddingBottom: 5,
                background: isSelected ? 'var(--cims-brand-soft)' : undefined,
                cursor: 'pointer', fontSize: 12,
              }}
              onClick={() => onSelect(n.code_path || n.code, n.name)}
            >
              <span style={{ width: 14, textAlign: 'center', cursor: hasChildren ? 'pointer' : 'default', userSelect: 'none', fontSize: 10 }}
                onClick={e => { e.stopPropagation(); if (hasChildren) toggleExpand(n.id) }}>
                {hasChildren
                ? (isExpanded ? <ChevronDown size={12} /> : <ChevronRight size={12} />)
                : <Dot size={12} className="text-muted-foreground" />}
              </span>
              <span style={{ fontWeight: isSelected ? 600 : 400 }}>{n.name}</span>
            </div>
          )
        })}
        {flat.length === 0 && <div className="p-3 text-sm text-muted-foreground">조직 없음</div>}
      </div>
    </div>
  )
}
