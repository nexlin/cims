// 자동 배포 — inventory.yaml + blueprint.yaml 업로드 → 검토·편집 → 계획 → 실행.
// 설계 정본: docs/design/features/auto_deployment.md §7
//
// 독립 페이지다(시스템/인프라의 탭이 아님): 좌측 서버 트리를 쓰지 않고, 실행이 수 분
// 걸리며 run 이력·재개·롤백이 영속 화면을 필요로 한다.
import { useCallback, useEffect, useRef, useState } from 'react'
import { useToast } from '../components/Toast'
import { useAdminCapable } from '../hooks/useAdminCapable'
import {
  provisionApi, blueprintRawUrl,
  type BlueprintSummary, type InventorySummary, type InventoryView,
  type ProvIssue, type PlanPhase, type Run, type RunSummary, type PreflightRow,
} from '../api/provision'

type Doc = 'blueprint' | 'inventory'
type View = 'form' | 'raw'

const PHASE_HINT: Record<string, string> = {
  AGENT:    'SSH 로 각 노드에 agent 설치 + enroll',
  TOPOLOGY: 'HA 그룹·멤버·VIP 구성',
  INSTALL:  '패키지 배치 (install job)',
  CONFIG:   '설정 주입 (overlay + collection)',
  START:    '순서대로 기동 (CMP→CSP, master 우선)',
  VERIFY:   '헬스체크 + VIP 보유 확인',
}

const STEP_ICON: Record<string, string> = {
  done: '✓', skipped: '–', failed: '✗', running: '⋯', pending: '·', aborted: '⊘',
}
const STEP_COLOR: Record<string, string> = {
  done: '#27ae60', skipped: 'var(--muted-foreground)', failed: '#e74c3c',
  running: '#1976d2', pending: 'var(--muted-foreground)', aborted: '#e67e22',
}

export default function AutoDeployPage() {
  const { show } = useToast()
  const canEdit = useAdminCapable()   // admin 세션 또는 admin 승격(sudo) 활성

  const [blueprints, setBlueprints]   = useState<BlueprintSummary[]>([])
  const [inventories, setInventories] = useState<InventorySummary[]>([])
  const [bpId, setBpId]   = useState<number | null>(null)
  const [invId, setInvId] = useState<number | null>(null)

  const [doc, setDoc]   = useState<Doc>('blueprint')
  const [view, setView] = useState<View>('form')
  const [bpRaw, setBpRaw]   = useState('')
  const [bpDoc, setBpDoc]   = useState<Record<string, unknown> | null>(null)
  const [invView, setInvView] = useState<InventoryView | null>(null)
  const [invRaw, setInvRaw]   = useState('')     // 원문 뷰 편집 버퍼 (서버는 원문을 안 돌려줌)

  const [issues, setIssues] = useState<ProvIssue[]>([])
  const [plan, setPlan]     = useState<PlanPhase[] | null>(null)
  const [preflight, setPreflight] = useState<PreflightRow[] | null>(null)
  const [busy, setBusy]     = useState('')

  const [runs, setRuns]   = useState<RunSummary[]>([])
  const [run, setRun]     = useState<Run | null>(null)
  const pollRef = useRef<number | null>(null)

  // ── 로드 ────────────────────────────────────────────────────
  const loadLists = useCallback(async () => {
    try {
      const [b, i, r] = await Promise.all([
        provisionApi.listBlueprints(), provisionApi.listInventories(),
        provisionApi.listRuns(),
      ])
      setBlueprints(b.blueprints || [])
      setInventories(i.inventories || [])
      setRuns(r.runs || [])
    } catch (e) { show((e as Error).message, 'err') }
  }, [show])

  useEffect(() => { void loadLists() }, [loadLists])

  useEffect(() => {
    if (bpId == null) { setBpRaw(''); setBpDoc(null); return }
    provisionApi.getBlueprint(bpId)
      .then(r => { setBpRaw(r.raw || ''); setBpDoc(r.doc || null) })
      .catch(e => show((e as Error).message, 'err'))
  }, [bpId, show])

  useEffect(() => {
    if (invId == null) { setInvView(null); setInvRaw(''); return }
    provisionApi.getInventory(invId)
      .then(r => setInvView(r.inventory))
      .catch(e => show((e as Error).message, 'err'))
  }, [invId, show])

  // 실행 중 run 폴링 (1초) — 게이트웨이 타임아웃 5초라 SSE 대신 폴링 (§2)
  useEffect(() => {
    if (!run || !['running', 'pending'].includes(run.status)) {
      if (pollRef.current) { window.clearInterval(pollRef.current); pollRef.current = null }
      return
    }
    pollRef.current = window.setInterval(async () => {
      try {
        const fresh = await provisionApi.getRun(run.id)
        setRun(fresh)
        if (!['running', 'pending'].includes(fresh.status)) void loadLists()
      } catch { /* 일시 오류는 다음 tick 에 회복 */ }
    }, 1000)
    return () => { if (pollRef.current) window.clearInterval(pollRef.current) }
  }, [run, loadLists])

  // ── 동작 ────────────────────────────────────────────────────
  async function upload(kind: Doc, file: File) {
    const text = await file.text()
    setBusy('업로드 중')
    try {
      if (kind === 'blueprint') {
        const r = await provisionApi.uploadBlueprint(text)
        setBpId(r.id); setIssues(r.issues || [])
        show(`블루프린트 '${r.name}' 업로드됨`, 'ok')
      } else {
        const r = await provisionApi.uploadInventory(text, file.name.replace(/\.ya?ml$/, ''))
        setInvId(r.id); setInvView(r.inventory); setIssues(r.issues || [])
        show(`인벤토리 '${r.name}' 업로드됨 (서버 ${r.inventory.servers.length})`, 'ok')
      }
      await loadLists()
    } catch (e) { show((e as Error).message, 'err') } finally { setBusy('') }
  }

  async function saveRaw() {
    setBusy('저장 중')
    try {
      if (doc === 'blueprint' && bpId != null) {
        const r = await provisionApi.saveBlueprint(bpId, { raw: bpRaw })
        setIssues(r.issues || []); show('블루프린트 저장됨', 'ok')
        const fresh = await provisionApi.getBlueprint(bpId)
        setBpDoc(fresh.doc || null)
      } else if (doc === 'inventory' && invId != null) {
        const r = await provisionApi.saveInventory(invId, { raw: invRaw })
        setIssues(r.issues || []); setInvView(r.inventory); show('인벤토리 저장됨', 'ok')
      }
    } catch (e) { show((e as Error).message, 'err') } finally { setBusy('') }
  }

  async function saveForm() {
    if (doc !== 'inventory' || invId == null || !invView) return
    if (!confirm('구성 뷰로 저장하면 원본 YAML 의 주석이 제거됩니다.\n계속할까요?\n\n' +
                 '(주석을 유지하려면 [원문 보기]에서 직접 편집하세요)')) return
    setBusy('저장 중')
    try {
      const r = await provisionApi.saveInventory(invId, { doc: invView })
      setIssues(r.issues || []); setInvView(r.inventory); show('인벤토리 저장됨', 'ok')
    } catch (e) { show((e as Error).message, 'err') } finally { setBusy('') }
  }

  async function doValidate() {
    if (bpId == null || invId == null) { show('블루프린트와 인벤토리를 모두 선택하세요', 'err'); return }
    setBusy('검증 중')
    try {
      const r = await provisionApi.validate(bpId, invId)
      setIssues(r.issues || [])
      show(r.ok ? '검증 통과' : `오류 ${r.issues.filter(i => i.level === 'error').length}건`,
           r.ok ? 'ok' : 'err')
    } catch (e) { show((e as Error).message, 'err') } finally { setBusy('') }
  }

  async function doPreflight() {
    if (invId == null) { show('인벤토리를 선택하세요', 'err'); return }
    setBusy('접속 확인 중 (수십 초 걸릴 수 있습니다)')
    try {
      const r = await provisionApi.preflight(invId)
      setPreflight(r.results)
      show(r.ok ? '전 서버 접속 가능' : '일부 서버 접속 실패', r.ok ? 'ok' : 'err')
    } catch (e) { show((e as Error).message, 'err') } finally { setBusy('') }
  }

  async function doPlan() {
    if (bpId == null || invId == null) { show('블루프린트와 인벤토리를 모두 선택하세요', 'err'); return }
    setBusy('계획 수립 중')
    try {
      const r = await provisionApi.dryRun(bpId, invId)
      setPlan(r.phases); setRun(null)
    } catch (e) { show((e as Error).message, 'err') } finally { setBusy('') }
  }

  async function doApply() {
    if (bpId == null || invId == null) return
    const total = (plan || []).reduce((n, p) => n + p.steps.length, 0)
    if (!confirm(`배포를 실행합니다.\n\n대상 서버에 agent 를 설치하고 모듈을 배치·기동합니다.\n` +
                 `총 ${total} 단계.\n\n계속할까요?`)) return
    setBusy('실행 시작')
    try {
      const r = await provisionApi.startRun(bpId, invId)
      const fresh = await provisionApi.getRun(r.run_id)
      setRun(fresh); setPlan(null)
    } catch (e) { show((e as Error).message, 'err') } finally { setBusy('') }
  }

  async function runAction(kind: 'resume' | 'abort' | 'rollback') {
    if (!run) return
    if (kind === 'rollback' &&
        !confirm('이 run 이 생성한 그룹·배포를 역순으로 제거합니다.\n' +
                 '(설치된 agent 는 제거되지 않습니다)\n\n계속할까요?')) return
    setBusy(kind)
    try {
      if (kind === 'resume') await provisionApi.resumeRun(run.id)
      else if (kind === 'abort') await provisionApi.abortRun(run.id)
      else await provisionApi.rollbackRun(run.id)
      setRun(await provisionApi.getRun(run.id))
    } catch (e) { show((e as Error).message, 'err') } finally { setBusy('') }
  }

  // ── 렌더 ────────────────────────────────────────────────────
  const errCount = issues.filter(i => i.level === 'error').length
  const ready = bpId != null && invId != null && errCount === 0

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 12, height: '100%', overflow: 'auto', padding: 14 }}>

      {/* ── 1. 문서 선택/업로드 ── */}
      <section style={SEC}>
        <h3 style={H3}>① 배포 정의</h3>
        <div style={{ display: 'flex', gap: 20, flexWrap: 'wrap' }}>
          <DocPicker label="블루프린트 (blueprint.yaml)" hint="무엇을 어떤 구조로 깔 것인가"
            items={blueprints.map(b => ({ id: b.id, label: b.name }))}
            value={bpId} onChange={setBpId} disabled={!canEdit}
            onUpload={f => upload('blueprint', f)} />
          <DocPicker label="인벤토리 (inventory.yaml)" hint="서버가 어디 있고 어떻게 로그인하나"
            items={inventories.map(i => ({ id: i.id, label: `${i.name} (서버 ${i.server_count})` }))}
            value={invId} onChange={setInvId} disabled={!canEdit}
            onUpload={f => upload('inventory', f)} />
        </div>
      </section>

      {/* ── 2. 검토·편집 ── */}
      {(bpId != null || invId != null) && (
        <section style={SEC}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10, marginBottom: 8 }}>
            <h3 style={{ ...H3, margin: 0 }}>② 검토·편집</h3>
            <Seg value={doc} onChange={v => setDoc(v as Doc)}
                 options={[{ v: 'blueprint', l: '블루프린트' }, { v: 'inventory', l: '인벤토리' }]} />
            <Seg value={view} onChange={v => setView(v as View)}
                 options={[{ v: 'form', l: '구성 보기' }, { v: 'raw', l: '원문 보기' }]} />
            <div style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
              {doc === 'blueprint' && bpId != null && (
                <a className="btn btn--sm btn--outline" href={blueprintRawUrl(bpId)} download>
                  ⤓ YAML 내려받기
                </a>
              )}
              {view === 'raw' && (
                <button className="btn btn--sm btn--primary" disabled={!canEdit || !!busy}
                        onClick={saveRaw}>저장 (주석 유지)</button>
              )}
              {view === 'form' && doc === 'inventory' && (
                <button className="btn btn--sm btn--primary" disabled={!canEdit || !!busy}
                        onClick={saveForm}>저장</button>
              )}
            </div>
          </div>

          {view === 'raw' ? (
            <RawEditor
              value={doc === 'blueprint' ? bpRaw : invRaw}
              onChange={doc === 'blueprint' ? setBpRaw : setInvRaw}
              placeholder={doc === 'inventory'
                ? '인벤토리 원문은 비밀정보를 담고 있어 서버가 돌려주지 않습니다.\n' +
                  '전체를 새로 붙여넣어 교체하려면 여기에 입력하세요.'
                : ''}
              issues={issues.filter(i => i.path.startsWith(doc))}
              disabled={!canEdit} />
          ) : doc === 'blueprint' ? (
            <BlueprintForm doc={bpDoc} issues={issues.filter(i => i.path.startsWith('blueprint'))} />
          ) : (
            <InventoryForm view={invView} onChange={setInvView} disabled={!canEdit}
                           issues={issues.filter(i => i.path.startsWith('inventory'))} />
          )}
        </section>
      )}

      {/* ── 지적 목록 ── */}
      {issues.length > 0 && (
        <section style={{ ...SEC, borderColor: errCount ? '#e74c3c' : '#f39c12' }}>
          <h3 style={H3}>검증 결과 — 오류 {errCount} · 경고 {issues.length - errCount}</h3>
          <div style={{ maxHeight: 180, overflow: 'auto', fontSize: 12.5 }}>
            {issues.map((i, n) => (
              <div key={n} style={{ padding: '3px 0', display: 'flex', gap: 8 }}>
                <span style={{ color: i.level === 'error' ? '#e74c3c' : '#f39c12', fontWeight: 700 }}>
                  {i.level === 'error' ? 'ERROR' : 'WARN'}
                </span>
                <code style={{ color: 'var(--muted-foreground)' }}>{i.path}</code>
                <span>{i.message}</span>
              </div>
            ))}
          </div>
        </section>
      )}

      {/* ── 3. 사전 확인 + 계획 ── */}
      <section style={SEC}>
        <h3 style={H3}>③ 사전 확인</h3>
        <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', alignItems: 'center' }}>
          <button className="btn btn--sm btn--outline" disabled={!canEdit || !!busy || invId == null}
                  onClick={doValidate}>검증</button>
          <button className="btn btn--sm btn--outline" disabled={!canEdit || !!busy || invId == null}
                  onClick={doPreflight} title="SSH·sudo 접속만 확인 — 아무것도 바꾸지 않습니다">
            접속 확인 (SSH/sudo)
          </button>
          <button className="btn btn--sm btn--outline" disabled={!canEdit || !!busy || !ready}
                  onClick={doPlan}>계획 확인 (dry-run)</button>
          <button className="btn btn--sm btn--primary" disabled={!canEdit || !!busy || !ready || !plan}
                  onClick={doApply} title={!plan ? '먼저 [계획 확인]' : ''}>▶ 배포 실행</button>
          {busy && <span style={{ fontSize: 12, color: 'var(--muted-foreground)' }}>{busy}…</span>}
        </div>

        {preflight && (
          <table className="table" style={{ marginTop: 10, fontSize: 12.5 }}>
            <thead><tr><th>서버</th><th>host</th><th>인증</th><th>OS</th><th>계정</th><th>sudo</th><th>결과</th></tr></thead>
            <tbody>
              {preflight.map(r => (
                <tr key={r.server}>
                  <td>{r.server}</td><td>{r.host}</td><td>{r.auth_mode}</td>
                  <td>{r.os || '-'}</td><td>{r.login_user || '-'}</td>
                  <td>{r.sudo_ok ? '✓' : '✗'}</td>
                  <td style={{ color: r.ok ? 'var(--cims-success)' : '#e74c3c' }}>
                    {r.ok ? 'OK' : `${r.error_code || ''} ${r.error || ''}`}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </section>

      {plan && <PlanView phases={plan} />}
      {run && <RunView run={run} onAction={runAction} busy={busy} canEdit={canEdit} />}

      {/* ── 최근 run ── */}
      {runs.length > 0 && !run && (
        <section style={SEC}>
          <h3 style={H3}>최근 배포</h3>
          <table className="table" style={{ fontSize: 12.5 }}>
            <thead><tr><th>#</th><th>블루프린트</th><th>상태</th><th>진행</th><th>시각</th><th /></tr></thead>
            <tbody>
              {runs.map(r => (
                <tr key={r.id}>
                  <td>{r.id}</td><td>{r.blueprint}</td>
                  <td style={{ color: r.status === 'succeeded' ? 'var(--cims-success)'
                             : r.status === 'failed' ? '#e74c3c' : 'var(--muted-foreground)' }}>
                    {r.status}</td>
                  <td>{r.progress.done}/{r.progress.total}
                      {r.progress.failed > 0 && ` (실패 ${r.progress.failed})`}</td>
                  <td style={{ color: 'var(--muted-foreground)' }}>{r.created_at}</td>
                  <td><button className="btn btn--sm btn--outline"
                              onClick={() => provisionApi.getRun(r.id).then(setRun)}>열기</button></td>
                </tr>
              ))}
            </tbody>
          </table>
        </section>
      )}
    </div>
  )
}

// ── 하위 컴포넌트 ─────────────────────────────────────────────

const SEC: React.CSSProperties = {
  border: '1px solid var(--border)', borderRadius: 6, padding: 12, background: 'var(--card)',
}
const H3: React.CSSProperties = { fontSize: 13.5, fontWeight: 700, margin: '0 0 8px' }

function Seg({ value, onChange, options }:
             { value: string; onChange: (v: string) => void; options: Array<{ v: string; l: string }> }) {
  return (
    <div style={{ display: 'inline-flex', border: '1px solid var(--border)', borderRadius: 4 }}>
      {options.map(o => (
        <button key={o.v} onClick={() => onChange(o.v)}
                style={{
                  padding: '3px 12px', fontSize: 12, border: 'none', cursor: 'pointer',
                  background: value === o.v ? '#1976d2' : 'transparent',
                  color: value === o.v ? '#fff' : 'var(--muted-foreground)',
                }}>{o.l}</button>
      ))}
    </div>
  )
}

function DocPicker({ label, hint, items, value, onChange, onUpload, disabled }: {
  label: string; hint: string
  items: Array<{ id: number; label: string }>
  value: number | null; onChange: (v: number | null) => void
  onUpload: (f: File) => void; disabled: boolean
}) {
  const ref = useRef<HTMLInputElement>(null)
  return (
    <div style={{ minWidth: 320, flex: 1 }}>
      <div style={{ fontSize: 12.5, fontWeight: 600 }}>{label}</div>
      <div style={{ fontSize: 11.5, color: 'var(--muted-foreground)', marginBottom: 5 }}>{hint}</div>
      <div style={{ display: 'flex', gap: 6 }}>
        <select value={value ?? ''} disabled={disabled}
                onChange={e => onChange(e.target.value ? Number(e.target.value) : null)}
                style={{ flex: 1 }}>
          <option value="">— 선택 —</option>
          {items.map(i => <option key={i.id} value={i.id}>{i.label}</option>)}
        </select>
        <button className="btn btn--sm btn--outline" disabled={disabled}
                onClick={() => ref.current?.click()}>⤒ 업로드</button>
        <input ref={ref} type="file" accept=".yaml,.yml" hidden
               onChange={e => { const f = e.target.files?.[0]; if (f) onUpload(f); e.target.value = '' }} />
      </div>
    </div>
  )
}

// monospace textarea + 줄번호. Monaco/CodeMirror 는 도입하지 않는다 —
// 콘솔 의존성 최소화(폐쇄망 번들) 정책 (§7.1).
function RawEditor({ value, onChange, issues, disabled, placeholder }: {
  value: string; onChange: (v: string) => void
  issues: ProvIssue[]; disabled: boolean; placeholder?: string
}) {
  const lines = value ? value.split('\n').length : 1
  return (
    <div style={{ display: 'flex', border: '1px solid var(--border)', borderRadius: 4,
                  fontFamily: 'monospace', fontSize: 12.5, maxHeight: 420 }}>
      <div style={{ padding: '8px 6px', textAlign: 'right', color: 'var(--muted-foreground)',
                    background: 'var(--background)', userSelect: 'none', overflow: 'hidden',
                    borderRight: '1px solid var(--border)', minWidth: 42 }}>
        {Array.from({ length: lines }, (_, i) => <div key={i} style={{ lineHeight: '18px' }}>{i + 1}</div>)}
      </div>
      <textarea value={value} onChange={e => onChange(e.target.value)}
                disabled={disabled} placeholder={placeholder} spellCheck={false}
                style={{ flex: 1, border: 'none', outline: 'none', resize: 'vertical',
                         padding: 8, minHeight: 260, lineHeight: '18px',
                         fontFamily: 'monospace', fontSize: 12.5,
                         background: 'transparent', color: 'var(--foreground)' }} />
      {issues.length > 0 && (
        <div style={{ flex: '0 0 220px', padding: 8, borderLeft: '1px solid var(--border)',
                      overflow: 'auto', fontSize: 11.5 }}>
          {issues.map((i, n) => (
            <div key={n} style={{ marginBottom: 6,
                                  color: i.level === 'error' ? '#e74c3c' : '#f39c12' }}>
              {i.path.replace(/^[a-z]+:/, '')}<br />
              <span style={{ color: 'var(--muted-foreground)' }}>{i.message}</span>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}

interface BpModule { package?: string; version?: string; process_name?: string; start?: boolean
                     config?: Record<string, unknown>; per_server?: Record<string, unknown>
                     collections?: Record<string, unknown[]> }
interface BpSystem { name?: string; mode?: string; members?: Array<{ server?: string; role?: string }>
                     vips?: Array<{ ip?: string; prefix?: number; interface?: string; slot?: string }>
                     modules?: BpModule[] }

// 블루프린트는 읽기 전용 트리로 보여준다 — 구조 편집은 원문 뷰에서.
// (시스템/모듈/컬렉션의 자유 구조를 폼으로 안전하게 편집하려면 스키마 UI 가 필요한데,
//  그건 콘솔 [패키지 설정] 탭이 이미 하는 일이라 배포 후 그쪽에서 조정하는 편이 낫다.)
function BlueprintForm({ doc, issues }: { doc: Record<string, unknown> | null; issues: ProvIssue[] }) {
  if (!doc) return <div className="empty">블루프린트를 선택하세요</div>
  const systems = (doc.systems as BpSystem[]) || []
  const order = (doc.start_order as string[]) || []
  const errFor = (p: string) => issues.find(i => i.path.includes(p))
  return (
    <div style={{ fontSize: 12.5 }}>
      <div style={{ marginBottom: 8, color: 'var(--muted-foreground)' }}>
        <b style={{ color: 'var(--foreground)' }}>{String(doc.name || '')}</b>
        {doc.description ? ` — ${doc.description}` : ''}
        {order.length > 0 && <> · 기동 순서: {order.join(' → ')}</>}
      </div>
      {systems.map((s, i) => (
        <div key={i} style={{ border: '1px solid var(--border)', borderRadius: 4,
                              padding: 10, marginBottom: 8 }}>
          <div style={{ fontWeight: 700 }}>
            {s.name} <span style={{ fontWeight: 400, color: 'var(--muted-foreground)' }}>· {s.mode}</span>
            {errFor(`systems[${i}]`) && <span style={{ color: '#e74c3c', marginLeft: 8 }}>⚠</span>}
          </div>
          <div style={{ color: 'var(--muted-foreground)', margin: '4px 0' }}>
            멤버: {(s.members || []).map(m => m.server + (m.role ? `(${m.role})` : '')).join(', ') || '-'}
            {(s.vips || []).length > 0 &&
              <> · VIP: {(s.vips || []).map(v => `${v.ip}/${v.prefix}@${v.interface}`).join(', ')}</>}
          </div>
          <table className="table" style={{ fontSize: 12 }}>
            <thead><tr><th>패키지</th><th>버전</th><th>프로세스</th><th>설정</th><th>컬렉션</th><th>기동</th></tr></thead>
            <tbody>
              {(s.modules || []).map((m, j) => (
                <tr key={j}>
                  <td>{m.package}</td><td>{m.version}</td><td>{m.process_name || '-'}</td>
                  <td>{Object.keys(m.config || {}).length + Object.keys(m.per_server || {}).length} 항목</td>
                  <td>{Object.entries(m.collections || {})
                        .map(([k, v]) => `${k}(${(v as unknown[]).length})`).join(', ') || '-'}</td>
                  <td>{m.start === false ? '수동' : '자동'}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ))}
    </div>
  )
}

function InventoryForm({ view, onChange, disabled, issues }: {
  view: InventoryView | null; onChange: (v: InventoryView) => void
  disabled: boolean; issues: ProvIssue[]
}) {
  if (!view) return <div className="empty">인벤토리를 선택하세요</div>
  const set = (idx: number, patch: Partial<InventoryView['servers'][0]>) => {
    const servers = view.servers.map((s, i) => i === idx ? { ...s, ...patch } : s)
    onChange({ ...view, servers })
  }
  return (
    <div>
      <table className="table" style={{ fontSize: 12.5 }}>
        <thead>
          <tr><th>서버 논리명</th><th>host</th><th>SSH 계정</th><th>포트</th>
              <th>SSH 비밀번호</th><th>sudo</th><th>sudo 비밀번호</th></tr>
        </thead>
        <tbody>
          {view.servers.map((s, i) => {
            const bad = issues.some(x => x.level === 'error' && x.path.includes(`servers[${i}]`))
            // agent 기설치 노드는 SSH 하지 않으므로 접속 칸을 비활성화한다.
            const pre = !!s.agent_preinstalled
            const lock = disabled || pre
            return (
              <tr key={i} style={bad ? { background: 'rgba(231,76,60,.08)' } : undefined}>
                <td>{s.name}
                  {pre && <div style={{ fontSize: 11, color: 'var(--muted-foreground)' }}>
                    agent 기설치 — SSH 안 함</div>}
                </td>
                <td><input value={s.host || ''} disabled={disabled} style={{ width: 130 }}
                           onChange={e => set(i, { host: e.target.value })} /></td>
                <td><input value={s.ssh?.user || ''} disabled={lock} style={{ width: 90 }}
                           onChange={e => set(i, { ssh: { ...s.ssh, user: e.target.value } })} /></td>
                <td><input type="number" value={s.ssh?.port ?? 22} disabled={lock} style={{ width: 64 }}
                           onChange={e => set(i, { ssh: { ...s.ssh, port: Number(e.target.value) } })} /></td>
                <td><input type="password" placeholder={pre ? '—' : '변경 안 함'}
                           disabled={lock} style={{ width: 110 }}
                           value={s.ssh?.password === '••••' ? '' : (s.ssh?.password || '')}
                           onChange={e => set(i, { ssh: { ...s.ssh, password: e.target.value } })} /></td>
                <td>
                  <select value={s.sudo?.method || 'password'} disabled={lock}
                          onChange={e => set(i, { sudo: { ...s.sudo, method: e.target.value } })}>
                    <option value="password">password</option>
                    <option value="nopasswd">nopasswd</option>
                  </select>
                </td>
                <td><input type="password" placeholder={pre ? '—' : '변경 안 함'}
                           disabled={lock} style={{ width: 110 }}
                           value={s.sudo?.password === '••••' ? '' : (s.sudo?.password || '')}
                           onChange={e => set(i, { sudo: { ...s.sudo, password: e.target.value } })} /></td>
              </tr>
            )
          })}
        </tbody>
      </table>
      <div style={{ fontSize: 11.5, color: 'var(--muted-foreground)', marginTop: 6 }}>
        비밀번호 칸을 비워 두면 저장된 값이 유지됩니다. 서버 추가·삭제는 [원문 보기]에서 하세요.
      </div>
    </div>
  )
}

function PlanView({ phases }: { phases: PlanPhase[] }) {
  const total = phases.reduce((n, p) => n + p.steps.length, 0)
  return (
    <section style={SEC}>
      <h3 style={H3}>계획 — 총 {total} 단계 (아직 아무것도 바뀌지 않았습니다)</h3>
      {phases.map(ph => (
        <div key={ph.key} style={{ marginBottom: 8 }}>
          <div style={{ fontSize: 12.5, fontWeight: 600 }}>
            {ph.key} · {ph.title}
            {ph.serial && <span style={{ marginLeft: 6, fontSize: 11, color: '#e67e22' }}>순차</span>}
            <span style={{ marginLeft: 8, fontWeight: 400, color: 'var(--muted-foreground)' }}>
              {PHASE_HINT[ph.key] || ''}
            </span>
          </div>
          {ph.error && <div style={{ color: '#e74c3c', fontSize: 12 }}>{ph.error}</div>}
          <div style={{ paddingLeft: 14, fontSize: 12, color: 'var(--muted-foreground)' }}>
            {ph.steps.map((s, i) => (
              <div key={i}>· {s.target} — {String(s.action || '')}</div>
            ))}
          </div>
        </div>
      ))}
    </section>
  )
}

function RunView({ run, onAction, busy, canEdit }: {
  run: Run; busy: string; canEdit: boolean
  onAction: (k: 'resume' | 'abort' | 'rollback') => void
}) {
  const running = run.status === 'running' || run.status === 'pending'
  const done = run.phases.reduce((n, p) =>
    n + p.steps.filter(s => s.status === 'done' || s.status === 'skipped').length, 0)
  const total = run.phases.reduce((n, p) => n + p.steps.length, 0)
  return (
    <section style={SEC}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, marginBottom: 8 }}>
        <h3 style={{ ...H3, margin: 0 }}>
          run #{run.id} — {run.blueprint}
          <span style={{
            marginLeft: 10, fontWeight: 400,
            color: run.status === 'succeeded' ? 'var(--cims-success)'
                 : run.status === 'failed' ? '#e74c3c' : '#1976d2',
          }}>{run.status}{running && ' ⋯'}</span>
        </h3>
        <span style={{ fontSize: 12, color: 'var(--muted-foreground)' }}>{done}/{total}</span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
          {running && <button className="btn btn--sm btn--outline" disabled={!canEdit || !!busy}
                              onClick={() => onAction('abort')}>■ 중단</button>}
          {!running && run.status !== 'succeeded' &&
            <button className="btn btn--sm btn--primary" disabled={!canEdit || !!busy}
                    onClick={() => onAction('resume')}>↻ 재개</button>}
          {!running && (run.created || []).length > 0 &&
            <button className="btn btn--sm btn--outline" disabled={!canEdit || !!busy}
                    onClick={() => onAction('rollback')}>⤺ 롤백</button>}
        </div>
      </div>

      {run.error && <div style={{ color: '#e74c3c', fontSize: 12.5, marginBottom: 8 }}>{run.error}</div>}
      {run.rollback && (
        <div style={{ fontSize: 12, marginBottom: 8 }}>
          롤백: 되돌림 {run.rollback.undone.length}건
          {run.rollback.failed.length > 0 &&
            <span style={{ color: '#e74c3c' }}> · 실패 {run.rollback.failed.join(' ; ')}</span>}
        </div>
      )}

      {run.phases.map(ph => (
        <div key={ph.key} style={{ marginBottom: 6 }}>
          <div style={{ fontSize: 12.5, fontWeight: 600 }}>
            {ph.key} · {ph.title}
            <span style={{ marginLeft: 8, fontWeight: 400, color: 'var(--muted-foreground)' }}>{ph.status}</span>
          </div>
          <div style={{ paddingLeft: 12 }}>
            {ph.steps.map((s, i) => (
              <div key={i} style={{ fontSize: 12, padding: '1px 0' }}>
                <span style={{ color: STEP_COLOR[s.status], fontWeight: 700, marginRight: 6 }}>
                  {STEP_ICON[s.status] || '·'}
                </span>
                <span style={{ display: 'inline-block', minWidth: 150 }}>{s.target}</span>
                <span style={{ color: s.status === 'failed' ? '#e74c3c' : 'var(--muted-foreground)' }}>
                  {s.detail || s.error || ''}
                </span>
                {s.elapsed_sec ? <span style={{ color: 'var(--muted-foreground)' }}> ({s.elapsed_sec}s)</span> : null}
              </div>
            ))}
          </div>
        </div>
      ))}
    </section>
  )
}
