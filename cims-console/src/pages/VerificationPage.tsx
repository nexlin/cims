import { useState, useRef, useEffect } from 'react'
import { api } from '../api/client'
import { useToast } from '../components/Toast'

interface VerResult {
  total: number
  pass: number
  fail: number
  skip: number
  elapsed: number
  modules: Array<{
    module: string
    total: number
    pass: number
    fail: number
    skip: number
    results: Array<{
      id: string
      name: string
      status: string
      detail: string
      elapsed_ms: number
    }>
  }>
}

interface PhaseRunResult {
  phase: number
  verdict: string            // PASS | FAIL | UNKNOWN
  returncode: number
  report_path: string | null
  report_ts: string
  stdout_tail: string
  argv: string[]
}

interface PhaseJobStart {
  job_id: string
  phase: number
  argv: string[]
  started_at: number
  message: string
}

interface PhaseJobStatus {
  job_id: string
  phase: number
  argv: string[]
  started_at: number
  ended_at: number | null
  elapsed: number
  done: boolean
  returncode: number | null
  verdict: string | null
  report_path: string | null
  report_ts: string
  stdout_tail: string
}

interface VerifyItemMeta {
  id: string
  phase: number
  category: string
  name: string
  depends_on: string[]
  presets: string[]
  side_effects?: string[]
  timeout_s?: number
  description?: string
}

interface VerifyPreset {
  name: string
  items: string[]
}

interface VerifyItemsResponse {
  phases: Array<{ phase: number; items: VerifyItemMeta[] }>
  presets: VerifyPreset[]
}

interface PhaseReport {
  phase: number
  path: string
  ts: string
  content: string
}

type Phase = 1 | 2 | 3

const PHASE_LABEL: Record<Phase, string> = {
  1: 'Phase 1 · 배포 전',
  2: 'Phase 2 · 배포 과정',
  3: 'Phase 3 · 배포 이후',
}

// phase 별 결과/리포트 sessionStorage 영속화 — 새로고침/re-mount 후에도 유지.
// 탭 전환 시 해당 phase 의 저장된 데이터로 화면 갱신.
function _ssGet<T>(key: string): T | null {
  try {
    if (typeof window === 'undefined') return null
    const s = window.sessionStorage.getItem(key)
    return s ? (JSON.parse(s) as T) : null
  } catch { return null }
}
function _ssSet<T>(key: string, v: T | null) {
  try {
    if (typeof window === 'undefined') return
    if (v === null || v === undefined) window.sessionStorage.removeItem(key)
    else window.sessionStorage.setItem(key, JSON.stringify(v))
  } catch { /* ignore */ }
}
const _resultKey = (p: Phase) => `verify.phase${p}.result`
const _reportKey = (p: Phase) => `verify.phase${p}.report`
const _activeJobKey = 'verify.activeJob'

interface ActiveJob {
  phase: Phase
  jobId: string
  startedAt: number
}

const PHASE_DESC: Record<Phase, string> = {
  1: '개발·보완 + 회귀 6항목. `build/dist/<모듈>/` 직접 기동.',
  2: 'tarball → TB-CSC → Test-agent 배포 메커니즘. csc 4445 overlay start/health/stop.',
  3: '배포 이후 기본 4시나리오 재수행 (VoLTE 음성/영상, PTT 그룹 음성/영상).',
}

export default function VerificationPage() {
  const { show } = useToast()

  // Phase 1/2/3 통합 탭 — sessionStorage 로 영속화 (새로고침 / 컴포넌트 re-mount 시
  // default 로 리셋되는 것 방지). Phase 1 부터 시작이 일반적인 워크플로 순서이므로 default=1.
  const [phase, setPhaseRaw] = useState<Phase>(() => {
    if (typeof window === 'undefined') return 1
    const saved = window.sessionStorage.getItem('verify.phase')
    return (saved === '1' || saved === '2' || saved === '3') ? (Number(saved) as Phase) : 1
  })
  const setPhase = (p: Phase) => {
    setPhaseRaw(p)
    try { window.sessionStorage.setItem('verify.phase', String(p)) } catch { /* ignore */ }
    // 탭 전환 시 해당 phase 의 저장된 결과/리포트 로드 (없으면 null)
    setPhaseResultRaw(_ssGet<PhaseRunResult>(_resultKey(p)))
    setPhaseReportRaw(_ssGet<PhaseReport>(_reportKey(p)))
    // 탭 전환 시 default 프리셋 (phase{N}-main > full) 자동 선택
    // 동일 phase 재선택은 prev 보존, 다른 phase 로 이동 시만 갱신.
    setSelectedItems(prev => {
      if (p === phase) return prev
      const candidates = [`phase${p}-main`, `phase${p}-full`]
      for (const name of candidates) {
        const found = (verifyMeta?.presets || []).find(pp => pp.name === name)
        if (found) return new Set(found.items)
      }
      return new Set()
    })
  }
  // 진행 중 job — sessionStorage 영속화. mount 시 복원하여 폴링 자동 재개.
  // re-mount / 페이지 이동 / 새로고침 후에도 진행 중인 검증 그대로 추적.
  const [activeJob, setActiveJobRaw] = useState<ActiveJob | null>(() => _ssGet<ActiveJob>(_activeJobKey))
  const setActiveJob = (j: ActiveJob | null) => {
    setActiveJobRaw(j)
    _ssSet(_activeJobKey, j)
  }
  const running = activeJob !== null
  const [skipBuild, setSkipBuild] = useState(true)
  const [skipPkg, setSkipPkg]     = useState(true)
  const [skipReset, setSkipReset] = useState(false)
  const [keepAgent, setKeepAgent] = useState(false)
  // phaseResult / phaseReport — sessionStorage 에 phase 별로 저장. mount 시 현재 phase 의 데이터 복원.
  const [phaseResult, setPhaseResultRaw] = useState<PhaseRunResult | null>(() => {
    if (typeof window === 'undefined') return null
    const saved = window.sessionStorage.getItem('verify.phase')
    const p = (saved === '1' || saved === '2' || saved === '3') ? (Number(saved) as Phase) : 1
    return _ssGet<PhaseRunResult>(_resultKey(p))
  })
  const [phaseReport, setPhaseReportRaw] = useState<PhaseReport | null>(() => {
    if (typeof window === 'undefined') return null
    const saved = window.sessionStorage.getItem('verify.phase')
    const p = (saved === '1' || saved === '2' || saved === '3') ? (Number(saved) as Phase) : 1
    return _ssGet<PhaseReport>(_reportKey(p))
  })
  // setPhaseReport 영속화 wrapper — "📄 최신 리포트" 수동 로드 시에도 sessionStorage 동기화.
  // (phaseResult 는 폴링 effect 가 직접 setPhaseResultRaw + _ssSet 사용)
  const setPhaseReport = (rep: PhaseReport | null) => {
    setPhaseReportRaw(rep)
    if (rep) _ssSet(_reportKey(rep.phase as Phase), rep)
  }
  // 실행 진행 상태 (job 폴링)
  const [progress, setProgress] = useState<PhaseJobStatus | null>(null)
  // verify_lib 메타 (phase 별 항목 + 프리셋) — Step 1 에서 phase 3 만 활성
  const [verifyMeta, setVerifyMeta] = useState<VerifyItemsResponse | null>(null)
  const [selectedItems, setSelectedItems] = useState<Set<string>>(new Set())
  // mount 시 한 번만 fetch — 현재 phase 의 default 프리셋 자동 선택
  // 우선순위: phase{N}-main > phase{N}-full (Phase 1 의 main 은 모듈 제외 — 무겁지 않게)
  useEffect(() => {
    api.get<VerifyItemsResponse>('/verification/items')
      .then(r => {
        setVerifyMeta(r)
        const candidates = [`phase${phase}-main`, `phase${phase}-full`]
        for (const name of candidates) {
          const found = (r.presets || []).find(p => p.name === name)
          if (found) { setSelectedItems(new Set(found.items)); break }
        }
      })
      .catch(() => { /* 메타 fetch 실패 시 동적 UI 비활성, 기존 동작 유지 */ })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])
  // 현재 phase 의 verify_lib 항목 (있는 경우만 동적 체크박스 노출)
  const currentPhaseItems: VerifyItemMeta[] = (verifyMeta?.phases || [])
    .find(p => p.phase === phase)?.items || []
  const currentPhasePresets: VerifyPreset[] = (verifyMeta?.presets || [])
    .filter(p => p.name.startsWith(`phase${phase}-`))
  const hasDynamicItems = currentPhaseItems.length > 0
  // 항목별 토글 / 프리셋 적용
  function toggleItem(id: string) {
    setSelectedItems(prev => {
      const n = new Set(prev)
      if (n.has(id)) n.delete(id); else n.add(id)
      return n
    })
  }
  function applyPreset(p: VerifyPreset) {
    setSelectedItems(new Set(p.items))
  }
  // 결과 영역 ref — 완료 시 자동 스크롤
  const resultRef = useRef<HTMLDivElement | null>(null)
  useEffect(() => {
    if (phaseResult) resultRef.current?.scrollIntoView({ behavior: 'smooth', block: 'start' })
  }, [phaseResult])

  // 기존 run_all.py 세밀 검증 (Phase 1 상세)
  const [detailRunning, setDetailRunning] = useState(false)
  const [detailResult, setDetailResult]   = useState<VerResult | null>(null)
  const [expandedModule, setExpandedModule] = useState<string | null>(null)
  const [legacyReportMd, setLegacyReportMd] = useState('')

  async function runPhase(targetPhase: Phase) {
    if (activeJob) {
      show('이미 검증이 진행 중입니다. 완료 후 재시도하세요.', 'err')
      return
    }
    // verify_lib 항목이 있는 phase 에서 선택 항목 0개 → 실행 거부
    const targetItems: VerifyItemMeta[] = (verifyMeta?.phases || [])
      .find(p => p.phase === targetPhase)?.items || []
    const useDynamic = targetItems.length > 0
    if (useDynamic && selectedItems.size === 0) {
      show('선택된 항목이 없습니다. 프리셋을 누르거나 항목을 체크하세요.', 'err')
      return
    }
    setProgress(null)
    try {
      const baseOpts = targetPhase === 1
        ? { skip_build: skipBuild, skip_reset: skipReset }
        : { skip_build: skipBuild, skip_pkg: skipPkg, keep_agent: keepAgent }
      const body: Record<string, unknown> = { ...baseOpts, async: true }
      if (useDynamic) {
        // verify_lib 활성 phase — 선택 항목 전송. phase 3 의 의존성 정합성은 backend 가 정렬.
        body.items = Array.from(selectedItems)
      }
      const start = await api.post<PhaseJobStart>(`/verification/phases/${targetPhase}`, body)
      setActiveJob({ phase: targetPhase, jobId: start.job_id, startedAt: start.started_at })
    } catch (e: unknown) {
      show(String(e), 'err')
      setActiveJob(null)
    }
  }

  // 폴링 effect — activeJob 이 set 될 때마다 (직접 시작 OR sessionStorage 에서 복원)
  // 자동으로 1.5초 간격 폴링. cleanup 으로 cancel 처리.
  useEffect(() => {
    if (!activeJob) return
    let cancelled = false
    const job = activeJob
    ;(async () => {
      while (!cancelled) {
        try {
          const s = await api.get<PhaseJobStatus>(`/verification/jobs/${job.jobId}`)
          if (cancelled) return
          setProgress(s)
          if (s.done) {
            const verdict = s.verdict || 'UNKNOWN'
            const result: PhaseRunResult = {
              phase: s.phase,
              verdict,
              returncode: s.returncode ?? -1,
              report_path: s.report_path,
              report_ts: s.report_ts,
              stdout_tail: s.stdout_tail,
              argv: s.argv,
            }
            // 결과는 phase 별 sessionStorage 에 저장 — 다른 phase 탭 보고 있어도 보존.
            _ssSet(_resultKey(s.phase as Phase), result)
            // 현재 보고 있는 탭이 이 phase 면 즉시 화면 갱신
            if (s.phase === phase) {
              setPhaseResultRaw(result)
            }
            // 리포트도 같이 로드
            try {
              const rep = await api.get<PhaseReport>(`/verification/phases/${s.phase}/latest-report`)
              _ssSet(_reportKey(s.phase as Phase), rep)
              if (s.phase === phase) setPhaseReportRaw(rep)
            } catch { /* ignore */ }
            show(`Phase ${s.phase}: ${verdict}`, verdict === 'PASS' ? 'ok' : 'err')
            setActiveJob(null)
            setProgress(null)
            return
          }
        } catch (e: unknown) {
          // 서버 재시작 / job GC 로 추적 불가 — activeJob 정리 후 종료
          if (!cancelled) {
            show(`job ${job.jobId} 추적 실패 — 진행 상태 모니터링 종료`, 'err')
            setActiveJob(null)
            setProgress(null)
          }
          return
        }
        await new Promise(res => setTimeout(res, 1500))
      }
    })()
    return () => { cancelled = true }
    // phase 변경 시에는 폴링 자체는 계속하되, "현재 phase 화면 갱신" 분기만 영향받음.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeJob?.jobId])

  function formatElapsed(sec: number): string {
    const m = Math.floor(sec / 60)
    const s = Math.floor(sec % 60)
    return m > 0 ? `${m}분 ${s}초` : `${s}초`
  }

  async function loadLatestReport() {
    try {
      const rep = await api.get<PhaseReport>(`/verification/phases/${phase}/latest-report`)
      setPhaseReport(rep)
    } catch (e: unknown) {
      show(String(e), 'err')
    }
  }

  async function runDetail() {
    setDetailRunning(true)
    setDetailResult(null)
    setLegacyReportMd('')
    try {
      const r = await api.post<VerResult>('/verification/run', {})
      setDetailResult(r)
      const rate = r.total > 0 ? ((r.pass / r.total) * 100).toFixed(1) : '0'
      show(`상세 검증 완료: ${r.pass}/${r.total} PASS (${rate}%)`, r.fail === 0 ? 'ok' : 'err')
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setDetailRunning(false)
    }
  }

  async function loadLegacyReport() {
    try {
      const r = await api.get<{ content: string }>('/verification/report')
      setLegacyReportMd(r.content)
    } catch (e: unknown) {
      show(String(e), 'err')
    }
  }

  const verdictColor = (v: string) =>
    v === 'PASS' ? 'var(--success, #22c55e)'
    : v === 'FAIL' ? 'var(--danger)'
    : 'var(--muted, #888)'

  return (
    <div className="page">
      {/* ── Phase 1/2/3 통합 검증 (cims.sh verify phaseN) ── */}
      <div style={{ marginBottom: 24 }}>
        <div style={{ display: 'flex', gap: 8, marginBottom: 12 }}>
          {([1, 2, 3] as Phase[]).map(p => (
            <button
              key={p}
              className={`btn ${phase === p ? 'btn--primary' : 'btn--outline'}`}
              onClick={() => setPhase(p)}    /* setPhase 가 해당 phase 의 저장된 결과 자동 복원 */
              /* 탭 전환은 진행 중에도 허용 — 다른 phase 결과 조회 가능. 실행 버튼만 비활성화. */
            >
              {PHASE_LABEL[p]}
              {activeJob?.phase === p && <span style={{ marginLeft: 6, fontSize: 11 }}>⏳</span>}
            </button>
          ))}
        </div>

        <div style={{ padding: '12px 16px', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6, marginBottom: 12 }}>
          <div style={{ fontSize: 13, color: 'var(--muted, #666)', marginBottom: 8 }}>{PHASE_DESC[phase]}</div>

          {/* 동적 항목 선택 영역 — verify_lib registry 가 채워진 phase 만 표시 (현재 Phase 3 만) */}
          {hasDynamicItems && (
            <div style={{ marginBottom: 14, paddingBottom: 12, borderBottom: '1px dashed var(--border)' }}>
              {currentPhasePresets.length > 0 && (
                <div style={{ display: 'flex', gap: 6, alignItems: 'center', marginBottom: 8, flexWrap: 'wrap' }}>
                  <span style={{ fontSize: 12, color: 'var(--muted, #888)' }}>프리셋:</span>
                  {currentPhasePresets.map(p => (
                    <button
                      key={p.name}
                      className="btn btn--sm btn--outline"
                      onClick={() => applyPreset(p)}
                      disabled={running}
                      title={`${p.items.length}개 항목 선택`}
                    >
                      {p.name} ({p.items.length})
                    </button>
                  ))}
                  <button
                    className="btn btn--sm btn--outline"
                    onClick={() => setSelectedItems(new Set())}
                    disabled={running}
                  >
                    전체 해제
                  </button>
                  <span style={{ marginLeft: 'auto', fontSize: 12, color: 'var(--muted, #888)' }}>
                    선택: <b>{selectedItems.size}</b> / {currentPhaseItems.length}
                  </span>
                </div>
              )}
              {/* 카테고리별 그룹 — 환경/시나리오/검증 등 */}
              {(() => {
                const byCategory: Record<string, VerifyItemMeta[]> = {}
                for (const it of currentPhaseItems) {
                  ;(byCategory[it.category || '기타'] ||= []).push(it)
                }
                const cats = Object.keys(byCategory).sort()
                return cats.map(cat => (
                  <div key={cat} style={{ marginBottom: 6 }}>
                    <div style={{ fontSize: 11, color: 'var(--muted, #888)', marginBottom: 4 }}>{cat}</div>
                    <div style={{ display: 'flex', flexWrap: 'wrap', gap: 12 }}>
                      {byCategory[cat].map(it => (
                        <label key={it.id} style={{ display: 'flex', alignItems: 'center', gap: 5, fontSize: 12, padding: '2px 6px', background: 'var(--bg-muted, #f5f5f5)', borderRadius: 4 }} title={it.description || ''}>
                          <input
                            type="checkbox"
                            checked={selectedItems.has(it.id)}
                            onChange={() => toggleItem(it.id)}
                            disabled={running}
                          />
                          <span style={{ fontFamily: 'monospace' }}>{it.id}</span>
                          <span style={{ color: 'var(--muted, #666)' }}>· {it.name}</span>
                        </label>
                      ))}
                    </div>
                  </div>
                ))
              })()}
            </div>
          )}

          <div style={{ display: 'flex', flexWrap: 'wrap', gap: 16, alignItems: 'center' }}>
            <label style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 13 }}>
              <input type="checkbox" checked={skipBuild} onChange={e => setSkipBuild(e.target.checked)} disabled={running} />
              --skip-build
            </label>
            {phase === 1 ? (
              <label style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 13 }}>
                <input type="checkbox" checked={skipReset} onChange={e => setSkipReset(e.target.checked)} disabled={running} />
                --skip-reset
              </label>
            ) : (
              <>
                <label style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 13 }}>
                  <input type="checkbox" checked={skipPkg} onChange={e => setSkipPkg(e.target.checked)} disabled={running} />
                  --skip-pkg
                </label>
                <label style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 13 }}>
                  <input type="checkbox" checked={keepAgent} onChange={e => setKeepAgent(e.target.checked)} disabled={running} />
                  --keep-agent
                </label>
              </>
            )}

            <div style={{ marginLeft: 'auto', display: 'flex', gap: 8 }}>
              <button className="btn btn--primary" onClick={() => runPhase(phase)} disabled={running}>
                {running
                  ? activeJob?.phase === phase
                    ? `Phase ${phase} 실행 중...`
                    : `Phase ${activeJob?.phase} 실행 중 (대기)`
                  : `▶ Phase ${phase} 실행`}
              </button>
              <button className="btn btn--outline" onClick={loadLatestReport} disabled={running}>
                📄 최신 리포트
              </button>
            </div>
          </div>
        </div>

        {/* 현재 보고 있는 phase 가 진행 중인 경우만 progress 패널 표시 */}
        {activeJob && activeJob.phase === phase && (
          <div style={{ padding: 16, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6, marginTop: 12 }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 10 }}>
              <span className="loader" style={{ width: 14, height: 14, border: '2px solid var(--border)', borderTopColor: 'var(--accent, #3b82f6)', borderRadius: '50%', animation: 'spin 0.8s linear infinite' }} />
              <span style={{ fontWeight: 600, fontSize: 14 }}>
                Phase {activeJob.phase} 실행 중
                <span className="ts" style={{ marginLeft: 8 }}>
                  경과 {formatElapsed(progress?.elapsed ?? ((Date.now() / 1000) - activeJob.startedAt))}
                </span>
              </span>
            </div>
            <div style={{ fontSize: 12, color: 'var(--muted, #888)', marginBottom: 8 }}>
              {activeJob.phase === 1 && 'Phase 1: 빌드 + 설정 + 시나리오 (수 분 소요)'}
              {activeJob.phase === 2 && 'Phase 2: 배포 메커니즘 검증 (약 1~2분)'}
              {activeJob.phase === 3 && 'Phase 3: 배포 + 4시나리오 (약 3~4분)'}
            </div>
            <pre style={{
              margin: 0, padding: 12, background: 'var(--bg-muted, #1a1a1a)', color: 'var(--text-muted, #ccc)',
              borderRadius: 4, fontSize: 11, lineHeight: 1.4,
              maxHeight: 280, overflowY: 'auto', whiteSpace: 'pre-wrap',
            }}>
              {progress?.stdout_tail || '(폴링 시작 중...)'}
            </pre>
            <style>{`@keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }`}</style>
          </div>
        )}

        {/* 결과 패널 — 진행 중이 아닐 때 (또는 진행 중이지만 다른 phase 보고 있을 때) 항상 표시 */}
        {phaseResult && !(activeJob && activeJob.phase === phase) && (
          <div ref={resultRef} style={{ padding: 16, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6, marginTop: 12 }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 16, marginBottom: 12 }}>
              <span style={{ fontSize: 16, fontWeight: 700, color: verdictColor(phaseResult.verdict) }}>
                Phase {phaseResult.phase}: {phaseResult.verdict}
              </span>
              <span className="ts">returncode={phaseResult.returncode}</span>
              {phaseResult.report_ts && <span className="ts">report_ts={phaseResult.report_ts}</span>}
            </div>

            <details style={{ marginTop: 8 }}>
              <summary style={{ cursor: 'pointer', fontSize: 13 }}>stdout (마지막 40줄)</summary>
              <pre style={{ marginTop: 8, padding: 12, background: 'var(--bg-muted, #1a1a1a)', color: 'var(--text-muted, #ccc)',
                borderRadius: 4, fontSize: 11, lineHeight: 1.4, maxHeight: 300, overflowY: 'auto', whiteSpace: 'pre-wrap' }}>
                {phaseResult.stdout_tail}
              </pre>
            </details>
          </div>
        )}

        {phaseReport && (
          <div style={{ marginTop: 12 }}>
            <div style={{ fontWeight: 600, marginBottom: 6, fontSize: 13 }}>
              📄 검증 리포트 <span className="ts">({phaseReport.ts})</span>
            </div>
            <pre style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6,
              padding: 16, fontSize: 11, lineHeight: 1.5, maxHeight: 500, overflowY: 'auto', whiteSpace: 'pre-wrap' }}>
              {phaseReport.content}
            </pre>
          </div>
        )}
      </div>

      {/* ── 기존 run_all.py 세밀 검증 (Phase 1 상세 테스트 세트) ── */}
      <div style={{ borderTop: '1px solid var(--border)', paddingTop: 20 }}>
        <div style={{ display: 'flex', alignItems: 'center', marginBottom: 12 }}>
          <div>
            <div style={{ fontWeight: 600, fontSize: 14 }}>Phase 1 상세 검증 (run_all.py)</div>
            <div style={{ fontSize: 12, color: 'var(--muted, #888)' }}>tests/run_all.py 기반 모듈별 개별 테스트 항목</div>
          </div>
          <div style={{ marginLeft: 'auto', display: 'flex', gap: 8 }}>
            <button className="btn btn--outline" onClick={runDetail} disabled={detailRunning}>
              {detailRunning ? '상세 검증 중...' : '상세 검증 실행'}
            </button>
            <button className="btn btn--outline" onClick={loadLegacyReport} disabled={detailRunning}>리포트</button>
          </div>
        </div>

        {detailRunning && <div className="empty" style={{ padding: 24, fontSize: 13 }}>상세 검증 실행 중... (약 3~4분)</div>}

        {detailResult && !detailRunning && (
          <>
            <div className="table-wrap" style={{ marginBottom: 16 }}>
              <table className="data-table">
                <thead><tr><th>모듈</th><th style={{ width: 60 }}>전체</th><th style={{ width: 60 }}>PASS</th><th style={{ width: 60 }}>FAIL</th><th style={{ width: 80 }}>합격률</th></tr></thead>
                <tbody>
                  {detailResult.modules.map(m => (
                    <tr key={m.module} style={{ cursor: 'pointer' }} onClick={() => setExpandedModule(expandedModule === m.module ? null : m.module)}>
                      <td style={{ fontWeight: 600 }}>
                        <span style={{ marginRight: 6 }}>{expandedModule === m.module ? '▼' : '▶'}</span>
                        {m.module}
                      </td>
                      <td>{m.total}</td>
                      <td style={{ color: 'var(--success, #22c55e)' }}>{m.pass}</td>
                      <td style={{ color: m.fail > 0 ? 'var(--danger)' : undefined }}>{m.fail}</td>
                      <td><span className={`badge ${m.fail === 0 ? 'badge--green' : 'badge--red'}`}>
                        {m.total > 0 ? ((m.pass / m.total) * 100).toFixed(0) : 0}%
                      </span></td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>

            {expandedModule && (() => {
              const mod = detailResult.modules.find(m => m.module === expandedModule)
              if (!mod) return null
              return (
                <div className="table-wrap">
                  <table className="data-table">
                    <thead><tr><th>ID</th><th>항목</th><th style={{ width: 60 }}>결과</th><th style={{ width: 60 }}>시간</th><th>상세</th></tr></thead>
                    <tbody>
                      {mod.results.map(r => (
                        <tr key={r.id}>
                          <td className="ts">{r.id}</td>
                          <td style={{ fontSize: 12 }}>{r.name}</td>
                          <td><span className={`badge ${r.status === 'PASS' ? 'badge--green' : r.status === 'FAIL' ? 'badge--red' : 'badge--gray'}`}>{r.status}</span></td>
                          <td className="ts">{r.elapsed_ms}ms</td>
                          <td style={{ fontSize: 11, maxWidth: 300, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}
                            title={r.detail}>{r.detail || '—'}</td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              )
            })()}
          </>
        )}

        {legacyReportMd && (
          <div style={{ marginTop: 16 }}>
            <div style={{ fontWeight: 600, marginBottom: 8, fontSize: 13 }}>tests/verification_report.md</div>
            <pre style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6,
              padding: 16, fontSize: 12, lineHeight: 1.6, maxHeight: 400, overflowY: 'auto', whiteSpace: 'pre-wrap' }}>
              {legacyReportMd}
            </pre>
          </div>
        )}
      </div>
    </div>
  )
}
