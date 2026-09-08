import { ChevronDown, ChevronRight, Plus } from 'lucide-react'
import { useConfirm } from '../components/custom/confirm'
import { useCallback, useEffect, useMemo, useState } from 'react'
import { deploymentApi, type SipPackage, type Deployment } from '../api/deployment'
import { useToast } from '../components/Toast'
import PackageUploadModal from './deploy/PackageUploadModal'
import { fmtSize, fmtRelTime, depEffectiveStatus } from './deploy/deployHelpers'
import { agentDisplayName } from '../components/agentDisplay'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { DataTable, Th, Td } from '@core/components/custom/data-table'
import { Badge } from '@core/components/ui/badge'
import { EmptyState } from '@core/components/custom/empty-state'
import Modal from '@core/components/Modal'

interface ModuleGroup {
 name: string
 versions: SipPackage[]
 latest: SipPackage
 totalSize: number
 lastUploadedAt: string | null
}

export default function PackagesPage() {
 const { show } = useToast()
 const confirm = useConfirm()
 const [packages, setPackages] = useState<SipPackage[]>([])
 const [deployments, setDeployments] = useState<Deployment[]>([])
 const [loading, setLoading] = useState(true)
 const [selected, setSelected] = useState<string | null>(null)
 const [filter, setFilter] = useState('')
 const [uploadOpen, setUploadOpen] = useState(false)

 const load = useCallback(async () => {
 try {
 const [p, d] = await Promise.all([
 deploymentApi.listPackages(),
 deploymentApi.listDeployments(),
      ])
 setPackages(p); setDeployments(d)
    } catch (e) { show((e as Error).message, 'err') }
 finally { setLoading(false) }
  }, [show])

 useEffect(() => { void load() }, [load])
 useEffect(() => {
 const iv = setInterval(() => void load(), 2_000)   // 실측 상태 자동 갱신 (제어 탭과 일관)
 return () => clearInterval(iv)
  }, [load])

 const modules = useMemo<ModuleGroup[]>(() => {
 const m = new Map<string, SipPackage[]>()
 for (const p of packages) {
 if (!m.has(p.name)) m.set(p.name, [])
 m.get(p.name)!.push(p)
    }
 const out: ModuleGroup[] = []
 for (const [name, vers] of m) {
 const sorted = [...vers].sort((a, b) => {
        // 최신순: uploaded_at 내림차순 → id 내림차순
 const ta = a.uploaded_at ? Date.parse(a.uploaded_at) : 0
 const tb = b.uploaded_at ? Date.parse(b.uploaded_at) : 0
 if (tb !== ta) return tb - ta
 return b.id - a.id
      })
 out.push({
 name,
 versions: sorted,
 latest: sorted[0],
 totalSize: sorted.reduce((s, v) => s + (v.file_size || 0), 0),
 lastUploadedAt: sorted[0].uploaded_at,
      })
    }
 return out.sort((a, b) => a.name.localeCompare(b.name))
  }, [packages])

 const filteredModules = useMemo(() => {
 const q = filter.trim().toLowerCase()
 if (!q) return modules
 return modules.filter(m => m.name.toLowerCase().includes(q))
  }, [modules, filter])

  // 선택된 모듈 자동 보정 (삭제/로드 후)
 useEffect(() => {
 if (loading) return
 if (modules.length === 0) { setSelected(null); return }
 if (!selected || !modules.find(m => m.name === selected)) {
 setSelected(modules[0].name)
    }
  }, [modules, selected, loading])

 const selectedModule = useMemo(
    () => modules.find(m => m.name === selected) || null,
 [modules, selected]
  )

  // 패키지별 배포 참조 수
 const depCountByPkgId = useMemo(() => {
 const m = new Map<number, number>()
 for (const d of deployments) {
 m.set(d.package_id, (m.get(d.package_id) || 0) + 1)
    }
 return m
  }, [deployments])

 async function removePackage(p: SipPackage) {
 const refs = depCountByPkgId.get(p.id) || 0
 if (refs > 0) {
 if (!await confirm({ title: '패키지 삭제', tone: 'danger', confirmLabel: '삭제',
 body: `${p.name} v${p.version} 은 ${refs}곳에 배포되어 있습니다. 계속 삭제할까요?` })) return
    } else {
 if (!await confirm({ title: '패키지 삭제', tone: 'danger', confirmLabel: '삭제',
 body: `${p.name} v${p.version} 을 삭제할까요?` })) return
    }
 try {
 await deploymentApi.deletePackage(p.id)
 show('삭제됨', 'ok')
 await load()
    } catch (e) { show((e as Error).message, 'err') }
  }

 if (loading) return <div className="flex min-h-0 flex-1 items-center justify-center p-8 text-center text-muted-foreground">로딩 중...</div>

 return (
    <div className="flex gap-4 flex-1 min-h-0">
      {/* ── 좌측: 모듈 목록 ── */}
      <div className="w-[280px] flex-none flex flex-col border border-border rounded-sm bg-card overflow-hidden">
        <div className="p-2.5 border-b border-border flex gap-1.5">
          <Input className="flex-1" placeholder="모듈 검색..."
 value={filter} onChange={e => setFilter(e.target.value)}/>
        </div>
        <div className="flex-1 overflow-auto">
          {filteredModules.length === 0 ? (
            <EmptyState title={modules.length === 0 ? '등록된 모듈 없음' : '검색 결과 없음'} className="p-[20px]" />
          ) : (
 filteredModules.map(m => (
              <ModuleRow key={m.name} mod={m}
 active={m.name === selected}
 onClick={() => setSelected(m.name)} />
            ))
          )}
        </div>
        <div className="p-2.5 border-t border-border">
          <Button className="w-full" variant="default" size="default"
 onClick={() => setUploadOpen(true)}><Plus size={13} /> 패키지 업로드</Button>
        </div>
      </div>

      {/* ── 우측: 선택 모듈 상세 ── */}
      <div className="flex-1 flex flex-col overflow-hidden border border-border rounded-sm bg-card">
        {!selectedModule ? (
          <EmptyState title="좌측에서 모듈을 선택하거나, 새 패키지를 업로드하세요" className="p-[40px]" />
        ) : (
          <ModuleDetail mod={selectedModule}
 depCountByPkgId={depCountByPkgId}
 deployments={deployments}
 onDelete={removePackage} />
        )}
      </div>

      {uploadOpen &&
        <PackageUploadModal onClose={() => setUploadOpen(false)} onDone={load} />}
    </div>
  )
}

// ──────────────────────────────────────────────────────────────

function ModuleRow({ mod, active, onClick }: {
 mod: ModuleGroup; active: boolean; onClick: () => void
}) {
 return (
    <button onClick={onClick}
 style={{
 display: 'block', width: '100%', textAlign: 'left',
 padding: '10px 12px', border: 'none', background: active ? 'var(--cims-brand-soft)' : 'transparent',
 borderLeft: `3px solid ${active ? 'var(--cims-info)' : 'transparent'}`,
 cursor: 'pointer', borderBottom: '1px solid var(--border)',
      }}>
      <div className="flex items-baseline gap-1.5">
        <b className="text-base">{mod.name}</b>
        <span className="ml-auto text-xs text-muted-foreground bg-secondary py-px px-1.5 rounded-[10px]">{mod.versions.length}</span>
      </div>
      <div className="text-xs text-muted-foreground mt-[3px]">
        최신 v{mod.latest.version} · {fmtRelTime(mod.lastUploadedAt)}
      </div>
    </button>
  )
}

function ModuleDetail({ mod, depCountByPkgId, deployments, onDelete }: {
 mod: ModuleGroup
 depCountByPkgId: Map<number, number>
 deployments: Deployment[]
 onDelete: (p: SipPackage) => void
}) {
 const [expanded, setExpanded] = useState<Set<number>>(() => new Set([mod.latest.id]))
 const [depViewFor, setDepViewFor] = useState<SipPackage | null>(null)

  // 선택 모듈 바뀌면 최신만 펼치게 초기화
 useEffect(() => {
 setExpanded(new Set([mod.latest.id]))
  }, [mod.name, mod.latest.id])

 function toggle(id: number) {
 setExpanded(s => {
 const n = new Set(s)
 if (n.has(id)) n.delete(id); else n.add(id)
 return n
    })
  }

 return (
    <>
      {/* 헤더 */}
      <div className="p-4 border-b border-border">
        <div className="flex items-baseline gap-2.5">
          <h3 className="m-0">{mod.name}</h3>
          <span className="text-muted-foreground text-md">({mod.versions.length}개 버전)</span>
          <span className="ml-auto text-muted-foreground text-sm">
            총 {fmtSize(mod.totalSize)}
          </span>
        </div>
        {mod.latest.description && (
          <div className="mt-1.5 text-sm text-muted-foreground">
            {mod.latest.description}
          </div>
        )}
      </div>

      {/* 버전 리스트 */}
      <div className="flex-1 overflow-auto p-4">
        {mod.versions.map(v => (
          <VersionRow key={v.id} pkg={v}
 isLatest={v.id === mod.latest.id}
 expanded={expanded.has(v.id)}
 onToggle={() => toggle(v.id)}
 depCount={depCountByPkgId.get(v.id) || 0}
 onShowDeployments={() => setDepViewFor(v)}
 onDelete={() => onDelete(v)} />
        ))}
      </div>

      {depViewFor && (
        <DeploymentsForPackageModal pkg={depViewFor}
 deployments={deployments.filter(d => d.package_id === depViewFor.id)}
 onClose={() => setDepViewFor(null)} />
      )}
    </>
  )
}

function VersionRow({ pkg: p, isLatest, expanded, onToggle,
 depCount, onShowDeployments, onDelete }: {
 pkg: SipPackage
 isLatest: boolean
 expanded: boolean
 onToggle: () => void
 depCount: number
 onShowDeployments: () => void
 onDelete: () => void
}) {
 return (
    <div style={{
 border: '1px solid var(--border)', borderRadius: 6, marginBottom: 8,
 background: isLatest ? 'var(--muted)' : 'var(--card)',
    }}>
      <div className="flex items-center gap-2.5 py-2.5 px-3.5 cursor-pointer select-none" onClick={onToggle}>
        <span className="text-muted-foreground">
                    {expanded ? <ChevronDown size={12} /> : <ChevronRight size={12} />}</span>
        <b className="text-base">v{p.version}</b>
        {isLatest && (
          <Badge variant="successSolid">최신</Badge>
        )}
        <span className="text-muted-foreground text-sm ml-2">
          {fmtRelTime(p.uploaded_at)}
        </span>
        <span className="text-muted-foreground text-sm">· {fmtSize(p.file_size)}</span>
        {depCount > 0 && (
          <span className="ml-auto text-xs text-primary bg-brandsoft py-0.5 px-2 rounded-[10px]">배포 {depCount}곳</span>
        )}
      </div>

      {expanded && (
        <div className="border-t border-border py-2.5 px-3.5 text-sm text-muted-foreground">
          <div style={{ display: 'grid', gridTemplateColumns: '90px 1fr', rowGap: 4, columnGap: 10 }}>
            <span className="text-muted-foreground">파일</span>
            <code style={{ fontSize: 11, wordBreak: 'break-all' }}>{p.file_path}</code>
            <span className="text-muted-foreground">SHA256</span>
            <code className="text-xs">{p.sha256.substring(0, 32)}…</code>
            <span className="text-muted-foreground">업로드</span>
            <span>
              {p.uploaded_at || '—'}
              {p.uploaded_by && <span className="text-muted-foreground"> · {p.uploaded_by}</span>}
            </span>
            {p.description && <>
              <span className="text-muted-foreground">설명</span>
              <span>{p.description}</span>
            </>}
          </div>

          <div className="mt-2.5 flex gap-1.5">
            <Button
 disabled={depCount === 0}
 onClick={onShowDeployments}
 title={depCount === 0 ? '배포된 곳 없음' : '배포 대상 보기'}>
              배포 대상 보기 ({depCount})
            </Button>
            <Button className="ml-auto" variant="destructive"
 onClick={onDelete}>삭제</Button>
          </div>
        </div>
      )}
    </div>
  )
}

function DeploymentsForPackageModal({ pkg, deployments, onClose }: {
 pkg: SipPackage
 deployments: Deployment[]
 onClose: () => void
}) {
 return (
    <Modal title={`${pkg.name} v${pkg.version} — 배포된 서버`} onClose={onClose} width={640}>
        <div>
          {deployments.length === 0 ? (
            <EmptyState title="배포된 곳 없음" />
          ) : (
            <DataTable sticky>
              <thead>
                <tr>
                  <Th>서버</Th>
                  <Th>서비스</Th>
                  <Th>상태</Th>
                  <Th>배포 시각</Th>
                </tr>
              </thead>
              <tbody>
                {deployments.map(d => (
                  <tr key={d.id}>
                    <Td>
                      {d.agent_name ? agentDisplayName(d.agent_name) : `#${d.agent_id}`}
                      {d.agent_name && agentDisplayName(d.agent_name) !== d.agent_name && (
                        <span className="text-xs text-muted-foreground ml-1.5">({d.agent_name})</span>
                      )}
                    </Td>
                    <Td>{d.process_name || '—'}</Td>
                    <Td>{depEffectiveStatus(d)}</Td>
                    <Td className="text-sm text-muted-foreground">{d.deployed_at || '—'}</Td>
                  </tr>
                ))}
              </tbody>
            </DataTable>
          )}
        </div>
        <div className="flex justify-end gap-2.5 pt-5 mt-4">
          <Button size="default" onClick={onClose}>닫기</Button>
        </div>
    </Modal>
  )
}
