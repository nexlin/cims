//  GroupConfigCompareView — 그룹 선택 + [패키지 설정] 탭 = 공통(service) 설정의 편집 창구 (R4).
//
//  AS 그룹:
//    · [동기화 ON/OFF 스위치] (그룹×패키지) — 백엔드 자동 교정 데몬(실측 ACTIVE 기준
//      STANDBY 교정, 이벤트+주기)을 켜고 끈다. 기본 ON.
//    · [공통 설정] 편집 — ON: 저장=전 멤버 적용 / OFF: 멤버 선택 후 그 멤버에만 저장
//      (업그레이드 창에서 새 버전 멤버의 설정 경로).
//    · 공통(service) 컬렉션 편집 — ON 이면 저장 직후 나머지 멤버로 즉시 전파.
//    · [멤버 비교] — 멤버별 값 나란히 비교 (공통+동일=정상 / 공통+상이=드리프트 /
//      개별=중립). 드리프트는 스위치 ON 이면 자동 교정이 곧 해소.
//  AA 그룹: 동기화 개념 없음 — 비교 표만 (편집은 각 서버의 설정 탭).
//
//  **드리프트 판정은 이 화면이 하지 않는다** — 서버(GET .../packages/{pkg}/sync)가
//  자동 교정과 같은 규칙으로 낸 status/drift 를 표시만 한다. 값(표)과 판정(드리프트)은
//  모두 "어느 패키지의 것인지" 태그와 함께 보관해, 탭 전환 대기 창에서 이전 패키지의
//  데이터가 새 템플릿에 얹히지 않게 한다. 정본: oam_base_service_split.md §14.6.
//
//  서버 개별(scope=system) 설정은 여기 없음 — 각 서버 선택 → [패키지 설정] 탭.
import { AlertTriangle } from 'lucide-react'
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useToast } from '../Toast'
import {
 deploymentApi, effectiveScope,
 type Deployment, type SipPackage, type ConfigTemplateField,
 type ConfigTemplateSection,
} from '../../api/deployment'
import { haGroupsApi, type HaGroup, type GroupPkgSync,
 type GroupPkgSyncMember } from '../../api/ha_groups'
import ModuleConfigEditor from '../module/ModuleConfigEditor'
import { StatusDot } from '../custom/status-dot'
import { Radio } from '../custom/radio'
import { StickySaveBar } from '../custom/sticky-save-bar'
import { Alert } from '../ui/alert'
import { EmptyState } from '../custom/empty-state'
import { Badge } from '../ui/badge'
import { SyncStatusRow } from '../custom/sync-status-row'
import { SubSection } from '../custom/collapsible-section'
import { DataTable, Td, Th } from '../custom/data-table'
import { ToggleGroup, ToggleGroupItem } from '../ui/toggle-group'
import {
  SectionBlock, StoreMigrateFooter, defaultValue, serviceScopeKeys, fieldValueEq, type FieldValue,
} from '../module/ModuleConfigModal'

interface Props {
 group: HaGroup
 members: Array<{ id: number; name: string }>
 deployments: Deployment[]
 packages: SipPackage[]
  // 셀/헤더 클릭 → 해당 서버의 패키지 설정 화면으로 점프
 onSelectMember: (agentId: number, packageName?: string) => void
}

type CellState = 'ok' | 'drift' | 'individual'
type View = 'edit' | 'compare' | string   // string = collection.key

// 값의 출처 — 왜 "같아 보이는데 드리프트"인지 설명하는 근거.
const SRC_HINT: Record<string, string> = {
 overlay:  '이 서버에 지정된 값 (deployment overlay)',
 injected: '배포 시 base 가 주입한 값 — overlay 에는 없지만 노드 config.json 에는 들어간다',
 default:  'overlay 미설정 → 템플릿 기본값. 값이 같아 보여도 지정된 값이 아니라 교정 대상이다',
}

// 서버가 "판정 불가"로 돌려준 사유 → 운영자 문구. 판정은 애매하면 하지 않는다(오방향 교정 방지).
const SYNC_REASON: Record<string, string> = {
 active_unknown:           'ACTIVE 미확정 (heartbeat·VIP 관측 대기)',
 version_mismatch:         '버전 혼재 — 버전이 같아지면 자동 판정',
 no_peers:                 '비교할 멤버 없음 (단일 배포)',
 active_has_no_deployment: 'ACTIVE 노드에 이 패키지 미배포',
 package_not_deployed:     '그룹에 이 패키지 배포 없음',
}

export function GroupConfigCompareView({ group, members: liveMembers,
 deployments: liveDeployments, packages: livePackages, onSelectMember }: Props) {
 const { show } = useToast()
  // 부모 폴링의 prop identity churn 차단 — 열린 시점 스냅샷 (새로고침 버튼으로 갱신).
  // group 은 스위치/ACTIVE 실시간 표시를 위해 live 사용 (필드 단위로만 참조).
 const [frozen] = useState(() => ({
 members: liveMembers, deployments: liveDeployments, packages: livePackages,
  }))
 const { members, deployments, packages } = frozen
 const isAS = group.mode === 'active_standby'

 const [selectedPkgName, setSelectedPkgName] = useState<string>('')
 const [view, setView] = useState<View>(isAS ? 'edit' : 'compare')
 const [loading, setLoading] = useState(false)
  // agent_id → config overlay (멤버별 GET /deployments/{id}/config 병렬 합성) +
  // 서버 정합 판정. 둘 다 **어느 패키지의 것인지 태그와 함께** 보관한다 — 태그 없이 두면
  // 탭 전환 직후(fetch 대기 창) 이전 패키지의 값·판정이 새 패키지 템플릿에 얹혀 렌더된다.
  // 응답 역전(느린 이전 요청이 나중에 도착)도 태그 불일치로 자동 무시된다.
 const [configs, setConfigs] =
 useState<{ pkg: string; map: Map<number, Record<string, unknown>> } | null>(null)
 const [sync, setSync] = useState<{ pkg: string; data: GroupPkgSync } | null>(null)
  // OFF 모드 멤버 선택 편집 대상 (agent_id)
 const [offTarget, setOffTarget] = useState<number | null>(null)
  // 스위치 토글 직후 group prop 폴링 반영 전까지의 낙관적 상태
 const [switchOverride, setSwitchOverride] = useState<Record<string, boolean>>({})
  // 공통 설정 편집 폼
 const [formValues, setFormValues]   = useState<Record<string, FieldValue>>({})
 const [formInitial, setFormInitial] = useState<Record<string, FieldValue>>({})
 const [saving, setSaving] = useState(false)
 const [toggling, setToggling] = useState(false)

 const memberIds = useMemo(() => new Set(members.map(m => m.id)), [members])
  // 패키지는 이름 단위 — 롤링 업그레이드 중 버전(=package_id)이 달라도 같은 화면.
 const groupPkgNames = useMemo(() => {
 const names: string[] = []
 for (const d of deployments) {
 if (memberIds.has(d.agent_id) && d.package_name && !names.includes(d.package_name)) {
 names.push(d.package_name)
      }
    }
 return names
  }, [deployments, memberIds])
 const effectivePkgName = selectedPkgName || groupPkgNames[0] || ''

 const memberDepsForPkg = useMemo(
    () => deployments.filter(d => memberIds.has(d.agent_id) && d.package_name === effectivePkgName),
 [deployments, memberIds, effectivePkgName]
  )
 const depByAgent = useMemo(() => {
 const m = new Map<number, Deployment>()
 for (const d of memberDepsForPkg) m.set(d.agent_id, d)
 return m
  }, [memberDepsForPkg])

 const deployedMembers = members.filter(m => depByAgent.has(m.id))
 const undeployedMembers = members.filter(m => !depByAgent.has(m.id))
 const deployedIds = deployedMembers.map(m => m.id)

 const memberVersions = useMemo(() => {
 const s = new Set<string>()
 for (const d of memberDepsForPkg) s.add(d.package_version || '?')
 return [...s]
  }, [memberDepsForPkg])
 const mixedVersions = memberVersions.length > 1

  // ── 스위치·ACTIVE (AS 전용, group prop live) ──
 const autoSyncOn = isAS
    ? (switchOverride[effectivePkgName] ?? group.auto_sync?.[effectivePkgName] ?? true)
    : false
 const activeAid = isAS ? (group.active_agent_id ?? null) : null
 const activeMember = activeAid != null ? members.find(m => m.id === activeAid) : undefined

  // 편집 기준 멤버 — ON: ACTIVE(배포됨) 우선, 없으면 첫 배포 멤버.
  //                 OFF: 선택 멤버 (기본 첫 배포 멤버).
  // ON 모드 기준은 최초 판정값으로 **고정** — group.active_agent_id 는 절체/플랩으로
  // 요동하는 live 값이라 그대로 따라가면 편집 중 컬렉션 편집기(editorSource 의
  // deploymentId)가 다른 멤버 배포로 갈아타며 리로드된다 (추가 행 닫힘 + 스피너로
  // 내용 붕괴 → 스크롤 맨 위 리셋). 저장은 그룹 전체 적용이라 기준이 낡아도 무해
  // (비교/드리프트 표시용). 패키지 전환 시 재판정.
 const autoBaseRef = useRef<number | null>(null)
  {
 const cand = activeAid != null && depByAgent.has(activeAid) ? activeAid : (deployedIds[0] ?? null)
 if (autoBaseRef.current == null || !depByAgent.has(autoBaseRef.current)) autoBaseRef.current = cand
  }
 const baseAgentId = !isAS ? null
    : autoSyncOn
      ? autoBaseRef.current
      : (offTarget != null && depByAgent.has(offTarget) ? offTarget : deployedIds[0] ?? null)
 const baseDep = baseAgentId != null ? depByAgent.get(baseAgentId) : undefined
 const basePkg = baseDep ? packages.find(p => p.id === baseDep.package_id) : undefined
 const template = basePkg?.config_template

  // ModuleConfigEditor 의 memo(prev.source === next.source)가 성립하도록 identity 고정 —
  // 인라인 리터럴이면 부모 폴링 리렌더마다 refetch 되어 편집 중 입력이 리셋된다
  // (ModuleConfigModal/ServicesPage 와 동일 관용).
 const editorSource = useMemo(
    () => baseDep ? { type: 'deployment' as const, deploymentId: baseDep.id } : null,
 [baseDep?.id])

  // 유효 scope=service 필드/섹션/컬렉션 (백엔드 마스크와 동일 규칙)
 const syncKeys = useMemo(
    () => new Set(serviceScopeKeys(template ?? null)), [template])
 const svcSections = useMemo(() => {
 if (!template) return [] as ConfigTemplateSection[]
 return template.sections
      .map(sec => {
 const fields = sec.fields.filter(f => effectiveScope(f, sec.scope) === 'service')
 return fields.length ? { ...sec, fields } : null
      })
      .filter((s): s is ConfigTemplateSection => !!s)
  }, [template])
 const svcCollections = useMemo(
    () => (template?.collections || []).filter(c => (c.scope ?? 'service') === 'service'),
 [template])

  // 진행 중 요청 식별자 — 응답 역전 가드. 태그(pkg)만으로는 "늦게 도착한 이전 요청이
  // 최신 응답을 덮어쓰고, 태그 불일치로 화면이 로딩에 머무는" 경우를 막지 못한다.
 const reqIdRef = useRef(0)

 const load = useCallback(async () => {
 const pkg = effectivePkgName
 const req = ++reqIdRef.current
 if (memberDepsForPkg.length === 0) { setConfigs(null); setSync(null); return }
 setLoading(true)
 try {
      // 표(값)와 판정(드리프트)을 같은 라운드에서 가져온다 — 판정은 서버 소유라
      // 실패해도 값 표시는 살린다(구 OAM 호환: 라우트 없으면 드리프트 표시만 빠짐).
 const [views, sv] = await Promise.all([
        Promise.all(memberDepsForPkg.map(d => deploymentApi.getDeploymentConfig(d.id))),
 haGroupsApi.getGroupPkgSync(group.id, pkg).catch(() => null),
      ])
 if (reqIdRef.current !== req) return   // 더 새 요청이 떴다 — 이 응답은 폐기
 const m = new Map<number, Record<string, unknown>>()
 memberDepsForPkg.forEach((d, i) => m.set(d.agent_id, views[i].config || {}))
 setConfigs({ pkg, map: m })
 setSync(sv ? { pkg, data: sv } : null)
    } catch (e) {
 if (reqIdRef.current === req) show((e as Error).message, 'err')
    } finally {
 if (reqIdRef.current === req) setLoading(false)
    }
  }, [memberDepsForPkg, effectivePkgName, group.id, show])

 useEffect(() => { void load() }, [load])
 useEffect(() => {   // 패키지 전환 시 뷰/선택/폼 초기화 (dirty 해제 → 새 기준으로 재초기화)
 setView(isAS ? 'edit' : 'compare')
 setOffTarget(null)
 setFormValues({})
 setFormInitial({})
 autoBaseRef.current = null   // ON 모드 기준 멤버 재판정
  }, [effectivePkgName, isAS])

  // 현재 패키지의 것일 때만 유효 — 태그가 다르면 아직 로딩 중으로 취급한다.
 const configView = configs && configs.pkg === effectivePkgName ? configs.map : null
 const syncView   = sync && sync.pkg === effectivePkgName ? sync.data : null

  // 멤버별 실효값 — overlay 값 없으면 template default (fromDefault 표시용)
 const effective = useCallback((agentId: number, f: ConfigTemplateField):
      { v: FieldValue; fromDefault: boolean } => {
 const c = configView?.get(agentId)
 const v = c?.[f.key]
 if (v === undefined) return { v: defaultValue(f), fromDefault: true }
 return { v: v as FieldValue, fromDefault: false }
  }, [configView])

  // 표시값도 서버가 계산한 **실효값**(overlay + 기본값 + 배포 시 주입)을 쓴다.
  // overlay 만 보고 그리면 판정(overlay 기준)과 표시 기준이 달라, 화면에는 같은 값이
  // 보이는데 드리프트로 표시되는 일이 생긴다 — src 배지로 그 차이를 드러낸다.
  // 서버 값이 아직 없으면(판정 보류·구 OAM) overlay 기준으로 폴백한다(표시 전용).
 const memberValues = useMemo(() => {
 const m = new Map<number, GroupPkgSyncMember['values']>()
 for (const mem of syncView?.members || []) m.set(mem.agent_id, mem.values)
 return m
  }, [syncView])

  // effect 에서 최신 dirty 여부를 deps 순환 없이 참조하기 위한 미러 ref
 const dirtyRef = useRef(false)

  // ── 공통 설정 폼 초기화 — 기준 멤버의 실효값 ──
  // dirty(미저장 편집) 중에는 재초기화하지 않는다 — 부모 폴링으로 live
  // group.active_agent_id 가 바뀌면(절체 등) baseAgentId 가 튀어 이 effect 가
  // 재실행되는데, 그때 편집 중이던 입력이 서버값으로 덮어써지던 것 방지.
  // 저장/패키지 전환으로 dirty 가 풀리면 다음 실행에서 새 기준으로 재초기화.
 useEffect(() => {
 if (!template || !configView || baseAgentId == null) return
 if (dirtyRef.current) return
    // 편집 폼도 비교 표와 **같은 기준**(서버가 계산한 실효값)으로 채운다. overlay 만
    // 보고 채우면 주입값(JWT 시크릿·store 경로 등)이 빈칸으로 보여 같은 화면 안에서
    // 표(실효값)와 폼(overlay)이 다른 값을 가리킨다. `default` 는 위젯 타입에 맞는
    // 템플릿 기본값을 쓴다(빈 기본값은 실효값에서 제외되므로 그대로 넣으면 위젯이 깨진다).
    // 서버 판정이 아직 없으면(구 OAM·판정 보류) overlay 기준 폴백.
 const base: Record<string, FieldValue> = {}
 for (const sec of svcSections) {
 for (const f of sec.fields) {
 const cell = memberValues.get(baseAgentId)?.[f.key]
 base[f.key] = cell
          ? (cell.src === 'default' ? defaultValue(f) : (cell.v as FieldValue))
          : effective(baseAgentId, f).v
      }
    }
 setFormValues(base)
 setFormInitial(base)
  }, [template, configView, baseAgentId, svcSections, effective, memberValues])

 const changed = useMemo(() => {
 const s = new Set<string>()
 for (const k of Object.keys(formValues)) {
 if (!fieldValueEq(formValues[k], formInitial[k])) s.add(k)
    }
 return s
  }, [formValues, formInitial])
 dirtyRef.current = changed.size > 0

  // 저장바 문구의 「재기동 필요 항목 포함」 — 바뀐 필드 중 재기동이 필요한 것이 있을 때만 적는다
  // (도안 G3-1 은 그 상태를 그렸다). `restart !== false` 가 기본 재기동이다.
 const restartNeeded = useMemo(() => {
 if (!template || changed.size === 0) return false
 for (const sec of template.sections)
 for (const f of sec.fields)
 if (changed.has(f.key) && f.restart !== false) return true
 return false
  }, [template, changed])

 async function saveForm() {
 if (!baseDep || changed.size === 0) return
 setSaving(true)
 try {
 const values: Record<string, unknown> = {}
 for (const k of changed) values[k] = formValues[k]
 const r = await haGroupsApi.putGroupPkgConfig(group.id, effectivePkgName, {
 values,
        ...(autoSyncOn ? {} : { target_deployment_id: baseDep.id }),
      })
 const jobs = r.members.map(m => `#${m.job_id}`).join(', ')
 show(autoSyncOn
        ? `저장됨 — 그룹 멤버 ${r.members.length}명 적용 (job ${jobs})`
        : `저장됨 — ${deployedMembers.find(m => m.id === baseAgentId)?.name} 에만 적용 (job ${jobs})`,
        'ok')
      // dirty 해제 — 저장된 값이 새 기준. (해제해야 load() 후 폼 재초기화 가드 통과)
 setFormInitial(formValues)
 dirtyRef.current = false
 await load()
    } catch (e) {
 show(`저장 실패: ${(e as Error).message}`, 'err')
    } finally {
 setSaving(false)
    }
  }

 async function toggleSwitch() {
 const next = !autoSyncOn
 setToggling(true)
 try {
 const r = await haGroupsApi.putGroupAutoSync(group.id, effectivePkgName, next)
 setSwitchOverride(p => ({ ...p, [effectivePkgName]: next }))
 if (!next) {
 show('동기화 OFF — 자동 교정 정지, 멤버별 편집 모드', 'ok')
      } else if (r.reconcile) {
 const rc = r.reconcile
 if (rc.status === 'synced') {
 show(`동기화 ON — 즉시 정합: ${rc.synced_keys.length + rc.removed_keys.length}개 필드 교정`, 'ok')
        } else if (rc.status === 'in_sync') {
 show('동기화 ON — 멤버 정합 확인됨', 'ok')
        } else {
 const why = rc.reason === 'version_mismatch' ? '버전 혼재 — 버전이 같아지면 자동 정합'
                    : rc.reason === 'active_unknown'   ? 'ACTIVE 판정 불가 — 판정되는 대로 자동 정합'
                    : rc.reason
 show(`동기화 ON — 정합 보류 (${why})`, 'ok')
        }
      }
 await load()
    } catch (e) {
 show(`스위치 전환 실패: ${(e as Error).message}`, 'err')
    } finally {
 setToggling(false)
    }
  }

  // 컬렉션 저장 직후 — ON 이면 나머지 멤버로 즉시 전파 (R3 sync 엔드포인트 재사용)
 const collectionSavedHook = useCallback((collKey: string) => async () => {
 if (!autoSyncOn || !baseDep) return
 const targets = deployedIds.filter(a => a !== baseAgentId)
      .map(a => depByAgent.get(a)!.id)
 if (targets.length === 0) return
 try {
 await deploymentApi.syncDeploymentConfig(baseDep.id, { targets, collections: [collKey] })
 show(`${collKey} — 그룹 멤버 전파 완료`, 'ok')
    } catch (e) {
 show(`멤버 전파 실패 (자동 교정이 재시도): ${(e as Error).message}`, 'err')
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [autoSyncOn, baseDep?.id, baseAgentId, deployedIds.join(','), depByAgent, show])

  // 드리프트 판정은 **서버 소유** — 자동 교정 데몬과 같은 규칙으로 낸 결과를 그대로 쓴다.
  // 여기서 멤버 값을 다시 비교하면 판정 주체가 둘이 되어 데몬이 손대지 않을 것을 드리프트로
  // 표시하게 된다 (정본: docs/design/features/oam_base_service_split.md §14.6).
 const driftKeys = useMemo(
    () => new Set((syncView?.drift || []).map(d => d.key)), [syncView])

 function cellState(f: ConfigTemplateField): CellState {
 if (!syncKeys.has(f.key)) return 'individual'
 return driftKeys.has(f.key) ? 'drift' : 'ok'
  }

 function memberValue(agentId: number, f: ConfigTemplateField):
      { v: FieldValue; src: 'overlay' | 'injected' | 'default' } {
 const cell = memberValues.get(agentId)?.[f.key]
 if (cell) return { v: cell.v as FieldValue, src: cell.src }
 const { v, fromDefault } = effective(agentId, f)
 return { v, src: fromDefault ? 'default' : 'overlay' }
  }

 function display(f: ConfigTemplateField, v: FieldValue): string {
 if (f.type === 'password') return v === null || v === undefined || v === '' ? '(빈 값)' : '●●●'
 if (v === null || v === undefined || v === '') return '(빈 값)'
 if (typeof v === 'boolean') return v ? 'true' : 'false'
 if (Array.isArray(v)) return v.join(', ')
 return String(v)
  }

 const summary = useMemo(() => {
 let ok = 0, drift = 0, individual = 0
 if (template) {
 for (const sec of template.sections) {
 for (const f of sec.fields) {
 if (!syncKeys.has(f.key)) individual++
 else if (driftKeys.has(f.key)) drift++
 else ok++
        }
      }
    }
 return { ok, drift, individual }
  }, [template, syncKeys, driftKeys])

 if (groupPkgNames.length === 0) {
    // ES-2 — 탭 본문 전체가 이 한 장으로 대체된다. 모듈 칩·세그먼트·저장바 모두 내지 않는다.
 return (
      <div className="p-4">
        <EmptyState title="그룹 멤버에 배포된 모듈 없음"
 description="[패키지 설치] 탭에서 모듈을 먼저 배포하세요." />
      </div>
    )
  }

 const baseMemberName = deployedMembers.find(m => m.id === baseAgentId)?.name

 return (
    <div className="flex h-full flex-col">
      {/* 모듈 칩 — 정본 Figma G3-1 `ModuleTabs`(262:4697). S3 와 같은 모양이고, 꼬리가
          설정 개수 대신 **동기화 상태 StatusDot** 이다(그룹 화면의 관심사). */}
      <div className="flex shrink-0 flex-wrap items-center gap-1.5 px-4 pb-2 pt-5">
        {groupPkgNames.map(name => {
 const active = name === effectivePkgName
 const vers = [...new Set(deployments
            .filter(d => memberIds.has(d.agent_id) && d.package_name === name)
            .map(d => d.package_version || '?'))]
 const on = isAS ? (switchOverride[name] ?? group.auto_sync?.[name] ?? true) : null
 return (
            <button key={name} onClick={() => setSelectedPkgName(name)}
 aria-pressed={active}
 className={`flex h-8 items-center gap-[7px] rounded-full border px-3 text-base font-semibold transition-colors ${
 active
                        ? 'border-primary bg-brandsoft text-brandsoft-on'
                        : 'border-border bg-card text-foreground hover:bg-accent'}`}>
              {name}
              <span className={`font-mono text-sm font-normal ${
 vers.length > 1 ? 'text-warning-on' : 'text-muted-foreground'}`}
 title={vers.length > 1 ? '멤버 간 버전 혼재' : undefined}>
 v{vers.join(' / v')}
              </span>
              {on !== null && (
                <StatusDot tone={on ? 'success' : 'warning'} label={on ? '동기화' : '수동'} />
              )}
            </button>
          )
        })}
      </div>

      {/* AS: 동기화 상태 줄 (459:7511). 구 화면은 초록/노랑 전폭 띠였는데 상시 켜져 있어
          경고로 읽히지 않았다 — 시안은 배지 + 상태 한 줄이고 **드리프트 0건이면 아무 색도
          쓰지 않는다.** */}
      {isAS && (
        <div className="shrink-0 px-4 pb-2">
          <SyncStatusRow
 syncOn={autoSyncOn} toggling={toggling} onToggle={() => void toggleSwitch()}
 activeNode={activeMember?.name ?? null}
 drift={syncView?.status === 'out_of_sync'
              ? `드리프트 ${summary.drift}건 — ${syncView.auto_sync ? '자동 교정 대기 중' : '동기화 OFF — 자동 교정 안 함'}`
              : undefined}
 extra={
              <>
                {mixedVersions && (
                  <span className="text-xs text-warning-on">
                    버전 혼재 (v{memberVersions.join(' / v')})
                  </span>
                )}
                {syncView?.status === 'unknown' && (
                  <span className="text-xs text-muted-foreground">
                    정합 판정 보류 — {SYNC_REASON[syncView.reason || ''] || syncView.reason}
                  </span>
                )}
              </>
            }
 onRefresh={() => void load()} refreshing={loading} />
        </div>
      )}

      {/* 뷰 세그먼트 (160:3188) — 구 화면의 파일 탭을 SegmentedItem 으로 */}
      <div className="shrink-0 px-4 pb-3">
        <ToggleGroup type="single" value={view} className="w-fit justify-start rounded-md bg-muted p-[3px]"
 onValueChange={(v: string) => v && setView(v)}>
          {isAS && (
            <ToggleGroupItem value="edit">
              공통 설정 ({svcSections.reduce((n, sec) => n + sec.fields.length, 0)})
            </ToggleGroupItem>
          )}
          {isAS && svcCollections.map(c => (
            <ToggleGroupItem key={c.key} value={c.key}>{c.title}</ToggleGroupItem>
          ))}
          <ToggleGroupItem value="compare">
            {/* 도안은 드리프트 수 앞에 경고 아이콘을 붙인다 (G3-1 `멤버 비교 (⚠1)`).
                0건이면 아이콘도 숫자도 그리지 않는다 — 0건에 경고색 금지(§3-7). */}
            멤버 비교 {summary.drift > 0 && (
              <>(<AlertTriangle size={11} className="mx-0.5 inline align-[-1px] text-warning-on" />{summary.drift})</>
            )}
          </ToggleGroupItem>
        </ToggleGroup>
      </div>

      <div className="flex-1 overflow-auto p-5">
        {!template ? (
          // A3 안내 그대로 (aa-group.md) — 모듈에 템플릿이 없으면 설정할 것이 없다
          <EmptyState title="이 패키지에는 config_template 이 없습니다 — 설정 항목 없음" />
        ) : view === 'edit' && isAS ? (
          /* ── 공통 설정 편집 ── */
 !configView ? <div className="flex min-h-0 flex-1 items-center justify-center text-center text-muted-foreground p-[20px]">로딩 중...</div> : (
            <>
              {/* 적용 범위 안내 (Figma G3-1 161:3105). 구 화면은 `🔗` 글리프 + 직접 칠한 상자였다. */}
              {autoSyncOn ? (
                <Alert variant={mixedVersions ? 'warning' : 'info'} className="mb-3">
                  <div className="font-medium">
                    저장하면 그룹 멤버 전체({deployedMembers.map(m => m.name).join(', ')})에 적용됩니다
                  </div>
                  <div className="mt-0.5 text-xs opacity-90">
                    표시값 기준: {baseMemberName}
                    {activeMember && baseAgentId === activeAid ? ' (ACTIVE)' : ''} ·
                    멤버 간 차이는 「멤버 비교」 에서 확인하세요.
                  </div>
                  {mixedVersions && (
                    <div className="mt-1.5 text-xs font-medium">
                      버전 혼재 중에는 그룹 일괄 저장이 차단됩니다 — 스위치 OFF 후 멤버별로 편집하세요.
                    </div>
                  )}
                </Alert>
              ) : (
                // 동기화 OFF 는 시안에 없는 상태다 — 기존 구성을 유지하되 껍데기만 시안 부품으로.
                <Alert variant="warning" className="mb-3">
                  <div className="font-medium">동기화 OFF — 저장은 선택한 멤버에만 적용됩니다</div>
                  <div className="mt-1.5 flex flex-wrap items-center gap-3">
                    <span className="text-xs">편집할 멤버:</span>
                    {deployedMembers.map(m => (
                      <label key={m.id} className="flex cursor-pointer select-none items-center gap-1.5 text-xs">
                        <Radio name="off-target" checked={baseAgentId === m.id}
 onChange={() => setOffTarget(m.id)} />
                        {m.name}
                        <span className="font-mono text-muted-foreground">
 v{depByAgent.get(m.id)?.package_version || '?'}
                        </span>
                      </label>
                    ))}
                  </div>
                </Alert>
              )}
              {svcSections.map(sec => (
                <SectionBlock key={`${baseAgentId}:${sec.key}`} section={sec}
 values={formValues} initial={formInitial} changed={changed}
 srcOf={(k) => (baseAgentId == null ? undefined : memberValues.get(baseAgentId)?.[k]?.src)}
 markerOf={(k) => (driftKeys.has(k)
                    ? <Badge variant="warningSoft" title="멤버 간 값이 다릅니다 — 「멤버 비교」 에서 확인">
                        드리프트
                      </Badge>
                    : undefined)}
 onChange={(k, v) => setFormValues(p => ({ ...p, [k]: v }))}
 onReset={(k) => setFormValues(p => ({ ...p, [k]: formInitial[k] }))}
 footer={sec.key === 'store'
                    ? <StoreMigrateFooter groupId={group.id}
 mountPoint={String(formValues['CimsRuntimeMount'] ?? '')}
 dirty={changed.has('CimsRuntimeMount') || changed.has('CimsRuntimeDir')}
 onDone={load} />
                    : undefined} />
              ))}

            </>
          )
        ) : view !== 'compare' && isAS ? (
          /* ── 공통 컬렉션 편집 (base 멤버 대상, ON 저장 시 즉시 전파) ── */
          (() => {
 const coll = svcCollections.find(c => c.key === view)
 if (!coll || !baseDep || !editorSource) return <EmptyState title="collection 을 찾을 수 없음" />
 return (
              <>
                <div className="text-sm text-muted-foreground mb-2.5">
                  편집 대상: <b>{baseMemberName}</b>
                  {autoSyncOn
                    ? ' — 저장 시 그룹 멤버 전체로 즉시 전파됩니다.'
                    : ' — 동기화 OFF: 이 멤버에만 저장됩니다.'}
                </div>
                <ModuleConfigEditor
 key={`${baseDep.id}:${coll.key}:${autoSyncOn}`}
 source={editorSource}
 collection={coll}
 onSaved={collectionSavedHook(coll.key)} />
              </>
            )
          })()
        ) : (
          /* ── 멤버 비교 표 ── */
 !configView ? <div className="flex min-h-0 flex-1 items-center justify-center text-center text-muted-foreground p-[20px]">로딩 중...</div> : (
            <>
              {/* 요약 — 시안 G3-2(162:2565)는 **배지 셋**이다(아이콘 없음).
                  0건에 경고색을 쓰지 않는다 (DESIGN-RULES §1-7). */}
              <div className="mb-3 flex flex-wrap items-center gap-1.5">
                <Badge variant="brandSoft">공통 일치 {summary.ok}</Badge>
                <Badge variant={summary.drift ? 'warningSoft' : 'neutralSoft'}>
                  드리프트 {summary.drift}
                </Badge>
                <Badge variant="neutralSoft">개별 {summary.individual}</Badge>
                {!isAS && (
                  <span className="text-xs text-muted-foreground">
                    AA 그룹 — 동기화 없음, 편집은 각 서버의 [패키지 설정] 탭
                  </span>
                )}
              </div>
              {undeployedMembers.length > 0 && (
                <div className="mb-3 text-sm text-muted-foreground">
                  미배포 멤버: {undeployedMembers.map(m => m.name).join(', ')}
                </div>
              )}
              {/* 섹션은 카드가 아니라 **Level 2 접힘 머리 + 레일 + 표** 다 (162:2565).
                  구 화면은 회색 머리띠를 얹은 카드였고 행 전체를 초록/노랑으로 칠했다 —
                  시안 표 계약은 「행 배경 tint 로 상태를 나타내지 않는다」(§Table). */}
              {template.sections.map(sec => (
                <SubSection key={sec.key} title={sec.title} hint={sec.description}>
                  <DataTable>
                    <thead>
                      <tr>
                        <Th width={240}>필드</Th>
                        <Th width={84} align="center">구분</Th>
                        {deployedMembers.map(m => (
                          <Th key={m.id} className="cursor-pointer"
 title={`${m.name} 의 설정 편집으로 이동`}
 onClick={() => onSelectMember(m.id, effectivePkgName)}>
                            <span className="inline-flex items-center gap-1.5">
                              {m.name}
                              {activeAid === m.id && (
                                <span className="size-1.5 rounded-full bg-success" title="ACTIVE" />
                              )}
                              <span className="font-mono font-normal">
 v{depByAgent.get(m.id)?.package_version || '?'}
                              </span>
                            </span>
                          </Th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {sec.fields.map(f => {
 const st = cellState(f)
 return (
                          <tr key={f.key}>
                            <Td className="align-top" title={f.key}>{f.label || f.key}</Td>
                            <Td align="center" className="align-top">
                              {st === 'individual' ? (
                                <Badge variant="neutralSoft"
 title="서버별 고유값 — 동기화 대상 아님">개별</Badge>
                              ) : st === 'drift' ? (
                                <Badge variant="warningSoft"
 title="멤버 간 값이 다르다 — 교정 대상">드리프트</Badge>
                              ) : (
                                <Badge variant="brandSoft"
 title="그룹 공통 — 멤버 간 일치">공통</Badge>
                              )}
                            </Td>
                            {deployedMembers.map(m => {
 const cell = memberValue(m.id, f)
 const muted = cell.src !== 'overlay'
 return (
                                <Td key={m.id} mono className="cursor-pointer align-top"
 title={SRC_HINT[cell.src]}
 onClick={() => onSelectMember(m.id, effectivePkgName)}>
                                  {display(f, cell.v)}
                                  {/* 출처는 값 아래 한 줄로 (시안은 `미설정` 을 둘째 줄에 둔다) */}
                                  {muted && (
                                    <div className="font-sans text-xs font-normal text-muted-foreground">
                                      {cell.src === 'injected' ? '주입' : '미설정'}
                                    </div>
                                  )}
                                </Td>
                              )
                            })}
                          </tr>
                        )
                      })}
                    </tbody>
                  </DataTable>
                </SubSection>
              ))}
            </>
          )
        )}
      </div>
      {/* 저장바 — 시안 G3-1(459:7310). 구 화면은 폼 안 우측 하단 버튼이라 긴 폼에서는
          스크롤을 끝까지 내려야 보였다. */}
      {isAS && view === 'edit' && template && (
        <StickySaveBar
 badge={changed.size > 0
            ? <Badge variant="warningSoft">변경 {changed.size}건</Badge>
            : <Badge variant="neutralSoft">변경 0건</Badge>}
 note={autoSyncOn
            ? `저장하면 멤버 ${deployedMembers.length}대의 ${effectivePkgName} 설정이 재생성되어 적용됩니다`
              + (restartNeeded ? ' · 재기동 필요 항목 포함' : '')
            : `동기화 OFF — ${baseMemberName} 에만 저장됩니다`}
 saveLabel={autoSyncOn ? '저장 — 전 멤버 적용' : `저장 — ${baseMemberName} 에만`}
 disabled={changed.size === 0 || (autoSyncOn && mixedVersions)}
 saving={saving}
 onRevert={() => setFormValues({ ...formInitial })}
 onSave={() => void saveForm()} />
      )}
    </div>
  )
}


export default GroupConfigCompareView
