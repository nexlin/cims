/**
 * GroupServiceConfigModal — HA 그룹 단위 서비스 설정 (정공법)
 *
 * 사용 맥락:
 *   HA 그룹 멤버의 "서비스 설정" (config_template 의 scope=service 항목) 은 양쪽
 *   멤버에 동일하게 유지되어야 함. 본 modal 은 그룹 단위로 한 번 편집하면 멤버
 *   전체에 자동 sync.
 *
 * 구성 (2026-06-10 개편):
 *   - 상단 **모듈 탭** — 그룹에 배포된 패키지(csp/csc/cmp...)별 탭. 한 모달에서
 *     여러 모듈의 그룹 공통 설정을 오가며 편집.
 *   - 좌측: [서비스 설정](scalar, scope=service 섹션) / [Preset] / 서비스 컬렉션들
 *   - **scope=service 항목만** 노출 — 멤버별 시스템 설정(scope=system; Local Node,
 *     _infra 등) 은 각 서버 모듈의 개별 [⚙ 설정] 에서. (동적 반영되는 서비스
 *     설정 전용 — 사용자 정책 2026-06-10)
 *
 * 동작:
 *   - 서비스 설정(scalar): 멤버[0] 의 config 로드 → scope=service 섹션 필드만
 *     편집 → 저장 시 멤버별 get+merge+PUT(propagate=false) — 멤버 고유 키
 *     (SystemId 등 scope=system overlay) 는 보존하면서 서비스 필드만 동기화.
 *   - Collection 탭들: ModuleConfigEditor (source.type='group') — 한 번 저장 시
 *     백엔드가 그룹 멤버 전체에 동시 PUT + SIGUSR1 reload.
 *   - Preset: preset.values 를 멤버별 merge + PUT (propagate=false).
 */
import { useCallback, useEffect, useMemo, useState } from 'react'
import { deploymentApi, type SipPackage, type Deployment,
         type ConfigTemplateCollection, type ConfigTemplateSection,
         type ConfigTemplatePreset } from '../../api/deployment'
import ModuleConfigEditor from '../module/ModuleConfigEditor'
import { SectionBlock, defaultValue, type FieldValue } from '../module/ModuleConfigModal'

interface Props {
  open: boolean
  onClose: () => void
  groupName: string
  members: Array<{ id: number; name: string }>
  deployments: Deployment[]
  packages: SipPackage[]
  // 표시용 — scope 필터에는 미사용 (그룹 설정 = scope=service 전용).
  haMode?: 'active_standby' | 'all_active' | 'standalone'
  onApplied?: () => Promise<void> | void
}

type Tab = { kind: 'scalar' } | { kind: 'preset' } | { kind: 'collection'; key: string }

export function GroupServiceConfigModal({ open, onClose, groupName, members, deployments, packages, haMode, onApplied }: Props) {
  const [selectedPkg, setSelectedPkg] = useState<number>(0)
  const [selectedPreset, setSelectedPreset] = useState<string>('')
  const [tab, setTab] = useState<Tab>({ kind: 'scalar' })
  const [status, setStatus] = useState<string>('')
  const [working, setWorking] = useState(false)

  const memberIds = useMemo(() => new Set(members.map(m => m.id)), [members])
  const groupPkgIds = useMemo(() => new Set(
    deployments.filter(d => memberIds.has(d.agent_id)).map(d => d.package_id)
  ), [deployments, memberIds])
  const groupPackages = useMemo(
    () => packages.filter(p => groupPkgIds.has(p.id)),
    [packages, groupPkgIds]
  )
  const effectivePkgId = selectedPkg || groupPackages[0]?.id || 0
  const pkg = groupPackages.find(p => p.id === effectivePkgId)
  const template = pkg?.config_template
  const presets = template?.presets || []

  // 그룹 설정 = 동적 반영되는 서비스(scope=service) 설정 전용 (2026-06-10 정책).
  // scope=system (Local Node 등) 은 멤버별 — 각 모듈의 개별 [⚙ 설정] 에서.
  // (A/S 의 system scope 도 그 경로의 PUT 이 자동으로 양 멤버 fan-out 하므로 정합 유지)
  const serviceCollections: ConfigTemplateCollection[] = useMemo(() => {
    const all = template?.collections || []
    return all.filter(c => c.scope === undefined || c.scope === 'service')
  }, [template])
  const serviceSections: ConfigTemplateSection[] = useMemo(
    () => (template?.sections || []).filter(s => s.scope === 'service'),
    [template])

  const memberDepsForPkg = useMemo(
    () => deployments.filter(d => memberIds.has(d.agent_id) && d.package_id === effectivePkgId),
    [deployments, memberIds, effectivePkgId]
  )
  const memberDeploymentIds = memberDepsForPkg.map(d => d.id)

  // ── 서비스 설정 (scalar) 상태 ──
  const [svcValues, setSvcValues]   = useState<Record<string, FieldValue>>({})
  const [svcInitial, setSvcInitial] = useState<Record<string, FieldValue>>({})
  const [svcLoading, setSvcLoading] = useState(false)
  const [showAdvanced, setShowAdvanced] = useState(false)
  const svcChanged = useMemo(() => {
    const s = new Set<string>()
    for (const k of new Set([...Object.keys(svcValues), ...Object.keys(svcInitial)])) {
      if (svcValues[k] !== svcInitial[k]) s.add(k)
    }
    return s
  }, [svcValues, svcInitial])
  const svcRestartRequired = useMemo(() => {
    for (const sec of serviceSections)
      for (const f of sec.fields)
        if (svcChanged.has(f.key) && (f.restart !== false)) return true
    return false
  }, [serviceSections, svcChanged])

  const firstDepId = memberDepsForPkg[0]?.id
  const loadScalar = useCallback(async () => {
    if (!template || !firstDepId) { setSvcValues({}); setSvcInitial({}); return }
    setSvcLoading(true)
    try {
      // 기준 = 멤버[0]. (서비스 필드는 멤버 간 동일해야 함 — 저장이 동기화를 보장)
      const view = await deploymentApi.getDeploymentConfig(firstDepId)
      const cfg = view.config || {}
      const base: Record<string, FieldValue> = {}
      for (const sec of serviceSections) {
        for (const f of sec.fields) {
          const existing = (cfg as Record<string, unknown>)[f.key]
          base[f.key] = existing !== undefined ? (existing as FieldValue) : defaultValue(f)
        }
      }
      setSvcValues(base)
      setSvcInitial(base)
    } catch {
      setSvcValues({}); setSvcInitial({})
    } finally {
      setSvcLoading(false)
    }
  }, [template, serviceSections, firstDepId])

  useEffect(() => { void loadScalar() }, [loadScalar])

  if (!open) return null

  async function saveScalar() {
    if (svcChanged.size === 0) return
    // 변경된 서비스 필드만 멤버별 merge — 멤버 고유 overlay 키 보존.
    const delta: Record<string, unknown> = {}
    for (const k of svcChanged) delta[k] = svcValues[k]
    setWorking(true)
    setStatus(`서비스 설정 저장 중 (${memberDepsForPkg.length} 멤버 × ${svcChanged.size} 필드)...`)
    let ok = 0; const errors: string[] = []
    for (const dep of memberDepsForPkg) {
      try {
        const view = await deploymentApi.getDeploymentConfig(dep.id)
        const merged = { ...(view.config ?? {}), ...delta }
        // propagate=false — 본 루프가 멤버 전체를 직접 순회 (멤버 고유 키 보존)
        await deploymentApi.putDeploymentConfig(dep.id, merged, true, false)
        ok++
      } catch (e) {
        errors.push(`deploy${dep.id}: ${(e as Error).message}`)
      }
    }
    setWorking(false)
    setStatus(errors.length === 0
      ? `✓ 서비스 설정 저장 (${ok}/${memberDepsForPkg.length} 멤버) — agent 가 SIGUSR1 reload.${svcRestartRequired ? ' ⚠ 재기동 필요 필드 포함 — 우측 하단 재기동.' : ''}`
      : `⚠ ${ok}/${memberDepsForPkg.length} 성공 — ${errors.slice(0, 2).join('; ')}`)
    setSvcInitial({ ...svcValues })
    if (onApplied) await onApplied()
  }

  async function applyPreset() {
    if (!pkg || !selectedPreset) return
    const preset = presets.find((p: ConfigTemplatePreset) => p.name === selectedPreset)
    if (!preset) return

    setWorking(true)
    setStatus(`${preset.label} 적용 중 (${memberDepsForPkg.length} 멤버)...`)

    let ok = 0, fail = 0
    const errors: string[] = []
    for (const dep of memberDepsForPkg) {
      try {
        const view = await deploymentApi.getDeploymentConfig(dep.id)
        const merged = { ...(view.config ?? {}), ...preset.values }
        // propagate=false — 멤버별 merge 결과가 서로 다른 고유 키를 보존해야 하므로
        // HA fan-out (마지막 멤버 값으로 전체 덮어쓰기) 비활성.
        await deploymentApi.putDeploymentConfig(dep.id, merged, true, false)
        ok++
      } catch (e) {
        fail++
        errors.push(`deploy${dep.id}: ${(e as Error).message}`)
      }
    }
    setWorking(false)
    setStatus(fail === 0
      ? `✓ preset 적용 + SIGUSR1 reload 큐잉 (${ok}/${memberDepsForPkg.length} 멤버). 부트스트랩 필드는 우측 하단 "재기동" 필요.`
      : `⚠ ${ok}/${memberDepsForPkg.length} 성공 — ${errors.slice(0, 2).join('; ')}`)
    if (onApplied) await onApplied()
    await loadScalar()
  }

  async function restartAll() {
    if (memberDepsForPkg.length === 0) return
    setWorking(true)
    setStatus(`재기동 큐잉 중 (${memberDepsForPkg.length} 멤버)...`)
    let ok = 0, fail = 0
    for (const dep of memberDepsForPkg) {
      try {
        await deploymentApi.queueJob(dep.id, 'restart')
        ok++
      } catch {
        fail++
      }
    }
    setWorking(false)
    setStatus(fail === 0
      ? `✓ 멤버 재기동 큐잉 완료 (${ok}/${memberDepsForPkg.length})`
      : `⚠ 재기동 ${ok}/${memberDepsForPkg.length} 큐잉`)
    if (onApplied) await onApplied()
  }

  function switchPkg(pid: number) {
    setSelectedPkg(pid)
    setSelectedPreset('')
    setTab({ kind: 'scalar' })
    setStatus('')
  }

  return (
    <div style={{
      position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.4)', zIndex: 1000,
      display: 'flex', alignItems: 'center', justifyContent: 'center',
    }}>
      <div style={{ background: 'white', padding: 0, borderRadius: 6,
                    width: '92vw', maxWidth: 1100, height: '88vh', display: 'flex', flexDirection: 'column' }}>
        {/* 헤더 */}
        <div style={{ padding: '14px 24px 0', borderBottom: '1px solid #e0e6ed' }}>
          <h3 style={{ margin: 0, fontSize: 18 }}>그룹 설정 — {groupName}
            {haMode && <span style={{ fontSize: 11, marginLeft: 8, color: 'var(--text-muted)', fontWeight: 'normal' }}>
              ({haMode === 'active_standby' ? 'A/S' : haMode === 'all_active' ? 'AA' : 'SA'} · 멤버 {members.length})
            </span>}
          </h3>
          <p style={{ color: 'var(--text-muted)', fontSize: 12, margin: '6px 0 10px' }}>
            동적으로 반영되는 <b>서비스 설정 (그룹 공통)</b> 전용 — 한 번 편집하면 멤버 전체에 자동 sync.
            서버별로 다른 시스템 설정 (Local Node, SystemId 등) 은 각 서버 모듈의 [⚙ 설정] 에서.
          </p>
          {/* 모듈 탭 */}
          <div style={{ display: 'flex', gap: 2 }}>
            {groupPackages.length === 0 && (
              <span style={{ fontSize: 12, color: 'var(--text-muted)', padding: '6px 0' }}>(그룹에 배포된 모듈 없음)</span>
            )}
            {groupPackages.map(p => {
              const active = p.id === effectivePkgId
              return (
                <button key={p.id} onClick={() => switchPkg(p.id)} disabled={working}
                        style={{
                          padding: '8px 18px', fontSize: 13, fontWeight: active ? 700 : 400,
                          background: active ? '#fff' : '#f0f3f6',
                          color: active ? '#1976d2' : '#555',
                          border: '1px solid #e0e6ed', borderBottom: active ? '2px solid #fff' : '1px solid #e0e6ed',
                          borderRadius: '6px 6px 0 0', cursor: 'pointer', marginBottom: -1,
                        }}>
                  {p.name} <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>v{p.version}</span>
                </button>
              )
            })}
          </div>
        </div>

        {/* 본문 — 좌측 탭 + 우측 패널 */}
        <div style={{ flex: 1, display: 'flex', minHeight: 0 }}>
          {/* 좌측 탭 사이드바 */}
          <div style={{ width: 240, borderRight: '1px solid #e0e6ed', padding: 12,
                        overflowY: 'auto', background: '#fafbfc' }}>
            <TabButton active={tab.kind === 'scalar'} onClick={() => setTab({ kind: 'scalar' })}>
              🛠 서비스 설정 ({serviceSections.reduce((n, s) => n + s.fields.length, 0)})
            </TabButton>
            <TabButton active={tab.kind === 'preset'} onClick={() => setTab({ kind: 'preset' })}>
              ✨ Preset 일괄 적용
            </TabButton>
            <div style={{ fontSize: 11, color: 'var(--text-muted)', margin: '12px 0 4px' }}>
              서비스 컬렉션 (그룹 공통)
            </div>
            {serviceCollections.map(c => (
              <TabButton key={c.key}
                         active={tab.kind === 'collection' && tab.key === c.key}
                         onClick={() => setTab({ kind: 'collection', key: c.key })}>
                {c.title}
              </TabButton>
            ))}
            {serviceCollections.length === 0 && (
              <div style={{ fontSize: 11, color: 'var(--text-muted)', padding: 8 }}>
                (scope=service collection 없음)
              </div>
            )}
          </div>

          {/* 우측 패널 */}
          <div style={{ flex: 1, padding: 16, overflow: 'auto' }}>
            {!pkg && (
              <div style={{ padding: 32, color: 'var(--text-muted)', textAlign: 'center' }}>
                그룹에 배포된 모듈 없음 — 멤버 서버에 모듈 추가 후 진입.
              </div>
            )}
            {pkg && tab.kind === 'scalar' && (
              <div>
                <h4 style={{ marginTop: 0 }}>
                  서비스 설정 (그룹 공통 · scalar)
                  <span style={{ fontSize: 11, marginLeft: 8, fontWeight: 'normal', color: '#27ae60' }}>
                    · ⚡ 저장 시 멤버 전체 sync + SIGUSR1 reload (🔁 표시 필드만 재기동 필요)
                  </span>
                </h4>
                {memberDeploymentIds.length === 0 ? (
                  <div style={{ color: 'var(--text-muted)' }}>그룹에 배포된 멤버 없음</div>
                ) : svcLoading ? (
                  <div className="empty" style={{ padding: 24 }}>로딩 중...</div>
                ) : serviceSections.length === 0 ? (
                  <div style={{ color: 'var(--text-muted)', fontSize: 13 }}>
                    이 모듈의 config_template 에 scope=service 섹션이 없습니다.
                  </div>
                ) : (
                  <>
                    <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 10,
                                  display: 'flex', alignItems: 'center', gap: 10 }}>
                      <span>기준값: {memberDepsForPkg[0] && (members.find(m => m.id === memberDepsForPkg[0].agent_id)?.name || `deploy#${memberDepsForPkg[0].id}`)}</span>
                      <label style={{ marginLeft: 'auto', cursor: 'pointer' }}>
                        <input type="checkbox" checked={showAdvanced}
                               onChange={e => setShowAdvanced(e.target.checked)} />
                        {' '}고급 설정
                      </label>
                    </div>
                    {serviceSections.map(sec => (
                      <SectionBlock key={sec.key} section={sec} values={svcValues}
                        initial={svcInitial} changed={svcChanged} showAdvanced={showAdvanced}
                        onChange={(k, v) => setSvcValues(p => ({ ...p, [k]: v }))}
                        onReset={(k) => setSvcValues(p => ({ ...p, [k]: svcInitial[k] }))} />
                    ))}
                    <div style={{ marginTop: 14, display: 'flex', gap: 8, alignItems: 'center' }}>
                      <button onClick={saveScalar}
                              disabled={working || svcChanged.size === 0}
                              style={{
                                background: working || svcChanged.size === 0 ? '#aaa' : '#1976d2',
                                color: 'white', padding: '8px 18px', fontSize: 13,
                                borderRadius: 4, border: 'none',
                                cursor: working || svcChanged.size === 0 ? 'not-allowed' : 'pointer',
                              }}>
                        {working ? '저장 중…' : `멤버 전체에 저장 (${svcChanged.size}개 변경)`}
                      </button>
                      {svcChanged.size > 0 && (
                        <button onClick={() => setSvcValues({ ...svcInitial })} disabled={working}
                                style={{ padding: '8px 12px', fontSize: 12 }}>되돌리기</button>
                      )}
                      {svcRestartRequired && (
                        <span style={{ fontSize: 12, color: '#e67e22' }}>⚠ 재기동 필요 필드 포함</span>
                      )}
                    </div>
                  </>
                )}
              </div>
            )}
            {pkg && tab.kind === 'preset' && (
              <div>
                <h4 style={{ marginTop: 0 }}>Preset 일괄 적용 <span style={{ fontSize: 11, color: '#27ae60', fontWeight: 'normal' }}>· ⚡ SIGUSR1 reload</span></h4>
                <p style={{ color: 'var(--text-muted)', fontSize: 12 }}>
                  preset 의 키-값을 멤버 전체 config.json 에 merge → PUT → agent 가 SIGUSR1
                  자동 발송. 부트스트랩 필드 (LocalIp, ThreadCount 등) 만 재기동 필요.
                </p>
                <div style={{ marginBottom: 16 }}>
                  <select value={selectedPreset}
                          onChange={e => setSelectedPreset(e.target.value)}
                          disabled={working || presets.length === 0}
                          style={{ width: '100%', padding: '6px 10px', fontSize: 13 }}>
                    <option value="">— preset 선택 —</option>
                    {presets.map(p => (
                      <option key={p.name} value={p.name}>
                        {p.label}{p.description ? ` — ${p.description}` : ''}
                      </option>
                    ))}
                  </select>
                </div>
                <button onClick={applyPreset}
                        disabled={!selectedPreset || working || memberDepsForPkg.length === 0}
                        style={{
                          background: !selectedPreset || working ? '#aaa' : '#1976d2',
                          color: 'white', padding: '8px 18px', fontSize: 13,
                          borderRadius: 4, border: 'none',
                          cursor: working || !selectedPreset ? 'not-allowed' : 'pointer',
                        }}>
                  {working ? '적용 중…' : '멤버 전체에 preset 적용'}
                </button>
              </div>
            )}
            {pkg && tab.kind === 'collection' && (() => {
              const c = serviceCollections.find(x => x.key === tab.key)
              if (!c) return <div>collection not found</div>
              if (memberDeploymentIds.length === 0) {
                return <div style={{ color: 'var(--text-muted)' }}>그룹에 배포된 멤버 없음</div>
              }
              return (
                <div>
                  <h4 style={{ marginTop: 0 }}>
                    {c.title}
                    <span style={{
                      fontSize: 11, marginLeft: 8, fontWeight: 'normal',
                      color: c.restart ? '#e67e22' : '#27ae60',
                    }}>
                      · {c.restart ? '재기동 필요' : `⚡ live reload (${c.reload_hint || 'SIGUSR1'})`}
                    </span>
                  </h4>
                  <p style={{ fontSize: 11, color: 'var(--text-muted)' }}>
                    저장 시 그룹 멤버 전체 ({memberDeploymentIds.length}개) 에 동시 PUT — 정합 보장.
                    {!c.restart && ' 별도 재기동 불필요 — agent 가 자동 SIGUSR1 reload.'}
                  </p>
                  <ModuleConfigEditor
                    source={{ type: 'group', deploymentIds: memberDeploymentIds }}
                    collection={c}
                  />
                </div>
              )
            })()}
          </div>
        </div>

        {/* 하단 상태 + 액션 */}
        <div style={{ padding: '12px 24px', borderTop: '1px solid #e0e6ed',
                      background: '#fafbfc', display: 'flex', alignItems: 'center', gap: 12 }}>
          {status && (
            <div style={{ flex: 1, padding: '6px 10px', background: '#fff',
                          borderRadius: 4, fontSize: 12, border: '1px solid #e0e6ed' }}>
              {status}
            </div>
          )}
          {!status && <div style={{ flex: 1 }} />}
          <button onClick={restartAll}
                  disabled={working || memberDepsForPkg.length === 0}
                  title="🔁 표시 필드/preset 변경 후에만 필요. collection 저장은 SIGUSR1 reload — 재기동 불필요."
                  style={{ padding: '6px 14px', fontSize: 12, cursor: working ? 'wait' : 'pointer',
                           color: 'var(--text-muted)', background: '#f5f5f5', border: '1px solid #ddd', borderRadius: 3 }}>
            {working ? '...' : `🔄 선택적 재기동 (${memberDepsForPkg.length})`}
          </button>
          <button onClick={onClose} disabled={working}
                  style={{ padding: '6px 14px', fontSize: 13 }}>
            닫기
          </button>
        </div>
      </div>
    </div>
  )
}

function TabButton({ active, onClick, children }: {
  active: boolean
  onClick: () => void
  children: React.ReactNode
}) {
  return (
    <button onClick={onClick}
            style={{
              display: 'block', width: '100%', textAlign: 'left',
              padding: '8px 12px', fontSize: 13, marginBottom: 4,
              background: active ? '#1976d2' : 'white',
              color: active ? 'white' : '#333',
              border: active ? 'none' : '1px solid #ddd',
              borderRadius: 4, cursor: 'pointer',
            }}>
      {children}
    </button>
  )
}
