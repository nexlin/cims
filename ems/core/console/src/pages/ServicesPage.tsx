import { useConfirm } from '../components/custom/confirm'
import { Download, Play, RotateCw, Square, Trash2 } from 'lucide-react'
import { useState, useEffect, useCallback, useMemo } from 'react'
import { Link } from 'react-router-dom'
import { servicesApi, parseServiceStatus, type ServiceName, type ServiceAction } from '../api/services'
import { deploymentApi, type SipPackage, type ConfigTemplate } from '../api/deployment'
import { buildApi, type BuildJobStatus, type ManifestResponse } from '../api/build'
import { useToast } from '../components/Toast'
import Modal from '../components/Modal'
import ModuleConfigModal from '../components/module/ModuleConfigModal'
import { Button } from '@core/components/ui/button'
import { Badge, type BadgeTone } from '@core/components/ui/badge'
import { EmptyState } from '@core/components/custom/empty-state'

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
  const confirm = useConfirm()
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
            // release 성공 시 — file_store 자동 등록 (DEV 환경 한정. 상용은 backend 가 403)
            if (job.kind === 'release' && ok) {
              try {
                const r = await deploymentApi.registerPackagesFromDist()
                show(`패키지 자동 등록: ${r.count}개${r.errors.length ? ` (errors=${r.errors.length})` : ''}`, 'ok')
              } catch (e) {
                show(`패키지 자동 등록 실패: ${(e as Error).message} — /deploy/packages 에서 수동 업로드 필요`, 'err')
              }
            }
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
    if (!await confirm({ title: '빌드 & 패키징', confirmLabel: '시작', body: <>
      빌드 &amp; 패키징을 시작합니다 (cmake + make + npm + tarball 12종).
      <div className="mt-1">5~15분 소요될 수 있습니다.</div>
      {v && <div className="mt-1">버전: {v} — 모든 컴포넌트의 pkg.json 에 반영됩니다.</div>}
      <div className="mt-2">계속할까요?</div>
    </> })) {
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
    if (!await confirm({ title: '패키지 산출물 삭제', tone: 'danger', confirmLabel: '삭제', body: <>
      패키지 산출물 (tarball 들 + manifest.json) 을 삭제합니다.
      <div className="mt-1">빌드 결과는 유지됩니다 (다음 패키징 시 재사용).</div>
      <div className="mt-2">계속할까요?</div>
    </> })) {
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
        const ok = await confirm({
          title: action === 'stop' ? 'CSC 중지' : 'CSC 재시작',
          tone: 'danger',
          confirmLabel: action === 'stop' ? '중지' : '재시작',
          body: action === 'stop'
            ? 'CSC 를 중지하면 Console UI 가 즉시 끊깁니다. 계속할까요?'
            : 'CSC 재시작 중 Console UI 일시 단절됩니다. 계속할까요?',
        })
        if (!ok) return
      } else if (action === 'stop' || action === 'restart') {
        const ok = await confirm({
          title: action === 'stop' ? '서비스 중지' : '서비스 재시작',
          tone: action === 'stop' ? 'danger' : 'default',
          confirmLabel: action === 'stop' ? '중지' : '재시작',
          body: `${name} 서비스를 ${action === 'stop' ? '중지' : '재시작'}할까요?`,
        })
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
      <div className="mb-4 flex gap-3 items-center flex-wrap">
        <h3 className="m-0">패키징</h3>
        <span className="text-muted text-md">
          빌드 → 시험 실행 → 패키징 → 다운로드. 신규 패키지 등록/편집은{' '}
          <Link to="/deploy/packages">배포 &gt; 패키지</Link> 에서.
        </span>
        <div className="ml-auto flex gap-2 items-center">
          {manifest ? (
            <Badge variant="neutralSolid"
                  title={`manifest_sha=${manifest._self_sha256 || '-'}\ngit=${manifestGit}\nts=${manifestTs}`}>
              {manifestGit ? `git=${manifestGit} ` : ''}
              {manifestSha ? `manifest=${manifestSha}…` : ''}
            </Badge>
          ) : (
            <span className="text-muted text-sm">패키지 미생성</span>
          )}
          {/* 빌드 + 패키징 통합 — 입력 버전을 -v 로 전달 (pkg.json 갱신) + tarball 산출 */}
          <input className="w-[110px] text-md py-1 px-2 border border-border rounded-[4px]"
            type="text"
            list="all-versions"
            value={globalVersion}
            onChange={e => setGlobalVersion(e.target.value)}
            onKeyDown={e => { if (e.key === 'Enter') void startRelease() }}
            placeholder="v1.2.3"
            title="빌드 & 패키징할 버전 (cims.sh build -v + pkg --no-bump). 비워두면 현재 pkg.json 버전 유지."/>
          <datalist id="all-versions">
            {allVersions.map(v => <option key={v} value={v} />)}
          </datalist>
          <Button variant="default" size="default" disabled={!!activeJob} onClick={() => { void startRelease() }}
                  title="빌드 + 패키징 한 번에 (cmake + make + npm + tarball 12종). 5~15분.">
            {activeJob?.kind === 'release' ? '진행 중…'
              : activeJob?.kind === 'build' ? '빌드 중…'
              : activeJob?.kind === 'pkg' ? '패키징 중…'
              : <><Play size={13} /> 빌드 & 패키징</>}
          </Button>
          <Button variant="destructive" size="default" disabled={!!activeJob} onClick={() => { void cleanPackages() }}
                  title="패키지 산출물 (tarball 들 + manifest.json) 삭제. 빌드 결과는 유지.">
            <Trash2 size={13} className="inline align-[-2px]" /> 정리
          </Button>
          <Button size="default" onClick={() => { void load(); void loadPackages(); void loadManifest() }}>
            <RotateCw size={13} /> 새로고침
          </Button>
        </div>
      </div>

      <div className="flex gap-4 items-stretch flex-1 min-h-[320px]">
        <div style={{ flex: '3 1 0', minWidth: 0, overflow: 'auto' }}>
      {loading ? (
        <div className="flex min-h-0 flex-1 items-center justify-center p-8 text-center text-muted-foreground">로딩 중...</div>
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
              <div className="border border-border rounded-sm bg-card p-3 flex flex-col gap-2.5" key={card.key}>
                {/* 헤더 — 모듈명 + critical */}
                <div className="flex items-center gap-2">
                  <span style={{ fontFamily: 'monospace', fontWeight: 'bold', fontSize: 14 }}>
                    {card.key}
                  </span>
                  {card.critical && <Badge variant="warningSolid">critical</Badge>}
                  {!card.hasProcess && <Badge variant="infoSolid">원격</Badge>}
                  <span className="ml-auto text-xs text-muted-foreground">
                    {card.label}
                  </span>
                </div>

                {/* 본문 — 2 col x 2 row 그리드: ¹³ / ²⁴.
                    한 행에 두 영역이 좌우로 놓여 컴팩트한 가로 와이드 카드. */}
                <div style={{
                  borderTop: '1px solid var(--border)', paddingTop: 8,
                  display: 'grid',
                  gridTemplateColumns: '1fr 1fr',
                  columnGap: 16, rowGap: 8,
                }}>
                  {/* ¹ 설정 — 템플릿/설정 편집 (버전 선택은 ³ 로 이동) */}
                  <div className="flex items-center gap-1.5 text-sm min-w-0">
                    <span className="text-muted-foreground font-medium min-w-[50px]">¹ 설정</span>
                    <Button
                      disabled={versions.length === 0}
                      onClick={() => openTemplate(card.key, true)}
                      title="설정 템플릿 편집">템플릿</Button>
                    {card.hasProcess && (
                      <Button
                        disabled={versions.length === 0}
                        onClick={() => setConfigModule(card.key)}
                        title="모듈 설정 편집">설정</Button>
                    )}
                    {needsRestart[card.key] && (
                      <Badge variant="dangerSolid" title="설정 변경 후 재시작 필요">!</Badge>
                    )}
                    {versions.length === 0 && (
                      <Link className="text-xs text-muted-foreground ml-auto" to="/deploy/packages"
                            title="신규 패키지 등록">등록</Link>
                    )}
                  </div>

                  {/* ² 실행 — hasProcess 카드만, 빈 셀로 정렬 유지 */}
                  {card.hasProcess ? (
                    <div className="flex items-center gap-1.5 text-sm min-w-0">
                      <span className="text-muted-foreground font-medium min-w-[50px]">² 실행</span>
                      <Badge variant={running ? 'successSolid' : 'neutralSolid'} className="min-w-10 justify-center">
                        {running ? 'on' : 'off'}
                      </Badge>
                      <span className="text-muted-foreground font-mono text-xs">
                        {running ? `pid=${s?.pid ?? '?'}` : '—'}
                      </span>
                      <div className="ml-auto flex gap-1">
                        <Button variant={running ? 'destructive' : 'outline'}
                          disabled={disabled}
                          onClick={() => toggleRunning(card.key, running, !!card.critical)}
                          title={running ? '종료' : '기동'}
                        >
                          {running ? <Square size={12} /> : <Play size={12} />}
                        </Button>
                        <Button
                          disabled={disabled || !running}
                          onClick={() => act(card.key, 'restart', !!card.critical)}
                          title="재기동">
                          <RotateCw size={12} />
                        </Button>
                      </div>
                    </div>
                  ) : (
                    <div className="text-xs text-muted-foreground flex items-center gap-1.5">
                      <span className="min-w-[50px]">² 실행</span>
                      <span>(원격 — 로컬 실행 없음)</span>
                    </div>
                  )}

                  {/* ³ 다운로드 — 헤더 ▣ 패키징 산출 tarball. 라벨에 모듈명 + 버전 (실수 방지). */}
                  <div style={{ gridColumn: '1 / -1',
                                display: 'flex', alignItems: 'center', gap: 6,
                                fontSize: 12, flexWrap: 'wrap', minWidth: 0 }}>
                    <span className="text-muted-foreground font-medium min-w-[50px]">³ 다운로드</span>
                    {variantTars.map(({ v, tar }) => (
                      <Button className="text-xs py-0.5 px-1.5 font-mono" key={v}
                        disabled={!tar}
                        title={tar ? `${tar.name} (${fmtSize(tar.size)})` : `${v} tarball 없음 — 먼저 [패키징]`}
                        onClick={() => { void downloadTarball(v) }}>
                        <Download size={12} /> {v}{tar?.version ? ` v${tar.version}` : ''}
                      </Button>
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
          <div className="p-3 rounded-[4px] bg-muted text-foreground font-mono text-sm flex flex-col min-h-0 flex-1 overflow-hidden">
            {(() => {
              // activeJob 진행 중이면 항상 job, 그 외엔 마지막 갱신 출처
              const showJob = !!activeJob || (terminalSource === 'job' && jobStatus)
              const showModule = !showJob && terminalSource === 'module' && lastModule
              const tagTone: BadgeTone =
                activeJob ? 'infoSolid'
                : showJob && jobStatus?.verdict === 'PASS' ? 'successSolid'
                : showJob && jobStatus?.verdict === 'FAIL' ? 'dangerSolid'
                : showModule && lastModule?.verdict === 'PASS' ? 'successSolid'
                : showModule && lastModule?.verdict === 'FAIL' ? 'dangerSolid'
                : 'neutralSolid'
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
                : '$ 전체 빌드 / 패키지화 / 모듈 시작·정지·재시작 시 출력이 여기에 표시됩니다.\n'
              return (
                <>
                  <div className="mb-1.5 flex justify-between flex-none">
                    <span>
                      <Badge variant={tagTone} className="mr-2">{tagText}</Badge>
                      {meta && <span>{meta}</span>}
                    </span>
                    {right && <span>{right}</span>}
                  </div>
                  <pre className="m-0 flex-1 min-h-0 overflow-auto whitespace-pre-wrap bg-muted p-2 rounded-[4px]">
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
          <div className="flex flex-col h-full">
            <div className="flex-1 overflow-auto p-5">
              {editing ? (
                <>
                  <textarea
                    className="rounded-md border border-border px-2.5 py-2 outline-none transition-colors focus-visible:border-primary focus-visible:shadow-focus w-full h-full min-h-[400px] font-mono text-sm leading-normal bg-muted text-foreground"
                    value={editText}
                    onChange={e => setEditText(e.target.value)}
                    spellCheck={false}/>
                  {editError && <div className="auth-error mt-2">{editError}</div>}
                  <div className="text-muted text-sm mt-2">
                    최상위 object. `sections[]`, `collections[]` 스키마. 저장 시 재배포되는 deployment 가 새 템플릿으로 overlay 됩니다.
                  </div>
                </>
              ) : templateModal.pkg.config_template ? (
                <pre className="bg-muted text-foreground p-3 rounded-[4px] text-sm overflow-auto m-0">
                  {JSON.stringify(templateModal.pkg.config_template, null, 2)}
                </pre>
              ) : (
                <EmptyState title="이 패키지에 config_template 이 포함되어 있지 않습니다. 편집 버튼으로 생성할 수 있습니다." />
              )}
            </div>
            <div className="flex justify-end gap-2.5 pt-5 flex-none">
              {editing ? (
                <>
                  <Button size="default" onClick={() => { setEditing(false); setEditError('') }} disabled={saving}>취소</Button>
                  <Button variant="default" size="default" onClick={saveEdit} disabled={saving}>
                    {saving ? '저장 중...' : '저장'}
                  </Button>
                </>
              ) : (
                <>
                  <Button size="default" onClick={() => setTemplateModal(null)}>닫기</Button>
                  <Button variant="default" size="default" onClick={startEdit}>편집</Button>
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
