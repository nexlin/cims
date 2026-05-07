import { useState, useEffect, useCallback, useMemo } from 'react'
import { Link } from 'react-router-dom'
import { servicesApi, parseServiceStatus, type ServiceName, type ServiceAction } from '../api/services'
import { deploymentApi, type SipPackage, type ConfigTemplate } from '../api/deployment'
import { buildApi, type BuildJobStatus, type ManifestResponse } from '../api/build'
import { useToast } from '../components/Toast'
import Modal from '../components/Modal'
import ModuleConfigModal from '../components/module/ModuleConfigModal'

type SvcState = { running: boolean; pid?: number }

const SERVICE_META: Array<{ name: ServiceName; label: string; critical?: boolean; managed?: boolean }> = [
  { name: 'cmp',     label: 'CMP (미디어)',            managed: true },
  { name: 'csp',     label: 'CSP (SIP)',                managed: true },
  { name: 'cwrtc',   label: 'cwrtc (WebRTC 브리지)',   managed: true },
  { name: 'csc',     label: 'CSC (Admin API)',          managed: true, critical: true },
  { name: 'console', label: 'Console (웹 UI)',          managed: true },
  { name: 'phone',   label: 'Phone (웹 단말)',          managed: true },
  { name: 'agent',   label: 'Agent (원격 관리 모듈)',   managed: false },
]

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
  const [activeJob, setActiveJob] = useState<{ id: string; kind: 'build' | 'pkg'; module?: string } | null>(null)
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
            const label = job.kind === 'build' ? '빌드' : `패키지화 (${job.module})`
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

  async function startPkg(module: string) {
    if (activeJob) {
      show('이미 진행 중인 빌드/패키지 작업이 있습니다.', 'err')
      return
    }
    try {
      const r = await buildApi.runPkg(module)
      setActiveJob({ id: r.job_id, kind: 'pkg', module })
      show(`${module} 패키지화 시작`, 'ok')
    } catch (e) {
      const msg = (e as Error).message
      show(`${module} 패키지화 실패: ${msg}`, 'err')
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

  // 관리 가능한 (managed=true) 모듈 — 체크박스/일괄 대상
  const managedNames = useMemo<ServiceName[]>(
    () => SERVICE_META.filter(s => s.managed !== false).map(s => s.name),
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
    const hasCritical = targets.some(n => SERVICE_META.find(s => s.name === n)?.critical)
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

      {/* 일괄 액션 — 체크된 모듈 대상 (아이콘만, hover 툴팁) */}
      <div style={{
        marginBottom: 12, padding: '8px 12px', borderRadius: 4, background: '#f4f6f8',
        display: 'flex', gap: 8, alignItems: 'center', flexWrap: 'wrap',
      }}>
        <span style={{ fontSize: 13, color: '#555' }}>
          선택 {selected.size} / {managedNames.length}
        </span>
        <span style={{ marginLeft: 8, color: '#aaa' }}>|</span>
        <button
          className={`btn btn--sm${selectedAnyRunning ? ' btn--danger' : ''}`}
          disabled={selected.size === 0}
          onClick={() => { void bulkAct(selectedAnyRunning ? 'stop' : 'start') }}
          title={selectedAnyRunning ? '선택 모듈 종료' : '선택 모듈 기동'}
        >
          {selectedAnyRunning ? '■' : '▶'}
        </button>
        <button className="btn btn--sm btn--outline"
          disabled={selected.size === 0}
          onClick={() => { void bulkAct('restart') }}
          title="선택 모듈 재기동"
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
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 32 }}>
                <input type="checkbox"
                  checked={selected.size === managedNames.length && managedNames.length > 0}
                  ref={el => { if (el) el.indeterminate = selected.size > 0 && selected.size < managedNames.length }}
                  onChange={toggleSelectAll}
                  aria-label="모두 선택"
                />
              </th>
              <th style={{ width: 70 }}>모듈</th>
              <th>설명</th>
              <th style={{ width: 90 }}>상태</th>
              <th style={{ width: 70 }}>PID</th>
              <th style={{ width: 120 }}>버전</th>
              <th style={{ width: 70 }}>템플릿</th>
              <th style={{ width: 80 }}>설정</th>
              <th style={{ width: 90 }}>패키지</th>
              <th style={{ width: 90 }}>제어</th>
            </tr>
          </thead>
          <tbody>
            {SERVICE_META.map(svc => {
              const s = states[svc.name]
              const running = s?.running ?? false
              const disabled = busy[svc.name]
              const versions = packagesByModule[svc.name] || []
              const curVer = selectedVersion[svc.name] || versions[0]?.version || ''
              const checkable = svc.managed !== false
              return (
                <tr key={svc.name}>
                  <td>
                    <input type="checkbox"
                      checked={selected.has(svc.name)}
                      disabled={!checkable}
                      onChange={() => toggleSelect(svc.name)}
                      aria-label={`${svc.name} 선택`}
                    />
                  </td>
                  <td style={{ fontFamily: 'monospace', fontWeight: 'bold' }}>{svc.name}</td>
                  <td>
                    {svc.label}
                    {svc.critical && <span className="tag" style={{ marginLeft: 8, background: '#f39c12', color: '#fff' }}>critical</span>}
                  </td>
                  <td>
                    {svc.managed === false ? (
                      <span className="tag" style={{ background: '#3498db', color: '#fff' }}>원격</span>
                    ) : (
                      <span className="tag" style={{ background: running ? '#2ecc71' : '#95a5a6', color: '#fff' }}>
                        {running ? '실행 중' : '중지됨'}
                      </span>
                    )}
                  </td>
                  <td>{svc.managed === false ? '—' : (s?.pid ?? '—')}</td>
                  <td>
                    {versions.length > 0 ? (
                      <select
                        className="form-input"
                        style={{ width: '100%', fontSize: 12, padding: '4px 6px' }}
                        value={curVer}
                        onChange={e => setSelectedVersion(v => ({ ...v, [svc.name]: e.target.value }))}
                      >
                        {versions.map(p => (
                          <option key={p.id} value={p.version}>{p.version}</option>
                        ))}
                      </select>
                    ) : (
                      <span className="text-muted" style={{ fontSize: 12 }}>등록된 패키지 없음</span>
                    )}
                  </td>
                  <td>
                    {versions.length > 0 ? (
                      <button className="btn btn--sm btn--outline"
                        onClick={() => openTemplate(svc.name, true)}
                        title="설정 템플릿 편집">
                        편집
                      </button>
                    ) : (
                      <span className="text-muted" style={{ fontSize: 12 }}>
                        없음 · <Link to="/deploy/packages">등록</Link>
                      </span>
                    )}
                  </td>
                  <td>
                    {versions.length > 0 ? (
                      svc.managed === false ? (
                        <span className="text-muted" style={{ fontSize: 12 }} title="원격 모듈 — install-agent.sh 옵션으로 설정">
                          (원격)
                        </span>
                      ) : (
                        <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
                          <button className="btn btn--sm btn--outline" onClick={() => setConfigModule(svc.name)}>
                            설정
                          </button>
                          {needsRestart[svc.name] && (
                            <span className="tag" style={{ background: '#e74c3c', color: '#fff' }} title="설정 변경 후 재시작 필요">!</span>
                          )}
                        </div>
                      )
                    ) : (
                      <span className="text-muted" style={{ fontSize: 12 }}>—</span>
                    )}
                  </td>
                  <td>
                    {(() => {
                      const tar = tarballByModule[svc.name]
                      const pkgBusy = activeJob?.kind === 'pkg' && activeJob.module === svc.name
                      const otherBusy = !!activeJob && !pkgBusy
                      return (
                        <div style={{ display: 'flex', gap: 6 }}>
                          <button className="btn btn--sm btn--outline"
                            disabled={otherBusy || pkgBusy}
                            title={pkgBusy ? '패키지화 진행 중…' : `${svc.name} 패키지화 (cims.sh pkg ${svc.name} --no-bump)`}
                            onClick={() => { void startPkg(svc.name) }}>
                            {pkgBusy ? '…' : '▣'}
                          </button>
                          <button className="btn btn--sm btn--outline"
                            disabled={!tar}
                            title={tar ? `다운로드 — ${tar.name} (${fmtSize(tar.size)})` : '패키지화 먼저 실행'}
                            onClick={() => { void downloadTarball(svc.name) }}>
                            ⤓
                          </button>
                        </div>
                      )
                    })()}
                  </td>
                  <td>
                    {svc.managed === false ? (
                      <span className="text-muted" style={{ fontSize: 12 }}>원격 모듈 · 로컬 제어 불가</span>
                    ) : (
                      <div style={{ display: 'flex', gap: 6 }}>
                        <button
                          className={`btn btn--sm${running ? ' btn--danger' : ''}`}
                          disabled={disabled}
                          onClick={() => toggleRunning(svc.name, running, !!svc.critical)}
                          title={running ? '종료' : '기동'}
                        >
                          {running ? '■' : '▶'}
                        </button>
                        <button className="btn btn--sm btn--outline"
                          disabled={disabled || !running}
                          onClick={() => act(svc.name, 'restart', !!svc.critical)}
                          title="재기동"
                        >
                          ↻
                        </button>
                      </div>
                    )}
                  </td>
                </tr>
              )
            })}
          </tbody>
        </table>
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
