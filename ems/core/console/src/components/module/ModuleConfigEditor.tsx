import { AlertTriangle, Check, Plus, RotateCw, Zap } from 'lucide-react'
import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useToast } from '../Toast'
import {
 deploymentApi, type ConfigTemplateCollection, type ConfigTemplateField,
} from '../../api/deployment'
import StringListInput from './StringListInput'
import { ObjectListEditor } from './ObjectListEditor'
import { useConfirm } from '../custom/confirm'
import { Button } from '@core/components/ui/button'
import { ToggleGroup, ToggleGroupItem } from '@core/components/ui/toggle-group'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { NONE, fromSel, toSel } from '@core/components/custom/select-value'
import { DataTable, Th, Td } from '@core/components/custom/data-table'

type Record_ = Record<string, unknown>

// ref 필드의 옵션 (collection name → option list) 을 전역 캐시로 관리.
// 한 페이지에서 여러 editor 가 열려도 중복 fetch 방지.
type RefOptions = Record<string, string[]>
const refOptionsCache: { current: RefOptions } = { current: {} }
// 참조 컬렉션의 전체 레코드 — 교차 전제조건 표시용 (access_services 편집 시 local_nodes 의
//   TLS/IPSEC 접속점 유무 판정). 이름 캐시와 같은 fetch 에서 채워지며 같은 신선도를 가진다.
type RefRecords = Record<string, Record_[]>
const refRecordsCache: { current: RefRecords } = { current: {} }

export type ModuleConfigEditorSource =
  | { type: 'deployment'; deploymentId: number }
  | { type: 'module'; moduleName: string }
  // HA 그룹 단위 — fetch 는 첫 멤버, save 는 모든 멤버에 PUT.
  // R2(그룹 설정 편집 폐지) 이후 콘솔 미사용 — 백엔드 그룹 collection API 와 세트라 보존.
  // 컬렉션 저장은 이 서버에만 — 그룹 정합은 그룹 [설정 비교]의 명시적 [동기화]로.
  | { type: 'group'; deploymentIds: number[] }

interface Props {
 source: ModuleConfigEditorSource
 collection: ConfigTemplateCollection
  // 저장 성공 직후 훅 — 그룹 설정 패널이 ON 상태에서 즉시 멤버 전파에 사용 (R4)
 onSaved?: () => void | Promise<void>
}

// T2 (2026-05-18) drift 정보 응답 구조 — UI 가 ha_group 멤버 정합 표시용.
interface DriftInfo {
 detected: boolean
 peers: Array<{ deployment_id: number; agent_id: number; status: number;
 ok: boolean; count: number | null; hash: string }>
 mode?: string | null
 scope?: string | null
}

function ModuleConfigEditorInner({ source, collection, onSaved }: Props) {
 const { show } = useToast()
 const confirm = useConfirm()
 const [records, setRecords]   = useState<Record_[]>([])
 const [original, setOriginal] = useState<Record_[]>([])
 const [loading, setLoading]   = useState(true)
 const [saving, setSaving]     = useState(false)
 const [editingIdx, setEditingIdx] = useState<number | null>(null)
 const [tagFilter, setTagFilter] = useState<string>('')
 const [refOpts, setRefOpts]     = useState<RefOptions>(refOptionsCache.current)
 const [refRecords, setRefRecords] = useState<RefRecords>(refRecordsCache.current)
 const [drift, setDrift]       = useState<DriftInfo>({ detected: false, peers: [] })
 const refOptsLoaded = useRef(new Set<string>())
  // load() 가 편집 중 refetch 로 입력을 덮어쓰지 않도록 하는 가드 미러 —
  // useCallback 클로저가 스테일 값을 보지 않게 ref 로 최신 상태 유지.
 const editGuardRef = useRef({ dirty: false, editing: false })

 const fields = collection.schema.fields
 const idField = collection.schema.id_field || 'id'

  // ref/ref_list 필드가 참조하는 collection 들 자동 fetch
 useEffect(() => {
 const needs = new Set<string>()
 const walk = (fs: ConfigTemplateField[]) => {
 for (const f of fs) {
 if ((f.type === 'ref' || f.type === 'ref_list') && f.ref_collection) {
 needs.add(f.ref_collection)
        }
 if (f.type === 'object_list' && f.item_schema) walk(f.item_schema.fields)
      }
    }
 walk(fields)
 const todo = Array.from(needs).filter(c => !refOptsLoaded.current.has(c))
 if (todo.length === 0) return
 todo.forEach(c => refOptsLoaded.current.add(c))
 const getter = source.type === 'deployment'
      ? (key: string) => deploymentApi.getDeploymentCollection(source.deploymentId, key)
      : source.type === 'group'
      ? (key: string) => deploymentApi.getDeploymentCollection(source.deploymentIds[0], key)
      : (key: string) => deploymentApi.getModuleCollection(source.moduleName, key)
    Promise.all(todo.map(async c => {
 try {
 const r = await getter(c)
 const recs = r.records as Record_[]
 const names = recs
          .map(rec => String(rec['name'] || rec['id'] || ''))
          .filter(s => !!s)
 return [c, names, recs] as [string, string[], Record_[]]
      } catch {
 return [c, [] as string[], [] as Record_[]] as [string, string[], Record_[]]
      }
    })).then(pairs => {
 const merge: RefOptions = { ...refOptionsCache.current }
 const mergeRecs: RefRecords = { ...refRecordsCache.current }
 for (const [c, v, recs] of pairs) { merge[c] = v; mergeRecs[c] = recs }
 refOptionsCache.current = merge
 refRecordsCache.current = mergeRecs
 setRefOpts({ ...merge })
 setRefRecords({ ...mergeRecs })
    })
  }, [fields, source])

  // source 분기 fetch/save — PUT 은 해당 deployment 에만 저장 (그룹 전파 없음).
  // GET 응답에 drift_detected / peers 가 포함되어 UI 가 양 멤버 정합 표시 가능.
  // 멤버 간 정합은 그룹 [설정 비교] 뷰의 명시적 [동기화]로 맞춘다.
 const fetchCollection = useCallback(() => {
 if (source.type === 'deployment')
 return deploymentApi.getDeploymentCollection(source.deploymentId, collection.key)
 if (source.type === 'group')
 return deploymentApi.getDeploymentCollection(source.deploymentIds[0], collection.key)
 return deploymentApi.getModuleCollection(source.moduleName, collection.key)
  }, [source, collection.key])

 const saveCollection = useCallback(async (recs: Record_[]) => {
 if (source.type === 'deployment') {
 return deploymentApi.putDeploymentCollection(source.deploymentId, collection.key, recs, true)
    }
 if (source.type === 'group') {
      // 미사용 경로 보존 — 첫 멤버에만 PUT (전파 없음).
 return deploymentApi.putDeploymentCollection(source.deploymentIds[0], collection.key, recs, true)
    }
 return deploymentApi.putModuleCollection(source.moduleName, collection.key, recs, true)
  }, [source, collection.key])

 const load = useCallback(async (force = false) => {
 setLoading(true)
 try {
 const r = await fetchCollection() as Record_ & {
 records: Record_[]
 drift_detected?: boolean
 peers?: DriftInfo['peers']
 ha_group_mode?: string | null
 scope?: string | null
      }
      // 편집 중(미저장 변경 또는 행 편집 열림) refetch 는 버퍼를 덮어쓰지 않는다 —
      // 불안정한 부모 prop 등으로 load 가 재실행돼도 입력 소실 방지. drift 정보만 갱신.
      // force = 사용자의 명시적 [다시 읽기] — 편집 중이어도 서버 값으로 교체.
 if (force || (!editGuardRef.current.dirty && !editGuardRef.current.editing)) {
 setRecords(r.records)
 setOriginal(JSON.parse(JSON.stringify(r.records)))
      }
 setDrift({
 detected: !!r.drift_detected,
 peers: r.peers || [],
 mode: r.ha_group_mode ?? null,
 scope: r.scope ?? null,
      })
    } catch (e) {
 show(`${collection.title} 로드 실패: ${(e as Error).message}`, 'err')
    } finally {
 setLoading(false)
    }
  }, [fetchCollection, collection.title, show])

 useEffect(() => { void load() }, [load])

 const dirty = useMemo(
    () => JSON.stringify(records) !== JSON.stringify(original),
 [records, original]
  )
 editGuardRef.current = { dirty, editing: editingIdx !== null }

 function addRow() {
 const r: Record_ = {}
 for (const f of fields) {
 if (f.auto) continue
 if (f.default !== undefined) r[f.key] = f.default
    }
 setRecords(rs => [...rs, r])
 setEditingIdx(records.length)
  }

 function removeRow(i: number) {
 setRecords(rs => rs.filter((_, idx) => idx !== i))
 if (editingIdx === i) setEditingIdx(null)
  }

 function updateField(i: number, key: string, value: unknown) {
 setRecords(rs => rs.map((r, idx) => idx === i ? { ...r, [key]: value } : r))
  }

 async function save() {
    // 교차 전제조건 확인 (access_services · media_security.md §4): media_srtp=required 인데
    //   TLS 도달 경로가 없으면 어떤 단말도 SRTP 를 못 켠다(CSP ERROR). 접속점을 나중에 열 수도
    //   있으므로 차단이 아닌 확인으로 둔다.
 if (collection.key === 'access_services') {
 const localNodes = refRecordsCache.current['local_nodes'] || []
 const bad = records
        .filter(r => String(r['media_srtp'] || '').toLowerCase() === 'required'
          && !accessServiceNodes(r, localNodes).some(n => String(n['protocol'] || '').toUpperCase() === 'TLS'))
        .map(r => String(r['name'] || '?'))
 if (bad.length && !await confirm({
 title: 'TLS 접속점 없음', tone: 'danger', confirmLabel: '저장', body: <>
 media_srtp=required 인데 TLS 접속점이 없는 서비스: {bad.join(', ')}
          <div className="mt-1">
            TLS 접속점이 없으면 어떤 단말도 SRTP 를 켤 수 없습니다 (CSP ERROR 로그). 그래도 저장할까요?
          </div>
        </> })) return
    }
 setSaving(true)
 try {
 const r = await saveCollection(records)
 show(`${collection.title} 저장됨 (${r.count}개, signal: ${r.signaled.length ? r.signaled.join(',') : 'n/a'})`, 'ok')
 setOriginal(JSON.parse(JSON.stringify(records)))
 if (onSaved) await onSaved()
    } catch (e) {
 show(`저장 실패: ${(e as Error).message}`, 'err')
    } finally {
 setSaving(false)
    }
  }

 async function reload() {
 await load(true)
 setEditingIdx(null)
  }

 const visibleFields = fields.filter(f => !f.readonly)
 const summaryFields = fields.filter(f =>
 !f.advanced && !f.readonly && f.type !== 'object_list'
  ).slice(0, 4) // 목록 테이블에 표시할 주요 필드 (최대 4개)

  // tag 기반 필터링 + 전체 태그 집합 수집
 const allTags = useMemo(() => {
 const s = new Set<string>()
 for (const r of records) {
 const t = r['tags']
 if (Array.isArray(t)) for (const v of t) if (typeof v === 'string' && v) s.add(v)
    }
 return Array.from(s).sort()
  }, [records])
 const visibleIdx = useMemo(() => {
 if (!tagFilter) return records.map((_, i) => i)
 return records
      .map((r, i) => ({ r, i }))
      .filter(({ r }) => Array.isArray(r['tags']) && (r['tags'] as unknown[]).includes(tagFilter))
      .map(({ i }) => i)
  }, [records, tagFilter])

 if (loading) return <div className="flex min-h-0 flex-1 items-center justify-center text-center text-muted-foreground p-[20px]">로딩 중...</div>

 return (
    <div>
      {collection.description && (
        <div className="text-sm text-muted-foreground mb-2.5">
          {collection.description}
          {collection.reload_hint && (
            <span className="ml-2 text-success inline-flex items-center gap-[3px]"><Zap size={12} /> {collection.reload_hint}</span>
          )}
        </div>
      )}

      {/* T2 drift 배너 — ha_group 멤버 정합 불일치 */}
      {drift.detected && (
        <div className="bg-warning-soft border border-border rounded-[4px] py-2 px-3 mb-2.5 text-sm text-warning">
          <AlertTriangle size={13} className="inline align-[-2px]" /> HA 그룹 멤버 간 정합 불일치 — 양 멤버의 jsonl 이 다릅니다.
          {drift.peers.length > 0 && (
            <span className="ml-2">
              ({drift.peers.map(p => `dep#${p.deployment_id}: ${p.count ?? 'err'}건 (${p.hash.slice(0, 6) || '–'})`).join(' / ')})
            </span>
          )}
          <span className="ml-2">정합은 그룹 선택 → [설정 비교] 뷰의 [동기화]로 맞춥니다.</span>
        </div>
      )}
      {!drift.detected && drift.peers.length > 1 && (
        <div className="text-xs text-success mb-2">
          <Check size={13} className="inline align-[-2px]" /> HA 그룹 멤버 정합 (mode={drift.mode || '?'}, {drift.peers.length} 멤버)
        </div>
      )}

      {/* tag filter chip */}
      {allTags.length > 0 && (
        <div className="mb-2 flex gap-1 flex-wrap items-center">
          <span className="text-xs text-muted-foreground">태그 필터:</span>
          <ToggleGroup type="single" value={tagFilter || '*'} className="shrink-0 flex-wrap justify-start rounded-md bg-muted p-[3px]"
 onValueChange={(v: string) => v && setTagFilter(v === '*' ? '' : v)}>
            <ToggleGroupItem value="*">전체</ToggleGroupItem>
            {allTags.map(t => (
              <ToggleGroupItem key={t} value={t}>{t}</ToggleGroupItem>
            ))}
          </ToggleGroup>
        </div>
      )}

      {/* 행 목록 */}
      <DataTable sticky>
        <thead>
          <tr>
            {summaryFields.map(f => <Th key={f.key}>{f.label}</Th>)}
            <Th className="w-[160px]">작업</Th>
          </tr>
        </thead>
        <tbody>
          {visibleIdx.length === 0 ? (
            <tr><Td className="text-center text-muted-foreground p-5" colSpan={summaryFields.length + 1}>
              {records.length === 0 ? '행 없음 — [추가] 로 생성' : '태그 필터 결과 없음'}
            </Td></tr>
          ) : (
 visibleIdx.map(i => {
 const r = records[i]
 return (
                <RowDisplay key={String(r[idField] || i)} row={r}
 summaryFields={summaryFields}
 active={editingIdx === i}
 onEdit={() => setEditingIdx(editingIdx === i ? null : i)}
 onRemove={() => removeRow(i)} />
              )
            })
          )}
        </tbody>
      </DataTable>

      {/* 편집 영역 */}
      {editingIdx !== null && records[editingIdx] && (
        <div className="mt-2.5 p-3 border border-border rounded-sm bg-muted">
          <div className="flex items-center gap-2.5 mb-2">
            <b className="text-md">행 #{editingIdx + 1} 편집</b>
          </div>
          <div style={{
 display: 'grid', gridTemplateColumns: '160px 1fr',
 rowGap: 8, columnGap: 10, alignItems: 'start',
          }}>
            {visibleFields.map(f => (
              <FieldEditor key={f.key} field={f}
 value={records[editingIdx][f.key]}
 refOpts={refOpts}
 onChange={v => updateField(editingIdx, f.key, v)} />
            ))}
            {collection.key === 'access_services' && (
              <AccessServiceSecurityHints record={records[editingIdx]}
 localNodes={refRecords['local_nodes'] || []} />
            )}
          </div>
        </div>
      )}

      <div className="mt-2.5 flex gap-2">
        <Button onClick={addRow}><Plus size={13} /> 추가</Button>
        <Button onClick={reload} disabled={saving}><RotateCw size={13} /> 다시 로드</Button>
        <span className="ml-auto">
          <Button variant="default" onClick={save}
 disabled={!dirty || saving}>
            {saving ? '저장 중...' : `저장 (${records.length}개)`}
          </Button>
        </span>
      </div>
    </div>
  )
}

/** 부모 (ModuleConfigModal) 가 재렌더링될 때, props (source, collection) 가
 *  동일 reference 이면 editor 재렌더 자체를 막는다. addRow 등 로컬 state 가
 *  상위 polling 에 의해 초기화되는 것을 방지하는 최종 방어선. */
const ModuleConfigEditor = memo(
  ModuleConfigEditorInner,
  (prev, next) => prev.source === next.source && prev.collection === next.collection
)
export default ModuleConfigEditor

// ── 접속서비스 보안 전제조건 표시 (sip_access_security.md §3·§8.3 / media_security.md §4) ──
// media_srtp·sec_mechanisms 는 local_nodes 의 도달 경로가 전제인데, 조합 오류가 CSP 기동
//   로그(ERROR)에서야 드러나므로 편집 시점에 상태를 보여준다. 판정 재료는 편집기 로드 시점의
// local_nodes 캐시 — 접속점을 방금 고쳤다면 [다시 로드] 후 확인.
function accessServiceNodes(record: Record_, localNodes: Record_[]): Record_[] {
 const restricted = record['inbound_policy'] === 'restricted'
 const refs = Array.isArray(record['allowed_local_node_refs'])
    ? new Set((record['allowed_local_node_refs'] as unknown[]).map(String)) : new Set<string>()
 return localNodes.filter(n => n['enabled'] !== false
    && (!restricted || refs.size === 0 || refs.has(String(n['name'] || ''))))
}

function AccessServiceSecurityHints({ record, localNodes }: { record: Record_; localNodes: Record_[] }) {
 const nodes = accessServiceNodes(record, localNodes)
 const byProto = (p: string) => nodes.filter(n => String(n['protocol'] || '').toUpperCase() === p)
 const fmt = (ns: Record_[]) => ns.map(n => `${n['name']}(${n['bind_port']})`).join(', ')
 const srtp = String(record['media_srtp'] || 'off').toLowerCase()
 const mechs = Array.isArray(record['sec_mechanisms'])
    ? (record['sec_mechanisms'] as unknown[]).map(String) : []
 const tls = byProto('TLS')
 const ipsec = byProto('IPSEC')
 const lines: Array<{ ok: boolean; text: string }> = []
 if (srtp !== 'off') {
 lines.push(tls.length
      ? { ok: true, text: `media_srtp=${srtp} — TLS 도달 경로: ${fmt(tls)} (TLS 접속 단말만 SRTP 반영)` }
      : { ok: false, text: `media_srtp=${srtp} — 이 서비스에 TLS 접속점이 없어 어떤 단말도 SRTP 를 켤 수 없습니다${srtp === 'required' ? ' (required 무효 — CSP ERROR)' : ''}` })
  }
 if (mechs.includes('ipsec-3gpp')) {
 lines.push(ipsec.length
      ? { ok: true, text: `ipsec-3gpp — IPSEC 접속점: ${fmt(ipsec)} (AKA 가입자에게만 제시, NAT 미지원)` }
      : { ok: false, text: 'ipsec-3gpp — IPSEC 접속점이 없어 제시 목록에서 제외됩니다 (Security-Server 에 tls 만)' })
  }
 if (lines.length === 0) return null
 return (
    <div style={{ gridColumn: '1 / -1', display: 'flex', flexDirection: 'column', gap: 4,
 borderTop: '1px solid var(--border)', paddingTop: 8 }}>
      {lines.map((l, i) => (
        <div key={i} style={{ fontSize: 11.5, color: l.ok ? 'var(--muted-foreground)' : 'var(--cims-warning)' }}>
          {l.ok ? <Check size={12} className="inline align-[-2px]" />
                       : <AlertTriangle size={12} className="inline align-[-2px]" />} {l.text}
        </div>
      ))}
    </div>
  )
}

function RowDisplay({ row, summaryFields, active, onEdit, onRemove }: {
 row: Record_
 summaryFields: ConfigTemplateField[]
 active: boolean
 onEdit: () => void
 onRemove: () => void
}) {
 return (
    <tr style={{ background: active ? 'var(--cims-brand-soft)' : undefined }}>
      {summaryFields.map(f => (
        <Td key={f.key} className="text-sm">
          {formatValue(row[f.key], f)}
        </Td>
      ))}
      <Td>
        <div className="flex gap-1">
          <Button onClick={onEdit}>
            {active ? '닫기' : '편집'}
          </Button>
          <Button variant="destructive" onClick={onRemove}>삭제</Button>
        </div>
      </Td>
    </tr>
  )
}

// 체크박스 다중 선택 — 문자열 배열 값 공통 (ref_list 참조 목록 / options 선언 string_list)
function CheckboxList({ options, value, onChange, emptyText }: {
 options: string[]
 value: unknown
 onChange: (v: unknown) => void
 emptyText?: string
}) {
 const selected = new Set(Array.isArray(value) ? (value as unknown[]).map(String) : [])
 return (
    <div className="flex flex-col gap-0.5 max-h-[120px] overflow-y-auto border border-border rounded-[4px] p-1">
      {options.length === 0 ? (
        <div className="text-xs text-muted-foreground">{emptyText || '(항목 없음)'}</div>
      ) : options.map(o => (
        <label className="text-sm" key={o}>
          <input type="checkbox" checked={selected.has(o)}
 onChange={e => {
 const next = new Set(selected)
 if (e.target.checked) next.add(o); else next.delete(o)
 onChange(Array.from(next))
            }} /> {o}
        </label>
      ))}
    </div>
  )
}

function formatValue(v: unknown, f: ConfigTemplateField): React.ReactNode {
 if (v === undefined || v === null || v === '') return '—'
 if (f.type === 'bool') return v ? <Check size={13} className="inline align-[-2px]" /> : ''
 if (f.type === 'password') return '••••'
 if (f.type === 'string_list' || f.type === 'ref_list') {
 return Array.isArray(v) ? (v.length ? v.join(', ') : '—') : String(v)
  }
 if (f.type === 'object_list') {
 return Array.isArray(v) ? `${v.length}개` : '—'
  }
 return String(v)
}

function FieldEditor({ field, value, refOpts, onChange }: {
 field: ConfigTemplateField
 value: unknown
 refOpts: RefOptions
 onChange: (v: unknown) => void
}) {
 return (
    <>
      <label className="text-md pt-1.5">
        {field.label}
        {field.required && <span className="text-destructive ml-1">*</span>}
      </label>
      <div>
        {renderInput(field, value, onChange, refOpts)}
        {field.help && (
          <div className="text-xs text-muted-foreground mt-[3px]">{field.help}</div>
        )}
      </div>
    </>
  )
}

function renderInput(f: ConfigTemplateField, value: unknown, onChange: (v: unknown) => void,
 refOpts: RefOptions = {}) {
 if (f.type === 'bool') {
 return <input type="checkbox" checked={!!value} onChange={e => onChange(e.target.checked)} />
  }
 if (f.type === 'enum') {
 return (
      <Select value={toSel((value as string) ?? '')} onValueChange={(v: string) => onChange(fromSel(v))}>
        <SelectTrigger><SelectValue /></SelectTrigger>
        <SelectContent>
          {(f.options || []).map(o => <SelectItem key={o} value={o}>{o}</SelectItem>)}
        </SelectContent>
      </Select>
    )
  }
 if (f.type === 'int') {
 return (
      <Input type="number"
 min={f.min} max={f.max}
 value={value === null || value === undefined ? '' : Number(value)}
 onChange={e => onChange(e.target.value === '' ? null : Number(e.target.value))} />
    )
  }
 if (f.type === 'password') {
 return (
      <Input type="password"
 value={(value as string) ?? ''}
 onChange={e => onChange(e.target.value)} />
    )
  }
 if (f.type === 'ref') {
 const options = (f.ref_collection && refOpts[f.ref_collection]) || []
 return (
      <Select value={toSel((value as string) ?? '')} onValueChange={(v: string) => onChange(fromSel(v))}>
        <SelectTrigger><SelectValue /></SelectTrigger>
        <SelectContent>
          <SelectItem value={NONE}>(선택 안함)</SelectItem>
          {options.map(o => <SelectItem key={o} value={o}>{o}</SelectItem>)}
        </SelectContent>
      </Select>
    )
  }
 if (f.type === 'string_list') {
    // options 선언 = 닫힌 값 공간 → 체크박스 다중 선택 (자유 입력 오타의 조용한 실패 방지 —
    //   예: access_services.sec_mechanisms). 미선언이면 종전 콤마 분리 입력.
 if (f.options?.length) {
 return <CheckboxList options={f.options} value={value} onChange={onChange} />
    }
    // 콤마 분리 입력 ↔ 문자열 배열
 return <StringListInput value={value} onChange={onChange} />
  }
 if (f.type === 'ref_list') {
 const options = (f.ref_collection && refOpts[f.ref_collection]) || []
 return <CheckboxList options={options} value={value} onChange={onChange} emptyText="(참조할 항목 없음)" />
  }
 if (f.type === 'object_list') {
    // 공용 편집기 — item 필드의 ref/ref_list 를 위해 이 renderInput 을 renderCell 로 주입.
 return (
      <ObjectListEditor field={f} value={value} onChange={onChange}
 renderCell={(cf, cv, con) => renderInput(cf, cv, con, refOpts)} />
    )
  }
 return (
    <Input type="text"
 value={(value as string) ?? ''}
 onChange={e => onChange(e.target.value)} />
  )
}

