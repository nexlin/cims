// 자동 배포 — inventory.yaml + blueprint.yaml 업로드 → 검토·편집 → 계획 → 실행.
// 설계 정본: docs/design/features/auto_deployment.md §7
//
// 독립 페이지다(시스템/인프라의 탭이 아님): 좌측 서버 트리를 쓰지 않고, 실행이 수 분
// 걸리며 run 이력·재개·롤백이 영속 화면을 필요로 한다.
import type React from 'react'
import { useConfirm } from '../components/custom/confirm'
import { AlertTriangle, Ban, Check, Dot, Download, Hourglass, Minus, Play, RotateCw, Square, Undo2, X } from 'lucide-react'
import { useCallback, useEffect, useRef, useState } from 'react'
import { useToast } from '../components/Toast'
import { useAdminCapable } from '../hooks/useAdminCapable'
import {
  provisionApi, blueprintRawUrl,
  type BlueprintSummary, type InventorySummary, type InventoryView,
  type ProvIssue, type PlanPhase, type Run, type RunSummary, type PreflightRow,
} from '../api/provision'
import { Button } from '@core/components/ui/button'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { NONE, fromSel, toSel } from '@core/components/custom/select-value'
import { EmptyState } from '@core/components/custom/empty-state'
import { DataTable, Th, Td } from '@core/components/custom/data-table'

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

const STEP_ICON: Record<string, React.ReactNode> = {
  done: <Check size={13} className="inline align-[-2px]" />,
  skipped: <Minus size={13} className="inline align-[-2px]" />,
  failed: <X size={13} className="inline align-[-2px]" />,
  running: <Hourglass size={13} className="inline align-[-2px]" />,
  pending: <Dot size={13} className="inline align-[-2px]" />,
  aborted: <Ban size={13} className="inline align-[-2px]" />,
}
const STEP_COLOR: Record<string, string> = {
  done: 'var(--cims-success)', skipped: 'var(--muted-foreground)', failed: 'var(--destructive)',
  running: 'var(--primary)', pending: 'var(--muted-foreground)', aborted: 'var(--cims-warning)',
}

export default function AutoDeployPage() {
  const { show } = useToast()
  const confirm = useConfirm()
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
    if (!await confirm({ title: '구성 뷰로 저장', confirmLabel: '저장', body: <>
      구성 뷰로 저장하면 원본 YAML 의 주석이 제거됩니다. 계속할까요?
      <div className="mt-2">(주석을 유지하려면 [원문 보기]에서 직접 편집하세요)</div>
    </> })) return
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
    if (!await confirm({ title: '배포 실행', confirmLabel: '실행', body: <>
      배포를 실행합니다.
      <div className="mt-2">대상 서버에 agent 를 설치하고 모듈을 배치·기동합니다. 총 {total} 단계.</div>
      <div className="mt-2">계속할까요?</div>
    </> })) return
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
        !await confirm({ title: '배포 롤백', tone: 'danger', confirmLabel: '롤백', body: <>
          이 run 이 생성한 그룹·배포를 역순으로 제거합니다.
          <div className="mt-1">(설치된 agent 는 제거되지 않습니다)</div>
          <div className="mt-2">계속할까요?</div>
        </> })) return
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
    <div className="flex flex-col gap-3 h-full overflow-auto p-3.5">

      {/* ── 1. 문서 선택/업로드 ── */}
      <section style={SEC}>
        <h3 style={H3}>① 배포 정의</h3>
        <div className="flex gap-5 flex-wrap">
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
          <div className="flex items-center gap-2.5 mb-2">
            <h3 style={{ ...H3, margin: 0 }}>② 검토·편집</h3>
            <Seg value={doc} onChange={v => setDoc(v as Doc)}
                 options={[{ v: 'blueprint', l: '블루프린트' }, { v: 'inventory', l: '인벤토리' }]} />
            <Seg value={view} onChange={v => setView(v as View)}
                 options={[{ v: 'form', l: '구성 보기' }, { v: 'raw', l: '원문 보기' }]} />
            <div className="ml-auto flex gap-1.5">
              {doc === 'blueprint' && bpId != null && (
                <Button asChild>
                  <a href={blueprintRawUrl(bpId)} download><Download size={13} /> YAML 내려받기</a>
                </Button>
              )}
              {view === 'raw' && (
                <Button variant="default" disabled={!canEdit || !!busy}
                        onClick={saveRaw}>저장 (주석 유지)</Button>
              )}
              {view === 'form' && doc === 'inventory' && (
                <Button variant="default" disabled={!canEdit || !!busy}
                        onClick={saveForm}>저장</Button>
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
        <section style={{ ...SEC, borderColor: errCount ? 'var(--destructive)' : 'var(--cims-warning)' }}>
          <h3 style={H3}>검증 결과 — 오류 {errCount} · 경고 {issues.length - errCount}</h3>
          <div className="max-h-[180px] overflow-auto text-sm">
            {issues.map((i, n) => (
              <div className="py-[3px] px-0 flex gap-2" key={n}>
                <span style={{ color: i.level === 'error' ? 'var(--destructive)' : 'var(--cims-warning)', fontWeight: 700 }}>
                  {i.level === 'error' ? 'ERROR' : 'WARN'}
                </span>
                <code className="text-muted-foreground">{i.path}</code>
                <span>{i.message}</span>
              </div>
            ))}
          </div>
        </section>
      )}

      {/* ── 3. 사전 확인 + 계획 ── */}
      <section style={SEC}>
        <h3 style={H3}>③ 사전 확인</h3>
        <div className="flex gap-2 flex-wrap items-center">
          <Button disabled={!canEdit || !!busy || invId == null}
                  onClick={doValidate}>검증</Button>
          <Button disabled={!canEdit || !!busy || invId == null}
                  onClick={doPreflight} title="SSH·sudo 접속만 확인 — 아무것도 바꾸지 않습니다">
            접속 확인 (SSH/sudo)
          </Button>
          <Button disabled={!canEdit || !!busy || !ready}
                  onClick={doPlan}>계획 확인 (dry-run)</Button>
          <Button variant="default" disabled={!canEdit || !!busy || !ready || !plan}
                  onClick={doApply} title={!plan ? '먼저 [계획 확인]' : ''}><Play size={13} /> 배포 실행</Button>
          {busy && <span className="text-sm text-muted-foreground">{busy}…</span>}
        </div>

        {preflight && (
          <DataTable sticky className="mt-2.5 text-sm">
            <thead><tr><Th>서버</Th><Th>host</Th><Th>인증</Th><Th>OS</Th><Th>계정</Th><Th>sudo</Th><Th>결과</Th></tr></thead>
            <tbody>
              {preflight.map(r => (
                <tr key={r.server}>
                  <Td>{r.server}</Td><Td>{r.host}</Td><Td>{r.auth_mode}</Td>
                  <Td>{r.os || '-'}</Td><Td>{r.login_user || '-'}</Td>
                  <Td>{r.sudo_ok ? <Check size={13} className="text-success" />
                             : <X size={13} className="text-destructive" />}</Td>
                  <Td style={{ color: r.ok ? 'var(--cims-success)' : 'var(--destructive)' }}>
                    {r.ok ? 'OK' : `${r.error_code || ''} ${r.error || ''}`}
                  </Td>
                </tr>
              ))}
            </tbody>
          </DataTable>
        )}
      </section>

      {plan && <PlanView phases={plan} />}
      {run && <RunView run={run} onAction={runAction} busy={busy} canEdit={canEdit} />}

      {/* ── 최근 run ── */}
      {runs.length > 0 && !run && (
        <section style={SEC}>
          <h3 style={H3}>최근 배포</h3>
          <DataTable sticky className="text-sm">
            <thead><tr><Th>#</Th><Th>블루프린트</Th><Th>상태</Th><Th>진행</Th><Th>시각</Th><Th /></tr></thead>
            <tbody>
              {runs.map(r => (
                <tr key={r.id}>
                  <Td>{r.id}</Td><Td>{r.blueprint}</Td>
                  <Td style={{ color: r.status === 'succeeded' ? 'var(--cims-success)'
                             : r.status === 'failed' ? 'var(--destructive)' : 'var(--muted-foreground)' }}>
                    {r.status}</Td>
                  <Td>{r.progress.done}/{r.progress.total}
                      {r.progress.failed > 0 && ` (실패 ${r.progress.failed})`}</Td>
                  <Td className="text-muted-foreground">{r.created_at}</Td>
                  <Td><Button
                              onClick={() => provisionApi.getRun(r.id).then(setRun)}>열기</Button></Td>
                </tr>
              ))}
            </tbody>
          </DataTable>
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
    <div className="inline-flex border border-border rounded-sm">
      {options.map(o => (
        <button key={o.v} onClick={() => onChange(o.v)}
                style={{
                  padding: '3px 12px', fontSize: 12, border: 'none', cursor: 'pointer',
                  background: value === o.v ? 'var(--cims-info)' : 'transparent',
                  color: value === o.v ? 'var(--cims-on-solid)' : 'var(--muted-foreground)',
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
    <div className="min-w-[320px] flex-1">
      <div className="text-sm font-semibold">{label}</div>
      <div className="text-xs text-muted-foreground mb-[5px]">{hint}</div>
      <div className="flex gap-1.5">
        <Select value={toSel(value == null ? '' : String(value))}
                onValueChange={(v: string) => onChange(fromSel(v) ? Number(fromSel(v)) : null)} disabled={disabled}>
          <SelectTrigger className="flex-1"><SelectValue /></SelectTrigger>
          <SelectContent>
            <SelectItem value={NONE}>— 선택 —</SelectItem>
            {items.map(i => <SelectItem key={i.id} value={String(i.id)}>{i.label}</SelectItem>)}
          </SelectContent>
        </Select>
        <Button disabled={disabled}
                onClick={() => ref.current?.click()}>⤒ 업로드</Button>
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
    <div className="flex border border-border rounded-sm font-mono text-sm max-h-[420px]">
      <div className="py-2 px-1.5 text-right text-muted-foreground bg-background select-none overflow-hidden border-r border-border min-w-[42px]">
        {Array.from({ length: lines }, (_, i) => <div key={i} style={{ lineHeight: '18px' }}>{i + 1}</div>)}
      </div>
      <textarea value={value} onChange={e => onChange(e.target.value)}
                disabled={disabled} placeholder={placeholder} spellCheck={false}
                style={{ flex: 1, border: 'none', outline: 'none', resize: 'vertical',
                         padding: 8, minHeight: 260, lineHeight: '18px',
                         fontFamily: 'monospace', fontSize: 12.5,
                         background: 'transparent', color: 'var(--foreground)' }} />
      {issues.length > 0 && (
        <div className="flex-[0_0_220px] p-2 border-l border-border overflow-auto text-xs">
          {issues.map((i, n) => (
            <div key={n} style={{ marginBottom: 6,
                                  color: i.level === 'error' ? 'var(--destructive)' : 'var(--cims-warning)' }}>
              {i.path.replace(/^[a-z]+:/, '')}<br />
              <span className="text-muted-foreground">{i.message}</span>
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
  if (!doc) return <EmptyState title="블루프린트를 선택하세요" />
  const systems = (doc.systems as BpSystem[]) || []
  const order = (doc.start_order as string[]) || []
  const errFor = (p: string) => issues.find(i => i.path.includes(p))
  return (
    <div className="text-sm">
      <div className="mb-2 text-muted-foreground">
        <b className="text-foreground">{String(doc.name || '')}</b>
        {doc.description ? ` — ${doc.description}` : ''}
        {order.length > 0 && <> · 기동 순서: {order.join(' → ')}</>}
      </div>
      {systems.map((s, i) => (
        <div className="border border-border rounded-sm p-2.5 mb-2" key={i}>
          <div className="font-bold">
            {s.name} <span className="font-normal text-muted-foreground">· {s.mode}</span>
            {errFor(`systems[${i}]`) && <AlertTriangle size={13} className="ml-2 inline text-destructive" />}
          </div>
          <div className="text-muted-foreground my-1 mx-0">
            멤버: {(s.members || []).map(m => m.server + (m.role ? `(${m.role})` : '')).join(', ') || '-'}
            {(s.vips || []).length > 0 &&
              <> · VIP: {(s.vips || []).map(v => `${v.ip}/${v.prefix}@${v.interface}`).join(', ')}</>}
          </div>
          <DataTable sticky className="text-sm">
            <thead><tr><Th>패키지</Th><Th>버전</Th><Th>프로세스</Th><Th>설정</Th><Th>컬렉션</Th><Th>기동</Th></tr></thead>
            <tbody>
              {(s.modules || []).map((m, j) => (
                <tr key={j}>
                  <Td>{m.package}</Td><Td>{m.version}</Td><Td>{m.process_name || '-'}</Td>
                  <Td>{Object.keys(m.config || {}).length + Object.keys(m.per_server || {}).length} 항목</Td>
                  <Td>{Object.entries(m.collections || {})
                        .map(([k, v]) => `${k}(${(v as unknown[]).length})`).join(', ') || '-'}</Td>
                  <Td>{m.start === false ? '수동' : '자동'}</Td>
                </tr>
              ))}
            </tbody>
          </DataTable>
        </div>
      ))}
    </div>
  )
}

function InventoryForm({ view, onChange, disabled, issues }: {
  view: InventoryView | null; onChange: (v: InventoryView) => void
  disabled: boolean; issues: ProvIssue[]
}) {
  if (!view) return <EmptyState title="인벤토리를 선택하세요" />
  const set = (idx: number, patch: Partial<InventoryView['servers'][0]>) => {
    const servers = view.servers.map((s, i) => i === idx ? { ...s, ...patch } : s)
    onChange({ ...view, servers })
  }
  return (
    <div>
      <DataTable sticky className="text-sm">
        <thead>
          <tr><Th>서버 논리명</Th><Th>host</Th><Th>SSH 계정</Th><Th>포트</Th>
              <Th>SSH 비밀번호</Th><Th>sudo</Th><Th>sudo 비밀번호</Th></tr>
        </thead>
        <tbody>
          {view.servers.map((s, i) => {
            const bad = issues.some(x => x.level === 'error' && x.path.includes(`servers[${i}]`))
            // agent 기설치 노드는 SSH 하지 않으므로 접속 칸을 비활성화한다.
            const pre = !!s.agent_preinstalled
            const lock = disabled || pre
            return (
              <tr key={i} style={bad ? { background: 'rgba(231,76,60,.08)' } : undefined}>
                <Td>{s.name}
                  {pre && <div className="text-xs text-muted-foreground">
                    agent 기설치 — SSH 안 함</div>}
                </Td>
                <Td><input className="w-[130px]" value={s.host || ''} disabled={disabled}
                           onChange={e => set(i, { host: e.target.value })}/></Td>
                <Td><input className="w-[90px]" value={s.ssh?.user || ''} disabled={lock}
                           onChange={e => set(i, { ssh: { ...s.ssh, user: e.target.value } })}/></Td>
                <Td><input className="w-[64px]" type="number" value={s.ssh?.port ?? 22} disabled={lock}
                           onChange={e => set(i, { ssh: { ...s.ssh, port: Number(e.target.value) } })}/></Td>
                <Td><input className="w-[110px]" type="password" placeholder={pre ? '—' : '변경 안 함'}
                           disabled={lock}
                           value={s.ssh?.password === '••••' ? '' : (s.ssh?.password || '')}
                           onChange={e => set(i, { ssh: { ...s.ssh, password: e.target.value } })}/></Td>
                <Td>
                  <Select value={toSel(s.sudo?.method || 'password')} onValueChange={(v: string) => set(i, { sudo: { ...s.sudo, method: fromSel(v) } })} disabled={lock}>
                    <SelectTrigger><SelectValue /></SelectTrigger>
                    <SelectContent>
                      <SelectItem value="password">password</SelectItem>
                      <SelectItem value="nopasswd">nopasswd</SelectItem>
                    </SelectContent>
                  </Select>
                </Td>
                <Td><input className="w-[110px]" type="password" placeholder={pre ? '—' : '변경 안 함'}
                           disabled={lock}
                           value={s.sudo?.password === '••••' ? '' : (s.sudo?.password || '')}
                           onChange={e => set(i, { sudo: { ...s.sudo, password: e.target.value } })}/></Td>
              </tr>
            )
          })}
        </tbody>
      </DataTable>
      <div className="text-xs text-muted-foreground mt-1.5">
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
        <div className="mb-2" key={ph.key}>
          <div className="text-sm font-semibold">
            {ph.key} · {ph.title}
            {ph.serial && <span className="ml-1.5 text-xs text-warning">순차</span>}
            <span className="ml-2 font-normal text-muted-foreground">
              {PHASE_HINT[ph.key] || ''}
            </span>
          </div>
          {ph.error && <div className="text-destructive text-sm">{ph.error}</div>}
          <div className="pl-3.5 text-sm text-muted-foreground">
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
      <div className="flex items-center gap-2.5 mb-2">
        <h3 style={{ ...H3, margin: 0 }}>
          run #{run.id} — {run.blueprint}
          <span style={{
            marginLeft: 10, fontWeight: 400,
            color: run.status === 'succeeded' ? 'var(--cims-success)'
                 : run.status === 'failed' ? 'var(--destructive)' : 'var(--cims-info)',
          }}>{run.status}{running && ' ⋯'}</span>
        </h3>
        <span className="text-sm text-muted-foreground">{done}/{total}</span>
        <div className="ml-auto flex gap-1.5">
          {running && <Button disabled={!canEdit || !!busy}
                              onClick={() => onAction('abort')}><Square size={13} /> 중단</Button>}
          {!running && run.status !== 'succeeded' &&
            <Button variant="default" disabled={!canEdit || !!busy}
                    onClick={() => onAction('resume')}><RotateCw size={13} /> 재개</Button>}
          {!running && (run.created || []).length > 0 &&
            <Button disabled={!canEdit || !!busy}
                    onClick={() => onAction('rollback')}><Undo2 size={13} /> 롤백</Button>}
        </div>
      </div>

      {run.error && <div className="text-destructive text-sm mb-2">{run.error}</div>}
      {run.rollback && (
        <div className="text-sm mb-2">
          롤백: 되돌림 {run.rollback.undone.length}건
          {run.rollback.failed.length > 0 &&
            <span className="text-destructive"> · 실패 {run.rollback.failed.join(' ; ')}</span>}
        </div>
      )}

      {run.phases.map(ph => (
        <div className="mb-1.5" key={ph.key}>
          <div className="text-sm font-semibold">
            {ph.key} · {ph.title}
            <span className="ml-2 font-normal text-muted-foreground">{ph.status}</span>
          </div>
          <div className="pl-3">
            {ph.steps.map((s, i) => (
              <div className="text-sm py-px px-0" key={i}>
                <span style={{ color: STEP_COLOR[s.status], fontWeight: 700, marginRight: 6 }}>
                  {STEP_ICON[s.status] || '·'}
                </span>
                <span className="inline-block min-w-[150px]">{s.target}</span>
                <span style={{ color: s.status === 'failed' ? 'var(--destructive)' : 'var(--muted-foreground)' }}>
                  {s.detail || s.error || ''}
                </span>
                {s.elapsed_sec ? <span className="text-muted-foreground"> ({s.elapsed_sec}s)</span> : null}
              </div>
            ))}
          </div>
        </div>
      ))}
    </section>
  )
}
