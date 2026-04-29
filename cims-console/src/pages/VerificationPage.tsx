import { useState, useRef, useEffect, Fragment } from 'react'
import { api } from '../api/client'
import { useToast } from '../components/Toast'

// ─────────────────────────────────────────────────────────────
// 타입 정의
// ─────────────────────────────────────────────────────────────
interface PhaseRunResult {
  phase: number
  verdict: string            // PASS | FAIL | UNKNOWN
  returncode: number
  report_path: string | null
  report_ts: string
  stdout_tail: string
  argv: string[]
  items_progress?: ItemsProgress | null
}

interface PhaseJobStart {
  job_id: string
  phase: number
  argv: string[]
  started_at: number
  message: string
}

// 진행 표 데이터 (backend _parse_items_progress 응답 매핑)
interface ItemProgressChild {
  id: string
  name: string
  status: string             // PASS | FAIL | SKIP | RUNNING
  elapsed_ms: number
}
interface ItemProgress {
  id: string
  name: string
  status: string             // PASS | FAIL | SKIP | RUNNING
  elapsed_ms: number
  idx?: number
  children: ItemProgressChild[]
}
interface ItemsProgress {
  selected: string[]
  total: number
  completed: number
  current: string | null
  items: ItemProgress[]
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
  items_progress?: ItemsProgress | null
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
type VerifyMode = 'full' | 'quick' | 'volte' | 'ptt'

const PHASE_LABEL: Record<Phase, string> = {
  1: 'Phase 1 · 배포 전',
  2: 'Phase 2 · 배포 과정',
  3: 'Phase 3 · 배포 이후',
}

// Phase 별 단계 상세 (① 단계 상세)
const PHASE_DETAIL: Record<Phase, { title: string; desc: string; entry: string; duration: string }> = {
  1: {
    title: 'Phase 1 · 배포 전 회귀 검증',
    desc: '개발/보완 후 build/dist/ 안에서 직접 기동하여 기능 회귀 + 보완 동작 확인. ens160 IP 자동 감지.',
    entry: 'TB 3종 동작 + git clean 권장',
    duration: '약 5~10분 (전체) / 10초 (신속)',
  },
  2: {
    title: 'Phase 2 · 배포 과정 + 환경 구축',
    desc: 'tarball → TB-CSC(4419) → Test-agent 배포 체인. csc 4445 + console 8081 overlay + csp/cmp/sim 설치 (22단계).',
    entry: 'Phase 1 PASS + tarball 최신 (pkg --no-bump)',
    duration: '약 1~2분',
  },
  3: {
    title: 'Phase 3 · 서비스 검증 전용',
    desc: '배포본에서 기본 4시나리오 (VoLTE 음성/영상, PTT 그룹 음성/영상) 재수행. 데이터 wipe 없음.',
    entry: 'Phase 2 완료 — 4445/8081/5060/9000 LISTEN',
    duration: '약 3~4분',
  },
}

// 검증 모드 → 프리셋 매핑 (Phase 2 는 단일 항목이라 모드 선택 비활성)
const PHASE_MODES: Record<Phase, Array<{ value: VerifyMode; label: string; preset: string }>> = {
  1: [
    { value: 'full',    label: '전체 (full)',    preset: 'phase1-full' },
    { value: 'quick',   label: '신속 (quick)',   preset: 'phase1-quick' },
  ],
  2: [
    { value: 'full',    label: '전체 (full)',    preset: 'phase2-full' },
  ],
  3: [
    { value: 'full',    label: '전체 (full)',    preset: 'phase3-full' },
    { value: 'volte',   label: 'VoLTE (volte)',  preset: 'phase3-volte' },
    { value: 'ptt',     label: 'PTT (ptt)',      preset: 'phase3-ptt' },
  ],
}
const DEFAULT_MODE: Record<Phase, VerifyMode> = { 1: 'full', 2: 'full', 3: 'full' }

// ─────────────────────────────────────────────────────────────
// sessionStorage 영속화 헬퍼
// ─────────────────────────────────────────────────────────────
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
const _modeKey   = (p: Phase) => `verify.phase${p}.mode`
const _activeJobKey = 'verify.activeJob'

interface ActiveJob {
  phase: Phase
  jobId: string
  startedAt: number
}

// ─────────────────────────────────────────────────────────────
// ProgressTable — ⑤ 항목별 진행/결과 표 (PDF 인쇄 대상)
// ─────────────────────────────────────────────────────────────
// 결과 라벨: 미진행 / 진행중 / PASS / FAILED / SKIP (의존 실패 시)
function statusLabel(s: string): string {
  if (s === 'PASS')    return 'PASS'
  if (s === 'FAIL')    return 'FAILED'
  if (s === 'SKIP')    return 'SKIP'
  if (s === 'RUNNING') return '진행중'
  return '미진행'
}
function statusIcon(s: string): string {
  if (s === 'PASS')    return '✓'
  if (s === 'FAIL')    return '✗'
  if (s === 'SKIP')    return '·'
  if (s === 'RUNNING') return '⏳'
  return '○'
}
function statusColor(s: string): string {
  if (s === 'PASS')    return 'var(--success, #22c55e)'
  if (s === 'FAIL')    return 'var(--danger, #ef4444)'
  if (s === 'SKIP')    return 'var(--muted, #888)'
  if (s === 'RUNNING') return 'var(--accent, #3b82f6)'
  return 'var(--muted, #aaa)'
}
function fmtMs(ms: number): string {
  if (!ms) return '–'
  if (ms < 1000) return `${ms}ms`
  if (ms < 60000) return `${(ms / 1000).toFixed(1)}s`
  const m = Math.floor(ms / 60000); const s = Math.floor((ms % 60000) / 1000)
  return `${m}분 ${s}초`
}

function ProgressTable({
  selectedIds, items, currentItemId, metaById,
  selectedSet, onToggle, disabled,
}: {
  selectedIds: string[]
  items: ItemProgress[]
  currentItemId: string | null
  metaById: Record<string, VerifyItemMeta>
  selectedSet: Set<string>
  onToggle: (id: string) => void
  disabled: boolean
}) {
  // 선택 항목 기준으로 빈 자리도 row 생성 (실행 전에도 표 미리보기 가능)
  const byId: Record<string, ItemProgress> = {}
  for (const it of items) byId[it.id] = it
  const rowIds = selectedIds.length ? selectedIds : items.map(it => it.id)
  const totalCount = rowIds.length || 1
  const completedCount = rowIds.filter(id => {
    const it = byId[id]
    return it && (it.status === 'PASS' || it.status === 'FAIL' || it.status === 'SKIP')
  }).length
  const overallPct = Math.round((completedCount / totalCount) * 100)

  return (
    <div className="verify-progress-table table-wrap">
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 6, fontSize: 12 }}>
        <b>전체 진행률</b>
        <div style={{ flex: 1, height: 8, background: 'var(--bg-muted, #eee)', borderRadius: 4, overflow: 'hidden' }}>
          <div style={{ width: `${overallPct}%`, height: '100%', background: 'var(--accent, #3b82f6)', transition: 'width 0.3s' }} />
        </div>
        <span className="ts">{completedCount}/{totalCount} ({overallPct}%)</span>
      </div>
      <table className="data-table verify-progress-table-inner">
        <thead>
          {/* 1행 — 그룹 헤더 (검증 항목 / 진행상태) */}
          <tr>
            <th rowSpan={2} style={{ width: 32 }} className="no-print">
              <input
                type="checkbox"
                checked={rowIds.length > 0 && rowIds.every(id => selectedSet.has(id))}
                onChange={() => {
                  // 토글: 모두 선택되어 있으면 모두 해제, 아니면 모두 선택
                  const allSelected = rowIds.length > 0 && rowIds.every(id => selectedSet.has(id))
                  rowIds.forEach(id => {
                    if (allSelected ? selectedSet.has(id) : !selectedSet.has(id)) onToggle(id)
                  })
                }}
                disabled={disabled}
                title="전체 선택/해제"
              />
            </th>
            <th rowSpan={2} style={{ width: 36 }}>#</th>
            <th colSpan={2} style={{ textAlign: 'center' }}>검증 항목</th>
            <th colSpan={2} style={{ textAlign: 'center' }}>진행상태</th>
            <th rowSpan={2} style={{ width: 110, textAlign: 'center' }}>결과</th>
          </tr>
          {/* 2행 — 서브 헤더 (검증명 / 검증내용 // 진행률 / 소요시간) */}
          <tr>
            <th style={{ width: 200 }}>검증명</th>
            <th>검증내용</th>
            <th style={{ width: 150 }}>진행률 (%)</th>
            <th style={{ width: 80 }}>소요시간</th>
          </tr>
        </thead>
        <tbody>
          {rowIds.map((rid, i) => {
            const it = byId[rid] || { id: rid, name: rid, status: 'PENDING', elapsed_ms: 0, children: [] }
            const meta = metaById[rid]
            const itemName = it.name || meta?.name || rid
            const itemDesc = meta?.description || meta?.category || ''
            const isCurrent = currentItemId === rid
            const isDone = it.status === 'PASS' || it.status === 'FAIL' || it.status === 'SKIP'
            const pct = isDone ? 100 : (isCurrent ? 50 : 0)
            const checked = selectedSet.has(rid)
            return (
              <Fragment key={rid}>
                <tr className={isCurrent ? 'is-current' : ''}>
                  <td className="no-print" style={{ textAlign: 'center' }}>
                    <input
                      type="checkbox"
                      checked={checked}
                      onChange={() => onToggle(rid)}
                      disabled={disabled}
                    />
                  </td>
                  <td className="ts">{i + 1}</td>
                  <td>
                    <div style={{ fontFamily: 'monospace', fontSize: 12, fontWeight: 600 }}>{rid}</div>
                    <div style={{ fontSize: 11, color: 'var(--muted, #888)' }}>{itemName}</div>
                  </td>
                  <td style={{ fontSize: 11, color: 'var(--muted, #666)' }}>
                    {itemDesc || '—'}
                  </td>
                  <td>
                    <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                      <div style={{ flex: 1, height: 6, background: 'var(--bg-muted, #eee)', borderRadius: 3, overflow: 'hidden' }}>
                        <div style={{
                          width: `${pct}%`, height: '100%',
                          background: isDone ? statusColor(it.status) : 'var(--accent, #3b82f6)',
                          transition: 'width 0.3s',
                        }} />
                      </div>
                      <span className="ts" style={{ minWidth: 36, textAlign: 'right' }}>{pct}%</span>
                    </div>
                  </td>
                  <td className="ts" style={{ textAlign: 'right' }}>
                    {isDone ? fmtMs(it.elapsed_ms) : (isCurrent ? '–' : '–')}
                  </td>
                  <td>
                    <span style={{
                      display: 'inline-flex', alignItems: 'center', gap: 4,
                      color: statusColor(it.status), fontWeight: 600, fontSize: 13,
                    }}>
                      {statusIcon(it.status)} {statusLabel(it.status)}
                    </span>
                  </td>
                </tr>
                {it.children && it.children.map((c, ci) => {
                  const cDone = c.status === 'PASS' || c.status === 'FAIL' || c.status === 'SKIP'
                  const cIsCurrent = c.status === 'RUNNING'
                  const cPct = cDone ? 100 : (cIsCurrent ? 50 : 0)
                  return (
                    <tr key={`${rid}.${c.id}`} className="child-row">
                      <td className="no-print" />
                      <td className="ts" style={{ color: 'var(--muted, #888)' }}>
                        {i + 1}.{ci + 1}
                      </td>
                      <td style={{ paddingLeft: 18 }}>
                        <div style={{ fontFamily: 'monospace', fontSize: 11 }}>└ {c.id}</div>
                      </td>
                      <td style={{ fontSize: 10, color: 'var(--muted, #888)' }}>{c.name}</td>
                      <td>
                        <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                          <div style={{ flex: 1, height: 4, background: 'var(--bg-muted, #eee)', borderRadius: 2, overflow: 'hidden' }}>
                            <div style={{
                              width: `${cPct}%`, height: '100%',
                              background: cDone ? statusColor(c.status) : 'var(--accent, #3b82f6)',
                            }} />
                          </div>
                          <span className="ts" style={{ minWidth: 30, textAlign: 'right', fontSize: 10 }}>{cPct}%</span>
                        </div>
                      </td>
                      <td className="ts" style={{ textAlign: 'right', fontSize: 10 }}>
                        {cDone ? fmtMs(c.elapsed_ms) : '–'}
                      </td>
                      <td>
                        <span style={{ color: statusColor(c.status), fontWeight: 600, fontSize: 11 }}>
                          {statusIcon(c.status)} {statusLabel(c.status)}
                        </span>
                      </td>
                    </tr>
                  )
                })}
              </Fragment>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}

// ─────────────────────────────────────────────────────────────
// VerificationPage — 메인
// ─────────────────────────────────────────────────────────────
export default function VerificationPage() {
  const { show } = useToast()

  const [phase, setPhaseRaw] = useState<Phase>(() => {
    if (typeof window === 'undefined') return 1
    const saved = window.sessionStorage.getItem('verify.phase')
    return (saved === '1' || saved === '2' || saved === '3') ? (Number(saved) as Phase) : 1
  })
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
  const setPhaseReport = (rep: PhaseReport | null) => {
    setPhaseReportRaw(rep)
    if (rep) _ssSet(_reportKey(rep.phase as Phase), rep)
  }

  const [progress, setProgress] = useState<PhaseJobStatus | null>(null)
  const [verifyMeta, setVerifyMeta] = useState<VerifyItemsResponse | null>(null)
  const [selectedItems, setSelectedItems] = useState<Set<string>>(new Set())

  // 검증 모드 (③) — phase 별 sessionStorage. mount 시 default 적용.
  const [mode, setModeRaw] = useState<VerifyMode>(() => {
    if (typeof window === 'undefined') return DEFAULT_MODE[1]
    const saved = window.sessionStorage.getItem('verify.phase')
    const p = (saved === '1' || saved === '2' || saved === '3') ? (Number(saved) as Phase) : 1
    return (_ssGet<VerifyMode>(_modeKey(p)) as VerifyMode) || DEFAULT_MODE[p]
  })

  // 모드 → 프리셋 → selectedItems 동기화
  function applyMode(p: Phase, m: VerifyMode) {
    const modeOpt = PHASE_MODES[p].find(x => x.value === m)
    if (!modeOpt) return
    const preset = (verifyMeta?.presets || []).find(pp => pp.name === modeOpt.preset)
    if (preset) setSelectedItems(new Set(preset.items))
  }
  const setMode = (m: VerifyMode) => {
    setModeRaw(m)
    _ssSet(_modeKey(phase), m)
    applyMode(phase, m)
  }

  const setPhase = (p: Phase) => {
    setPhaseRaw(p)
    try { window.sessionStorage.setItem('verify.phase', String(p)) } catch { /* ignore */ }
    setPhaseResultRaw(_ssGet<PhaseRunResult>(_resultKey(p)))
    setPhaseReportRaw(_ssGet<PhaseReport>(_reportKey(p)))
    // mode: phase 별 저장값 또는 default
    const savedMode = _ssGet<VerifyMode>(_modeKey(p)) as VerifyMode | null
    const newMode = savedMode || DEFAULT_MODE[p]
    setModeRaw(newMode)
    _ssSet(_modeKey(p), newMode)
    applyMode(p, newMode)
  }

  // mount 시 메타 fetch + 현재 phase 의 mode 에 해당하는 프리셋 selectedItems 채움
  useEffect(() => {
    api.get<VerifyItemsResponse>('/verification/items')
      .then(r => {
        setVerifyMeta(r)
        const modeOpt = PHASE_MODES[phase].find(x => x.value === mode) || PHASE_MODES[phase][0]
        const preset = (r.presets || []).find(pp => pp.name === modeOpt.preset)
        if (preset) setSelectedItems(new Set(preset.items))
      })
      .catch(() => { /* 실패 시 동적 UI 비활성 */ })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const currentPhaseItems: VerifyItemMeta[] = (verifyMeta?.phases || [])
    .find(p => p.phase === phase)?.items || []
  const hasDynamicItems = currentPhaseItems.length > 0
  // 모든 phase 의 항목 메타를 ID 로 lookup (자식 메타도 잠재적으로 — 현재는 부모만)
  const metaById: Record<string, VerifyItemMeta> = {}
  for (const phs of (verifyMeta?.phases || [])) {
    for (const it of phs.items) metaById[it.id] = it
  }

  function toggleItem(id: string) {
    setSelectedItems(prev => {
      const n = new Set(prev)
      if (n.has(id)) n.delete(id); else n.add(id)
      return n
    })
  }

  const resultRef = useRef<HTMLDivElement | null>(null)
  useEffect(() => {
    if (phaseResult) resultRef.current?.scrollIntoView({ behavior: 'smooth', block: 'start' })
  }, [phaseResult])

  async function runPhase(targetPhase: Phase) {
    if (activeJob) {
      show('이미 검증이 진행 중입니다. 완료 후 재시도하세요.', 'err')
      return
    }
    const targetItems: VerifyItemMeta[] = (verifyMeta?.phases || [])
      .find(p => p.phase === targetPhase)?.items || []
    const useDynamic = targetItems.length > 0
    if (useDynamic && selectedItems.size === 0) {
      show('선택된 항목이 없습니다. 모드를 선택하거나 항목을 체크하세요.', 'err')
      return
    }
    setProgress(null)
    try {
      const baseOpts = targetPhase === 1
        ? { skip_build: skipBuild, skip_reset: skipReset }
        : { skip_build: skipBuild, skip_pkg: skipPkg, keep_agent: keepAgent }
      const body: Record<string, unknown> = { ...baseOpts, async: true }
      if (useDynamic) body.items = Array.from(selectedItems)
      const start = await api.post<PhaseJobStart>(`/verification/phases/${targetPhase}`, body)
      setActiveJob({ phase: targetPhase, jobId: start.job_id, startedAt: start.started_at })
    } catch (e: unknown) {
      show(String(e), 'err')
      setActiveJob(null)
    }
  }

  // 폴링 effect — 1.5초 간격
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
              items_progress: s.items_progress || null,
            }
            _ssSet(_resultKey(s.phase as Phase), result)
            if (s.phase === phase) setPhaseResultRaw(result)
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
        } catch (_e: unknown) {
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

  const verdictColor = (v: string) =>
    v === 'PASS' ? 'var(--success, #22c55e)'
    : v === 'FAIL' ? 'var(--danger)'
    : 'var(--muted, #888)'

  // 표 데이터: 진행 중에는 progress.items_progress, 완료 후에는 phaseResult.items_progress
  const liveProgress: ItemsProgress | null =
    (activeJob?.phase === phase ? (progress?.items_progress ?? null) : null)
    || (phaseResult?.items_progress ?? null)

  const selectedIdList: string[] = liveProgress?.selected?.length
    ? liveProgress.selected
    : Array.from(selectedItems)
  const tableItems: ItemProgress[] = liveProgress?.items || []
  const currentItem = liveProgress?.current ?? null

  // ⑥ 결과 정리 통계
  let pass = 0, fail = 0, skip = 0
  for (const it of tableItems) {
    if (it.status === 'PASS') pass++
    else if (it.status === 'FAIL') fail++
    else if (it.status === 'SKIP') skip++
  }
  const total = selectedIdList.length || tableItems.length

  return (
    <div className="page verify-page">
      {/* print stylesheet — Step D */}
      <style>{`
        @keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }
        .verify-page .verify-progress-table tr.is-current { background: rgba(59, 130, 246, 0.06); }
        .verify-page .print-only { display: none; }
        @media print {
          /* 1. UI chrome 숨김 */
          nav, .sidebar, header, .tabbar-actions, .toast,
          .verify-page .no-print, .verify-page button { display: none !important; }
          /* 2. 표 전체 너비 */
          .verify-page { padding: 0 !important; }
          .verify-page .verify-progress-table { width: 100%; font-size: 10pt; }
          .verify-page .verify-progress-table tr { page-break-inside: avoid; }
          /* 3. 인쇄 헤더 표시 */
          .verify-page .print-only { display: block !important; }
          /* 4. 접기 영역 숨김 */
          .verify-page details { display: none !important; }
        }
      `}</style>

      {/* 인쇄 시에만 보이는 헤더 — git/ts/phase */}
      <div className="print-only" style={{ marginBottom: 12, fontSize: 11, color: '#333' }}>
        <b>CIMS 검증 보고서 — Phase {phase}</b>
        {phaseResult?.report_ts && <> · <span>ts={phaseResult.report_ts}</span></>}
        {phaseResult?.verdict && <> · <span>판정: {phaseResult.verdict}</span></>}
      </div>

      {/* Phase 탭 */}
      <div style={{ marginBottom: 16 }} className="no-print">
        <div style={{ display: 'flex', gap: 8 }}>
          {([1, 2, 3] as Phase[]).map(p => (
            <button
              key={p}
              className={`btn ${phase === p ? 'btn--primary' : 'btn--outline'}`}
              onClick={() => setPhase(p)}
            >
              {PHASE_LABEL[p]}
              {activeJob?.phase === p && <span style={{ marginLeft: 6, fontSize: 11 }}>⏳</span>}
            </button>
          ))}
        </div>
      </div>

      {/* ① 단계 상세 */}
      <div className="no-print" style={{ padding: '12px 16px', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6, marginBottom: 12 }}>
        <div style={{ fontWeight: 700, fontSize: 14, marginBottom: 6 }}>① {PHASE_DETAIL[phase].title}</div>
        <div style={{ fontSize: 12, color: 'var(--muted, #666)', lineHeight: 1.55 }}>
          <div>{PHASE_DETAIL[phase].desc}</div>
          <div style={{ marginTop: 4 }}>
            <span className="ts">진입조건: {PHASE_DETAIL[phase].entry}</span>
            <span className="ts" style={{ marginLeft: 12 }}>예상 소요: {PHASE_DETAIL[phase].duration}</span>
          </div>
        </div>
      </div>

      {/* ② 검증 옵션 */}
      <div className="no-print" style={{ padding: '10px 16px', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6, marginBottom: 12 }}>
        <div style={{ fontWeight: 600, fontSize: 13, marginBottom: 6 }}>② 검증 옵션</div>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 16, alignItems: 'center', fontSize: 12 }}>
          <label style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
            <input type="checkbox" checked={skipBuild} onChange={e => setSkipBuild(e.target.checked)} disabled={running} />
            빌드 건너뛰기 (--skip-build)
          </label>
          {phase === 1 ? (
            <label style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
              <input type="checkbox" checked={skipReset} onChange={e => setSkipReset(e.target.checked)} disabled={running} />
              데이터 초기화 건너뛰기 (--skip-reset)
            </label>
          ) : (
            <>
              <label style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                <input type="checkbox" checked={skipPkg} onChange={e => setSkipPkg(e.target.checked)} disabled={running} />
                패키지 빌드 건너뛰기 (--skip-pkg)
              </label>
              <label style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                <input type="checkbox" checked={keepAgent} onChange={e => setKeepAgent(e.target.checked)} disabled={running} />
                Test-agent 유지 (--keep-agent)
              </label>
            </>
          )}
        </div>
      </div>

      {/* ③ 검증 모드 */}
      <div className="no-print" style={{ padding: '10px 16px', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6, marginBottom: 12 }}>
        <div style={{ fontWeight: 600, fontSize: 13, marginBottom: 6 }}>③ 검증 모드</div>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 12, alignItems: 'center' }}>
          {PHASE_MODES[phase].map(opt => (
            <label key={opt.value} style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 12, cursor: 'pointer' }}>
              <input
                type="radio"
                name={`verify-mode-phase${phase}`}
                checked={mode === opt.value}
                onChange={() => setMode(opt.value)}
                disabled={running}
              />
              {opt.label}
            </label>
          ))}
          <span style={{ marginLeft: 'auto', fontSize: 11, color: 'var(--muted, #888)' }}>
            선택 항목: <b>{selectedItems.size}</b>{hasDynamicItems && <> / {currentPhaseItems.length}</>}
          </span>
        </div>
      </div>

      {/* ④ 실행 + PDF */}
      <div className="no-print" style={{ display: 'flex', gap: 8, marginBottom: 14 }}>
        <button className="btn btn--primary" onClick={() => runPhase(phase)} disabled={running}>
          {running
            ? activeJob?.phase === phase
              ? `Phase ${phase} 실행 중...`
              : `Phase ${activeJob?.phase} 실행 중 (대기)`
            : `▶ Phase ${phase} 검증 실행`}
        </button>
        <button
          className="btn btn--outline"
          onClick={() => window.print()}
          disabled={running || !phaseResult}
          title="브라우저 인쇄 다이얼로그 → PDF 로 저장"
        >
          📄 보고서 PDF 출력
        </button>
        <button className="btn btn--outline" onClick={loadLatestReport} disabled={running}>
          최신 리포트 불러오기
        </button>
      </div>

      {/* 진행 중 stdout tail (디버깅) — 작은 collapse */}
      {activeJob && activeJob.phase === phase && (
        <details className="no-print" style={{ marginBottom: 12 }}>
          <summary style={{ cursor: 'pointer', fontSize: 12, color: 'var(--muted, #666)' }}>
            <span className="loader" style={{ display: 'inline-block', width: 10, height: 10, marginRight: 6, border: '2px solid var(--border)', borderTopColor: 'var(--accent, #3b82f6)', borderRadius: '50%', animation: 'spin 0.8s linear infinite' }} />
            stdout 실시간 (마지막 50줄, 경과 {formatElapsed(progress?.elapsed ?? ((Date.now() / 1000) - activeJob.startedAt))})
          </summary>
          <pre style={{
            margin: '8px 0 0', padding: 12, background: 'var(--bg-muted, #1a1a1a)', color: 'var(--text-muted, #ccc)',
            borderRadius: 4, fontSize: 11, lineHeight: 1.4, maxHeight: 240, overflowY: 'auto', whiteSpace: 'pre-wrap',
          }}>
            {progress?.stdout_tail || '(폴링 시작 중...)'}
          </pre>
        </details>
      )}

      {/* ⑤ 진행 표 */}
      <div ref={resultRef} style={{ marginBottom: 14 }}>
        <div style={{ fontWeight: 600, fontSize: 13, marginBottom: 6 }}>⑤ 검증 진행/결과</div>
        <ProgressTable
          selectedIds={selectedIdList}
          items={tableItems}
          currentItemId={currentItem}
          metaById={metaById}
          selectedSet={selectedItems}
          onToggle={toggleItem}
          disabled={running}
        />
      </div>

      {/* ⑥ 결과 정리 */}
      {phaseResult && (
        <div style={{ padding: '12px 16px', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6, marginBottom: 12 }}>
          <div style={{ fontWeight: 600, fontSize: 13, marginBottom: 8 }}>⑥ 결과 정리</div>
          <div style={{ display: 'flex', flexWrap: 'wrap', alignItems: 'center', gap: 16, marginBottom: 6 }}>
            <span style={{ fontSize: 18, fontWeight: 700, color: verdictColor(phaseResult.verdict) }}>
              {phaseResult.verdict}
            </span>
            <span className="ts">총 {total}항목</span>
            <span style={{ color: 'var(--success, #22c55e)' }}>PASS {pass}</span>
            <span style={{ color: 'var(--danger, #ef4444)' }}>FAIL {fail}</span>
            <span style={{ color: 'var(--muted, #888)' }}>SKIP {skip}</span>
            {phaseResult.report_ts && <span className="ts">ts={phaseResult.report_ts}</span>}
            <span className="ts">rc={phaseResult.returncode}</span>
          </div>
          {phaseResult.report_path && (
            <div style={{ fontSize: 11, color: 'var(--muted, #888)', fontFamily: 'monospace' }}>
              {phaseResult.report_path}
            </div>
          )}
        </div>
      )}

      {/* 항목 체크박스 — 모드 선택 후 미세 조정용 (collapse, no-print) */}
      {hasDynamicItems && (
        <details className="no-print" style={{ marginBottom: 12 }}>
          <summary style={{ cursor: 'pointer', fontSize: 12, color: 'var(--muted, #666)' }}>
            세부 항목 직접 체크 ({selectedItems.size} / {currentPhaseItems.length})
          </summary>
          <div style={{ marginTop: 8 }}>
            {(() => {
              const byCategory: Record<string, VerifyItemMeta[]> = {}
              for (const it of currentPhaseItems) {
                ;(byCategory[it.category || '기타'] ||= []).push(it)
              }
              const cats = Object.keys(byCategory).sort()
              return cats.map(cat => (
                <div key={cat} style={{ marginBottom: 8 }}>
                  <div style={{ fontSize: 11, color: 'var(--muted, #888)', marginBottom: 4 }}>{cat}</div>
                  <div style={{ display: 'flex', flexWrap: 'wrap', gap: 8 }}>
                    {byCategory[cat].map(it => (
                      <label key={it.id} style={{ display: 'flex', alignItems: 'center', gap: 5, fontSize: 11, padding: '2px 6px', background: 'var(--bg-muted, #f5f5f5)', borderRadius: 4 }} title={it.description || ''}>
                        <input
                          type="checkbox"
                          checked={selectedItems.has(it.id)}
                          onChange={() => toggleItem(it.id)}
                          disabled={running}
                        />
                        <span style={{ fontFamily: 'monospace' }}>{it.id}</span>
                      </label>
                    ))}
                  </div>
                </div>
              ))
            })()}
          </div>
        </details>
      )}

      {phaseReport && (
        <details className="no-print" style={{ marginTop: 8 }}>
          <summary style={{ cursor: 'pointer', fontSize: 13, fontWeight: 600 }}>
            📄 검증 리포트 markdown <span className="ts">({phaseReport.ts})</span>
          </summary>
          <pre style={{ marginTop: 8, background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6,
            padding: 16, fontSize: 11, lineHeight: 1.5, maxHeight: 500, overflowY: 'auto', whiteSpace: 'pre-wrap' }}>
            {phaseReport.content}
          </pre>
        </details>
      )}

    </div>
  )
}
