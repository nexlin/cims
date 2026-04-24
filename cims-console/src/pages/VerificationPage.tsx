import { useState } from 'react'
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

const PHASE_DESC: Record<Phase, string> = {
  1: '개발·보완 + 회귀 6항목. `build/dist/<모듈>/` 직접 기동.',
  2: 'tarball → TB-CSC → Test-agent 배포 메커니즘. csc 4445 overlay start/health/stop.',
  3: '배포 이후 기본 4시나리오 재수행 (VoLTE 음성/영상, PTT 그룹 음성/영상).',
}

export default function VerificationPage() {
  const { show } = useToast()

  // Phase 1/2/3 통합 탭
  const [phase, setPhase] = useState<Phase>(3)
  const [running, setRunning] = useState(false)
  const [skipBuild, setSkipBuild] = useState(true)
  const [skipPkg, setSkipPkg]     = useState(true)
  const [keepAgent, setKeepAgent] = useState(false)
  const [phaseResult, setPhaseResult] = useState<PhaseRunResult | null>(null)
  const [phaseReport, setPhaseReport] = useState<PhaseReport | null>(null)

  // 기존 run_all.py 세밀 검증 (Phase 1 상세)
  const [detailRunning, setDetailRunning] = useState(false)
  const [detailResult, setDetailResult]   = useState<VerResult | null>(null)
  const [expandedModule, setExpandedModule] = useState<string | null>(null)
  const [legacyReportMd, setLegacyReportMd] = useState('')

  async function runPhase() {
    setRunning(true)
    setPhaseResult(null)
    setPhaseReport(null)
    try {
      const body = { skip_build: skipBuild, skip_pkg: skipPkg, keep_agent: keepAgent }
      const r = await api.post<PhaseRunResult>(`/verification/phases/${phase}`, body)
      setPhaseResult(r)
      show(`Phase ${phase}: ${r.verdict}`, r.verdict === 'PASS' ? 'ok' : 'err')
      // 자동으로 최신 리포트 로드
      try {
        const rep = await api.get<PhaseReport>(`/verification/phases/${phase}/latest-report`)
        setPhaseReport(rep)
      } catch { /* ignore */ }
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setRunning(false)
    }
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
              onClick={() => { setPhase(p); setPhaseResult(null); setPhaseReport(null) }}
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
            <label style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 13 }}>
              <input type="checkbox" checked={skipPkg} onChange={e => setSkipPkg(e.target.checked)} disabled={running} />
              --skip-pkg
            </label>
            {phase !== 1 && (
              <label style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 13 }}>
                <input type="checkbox" checked={keepAgent} onChange={e => setKeepAgent(e.target.checked)} disabled={running} />
                --keep-agent
              </label>
            )}

            <div style={{ marginLeft: 'auto', display: 'flex', gap: 8 }}>
              <button className="btn btn--primary" onClick={runPhase} disabled={running}>
                {running ? `Phase ${phase} 실행 중...` : `▶ Phase ${phase} 실행`}
              </button>
              <button className="btn btn--outline" onClick={loadLatestReport} disabled={running}>
                📄 최신 리포트
              </button>
            </div>
          </div>
        </div>

        {running && (
          <div className="empty" style={{ padding: 32, fontSize: 13 }}>
            cims.sh verify phase{phase} 실행 중...
            {phase === 1 && ' (Phase 1: 빌드 + 설정 + 시나리오 포함 — 수 분 소요)'}
            {phase === 2 && ' (Phase 2: 배포 메커니즘 검증 — 약 1~2분)'}
            {phase === 3 && ' (Phase 3: 배포 + 4시나리오 — 약 3~4분)'}
          </div>
        )}

        {phaseResult && !running && (
          <div style={{ padding: 16, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6, marginTop: 12 }}>
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
