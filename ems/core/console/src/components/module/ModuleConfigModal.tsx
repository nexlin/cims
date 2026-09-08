import { ArrowRight, ChevronDown, ChevronRight, Dot, RotateCcw, Zap } from 'lucide-react'
import { useCallback, useEffect, useMemo, useState } from 'react'
import Modal from '../Modal'
import { useToast } from '../Toast'
import {
 deploymentApi, effectiveScope,
 type Deployment, type ConfigTemplate, type ConfigTemplateField,
 type ConfigTemplateSection, type ConfigScope, type DeploymentConfigHa,
 type ConfigValueSrc,
} from '../../api/deployment'
import ModuleConfigEditor, { type ModuleConfigEditorSource } from './ModuleConfigEditor'
import StringListInput from './StringListInput'
import { ObjectListEditor } from './ObjectListEditor'
import { haGroupsApi } from '../../api/ha_groups'
import { Badge } from '../ui/badge'
import { Button } from '../ui/button'
import { FormField } from '../custom/form-field'
import { SubSection } from '../custom/collapsible-section'
import { StickySaveBar } from '../custom/sticky-save-bar'
import { useConfirm } from '../custom/confirm'
import { Alert } from '../ui/alert'
import { ToggleGroup, ToggleGroupItem } from '../ui/toggle-group'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { fromSel, toSel } from '@core/components/custom/select-value'
import { EmptyState } from '@core/components/custom/empty-state'
import { Checkbox } from '@core/components/ui/checkbox'

export type FieldValue = string | number | boolean | null | string[]
// 'scalar' = 필드(sections) 탭, 나머지 문자열 = collection.key
type Tab = 'scalar' | string

export type ModuleConfigSource =
  | { type: 'deployment'; deployment: Deployment }
  | { type: 'module'; name: string; version?: string }

interface Props {
 source: ModuleConfigSource
 onClose: () => void
 onDone?: () => void | Promise<void>
  /** 그룹 공통 설정으로 가는 통로 — 시안 S3 의 [그룹 공통 설정 편집 →] (159:3166) */
 onOpenGroupConfig?: (groupId: number) => void
  // true 면 Modal 오버레이 없이 패널만 렌더 (시스템/인프라 [패키지 설정] 탭의 페이지 임베드).
 inline?: boolean
}

/**
 * 모듈 설정 모달 — deployment 모드 (배포 > 서버) / module 모드 (빌드 · 검증 > 모듈관리) 공용.
 *
 *  - deployment 모드: agent_deployment 레코드 대상. PUT → 이 서버에만 저장 +
 * update_config job (그룹 전파 없음).
 *    · AS 그룹 멤버: **서버 개별(유효 scope=system) 설정만** 노출 — 공통(service)
 *      설정·컬렉션은 그룹 탭(GroupConfigCompareView)이 유일한 편집 창구 (R4).
 *    · AA 그룹·standalone: 동기화 개념 없음 — 전체 섹션·컬렉션 편집.
 *  - module 모드:     Phase 1 로컬. PUT → build/dist/config.json (scalar) /
 * build/dist/{name}/config/*.jsonl (collection) + 로컬 PID SIGUSR1.
 */
export default function ModuleConfigModal({ source: sourceProp, onClose, onDone, inline,
 onOpenGroupConfig }: Props) {
  // 부모(ServersPage 등)가 주기 폴링으로 재렌더하며 source 객체를 매번 새로 만들면
  // fetch/editor 의 useEffect 가 재실행돼 편집값이 서버 값으로 덮어써진다 —
  // mount 시점 스냅샷으로 identity 고정 (모듈 전환은 caller 가 key 로 리마운트).
 const [source] = useState(sourceProp)
 const { show } = useToast()
 const [loading, setLoading]     = useState(true)
 const [saving, setSaving]       = useState(false)
 const [template, setTemplate]   = useState<ConfigTemplate | null>(null)
 const [values, setValues]       = useState<Record<string, FieldValue>>({})
 const [initial, setInitial]     = useState<Record<string, FieldValue>>({})
  // 값의 출처 — 'injected' 는 배포 시 OAM 이 채운 값(운영자 입력 아님). 배지 표시용.
 const [srcMap, setSrcMap]       = useState<Record<string, ConfigValueSrc>>({})
 const [appliedAt, setAppliedAt] = useState<string | null>(null)
 const [tab, setTab]             = useState<Tab>('scalar')
  // HA 그룹 컨텍스트 (deployment 모드 + 그룹 멤버일 때만) — 있으면 공통/개별 탭 분리.
 const [ha, setHa]               = useState<DeploymentConfigHa | null>(null)

  // 제목/식별자
 const title = source.type === 'deployment'
    ? `${source.deployment.package_name} v${source.deployment.package_version} — 설정`
    : `${source.name}${source.version ? ` v${source.version}` : ''} — 설정 (로컬)`

  // 설치 전(pending) — 저장은 overlay 로 보존되고 설치 시 반영. 프로세스가 없어 restart 불가.
 const isPending = source.type === 'deployment' && source.deployment.status === 'pending'

  // Editor 에 전달할 source — 매 렌더마다 새 객체를 만들면 Editor 가 useEffect 재실행 →
  // 편집 중이던 행이 서버 응답으로 덮어써진다. identity 고정 필수.
 const editorSource: ModuleConfigEditorSource = useMemo(
    () => source.type === 'deployment'
      ? { type: 'deployment', deploymentId: source.deployment.id }
      : { type: 'module', moduleName: source.name },
 [source]
  )

  // AS 그룹 멤버 — 이 화면은 서버 개별(system) 설정 전용, 공통은 그룹 탭에서 (R4).
  // AA/standalone/module 모드는 전체 편집.
 const asMember = source.type === 'deployment' && ha?.mode === 'active_standby'
 const svcFieldCount = useMemo(
    () => template ? serviceScopeKeys(template).length : 0, [template])
 const sysSections = useMemo(
    () => template ? template.sections.map(s => sectionForScope(s, 'system'))
                       .filter((s): s is ConfigTemplateSection => !!s) : [],
 [template])
 const visibleSections = asMember ? sysSections : (template?.sections ?? [])
 const visibleCollections = useMemo(
    () => (template?.collections || []).filter(
 c => !asMember || (c.scope ?? 'service') === 'system'),
 [template, asMember])
  // 제목·개수 — 그룹 멤버면 「서버 개별 설정」, 아니면 「설정」 (시안 S3 / SA3)
 const scalarTitle = asMember ? '서버 개별 설정' : '설정'
 const modName = source.type === 'deployment'
    ? (source.deployment.package_name || '모듈') : source.name
 const scalarCount = visibleSections.reduce((n, sec) => n + sec.fields.length, 0)

  // source 분기 fetch
 const fetchConfig = useCallback(async () => {
 if (source.type === 'deployment') {
 const r = await deploymentApi.getDeploymentConfig(source.deployment.id)
 return {
 template: r.template,
 config: r.config || {},
        // 노드에 실제로 들어가는 값 — 화면은 이걸 그린다(§아래 load 주석).
 effective: r.effective ?? null,
 appliedAt: r.config_applied_at,
 ha: r.ha ?? null,
      }
    }
    // 모듈 모드(소스트리 dev)는 overlay 파일이 곧 적용 설정이라 주입이 없다 — effective 불필요.
 const r = await deploymentApi.getModuleConfig(source.name)
 return {
 template: r.template,
 config: r.current || {},
 effective: null,
 appliedAt: null,
 ha: null,
    }
  }, [source])

  // source 분기 save — 항상 이 서버(또는 로컬 모듈)에만 저장. 그룹 전파 없음.
 const saveConfig = useCallback(async (vals: Record<string, FieldValue>,
 changedKeys: Set<string>) => {
 if (source.type === 'deployment') {
      // 변경된 키만 전송 — 서버는 기존 overlay 에 병합한다. 전체 값을 되돌려 보내면
      // 화면에 빈칸으로 보이던 값(다른 노드에서 만들어진 _infra 시크릿 등)이 빈 값으로
      // 덮여 사라진다(시크릿 소실 → 전면 401). 시크릿은 조회 시 마스킹돼 오므로
      // 손대지 않은 필드는 애초에 changed 에 들어오지 않는다.
 const payload: Record<string, FieldValue> = {}
 for (const k of changedKeys) payload[k] = vals[k]
 const r = await deploymentApi.putDeploymentConfig(source.deployment.id, payload, true)
      // overlay 는 config_template 선언 키만 담는다 — 템플릿 밖 키는 저장되지 않고
      // pruned_keys 로 돌아온다. 조용히 사라지면 안 되므로 결과 문구에 싣는다.
 const pruned = r.pruned_keys?.length
        ? ` · 미저장(템플릿에 없는 키): ${r.pruned_keys.join(', ')}` : ''
 return {
 ok: true,
 message: (r.job_id ? `저장됨. update_config job #${r.job_id}` : '저장됨') + pruned,
      }
    }
    // module 모드: 변경된 키만 보냄 (not_owned_by_module 오류 회피 위해 모든 키 아닌 템플릿 소유 키만)
 const payload: Record<string, unknown> = {}
 for (const k of changedKeys) payload[k] = vals[k]
 const r = await deploymentApi.putModuleConfig(source.name, payload)
 return {
 ok: true,
 message: `${r.applied}개 저장${r.removed ? ` · ${r.removed}개 제거` : ''}${r.restart_required ? ' · 재시작 필요' : ''}`,
    }
  }, [source])

 const load = useCallback(async () => {
 try {
 const r = await fetchConfig()
 setTemplate(r.template)
 setAppliedAt(r.appliedAt)
      // 표시 기준 = **노드에 실제로 들어가는 값**(`effective`). overlay 만 그리면
      // 배포 시 주입되는 값(JWT 시크릿·store 경로 등)이 빈칸으로 보여 "설정 안 됨"으로
      // 오해되고, 주입이 overlay 를 덮는 키는 화면과 노드가 다른 상태가 드러나지 않는다.
      // `src === 'default'` 는 템플릿 기본값이므로 위젯 타입에 맞는 `defaultValue(f)` 를
      // 쓴다 — 빈 기본값(''/[]/null)은 실체화에서 제외되므로 그 값을 그대로 넣으면
      // 배열/불린 위젯이 깨진다. `effective` 없는 응답(구 OAM·모듈 모드)은 종전 규칙.
 const base: Record<string, FieldValue> = {}
 if (r.template) {
 for (const s of r.template.sections) {
 for (const f of s.fields) {
 const eff = r.effective?.[f.key]
 if (eff && eff.src !== 'default') {
 base[f.key] = eff.v as FieldValue
            } else if (!r.effective) {
 const existing = r.config[f.key]
 base[f.key] = existing !== undefined
                ? (existing as FieldValue)
                : defaultValue(f)
            } else {
 base[f.key] = defaultValue(f)
            }
          }
        }
      }
 for (const [k, v] of Object.entries(r.config)) {
 if (!(k in base)) base[k] = v as FieldValue
      }
 setValues(base)
 setInitial(base)
 setSrcMap(Object.fromEntries(
        Object.entries(r.effective || {}).map(([k, c]) => [k, c.src])))
 setHa(r.ha)
    } catch (e) {
 show((e as Error).message, 'err')
    } finally {
 setLoading(false)
    }
  }, [fetchConfig, show])

 useEffect(() => { void load() }, [load])

  // 변경된 필드 추적 — string[] 은 참조가 아닌 내용 비교 (StringListInput 이 새 배열을
  // 반환해도 값이 같으면 미변경 — 체크된 배열 필드의 불필요한 피어 전파 차단)
 const changed = useMemo(() => {
 const s = new Set<string>()
 for (const k of new Set([...Object.keys(values), ...Object.keys(initial)])) {
 if (!fieldValueEq(values[k], initial[k])) s.add(k)
    }
 return s
  }, [values, initial])

  // 이 화면 필드 중 재기동이 필요한 것이 있는가 — 변경 0건일 때 저장바가 쓰는 값.
  // (변경이 있으면 아래 restartRequired 가 **바뀐 것만** 보고 판정한다.)
 const scopeNeedsRestart = useMemo(
    () => visibleSections.some(sec => sec.fields.some(f => f.restart !== false)),
 [visibleSections])

  // 변경된 필드 중 restart:true 포함 여부
 const restartRequired = useMemo(() => {
 if (!template) return false
 for (const s of template.sections) {
 for (const f of s.fields) {
 if (changed.has(f.key) && (f.restart !== false)) return true
      }
    }
 return false
  }, [template, changed])
  // 저장바 표시용 — 변경이 있으면 바뀐 것만, 없으면 이 화면 성격을 알린다.
 const barRestart = changed.size > 0 ? restartRequired : scopeNeedsRestart

  // 저장 전 validation — required + range. 이 화면에 보이는 필드만 검사
  // (AS 멤버는 공통 필드가 숨겨져 있어 사용자가 고칠 수 없으므로 대상 제외).
 function validate(): string | null {
 if (!template) return null
 for (const s of visibleSections) {
 for (const f of s.fields) {
 const v = values[f.key]
 if (f.required && (v === '' || v === null || v === undefined)) {
 return `필수 항목 비어있음: ${f.label} (${f.key})`
        }
 if (f.type === 'int' && typeof v === 'number') {
 if (f.min !== undefined && v < f.min) return `${f.label}: ${v} < min(${f.min})`
 if (f.max !== undefined && v > f.max) return `${f.label}: ${v} > max(${f.max})`
        }
      }
    }
 return null
  }

 async function save(opts: { restartAfter?: boolean } = {}) {
 if (changed.size === 0) { show('변경된 항목 없음', 'err'); return }
 const err = validate()
 if (err) { show(err, 'err'); return }
 setSaving(true)
 try {
 const r = await saveConfig(values, changed)
 show(r.message, 'ok')
      // 재기동 옵션 (deployment 모드 + restart_required + restartAfter true)
 if (opts.restartAfter && source.type === 'deployment') {
 try {
 const jr = await deploymentApi.queueJob(source.deployment.id, 'restart')
 show(`재기동 큐 등록 (#${jr.job_id})`, 'ok')
        } catch (e) {
 show(`재기동 실패: ${(e as Error).message}`, 'err')
        }
      }
 if (onDone) await onDone()
 if (source.type === 'deployment') {
        // 저장 성공 = 현재 값이 새 기준값 — inline 임베드(패키지 설정 탭)는 onClose 가
        // no-op 이라 여기서 리셋하지 않으면 changed 가 남아 저장 버튼이 계속 활성.
 setInitial(values)
 onClose()
      } else {
 await load()
      }
    } catch (e) {
 show((e as Error).message, 'err')
    } finally {
 setSaving(false)
    }
  }

 const body = (
      <div className="flex flex-col h-full">
        {loading ? (
          <div className="flex min-h-0 flex-1 items-center justify-center text-center text-muted-foreground p-[40px]">로딩 중...</div>
        ) : !template ? (
          <EmptyState title={<>이 패키지에는 <code>config_template.json</code> 이 포함되어 있지 않습니다.</>}
                      description="설정 가능한 항목이 없습니다." />
        ) : (
          <>
            {/* linkRow — 정본 Figma S3 `linkRow`(159:3164): 제목 + 범례 + 우측
 [그룹 공통 설정 편집 →]. 컬렉션이 있는 모듈은 제목 자리에 세그먼트를 둔다
                (시안 G3 의 `공통 설정 | 멤버 비교` 와 같은 자리·같은 모양). */}
            <div className="flex shrink-0 items-center gap-2 px-4 pb-2.5 pt-3.5">
              {visibleCollections.length === 0 ? (
                <span className="shrink-0 text-md font-semibold">
                  {scalarTitle} ({scalarCount})
                </span>
              ) : (
                <ToggleGroup type="single" value={tab} className="shrink-0 justify-start rounded-md bg-muted p-[3px]"
 onValueChange={(v: string) => v && setTab(v)}>
                  <ToggleGroupItem value="scalar">{scalarTitle} ({scalarCount})</ToggleGroupItem>
                  {visibleCollections.map(c => (
                    <ToggleGroupItem key={c.key} value={c.key}>{c.title}</ToggleGroupItem>
                  ))}
                </ToggleGroup>
              )}
              <span className="truncate text-xs text-muted-foreground">
                재기동 = 저장 후 재기동 시 반영 · 즉시 = 저장 즉시 반영
                {appliedAt && ` · 마지막 적용: ${appliedAt}`}
              </span>
              {asMember && ha && onOpenGroupConfig && (
                <Button variant="ghost" className="ml-auto shrink-0"
 onClick={() => onOpenGroupConfig(ha.group_id)}
 title={`그룹 ${ha.group_name} 의 [패키지 설정] 으로 이동`}>
                  그룹 공통 설정 편집 <ArrowRight />
                </Button>
              )}
            </div>

            {/* 스크롤 영역 */}
            <div className="min-h-0 flex-1 overflow-auto px-4 pb-4">
              {tab === 'scalar' ? (
                <>
                  {asMember && ha && (
                    <Alert variant="info" className="mb-3">
                      <div className="font-medium">
                        이 화면은 이 서버 고유 설정(bind IP · 노드 식별자 등)만 다룹니다
                      </div>
                      <div className="mt-0.5 text-xs opacity-90">
                        그룹 공통 설정 {svcFieldCount}개 필드와 공통 컬렉션은 좌측 트리에서
                        그룹 <b>{ha.group_name}</b> 선택 → [패키지 설정] 에서 편집합니다
                        (동기화 스위치 포함).
                      </div>
                    </Alert>
                  )}
                  {changed.size > 0 && (
                    <ChangeSummaryPanel template={template} values={values} initial={initial}
 changed={changed}
 onReset={(k) => setValues(p => ({ ...p, [k]: initial[k] }))}
 onResetAll={() => setValues({ ...initial })} />
                  )}
                  {visibleSections.map(sec => (
                    <SectionBlock key={sec.key} section={sec} values={values}
 initial={initial} changed={changed}
 srcOf={(k) => srcMap[k]}
 onChange={(k, v) => setValues(p => ({ ...p, [k]: v }))}
 onReset={(k) => setValues(p => ({ ...p, [k]: initial[k] }))}
 footer={sec.key === 'store'
                        ? <StoreMigrateFooter groupId={ha?.group_id ?? null}
 mountPoint={String(values['CimsRuntimeMount'] ?? '')}
 dirty={changed.has('CimsRuntimeMount') || changed.has('CimsRuntimeDir')}
 onDone={onDone} />
                        : undefined} />
                  ))}
                  {isPending && (
                    <Alert variant="info" className="mt-3">
                      아직 <b>설치 전</b>입니다 — 저장한 값은 [패키지 설치] 탭에서
                      <b> 설치</b> 실행 시 반영됩니다.
                    </Alert>
                  )}
                </>
              ) : (
                (() => {
 const coll = visibleCollections.find(c => c.key === tab)
 if (!coll) return <EmptyState title="collection 을 찾을 수 없음" />
 return <ModuleConfigEditor source={editorSource} collection={coll} />
                })()
              )}
            </div>
          </>
        )}
        {/* 저장바 — 시안 S3 는 StickySaveBar 하나다 (459:7238).
            배지 자리에 「무엇을 더 해야 반영되는가」를 둔다: 재기동이 필요한 변경이 섞이면
            `재기동`, 아니면 `즉시`. 저장 후 재기동은 별도 버튼이 아니라 저장의 옵션이다. */}
        {template && tab === 'scalar' && (
          <StickySaveBar
 badge={<Badge variant="neutralSoft">{barRestart ? '재기동' : '즉시'}</Badge>}
 note={isPending
              ? '아직 설치 전 — 저장한 값은 [패키지 설치] 탭의 설치 실행 시 반영됩니다'
              : barRestart
                ? `저장 후 ${modName} 을 재기동하면 새 설정이 적용됩니다`
                : '저장 즉시 반영됩니다'}
 saveLabel={`저장 (${changed.size} 변경)`}
 disabled={changed.size === 0}
 saving={saving}
 extra={!inline
              ? <Button variant="outline" onClick={onClose}>닫기</Button>
              : undefined}
 onRevert={() => setValues({ ...initial })}
 onSave={() => void save({
 restartAfter: source.type === 'deployment' && restartRequired && !isPending,
            })} />
        )}
        {!inline && !(template && tab === 'scalar') && (
          <div className="flex shrink-0 justify-end border-t border-border bg-card px-4 py-2.5">
            <Button variant="outline" onClick={onClose}>닫기</Button>
          </div>
        )}
      </div>
  )
 if (inline) return body
 return (
    <Modal title={title} onClose={onClose} fullscreen>
      {body}
    </Modal>
  )
}

// 섹션을 유효 scope 로 필터 — 해당 scope 필드가 없으면 null (화면에서 섹션 생략).
// 필드 오버라이드(f.scope) 덕에 한 섹션이 서버/그룹 화면에 나뉘어 나타날 수 있다
// (예: csp media_server 는 그룹 화면, media_server.LocalIp 만 서버 화면).
export function sectionForScope(sec: ConfigTemplateSection, scope: ConfigScope): ConfigTemplateSection | null {
 const fields = sec.fields.filter(f => effectiveScope(f, sec.scope) === scope)
 if (fields.length === 0) return null
 return { ...sec, fields }
}

function ChangeSummaryPanel({ template, values, initial, changed, onReset, onResetAll }: {
 template: ConfigTemplate
 values: Record<string, FieldValue>
 initial: Record<string, FieldValue>
 changed: Set<string>
 onReset: (key: string) => void
 onResetAll: () => void
}) {
 const [collapsed, setCollapsed] = useState(false)
  // key → field 매핑 (label + restart 추출)
 const fieldByKey = useMemo(() => {
 const m = new Map<string, ConfigTemplateField>()
 for (const s of template.sections) for (const f of s.fields) m.set(f.key, f)
 return m
  }, [template])

 const restartKeys = Array.from(changed).filter(k => (fieldByKey.get(k)?.restart !== false))
 const hotKeys     = Array.from(changed).filter(k => (fieldByKey.get(k)?.restart === false))

 function display(v: FieldValue): string {
 if (v === null || v === undefined || v === '') return '(빈 값)'
 if (typeof v === 'boolean') return v ? 'true' : 'false'
 return String(v)
  }

 return (
    <div className="border border-border rounded-sm mb-3 bg-muted">
      <div onClick={() => setCollapsed(c => !c)}
 style={{
 padding: '8px 14px', cursor: 'pointer', userSelect: 'none',
 display: 'flex', alignItems: 'center', gap: 8,
 background: 'var(--cims-brand-soft)', borderBottom: collapsed ? 'none' : '1px solid var(--border)',
 borderRadius: '6px 6px 0 0',
        }}>
        <span className="text-primary">
          {collapsed ? <ChevronRight size={11} /> : <ChevronDown size={11} />}
        </span>
        <b className="text-primary">변경 사항 ({changed.size})</b>
        <span className="text-xs text-muted-foreground">
          <RotateCcw size={12} /> 재기동 {restartKeys.length} · <Zap size={12} /> 즉시 {hotKeys.length}
        </span>
        <button className="ml-auto text-xs py-0.5 px-2 bg-card border border-border rounded-[3px] cursor-pointer" onClick={(e) => { e.stopPropagation(); onResetAll() }}>
          전체 초기화
        </button>
      </div>
      {!collapsed && (
        <div className="p-2 max-h-[240px] overflow-auto">
          <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
            <thead>
              <tr className="text-muted-foreground">
                <th className="text-left py-1 px-1.5 w-[220px]">필드</th>
                <th className="text-left py-1 px-1.5">옛 값</th>
                <th className="w-[30px] text-center" aria-label="바뀜"><ArrowRight size={12} /></th>
                <th className="text-left py-1 px-1.5">새 값</th>
                <th className="w-[60px] text-center"></th>
              </tr>
            </thead>
            <tbody>
              {Array.from(changed).map(k => {
 const f = fieldByKey.get(k)
 const restart = f?.restart !== false
 return (
                  <tr className="border-t border-border" key={k}>
                    <td className="py-1 px-1.5">
                      <span title={k}>{f?.label ?? k}</span>
                      <span style={{ marginLeft: 4, fontSize: 10, color: restart ? 'var(--destructive)' : 'var(--cims-success)' }}>
                        {restart ? <RotateCcw size={12} /> : <Zap size={12} />}
                      </span>
                    </td>
                    <td className="py-1 px-1.5 text-muted-foreground font-mono">
                      {display(initial[k])}
                    </td>
                    <td className="text-center text-primary">→</td>
                    <td className="py-1 px-1.5 text-primary font-mono">
                      {display(values[k])}
                    </td>
                    <td className="text-center">
                      <Button variant="ghost" size="iconSm" onClick={() => onReset(k)}
 title="이 필드만 초기화">
                        <RotateCcw />
                      </Button>
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}

export function SectionBlock({ section, values, initial, changed, onChange, onReset, footer,
 srcOf, markerOf }: {
 section: {
 key: string; title: string; description?: string
 fields: ConfigTemplateField[]
 hidden?: boolean
 groups?: { key: string; title: string; description?: string }[]
  }
 values: Record<string, FieldValue>
 initial: Record<string, FieldValue>
 changed: Set<string>
 onChange: (key: string, v: FieldValue) => void
 onReset: (key: string) => void
  /** 섹션 하단 액션 — 저장만으로는 적용되지 않는 값(관리 store 경로 등)의 정규 경로. */
 footer?: React.ReactNode
  /** 값의 출처 — `injected`(배포 시 자동 채움)를 배지로 드러낸다. 없으면 배지 없음. */
 srcOf?: (key: string) => ConfigValueSrc | undefined
  /** 마커 자리에 덧붙일 배지 — 그룹 화면의 `드리프트` 처럼 화면마다 다른 표식 (시안 G3-1 209:3255). */
 markerOf?: (key: string) => React.ReactNode
}) {
  // 접힘 상태는 SubSection 이 갖는다 — `hidden` 섹션(인프라)만 기본 접힘.
  // 모든 필드 노출 (고급/숨김 구분 제거).
 const visibleFields = section.fields

  // 필드를 group 단위로 묶기 — groups 정의 없으면 단일 묶음.
  // 그룹 선언된 순서대로 정렬하고, 소속 없는 필드는 '기타' 로.
 const groupDefs = section.groups || []
 type Bucket = { key: string; title: string; description?: string; fields: ConfigTemplateField[] }
 const buckets: Bucket[] = []
 if (groupDefs.length === 0) {
 buckets.push({ key: '__all__', title: '', fields: visibleFields })
  } else {
 const byKey = new Map<string, Bucket>()
 for (const g of groupDefs) {
 const b: Bucket = { key: g.key, title: g.title, description: g.description, fields: [] }
 byKey.set(g.key, b); buckets.push(b)
    }
 const misc: Bucket = { key: '__misc__', title: '기타', fields: [] }
 for (const f of visibleFields) {
 const target = f.group && byKey.get(f.group)
 if (target) target.fields.push(f)
 else misc.fields.push(f)
    }
 if (misc.fields.length) buckets.push(misc)
  }
 const nonEmptyBuckets = buckets.filter(b => b.fields.length > 0)

  // 시안(Figma S3 93:2208·169:2826)은 섹션을 카드로 감싸지 않는다 — 접힘 머리 + 들여쓴 본문뿐.
  // `Infrastructure` 처럼 `hidden` 인 섹션은 기본 접힘으로 열고, 「내부 전용」임을 힌트로 알린다.
 const hint = [section.hidden ? '내부 전용' : '', section.description || '']
    .filter(Boolean).join(' · ')
 return (
    <SubSection title={section.title} hint={hint || undefined} defaultOpen={!section.hidden}>
      {nonEmptyBuckets.map((b, idx) => (
        <div key={b.key} className={idx === nonEmptyBuckets.length - 1 ? undefined : 'mb-4'}>
          {b.title && (
            // sub-group 머리 — 시안 `subgroup`(169:2832). 중첩 2단 계약 안에 있으므로
            // 또 하나의 접힘 섹션이 아니라 라벨 한 줄이다.
            <div className="mb-2 flex items-baseline gap-1.5 text-sm font-semibold text-muted-foreground">
              <span>{b.title}</span>
              {b.description && (
                <span className="text-xs font-normal">— {b.description}</span>
              )}
            </div>
          )}
          <div className="flex flex-col gap-3.5">
            {b.fields.map(f => (
              <FieldRow key={f.key} field={f}
 value={values[f.key]}
 initialValue={initial[f.key]}
 isChanged={changed.has(f.key)}
 src={srcOf?.(f.key)}
 markerExtra={markerOf?.(f.key)}
 onChange={v => onChange(f.key, v)}
 onReset={() => onReset(f.key)} />
            ))}
          </div>
        </div>
      ))}
      {footer}
    </SubSection>
  )
}

/**
 * 관리 store 섹션 하단 — 경로 변경의 정규 경로.
 *
 * `CimsRuntimeMount`/`CimsRuntimeDir` 은 **저장으로 적용되는 키가 아니다.** 저장하면
 * `update_config` 만 돌아 경로만 바뀌고 데이터는 따라가지 않는다 → 새 경로에 빈 store 가
 * 생기거나(마운트 없으면) mount guard 가 기동을 거부한다. 데이터를 옮기는 것은 이관
 * (`migrate_oam_store` job: 정지 → 복사 → 기록 → 기동)뿐이므로 그 버튼을 여기 둔다.
 * 최초 지정은 부트스트랩 설치가 담당하므로(oam_ha.md §9.4) 보통 이 버튼은 쓰지 않는다.
 *
 * 이 footer 는 결과적으로 **oam 에만** 붙는다 — store 섹션을 가진 템플릿이 oam 하나이기
 * 때문이다. oam-svc 도 store 를 읽지만 위치는 oam 에서 유도되는 파생값이라 입력 창구를
 * 두지 않는다(oam_ha.md §4.1). 창구가 둘이면 서로 다른 값이 저장될 수 있고, 그때부터
 * "두 값이 같은가" 를 검사하는 코드가 따라붙는다.
 */
export function StoreMigrateFooter({ groupId, mountPoint, dirty, onDone }: {
 groupId: number | null
 mountPoint: string
 dirty: boolean
 onDone?: () => void | Promise<void>
}) {
 const { show } = useToast()
 const confirm = useConfirm()
 const [busy, setBusy] = useState(false)
 const mp = mountPoint.trim().replace(/\/+$/, '')

 async function migrate() {
 if (!groupId) return
 if (!await confirm({ title: '관리 store 이관', tone: 'danger', confirmLabel: '이관', body: <>
      관리 데이터를 이 경로로 이관합니다.
      <div className="mt-1 font-mono text-xs">{mp}/runtime</div>
      <div className="mt-2">OAM 이 정지 → 복사 → 재기동되므로 콘솔이 30초 내외 끊깁니다.</div>
      <div className="mt-1">대상에 이전 데이터가 있으면 .stale-&lt;시각&gt; 으로 보관하고 덮어씁니다.</div>
      <div className="mt-2">진행할까요?</div>
    </> })) return
 setBusy(true)
 try {
 const r = await haGroupsApi.migrateSharedStore(groupId, mp)
 show(`관리 store 이관 개시 — ${r.detail || r.runtime_dir}`, 'ok')
 await onDone?.()
    } catch (e) { show((e as Error).message, 'err') }
 finally { setBusy(false) }
  }

 return (
    <div className="mt-3 py-2 px-2.5 rounded-[4px] text-sm leading-[1.6] bg-warning-soft border border-border">
      <b>경로를 바꾸려면 이관을 쓰세요.</b> 저장은 경로만 바꾸고 <b>데이터를 옮기지
      않습니다</b> — 새 경로에 빈 store 가 생기거나, 마운트가 없으면 OAM 이 기동을
      거부합니다. 이관은 정지 → 복사 → 기동을 한 번에 처리합니다. 이 값은 <b>멤버 간
      동일해야</b> 하므로 공통 설정입니다 — 최초 지정은 부트스트랩 설치가 담당합니다.
      {groupId ? (
        <div className="mt-2 flex gap-2 items-center flex-wrap">
          <Button variant="default" disabled={busy || !mp}
 onClick={migrate}
 title="현재 입력된 마운트 지점으로 관리 store 를 이관 (콘솔 30초 단절)">
            {busy ? '이관 요청 중…' : <><ArrowRight size={13} /> {mp || '(마운트 지점)'} 으로 이관</>}
          </Button>
          {dirty && (
            <span className="text-warning">
              편집한 값이 있습니다 — 저장 대신 이 버튼을 쓰세요.
            </span>
          )}
        </div>
      ) : (
        <div className="mt-1.5 text-muted-foreground">
          이관은 HA 그룹 멤버에서만 실행할 수 있습니다 (이관 대상 노드 선정이 그룹 기준).
          단일 노드는 부트스트랩 재설치 또는 그룹 편성 후 실행하세요.
        </div>
      )}
    </div>
  )
}

/**
 * 필드 한 칸 — 정본 = Figma S3 `field`(93:2218): 세로 배치(라벨·입력·헬프)에
 * **입력 오른쪽으로 마커 배지**가 붙는다. 구 화면은 200px 라벨 컬럼 + 값 컬럼이라
 * 라벨이 길면 줄바꿈되고 헬프텍스트가 값 밑으로 흘렀다.
 *
 * 마커(`재기동`/`즉시`)는 **필드마다 반드시 하나** — 저장 후 무엇을 더 해야 반영되는지를
 * 필드 옆에서 알린다(contracts.md §TextInput/Select). 시안 실측 색은 `neutralSoft` 하나다
 * (구 화면은 재기동을 빨강으로 칠해 전 필드가 경고처럼 보였다).
 */
function FieldRow({ field, value, initialValue, isChanged, src, markerExtra, onChange, onReset }: {
 field: ConfigTemplateField
 value: FieldValue
 initialValue: FieldValue
 isChanged: boolean
  /** 값의 출처. `injected` = 운영자가 입력한 값이 아니라 배포 시 OAM 이 채운 값. */
 src?: ConfigValueSrc
  /** 마커 자리에 세로로 덧붙는 배지 (그룹 화면의 `드리프트`) */
 markerExtra?: React.ReactNode
 onChange: (v: FieldValue) => void
 onReset: () => void
}) {
 const needsRestart = field.restart !== false
 return (
    <FormField
 changed={isChanged}
 required={field.required}
 label={
        <>
          {field.label}
          {/* 자동 채움·변경 표식은 시안에 없지만 지우지 않는다 — 값의 출처와 미저장 변경은
              화면에서만 알 수 있는 사실이다 (정본 문서 §1 「시안의 침묵은 삭제 지시가 아님」). */}
          {src === 'injected' && !isChanged && (
            <Badge variant="brandSoft" className="ml-1.5 align-[1px]"
 title={'배포 시 OAM 이 채운 값입니다 — 이 서버 설정에 저장된 값이 아닙니다. '
                        + '노드에는 이 값이 들어갑니다. 일부 키(시크릿·관리망 대역)는 저장하더라도 '
                        + '배포 시 OAM 값으로 다시 채워집니다.'}>
              자동 채움
            </Badge>
          )}
          {isChanged && <Dot size={14} className="ml-0.5 inline text-primary" aria-label="저장하지 않은 변경" />}
        </>
      }
 help={
 !needsRestart && field.reload_hint
          ? <span className="inline-flex items-center gap-1 text-success">
              <Zap size={11} /> {field.reload_hint}
            </span>
          : field.help
      }
 aside={isChanged
        ? <Button variant="ghost" size="iconSm" onClick={onReset}
 title={`초기값으로 되돌림: ${initialValue === null || initialValue === '' ? '(빈 값)' : String(initialValue)}`}>
            <RotateCcw />
          </Button>
        : undefined}
 marker={
        // 시안은 마커를 세로로 쌓는다 (209:3255 — `재기동` 위, `드리프트` 아래)
        <span className="flex flex-col items-start gap-1">
          <Badge variant="neutralSoft"
 title={needsRestart ? '저장 후 재기동해야 반영됩니다' : '저장 즉시 반영됩니다'}>
            {needsRestart ? '재기동' : '즉시'}
          </Badge>
          {markerExtra}
        </span>
      }>
      {renderInput(field, value, onChange)}
    </FormField>
  )
}

function renderInput(f: ConfigTemplateField, value: FieldValue, onChange: (v: FieldValue) => void) {
 if (f.type === 'bool') {
 return (
      <Checkbox  checked={!!value} onCheckedChange={(c) => onChange((c === true))} />
    )
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
 onChange={e => {
 const s = e.target.value
 onChange(s === '' ? null : Number(s))
        }} />
    )
  }
 if (f.type === 'password') {
 return (
      <Input type="password"
 value={(value as string) ?? ''}
 onChange={e => onChange(e.target.value)} />
    )
  }
 if (f.type === 'string_list' || f.type === 'ref_list') {
    // 콤마 분리 입력 ↔ 문자열 배열 (ModuleConfigEditor 와 동일 동작).
 return (
      <StringListInput value={value}
 placeholder="콤마로 구분 (예: 10.0.1.48:9000, 10.0.1.49:9000)"
 onChange={onChange} />
    )
  }
 if (f.type === 'object_list') {
    // ip/port 등 구조화 항목 리스트. 값이 비면 빈 1행 표시 + [추가] 로 추가(최소 1행 유지).
 return (
      <ObjectListEditor field={f} value={value}
 onChange={(v) => onChange(v as FieldValue)} ensureOne />
    )
  }
  // string / path
 return (
    <Input type="text"
 value={(value as string) ?? ''}
 onChange={e => onChange(e.target.value)} />
  )
}

export function defaultValue(f: ConfigTemplateField): FieldValue {
 if (f.default !== undefined && f.default !== null) {
 return f.default as FieldValue
  }
 switch (f.type) {
 case 'bool': return false
 case 'int': return 0
 default: return ''
  }
}

// FieldValue 내용 비교 — string[] 은 참조가 아닌 원소 비교 (changed 오탐 방지).
export function fieldValueEq(a: FieldValue | undefined, b: FieldValue | undefined): boolean {
 if (Array.isArray(a) || Array.isArray(b)) return JSON.stringify(a) === JSON.stringify(b)
 return a === b
}

// 유효 scope(field.scope ?? section.scope)=service 인 필드 키 — 그룹 동기화 복사
// 대상(공통 탭 필드). ModuleConfigModal 과 GroupConfigCompareView 가 동일 규칙 공유,
// 백엔드 handlers.agents._service_scope_keys 와도 일치해야 한다.
export function serviceScopeKeys(t: ConfigTemplate | null): string[] {
 if (!t) return []
 const out: string[] = []
 for (const s of t.sections) {
 for (const f of s.fields) {
 if (effectiveScope(f, s.scope) === 'service') out.push(f.key)
    }
  }
 return out
}
