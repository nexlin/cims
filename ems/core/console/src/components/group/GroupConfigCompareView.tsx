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
//  서버 개별(scope=system) 설정은 여기 없음 — 각 서버 선택 → [패키지 설정] 탭.
import { useCallback, useEffect, useMemo, useState } from 'react'
import { useToast } from '../Toast'
import {
  deploymentApi, effectiveScope,
  type Deployment, type SipPackage, type ConfigTemplateField,
  type ConfigTemplateSection,
} from '../../api/deployment'
import { haGroupsApi, type HaGroup } from '../../api/ha_groups'
import ModuleConfigEditor from '../module/ModuleConfigEditor'
import {
  SectionBlock, defaultValue, serviceScopeKeys, fieldValueEq, type FieldValue,
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
  // agent_id → config overlay (멤버별 GET /deployments/{id}/config 병렬 합성)
  const [configs, setConfigs] = useState<Map<number, Record<string, unknown>> | null>(null)
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
  const baseAgentId = !isAS ? null
    : autoSyncOn
      ? (activeAid != null && depByAgent.has(activeAid) ? activeAid : deployedIds[0] ?? null)
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

  const load = useCallback(async () => {
    if (memberDepsForPkg.length === 0) { setConfigs(null); return }
    setLoading(true)
    try {
      const views = await Promise.all(
        memberDepsForPkg.map(d => deploymentApi.getDeploymentConfig(d.id)))
      const m = new Map<number, Record<string, unknown>>()
      memberDepsForPkg.forEach((d, i) => m.set(d.agent_id, views[i].config || {}))
      setConfigs(m)
    } catch (e) {
      show((e as Error).message, 'err')
    } finally {
      setLoading(false)
    }
  }, [memberDepsForPkg, show])

  useEffect(() => { void load() }, [load])
  useEffect(() => {   // 패키지 전환 시 뷰/선택 초기화
    setView(isAS ? 'edit' : 'compare')
    setOffTarget(null)
  }, [effectivePkgName, isAS])

  // 멤버별 실효값 — overlay 값 없으면 template default (fromDefault 표시용)
  const effective = useCallback((agentId: number, f: ConfigTemplateField):
      { v: FieldValue; fromDefault: boolean } => {
    const c = configs?.get(agentId)
    const v = c?.[f.key]
    if (v === undefined) return { v: defaultValue(f), fromDefault: true }
    return { v: v as FieldValue, fromDefault: false }
  }, [configs])

  // ── 공통 설정 폼 초기화 — 기준 멤버의 실효값 ──
  useEffect(() => {
    if (!template || !configs || baseAgentId == null) return
    const base: Record<string, FieldValue> = {}
    for (const sec of svcSections) {
      for (const f of sec.fields) base[f.key] = effective(baseAgentId, f).v
    }
    setFormValues(base)
    setFormInitial(base)
  }, [template, configs, baseAgentId, svcSections, effective])

  const changed = useMemo(() => {
    const s = new Set<string>()
    for (const k of Object.keys(formValues)) {
      if (!fieldValueEq(formValues[k], formInitial[k])) s.add(k)
    }
    return s
  }, [formValues, formInitial])

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

  function cellState(f: ConfigTemplateField, ids: number[]): CellState {
    if (!syncKeys.has(f.key)) return 'individual'
    if (ids.length < 2) return 'ok'
    const first = JSON.stringify(effective(ids[0], f).v)
    return ids.every(aid => JSON.stringify(effective(aid, f).v) === first) ? 'ok' : 'drift'
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
    const driftFields: string[] = []
    if (template && configs) {
      for (const sec of template.sections) {
        for (const f of sec.fields) {
          const st = cellState(f, deployedIds)
          if (st === 'drift') { drift++; driftFields.push(f.label || f.key) }
          else if (st === 'ok' && syncKeys.has(f.key)) ok++
          else individual++
        }
      }
    }
    return { ok, drift, individual, driftFields }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [template, configs, syncKeys, deployedIds.join(',')])

  const stateStyle: Record<CellState, React.CSSProperties> = {
    ok:         { background: '#f0f9f1' },
    drift:      { background: '#fff3e0' },
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
                      color: active ? '#1976d2' : 'var(--text-muted)',
                      border: '1px solid var(--border)', borderBottom: 'none',
                      borderRadius: '6px 6px 0 0', cursor: 'pointer',
                    }}>
              {name} <span style={{ fontSize: 10,
                                    color: vers.length > 1 ? '#e67e22' : undefined }}>
                v{vers.join(' / v')}
              </span>
              {on !== null && (
                <span style={{ marginLeft: 5, fontSize: 10,
                               color: on ? '#1e7d34' : '#e67e22' }}>
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
                      background: autoSyncOn ? '#f4fbf5' : '#fdf6ec' }}>
          <label style={{ display: 'flex', alignItems: 'center', gap: 6,
                          cursor: toggling ? 'wait' : 'pointer', userSelect: 'none',
                          fontWeight: 700,
                          color: autoSyncOn ? '#1e7d34' : '#e67e22' }}
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
              ⚠ 버전 혼재 (v{memberVersions.join(' / v')}) — 자동 교정은 버전이 같아질 때까지 보류
            </span>
          )}
          {autoSyncOn && summary.drift > 0 && (
            <span style={{ color: '#e67e22' }}>⚠ 드리프트 {summary.drift}건 — 자동 교정 대기 중</span>
          )}
        </div>
      )}

      {/* 내부 뷰 탭 */}
      <div style={{ flex: '0 0 auto', display: 'flex', gap: 0, padding: '0 16px',
                    borderBottom: '1px solid #eee', background: '#fafbfc' }}>
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
          !configs ? <div className="empty" style={{ padding: 20 }}>로딩 중...</div> : (
            <>
              {autoSyncOn ? (
                <div style={{ padding: 10, background: '#e8f0fe', border: '1px solid #b8d4f5',
                              borderRadius: 4, fontSize: 12, marginBottom: 12 }}>
                  🔗 저장하면 그룹 멤버 <b>전체({deployedMembers.map(m => m.name).join(', ')})</b>에
                  적용됩니다. 표시값 기준: <b>{baseMemberName}</b>
                  {activeMember && baseAgentId === activeAid ? ' (ACTIVE)' : ''}
                  {mixedVersions && (
                    <div style={{ marginTop: 6, color: '#c0392b' }}>
                      ⚠ 버전 혼재 중에는 그룹 일괄 저장이 차단됩니다 — 스위치 OFF 후 멤버별로 편집하세요.
                    </div>
                  )}
                </div>
              ) : (
                <div style={{ padding: 10, background: '#fdf6ec', border: '1px solid #f0c987',
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
                  onChange={(k, v) => setFormValues(p => ({ ...p, [k]: v }))}
                  onReset={(k) => setFormValues(p => ({ ...p, [k]: formInitial[k] }))} />
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
          !configs ? <div className="empty" style={{ padding: 20 }}>로딩 중...</div> : (
            <>
              <div style={{ fontSize: 12, marginBottom: 12, display: 'flex', gap: 12,
                            alignItems: 'center', flexWrap: 'wrap' }}>
                <span style={{ color: '#1e7d34' }}>🔗 공통 일치 {summary.ok}</span>
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
                <div key={sec.key} style={{ border: '1px solid #e5e5e5', borderRadius: 6,
                                            marginBottom: 12, background: '#fff', overflow: 'hidden' }}>
                  <div style={{ padding: '10px 14px', background: '#fafafa',
                                borderBottom: '1px solid #eee',
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
                                                  cursor: 'pointer', color: '#1976d2' }}
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
                        const st = cellState(f, deployedIds)
                        return (
                          <tr key={f.key} style={{ borderTop: '1px solid #eee', ...stateStyle[st] }}>
                            <td style={{ padding: '6px 14px' }} title={f.key}>
                              {f.label || f.key}
                            </td>
                            <td style={{ textAlign: 'center' }}>
                              {syncKeys.has(f.key)
                                ? (st === 'drift'
                                    ? <span title="공통이어야 하는데 멤버 간 값 상이" style={{ color: '#e67e22' }}>⚠</span>
                                    : <span title="그룹 공통 — 멤버 간 값 동일" style={{ color: '#1e7d34' }}>🔗</span>)
                                : <span title="서버별 고유값 — 동기화 대상 아님" style={{ fontSize: 10, color: 'var(--text-muted)' }}>개별</span>}
                            </td>
                            {deployedMembers.map(m => {
                              const { v, fromDefault } = effective(m.id, f)
                              return (
                                <td key={m.id}
                                    style={{ padding: '6px 10px', fontFamily: 'monospace',
                                             cursor: 'pointer',
                                             color: fromDefault ? 'var(--text-muted)' : undefined,
                                             fontStyle: fromDefault ? 'italic' : undefined }}
                                    title={fromDefault ? '템플릿 기본값 (overlay 미설정)' : undefined}
                                    onClick={() => onSelectMember(m.id, effectivePkgName)}>
                                  {display(f, v)}
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
        background: active ? '#fff' : 'transparent',
        borderBottom: `2px solid ${active ? '#3498db' : 'transparent'}`,
        fontWeight: active ? 600 : 400, cursor: 'pointer', fontSize: 13,
      }}>
      {children}
    </button>
  )
}

export default GroupConfigCompareView
