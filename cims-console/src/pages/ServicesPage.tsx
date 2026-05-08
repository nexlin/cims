import { useState, useEffect, useCallback, useMemo } from 'react'
import { Link } from 'react-router-dom'
import { servicesApi, parseServiceStatus, type ServiceName, type ServiceAction } from '../api/services'
import { deploymentApi, type SipPackage, type ConfigTemplate } from '../api/deployment'
import { buildApi, type BuildJobStatus, type ManifestResponse } from '../api/build'
import { useToast } from '../components/Toast'
import Modal from '../components/Modal'
import ModuleConfigModal from '../components/module/ModuleConfigModal'

type SvcState = { running: boolean; pid?: number }

// 빌드 단위 카드 정의. 두 축이 분리됨:
//   - 프로세스 (key): 로컬에서 시험 실행할 단일 base 모듈 이름. cims.sh / service API 가 이 이름만 인식.
//     hasProcess=true 카드는 ServiceName 으로 한정되고, false 카드 (cspsim 등) 는 패키지/다운로드만이라 임의 식별자 허용.
//   - 패키지 (packageVariants): ▣ 한 번에 산출되는 tarball 변종 이름들. csp 카드는 [csp,psp,isp] 3종.
//     미지정 시 [key] 단일 산출물.
//   critical: 종료 시 Console 단절 경고.
type BuildCard =
  | { key: ServiceName;     label: string; hasProcess: true;  critical?: boolean; packageVariants?: string[] }
  | { key: string;          label: string; hasProcess: false; critical?: boolean; packageVariants?: string[] }
const BUILD_CARDS: BuildCard[] = [
  { key: 'csp',     label: 'CSP (VoLTE/PTT/IBCF SIP)',    hasProcess: true,  packageVariants: ['csp', 'psp', 'isp'] },
  { key: 'cmp',     label: 'CMP (VoLTE/PTT/IBCF 미디어)', hasProcess: true,  packageVariants: ['cmp', 'pmp', 'imp'] },
  { key: 'cwrtc',   label: 'cwrtc (WebRTC 브리지)',       hasProcess: true },
  { key: 'csc',     label: 'CSC (Admin API)',             hasProcess: true,  critical: true },
  { key: 'console', label: 'Console (웹 UI)',             hasProcess: true },
  { key: 'phone',   label: 'Phone (웹 단말)',             hasProcess: true },
  { key: 'cspsim',  label: 'cspsim (시뮬레이터)',         hasProcess: false },
  { key: 'agent',   label: 'Agent (원격 관리)',           hasProcess: false },
]
// 카드의 패키지 산출물 이름들 — 다운로드 버튼/▣ 인자 계산용.
const cardPackages = (c: BuildCard): string[] => c.packageVariants ?? [c.key]

export default function ServicesPage() {
  const { show } = useToast()
  const [states, setStates] = useState<Record<string, SvcState>>({})
  const [loading, setLoading] = useState(true)
  const [busy, setBusy] = useState<Record<string, boolean>>({})
  const [packages, setPackages] = useState<SipPackage[]>([])
  const [templateModal, setTemplateModal] = useState<{ module: string; pkg: SipPackage } | null>(null)
  const [editing, setEditing] = useState(false)
  const [editText, setEditText] = useState('')
  const [editError, setEditError] = useState('')
  const [saving, setSaving] = useState(false)
  const [configModule, setConfigModule] = useState<string | null>(null)
  const [needsRestart, setNeedsRestart] = useState<Record<string, boolean>>({})
  // 전체 패키징 시 입력 버전 — cims.sh pkg -v 로 전달.
  const [globalVersion, setGlobalVersion] = useState('')
  // ── 빌드 / 패키지화 ─────────────────────────────────────────
  const [manifest, setManifest] = useState<ManifestResponse | null>(null)
  // activeJob.label 은 "csp" / "csp psp isp" 같이 묶음 표기
  const [activeJob, setActiveJob] = useState<{ id: string; kind: 'build' | 'pkg' | 'release'; module?: string; cardKey?: string } | null>(null)
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
            const label =
              job.kind === 'release' ? '빌드 & 패키징'
              : job.kind === 'build' ? '빌드'
              : `패키지화 (${job.cardKey || job.module || ''})`
            show(`${label} ${ok ? '완료' : '실패'} rc=${s.returncode}`, ok ? 'ok' : 'err')
            setActiveJob(null)
            // jobStatus 는 비우지 않음 — 우측 터미널 패널에 마지막 출력 유지
            await loadManifest()
            await loadPackages()
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

  // 모듈별 manifest tarball 존재 여부 (다운로드 버튼 활성 + 버전 표시)
  const tarballByModule = useMemo(() => {
    const map: Record<string, { name: string; size: number; version?: string }> = {}
    for (const p of manifest?.packages || []) {
      // <name>-<version>.tar.gz — version 은 영숫자 + 점/대시/플러스/언더스코어 허용.
      const m = /^([a-z]+)-([0-9][0-9A-Za-z.+\-_]*)\.tar\.gz$/.exec(p.name)
      if (m) map[m[1]] = { name: p.name, size: p.size, version: m[2] }
      else {
        const fallback = /^([a-z]+)-/.exec(p.name)
        if (fallback) map[fallback[1]] = { name: p.name, size: p.size }
      }
    }
    return map
  }, [manifest])

  // 빌드 + 패키징 통합 — 입력 버전을 cims.sh build -v 로 (모든 pkg.json 갱신) 후
  // 자동으로 cims.sh pkg --no-bump 까지 한 job 으로 실행. 빈 입력이면 현재 버전 유지.
  async function startRelease() {
    if (activeJob) {
      show('이미 진행 중인 작업이 있습니다.', 'err')
      return
    }
    const v = globalVersion.trim()
    if (v && !/^[0-9A-Za-z._+-]{1,64}$/.test(v)) {
      show(`잘못된 버전 형식: ${v}`, 'err')
      return
    }
    if (!confirm(`빌드 & 패키징을 시작합니다 (cmake + make + npm + tarball 12종).\n5~15분 소요될 수 있습니다.${v ? `\n버전: ${v} — 모든 컴포넌트의 pkg.json 에 반영됩니다.` : ''}\n계속할까요?`)) {
      return
    }
    try {
      const r = await buildApi.runRelease(v ? { version: v } : {})
      setActiveJob({ id: r.job_id, kind: 'release' })
      show(`빌드 & 패키징 시작${v ? ` (v=${v})` : ''}`, 'ok')
    } catch (e) {
      show(`빌드 & 패키징 시작 실패: ${(e as Error).message}`, 'err')
    }
  }

  // 정리 — packages/*.tar.gz + manifest.json 삭제. 빌드 산출물 (build/dist/<comp>) 은 유지.
  async function cleanPackages() {
    if (activeJob) {
      show('진행 중인 작업이 있습니다. 끝난 후 정리하세요.', 'err')
      return
    }
    if (!confirm('패키지 산출물 (tarball 들 + manifest.json) 을 삭제합니다.\n빌드 결과는 유지됩니다 (다음 패키징 시 재사용).\n계속할까요?')) {
      return
    }
    try {
      const r = await buildApi.cleanPackages()
      const errStr = r.errors.length > 0 ? ` · 오류 ${r.errors.length}건` : ''
      show(`정리 완료 — tarball ${r.removed_tarballs}개${r.removed_manifest ? ' + manifest' : ''}${errStr}`,
           r.errors.length > 0 ? 'err' : 'ok')
      void loadManifest()    // 정리 후 헤더 manifest 태그 갱신
    } catch (e) {
      show(`정리 실패: ${(e as Error).message}`, 'err')
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

  // 모든 모듈에 등록된 버전의 합집합 (datalist 후보용, 내림차순)
  const allVersions = useMemo(() => {
    const set = new Set<string>()
    for (const list of Object.values(packagesByModule)) {
      for (const p of list) set.add(p.version)
    }
    return Array.from(set).sort((a, b) => b.localeCompare(a, undefined, { numeric: true }))
  }, [packagesByModule])

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

  async function toggleRunning(name: ServiceName, running: boolean, critical: boolean) {
    await act(name, running ? 'stop' : 'start', critical)
  }

  function openTemplate(moduleName: string, edit = false) {
    const versions = packagesByModule[moduleName]
    if (!versions || versions.length === 0) { show('등록된 패키지 없음', 'err'); return }
    // 카드별 버전 선택 input 이 제거된 후 — 항상 가장 최신 등록 버전 사용.
    const pkg = versions[0]
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
        <h3 style={{ margin: 0 }}>패키징</h3>
        <span className="text-muted" style={{ fontSize: 13 }}>
          빌드 → 시험 실행 → 패키징 → 다운로드. 신규 패키지 등록/편집은{' '}
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
          {/* 빌드 + 패키징 통합 — 입력 버전을 -v 로 전달 (pkg.json 갱신) + tarball 산출 */}
          <input
            type="text"
            list="all-versions"
            value={globalVersion}
            onChange={e => setGlobalVersion(e.target.value)}
            onKeyDown={e => { if (e.key === 'Enter') void startRelease() }}
            placeholder="v1.2.3"
            title="빌드 & 패키징할 버전 (cims.sh build -v + pkg --no-bump). 비워두면 현재 pkg.json 버전 유지."
            style={{ width: 110, fontSize: 13, padding: '4px 8px',
                     border: '1px solid #d1d5db', borderRadius: 4 }}
          />
          <datalist id="all-versions">
            {allVersions.map(v => <option key={v} value={v} />)}
          </datalist>
          <button className="btn btn--primary" disabled={!!activeJob} onClick={() => { void startRelease() }}
                  title="빌드 + 패키징 한 번에 (cmake + make + npm + tarball 12종). 5~15분.">
            {activeJob?.kind === 'release' ? '진행 중…'
              : activeJob?.kind === 'build' ? '빌드 중…'
              : activeJob?.kind === 'pkg' ? '패키징 중…'
              : '▶ 빌드 & 패키징'}
          </button>
          <button className="btn btn--danger" disabled={!!activeJob} onClick={() => { void cleanPackages() }}
                  title="패키지 산출물 (tarball 들 + manifest.json) 삭제. 빌드 결과는 유지.">
            🗑 정리
          </button>
          <button className="btn btn--outline" onClick={() => { void load(); void loadPackages(); void loadManifest() }}>
            ↻ 새로고침
          </button>
        </div>
      </div>

      <div style={{
        display: 'flex', gap: 16, alignItems: 'stretch',
        height: 'calc(100vh - 180px)', minHeight: 320,
      }}>
        <div style={{ flex: '3 1 0', minWidth: 0, overflow: 'auto' }}>
      {loading ? (
        <div className="empty">로딩 중...</div>
      ) : (
        <div style={{
          display: 'grid',
          gridTemplateColumns: 'repeat(auto-fill, minmax(560px, 1fr))',
          gap: 12, padding: 4,
        }}>
          {BUILD_CARDS.map(card => {
            const versions = packagesByModule[card.key] || []
            // 카드의 패키지 산출물별 tarball 존재 여부 (³ 다운로드 영역에서 사용)
            const variantTars = cardPackages(card).map(v => ({ v, tar: tarballByModule[v] }))
            const s = states[card.key]
            const running = s?.running ?? false
            const disabled = busy[card.key]
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

                {/* 본문 — 2 col x 2 row 그리드: ¹³ / ²⁴.
                    한 행에 두 영역이 좌우로 놓여 컴팩트한 가로 와이드 카드. */}
                <div style={{
                  borderTop: '1px solid #f0f2f4', paddingTop: 8,
                  display: 'grid',
                  gridTemplateColumns: '1fr 1fr',
                  columnGap: 16, rowGap: 8,
                }}>
                  {/* ¹ 설정 — 템플릿/설정 편집 (버전 선택은 ³ 로 이동) */}
                  <div style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 12, minWidth: 0 }}>
                    <span style={{ color: '#555', fontWeight: 500, minWidth: 50 }}>¹ 설정</span>
                    <button className="btn btn--sm btn--outline"
                      disabled={versions.length === 0}
                      onClick={() => openTemplate(card.key, true)}
                      title="설정 템플릿 편집">템플릿</button>
                    {card.hasProcess && (
                      <button className="btn btn--sm btn--outline"
                        disabled={versions.length === 0}
                        onClick={() => setConfigModule(card.key)}
                        title="모듈 설정 편집">설정</button>
                    )}
                    {needsRestart[card.key] && (
                      <span className="tag" style={{ background: '#e74c3c', color: '#fff' }}
                            title="설정 변경 후 재시작 필요">!</span>
                    )}
                    {versions.length === 0 && (
                      <Link to="/deploy/packages" style={{ fontSize: 11, color: '#888', marginLeft: 'auto' }}
                            title="신규 패키지 등록">등록</Link>
                    )}
                  </div>

                  {/* ² 실행 — hasProcess 카드만, 빈 셀로 정렬 유지 */}
                  {card.hasProcess ? (
                    <div style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 12, minWidth: 0 }}>
                      <span style={{ color: '#555', fontWeight: 500, minWidth: 50 }}>² 실행</span>
                      <span className="tag" style={{
                        background: running ? '#2ecc71' : '#95a5a6', color: '#fff',
                        minWidth: 40, textAlign: 'center',
                      }}>
                        {running ? 'on' : 'off'}
                      </span>
                      <span style={{ color: '#777', fontFamily: 'monospace', fontSize: 11 }}>
                        {running ? `pid=${s?.pid ?? '?'}` : '—'}
                      </span>
                      <div style={{ marginLeft: 'auto', display: 'flex', gap: 4 }}>
                        <button
                          className={`btn btn--sm${running ? ' btn--danger' : ''}`}
                          disabled={disabled}
                          onClick={() => toggleRunning(card.key, running, !!card.critical)}
                          title={running ? '종료' : '기동'}
                        >
                          {running ? '■' : '▶'}
                        </button>
                        <button className="btn btn--sm btn--outline"
                          disabled={disabled || !running}
                          onClick={() => act(card.key, 'restart', !!card.critical)}
                          title="재기동">
                          ↻
                        </button>
                      </div>
                    </div>
                  ) : (
                    <div style={{ fontSize: 11, color: '#aaa', display: 'flex', alignItems: 'center', gap: 6 }}>
                      <span style={{ minWidth: 50 }}>² 실행</span>
                      <span>(원격 — 로컬 실행 없음)</span>
                    </div>
                  )}

                  {/* ³ 다운로드 — 헤더 ▣ 패키징 산출 tarball. 라벨에 모듈명 + 버전 (실수 방지). */}
                  <div style={{ gridColumn: '1 / -1',
                                display: 'flex', alignItems: 'center', gap: 6,
                                fontSize: 12, flexWrap: 'wrap', minWidth: 0 }}>
                    <span style={{ color: '#555', fontWeight: 500, minWidth: 50 }}>³ 다운로드</span>
                    {variantTars.map(({ v, tar }) => (
                      <button key={v} className="btn btn--sm btn--outline"
                        disabled={!tar}
                        title={tar ? `${tar.name} (${fmtSize(tar.size)})` : `${v} tarball 없음 — 먼저 ▣ 패키징`}
                        onClick={() => { void downloadTarball(v) }}
                        style={{ fontSize: 11, padding: '2px 6px',
                                 fontFamily: 'monospace' }}>
                        ⤓ {v}{tar?.version ? ` v${tar.version}` : ''}
                      </button>
                    ))}
                  </div>
                </div>
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
                ? (jobStatus.kind === 'release' ? '빌드 & 패키징'
                   : jobStatus.kind === 'build' ? '빌드'
                   : `패키지화 ${activeJob?.module || jobStatus.label.replace(/^cims\.sh pkg /, '') || ''}`)
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
          version={(packagesByModule[configModule] || [])[0]?.version}
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
