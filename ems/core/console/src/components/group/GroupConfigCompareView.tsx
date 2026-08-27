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

  const stateStyle: Record<CellState, React.CSSProperties> = {
    ok:         { background: 'var(--success-soft)' },
    drift:      { background: 'var(--warn-soft)' },
    individual: {},
  }

  if (groupPkgNames.length === 0) {
    return <div className="empty" style={{ padding: 40 }}>
      그룹 멤버에 배포된 모듈 없음 — [패키지 설치] 탭에서 모듈을 먼저 배포하세요
    </div>
  }

  const baseMemberName = deployedMembers.find(m => m.id === baseAgentId)?.name

  return (
    <div style={{ height: '100%', display: 'flex', flexDirection: 'column' }}>
      {/* 패키지 탭 (이름 단위 — 버전 혼재도 한 화면) */}
      <div style={{ flex: '0 0 auto', display: 'flex', gap: 2, padding: '10px 16px 0',
                    borderBottom: '1px solid var(--border)', background: 'var(--bg-soft)' }}>
        {groupPkgNames.map(name => {
          const active = name === effectivePkgName
          const vers = [...new Set(deployments
            .filter(d => memberIds.has(d.agent_id) && d.package_name === name)
            .map(d => d.package_version || '?'))]
          const on = isAS ? (switchOverride[name] ?? group.auto_sync?.[name] ?? true) : null
          return (
            <button key={name} onClick={() => setSelectedPkgName(name)}
                    style={{
                      padding: '8px 18px', fontSize: 13, fontWeight: active ? 700 : 400,
                      background: active ? 'var(--surface)' : 'transparent',
                      color: active ? 'var(--primary)' : 'var(--text-muted)',
                      border: '1px solid var(--border)', borderBottom: 'none',
                      borderRadius: '6px 6px 0 0', cursor: 'pointer',
                    }}>
              {name} <span style={{ fontSize: 10,
                                    color: vers.length > 1 ? '#e67e22' : undefined }}>
                v{vers.join(' / v')}
              </span>
              {on !== null && (
                <span style={{ marginLeft: 5, fontSize: 10,
                               color: on ? 'var(--success)' : '#e67e22' }}>
                  {on ? '⬤동기화' : '○수동'}
                </span>
              )}
            </button>
          )
        })}
        <button className="btn btn--sm" style={{ marginLeft: 'auto', marginBottom: 6 }}
                onClick={() => void load()} disabled={loading}>
          {loading ? '로딩...' : '↻ 새로고침'}
        </button>
      </div>

      {/* AS: 동기화 스위치 + ACTIVE 상태줄 */}
      {isAS && (
        <div style={{ flex: '0 0 auto', padding: '10px 16px', fontSize: 12,
                      display: 'flex', alignItems: 'center', gap: 14, flexWrap: 'wrap',
                      borderBottom: '1px solid var(--border)',
                      background: autoSyncOn ? 'var(--success-soft)' : 'var(--warn-soft)' }}>
          <label style={{ display: 'flex', alignItems: 'center', gap: 6,
                          cursor: toggling ? 'wait' : 'pointer', userSelect: 'none',
                          fontWeight: 700,
                          color: autoSyncOn ? 'var(--success)' : '#e67e22' }}
                 title={autoSyncOn
                   ? 'ON — ACTIVE 기준으로 STANDBY 공통 설정을 자동 교정 (이벤트+주기). 업데이트 작업 전 OFF 로 전환하세요.'
                   : 'OFF — 자동 교정 정지. 멤버별로 독립 편집 (업그레이드 창). 작업 완료 후 ON 으로.'}>
            <input type="checkbox" checked={autoSyncOn} disabled={toggling}
                   onChange={() => void toggleSwitch()} />
            동기화 {autoSyncOn ? 'ON' : 'OFF'}
          </label>
          <span>
            ACTIVE:&nbsp;
            {activeMember
              ? <b style={{ color: '#e67e22' }}>● {activeMember.name}</b>
              : <span style={{ color: 'var(--text-muted)' }}>판정 불가 (heartbeat 관측 대기)</span>}
          </span>
          {mixedVersions && (
            <span style={{ color: '#e67e22' }}>
              ⚠ 버전 혼재 (v{memberVersions.join(' / v')})
            </span>
          )}
          {/* 정합 상태 — 서버 판정(GET .../packages/{pkg}/sync)을 그대로 표시.
              화면이 자체 계산하면 자동 교정이 실제로 할 일과 어긋난다. */}
          {syncView?.status === 'out_of_sync' && (
            <span style={{ color: '#e67e22' }}>
              ⚠ 드리프트 {summary.drift}건 —{' '}
              {syncView.auto_sync ? '자동 교정 대기 중' : '동기화 OFF — 자동 교정 안 함'}
            </span>
          )}
          {syncView?.status === 'unknown' && (
            <span style={{ color: 'var(--text-muted)' }}>
              정합 판정 보류 — {SYNC_REASON[syncView.reason || ''] || syncView.reason}
            </span>
          )}
        </div>
      )}

      {/* 내부 뷰 탭 */}
      <div style={{ flex: '0 0 auto', display: 'flex', gap: 0, padding: '0 16px',
                    borderBottom: '1px solid var(--border)', background: 'var(--bg-soft)' }}>
        {isAS && (
          <ViewBtn active={view === 'edit'} onClick={() => setView('edit')}>
            공통 설정 ({svcSections.reduce((n, s) => n + s.fields.length, 0)})
          </ViewBtn>
        )}
        {isAS && svcCollections.map(c => (
          <ViewBtn key={c.key} active={view === c.key} onClick={() => setView(c.key)}>
            {c.title}
          </ViewBtn>
        ))}
        <ViewBtn active={view === 'compare'} onClick={() => setView('compare')}>
          멤버 비교 {summary.drift > 0 ? `(⚠${summary.drift})` : ''}
        </ViewBtn>
      </div>

      <div style={{ flex: 1, overflow: 'auto', padding: 20 }}>
        {!template ? (
          <div className="empty" style={{ padding: 20 }}>
            이 패키지에는 config_template 이 없습니다 — 설정 항목 없음
          </div>
        ) : view === 'edit' && isAS ? (
          /* ── 공통 설정 편집 ── */
          !configView ? <div className="empty" style={{ padding: 20 }}>로딩 중...</div> : (
            <>
              {autoSyncOn ? (
                <div style={{ padding: 10, background: 'var(--primary-soft)', border: '1px solid var(--border)',
                              borderRadius: 4, fontSize: 12, marginBottom: 12 }}>
                  🔗 저장하면 그룹 멤버 <b>전체({deployedMembers.map(m => m.name).join(', ')})</b>에
                  적용됩니다. 표시값 기준: <b>{baseMemberName}</b>
                  {activeMember && baseAgentId === activeAid ? ' (ACTIVE)' : ''}
                  {mixedVersions && (
                    <div style={{ marginTop: 6, color: 'var(--danger)' }}>
                      ⚠ 버전 혼재 중에는 그룹 일괄 저장이 차단됩니다 — 스위치 OFF 후 멤버별로 편집하세요.
                    </div>
                  )}
                </div>
              ) : (
                <div style={{ padding: 10, background: 'var(--warn-soft)', border: '1px solid var(--border)',
                              borderRadius: 4, fontSize: 12, marginBottom: 12,
                              display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
                  <span>○ 동기화 OFF — <b>편집할 멤버:</b></span>
                  {deployedMembers.map(m => (
                    <label key={m.id} style={{ display: 'flex', alignItems: 'center', gap: 4,
                                               cursor: 'pointer', userSelect: 'none' }}>
                      <input type="radio" name="off-target" checked={baseAgentId === m.id}
                             onChange={() => setOffTarget(m.id)} />
                      {m.name}
                      <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>
                        v{depByAgent.get(m.id)?.package_version || '?'}
                      </span>
                    </label>
                  ))}
                  <span style={{ color: 'var(--text-muted)' }}>저장은 선택한 멤버에만 적용됩니다.</span>
                </div>
              )}
              {svcSections.map(sec => (
                <SectionBlock key={`${baseAgentId}:${sec.key}`} section={sec}
                  values={formValues} initial={formInitial} changed={changed}
                  srcOf={(k) => (baseAgentId == null ? undefined : memberValues.get(baseAgentId)?.[k]?.src)}
                  onChange={(k, v) => setFormValues(p => ({ ...p, [k]: v }))}
                  onReset={(k) => setFormValues(p => ({ ...p, [k]: formInitial[k] }))}
                  footer={sec.key === 'store'
                    ? <StoreMigrateFooter groupId={group.id}
                        mountPoint={String(formValues['CimsRuntimeMount'] ?? '')}
                        dirty={changed.has('CimsRuntimeMount') || changed.has('CimsRuntimeDir')}
                        onDone={load} />
                    : undefined} />
              ))}
              <div style={{ display: 'flex', justifyContent: 'flex-end', marginTop: 12 }}>
                <button className="btn btn--primary" onClick={() => void saveForm()}
                        disabled={saving || changed.size === 0 || (autoSyncOn && mixedVersions)}>
                  {saving ? '저장 중...'
                    : autoSyncOn ? `저장 — 전 멤버 적용 (${changed.size} 변경)`
                                 : `저장 — ${baseMemberName} 에만 (${changed.size} 변경)`}
                </button>
              </div>
            </>
          )
        ) : view !== 'compare' && isAS ? (
          /* ── 공통 컬렉션 편집 (base 멤버 대상, ON 저장 시 즉시 전파) ── */
          (() => {
            const coll = svcCollections.find(c => c.key === view)
            if (!coll || !baseDep || !editorSource) return <div className="empty">collection 을 찾을 수 없음</div>
            return (
              <>
                <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 10 }}>
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
          !configView ? <div className="empty" style={{ padding: 20 }}>로딩 중...</div> : (
            <>
              <div style={{ fontSize: 12, marginBottom: 12, display: 'flex', gap: 12,
                            alignItems: 'center', flexWrap: 'wrap' }}>
                <span style={{ color: 'var(--success)' }}>🔗 공통 일치 {summary.ok}</span>
                <span style={{ color: summary.drift ? '#e67e22' : 'var(--text-muted)',
                               fontWeight: summary.drift ? 700 : 400 }}>
                  ⚠ 드리프트 {summary.drift}
                </span>
                <span style={{ color: 'var(--text-muted)' }}>개별 {summary.individual}</span>
                {!isAS && (
                  <span style={{ color: 'var(--text-muted)' }}>
                    · AA 그룹 — 동기화 없음, 편집은 각 서버의 [패키지 설정] 탭
                  </span>
                )}
              </div>
              {undeployedMembers.length > 0 && (
                <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 12 }}>
                  미배포 멤버: {undeployedMembers.map(m => m.name).join(', ')}
                </div>
              )}
              {template.sections.map(sec => (
                <div key={sec.key} style={{ border: '1px solid var(--border)', borderRadius: 6,
                                            marginBottom: 12, background: 'var(--surface)', overflow: 'hidden' }}>
                  <div style={{ padding: '10px 14px', background: 'var(--bg-soft)',
                                borderBottom: '1px solid var(--border)',
                                display: 'flex', alignItems: 'baseline', gap: 8 }}>
                    <b>{sec.title}</b>
                    {sec.description && (
                      <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>— {sec.description}</span>
                    )}
                  </div>
                  <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
                    <thead>
                      <tr style={{ color: 'var(--text-muted)' }}>
                        <th style={{ textAlign: 'left', padding: '6px 14px', width: 240 }}>필드</th>
                        <th style={{ width: 70, textAlign: 'center' }}>구분</th>
                        {deployedMembers.map(m => (
                          <th key={m.id} style={{ textAlign: 'left', padding: '6px 10px',
                                                  cursor: 'pointer', color: 'var(--primary)' }}
                              title={`${m.name} 의 설정 편집으로 이동`}
                              onClick={() => onSelectMember(m.id, effectivePkgName)}>
                            {m.name}{activeAid === m.id ? ' ●' : ''}
                            <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>
                              {' '}v{depByAgent.get(m.id)?.package_version || '?'}
                            </span> ↗
                          </th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {sec.fields.map(f => {
                        const st = cellState(f)
                        return (
                          <tr key={f.key} style={{ borderTop: '1px solid var(--border)', ...stateStyle[st] }}>
                            <td style={{ padding: '6px 14px' }} title={f.key}>
                              {f.label || f.key}
                            </td>
                            <td style={{ textAlign: 'center' }}>
                              {syncKeys.has(f.key)
                                ? (st === 'drift'
                                    ? <span title="공통이어야 하는데 멤버 간 값 상이" style={{ color: '#e67e22' }}>⚠</span>
                                    : <span title="그룹 공통 — 멤버 간 값 동일" style={{ color: 'var(--success)' }}>🔗</span>)
                                : <span title="서버별 고유값 — 동기화 대상 아님" style={{ fontSize: 10, color: 'var(--text-muted)' }}>개별</span>}
                            </td>
                            {deployedMembers.map(m => {
                              const cell = memberValue(m.id, f)
                              const muted = cell.src !== 'overlay'
                              return (
                                <td key={m.id}
                                    style={{ padding: '6px 10px', fontFamily: 'monospace',
                                             cursor: 'pointer',
                                             color: muted ? 'var(--text-muted)' : undefined,
                                             fontStyle: muted ? 'italic' : undefined }}
                                    title={SRC_HINT[cell.src]}
                                    onClick={() => onSelectMember(m.id, effectivePkgName)}>
                                  {display(f, cell.v)}
                                  {muted && (
                                    <span style={{ fontSize: 10, marginLeft: 5, fontStyle: 'normal' }}>
                                      {cell.src === 'injected' ? '(주입)' : '(미설정)'}
                                    </span>
                                  )}
                                </td>
                              )
                            })}
                          </tr>
                        )
                      })}
                    </tbody>
                  </table>
                </div>
              ))}
            </>
          )
        )}
      </div>
    </div>
  )
}

function ViewBtn({ active, children, onClick }: {
  active: boolean; children: React.ReactNode; onClick: () => void
}) {
  return (
    <button onClick={onClick}
      style={{
        padding: '8px 16px', border: 'none',
        background: active ? 'var(--surface)' : 'transparent',
        borderBottom: `2px solid ${active ? '#3498db' : 'transparent'}`,
        fontWeight: active ? 600 : 400, cursor: 'pointer', fontSize: 13,
      }}>
      {children}
    </button>
  )
}

export default GroupConfigCompareView
