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
  }
  const [running, setRunning] = useState(false)
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
  // 영속화 wrapper — setter 호출 시 sessionStorage 자동 동기화
  const setPhaseResult = (r: PhaseRunResult | null) => {
    setPhaseResultRaw(r)
    if (r) _ssSet(_resultKey(r.phase as Phase), r)
  }
  const setPhaseReport = (rep: PhaseReport | null) => {
    setPhaseReportRaw(rep)
    if (rep) _ssSet(_reportKey(rep.phase as Phase), rep)
  }
  // 실행 진행 상태 (job 폴링)
  const [progress, setProgress] = useState<PhaseJobStatus | null>(null)
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
    // 클릭 시점의 phase 를 명시적으로 캡처 — runPhase 내부에서 phase state 가
    // 다른 사이드 이펙트로 변경되더라도 실행 흐름 / 결과 표시는 targetPhase 기준 유지.
    setRunning(true)
    setPhaseResult(null)
    setPhaseReport(null)
    setProgress(null)
    try {
      const body = targetPhase === 1
        ? { skip_build: skipBuild, skip_reset: skipReset, async: true }
        : { skip_build: skipBuild, skip_pkg: skipPkg, keep_agent: keepAgent, async: true }
      // 1) job 시작 — 즉시 job_id 반환
      const start = await api.post<PhaseJobStart>(`/verification/phases/${targetPhase}`, body)
      // 2) 1.5초 간격 폴링 — 진행 중 stdout tail 실시간 갱신
      const pollMs = 1500
      let final: PhaseJobStatus | null = null
      while (true) {
        await new Promise(res => setTimeout(res, pollMs))
        const s = await api.get<PhaseJobStatus>(`/verification/jobs/${start.job_id}`)
        setProgress(s)
        if (s.done) { final = s; break }
      }
      // 3) 완료 — 결과 + 리포트 즉시 표시
      const verdict = final.verdict || 'UNKNOWN'
      setPhaseResult({
        phase: final.phase,
        verdict,
        returncode: final.returncode ?? -1,
        report_path: final.report_path,
        report_ts: final.report_ts,
        stdout_tail: final.stdout_tail,
        argv: final.argv,
      })
      show(`Phase ${targetPhase}: ${verdict}`, verdict === 'PASS' ? 'ok' : 'err')
      try {
        const rep = await api.get<PhaseReport>(`/verification/phases/${targetPhase}/latest-report`)
        setPhaseReport(rep)
      } catch { /* ignore */ }
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setRunning(false)
      setProgress(null)
    }
  }

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
              disabled={running}
            >
              {PHASE_LABEL[p]}
            </button>
          ))}
        </div>

        <div style={{ padding: '12px 16px', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6, marginBottom: 12 }}>
          <div style={{ fontSize: 13, color: 'var(--muted, #666)', marginBottom: 8 }}>{PHASE_DESC[phase]}</div>

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
                {running ? `Phase ${phase} 실행 중...` : `▶ Phase ${phase} 실행`}
              </button>
              <button className="btn btn--outline" onClick={loadLatestReport} disabled={running}>
                📄 최신 리포트
              </button>
            </div>
          </div>
        </div>

        {running && (
          <div style={{ padding: 16, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6, marginTop: 12 }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 10 }}>
              <span className="loader" style={{ width: 14, height: 14, border: '2px solid var(--border)', borderTopColor: 'var(--accent, #3b82f6)', borderRadius: '50%', animation: 'spin 0.8s linear infinite' }} />
              <span style={{ fontWeight: 600, fontSize: 14 }}>
                Phase {progress?.phase ?? phase} 실행 중
                {progress?.elapsed != null && (
                  <span className="ts" style={{ marginLeft: 8 }}>
                    경과 {formatElapsed(progress.elapsed)}
                  </span>
                )}
              </span>
            </div>
            <div style={{ fontSize: 12, color: 'var(--muted, #888)', marginBottom: 8 }}>
              {(progress?.phase ?? phase) === 1 && 'Phase 1: 빌드 + 설정 + 시나리오 (수 분 소요)'}
              {(progress?.phase ?? phase) === 2 && 'Phase 2: 배포 메커니즘 검증 (약 1~2분)'}
              {(progress?.phase ?? phase) === 3 && 'Phase 3: 배포 + 4시나리오 (약 3~4분)'}
            </div>
            <pre style={{
              margin: 0, padding: 12, background: 'var(--bg-muted, #1a1a1a)', color: 'var(--text-muted, #ccc)',
              borderRadius: 4, fontSize: 11, lineHeight: 1.4,
              maxHeight: 280, overflowY: 'auto', whiteSpace: 'pre-wrap',
            }}>
              {progress?.stdout_tail || '(시작 대기 중...)'}
            </pre>
            <style>{`@keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }`}</style>
          </div>
        )}

        {phaseResult && !running && (
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
