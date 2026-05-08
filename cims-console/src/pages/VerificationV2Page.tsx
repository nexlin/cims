import { useState, useEffect, useRef, Fragment, useCallback } from 'react'

import { verifyApi, type VerifyStagesOverview, type ItemsProgress, type VerifyEnvResponse } from '../api/verification'
import { VerificationPrintReport } from '../components/VerificationPrintReport'

// ─────────────────────────────────────────────────────────────
// 검증 v2 — 6단계 (S1~S6) + 그룹핑
// 백엔드 (csc/src/handlers/verification.py) 와 연결.
//   - 초기 로드: GET /api/v1/verification/stages
//   - 실행:    POST /api/v1/verification/stages/<N> 또는 /run
//   - 폴링:    GET /api/v1/verification/jobs/<id>     (1.5s)
//   - 종료 시: run_id 표시 + 이력 페이지 (/testbed/verify-history) 안내
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
// API 응답 → Stage[] 변환
// ─────────────────────────────────────────────────────────────
function apiToStages(res: VerifyStagesOverview): Stage[] {
  return res.stages.map(s => ({
    num:   s.stage,
    id:    `S${s.stage}`,
    title: s.title,
    desc:  s.description || '',
    items: s.items.map(i => ({
      id:        i.id,
      name:      i.name,
      desc:      i.description || '',
      status:    'PENDING' as ItemStatus,
      elapsedMs: 0,
      isGroup:   i.is_group || false,
      parent:    i.parent || undefined,
    })),
  }))
}

// progress 결과를 stages 의 항목 status/elapsedMs 에 머지 (부모/자식 모두)
function mergeProgress(stages: Stage[], progress: ItemsProgress): Stage[] {
  const byId = new Map<string, { status: string; elapsed_ms: number }>()
  for (const it of progress.items) {
    byId.set(it.id, { status: it.status, elapsed_ms: it.elapsed_ms })
    for (const c of it.children) {
      byId.set(c.id, { status: c.status, elapsed_ms: c.elapsed_ms })
    }
  }
  return stages.map(st => ({
    ...st,
    items: st.items.map(item => {
      const p = byId.get(item.id)
      if (!p) return item
      return { ...item, status: p.status as ItemStatus, elapsedMs: p.elapsed_ms }
    }),
  }))
}

// 그룹 부모는 자식 worst-status 로 합산하여 표시 (UI 가시성)
function recomputeGroupStatus(stages: Stage[]): Stage[] {
  const RANK: Record<string, number> = { PASS: 0, PENDING: 0, RUNNING: 1, SKIP: 2, BLOCKED: 3, FAIL: 4 }
  return stages.map(st => {
    const items = [...st.items]
    for (let i = 0; i < items.length; i++) {
      const p = items[i]
      if (!p.isGroup) continue
      const kids = items.filter(c => c.parent === p.id)
      if (!kids.length) continue
      let worst: ItemStatus = 'PENDING'
      let total = 0
      let anyRunning = false
      for (const k of kids) {
        if (k.status === 'RUNNING') anyRunning = true
        if ((RANK[k.status] ?? 0) > (RANK[worst] ?? 0)) worst = k.status
        total += k.elapsedMs || 0
      }
      // PENDING 인 자식이 있고 다른 자식이 RUNNING/완료 면 RUNNING 으로
      const allDone = kids.every(k => k.status === 'PASS' || k.status === 'FAIL' || k.status === 'SKIP' || k.status === 'BLOCKED')
      if (!allDone && (anyRunning || kids.some(k => k.status !== 'PENDING'))) worst = 'RUNNING'
      items[i] = { ...p, status: worst, elapsedMs: total }
    }
    return { ...st, items }
  })
}

// 빈 fallback (API 로드 전)
const STAGES_FALLBACK: Stage[] = []

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
        const isBlocked = status === 'BLOCKED'

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
              title={
                disabled ? '실행 중에는 변경 불가'
                : isBlocked ? '선행 stage FAIL 로 차단됨 — 원인 stage 재실행 필요'
                : '클릭하여 재개 지점 설정'
              }
              style={{
                flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center',
                padding: '4px', cursor: disabled ? 'not-allowed' : 'pointer',
                background: 'transparent',
                border: '2px solid transparent',
                borderRadius: 8,
                textAlign: 'center', position: 'relative',
                opacity: disabled ? 0.7 : (isBlocked ? 0.6 : 1),
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
              {/* 외곽 ring (진행률 / 상태 색) — BLOCKED 면 점선 경계 + 회색 톤 */}
              <div style={{
                width: 120, height: 120, borderRadius: 60,
                background: isBlocked ? '#9ca3af' : ringBg,
                padding: 6,
                boxShadow: isResume ? `0 0 0 4px #3b82f633` : 'none',
                transition: 'all 0.2s',
                position: 'relative',
              }}>
                {isBlocked && (
                  <div style={{
                    position: 'absolute', top: 4, right: 4,
                    width: 28, height: 28, borderRadius: 14,
                    background: '#a16207', color: '#fff',
                    display: 'flex', alignItems: 'center', justifyContent: 'center',
                    fontSize: 14, fontWeight: 700,
                    boxShadow: '0 1px 3px rgba(0,0,0,0.3)',
                    zIndex: 2,
                  }} title="선행 stage FAIL 로 차단됨">
                    🚫
                  </div>
                )}
                {/* 내부 흰 원 — BLOCKED 면 회색 배경 */}
                <div style={{
                  width: '100%', height: '100%', borderRadius: '50%',
                  background: isBlocked ? '#f3f4f6' : '#fff',
                  display: 'flex', flexDirection: 'column',
                  alignItems: 'center', justifyContent: 'center',
                  fontWeight: 700, lineHeight: 1.2,
                }}>
                  <div style={{
                    fontSize: 26, color: isBlocked ? '#6b7280' : color, letterSpacing: 0.5,
                    textDecoration: isBlocked ? 'line-through' : 'none',
                  }}>{st.id}</div>
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
                    {isBlocked ? '차단됨' : `${done}/${total}`}
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
  onPrepReset, prepResetRunning, anyOtherRunning,
}: {
  stages: Stage[]
  onPipelineToggle: () => void
  running: boolean
  resumeStage: number
  setResumeStage: (n: number) => void
  onPrintReport: () => void
  onPrepReset: () => void
  prepResetRunning: boolean
  anyOtherRunning: boolean
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
        disabled={prepResetRunning}
        style={{
          minWidth: 160, height: 36,
          padding: '0 16px',
          background: running ? '#dc2626' : '#3b82f6',
          color: '#fff', border: 'none', borderRadius: 6,
          fontSize: 13, fontWeight: 700,
          cursor: prepResetRunning ? 'not-allowed' : 'pointer',
          opacity: prepResetRunning ? 0.5 : 1,
          transition: 'background 0.2s',
        }}
      >
        {running ? '⏹ 전체검증 중단' : '▶ 전체검증'}
      </button>

      {/* 데이터 초기화 (prep-reset) — 검증 회차에서 분리된 사전 cleanup */}
      <button
        onClick={onPrepReset}
        disabled={anyOtherRunning}
        title="dev/배포본 dist/, 로그, DB 일부 wipe (가입자/그룹은 보존). 회차 진입 전 1회 실행 권장."
        style={{
          minWidth: 140, height: 36,
          padding: '0 14px',
          background: prepResetRunning ? '#dc2626' : '#f59e0b',
          color: '#fff', border: 'none', borderRadius: 6,
          fontSize: 12, fontWeight: 600,
          cursor: anyOtherRunning ? 'not-allowed' : 'pointer',
          opacity: anyOtherRunning ? 0.5 : 1,
          transition: 'background 0.2s',
        }}
      >
        {prepResetRunning ? '⏹ 초기화 중단' : '🧹 데이터 초기화'}
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
  const isBlocked = status === 'BLOCKED'

  // top-level items (groups + singletons, no children)
  const topItems = stage.items.filter(it => !it.parent)

  return (
    <div className="stage-card" style={{
      border: isResume
        ? '2px solid #3b82f6'
        : isBlocked
          ? '1px dashed #a16207'
          : '1px solid var(--border, #e5e7eb)',
      borderRadius: 8, marginBottom: 8,
      // BLOCKED 면 옅은 amber tint 배경 (차단된 stage 가 한눈에)
      background: isBlocked ? '#fffbeb' : 'var(--bg-elevated, #fff)',
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
          {isBlocked ? '🚫' : stage.num}
        </div>
        <div style={{ flex: 1 }}>
          <div style={{ fontSize: 14, fontWeight: 600 }}>
            {stage.id} · {stage.title}
            <span style={{ marginLeft: 8, fontSize: 12, color: statusColor(status), fontWeight: 500 }}>
              {statusIcon(status)} {statusLabel(status)}
            </span>
            {isBlocked && (
              <span style={{
                marginLeft: 8, fontSize: 10, fontWeight: 700,
                padding: '1px 6px', borderRadius: 3,
                background: '#a16207', color: '#fff', letterSpacing: 0.3,
              }}>
                선행 FAIL 로 자동 차단
              </span>
            )}
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
                const itBlocked = itStatus === 'BLOCKED'
                const checked = selectedItems.has(it.id)
                const childList = isGroup ? stage.items.filter(c => c.parent === it.id) : []
                const childSelectedCount = childList.filter(c => selectedItems.has(c.id)).length
                const groupIndeterminate = isGroup && childSelectedCount > 0 && childSelectedCount < childList.length
                return (
                  <Fragment key={it.id}>
                    <tr style={{
                      borderTop: '1px solid var(--border, #f3f4f6)',
                      background: itBlocked ? '#fef3c7' : undefined,
                      opacity: itBlocked ? 0.7 : 1,
                    }}
                    title={itBlocked ? '선행 stage FAIL 로 차단됨 — 함수 호출 없이 BLOCKED' : undefined}>
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
                      const cBlocked = c.status === 'BLOCKED'
                      return (
                        <tr key={c.id} style={{
                          background: cBlocked ? '#fef3c7' : 'var(--bg-muted, #fafafa)',
                          opacity: cBlocked ? 0.7 : 1,
                        }}
                        title={cBlocked ? '선행 stage FAIL 로 차단됨' : undefined}>
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
  const [stages, setStages] = useState<Stage[]>(STAGES_FALLBACK)
  const [loading, setLoading] = useState(true)
  const [expandedStages, setExpandedStages] = useState<Set<number>>(new Set())
  const [expandedGroups, setExpandedGroups] = useState<Set<string>>(new Set(['S5-CSC-DEPLOY']))
  const [selectedItems, setSelectedItems] = useState<Set<string>>(new Set())
  const [pipelineRunning, setPipelineRunning] = useState(false)
  const [soloStage, setSoloStage] = useState<number | null>(null)
  const [resumeStage, setResumeStage] = useState(1)
  const [jobId, setJobId] = useState<string | null>(null)
  const [lastRunId, setLastRunId] = useState<number | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [stageGate, setStageGate] = useState<{ first_failed: number; blocked_stages: Record<number, number> } | null>(null)
  const [env, setEnv] = useState<VerifyEnvResponse | null>(null)
  const [prepResetRunning, setPrepResetRunning] = useState(false)
  const anyRunning = pipelineRunning || soloStage !== null || prepResetRunning

  // 초기 로드 — GET /verification/stages + /verification/env + /active 자동 부착
  useEffect(() => {
    let cancelled = false
    verifyApi.getStages()
      .then(async res => {
        if (cancelled) return
        setStages(apiToStages(res))
        setLoading(false)
        // 진행 중 회차가 있으면 자동 부착 — 다른 페이지 → 돌아왔을 때 이어서 폴링.
        // CLI 직접 실행 회차도 동일하게 진입점으로 표시 (source='cli').
        try {
          const active = await verifyApi.getActive()
          if (cancelled) return
          const running = (active.runs || []).find(r => !r.done)
          if (running) {
            const m = /^stage(\d+)$/.exec(running.scope || '')
            if (m) setSoloStage(parseInt(m[1], 10))
            else if (running.scope === 'preset:prep-reset') setPrepResetRunning(true)
            else setPipelineRunning(true)
            setJobId(running.job_id)
          }
        } catch { /* /active 미지원·일시 오류 무시 */ }
      })
      .catch(e => {
        if (cancelled) return
        setError(e instanceof Error ? e.message : String(e))
        setLoading(false)
      })
    verifyApi.getEnv()
      .then(res => { if (!cancelled) setEnv(res) })
      .catch(() => { /* PrintReport 에서 "-" 로 표시 */ })
    return () => { cancelled = true }
  }, [])

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
  const togglePipeline = useCallback(async () => {
    if (pipelineRunning) {
      setPipelineRunning(false)
      // 현재 backend 는 job kill API 없음 — 사용자에게 알림만
      return
    }
    if (soloStage !== null) return
    setError(null)
    // resumeStage 부터의 모든 부모/평면 항목 ID 모음 (그룹은 자식으로 자동 펼쳐짐)
    const items: string[] = stages
      .filter(st => st.num >= resumeStage)
      .flatMap(st => st.items.filter(it => !it.parent).map(it => it.id))
    // resumeStage 이후 reset
    setStages(prev => prev.map(st => {
      if (st.num < resumeStage) return st
      return {
        ...st,
        items: st.items.map(it => ({ ...it, status: 'PENDING' as ItemStatus, elapsedMs: 0 })),
      }
    }))
    try {
      const res = resumeStage <= 1
        ? await verifyApi.runArbitrary({ preset: 'pipeline-full', async: true })
        : await verifyApi.runArbitrary({ items, async: true })
      setJobId(res.job_id)
      setPipelineRunning(true)
    } catch (e: unknown) {
      setError('파이프라인 시작 실패: ' + (e instanceof Error ? e.message : String(e)))
    }
  }, [pipelineRunning, soloStage, resumeStage, stages])

  // 데이터 초기화 — prep-reset preset (S3-RESET + S5-RESET 묶음).
  // 검증 회차에서 분리된 사전 정리 단계. dist/ + DB 일부 wipe.
  const togglePrepReset = useCallback(async () => {
    if (prepResetRunning) return  // 진행 중에는 무시
    if (pipelineRunning || soloStage !== null) {
      setError('검증 진행 중에는 데이터 초기화 불가')
      return
    }
    if (!window.confirm('데이터 초기화 — dev/배포본 dist/, 로그, DB 일부 wipe '
                        + '(가입자/그룹은 보존). 진행하시겠습니까?')) return
    setError(null)
    try {
      const res = await verifyApi.runArbitrary({
        preset: 'prep-reset', async: true, trigger: 'user',
      })
      setJobId(res.job_id)
      setPrepResetRunning(true)
    } catch (e: unknown) {
      setError('데이터 초기화 시작 실패: ' + (e instanceof Error ? e.message : String(e)))
    }
  }, [prepResetRunning, pipelineRunning, soloStage])

  // 개별 stage 단독 실행/중단 toggle
  const toggleStageRun = useCallback(async (stageNum: number) => {
    if (soloStage === stageNum) {
      setSoloStage(null)
      return
    }
    if (pipelineRunning || soloStage !== null) return
    setError(null)
    // 해당 stage reset
    setStages(prev => prev.map(st => {
      if (st.num !== stageNum) return st
      return {
        ...st,
        items: st.items.map(it => ({ ...it, status: 'PENDING' as ItemStatus, elapsedMs: 0 })),
      }
    }))
    try {
      const res = await verifyApi.runStage(stageNum, { async: true })
      setJobId(res.job_id)
      setSoloStage(stageNum)
    } catch (e: unknown) {
      setError(`Stage ${stageNum} 시작 실패: ` + (e instanceof Error ? e.message : String(e)))
    }
  }, [pipelineRunning, soloStage])

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

  // 폴링 — jobId 가 set 되면 1.5s 간격으로 GET /jobs/<id>
  useEffect(() => {
    if (!jobId) return
    let cancelled = false
    let timer: ReturnType<typeof setTimeout> | null = null

    const tick = async () => {
      try {
        const job = await verifyApi.getJob(jobId)
        if (cancelled) return
        // 진행 결과를 stages 에 머지 + 그룹 부모 status 재계산
        setStages(prev => recomputeGroupStatus(mergeProgress(prev, job.items_progress)))
        setStageGate(job.items_progress?.stage_gate ?? null)
        if (job.done) {
          setPipelineRunning(false)
          setSoloStage(null)
          setPrepResetRunning(false)
          if (job.run_id) setLastRunId(job.run_id)
          if (job.verdict === 'FAIL') {
            setError(`검증 FAIL — 회차 #${job.run_id ?? '?'} (이력 페이지 참조)`)
          }
          setJobId(null)
          return
        }
      } catch (e: unknown) {
        if (!cancelled) {
          // 일시 네트워크 에러는 무시하고 계속 폴링
          // eslint-disable-next-line no-console
          console.warn('jobs polling 일시 실패', e)
        }
      }
      if (!cancelled) timer = setTimeout(tick, 1500)
    }
    tick()
    return () => {
      cancelled = true
      if (timer) clearTimeout(timer)
    }
  }, [jobId])

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
        <h2 style={{ margin: 0, fontSize: 18 }}>검증 v2 — 6단계 파이프라인</h2>
        <span style={{
          fontSize: 10, padding: '2px 8px',
          background: '#dcfce7', color: '#15803d',
          borderRadius: 4, fontWeight: 600,
        }}>
          LIVE
        </span>
        {loading && (
          <span style={{ fontSize: 12, color: '#6b7280' }}>로딩 중…</span>
        )}
        {error && (
          <span style={{ fontSize: 12, color: '#dc2626', maxWidth: 600, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
            ⚠ {error}
          </span>
        )}
        {lastRunId !== null && (
          <span style={{ fontSize: 12, color: '#6b7280' }}>
            마지막 회차: <a href="/testbed/verify-history">#{lastRunId}</a>
          </span>
        )}
        <span style={{ marginLeft: 'auto', fontSize: 11, color: 'var(--muted, #6b7280)' }}>
          이력: <a href="/testbed/verify-history">/testbed/verify-history</a>
          {' · '}구버전: <a href="/testbed/verify">/testbed/verify</a>
        </span>
      </div>

      {stageGate && (
        <div
          className="v2-no-print"
          style={{
            display: 'flex', alignItems: 'center', gap: 8,
            padding: '8px 12px', marginBottom: 10,
            background: '#fef3c7', border: '1px solid #f59e0b',
            borderRadius: 6, fontSize: 12, color: '#92400e',
          }}
        >
          <span style={{ fontSize: 16 }}>🚫</span>
          <span>
            <b>Stage Gate 발동</b> — Stage <b>S{stageGate.first_failed}</b> 의 FAIL 로
            후속 단계{' '}
            <b>
              {Object.keys(stageGate.blocked_stages)
                .map(n => `S${n}`).join(', ')}
            </b>
            {' '}의{' '}
            {Object.values(stageGate.blocked_stages).reduce((a, b) => a + b, 0)}건이
            자동 BLOCKED 처리되었습니다. 해당 stage 부터 재검증 필요.
          </span>
        </div>
      )}

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
          onPrepReset={togglePrepReset}
          prepResetRunning={prepResetRunning}
          anyOtherRunning={pipelineRunning || soloStage !== null}
        />
      </div>

      <VerificationPrintReport
        stages={stages}
        resumeStage={resumeStage}
        meta={env ? {
          host:        env.host,
          gitBranch:   env.git_branch,
          gitSha:      env.git_sha,
          pkgManifest: env.pkg_manifest_hash || '-',
        } : {}}
      />

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
        <b>ℹ 안내</b>
        <ul style={{ margin: '6px 0', paddingLeft: 20 }}>
          <li>전체검증 ▶ — Stepper 의 재개 지점부터 시작 (S1=처음이면 <code>pipeline-full</code> preset)</li>
          <li>Stage 단독 ▶ — 해당 stage 의 부모/평면 항목만 (그룹은 자식 자동 포함)</li>
          <li>1.5초 폴링으로 진행 상태 갱신. 완료 시 회차 #ID 가 위에 표시되고 <a href="/testbed/verify-history">이력 페이지</a>에 자동 기록됨</li>
          <li>S5 22 step 모두 native Python 포팅 완료 — 자식 단독 실행 가능 (예: <code>S5-CSC-DEPLOY-INSTALL</code> 만 선택)</li>
          <li>S6-ENTRY-CHECK 가 immutability gate 검사 — S5-MODULES-RUN-START 가 기록한 <code>.deployed-manifest.json</code> ↔ <code>packages/manifest.json</code> SHA-256 매칭</li>
          <li>이력은 파일 기반 — <code>verify_runs/YYYY/MM/&lt;id&gt;.json</code> (DB 의존 X)</li>
        </ul>
      </div>
    </div>
  )
}
