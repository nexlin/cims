import { useState, useEffect, useCallback } from 'react'
import {
  cspRuntimeApi,
  type RouteRule, type RouteRuleInput, type MatchCond, type TransformStep,
  type MatchOp, type TransformAction, type TargetMode, type FailAction,
  type SipTrunk, type RouteDryRunSample, type RouteDryRunResult,
} from '../api/cspRuntime'
import { useToast } from '../components/Toast'

type Mode = 'form' | 'json'

const MATCH_OPS: MatchOp[] = ['equals', 'not_equals', 'prefix', 'suffix', 'contains', 'regex', 'cidr']
const COMMON_FIELDS = [
  'req_uri_user', 'req_uri_host', 'from_uri', 'to_uri',
  'method', 'source_ip', 'source_trunk',
]
const TRANSFORM_ACTIONS: TransformAction[] = [
  'set_req_uri_user', 'set_req_uri_host', 'set_from_host',
  'add_header', 'remove_header', 'replace_header',
  'strip_prefix', 'add_prefix',
  'set_transport', 'set_privacy', 'anonymize_from',
]
const TARGET_MODES: TargetMode[] = ['trunk', 'priority_list', 'round_robin', 'weighted', 'reject']
const FAIL_ACTIONS: FailAction[] = ['reject', 'fallback', 'next_rule']

const EMPTY_RULE: RouteRuleInput = {
  name: '',
  enabled: true,
  priority: 100,
  description: '',
  match: [],
  transform: [],
  target: { mode: 'trunk', trunk_id: null },
  fail: { action: 'reject', code: 404, reason: 'Not Found', fallback: null, timeout_ms: 4000, retry_count: 0 },
}

export default function SipRoutesPage() {
  const { show } = useToast()
  const [items, setItems] = useState<RouteRule[]>([])
  const [trunks, setTrunks] = useState<SipTrunk[]>([])
  const [loading, setLoading] = useState(true)

  const [editOpen, setEditOpen] = useState(false)
  const [editId, setEditId] = useState<number | null>(null)
  const [mode, setMode] = useState<Mode>('form')
  const [form, setForm] = useState<RouteRuleInput>(EMPTY_RULE)
  const [jsonText, setJsonText] = useState('')
  const [jsonErr, setJsonErr] = useState('')

  const [dryOpen, setDryOpen] = useState(false)
  const [dryInput, setDryInput] = useState<string>(JSON.stringify({
    method: 'INVITE', req_uri_user: '91234567', req_uri_host: 'example.com',
  }, null, 2))
  const [dryResult, setDryResult] = useState<RouteDryRunResult | null>(null)
  const [dryErr, setDryErr] = useState('')

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [rules, ts] = await Promise.all([
        cspRuntimeApi.listRoutes(),
        cspRuntimeApi.listTrunks(),
      ])
      setItems(rules)
      setTrunks(ts)
    } catch (e) {
      show(`조회 실패: ${(e as Error).message}`, 'err')
    } finally {
      setLoading(false)
    }
  }, [show])

  useEffect(() => { void load() }, [load])

  function openCreate() {
    setEditId(null); setMode('form')
    setForm(JSON.parse(JSON.stringify(EMPTY_RULE)))
    setJsonText(JSON.stringify(EMPTY_RULE, null, 2))
    setJsonErr(''); setEditOpen(true)
  }

  function openEdit(row: RouteRule) {
    setEditId(row.id); setMode('form')
    const f: RouteRuleInput = {
      name: row.name,
      enabled: row.enabled,
      priority: row.priority,
      description: row.description ?? '',
      match: row.match,
      transform: row.transform,
      target: { mode: row.target.mode, trunk_id: row.target.trunk_id },
      fail: { ...row.fail },
    }
    setForm(f)
    setJsonText(JSON.stringify(f, null, 2))
    setJsonErr(''); setEditOpen(true)
  }

  function toggleMode(newMode: Mode) {
    if (newMode === mode) return
    if (newMode === 'json') {
      setJsonText(JSON.stringify(form, null, 2))
      setJsonErr('')
    } else {
      try {
        const parsed = JSON.parse(jsonText) as RouteRuleInput
        setForm({ ...EMPTY_RULE, ...parsed })
        setJsonErr('')
      } catch (e) { setJsonErr((e as Error).message); return }
    }
    setMode(newMode)
  }

  async function save() {
    let body: RouteRuleInput
    if (mode === 'json') {
      try { body = JSON.parse(jsonText) as RouteRuleInput }
      catch (e) { setJsonErr((e as Error).message); return }
    } else {
      body = { ...form }
    }
    try {
      if (editId == null) {
        await cspRuntimeApi.createRoute(body)
        show('규칙 추가됨 (즉시 적용)', 'ok')
      } else {
        await cspRuntimeApi.updateRoute(editId, body)
        show('규칙 수정됨 (즉시 적용)', 'ok')
      }
      setEditOpen(false); await load()
    } catch (e) { show((e as Error).message, 'err') }
  }

  async function remove(row: RouteRule) {
    if (!confirm(`규칙 "${row.name}" (priority=${row.priority}) 을 삭제할까요?`)) return
    try {
      await cspRuntimeApi.deleteRoute(row.id)
      show('규칙 삭제됨', 'ok'); await load()
    } catch (e) { show((e as Error).message, 'err') }
  }

  async function runDryRun() {
    setDryErr(''); setDryResult(null)
    let sample: RouteDryRunSample
    try { sample = JSON.parse(dryInput) as RouteDryRunSample }
    catch (e) { setDryErr((e as Error).message); return }
    try {
      const r = await cspRuntimeApi.dryrunRoute(sample)
      setDryResult(r)
    } catch (e) { setDryErr((e as Error).message) }
  }

  // ── match/transform row editors ─────────────────────────────
  function updateMatch(idx: number, patch: Partial<MatchCond>) {
    setForm(f => ({ ...f, match: f.match!.map((m, i) => i === idx ? { ...m, ...patch } : m) }))
  }
  function addMatch() {
    setForm(f => ({ ...f, match: [...(f.match ?? []), { field: 'req_uri_user', op: 'equals', value: '' }] }))
  }
  function removeMatch(idx: number) {
    setForm(f => ({ ...f, match: f.match!.filter((_, i) => i !== idx) }))
  }
  function updateTransform(idx: number, patch: Partial<TransformStep>) {
    setForm(f => ({ ...f, transform: f.transform!.map((t, i) => i === idx ? { ...t, ...patch } : t) }))
  }
  function addTransform() {
    setForm(f => ({ ...f, transform: [...(f.transform ?? []), { action: 'set_req_uri_user', value: '' }] }))
  }
  function removeTransform(idx: number) {
    setForm(f => ({ ...f, transform: f.transform!.filter((_, i) => i !== idx) }))
  }

  return (
    <div>
      <div style={{ marginBottom: 16, display: 'flex', gap: 12, alignItems: 'center' }}>
        <h3 style={{ margin: 0 }}>SIP 라우팅 규칙</h3>
        <span className="text-muted" style={{ fontSize: 13 }}>
          수신한 SIP 메시지를 조건별로 변환 후 타겟 트렁크로 라우팅. 우선순위 낮을수록 먼저 평가, first-match-wins.
        </span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 8 }}>
          <button className="btn btn--outline" onClick={() => setDryOpen(true)}>🧪 Dry-run</button>
          <button className="btn btn--outline" onClick={() => void load()}>↻</button>
          <button className="btn btn--primary" onClick={openCreate}>＋ 규칙 추가</button>
        </div>
      </div>

      {loading ? (
        <div className="empty">로딩 중...</div>
      ) : items.length === 0 ? (
        <div className="empty">등록된 규칙 없음</div>
      ) : (
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 40 }}>ID</th>
              <th style={{ width: 70 }}>Priority</th>
              <th>이름</th>
              <th>Match</th>
              <th>Target</th>
              <th style={{ width: 80 }}>Hits</th>
              <th style={{ width: 60 }}>활성</th>
              <th style={{ width: 140 }}>작업</th>
            </tr>
          </thead>
          <tbody>
            {items.map(r => (
              <tr key={r.id}>
                <td>{r.id}</td>
                <td>{r.priority}</td>
                <td>{r.name}</td>
                <td>
                  {r.match.length === 0 ? '—' : r.match.map((m, i) => (
                    <div key={i} style={{ fontSize: 12, fontFamily: 'monospace' }}>
                      {m.invert && '!'}{m.field} {m.op} "{m.value}"
                    </div>
                  ))}
                </td>
                <td>
                  {r.target.mode === 'reject'
                    ? <span className="tag" style={{ background: '#e74c3c', color: '#fff' }}>reject {r.fail.code}</span>
                    : r.target.trunk_id
                      ? <span className="tag">trunk #{r.target.trunk_id}</span>
                      : <span className="tag">{r.target.mode}</span>}
                </td>
                <td>{r.hit_count}</td>
                <td>{r.enabled ? '✓' : '×'}</td>
                <td>
                  <button className="btn btn--sm" onClick={() => openEdit(r)}>수정</button>{' '}
                  <button className="btn btn--sm btn--danger" onClick={() => remove(r)}>삭제</button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}

      {editOpen && (
        <div className="modal-overlay" onClick={() => setEditOpen(false)}>
          <div className="modal-box" style={{ minWidth: 720, maxHeight: '85vh', overflow: 'auto' }}
               onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">{editId == null ? '라우팅 규칙 추가' : '라우팅 규칙 수정'}</span>
              <button className="modal-close" onClick={() => setEditOpen(false)}>✕</button>
            </div>
            <div className="modal-body">
              <div style={{ marginBottom: 12, display: 'flex', gap: 8 }}>
                <button className={`btn btn--sm ${mode === 'form' ? 'btn--primary' : 'btn--outline'}`}
                        onClick={() => toggleMode('form')}>폼</button>
                <button className={`btn btn--sm ${mode === 'json' ? 'btn--primary' : 'btn--outline'}`}
                        onClick={() => toggleMode('json')}>JSON</button>
              </div>

              {mode === 'form' ? (
                <div>
                  <div className="form-grid">
                    <label>이름</label>
                    <input className="form-input" value={form.name}
                      onChange={e => setForm(f => ({ ...f, name: e.target.value }))} />
                    <label>우선순위</label>
                    <input className="form-input" type="number" value={form.priority ?? 100}
                      onChange={e => setForm(f => ({ ...f, priority: parseInt(e.target.value || '0', 10) }))} />
                    <label>활성</label>
                    <input type="checkbox" checked={form.enabled ?? true}
                      onChange={e => setForm(f => ({ ...f, enabled: e.target.checked }))} />
                    <label>설명</label>
                    <input className="form-input" value={form.description ?? ''}
                      onChange={e => setForm(f => ({ ...f, description: e.target.value }))} />
                  </div>

                  <h4 style={{ marginTop: 20, marginBottom: 8 }}>매칭 조건 (AND)</h4>
                  {(form.match ?? []).map((m, i) => (
                    <div key={i} style={{ display: 'flex', gap: 6, marginBottom: 6 }}>
                      <input className="form-input" style={{ flex: 1 }} placeholder="field (예: req_uri_user 또는 header:P-Asserted-Identity)"
                        list={`fields-${i}`} value={m.field}
                        onChange={e => updateMatch(i, { field: e.target.value })} />
                      <datalist id={`fields-${i}`}>
                        {COMMON_FIELDS.map(f => <option key={f} value={f} />)}
                      </datalist>
                      <select className="form-input" style={{ width: 100 }} value={m.op}
                        onChange={e => updateMatch(i, { op: e.target.value as MatchOp })}>
                        {MATCH_OPS.map(o => <option key={o}>{o}</option>)}
                      </select>
                      <input className="form-input" style={{ flex: 1 }} placeholder="value"
                        value={m.value}
                        onChange={e => updateMatch(i, { value: e.target.value })} />
                      <label style={{ display: 'flex', alignItems: 'center', fontSize: 12 }}>
                        <input type="checkbox" checked={m.invert ?? false}
                          onChange={e => updateMatch(i, { invert: e.target.checked })} /> invert
                      </label>
                      <button className="btn btn--sm btn--danger" onClick={() => removeMatch(i)}>✕</button>
                    </div>
                  ))}
                  <button className="btn btn--sm btn--outline" onClick={addMatch}>＋ 조건 추가</button>

                  <h4 style={{ marginTop: 20, marginBottom: 8 }}>변환 (순서대로 적용)</h4>
                  {(form.transform ?? []).map((t, i) => (
                    <div key={i} style={{ display: 'flex', gap: 6, marginBottom: 6 }}>
                      <select className="form-input" style={{ width: 180 }} value={t.action}
                        onChange={e => updateTransform(i, { action: e.target.value as TransformAction })}>
                        {TRANSFORM_ACTIONS.map(a => <option key={a}>{a}</option>)}
                      </select>
                      <input className="form-input" style={{ flex: 1 }} placeholder="target (헤더 이름 등)"
                        value={t.target ?? ''}
                        onChange={e => updateTransform(i, { target: e.target.value })} />
                      <input className="form-input" style={{ flex: 1 }} placeholder="value"
                        value={t.value ?? ''}
                        onChange={e => updateTransform(i, { value: e.target.value })} />
                      <button className="btn btn--sm btn--danger" onClick={() => removeTransform(i)}>✕</button>
                    </div>
                  ))}
                  <button className="btn btn--sm btn--outline" onClick={addTransform}>＋ 변환 추가</button>

                  <h4 style={{ marginTop: 20, marginBottom: 8 }}>타겟</h4>
                  <div className="form-grid">
                    <label>Mode</label>
                    <select className="form-input" value={form.target?.mode ?? 'trunk'}
                      onChange={e => setForm(f => ({ ...f, target: { ...f.target!, mode: e.target.value as TargetMode } }))}>
                      {TARGET_MODES.map(m => <option key={m}>{m}</option>)}
                    </select>
                    {form.target?.mode === 'trunk' && (
                      <>
                        <label>Trunk</label>
                        <select className="form-input" value={form.target?.trunk_id ?? ''}
                          onChange={e => setForm(f => ({
                            ...f, target: { ...f.target!, trunk_id: e.target.value ? parseInt(e.target.value, 10) : null },
                          }))}>
                          <option value="">— 선택 —</option>
                          {trunks.map(t => <option key={t.id} value={t.id}>{t.name} ({t.remote_ip}:{t.remote_port})</option>)}
                        </select>
                      </>
                    )}
                  </div>

                  <h4 style={{ marginTop: 20, marginBottom: 8 }}>실패 처리</h4>
                  <div className="form-grid">
                    <label>Action</label>
                    <select className="form-input" value={form.fail?.action ?? 'reject'}
                      onChange={e => setForm(f => ({ ...f, fail: { ...f.fail!, action: e.target.value as FailAction } }))}>
                      {FAIL_ACTIONS.map(a => <option key={a}>{a}</option>)}
                    </select>
                    <label>Code</label>
                    <input className="form-input" type="number" value={form.fail?.code ?? 404}
                      onChange={e => setForm(f => ({ ...f, fail: { ...f.fail!, code: parseInt(e.target.value || '0', 10) } }))} />
                    <label>Reason</label>
                    <input className="form-input" value={form.fail?.reason ?? ''}
                      onChange={e => setForm(f => ({ ...f, fail: { ...f.fail!, reason: e.target.value } }))} />
                    <label>Timeout(ms)</label>
                    <input className="form-input" type="number" value={form.fail?.timeout_ms ?? 4000}
                      onChange={e => setForm(f => ({ ...f, fail: { ...f.fail!, timeout_ms: parseInt(e.target.value || '0', 10) } }))} />
                  </div>
                </div>
              ) : (
                <div>
                  <textarea className="form-input" style={{ width: '100%', minHeight: 400, fontFamily: 'monospace' }}
                    value={jsonText} onChange={e => setJsonText(e.target.value)} />
                  {jsonErr && <div className="auth-error" style={{ marginTop: 8 }}>JSON 오류: {jsonErr}</div>}
                </div>
              )}
            </div>
            <div className="modal-footer">
              <button className="btn btn--outline" onClick={() => setEditOpen(false)}>취소</button>
              <button className="btn btn--primary" onClick={save}>저장</button>
            </div>
          </div>
        </div>
      )}

      {dryOpen && (
        <div className="modal-overlay" onClick={() => setDryOpen(false)}>
          <div className="modal-box" style={{ minWidth: 600 }} onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">🧪 Dry-run (규칙 매칭 시뮬레이션)</span>
              <button className="modal-close" onClick={() => setDryOpen(false)}>✕</button>
            </div>
            <div className="modal-body">
              <div style={{ fontSize: 13, marginBottom: 8, color: '#666' }}>
                샘플 SIP 메시지 필드를 JSON 으로 입력하고 평가. 실제 규칙 집합(DB)으로 평가됩니다.
              </div>
              <textarea className="form-input" style={{ width: '100%', minHeight: 180, fontFamily: 'monospace' }}
                value={dryInput} onChange={e => setDryInput(e.target.value)} />
              {dryErr && <div className="auth-error" style={{ marginTop: 8 }}>오류: {dryErr}</div>}
              {dryResult && (
                <div style={{ marginTop: 12, padding: 12, background: '#f5f5f5', borderRadius: 4 }}>
                  {dryResult.matched ? (
                    <div>
                      <div style={{ color: '#27ae60', fontWeight: 'bold' }}>
                        ✓ matched rule #{dryResult.rule_id} "{dryResult.rule_name}"
                      </div>
                      <pre style={{ fontSize: 12 }}>{JSON.stringify(dryResult, null, 2)}</pre>
                    </div>
                  ) : (
                    <div style={{ color: '#999' }}>매칭 규칙 없음 (기본 처리)</div>
                  )}
                </div>
              )}
            </div>
            <div className="modal-footer">
              <button className="btn btn--outline" onClick={() => setDryOpen(false)}>닫기</button>
              <button className="btn btn--primary" onClick={runDryRun}>평가</button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
