import { useCallback, useEffect, useMemo, useState } from 'react'
import { deploymentApi, type SipPackage, type Deployment } from '../api/deployment'
import { useToast } from '../components/Toast'
import PackageUploadModal from './deploy/PackageUploadModal'
import { fmtSize, fmtRelTime, depEffectiveStatus } from './deploy/deployHelpers'
import { agentDisplayName } from '../components/agentDisplay'

interface ModuleGroup {
  name: string
  versions: SipPackage[]
  latest: SipPackage
  totalSize: number
  lastUploadedAt: string | null
}

export default function PackagesPage() {
  const { show } = useToast()
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
      if (!confirm(`${p.name} v${p.version} 은 ${refs}곳에 배포되어 있습니다. 계속 삭제할까요?`)) return
    } else {
      if (!confirm(`${p.name} v${p.version} 을 삭제할까요?`)) return
    }
    try {
      await deploymentApi.deletePackage(p.id)
      show('삭제됨', 'ok')
      await load()
    } catch (e) { show((e as Error).message, 'err') }
  }

  if (loading) return <div className="empty">로딩 중...</div>

  return (
    <div style={{ display: 'flex', gap: 16, flex: 1, minHeight: 0 }}>
      {/* ── 좌측: 모듈 목록 ── */}
      <div style={{
        width: 280, flex: '0 0 auto', display: 'flex', flexDirection: 'column',
        border: '1px solid var(--border)', borderRadius: 6, background: 'var(--card)', overflow: 'hidden',
      }}>
        <div style={{ padding: 10, borderBottom: '1px solid var(--border)', display: 'flex', gap: 6 }}>
          <input className="form-input" placeholder="모듈 검색..."
            value={filter} onChange={e => setFilter(e.target.value)}
            style={{ flex: 1 }} />
        </div>
        <div style={{ flex: 1, overflow: 'auto' }}>
          {filteredModules.length === 0 ? (
            <div className="empty" style={{ padding: 20 }}>
              {modules.length === 0 ? '등록된 모듈 없음' : '검색 결과 없음'}
            </div>
          ) : (
            filteredModules.map(m => (
              <ModuleRow key={m.name} mod={m}
                active={m.name === selected}
                onClick={() => setSelected(m.name)} />
            ))
          )}
        </div>
        <div style={{ padding: 10, borderTop: '1px solid var(--border)' }}>
          <button className="btn btn--primary" style={{ width: '100%' }}
            onClick={() => setUploadOpen(true)}>＋ 패키지 업로드</button>
        </div>
      </div>

      {/* ── 우측: 선택 모듈 상세 ── */}
      <div style={{
        flex: 1, display: 'flex', flexDirection: 'column', overflow: 'hidden',
        border: '1px solid var(--border)', borderRadius: 6, background: 'var(--card)',
      }}>
        {!selectedModule ? (
          <div className="empty" style={{ padding: 40 }}>
            좌측에서 모듈을 선택하거나, 새 패키지를 업로드하세요
          </div>
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
        borderLeft: `3px solid ${active ? '#3498db' : 'transparent'}`,
        cursor: 'pointer', borderBottom: '1px solid var(--border)',
      }}>
      <div style={{ display: 'flex', alignItems: 'baseline', gap: 6 }}>
        <b style={{ fontSize: 14 }}>{mod.name}</b>
        <span style={{
          marginLeft: 'auto', fontSize: 11, color: 'var(--muted-foreground)',
          background: 'var(--secondary)', padding: '1px 6px', borderRadius: 10,
        }}>{mod.versions.length}</span>
      </div>
      <div style={{ fontSize: 11, color: 'var(--muted-foreground)', marginTop: 3 }}>
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
      <div style={{ padding: 16, borderBottom: '1px solid var(--border)' }}>
        <div style={{ display: 'flex', alignItems: 'baseline', gap: 10 }}>
          <h3 style={{ margin: 0 }}>{mod.name}</h3>
          <span style={{ color: 'var(--muted-foreground)', fontSize: 13 }}>({mod.versions.length}개 버전)</span>
          <span style={{ marginLeft: 'auto', color: 'var(--muted-foreground)', fontSize: 12 }}>
            총 {fmtSize(mod.totalSize)}
          </span>
        </div>
        {mod.latest.description && (
          <div style={{ marginTop: 6, fontSize: 12, color: 'var(--muted-foreground)' }}>
            {mod.latest.description}
          </div>
        )}
      </div>

      {/* 버전 리스트 */}
      <div style={{ flex: 1, overflow: 'auto', padding: 16 }}>
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
      <div onClick={onToggle}
        style={{
          display: 'flex', alignItems: 'center', gap: 10, padding: '10px 14px',
          cursor: 'pointer', userSelect: 'none',
        }}>
        <span style={{ color: 'var(--muted-foreground)', fontSize: 11 }}>{expanded ? '▾' : '▸'}</span>
        <b style={{ fontSize: 14 }}>v{p.version}</b>
        {isLatest && (
          <span className="tag" style={{
            background: '#15803d', color: '#fff', fontSize: 10,   // 흰 글자 대비 4.5 — 테마 토큰은 다크에서 밝아져 못 쓴다
            padding: '1px 6px', borderRadius: 3,
          }}>최신</span>
        )}
        <span style={{ color: 'var(--muted-foreground)', fontSize: 12, marginLeft: 8 }}>
          {fmtRelTime(p.uploaded_at)}
        </span>
        <span style={{ color: 'var(--muted-foreground)', fontSize: 12 }}>· {fmtSize(p.file_size)}</span>
        {depCount > 0 && (
          <span style={{
            marginLeft: 'auto', fontSize: 11, color: 'var(--primary)',
            background: 'var(--cims-brand-soft)', padding: '2px 8px', borderRadius: 10,
          }}>배포 {depCount}곳</span>
        )}
      </div>

      {expanded && (
        <div style={{ borderTop: '1px solid var(--border)', padding: '10px 14px', fontSize: 12, color: 'var(--muted-foreground)' }}>
          <div style={{ display: 'grid', gridTemplateColumns: '90px 1fr', rowGap: 4, columnGap: 10 }}>
            <span style={{ color: 'var(--muted-foreground)' }}>파일</span>
            <code style={{ fontSize: 11, wordBreak: 'break-all' }}>{p.file_path}</code>
            <span style={{ color: 'var(--muted-foreground)' }}>SHA256</span>
            <code style={{ fontSize: 11 }}>{p.sha256.substring(0, 32)}…</code>
            <span style={{ color: 'var(--muted-foreground)' }}>업로드</span>
            <span>
              {p.uploaded_at || '—'}
              {p.uploaded_by && <span style={{ color: 'var(--muted-foreground)' }}> · {p.uploaded_by}</span>}
            </span>
            {p.description && <>
              <span style={{ color: 'var(--muted-foreground)' }}>설명</span>
              <span>{p.description}</span>
            </>}
          </div>

          <div style={{ marginTop: 10, display: 'flex', gap: 6 }}>
            <button className="btn btn--sm"
              disabled={depCount === 0}
              onClick={onShowDeployments}
              title={depCount === 0 ? '배포된 곳 없음' : '배포 대상 보기'}>
              배포 대상 보기 ({depCount})
            </button>
            <button className="btn btn--sm btn--danger" style={{ marginLeft: 'auto' }}
              onClick={onDelete}>삭제</button>
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
    <div className="modal-overlay" onClick={onClose}>
      <div className="modal-box" style={{ width: 640 }} onClick={e => e.stopPropagation()}>
        <div className="modal-header">
          <span className="modal-title">{pkg.name} v{pkg.version} — 배포된 서버</span>
          <button className="modal-close" onClick={onClose}>✕</button>
        </div>
        <div className="modal-body">
          {deployments.length === 0 ? (
            <div className="empty">배포된 곳 없음</div>
          ) : (
            <table className="data-table">
              <thead>
                <tr>
                  <th>서버</th>
                  <th>서비스</th>
                  <th>상태</th>
                  <th>배포 시각</th>
                </tr>
              </thead>
              <tbody>
                {deployments.map(d => (
                  <tr key={d.id}>
                    <td>
                      {d.agent_name ? agentDisplayName(d.agent_name) : `#${d.agent_id}`}
                      {d.agent_name && agentDisplayName(d.agent_name) !== d.agent_name && (
                        <span style={{ fontSize: 11, color: 'var(--muted-foreground)', marginLeft: 6 }}>({d.agent_name})</span>
                      )}
                    </td>
                    <td>{d.process_name || '—'}</td>
                    <td>{depEffectiveStatus(d)}</td>
                    <td style={{ fontSize: 12, color: 'var(--muted-foreground)' }}>{d.deployed_at || '—'}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </div>
        <div className="modal-footer" style={{ marginTop: 16 }}>
          <button className="btn btn--outline" onClick={onClose}>닫기</button>
        </div>
      </div>
    </div>
  )
}
