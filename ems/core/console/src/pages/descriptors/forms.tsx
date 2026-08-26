// 서비스 정의 / 데이터 소스 폼 입력 화면 — JSON 직접 편집 대신 구조화 폼.
// 복잡한 map 매핑은 "고급(JSON)" 토글로 fallback 제공.
import { useState } from 'react'
import Modal from '../../components/Modal'
import { useToast } from '../../components/Toast'
import {
  serviceDescriptorsApi,
  type ServiceDescriptor, type ServiceModule, type AlertRule,
} from '../../api/serviceDescriptors'
import type { DataSourceSpec } from '../../widgets/shapes/dataSourceSpec'

// ── 공용 입력 조각 ──────────────────────────────────────────────
function Field({ label, hint, children }: { label: string; hint?: string; children: React.ReactNode }) {
  return (
    <label style={{ display: 'flex', flexDirection: 'column', gap: 3, fontSize: 12 }}>
      <span style={{ color: 'var(--text-muted)' }}>{label}{hint && <i style={{ marginLeft: 6, opacity: 0.7 }}>{hint}</i>}</span>
      {children}
    </label>
  )
}
const inp: React.CSSProperties = { fontSize: 13 }
const rowCard: React.CSSProperties = {
  border: '1px solid var(--border)', borderRadius: 'var(--radius)', padding: 10, marginBottom: 8, background: 'var(--bg-soft)',
}
function Btn({ onClick, children, danger, disabled }: { onClick: () => void; children: React.ReactNode; danger?: boolean; disabled?: boolean }) {
  return <button type="button" className="btn btn--sm btn--outline" disabled={disabled}
    style={danger ? { color: 'var(--danger)' } : undefined} onClick={onClick}>{children}</button>
}

const SHAPES = ['time-bar', 'kpi', 'distribution', 'table'] as const
const SHAPE_LABEL: Record<string, string> = { 'time-bar': '시계열 차트', kpi: 'KPI', distribution: '분포', table: '표' }
// 구 probe check(process_down/service_unresponsive)는 process_unresponsive 로 개정 —
// 서버가 read 시 이행(check 개정).
const CHECKS = ['process_unresponsive', 'db_down', 'rtp_pct_gte', 'disk_high', 'module_down']
// 알람 표준화(X.733/32.111)
const ALARM_CLASSES = ['process_down', 'process_unresponsive', 'connection_lost', 'threshold_crossed']
const SEVERITIES = ['critical', 'major', 'minor', 'warning', 'indeterminate']
const EVENT_TYPES = ['processingError', 'communications', 'qualityOfService', 'equipment', 'environmental']
const MO_CLASSES = ['software', 'service', 'host', 'equipment', 'network']

// ════════════════════════════════════════════════════════════════
//  서비스 정의 폼 — **id/label 만**. 모듈·알람 규칙·데이터 소스는 각각의 위젯에서
//  항목 단위로 추가/편집/삭제한다(편집 경로 일원화).
// ════════════════════════════════════════════════════════════════
export function ServiceForm({ initial, onClose, onSaved }: {
  initial: ServiceDescriptor | null; onClose: () => void; onSaved: () => void
}) {
  const { show } = useToast()
  const isNew = !initial
  const [id, setId] = useState(initial?.id || '')
  const [label, setLabel] = useState(initial?.label || '')
  const [saving, setSaving] = useState(false)

  const save = async () => {
    if (!id.trim()) { show('id 가 필요합니다', 'err'); return }
    const doc: ServiceDescriptor = {
      ...(initial ?? { modules: [] as ServiceModule[] }),
      id: id.trim(), label: label.trim() || id.trim(),
    }
    setSaving(true)
    try { await serviceDescriptorsApi.put(doc.id, doc); show('서비스 정의 저장됨', 'ok'); onSaved(); onClose() }
    catch (e) { show((e as Error).message, 'err') }
    finally { setSaving(false) }
  }

  return (
    <Modal title={isNew ? '새 서비스 정의' : `서비스 정의 — ${initial!.id}`} onClose={onClose} width={440}>
      <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        <Field label="서비스 id" hint="(예: cims — 만든 뒤 바꿀 수 없음)">
          <input className="form-input" style={inp} value={id} disabled={!isNew}
                 onChange={e => setId(e.target.value)} placeholder="myservice" />
        </Field>
        <Field label="표시명">
          <input className="form-input" style={inp} value={label}
                 onChange={e => setLabel(e.target.value)} placeholder="My Service" />
        </Field>
        <div style={{ fontSize: 11, color: 'var(--text-muted)' }}>
          모듈 · 알람 규칙 · 데이터 소스는 각 위젯에서 항목별로 추가/편집합니다.
        </div>
      </div>
      <div className="modal-footer">
        <Btn onClick={onClose} disabled={saving}>취소</Btn>
        <button className="btn btn--primary" onClick={save} disabled={saving}>저장</button>
      </div>
    </Modal>
  )
}

// 서비스 문서에서 배열 하나만 갈아끼워 저장 — 항목 단위 폼들의 공통 저장 경로.
async function putWith(svc: ServiceDescriptor, patch: Partial<ServiceDescriptor>) {
  await serviceDescriptorsApi.put(svc.id, { ...svc, ...patch })
}

// ════════════════════════════════════════════════════════════════
//  모듈 1건 폼
// ════════════════════════════════════════════════════════════════
export function ModuleForm({ svc, index, onClose, onSaved }: {
  svc: ServiceDescriptor; index: number | null; onClose: () => void; onSaved: () => void
}) {
  const { show } = useToast()
  const cur = index != null ? svc.modules[index] : null
  const [m, setM] = useState<ServiceModule>(cur ? structuredClone(cur) : { name: '', proto: 'tcp' })
  const [saving, setSaving] = useState(false)
  const up = (p: Partial<ServiceModule>) => setM(v => ({ ...v, ...p }))

  const save = async () => {
    if (!m.name?.trim()) { show('모듈 이름은 필수입니다', 'err'); return }
    const item: ServiceModule = {
      name: m.name.trim(),
      ...(m.port ? { port: Number(m.port) } : {}),
      ...(m.proto ? { proto: m.proto } : {}),
      ...(m.controllable ? { controllable: true } : {}),
    }
    const modules = index != null
      ? svc.modules.map((x, i) => (i === index ? item : x))
      : [...svc.modules, item]
    setSaving(true)
    try { await putWith(svc, { modules }); show('모듈 저장됨', 'ok'); onSaved(); onClose() }
    catch (e) { show((e as Error).message, 'err') }
    finally { setSaving(false) }
  }

  return (
    <Modal title={index == null ? `모듈 추가 — ${svc.id}` : `모듈 편집 — ${cur?.name}`} onClose={onClose} width={460}>
      <div style={{ display: 'flex', gap: 10, flexWrap: 'wrap', alignItems: 'flex-end' }}>
        <Field label="이름"><input className="form-input" style={{ ...inp, width: 140 }} value={m.name}
          onChange={e => up({ name: e.target.value })} /></Field>
        <Field label="포트"><input className="form-input" style={{ ...inp, width: 90 }} type="number" value={m.port ?? ''}
          onChange={e => up({ port: e.target.value ? Number(e.target.value) : undefined })} /></Field>
        <Field label="proto"><select className="form-input" style={{ ...inp, width: 80 }} value={m.proto ?? ''}
          onChange={e => up({ proto: e.target.value || undefined })}>
          <option value="">—</option><option value="tcp">tcp</option><option value="udp">udp</option></select></Field>
        <label style={{ fontSize: 12, display: 'flex', alignItems: 'center', gap: 4, paddingBottom: 6 }}>
          <input type="checkbox" checked={!!m.controllable} onChange={e => up({ controllable: e.target.checked })} />제어
        </label>
      </div>
      <div className="modal-footer">
        <Btn onClick={onClose} disabled={saving}>취소</Btn>
        <button className="btn btn--primary" onClick={save} disabled={saving}>저장</button>
      </div>
    </Modal>
  )
}

// ════════════════════════════════════════════════════════════════
//  알람 규칙 1건 폼
// ════════════════════════════════════════════════════════════════
export function AlertRuleForm({ svc, index, onClose, onSaved }: {
  svc: ServiceDescriptor; index: number | null; onClose: () => void; onSaved: () => void
}) {
  const { show } = useToast()
  const list = svc.alert_rules || []
  const cur = index != null ? list[index] : null
  const [r, setR] = useState<AlertRule>(cur ? structuredClone(cur) : {
    type: 'process_unresponsive', code: 'A-PRC-004', perceived_severity: 'major',
    event_type: 'processingError', mo_class: 'service', check: 'process_unresponsive',
  })
  const [saving, setSaving] = useState(false)
  const up = (p: Partial<AlertRule>) => setR(v => ({ ...v, ...p }))

  const save = async () => {
    const rules = index != null ? list.map((x, i) => (i === index ? r : x)) : [...list, r]
    setSaving(true)
    try { await putWith(svc, { alert_rules: rules }); show('알람 규칙 저장됨', 'ok'); onSaved(); onClose() }
    catch (e) { show((e as Error).message, 'err') }
    finally { setSaving(false) }
  }

  return (
    <Modal title={index == null ? `알람 규칙 추가 — ${svc.id}` : `알람 규칙 편집 — ${cur?.code || cur?.type}`}
           onClose={onClose} width={720}>
      <div style={{ display: 'flex', flexDirection: 'column', gap: 8, maxHeight: '65vh', overflowY: 'auto' }}>
              {/* 1행: 클래스 / 코드 / 심각도 / check */}
              <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', alignItems: 'flex-end' }}>
                <Field label="클래스(type)"><select className="form-input" style={{ ...inp, width: 130 }} value={r.type}
                  onChange={e => up({ type: e.target.value })}>
                  {ALARM_CLASSES.map(c => <option key={c} value={c}>{c}</option>)}</select></Field>
                <Field label="code"><input className="form-input" style={{ ...inp, width: 110 }} value={r.code ?? ''}
                  onChange={e => up({ code: e.target.value })} placeholder="A-PRC-001" /></Field>
                <Field label="심각도"><select className="form-input" style={{ ...inp, width: 100 }} value={r.perceived_severity ?? r.severity ?? 'warning'}
                  onChange={e => up({ perceived_severity: e.target.value })}>
                  {SEVERITIES.map(s => <option key={s} value={s}>{s}</option>)}</select></Field>
                <Field label="check"><select className="form-input" style={{ ...inp, width: 120 }} value={r.check ?? ''}
                  onChange={e => up({ check: e.target.value })}>
                  {CHECKS.map(c => <option key={c} value={c}>{c}</option>)}</select></Field>
              </div>
              {/* 2행: event_type / probable_cause / mo_class / mo_instance / 조건부 target·threshold */}
              <div style={{ display: 'flex', gap: 8, marginTop: 6, flexWrap: 'wrap', alignItems: 'flex-end' }}>
                <Field label="event_type"><select className="form-input" style={{ ...inp, width: 140 }} value={r.event_type ?? 'processingError'}
                  onChange={e => up({ event_type: e.target.value })}>
                  {EVENT_TYPES.map(t => <option key={t} value={t}>{t}</option>)}</select></Field>
                <Field label="probable_cause"><input className="form-input" style={{ ...inp, width: 160 }} value={r.probable_cause ?? ''}
                  onChange={e => up({ probable_cause: e.target.value })} placeholder="softwareError" /></Field>
                <Field label="mo_class"><select className="form-input" style={{ ...inp, width: 100 }} value={r.mo_class ?? 'service'}
                  onChange={e => up({ mo_class: e.target.value })}>
                  {MO_CLASSES.map(m => <option key={m} value={m}>{m}</option>)}</select></Field>
                <Field label="mo_instance" hint="(소스, service)"><input className="form-input" style={{ ...inp, width: 120 }} value={r.mo_instance ?? ''}
                  onChange={e => up({ mo_instance: e.target.value })} placeholder="비우면 관측 신원으로 합성" /></Field>
                {(r.check === 'process_unresponsive' || r.check === 'service_unresponsive' || r.check === 'process_down') && (
                  <Field label="target" hint="(모듈명)"><input className="form-input" style={{ ...inp, width: 80 }} value={r.target ?? ''}
                    onChange={e => up({ target: e.target.value })} /></Field>)}
                {(r.check === 'rtp_pct_gte' || r.check === 'disk_high') && (
                  <Field label="threshold"><input className="form-input" style={{ ...inp, width: 80 }} type="number" value={r.threshold ?? ''}
                    onChange={e => up({ threshold: e.target.value ? Number(e.target.value) : undefined })} /></Field>)}
              </div>
              {/* 3행: metric / 메시지 */}
              <div style={{ display: 'flex', gap: 8, marginTop: 6, flexWrap: 'wrap' }}>
                <Field label="metric(표시명)"><input className="form-input" style={{ ...inp, width: 120 }} value={r.metric ?? ''}
                  onChange={e => up({ metric: e.target.value })} /></Field>
                <Field label="발생 메시지" hint="({mo} 치환)"><input className="form-input" style={{ ...inp, width: 200 }} value={r.msg_open ?? ''}
                  onChange={e => up({ msg_open: e.target.value })} /></Field>
                <Field label="해제 메시지"><input className="form-input" style={{ ...inp, width: 160 }} value={r.msg_close ?? ''}
                  onChange={e => up({ msg_close: e.target.value })} /></Field>
              </div>
              {/* 4행: effect / recommended_action (운영 runbook) */}
              <div style={{ display: 'flex', gap: 8, marginTop: 6, flexWrap: 'wrap' }}>
                <Field label="영향(effect)"><input className="form-input" style={{ ...inp, width: 240 }} value={r.effect ?? ''}
                  onChange={e => up({ effect: e.target.value })} /></Field>
                <Field label="권장 조치(action)"><input className="form-input" style={{ ...inp, width: 240 }} value={r.recommended_action ?? ''}
                  onChange={e => up({ recommended_action: e.target.value })} /></Field>
              </div>
      </div>
      <div className="modal-footer">
        <Btn onClick={onClose} disabled={saving}>취소</Btn>
        <button className="btn btn--primary" onClick={save} disabled={saving}>저장</button>
      </div>
    </Modal>
  )
}

// ════════════════════════════════════════════════════════════════
//  데이터 소스 폼
// ════════════════════════════════════════════════════════════════
interface KpiItem { label: string; path: string; unit?: string; format?: string }

export function DataSourceForm({ svc, index, onClose, onSaved }: {
  svc: ServiceDescriptor; index: number | null; onClose: () => void; onSaved: () => void
}) {
  const { show } = useToast()
  const existing = index != null ? (svc.data_sources || [])[index] : null
  const m = (existing?.map || {}) as Record<string, Record<string, unknown>>

  const [id, setId] = useState(existing?.id || `${svc.id}.source`)
  const [label, setLabel] = useState(existing?.label || '')
  const [endpoint, setEndpoint] = useState(existing?.endpoint || '')
  const [qDate, setQDate] = useState((existing?.query || []).includes('date'))
  const [qGran, setQGran] = useState((existing?.query || []).includes('granularity'))
  const [shapes, setShapes] = useState<Set<string>>(new Set(existing?.shapes || ['time-bar']))
  const [saving, setSaving] = useState(false)

  // time-bar
  const tb = (m['time-bar'] || {}) as Record<string, unknown>
  const [tbFrom, setTbFrom] = useState(String(tb.from || ''))
  const [tbLabel, setTbLabel] = useState((tb.label as string[] | undefined)?.join(', ') || '')
  const [tbValue, setTbValue] = useState(String(tb.value || ''))
  // kpi
  const [kpiItems, setKpiItems] = useState<KpiItem[]>(((m.kpi?.items as KpiItem[]) || []))
  // distribution
  const dist = (m.distribution || {}) as Record<string, unknown>
  const [distObj, setDistObj] = useState(String(dist.fromObject || ''))
  const [distTotal, setDistTotal] = useState(String(dist.totalPath || ''))
  // table
  const tbl = (m.table || {}) as Record<string, unknown>
  const [tblObj, setTblObj] = useState(String(tbl.fromObject || ''))
  const [tblCols, setTblCols] = useState(((tbl.columns as string[] | undefined) || ['', '']).join(', '))

  const toggleShape = (s: string) => setShapes(prev => {
    const n = new Set(prev); if (n.has(s)) n.delete(s); else n.add(s); return n
  })
  const upKpi = (i: number, p: Partial<KpiItem>) => setKpiItems(it => it.map((x, k) => k === i ? { ...x, ...p } : x))

  const save = async () => {
    if (!id.trim()) { show('id 가 필요합니다', 'err'); return }
    if (!endpoint.trim()) { show('endpoint 가 필요합니다', 'err'); return }
    if (shapes.size === 0) { show('shape 를 1개 이상 선택하세요', 'err'); return }
    const map: Record<string, unknown> = {}
    if (shapes.has('time-bar')) map['time-bar'] = { from: tbFrom, label: tbLabel.split(',').map(s => s.trim()).filter(Boolean), value: tbValue }
    if (shapes.has('kpi')) map.kpi = { items: kpiItems.map(k => ({ label: k.label, path: k.path, ...(k.unit ? { unit: k.unit } : {}), ...(k.format ? { format: k.format } : {}) })) }
    if (shapes.has('distribution')) map.distribution = { fromObject: distObj, totalPath: distTotal }
    if (shapes.has('table')) map.table = { fromObject: tblObj, columns: tblCols.split(',').map(s => s.trim()).slice(0, 2) }
    const query: string[] = []; if (qDate) query.push('date'); if (qGran) query.push('granularity')
    const spec: DataSourceSpec = {
      id: id.trim(), label: label.trim() || id.trim(), endpoint: endpoint.trim(),
      shapes: SHAPES.filter(s => shapes.has(s)),
      ...(query.length ? { query } : {}),
      map: map as DataSourceSpec['map'],
    }
    const sources = [...(svc.data_sources || [])]
    const dupAt = sources.findIndex(s => s.id === spec.id)
    if (dupAt >= 0 && dupAt !== index) { show(`id 중복: ${spec.id}`, 'err'); return }
    if (index != null) sources[index] = spec; else sources.push(spec)
    setSaving(true)
    try { await serviceDescriptorsApi.put(svc.id, { ...svc, data_sources: sources }); show('데이터 소스 저장됨', 'ok'); onSaved(); onClose() }
    catch (e) { show((e as Error).message, 'err') }
    finally { setSaving(false) }
  }

  return (
    <Modal title={index != null ? `데이터 소스 편집 — ${existing?.id}` : '데이터 소스 추가'} onClose={onClose} width={680}>
      <div style={{ display: 'flex', flexDirection: 'column', gap: 14, maxHeight: '70vh', overflowY: 'auto' }}>
        <div style={{ display: 'flex', gap: 12, flexWrap: 'wrap' }}>
          <Field label="id"><input className="form-input" style={{ ...inp, width: 180 }} value={id} onChange={e => setId(e.target.value)} /></Field>
          <Field label="표시명"><input className="form-input" style={{ ...inp, width: 160 }} value={label} onChange={e => setLabel(e.target.value)} /></Field>
        </div>
        <div style={{ display: 'flex', gap: 12, alignItems: 'flex-end', flexWrap: 'wrap' }}>
          <Field label="endpoint" hint="(REST 경로)"><input className="form-input" style={{ ...inp, width: 300 }} value={endpoint}
            onChange={e => setEndpoint(e.target.value)} placeholder="/stats/messages/sip" /></Field>
          <div style={{ display: 'flex', gap: 10, paddingBottom: 6, fontSize: 12 }}>
            <span style={{ color: 'var(--text-muted)' }}>query:</span>
            <label style={{ display: 'flex', gap: 4 }}><input type="checkbox" checked={qDate} onChange={e => setQDate(e.target.checked)} />date</label>
            <label style={{ display: 'flex', gap: 4 }}><input type="checkbox" checked={qGran} onChange={e => setQGran(e.target.checked)} />granularity</label>
          </div>
        </div>

        <div>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 4 }}>지원 shape (선택한 것만 매핑 입력)</div>
          <div style={{ display: 'flex', gap: 14, fontSize: 13 }}>
            {SHAPES.map(s => (
              <label key={s} style={{ display: 'flex', gap: 4, alignItems: 'center' }}>
                <input type="checkbox" checked={shapes.has(s)} onChange={() => toggleShape(s)} />{SHAPE_LABEL[s]}
              </label>
            ))}
          </div>
        </div>

        {/* shape별 매핑 */}
        {shapes.has('time-bar') && (
          <div style={rowCard}>
            <b style={{ fontSize: 12 }}>시계열 차트 매핑</b>
            <div style={{ display: 'flex', gap: 8, marginTop: 6, flexWrap: 'wrap' }}>
              <Field label="from" hint="(배열 경로)"><input className="form-input" style={{ ...inp, width: 160 }} value={tbFrom} onChange={e => setTbFrom(e.target.value)} placeholder="buckets / voip.buckets" /></Field>
              <Field label="label" hint="(필드 후보, 쉼표)"><input className="form-input" style={{ ...inp, width: 130 }} value={tbLabel} onChange={e => setTbLabel(e.target.value)} placeholder="hour, date" /></Field>
              <Field label="value"><input className="form-input" style={{ ...inp, width: 110 }} value={tbValue} onChange={e => setTbValue(e.target.value)} placeholder="count" /></Field>
            </div>
          </div>
        )}
        {shapes.has('kpi') && (
          <div style={rowCard}>
            <div style={{ display: 'flex', alignItems: 'center' }}>
              <b style={{ fontSize: 12 }}>KPI 항목 ({kpiItems.length})</b>
              <span style={{ marginLeft: 'auto' }}><Btn onClick={() => setKpiItems(it => [...it, { label: '', path: '' }])}>＋ 항목</Btn></span>
            </div>
            {kpiItems.map((k, i) => (
              <div key={i} style={{ display: 'flex', gap: 8, marginTop: 6, alignItems: 'flex-end' }}>
                <Field label="라벨"><input className="form-input" style={{ ...inp, width: 110 }} value={k.label} onChange={e => upKpi(i, { label: e.target.value })} /></Field>
                <Field label="path"><input className="form-input" style={{ ...inp, width: 160 }} value={k.path} onChange={e => upKpi(i, { path: e.target.value })} placeholder="voip.total_attempts" /></Field>
                <Field label="단위"><input className="form-input" style={{ ...inp, width: 50 }} value={k.unit ?? ''} onChange={e => upKpi(i, { unit: e.target.value })} /></Field>
                <Field label="format"><select className="form-input" style={{ ...inp, width: 90 }} value={k.format ?? ''} onChange={e => upKpi(i, { format: e.target.value || undefined })}>
                  <option value="">—</option><option value="duration">duration</option></select></Field>
                <span style={{ paddingBottom: 4 }}><Btn danger onClick={() => setKpiItems(it => it.filter((_, x) => x !== i))}>✕</Btn></span>
              </div>
            ))}
          </div>
        )}
        {shapes.has('distribution') && (
          <div style={rowCard}>
            <b style={{ fontSize: 12 }}>분포 매핑</b>
            <div style={{ display: 'flex', gap: 8, marginTop: 6, flexWrap: 'wrap' }}>
              <Field label="fromObject" hint="(dict 경로)"><input className="form-input" style={{ ...inp, width: 180 }} value={distObj} onChange={e => setDistObj(e.target.value)} placeholder="voip.end_reasons" /></Field>
              <Field label="totalPath" hint="(분모)"><input className="form-input" style={{ ...inp, width: 180 }} value={distTotal} onChange={e => setDistTotal(e.target.value)} placeholder="voip.total_attempts" /></Field>
            </div>
          </div>
        )}
        {shapes.has('table') && (
          <div style={rowCard}>
            <b style={{ fontSize: 12 }}>표 매핑</b>
            <div style={{ display: 'flex', gap: 8, marginTop: 6, flexWrap: 'wrap' }}>
              <Field label="fromObject" hint="(dict 경로)"><input className="form-input" style={{ ...inp, width: 180 }} value={tblObj} onChange={e => setTblObj(e.target.value)} placeholder="method_counts" /></Field>
              <Field label="컬럼" hint="(키, 값 — 쉼표)"><input className="form-input" style={{ ...inp, width: 160 }} value={tblCols} onChange={e => setTblCols(e.target.value)} placeholder="메서드, 건수" /></Field>
            </div>
          </div>
        )}
      </div>
      <div className="modal-footer">
        <Btn onClick={onClose} disabled={saving}>취소</Btn>
        <button className="btn btn--primary" onClick={save} disabled={saving}>저장</button>
      </div>
    </Modal>
  )
}
