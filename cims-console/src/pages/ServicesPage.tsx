import { useState, useEffect, useCallback, useMemo } from 'react'
import { Link } from 'react-router-dom'
import { servicesApi, parseServiceStatus, type ServiceName, type ServiceAction } from '../api/services'
import { deploymentApi, type SipPackage, type ConfigTemplate } from '../api/deployment'
import { buildApi, type BuildJobStatus, type ManifestResponse } from '../api/build'
import { useToast } from '../components/Toast'
import Modal from '../components/Modal'
import ModuleConfigModal from '../components/module/ModuleConfigModal'

type SvcState = { running: boolean; pid?: number }

// 빌드 단위 카드 정의 — csp/cmp 는 같은 dist 에서 변종 (psp/isp/pmp/imp) 까지
// 셋이 묶인 패키지 산출물로 묶임. ▣ 한 번 = 셋 동시 재패키징.
//   key       — 카드 식별자 (소스 트리 매핑 = base dist)
//   variants  — 패키지/프로세스 단위 module 이름 배열 (psp/isp 도 process_name 로 사용)
//   hasProcess — 로컬 프로세스 제어 가능 여부 (false 면 패키지화/다운로드만)
//   critical  — 종료 시 Console 단절 경고
type BuildCard = {
  key: string
  label: string
  variants: ServiceName[]
  hasProcess: boolean
  critical?: boolean
}
const BUILD_CARDS: BuildCard[] = [
  { key: 'csp',     label: 'CSP (VoLTE/PTT/IBCF SIP)', variants: ['csp', 'psp', 'isp'] as ServiceName[], hasProcess: true },
  { key: 'cmp',     label: 'CMP (VoLTE/PTT/IBCF 미디어)', variants: ['cmp', 'pmp', 'imp'] as ServiceName[], hasProcess: true },
  { key: 'cwrtc',   label: 'cwrtc (WebRTC 브리지)',    variants: ['cwrtc'] as ServiceName[], hasProcess: true },
  { key: 'csc',     label: 'CSC (Admin API)',          variants: ['csc'] as ServiceName[], hasProcess: true, critical: true },
  { key: 'console', label: 'Console (웹 UI)',          variants: ['console'] as ServiceName[], hasProcess: true },
  { key: 'phone',   label: 'Phone (웹 단말)',          variants: ['phone'] as ServiceName[], hasProcess: true },
  { key: 'cspsim',  label: 'cspsim (시뮬레이터)',      variants: ['cspsim'] as ServiceName[], hasProcess: false },
  { key: 'agent',   label: 'Agent (원격 관리)',        variants: ['agent'] as ServiceName[], hasProcess: false },
]
// 프로세스 제어 가능한 변종 평탄화 — 일괄 선택/상태 폴링 등에 사용.
const PROCESS_VARIANTS: ServiceName[] = BUILD_CARDS.filter(c => c.hasProcess).flatMap(c => c.variants)

export default function ServicesPage() {
  const { show } = useToast()
  const [states, setStates] = useState<Record<string, SvcState>>({})
  const [loading, setLoading] = useState(true)
  const [busy, setBusy] = useState<Record<string, boolean>>({})
  const [packages, setPackages] = useState<SipPackage[]>([])
  const [selectedVersion, setSelectedVersion] = useState<Record<string, string>>({})
  const [templateModal, setTemplateModal] = useState<{ module: string; pkg: SipPackage } | null>(null)
  const [editing, setEditing] = useState(false)
  const [editText, setEditText] = useState('')
  const [editError, setEditError] = useState('')
  const [saving, setSaving] = useState(false)
  const [configModule, setConfigModule] = useState<string | null>(null)
  const [needsRestart, setNeedsRestart] = useState<Record<string, boolean>>({})
  const [selected, setSelected] = useState<Set<ServiceName>>(new Set())
  // ── 빌드 / 패키지화 ─────────────────────────────────────────
  const [manifest, setManifest] = useState<ManifestResponse | null>(null)
  // activeJob.label 은 "csp" / "csp psp isp" 같이 묶음 표기
  const [activeJob, setActiveJob] = useState<{ id: string; kind: 'build' | 'pkg'; module?: string; cardKey?: string } | null>(null)
  const [jobStatus, setJobStatus] = useState<BuildJobStatus | null>(null)
  // 우측 터미널이 어느 출처를 표시 중인지 — 마지막으로 갱신된 쪽 우선
  const [terminalSource, setTerminalSource] = useState<'job' | 'module' | null>(null)
  const [lastModule, setLastModule] = useState<{ label: string; verdict: 'PASS' | 'FAIL'; returncode: number; output: string } | null>(null)

  const load = useCallback(async () => {
    try {
      const r = await servicesApi.status()
      setStates(parseServiceStatus(r.output))
    } catch (e) {
      show(`상태 조회 실패: ${(e as Error).message}`, 'err')
    } finally {
      setLoading(false)
    }
  }, [show])

  const loadPackages = useCallback(async () => {
    try {
      const items = await deploymentApi.listPackages()
      setPackages(items)
    } catch {
      // 패키지 API 실패해도 기본 동작은 유지 (프로세스 제어만)
    }
  }, [])

  const loadManifest = useCallback(async () => {
    try {
      const m = await buildApi.getManifest()
      setManifest(m)
    } catch {
      setManifest(null)   // manifest 없음 — 패키지화 안 된 상태
    }
  }, [])

  useEffect(() => {
    void load()
    void loadPackages()
    void loadManifest()
    const iv = setInterval(load, 5000)
    return () => clearInterval(iv)
  }, [load, loadPackages, loadManifest])

  // 빌드/패키지 job 폴링 — verification 페이지와 동일 패턴
  useEffect(() => {
    if (!activeJob) return
    let cancelled = false
    const job = activeJob
    ;(async () => {
      while (!cancelled) {
        try {
          const s = await buildApi.getJob(job.id)
          if (cancelled) return
          setJobStatus(s)
          setTerminalSource('job')
          if (s.done) {
            const ok = s.verdict === 'PASS'
            const label = job.kind === 'build' ? '빌드' : `패키지화 (${job.cardKey || job.module || ''})`
            show(`${label} ${ok ? '완료' : '실패'} rc=${s.returncode}`, ok ? 'ok' : 'err')
            setActiveJob(null)
            // jobStatus 는 비우지 않음 — 우측 터미널 패널에 마지막 출력 유지
            await loadManifest()
            return
          }
        } catch {
          if (!cancelled) {
            show(`job ${job.id} 추적 실패`, 'err')
            setActiveJob(null)
            setJobStatus(null)
          }
          return
        }
        await new Promise(res => setTimeout(res, 1500))
      }
    })()
    return () => { cancelled = true }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeJob?.id])

  // 모듈별 manifest tarball 존재 여부 (다운로드 버튼 활성)
  const tarballByModule = useMemo(() => {
    const map: Record<string, { name: string; size: number }> = {}
    for (const p of manifest?.packages || []) {
      const m = /^([a-z]+)-/.exec(p.name)
      if (m) map[m[1]] = { name: p.name, size: p.size }
    }
    return map
  }, [manifest])

  async function startBuild() {
    if (activeJob) {
      show('이미 진행 중인 빌드/패키지 작업이 있습니다.', 'err')
      return
    }
    if (!confirm('전체 빌드를 시작합니다 (cmake + make + npm). 5~15분 소요될 수 있습니다. 계속할까요?')) {
      return
    }
    try {
      const r = await buildApi.runBuild()
      setActiveJob({ id: r.job_id, kind: 'build' })
      show('빌드 시작', 'ok')
    } catch (e) {
      const msg = (e as Error).message
      show(`빌드 시작 실패: ${msg}`, 'err')
    }
  }

  // 카드 단위 패키지화 — csp 카드는 [csp,psp,isp] 묶음, cmp 카드는 [cmp,pmp,imp].
  // 단일 변종 카드 (cwrtc/csc/...) 는 단일 module 호출.
  async function startPkgForCard(card: BuildCard) {
    if (activeJob) {
      show('이미 진행 중인 빌드/패키지 작업이 있습니다.', 'err')
      return
    }
    try {
      const r = await buildApi.runPkg(card.variants.length > 1 ? card.variants : card.variants[0])
      setActiveJob({ id: r.job_id, kind: 'pkg', module: card.variants[0], cardKey: card.key })
      const label = card.variants.length > 1 ? card.variants.join('/') : card.variants[0]
      show(`${label} 패키지화 시작`, 'ok')
    } catch (e) {
      const msg = (e as Error).message
      show(`${card.key} 패키지화 실패: ${msg}`, 'err')
    }
  }

  async function downloadTarball(module: string) {
    try {
      await buildApi.downloadPackage(module)
    } catch (e) {
      show(`${module} 다운로드 실패: ${(e as Error).message}`, 'err')
    }
  }

  function fmtSize(n: number): string {
    if (n < 1024) return `${n} B`
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`
    return `${(n / 1024 / 1024).toFixed(1)} MB`
  }

  // 모듈별로 그룹화된 패키지 목록 (버전 내림차순)
  const packagesByModule = useMemo(() => {
    const map: Record<string, SipPackage[]> = {}
    for (const p of packages) {
      (map[p.name] ||= []).push(p)
    }
    for (const name of Object.keys(map)) {
      map[name].sort((a, b) => b.version.localeCompare(a.version, undefined, { numeric: true }))
    }
    return map
  }, [packages])

  async function act(name: ServiceName, action: ServiceAction, critical: boolean, skipConfirm = false) {
    if (!skipConfirm) {
      if (critical && action !== 'start') {
        const ok = confirm(
          action === 'stop'
            ? `⚠️ CSC 를 중지하면 Console UI 가 즉시 끊깁니다. 계속할까요?`
            : `⚠️ CSC 재시작 중 Console UI 일시 단절됩니다. 계속할까요?`
        )
        if (!ok) return
      } else if (action === 'stop' || action === 'restart') {
        const ok = confirm(`${name} 서비스를 ${action === 'stop' ? '중지' : '재시작'}할까요?`)
        if (!ok) return
      }
    }

    setBusy(b => ({ ...b, [name]: true }))
    try {
      const r = await servicesApi.act(name, action)
      const out = r.stdout + (r.stderr ? `\n[stderr]\n${r.stderr}` : '')
      setLastModule({
        label: `${name} ${action}`,
        verdict: r.returncode === 0 ? 'PASS' : 'FAIL',
        returncode: r.returncode,
        output: out,
      })
      setTerminalSource('module')
      if (r.returncode === 0) {
        show(`${name} ${action} 완료`, 'ok')
        if (action === 'restart' || action === 'start') {
          setNeedsRestart(s => { const n = { ...s }; delete n[name]; return n })
        }
      } else {
        show(`${name} ${action} 실패 rc=${r.returncode}`, 'err')
      }
      setTimeout(() => { void load() }, 1500)
    } catch (e) {
      show((e as Error).message, 'err')
    } finally {
      setBusy(b => ({ ...b, [name]: false }))
    }
  }

  // 프로세스 제어 가능한 변종 (hasProcess=true 카드의 variants) — 체크박스/일괄 대상
  const managedNames = PROCESS_VARIANTS
  // variant 가 critical 카드에 속하는지
  const isCriticalVariant = useCallback(
    (n: ServiceName) => !!BUILD_CARDS.find(c => c.variants.includes(n) && c.critical),
    [],
  )

  function toggleSelect(name: ServiceName) {
    setSelected(prev => {
      const n = new Set(prev)
      if (n.has(name)) n.delete(name); else n.add(name)
      return n
    })
  }
  function toggleSelectAll() {
    setSelected(prev => prev.size === managedNames.length
      ? new Set()
      : new Set(managedNames))
  }

  // 선택 모듈 중 1개라도 실행 중이면 "stop" 모드, 모두 정지면 "start" 모드
  const selectedAnyRunning = useMemo(
    () => managedNames.some(n => selected.has(n) && states[n]?.running),
    [managedNames, selected, states],
  )

  async function bulkAct(action: ServiceAction) {
    const targets = managedNames.filter(n => selected.has(n))
    if (targets.length === 0) {
      show('선택된 모듈이 없습니다.', 'err')
      return
    }
    const hasCritical = targets.some(n => isCriticalVariant(n))
    if ((action === 'stop' || action === 'restart') && hasCritical) {
      const ok = confirm(
        `⚠️ 선택 모듈 ${targets.length}개에 critical(CSC) 가 포함됩니다.\n` +
        `${action === 'stop' ? '정지' : '재시작'} 시 Console UI 가 즉시 단절됩니다.\n계속할까요?`,
      )
      if (!ok) return
    } else if (action === 'stop' || action === 'restart') {
      const ok = confirm(`선택 모듈 ${targets.length}개를 ${action === 'stop' ? '정지' : '재시작'}할까요?\n${targets.join(', ')}`)
      if (!ok) return
    }
    // 직렬 실행 — 동시에 여러 process 띄우면 cims.sh 가 충돌할 수 있음
    for (const name of targets) {
      await act(name, action, false, true /* skipConfirm — 위에서 묶음 처리 */)
    }
  }

  async function toggleRunning(name: ServiceName, running: boolean, critical: boolean) {
    await act(name, running ? 'stop' : 'start', critical)
  }

  function openTemplate(moduleName: string, edit = false) {
    const versions = packagesByModule[moduleName]
    if (!versions || versions.length === 0) { show('등록된 패키지 없음', 'err'); return }
    const v = selectedVersion[moduleName] || versions[0].version
    const pkg = versions.find(p => p.version === v) || versions[0]
    setTemplateModal({ module: moduleName, pkg })
    setEditError('')
    if (edit) {
      setEditText(JSON.stringify(pkg.config_template ?? { version: 1, sections: [] }, null, 2))
      setEditing(true)
    } else {
      setEditing(false)
    }
  }

  function startEdit() {
    if (!templateModal) return
    setEditText(JSON.stringify(templateModal.pkg.config_template ?? { version: 1, sections: [] }, null, 2))
    setEditError('')
    setEditing(true)
  }

  async function saveEdit() {
    if (!templateModal) return
    let parsed: ConfigTemplate
    try {
      parsed = JSON.parse(editText)
    } catch (e) {
      setEditError(`JSON 파싱 실패: ${(e as Error).message}`)
      return
    }
    if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
      setEditError('최상위는 object 여야 합니다')
      return
    }
    setSaving(true)
    try {
      const updated = await deploymentApi.updatePackage(templateModal.pkg.id, { config_template: parsed })
      setTemplateModal({ module: templateModal.module, pkg: updated })
      setEditing(false)
      show('설정 템플릿 저장 완료', 'ok')
      void loadPackages()
    } catch (e) {
      setEditError((e as Error).message)
    } finally {
      setSaving(false)
    }
  }

  const manifestTs = manifest?.ts ? new Date(manifest.ts).toLocaleString() : ''
  const manifestSha = (manifest?._self_sha256 || '').slice(0, 8)
  const manifestGit = manifest?.git?.sha || ''

  return (
    <div>
      <div style={{ marginBottom: 16, display: 'flex', gap: 12, alignItems: 'center', flexWrap: 'wrap' }}>
        <h3 style={{ margin: 0 }}>빌드</h3>
        <span className="text-muted" style={{ fontSize: 13 }}>
          빌드 / 패키지화 / 다운로드 + 모듈 프로세스 제어. 신규 패키지 등록/편집은{' '}
          <Link to="/deploy/packages">배포 &gt; 패키지</Link> 에서.
        </span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 8, alignItems: 'center' }}>
          {manifest ? (
            <span className="tag" style={{ background: '#34495e', color: '#fff', fontSize: 11 }}
                  title={`manifest_sha=${manifest._self_sha256 || '-'}\ngit=${manifestGit}\nts=${manifestTs}`}>
              {manifestGit ? `git=${manifestGit} ` : ''}
              {manifestSha ? `manifest=${manifestSha}…` : ''}
            </span>
          ) : (
            <span className="text-muted" style={{ fontSize: 12 }}>패키지 미생성</span>
          )}
          <button className="btn btn--primary" disabled={!!activeJob} onClick={() => { void startBuild() }}>
            {activeJob?.kind === 'build' ? '빌드 중…' : '⚙ 전체 빌드'}
          </button>
          <button className="btn btn--outline" onClick={() => { void load(); void loadPackages(); void loadManifest() }}>
            ↻ 새로고침
          </button>
        </div>
      </div>

      {/* 일괄 액션 — 체크된 변종 대상 (아이콘만, hover 툴팁) */}
      <div style={{
        marginBottom: 12, padding: '8px 12px', borderRadius: 4, background: '#f4f6f8',
        display: 'flex', gap: 8, alignItems: 'center', flexWrap: 'wrap',
      }}>
        <button
          className="btn btn--sm btn--outline"
          onClick={toggleSelectAll}
          title={selected.size === managedNames.length ? '선택 해제' : '모두 선택'}
        >
          {selected.size === managedNames.length ? '☐ 해제' : '☑ 모두'}
        </button>
        <span style={{ fontSize: 13, color: '#555' }}>
          선택 {selected.size} / {managedNames.length}
        </span>
        <span style={{ marginLeft: 8, color: '#aaa' }}>|</span>
        <button
          className={`btn btn--sm${selectedAnyRunning ? ' btn--danger' : ''}`}
          disabled={selected.size === 0}
          onClick={() => { void bulkAct(selectedAnyRunning ? 'stop' : 'start') }}
          title={selectedAnyRunning ? '선택 변종 종료' : '선택 변종 기동'}
        >
          {selectedAnyRunning ? '■' : '▶'}
        </button>
        <button className="btn btn--sm btn--outline"
          disabled={selected.size === 0}
          onClick={() => { void bulkAct('restart') }}
          title="선택 변종 재기동"
        >
          ↻
        </button>
      </div>

      <div style={{
        display: 'flex', gap: 16, alignItems: 'stretch',
        height: 'calc(100vh - 240px)', minHeight: 320,
      }}>
        <div style={{ flex: '3 1 0', minWidth: 0, overflow: 'auto' }}>
      {loading ? (
        <div className="empty">로딩 중...</div>
      ) : (
        <div style={{
          display: 'grid',
          gridTemplateColumns: 'repeat(auto-fill, minmax(300px, 1fr))',
          gap: 12, padding: 4,
        }}>
          {BUILD_CARDS.map(card => {
            const versions = packagesByModule[card.key] || []
            const curVer = selectedVersion[card.key] || versions[0]?.version || ''
            const pkgBusy = activeJob?.kind === 'pkg' && activeJob.cardKey === card.key
            const otherBusy = !!activeJob && !pkgBusy
            // 카드 안 어떤 변종이든 tarball 있으면 다운로드 가능
            const variantTars = card.variants.map(v => ({ v, tar: tarballByModule[v] }))
            const anyTar = variantTars.some(x => !!x.tar)
            return (
              <div key={card.key} className="card" style={{
                border: '1px solid #e5e7eb', borderRadius: 6,
                background: '#fff', padding: 12,
                display: 'flex', flexDirection: 'column', gap: 10,
              }}>
                {/* 헤더 — 모듈명 + critical */}
                <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                  <span style={{ fontFamily: 'monospace', fontWeight: 'bold', fontSize: 14 }}>
                    {card.key}
                  </span>
                  {card.critical && <span className="tag" style={{ background: '#f39c12', color: '#fff' }}>critical</span>}
                  {!card.hasProcess && <span className="tag" style={{ background: '#3498db', color: '#fff' }}>원격</span>}
                  <span style={{ marginLeft: 'auto', fontSize: 11, color: '#777' }}>
                    {card.label}
                  </span>
                </div>

                {/* 패키지 영역 */}
                <div style={{
                  borderTop: '1px solid #f0f2f4', paddingTop: 8,
                  display: 'flex', flexDirection: 'column', gap: 6,
                }}>
                  <div style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 12 }}>
                    <span style={{ color: '#555', minWidth: 50 }}>패키지</span>
                    {versions.length > 0 ? (
                      <select
                        className="form-input"
                        style={{ flex: 1, fontSize: 12, padding: '2px 6px' }}
                        value={curVer}
                        onChange={e => setSelectedVersion(v => ({ ...v, [card.key]: e.target.value }))}
                      >
                        {versions.map(p => (
                          <option key={p.id} value={p.version}>{p.version}</option>
                        ))}
                      </select>
                    ) : (
                      <span className="text-muted" style={{ fontSize: 12, flex: 1 }}>
                        없음 · <Link to="/deploy/packages">등록</Link>
                      </span>
                    )}
                    {versions.length > 0 && (
                      <button className="btn btn--sm btn--outline"
                        onClick={() => openTemplate(card.key, true)}
                        title="설정 템플릿 편집">템플릿</button>
                    )}
                  </div>
                  {/* 변종별 다운로드 + 묶음 ▣ */}
                  <div style={{ display: 'flex', alignItems: 'center', gap: 6, flexWrap: 'wrap' }}>
                    {variantTars.map(({ v, tar }) => (
                      <button key={v} className="btn btn--sm btn--outline"
                        disabled={!tar}
                        title={tar ? `${tar.name} (${fmtSize(tar.size)})` : `${v} tarball 없음 — 먼저 패키지화`}
                        onClick={() => { void downloadTarball(v) }}
                        style={{ fontSize: 11, padding: '2px 6px' }}>
                        ⤓ {v}
                      </button>
                    ))}
                    <button className="btn btn--sm btn--outline"
                      disabled={otherBusy || pkgBusy}
                      title={pkgBusy ? '패키지화 진행 중…'
                        : card.variants.length > 1
                          ? `cims.sh pkg ${card.variants.join(' ')} --no-bump`
                          : `cims.sh pkg ${card.variants[0]} --no-bump`}
                      onClick={() => { void startPkgForCard(card) }}
                      style={{ marginLeft: 'auto' }}>
                      {pkgBusy ? '…' : `▣ 재패키징${card.variants.length > 1 ? ` (${card.variants.length})` : ''}`}
                    </button>
                  </div>
                  {versions.length > 0 && card.hasProcess && (
                    <div style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 12 }}>
                      <button className="btn btn--sm btn--outline" onClick={() => setConfigModule(card.key)}>
                        설정
                      </button>
                      {card.variants.some(v => needsRestart[v]) && (
                        <span className="tag" style={{ background: '#e74c3c', color: '#fff' }}
                              title="설정 변경 후 재시작 필요">!</span>
                      )}
                    </div>
                  )}
                  {!anyTar && versions.length === 0 && (
                    <span className="text-muted" style={{ fontSize: 11 }}>패키지 미생성</span>
                  )}
                </div>

                {/* 프로세스 영역 — hasProcess 인 카드만 */}
                {card.hasProcess && (
                  <div style={{
                    borderTop: '1px solid #f0f2f4', paddingTop: 8,
                    display: 'flex', flexDirection: 'column', gap: 4,
                  }}>
                    <div style={{ fontSize: 12, color: '#555' }}>프로세스</div>
                    {card.variants.map(v => {
                      const s = states[v]
                      const running = s?.running ?? false
                      const disabled = busy[v]
                      return (
                        <div key={v} style={{
                          display: 'flex', alignItems: 'center', gap: 6, fontSize: 12,
                        }}>
                          <input type="checkbox"
                            checked={selected.has(v)}
                            onChange={() => toggleSelect(v)}
                            aria-label={`${v} 선택`}
                          />
                          {card.variants.length > 1 && (
                            <span style={{ fontFamily: 'monospace', minWidth: 32 }}>{v}</span>
                          )}
                          <span className="tag" style={{
                            background: running ? '#2ecc71' : '#95a5a6', color: '#fff',
                            minWidth: 50, textAlign: 'center',
                          }}>
                            {running ? 'on' : 'off'}
                          </span>
                          <span style={{ minWidth: 60, color: '#777', fontFamily: 'monospace' }}>
                            {running ? `pid=${s?.pid ?? '?'}` : '—'}
                          </span>
                          <div style={{ marginLeft: 'auto', display: 'flex', gap: 4 }}>
                            <button
                              className={`btn btn--sm${running ? ' btn--danger' : ''}`}
                              disabled={disabled}
                              onClick={() => toggleRunning(v, running, !!card.critical)}
                              title={running ? '종료' : '기동'}
                            >
                              {running ? '■' : '▶'}
                            </button>
                            <button className="btn btn--sm btn--outline"
                              disabled={disabled || !running}
                              onClick={() => act(v, 'restart', !!card.critical)}
                              title="재기동">
                              ↻
                            </button>
                          </div>
                        </div>
                      )
                    })}
                  </div>
                )}
              </div>
            )
          })}
        </div>
      )}
        </div>
        <div style={{
          flex: '2 1 0', minWidth: 0,
          display: 'flex', flexDirection: 'column',
        }}>
          <div style={{
            padding: 12, borderRadius: 4,
            background: '#1f2937', color: '#e5e7eb', fontFamily: 'monospace', fontSize: 12,
            display: 'flex', flexDirection: 'column',
            minHeight: 0, flex: 1, overflow: 'hidden',
          }}>
            {(() => {
              // activeJob 진행 중이면 항상 job, 그 외엔 마지막 갱신 출처
              const showJob = !!activeJob || (terminalSource === 'job' && jobStatus)
              const showModule = !showJob && terminalSource === 'module' && lastModule
              const tagBg =
                activeJob ? '#3b82f6'
                : showJob && jobStatus?.verdict === 'PASS' ? '#16a34a'
                : showJob && jobStatus?.verdict === 'FAIL' ? '#dc2626'
                : showModule && lastModule?.verdict === 'PASS' ? '#16a34a'
                : showModule && lastModule?.verdict === 'FAIL' ? '#dc2626'
                : '#475569'
              const tagText = showJob && jobStatus
                ? (jobStatus.kind === 'build' ? '빌드' : `패키지화 ${activeJob?.module || jobStatus.label.replace(/^cims\.sh pkg /, '') || ''}`)
                  + (activeJob ? ' · 실행 중'
                     : jobStatus.verdict === 'PASS' ? ' · PASS'
                     : jobStatus.verdict === 'FAIL' ? ' · FAIL'
                     : '')
                : showModule && lastModule
                ? `${lastModule.label} · ${lastModule.verdict}`
                : '터미널'
              const meta = showJob && jobStatus
                ? `elapsed=${jobStatus.elapsed.toFixed(1)}s${!activeJob && jobStatus.returncode !== null ? ` rc=${jobStatus.returncode}` : ''}`
                : showModule && lastModule
                ? `rc=${lastModule.returncode}`
                : ''
              const right = showJob && jobStatus ? `job=${jobStatus.job_id}` : ''
              const content = showJob && jobStatus
                ? (jobStatus.stdout_tail || '(no output yet)').replace(/\x1b\[[0-9;]*m/g, '')
                : showModule && lastModule
                ? (lastModule.output || '(no output)').replace(/\x1b\[[0-9;]*m/g, '').replace(/\[[0-9;]*m/g, '')
                : '$ ⚙ 전체 빌드 / 패키지화 / 모듈 시작·정지·재시작 시 출력이 여기에 표시됩니다.\n'
              return (
                <>
                  <div style={{ marginBottom: 6, display: 'flex', justifyContent: 'space-between', flex: '0 0 auto' }}>
                    <span>
                      <span className="tag" style={{ background: tagBg, color: '#fff', marginRight: 8 }}>
                        {tagText}
                      </span>
                      {meta && <span>{meta}</span>}
                    </span>
                    {right && <span>{right}</span>}
                  </div>
                  <pre style={{
                    margin: 0, flex: 1, minHeight: 0,
                    overflow: 'auto', whiteSpace: 'pre-wrap',
                    background: '#0d1117', padding: 8, borderRadius: 4,
                  }}>
                    {content}
                  </pre>
                </>
              )
            })()}
          </div>
        </div>
      </div>


      {templateModal && (
        <Modal
          fullscreen
          title={`설정 템플릿 — ${templateModal.module} (v${templateModal.pkg.version})${editing ? ' · 편집 중' : ''}`}
          onClose={() => { if (!saving) setTemplateModal(null) }}
        >
          <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
            <div style={{ flex: 1, overflow: 'auto', padding: 20 }}>
              {editing ? (
                <>
                  <textarea
                    className="form-input"
                    style={{
                      width: '100%', height: 'calc(100vh - 260px)', minHeight: 400,
                      fontFamily: 'monospace', fontSize: 12, lineHeight: 1.5,
                      background: '#0d1117', color: '#c9d1d9',
                    }}
                    value={editText}
                    onChange={e => setEditText(e.target.value)}
                    spellCheck={false}
                  />
                  {editError && <div className="auth-error" style={{ marginTop: 8 }}>{editError}</div>}
                  <div className="text-muted" style={{ fontSize: 12, marginTop: 8 }}>
                    최상위 object. `sections[]`, `collections[]` 스키마. 저장 시 재배포되는 deployment 가 새 템플릿으로 overlay 됩니다.
                  </div>
                </>
              ) : templateModal.pkg.config_template ? (
                <pre style={{
                  background: '#0d1117', color: '#c9d1d9', padding: 12, borderRadius: 4,
                  fontSize: 12, overflow: 'auto', margin: 0,
                }}>
                  {JSON.stringify(templateModal.pkg.config_template, null, 2)}
                </pre>
              ) : (
                <div className="empty">이 패키지에 config_template 이 포함되어 있지 않습니다. 편집 버튼으로 생성할 수 있습니다.</div>
              )}
            </div>
            <div className="modal-footer" style={{ flex: '0 0 auto' }}>
              {editing ? (
                <>
                  <button className="btn btn--outline" onClick={() => { setEditing(false); setEditError('') }} disabled={saving}>취소</button>
                  <button className="btn btn--primary" onClick={saveEdit} disabled={saving}>
                    {saving ? '저장 중...' : '저장'}
                  </button>
                </>
              ) : (
                <>
                  <button className="btn btn--outline" onClick={() => setTemplateModal(null)}>닫기</button>
                  <button className="btn btn--primary" onClick={startEdit}>편집</button>
                </>
              )}
            </div>
          </div>
        </Modal>
      )}

      {configModule && (
        <ConfigModalWrapper
          moduleName={configModule}
          version={selectedVersion[configModule] || (packagesByModule[configModule] || [])[0]?.version}
          onClose={() => setConfigModule(null)}
          onDone={() => { setNeedsRestart(s => ({ ...s, [configModule]: true })) }}
        />
      )}
    </div>
  )
}

/** source 객체의 reference 가 매 렌더마다 바뀌면 ModuleConfigModal 내부의 useEffect 가
 *  재실행돼 collection edit 중인 행이 서버 응답으로 덮어써진다 (추가 중 화면 사라짐 버그).
 *  이 얇은 래퍼가 source 를 useMemo 로 고정해서, 부모의 5s polling re-render 가
 *  모달 내부 로직에 전파되지 않도록 한다.
 */
function ConfigModalWrapper({
  moduleName, version, onClose, onDone,
}: {
  moduleName: string
  version: string | undefined
  onClose: () => void
  onDone: () => void
}) {
  const source = useMemo(
    () => ({ type: 'module' as const, name: moduleName, version }),
    [moduleName, version]
  )
  return <ModuleConfigModal source={source} onClose={onClose} onDone={onDone} />
}
