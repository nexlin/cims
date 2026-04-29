import { useState, useEffect, useRef, Fragment } from 'react'

// ─────────────────────────────────────────────────────────────
// 검증 v2 프로토타입 — 6단계 (S1~S6) + 그룹핑 인프라
// 백엔드 미연결 / mock 데이터 / 시각 검토용
// ─────────────────────────────────────────────────────────────

type ItemStatus = 'PENDING' | 'RUNNING' | 'PASS' | 'FAIL' | 'SKIP' | 'BLOCKED'

interface MockItem {
  id: string
  name: string
  desc?: string
  status: ItemStatus
  elapsedMs: number
  isGroup?: boolean
  parent?: string
}

interface Stage {
  num: number
  id: string
  title: string
  desc: string
  items: MockItem[]
}

// ─────────────────────────────────────────────────────────────
// Mock 데이터 — 30 항목 (5+2+7+2+7+7) + S5 자식 14
// ─────────────────────────────────────────────────────────────
const STAGES_INIT: Stage[] = [
  {
    num: 1, id: 'S1', title: '정적 검사',
    desc: 'lint / format / unit test — 코드 위생 gate',
    items: [
      { id: 'S1-PY-SYNTAX',          name: 'Python syntax', desc: 'py_compile verify/, tests/, csc/src/', status: 'PASS', elapsedMs: 1200 },
      { id: 'S1-FRONTEND-LINT',      name: 'Console ESLint', desc: 'npm run lint (cims-console)', status: 'PASS', elapsedMs: 3400 },
      { id: 'S1-FRONTEND-TYPECHECK', name: 'Console TypeCheck', desc: 'tsc -b --noEmit', status: 'PASS', elapsedMs: 4100 },
      { id: 'S1-CPP-FORMAT',         name: 'CPP clang-format', desc: 'csp/.clang-format dry-run', status: 'PASS', elapsedMs: 800 },
      { id: 'S1-UNIT-VERIFY-LIB',    name: 'verify_lib unit test', desc: 'python3 -m unittest tests.test_verify_lib (31 tests)', status: 'PASS', elapsedMs: 280 },
    ],
  },
  {
    num: 2, id: 'S2', title: '빌드',
    desc: 'preflight + cmake build — 컴파일 통과 gate',
    items: [
      { id: 'S2-PREFLIGHT', name: 'Preflight', desc: 'ens160 IP / git / 포트 / DB 점검', status: 'PASS', elapsedMs: 5000 },
      { id: 'S2-BUILD',     name: 'Build (dist)', desc: 'cmake + make -j (산출물 검증 포함)', status: 'PASS', elapsedMs: 83000 },
    ],
  },
  {
    num: 3, id: 'S3', title: '스모크 검증',
    desc: 'dev 환경 1콜 회귀 (~1분) — 빠른 sanity gate',
    items: [
      { id: 'S3-RESET',          name: 'Dev reset', desc: 'cmd_reset --all (가입자 보존)', status: 'PASS', elapsedMs: 3500 },
      { id: 'S3-CONFIGURE',      name: 'Configure', desc: 'cmd_configure --local-ip <ens160>', status: 'PASS', elapsedMs: 1200 },
      { id: 'S3-START',          name: 'Start (dev)', desc: 'cmp → csp → cwrtc → csc → console → phone', status: 'PASS', elapsedMs: 8000 },
      { id: 'S3-HEALTH',         name: 'Health check', desc: 'csp/cmp/csc 로그 ERROR/FATAL 스캔', status: 'PASS', elapsedMs: 600 },
      { id: 'S3-SEED',           name: 'Seed', desc: '가입자/그룹 + access_services + csp reload', status: 'FAIL', elapsedMs: 4000 },
      { id: 'S3-SCN-VOIP-SMOKE', name: 'VoIP smoke', desc: '2자 통화 1콜 (B2BUA)', status: 'BLOCKED', elapsedMs: 0 },
      { id: 'S3-SCN-PTT-SMOKE',  name: 'PTT smoke', desc: '5인 그룹콜 1회', status: 'BLOCKED', elapsedMs: 0 },
    ],
  },
  {
    num: 4, id: 'S4', title: '패키지화',
    desc: 'tarball 5개 + manifest hash — immutability gate (S6 매칭용)',
    items: [
      { id: 'S4-PKG-BUILD',    name: 'Pkg build', desc: 'cmd_pkg --no-bump (csc/console/csp/cmp/sim)', status: 'PENDING', elapsedMs: 0 },
      { id: 'S4-PKG-MANIFEST', name: 'Manifest hash', desc: '5 tarball SHA-256 + timestamp 기록', status: 'PENDING', elapsedMs: 0 },
    ],
  },
  {
    num: 5, id: 'S5', title: '로컬배포',
    desc: 'reset → install → start → health (S4 산출물) — 배포 절차 회귀',
    items: [
      { id: 'S5-RESET',                       name: 'Cleanup', desc: 'cmd_reset --keep-processes', status: 'PENDING', elapsedMs: 0 },

      { id: 'S5-CSC-DEPLOY',                  name: 'csc/console 배포', desc: 'TB-CSC(4419) → agent + install', status: 'PENDING', elapsedMs: 0, isGroup: true },
      { id: 'S5-CSC-DEPLOY.AGENT-ENROLL',     name: 'Agent enroll', desc: 'admin login + Test-agent 9903', status: 'PENDING', elapsedMs: 0, parent: 'S5-CSC-DEPLOY' },
      { id: 'S5-CSC-DEPLOY.PKG-UPLOAD',       name: 'Pkg upload', desc: 'csc + console tarball → 4419', status: 'PENDING', elapsedMs: 0, parent: 'S5-CSC-DEPLOY' },
      { id: 'S5-CSC-DEPLOY.INSTALL',          name: 'Install', desc: 'deployment 생성 + install poll', status: 'PENDING', elapsedMs: 0, parent: 'S5-CSC-DEPLOY' },

      { id: 'S5-CSC-VERIFY',                  name: 'csc 설치 검증', desc: '파일 + overlay', status: 'PENDING', elapsedMs: 0, isGroup: true },
      { id: 'S5-CSC-VERIFY.FILES',            name: 'Files', desc: 'meta.json + config/ 존재', status: 'PENDING', elapsedMs: 0, parent: 'S5-CSC-VERIFY' },
      { id: 'S5-CSC-VERIFY.OVERLAY',          name: 'Overlay', desc: 'csc/config.json Server.Port=4445', status: 'PENDING', elapsedMs: 0, parent: 'S5-CSC-VERIFY' },

      { id: 'S5-CSC-RUN',                     name: 'csc/console 기동', desc: 'start + health + listen', status: 'PENDING', elapsedMs: 0, isGroup: true },
      { id: 'S5-CSC-RUN.CSC-START',           name: 'csc start', desc: 'port 4445 LISTEN', status: 'PENDING', elapsedMs: 0, parent: 'S5-CSC-RUN' },
      { id: 'S5-CSC-RUN.CSC-HEALTH',          name: 'csc health', desc: 'health_check job', status: 'PENDING', elapsedMs: 0, parent: 'S5-CSC-RUN' },
      { id: 'S5-CSC-RUN.CONSOLE-START',       name: 'console start', desc: 'port 8081 LISTEN', status: 'PENDING', elapsedMs: 0, parent: 'S5-CSC-RUN' },

      { id: 'S5-MODULES-DEPLOY',              name: 'csp/cmp/sim 배포', desc: '배포본 csc(4445) → 3 agent', status: 'PENDING', elapsedMs: 0, isGroup: true },
      { id: 'S5-MODULES-DEPLOY.AUTH',         name: 'Auth', desc: '배포본 csc 4445 admin login', status: 'PENDING', elapsedMs: 0, parent: 'S5-MODULES-DEPLOY' },
      { id: 'S5-MODULES-DEPLOY.PKG-UPLOAD',   name: 'Pkg upload', desc: 'csp + cmp + sim → 4445', status: 'PENDING', elapsedMs: 0, parent: 'S5-MODULES-DEPLOY' },
      { id: 'S5-MODULES-DEPLOY.AGENT-ENROLL', name: 'Agent enroll', desc: '3 agent + Test-agent 9904/5/6', status: 'PENDING', elapsedMs: 0, parent: 'S5-MODULES-DEPLOY' },
      { id: 'S5-MODULES-DEPLOY.INSTALL',      name: 'Install', desc: 'deployment + install poll', status: 'PENDING', elapsedMs: 0, parent: 'S5-MODULES-DEPLOY' },

      { id: 'S5-MODULES-RUN',                 name: 'csp/cmp 기동', desc: 'start + LISTEN', status: 'PENDING', elapsedMs: 0, isGroup: true },
      { id: 'S5-MODULES-RUN.START',           name: 'Module start', desc: 'csp 5060/udp + cmp 9000/udp', status: 'PENDING', elapsedMs: 0, parent: 'S5-MODULES-RUN' },

      { id: 'S5-FINALIZE',                    name: 'Finalize (옵션)', desc: '--stop-after 시 전체 stop', status: 'PENDING', elapsedMs: 0 },
    ],
  },
  {
    num: 6, id: 'S6', title: '통합 검증',
    desc: 'VoLTE/PTT 음성·영상 + 회귀 (~10분) — 상용 진입 gate',
    items: [
      { id: 'S6-ENTRY-CHECK',     name: 'Entry check', desc: 'csc/console/csp/cmp 4포트 LISTEN', status: 'PENDING', elapsedMs: 0 },
      { id: 'S6-SEED',            name: 'Seed', desc: '가입자/그룹 + access_services + csp reload', status: 'PENDING', elapsedMs: 0 },
      { id: 'S6-SCN-VOLTE-VOICE', name: 'VoLTE 음성', desc: '2자 음성 통화', status: 'PENDING', elapsedMs: 0 },
      { id: 'S6-SCN-VOLTE-VIDEO', name: 'VoLTE 영상', desc: '2자 영상 통화', status: 'PENDING', elapsedMs: 0 },
      { id: 'S6-SCN-PTT-VOICE',   name: 'PTT 음성', desc: '5인 그룹 음성', status: 'PENDING', elapsedMs: 0 },
      { id: 'S6-SCN-PTT-VIDEO',   name: 'PTT 영상', desc: '5인 그룹 영상', status: 'PENDING', elapsedMs: 0 },
      { id: 'S6-SUMMARY',         name: '결과 요약', desc: '녹취/SIP/ERROR 카운트', status: 'PENDING', elapsedMs: 0 },
    ],
  },
]

// ─────────────────────────────────────────────────────────────
// 헬퍼
// ─────────────────────────────────────────────────────────────

// 부모 그룹 체크박스 — 자식 일부만 선택 시 indeterminate
function GroupCheckbox({ checked, indeterminate, disabled, onChange }: {
  checked: boolean; indeterminate: boolean; disabled: boolean; onChange: () => void
}) {
  const ref = useRef<HTMLInputElement>(null)
  useEffect(() => {
    if (ref.current) ref.current.indeterminate = !checked && indeterminate
  }, [checked, indeterminate])
  return <input ref={ref} type="checkbox" checked={checked} disabled={disabled} onChange={onChange} />
}

function statusIcon(s: ItemStatus): string {
  if (s === 'PASS')    return '✅'
  if (s === 'FAIL')    return '❌'
  if (s === 'SKIP')    return '⏭'
  if (s === 'RUNNING') return '⏳'
  if (s === 'BLOCKED') return '🚫'
  return '⏸'
}

function statusLabel(s: ItemStatus): string {
  if (s === 'PASS')    return '성공'
  if (s === 'FAIL')    return '실패'
  if (s === 'SKIP')    return '건너뜀'
  if (s === 'RUNNING') return '진행중'
  if (s === 'BLOCKED') return '차단'
  return '대기'
}

function statusColor(s: ItemStatus): string {
  if (s === 'PASS')    return '#16a34a'
  if (s === 'FAIL')    return '#dc2626'
  if (s === 'RUNNING') return '#3b82f6'
  if (s === 'BLOCKED') return '#a16207'
  if (s === 'SKIP')    return '#6b7280'
  return '#9ca3af'
}

function fmtMs(ms: number): string {
  if (ms < 1000) return `${ms}ms`
  if (ms < 60000) return `${(ms / 1000).toFixed(1)}s`
  const m = Math.floor(ms / 60000)
  const s = Math.floor((ms % 60000) / 1000)
  return `${m}m ${s}s`
}

function stageStatus(items: MockItem[]): ItemStatus {
  const flat = items.filter(it => !it.isGroup)
  if (flat.length === 0) return 'PENDING'
  if (flat.some(it => it.status === 'FAIL'))    return 'FAIL'
  if (flat.some(it => it.status === 'RUNNING')) return 'RUNNING'
  if (flat.some(it => it.status === 'BLOCKED') && !flat.some(it => it.status === 'PENDING')) return 'BLOCKED'
  if (flat.every(it => it.status === 'PASS' || it.status === 'SKIP')) return 'PASS'
  if (flat.some(it => it.status === 'PASS') && flat.some(it => it.status === 'PENDING')) return 'RUNNING'
  return 'PENDING'
}

function stageProgress(items: MockItem[]): { done: number; total: number; elapsed: number } {
  const flat = items.filter(it => !it.isGroup)
  const total = flat.length
  const done = flat.filter(it => it.status === 'PASS' || it.status === 'FAIL' || it.status === 'SKIP').length
  const elapsed = flat.reduce((sum, it) => sum + it.elapsedMs, 0)
  return { done, total, elapsed }
}

function groupStatus(stage: Stage, groupId: string): { status: ItemStatus; doneCount: number; totalCount: number } {
  const children = stage.items.filter(it => it.parent === groupId)
  if (children.length === 0) return { status: 'PENDING', doneCount: 0, totalCount: 0 }
  const total = children.length
  const done = children.filter(c => c.status === 'PASS' || c.status === 'FAIL' || c.status === 'SKIP').length
  let status: ItemStatus = 'PENDING'
  if (children.some(c => c.status === 'FAIL'))    status = 'FAIL'
  else if (children.some(c => c.status === 'RUNNING')) status = 'RUNNING'
  else if (children.every(c => c.status === 'PASS' || c.status === 'SKIP')) status = 'PASS'
  else if (children.some(c => c.status === 'PASS')) status = 'RUNNING'
  return { status, doneCount: done, totalCount: total }
}

// ─────────────────────────────────────────────────────────────
// Stepper — 가로 6단계 흐름
// ─────────────────────────────────────────────────────────────

function Stepper({ stages, onSelect, resumeStage, disabled }: {
  stages: Stage[]; onSelect: (n: number) => void; resumeStage: number; disabled: boolean
}) {
  return (
    <div style={{
      display: 'flex', alignItems: 'center', gap: 0,
      background: 'var(--bg-elevated, #fff)',
      border: '1px solid var(--border, #e5e7eb)',
      borderRadius: 8, padding: '24px 16px 16px', marginBottom: 12,
    }}>
      {stages.map((st, i) => {
        const status = stageStatus(st.items)
        const { done, total } = stageProgress(st.items)
        const color = statusColor(status)
        const isResume = resumeStage === st.num

        // 진행률 (테두리 호로 표시)
        const pct = total > 0 ? (done / total) * 100 : 0
        // PASS/FAIL/BLOCKED 는 100% (테두리 가득), RUNNING 은 진행률, PENDING 은 0%
        const ringPct = (status === 'PASS' || status === 'FAIL' || status === 'BLOCKED') ? 100 : pct
        const ringColor = status === 'PENDING' ? '#e5e7eb' : color
        const ringBg = ringPct >= 100
          ? ringColor
          : `conic-gradient(${ringColor} 0deg ${ringPct * 3.6}deg, #e5e7eb ${ringPct * 3.6}deg 360deg)`

        return (
          <Fragment key={st.id}>
            <button
              onClick={() => onSelect(st.num)}
              disabled={disabled}
              title={disabled ? '실행 중에는 변경 불가' : '클릭하여 재개 지점 설정'}
              style={{
                flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center',
                padding: '4px', cursor: disabled ? 'not-allowed' : 'pointer',
                background: 'transparent',
                border: '2px solid transparent',
                borderRadius: 8,
                textAlign: 'center', position: 'relative',
                opacity: disabled ? 0.7 : 1,
              }}
            >
              {isResume && (
                <div style={{
                  position: 'absolute', top: -18, left: '50%', transform: 'translateX(-50%)',
                  background: '#3b82f6', color: '#fff',
                  fontSize: 10, fontWeight: 700,
                  padding: '2px 8px', borderRadius: 10,
                  whiteSpace: 'nowrap', boxShadow: '0 1px 3px rgba(0,0,0,0.2)',
                  zIndex: 1,
                }}>
                  🚩 재개 지점
                </div>
              )}
              {/* 외곽 ring (진행률 / 상태 색) */}
              <div style={{
                width: 120, height: 120, borderRadius: 60,
                background: ringBg,
                padding: 6,
                boxShadow: isResume ? `0 0 0 4px #3b82f633` : 'none',
                transition: 'all 0.2s',
              }}>
                {/* 내부 흰 원 */}
                <div style={{
                  width: '100%', height: '100%', borderRadius: '50%',
                  background: '#fff',
                  display: 'flex', flexDirection: 'column',
                  alignItems: 'center', justifyContent: 'center',
                  fontWeight: 700, lineHeight: 1.2,
                }}>
                  <div style={{ fontSize: 26, color: color, letterSpacing: 0.5 }}>{st.id}</div>
                  <div style={{
                    fontSize: 12, color: 'var(--muted, #374151)',
                    marginTop: 4, padding: '0 6px',
                    whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis',
                    maxWidth: '100%', fontWeight: 600,
                  }}>
                    {st.title}
                  </div>
                  <div style={{
                    fontSize: 11, color: 'var(--muted, #9ca3af)',
                    marginTop: 3, fontWeight: 500,
                  }}>
                    {done}/{total}
                  </div>
                </div>
              </div>
            </button>
            {i < stages.length - 1 && (
              <div style={{
                flex: '0 0 24px', height: 3,
                background: stageStatus(stages[i].items) === 'PASS'
                  ? statusColor('PASS')
                  : 'var(--border, #e5e7eb)',
                margin: '0 -1px',
                borderRadius: 2,
              }} />
            )}
          </Fragment>
        )
      })}
    </div>
  )
}

// ─────────────────────────────────────────────────────────────
// Global Header — Pipeline 버튼 + 재개 dropdown + 패키지 카드
// ─────────────────────────────────────────────────────────────

function GlobalHeader({
  stages, onPipelineToggle, running, resumeStage, setResumeStage, onPrintReport,
}: {
  stages: Stage[]
  onPipelineToggle: () => void
  running: boolean
  resumeStage: number
  setResumeStage: (n: number) => void
  onPrintReport: () => void
}) {
  const overallStatus = (() => {
    const sts = stages.map(s => stageStatus(s.items))
    if (sts.some(s => s === 'FAIL'))    return 'FAIL'
    if (sts.some(s => s === 'RUNNING')) return 'RUNNING'
    if (sts.every(s => s === 'PASS'))   return 'PASS'
    return 'PENDING'
  })()
  return (
    <div style={{
      display: 'flex', gap: 12, alignItems: 'center', flexWrap: 'wrap',
      background: 'var(--bg-elevated, #fff)',
      border: '1px solid var(--border, #e5e7eb)',
      borderRadius: 8, padding: 12, marginBottom: 12,
    }}>
      {/* 시작/중단 toggle 버튼 — 크기 고정 */}
      <button
        onClick={onPipelineToggle}
        style={{
          minWidth: 160, height: 36,
          padding: '0 16px',
          background: running ? '#dc2626' : '#3b82f6',
          color: '#fff', border: 'none', borderRadius: 6,
          fontSize: 13, fontWeight: 700, cursor: 'pointer',
          transition: 'background 0.2s',
        }}
      >
        {running ? '⏹ 전체검증 중단' : '▶ 전체검증'}
      </button>

      {/* 재개 지점 dropdown — Run 옆 */}
      <label style={{
        display: 'flex', alignItems: 'center', gap: 6,
        fontSize: 12, color: 'var(--muted, #6b7280)',
      }}>
        🚩 재개 지점:
        <select
          value={resumeStage}
          onChange={e => setResumeStage(Number(e.target.value))}
          disabled={running}
          style={{
            padding: '6px 8px', borderRadius: 4,
            border: '1px solid #93c5fd',
            background: '#eff6ff', color: '#1d4ed8',
            fontSize: 12, fontWeight: 600,
            cursor: running ? 'not-allowed' : 'pointer',
          }}
        >
          {stages.map(s => (
            <option key={s.id} value={s.num}>{s.id} · {s.title}</option>
          ))}
        </select>
      </label>

      <div style={{
        marginLeft: 'auto',
        display: 'flex', alignItems: 'center', gap: 8,
        padding: '6px 12px', background: 'var(--bg-muted, #f9fafb)',
        borderRadius: 6, fontSize: 12,
      }}>
        <span style={{ color: 'var(--muted, #6b7280)' }}>📦 마지막 패키지:</span>
        <code style={{ fontSize: 11, fontWeight: 600 }}>cims-2026.04.29-a3f2b1c</code>
        <span style={{ color: statusColor('PASS') }}>(S4 ✅)</span>
      </div>

      <button
        onClick={onPrintReport}
        title="검증 보고서 PDF 출력 (모든 stage 펼침 → 인쇄)"
        style={{
          padding: '6px 12px', height: 36,
          background: '#fff',
          border: '1px solid var(--border, #d1d5db)',
          borderRadius: 6, fontSize: 12, fontWeight: 600,
          cursor: 'pointer',
        }}
      >
        📄 보고서 출력
      </button>

      <div style={{
        padding: '4px 10px',
        background: statusColor(overallStatus as ItemStatus), color: '#fff',
        borderRadius: 4, fontSize: 11, fontWeight: 600,
      }}>
        전체: {statusIcon(overallStatus as ItemStatus)} {statusLabel(overallStatus as ItemStatus)}
      </div>
    </div>
  )
}

// ─────────────────────────────────────────────────────────────
// PrintReport — print 시에만 보임. 보고서 자체가 전체 콘텐츠를 그림.
// 1. 검증결과 요약 / 2. 검증항목별 결과 / 3. 검증 상세내용
// ─────────────────────────────────────────────────────────────

function PrintReport({ stages, resumeStage }: { stages: Stage[]; resumeStage: number }) {
  const now = new Date().toLocaleString('ko-KR')
  const overallStatus = (() => {
    const sts = stages.map(s => stageStatus(s.items))
    if (sts.some(s => s === 'FAIL'))    return 'FAIL'
    if (sts.some(s => s === 'RUNNING')) return 'RUNNING'
    if (sts.every(s => s === 'PASS'))   return 'PASS'
    return 'PENDING'
  })()
  const flatItems = stages.flatMap(st => st.items.filter(it => !it.isGroup).map(it => ({ st, it })))
  const totalItems = flatItems.length
  const passCount = flatItems.filter(({ it }) => it.status === 'PASS').length
  const failCount = flatItems.filter(({ it }) => it.status === 'FAIL').length
  const skipCount = flatItems.filter(({ it }) => it.status === 'SKIP').length
  const blockCount = flatItems.filter(({ it }) => it.status === 'BLOCKED').length
  const pendCount = flatItems.filter(({ it }) => it.status === 'PENDING').length
  const totalElapsed = flatItems.reduce((s, { it }) => s + it.elapsedMs, 0)

  const cell = (text: string | number, opts: React.CSSProperties = {}) => (
    <td style={{ padding: '5px 8px', borderBottom: '1px solid #e5e7eb', ...opts }}>{text}</td>
  )
  const headCell = (text: string, opts: React.CSSProperties = {}) => (
    <th style={{ padding: '6px 8px', borderBottom: '1.5px solid #111', textAlign: 'left', background: '#f3f4f6', ...opts }}>{text}</th>
  )
  const sectionH = (color = '#111'): React.CSSProperties => ({
    fontSize: 16, fontWeight: 800, marginTop: 18, marginBottom: 8, color,
    borderLeft: `4px solid ${color}`, paddingLeft: 8,
  })

  return (
    <div className="v2-report" style={{ display: 'none', fontFamily: 'sans-serif', color: '#111' }}>
      {/* 표지 */}
      <div style={{ borderBottom: '3px double #111', paddingBottom: 16, marginBottom: 20 }}>
        <div style={{ fontSize: 11, color: '#6b7280', marginBottom: 4 }}>CIMS Verification Report</div>
        <h1 style={{ margin: 0, fontSize: 26, fontWeight: 900, letterSpacing: -0.5 }}>
          CIMS 검증 보고서
        </h1>
        <div style={{ marginTop: 12, fontSize: 11, lineHeight: 1.8, columnCount: 2 }}>
          <div><b>발행 일시:</b> {now}</div>
          <div><b>호스트:</b> 192.168.199.129 (ens160)</div>
          <div><b>git 브랜치:</b> feature/sip-console-runtime</div>
          <div><b>git revision:</b> 8d71c70</div>
          <div><b>패키지 manifest:</b> <code>cims-2026.04.29-a3f2b1c</code></div>
          <div><b>재개 지점:</b> S{resumeStage} 단계부터</div>
        </div>
      </div>

      {/* 1. 검증결과 요약 */}
      <h2 style={sectionH()}>1. 검증결과 요약</h2>
      <div style={{ fontSize: 12, marginBottom: 12 }}>
        본 검증은 6단계 파이프라인 (S1 정적검사 → S2 빌드 → S3 스모크 → S4 패키지화 → S5 로컬배포 → S6 통합검증) 으로
        구성된 절차에 따라 수행되었습니다. 각 단계는 이전 단계의 PASS 를 전제로 진행되며,
        S4 의 패키지 manifest hash 가 S6 검증 시점과 매칭되어 빌드 산출물의 무결성(immutability)을 보장합니다.
      </div>

      {/* 종합 판정 박스 */}
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12, marginBottom: 12 }}>
        <tbody>
          <tr>
            <td style={{
              padding: '12px 16px', width: '30%',
              background: `${statusColor(overallStatus)}15`,
              border: `2px solid ${statusColor(overallStatus)}`,
              fontSize: 18, fontWeight: 800, color: statusColor(overallStatus),
              textAlign: 'center', verticalAlign: 'middle',
            }}>
              {statusIcon(overallStatus)} {statusLabel(overallStatus)}
            </td>
            <td style={{ padding: '12px 16px', border: '1px solid #d1d5db' }}>
              <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: 6, fontSize: 11 }}>
                <div><b>전체 항목:</b> {totalItems}건</div>
                <div><b>총 소요시간:</b> {fmtMs(totalElapsed)}</div>
                <div><b>완료율:</b> {totalItems > 0 ? Math.round((passCount + failCount + skipCount) / totalItems * 100) : 0}%</div>
                <div style={{ color: statusColor('PASS') }}><b>✅ 성공:</b> {passCount}건</div>
                <div style={{ color: statusColor('FAIL') }}><b>❌ 실패:</b> {failCount}건</div>
                <div style={{ color: statusColor('SKIP') }}><b>⏭ 건너뜀:</b> {skipCount}건</div>
                <div style={{ color: statusColor('BLOCKED') }}><b>🚫 차단:</b> {blockCount}건</div>
                <div style={{ color: statusColor('PENDING') }}><b>⏸ 대기:</b> {pendCount}건</div>
              </div>
            </td>
          </tr>
        </tbody>
      </table>

      {/* 단계별 요약 표 */}
      <div style={{ fontSize: 12, fontWeight: 700, marginBottom: 6 }}>1.1 단계별 요약</div>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11, marginBottom: 12 }}>
        <thead>
          <tr>
            {headCell('Stage', { width: 50 })}
            {headCell('단계명', { width: 110 })}
            {headCell('범위 / Gate')}
            {headCell('판정', { width: 80, textAlign: 'center' })}
            {headCell('완료/전체', { width: 80, textAlign: 'right' })}
            {headCell('소요', { width: 70, textAlign: 'right' })}
          </tr>
        </thead>
        <tbody>
          {stages.map(st => {
            const s = stageStatus(st.items)
            const { done, total, elapsed } = stageProgress(st.items)
            return (
              <tr key={st.id}>
                {cell(st.id, { fontWeight: 700 })}
                {cell(st.title, { fontWeight: 600 })}
                {cell(st.desc, { color: '#4b5563', fontSize: 10 })}
                {cell(`${statusIcon(s)} ${statusLabel(s)}`, { textAlign: 'center', color: statusColor(s), fontWeight: 700 })}
                {cell(`${done} / ${total}`, { textAlign: 'right' })}
                {cell(fmtMs(elapsed), { textAlign: 'right' })}
              </tr>
            )
          })}
        </tbody>
      </table>

      {/* 실패 항목 강조 (있을 때만) */}
      {failCount > 0 && (
        <>
          <div style={{ fontSize: 12, fontWeight: 700, marginBottom: 6, color: '#dc2626' }}>
            1.2 실패 항목 ({failCount}건) — 우선 조치 필요
          </div>
          <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11, marginBottom: 12 }}>
            <thead>
              <tr>
                {headCell('Stage', { width: 50, background: '#fef2f2', borderBottom: '1.5px solid #dc2626' })}
                {headCell('항목 ID', { width: 220, background: '#fef2f2', borderBottom: '1.5px solid #dc2626' })}
                {headCell('항목명', { background: '#fef2f2', borderBottom: '1.5px solid #dc2626' })}
                {headCell('설명', { background: '#fef2f2', borderBottom: '1.5px solid #dc2626' })}
                {headCell('소요', { width: 60, background: '#fef2f2', borderBottom: '1.5px solid #dc2626', textAlign: 'right' })}
              </tr>
            </thead>
            <tbody>
              {flatItems.filter(({ it }) => it.status === 'FAIL').map(({ st, it }) => (
                <tr key={it.id}>
                  {cell(st.id, { fontWeight: 700 })}
                  {cell(it.id, { fontFamily: 'monospace', color: '#dc2626', fontWeight: 600 })}
                  {cell(it.name)}
                  {cell(it.desc || '—', { color: '#4b5563', fontSize: 10 })}
                  {cell(fmtMs(it.elapsedMs), { textAlign: 'right' })}
                </tr>
              ))}
            </tbody>
          </table>
        </>
      )}

      {/* 2. 검증항목별 결과 */}
      <h2 style={sectionH()}>2. 검증항목별 결과</h2>
      <div style={{ fontSize: 11, color: '#4b5563', marginBottom: 6 }}>
        전체 {totalItems}개 항목의 단순 결과 표 (그룹 자식 포함, 그룹 자체는 자식의 worst-status 반영).
      </div>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 10, marginBottom: 12 }}>
        <thead>
          <tr>
            {headCell('#', { width: 30, textAlign: 'right' })}
            {headCell('Stage', { width: 50 })}
            {headCell('항목 ID', { width: 240 })}
            {headCell('항목명')}
            {headCell('판정', { width: 75, textAlign: 'center' })}
            {headCell('소요', { width: 60, textAlign: 'right' })}
          </tr>
        </thead>
        <tbody>
          {flatItems.map(({ st, it }, idx) => (
            <tr key={it.id}>
              {cell(idx + 1, { textAlign: 'right', color: '#9ca3af' })}
              {cell(st.id, { fontWeight: 600 })}
              {cell(it.id, { fontFamily: 'monospace', fontSize: 9, paddingLeft: it.parent ? 24 : 8 })}
              {cell(it.name, { fontSize: 10 })}
              {cell(`${statusIcon(it.status)} ${statusLabel(it.status)}`, {
                textAlign: 'center', color: statusColor(it.status), fontWeight: 600,
              })}
              {cell(fmtMs(it.elapsedMs), { textAlign: 'right', color: '#6b7280' })}
            </tr>
          ))}
        </tbody>
      </table>

      {/* 3. 검증 상세내용 */}
      <h2 style={sectionH()}>3. 검증 상세내용</h2>
      <div style={{ fontSize: 11, color: '#4b5563', marginBottom: 8 }}>
        각 단계별 항목의 검증 의도 / 수행 내용 / 결과를 차례로 기술합니다.
      </div>

      {stages.map((st, sIdx) => {
        const s = stageStatus(st.items)
        const { done, total, elapsed } = stageProgress(st.items)
        return (
          <div key={st.id} style={{
            marginBottom: 16, pageBreakInside: 'avoid', breakInside: 'avoid',
          }}>
            <h3 style={{
              fontSize: 14, fontWeight: 800, marginTop: 12, marginBottom: 6,
              padding: '6px 10px',
              background: `${statusColor(s)}15`,
              borderLeft: `4px solid ${statusColor(s)}`,
            }}>
              3.{sIdx + 1} {st.id} · {st.title}
              <span style={{ marginLeft: 8, fontSize: 11, color: statusColor(s) }}>
                {statusIcon(s)} {statusLabel(s)}
              </span>
              <span style={{ marginLeft: 8, fontSize: 10, color: '#6b7280', fontWeight: 500 }}>
                ({done}/{total} · {fmtMs(elapsed)})
              </span>
            </h3>
            <div style={{ fontSize: 10, color: '#4b5563', marginBottom: 6, paddingLeft: 14 }}>
              <b>범위:</b> {st.desc}
            </div>

            {st.items.filter(it => !it.parent).map(it => {
              const isGroup = it.isGroup === true
              const children = isGroup ? st.items.filter(c => c.parent === it.id) : []
              const itStatus = isGroup ? groupStatus(st, it.id).status : it.status
              return (
                <div key={it.id} style={{
                  marginBottom: 8, paddingLeft: 14,
                  borderLeft: `2px solid ${statusColor(itStatus)}33`,
                  pageBreakInside: 'avoid', breakInside: 'avoid',
                }}>
                  <div style={{ fontSize: 11, fontWeight: 700, marginBottom: 2 }}>
                    <code style={{ fontSize: 11, color: statusColor(itStatus) }}>{it.id}</code>
                    {' '}— {it.name}
                    {isGroup && <span style={{ color: '#6b7280', fontWeight: 500, fontSize: 10 }}>{' '}(그룹: 자식 {children.length}개)</span>}
                    <span style={{ marginLeft: 8, color: statusColor(itStatus), fontSize: 10 }}>
                      {statusIcon(itStatus)} {statusLabel(itStatus)}
                    </span>
                    {!isGroup && (
                      <span style={{ marginLeft: 6, color: '#6b7280', fontSize: 10 }}>
                        · {fmtMs(it.elapsedMs)}
                      </span>
                    )}
                  </div>
                  {it.desc && (
                    <div style={{ fontSize: 10, color: '#4b5563', marginBottom: 4 }}>
                      <b>설명:</b> {it.desc}
                    </div>
                  )}

                  {/* 자식 항목 list */}
                  {isGroup && children.length > 0 && (
                    <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 10, marginTop: 4 }}>
                      <thead>
                        <tr>
                          {headCell('자식 항목 ID', { width: 240, fontSize: 10, padding: '4px 6px' })}
                          {headCell('이름', { fontSize: 10, padding: '4px 6px' })}
                          {headCell('설명', { fontSize: 10, padding: '4px 6px' })}
                          {headCell('판정', { width: 70, textAlign: 'center', fontSize: 10, padding: '4px 6px' })}
                          {headCell('소요', { width: 50, textAlign: 'right', fontSize: 10, padding: '4px 6px' })}
                        </tr>
                      </thead>
                      <tbody>
                        {children.map(c => (
                          <tr key={c.id}>
                            {cell(c.id.split('.').pop() || c.id, { fontFamily: 'monospace', fontSize: 9, padding: '3px 6px', paddingLeft: 14 })}
                            {cell(c.name, { fontSize: 10, padding: '3px 6px' })}
                            {cell(c.desc || '—', { fontSize: 9, color: '#6b7280', padding: '3px 6px' })}
                            {cell(`${statusIcon(c.status)} ${statusLabel(c.status)}`, {
                              textAlign: 'center', color: statusColor(c.status), fontWeight: 600, fontSize: 9, padding: '3px 6px',
                            })}
                            {cell(fmtMs(c.elapsedMs), { textAlign: 'right', fontSize: 9, color: '#6b7280', padding: '3px 6px' })}
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  )}
                </div>
              )
            })}
          </div>
        )
      })}

      {/* 푸터 */}
      <div style={{
        marginTop: 24, paddingTop: 12, borderTop: '1px solid #d1d5db',
        fontSize: 9, color: '#9ca3af', textAlign: 'center',
      }}>
        본 보고서는 CIMS 검증 v2 (6단계 파이프라인) 시스템에 의해 자동 생성되었습니다.
        {' · '} 발행 시각: {now}
      </div>
    </div>
  )
}

// ─────────────────────────────────────────────────────────────
// Stage Accordion — 헤더 + 펼침 영역
// ─────────────────────────────────────────────────────────────

function StageRow({
  stage, expanded, onToggle, onStageToggle, selectedItems, toggleItemSelect, expandedGroups, toggleGroup, anyRunning, isThisRunning,
  isResume,
}: {
  stage: Stage
  expanded: boolean
  onToggle: () => void
  onStageToggle: () => void
  selectedItems: Set<string>
  toggleItemSelect: (id: string) => void
  expandedGroups: Set<string>
  toggleGroup: (id: string) => void
  anyRunning: boolean
  isThisRunning: boolean
  isResume: boolean
}) {
  const status = stageStatus(stage.items)
  const { done, total, elapsed } = stageProgress(stage.items)
  const color = statusColor(status)

  // top-level items (groups + singletons, no children)
  const topItems = stage.items.filter(it => !it.parent)

  return (
    <div className="stage-card" style={{
      border: isResume ? '2px solid #3b82f6' : '1px solid var(--border, #e5e7eb)',
      borderRadius: 8, marginBottom: 8,
      background: 'var(--bg-elevated, #fff)',
      boxShadow: isResume ? '0 0 0 3px #3b82f622' : 'none',
      transition: 'all 0.2s',
    }}>
      {/* Stage 헤더 */}
      <div
        style={{
          display: 'flex', alignItems: 'center', gap: 12,
          padding: '12px 16px', cursor: 'pointer',
          borderBottom: expanded ? '1px solid var(--border, #e5e7eb)' : 'none',
        }}
        onClick={onToggle}
      >
        <span className="v2-no-print" style={{ fontSize: 14, color: 'var(--muted, #6b7280)' }}>
          {expanded ? '▼' : '▶'}
        </span>
        <div style={{
          width: 28, height: 28, borderRadius: 14,
          background: color, color: '#fff',
          display: 'flex', alignItems: 'center', justifyContent: 'center',
          fontSize: 13, fontWeight: 700,
        }}>
          {stage.num}
        </div>
        <div style={{ flex: 1 }}>
          <div style={{ fontSize: 14, fontWeight: 600 }}>
            {stage.id} · {stage.title}
            <span style={{ marginLeft: 8, fontSize: 12, color: statusColor(status), fontWeight: 500 }}>
              {statusIcon(status)} {statusLabel(status)}
            </span>
          </div>
          <div style={{ fontSize: 11, color: 'var(--muted, #6b7280)' }}>{stage.desc}</div>
        </div>
        <div style={{ fontSize: 11, color: 'var(--muted, #6b7280)' }}>
          {done}/{total} 완료 · {fmtMs(elapsed)}
        </div>
        <button
          className="v2-no-print"
          onClick={e => { e.stopPropagation(); onStageToggle() }}
          disabled={anyRunning && !isThisRunning}
          title={isThisRunning ? `${stage.id} 단독 실행 중단` : `${stage.id} 만 실행`}
          style={{
            minWidth: 110, height: 28,
            padding: '0 12px', fontSize: 12, fontWeight: 600,
            background: isThisRunning ? '#dc2626' : '#fff',
            color: isThisRunning ? '#fff' : 'var(--text, #111827)',
            border: `1px solid ${isThisRunning ? '#dc2626' : 'var(--border, #d1d5db)'}`,
            borderRadius: 4,
            cursor: (anyRunning && !isThisRunning) ? 'not-allowed' : 'pointer',
            opacity: (anyRunning && !isThisRunning) ? 0.5 : 1,
          }}
        >
          {isThisRunning ? '⏹ 중단' : '▶ 검증'}
        </button>
      </div>

      {/* 펼침 영역 */}
      {expanded && (
        <div style={{ padding: 12 }}>
          <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12, tableLayout: 'fixed' }}>
            <colgroup>
              <col style={{ width: 32 }} />
              <col style={{ width: 36 }} />
              <col style={{ width: '28%' }} />
              <col style={{ width: '42%' }} />
              <col style={{ width: 120 }} />
              <col style={{ width: 70 }} />
              <col style={{ width: 90 }} />
            </colgroup>
            <thead>
              <tr style={{ background: 'var(--bg-muted, #f9fafb)', textAlign: 'left' }}>
                <th style={{ padding: '6px 8px' }}></th>
                <th style={{ padding: '6px 8px' }}>#</th>
                <th style={{ padding: '6px 8px' }}>항목</th>
                <th style={{ padding: '6px 8px' }}>설명</th>
                <th style={{ padding: '6px 8px' }}>진행률</th>
                <th style={{ padding: '6px 8px', textAlign: 'right' }}>소요</th>
                <th style={{ padding: '6px 8px' }}>결과</th>
              </tr>
            </thead>
            <tbody>
              {topItems.map((it, idx) => {
                const isGroup = it.isGroup === true
                const groupOpen = isGroup ? expandedGroups.has(it.id) : false
                const groupInfo = isGroup ? groupStatus(stage, it.id) : null
                const itStatus = isGroup && groupInfo ? groupInfo.status : it.status
                const itDone = itStatus === 'PASS' || itStatus === 'FAIL' || itStatus === 'SKIP'
                const itPct = itDone ? 100 : (itStatus === 'RUNNING' ? 50 : 0)
                const checked = selectedItems.has(it.id)
                const childList = isGroup ? stage.items.filter(c => c.parent === it.id) : []
                const childSelectedCount = childList.filter(c => selectedItems.has(c.id)).length
                const groupIndeterminate = isGroup && childSelectedCount > 0 && childSelectedCount < childList.length
                return (
                  <Fragment key={it.id}>
                    <tr style={{ borderTop: '1px solid var(--border, #f3f4f6)' }}>
                      <td style={{ padding: '6px 8px', textAlign: 'center' }}>
                        {isGroup ? (
                          <GroupCheckbox
                            checked={checked}
                            indeterminate={groupIndeterminate}
                            disabled={anyRunning}
                            onChange={() => toggleItemSelect(it.id)}
                          />
                        ) : (
                          <input
                            type="checkbox"
                            checked={checked}
                            onChange={() => toggleItemSelect(it.id)}
                            disabled={anyRunning}
                          />
                        )}
                      </td>
                      <td style={{ padding: '6px 8px', color: 'var(--muted, #9ca3af)' }}>{idx + 1}</td>
                      <td style={{ padding: '6px 8px' }}>
                        {isGroup && (
                          <span
                            onClick={() => toggleGroup(it.id)}
                            style={{ cursor: 'pointer', marginRight: 4, color: 'var(--muted, #6b7280)' }}
                          >
                            {groupOpen ? '▼' : '▶'}
                          </span>
                        )}
                        <code style={{ fontSize: 11, fontWeight: 600 }}>{it.id}</code>
                        <div style={{ fontSize: 11, color: 'var(--muted, #6b7280)', marginLeft: isGroup ? 16 : 0 }}>
                          {it.name}
                          {isGroup && groupInfo && (
                            <span style={{ marginLeft: 6, color: 'var(--muted, #9ca3af)' }}>
                              ({groupInfo.doneCount}/{groupInfo.totalCount} 자식)
                            </span>
                          )}
                        </div>
                      </td>
                      <td style={{ padding: '6px 8px', color: 'var(--muted, #6b7280)', fontSize: 11 }}>
                        {it.desc || '—'}
                      </td>
                      <td style={{ padding: '6px 8px' }}>
                        <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                          <div style={{ flex: 1, height: 6, background: 'var(--bg-muted, #f3f4f6)', borderRadius: 3, overflow: 'hidden' }}>
                            <div style={{
                              width: `${itPct}%`, height: '100%',
                              background: itDone ? statusColor(itStatus) : '#3b82f6',
                              transition: 'width 0.3s',
                            }} />
                          </div>
                          <span style={{ minWidth: 30, textAlign: 'right', fontSize: 10 }}>{itPct}%</span>
                        </div>
                      </td>
                      <td style={{ padding: '6px 8px', textAlign: 'right', fontSize: 10, color: 'var(--muted, #6b7280)' }}>
                        {itDone || isGroup ? fmtMs(it.elapsedMs || stage.items.filter(c => c.parent === it.id).reduce((s, c) => s + c.elapsedMs, 0)) : '–'}
                      </td>
                      <td style={{ padding: '6px 8px' }}>
                        <span style={{ color: statusColor(itStatus), fontWeight: 600 }}>
                          {statusIcon(itStatus)} {statusLabel(itStatus)}
                        </span>
                      </td>
                    </tr>
                    {/* 자식 항목 (그룹이 펼쳐진 경우만) */}
                    {isGroup && groupOpen && stage.items.filter(c => c.parent === it.id).map((c, ci) => {
                      const cDone = c.status === 'PASS' || c.status === 'FAIL' || c.status === 'SKIP'
                      const cPct = cDone ? 100 : (c.status === 'RUNNING' ? 50 : 0)
                      const cChecked = selectedItems.has(c.id)
                      return (
                        <tr key={c.id} style={{ background: 'var(--bg-muted, #fafafa)' }}>
                          <td style={{ padding: '4px 8px', textAlign: 'center' }}>
                            <input
                              type="checkbox"
                              checked={cChecked}
                              onChange={() => toggleItemSelect(c.id)}
                              disabled={anyRunning}
                            />
                          </td>
                          <td style={{ padding: '4px 8px', color: 'var(--muted, #9ca3af)', fontSize: 10 }}>
                            {idx + 1}.{ci + 1}
                          </td>
                          <td style={{ padding: '4px 8px', paddingLeft: 32 }}>
                            <code style={{ fontSize: 10, color: 'var(--muted, #6b7280)' }}>
                              └ {c.id.split('.').pop()}
                            </code>
                            <div style={{ fontSize: 10, color: 'var(--muted, #9ca3af)' }}>{c.name}</div>
                          </td>
                          <td style={{ padding: '4px 8px', color: 'var(--muted, #9ca3af)', fontSize: 10 }}>
                            {c.desc || '—'}
                          </td>
                          <td style={{ padding: '4px 8px' }}>
                            <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
                              <div style={{ flex: 1, height: 4, background: '#f3f4f6', borderRadius: 2, overflow: 'hidden' }}>
                                <div style={{
                                  width: `${cPct}%`, height: '100%',
                                  background: cDone ? statusColor(c.status) : '#3b82f6',
                                }} />
                              </div>
                              <span style={{ minWidth: 26, textAlign: 'right', fontSize: 9 }}>{cPct}%</span>
                            </div>
                          </td>
                          <td style={{ padding: '4px 8px', textAlign: 'right', fontSize: 9, color: 'var(--muted, #9ca3af)' }}>
                            {cDone ? fmtMs(c.elapsedMs) : '–'}
                          </td>
                          <td style={{ padding: '4px 8px' }}>
                            <span style={{ color: statusColor(c.status), fontWeight: 500, fontSize: 10 }}>
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
      )}
    </div>
  )
}

// ─────────────────────────────────────────────────────────────
// 메인 페이지
// ─────────────────────────────────────────────────────────────

export default function VerificationV2Page() {
  const [stages, setStages] = useState<Stage[]>(STAGES_INIT)
  const [expandedStages, setExpandedStages] = useState<Set<number>>(new Set([3]))   // S3 fail 상태라 펼쳐두기
  const [expandedGroups, setExpandedGroups] = useState<Set<string>>(new Set(['S5-CSC-DEPLOY']))
  const [selectedItems, setSelectedItems] = useState<Set<string>>(new Set())
  const [pipelineRunning, setPipelineRunning] = useState(false)
  const [soloStage, setSoloStage] = useState<number | null>(null)
  const [resumeStage, setResumeStage] = useState(1)
  const anyRunning = pipelineRunning || soloStage !== null

  const toggleStage = (n: number) => {
    setExpandedStages(prev => {
      const next = new Set(prev)
      if (next.has(n)) next.delete(n); else next.add(n)
      return next
    })
  }
  const toggleGroup = (id: string) => {
    setExpandedGroups(prev => {
      const next = new Set(prev)
      if (next.has(id)) next.delete(id); else next.add(id)
      return next
    })
  }
  // 그룹 cascade — 부모 토글 시 자식 모두, 자식 토글 시 부모 동기화
  const toggleItemSelect = (id: string) => {
    setSelectedItems(prev => {
      const next = new Set(prev)
      // id 가 어떤 stage 의 어떤 item 인지 찾기
      let item: MockItem | undefined
      let stItems: MockItem[] = []
      for (const st of stages) {
        const found = st.items.find(x => x.id === id)
        if (found) { item = found; stItems = st.items; break }
      }
      if (!item) return next

      if (item.isGroup) {
        // 부모: 자식 모두 선택돼있으면 모두 해제, 아니면 모두 선택
        const children = stItems.filter(c => c.parent === id)
        const allSelected = children.length > 0 && children.every(c => next.has(c.id))
        if (allSelected) {
          next.delete(id)
          children.forEach(c => next.delete(c.id))
        } else {
          next.add(id)
          children.forEach(c => next.add(c.id))
        }
      } else {
        // 일반 항목 또는 자식 항목
        if (next.has(id)) next.delete(id); else next.add(id)
        // 자식이라면 부모 동기화
        if (item.parent) {
          const sibs = stItems.filter(c => c.parent === item!.parent)
          const allSel = sibs.every(c => next.has(c.id))
          if (allSel) next.add(item.parent)
          else next.delete(item.parent)
        }
      }
      return next
    })
  }

  // 전체검증 시작/중단 toggle
  const togglePipeline = () => {
    if (pipelineRunning) {
      setPipelineRunning(false)
      return
    }
    if (soloStage !== null) return   // 다른 실행 중이면 무시
    // resume stage 이후 reset
    setStages(prev => prev.map(st => {
      if (st.num < resumeStage) return st
      return {
        ...st,
        items: st.items.map(it => ({ ...it, status: 'PENDING' as ItemStatus, elapsedMs: 0 })),
      }
    }))
    setPipelineRunning(true)
  }

  // 개별 stage 단독 실행/중단 toggle
  const toggleStageRun = (stageNum: number) => {
    if (soloStage === stageNum) {
      setSoloStage(null)
      return
    }
    if (pipelineRunning || soloStage !== null) return
    // 해당 stage reset
    setStages(prev => prev.map(st => {
      if (st.num !== stageNum) return st
      return {
        ...st,
        items: st.items.map(it => ({ ...it, status: 'PENDING' as ItemStatus, elapsedMs: 0 })),
      }
    }))
    setSoloStage(stageNum)
  }

  // PDF 보고서 출력 — 모든 stage 펼침 + 모든 그룹 펼침 → window.print
  const handlePrintReport = () => {
    const allStageNums = new Set(stages.map(s => s.num))
    const allGroupIds = new Set(
      stages.flatMap(st => st.items.filter(it => it.isGroup).map(it => it.id))
    )
    setExpandedStages(allStageNums)
    setExpandedGroups(allGroupIds)
    setTimeout(() => window.print(), 250)
  }

  // mock 진행 — anyRunning=true 일 때 700ms 간격으로 첫 PENDING 항목 → RUNNING → PASS
  useEffect(() => {
    if (!anyRunning) return
    const timer = setInterval(() => {
      setStages(prev => {
        let changed = false
        const next = prev.map(st => {
          if (changed) return st
          // soloStage 모드: 해당 stage 만 처리
          if (soloStage !== null && st.num !== soloStage) return st
          // pipeline 모드: resumeStage 미만 skip
          if (pipelineRunning && st.num < resumeStage) return st
          // running → pass
          const runIdx = st.items.findIndex(it => it.status === 'RUNNING' && !it.isGroup)
          if (runIdx >= 0) {
            const items = [...st.items]
            items[runIdx] = { ...items[runIdx], status: 'PASS', elapsedMs: 1500 + Math.floor(Math.random() * 3000) }
            changed = true
            return { ...st, items }
          }
          // first pending → running
          const pendIdx = st.items.findIndex(it => it.status === 'PENDING' && !it.isGroup)
          if (pendIdx >= 0) {
            const items = [...st.items]
            items[pendIdx] = { ...items[pendIdx], status: 'RUNNING' }
            changed = true
            return { ...st, items }
          }
          return st
        })
        if (!changed) {
          setTimeout(() => {
            setPipelineRunning(false)
            setSoloStage(null)
          }, 300)
        }
        return next
      })
    }, 700)
    return () => clearInterval(timer)
  }, [anyRunning, soloStage, pipelineRunning, resumeStage])

  return (
    <div className="verify-v2-page" style={{ padding: 16, maxWidth: 1400, margin: '0 auto' }}>
      <style>{`
        @media print {
          @page { margin: 3mm 15mm 2mm 15mm; size: A4; }
          html, body {
            background: #fff !important;
            margin: 0 !important; padding: 0 !important;
          }

          /* 부모 chain 의 layout/여백 제거 */
          .app-layout, .app-layout--collapsed,
          .app-content, .app-content-body,
          .verify-v2-page {
            display: block !important;
            margin: 0 !important;
            padding: 0 !important;
            grid-template-columns: 1fr !important;
            max-width: none !important;
            width: 100% !important;
          }

          /* 사이드바 / 헤더 / 서브탭 (layout 자체에서 제외) */
          .sidebar, .sidebar--collapsed,
          .app-header, .sub-tabs { display: none !important; }

          /* verify-v2 페이지 안의 .v2-report 외 모든 직접/간접 영역 layout 에서 제외 */
          .verify-v2-page > .v2-no-print,
          .verify-v2-page > .stage-card { display: none !important; }

          /* 보고서 보임 (화면에서는 inline style 의 display:none 으로 숨겨짐) */
          .v2-report {
            display: block !important;
            position: static !important;
            margin: 0 !important;
            padding: 0 !important;
            width: 100% !important;
          }

          /* 표/카드 page-break 회피 */
          .v2-report table { page-break-inside: auto; }
          .v2-report tr    { page-break-inside: avoid; }

          /* 색상 보존 */
          .v2-report * {
            -webkit-print-color-adjust: exact !important;
            print-color-adjust: exact !important;
          }
        }
      `}</style>

      <div className="v2-no-print" style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 12 }}>
        <h2 style={{ margin: 0, fontSize: 18 }}>검증 v2 (β) — 6단계 파이프라인</h2>
        <span style={{
          fontSize: 10, padding: '2px 8px',
          background: '#fef3c7', color: '#92400e',
          borderRadius: 4, fontWeight: 600,
        }}>
          PROTOTYPE · mock 데이터
        </span>
        <span style={{ marginLeft: 'auto', fontSize: 11, color: 'var(--muted, #6b7280)' }}>
          기존 검증 페이지: <a href="/testbed/verify">/testbed/verify</a>
        </span>
      </div>

      <div className="v2-no-print">
        <Stepper
          stages={stages}
          resumeStage={resumeStage}
          disabled={anyRunning}
          onSelect={n => setResumeStage(n)}
        />

        <GlobalHeader
          stages={stages}
          onPipelineToggle={togglePipeline}
          running={pipelineRunning}
          resumeStage={resumeStage}
          setResumeStage={setResumeStage}
          onPrintReport={handlePrintReport}
        />
      </div>

      <PrintReport stages={stages} resumeStage={resumeStage} />

      {stages.map(st => (
        <StageRow
          key={st.id}
          stage={st}
          expanded={expandedStages.has(st.num)}
          onToggle={() => toggleStage(st.num)}
          onStageToggle={() => toggleStageRun(st.num)}
          selectedItems={selectedItems}
          toggleItemSelect={toggleItemSelect}
          expandedGroups={expandedGroups}
          toggleGroup={toggleGroup}
          anyRunning={anyRunning}
          isThisRunning={soloStage === st.num}
          isResume={resumeStage === st.num}
        />
      ))}

      <div className="v2-no-print" style={{
        marginTop: 16, padding: 12,
        background: 'var(--bg-muted, #f9fafb)',
        border: '1px dashed var(--border, #d1d5db)',
        borderRadius: 6,
        fontSize: 11, color: 'var(--muted, #6b7280)',
      }}>
        <b>📝 프로토타입 안내</b>
        <ul style={{ margin: '6px 0', paddingLeft: 20 }}>
          <li>현재 mock 데이터 — 백엔드 미연결. 실제 검증은 <a href="/testbed/verify">/testbed/verify</a> 사용</li>
          <li>"Run Full Pipeline" 클릭 시 mock 시뮬레이션 (700ms 간격으로 PENDING → RUNNING → PASS)</li>
          <li>S3 는 Seed 항목에서 FAIL 상태로 초기화 — 후속 항목은 BLOCKED 표시 demo</li>
          <li>S5 는 그룹화 demo — S5-CSC-DEPLOY 가 펼쳐짐 (자식 3개), 나머지 그룹은 클릭으로 펼침</li>
          <li>Stepper / Header / Accordion / 그룹핑 UX 확인 후 피드백 부탁</li>
        </ul>
      </div>
    </div>
  )
}
