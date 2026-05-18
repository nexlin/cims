/**
 * GroupServiceConfigModal — HA 그룹 단위 서비스 설정 (정공법)
 *
 * 사용 맥락:
 *   HA 그룹 멤버의 "서비스 설정" (config_template 의 scope=service 항목) 은 양쪽
 *   멤버에 동일하게 유지되어야 함. 본 modal 은 그룹 단위로 한 번 편집하면 멤버
 *   전체에 자동 sync.
 *
 * scope 분류:
 *   - section.scope / collection.scope === "service" — 그룹 (본 modal 에서 편집)
 *   - section.scope / collection.scope === "system"  — 멤버별 (ServersPage 의 ModuleConfigEditor)
 *   - undefined — default "service" (보수적, 그룹 단위 가정. config_template 에 명시 권장)
 *
 * 동작:
 *   - Preset 탭: 선택 후 적용 → 양쪽 멤버 config 에 preset.values merge + PUT + restart
 *   - Collection 탭들: ModuleConfigEditor (source.type='group', deploymentIds=[...])
 *     → 한 번 편집 + 저장 시 양쪽 멤버에 동시 PUT (update_config job 자동 큐잉)
 *   - 하단 "양쪽 멤버 재기동" 버튼 — 모든 collection 저장 후 한 번에 restart
 */
import { useMemo, useState } from 'react'
import { deploymentApi, type SipPackage, type Deployment,
         type ConfigTemplateCollection } from '../../api/deployment'
import ModuleConfigEditor from '../module/ModuleConfigEditor'

interface Props {
  open: boolean
  onClose: () => void
  groupName: string
  members: Array<{ id: number; name: string }>
  deployments: Deployment[]
  packages: SipPackage[]
  // T3 (2026-05-18): HA mode. 'active_standby' 면 scope=system collection 도 양 멤버 동일
  //   (T4 의 scope 재정의 — VIP 모델). 'all_active' / 'standalone' 이면 scope=service 만.
  haMode?: 'active_standby' | 'all_active' | 'standalone'
  onApplied?: () => Promise<void> | void
}

type Tab = { kind: 'preset' } | { kind: 'collection'; key: string }

export function GroupServiceConfigModal({ open, onClose, groupName, members, deployments, packages, haMode, onApplied }: Props) {
  const [selectedPkg, setSelectedPkg] = useState<number>(0)
  const [selectedPreset, setSelectedPreset] = useState<string>('')
  const [tab, setTab] = useState<Tab>({ kind: 'preset' })
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

  // T4 (2026-05-18): scope 의미 재정의 후
  //  - scope = 'service' / undefined  → 항상 그룹 단위 (본 modal)
  //  - scope = 'system' + active_standby → VIP 모델 — 그룹 단위 fan-out
  //  - scope = 'system' + all_active     → 멤버별 (이 modal 미포함)
  const serviceCollections: ConfigTemplateCollection[] = useMemo(() => {
    const all = template?.collections || []
    return all.filter(c => {
      if (c.scope === undefined || c.scope === 'service') return true
      if (c.scope === 'system' && haMode === 'active_standby') return true
      return false
    })
  }, [template, haMode])

  const memberDepsForPkg = useMemo(
    () => deployments.filter(d => memberIds.has(d.agent_id) && d.package_id === effectivePkgId),
    [deployments, memberIds, effectivePkgId]
  )
  const memberDeploymentIds = memberDepsForPkg.map(d => d.id)

  if (!open) return null

  async function applyPreset() {
    if (!pkg || !selectedPreset) return
    const preset = presets.find(p => p.name === selectedPreset)
    if (!preset) return

    setWorking(true)
    setStatus(`${preset.label} 적용 중 (${memberDepsForPkg.length} 멤버)...`)

    let ok = 0, fail = 0
    const errors: string[] = []
    for (const dep of memberDepsForPkg) {
      try {
        const view = await deploymentApi.getDeploymentConfig(dep.id)
        const merged = { ...(view.config ?? {}), ...preset.values }
        await deploymentApi.putDeploymentConfig(dep.id, merged, true)
        ok++
      } catch (e) {
        fail++
        errors.push(`deploy${dep.id}: ${(e as Error).message}`)
      }
    }
    setWorking(false)
    // Phase D 이후: job_update_config 가 SIGUSR1 자동 발송 (cims_agent.py:job_update_config).
    // 단, **부트스트랩 필드** (Setup.Sip.LocalIp, UdpThreadCount 등) 는 이미 bound 된
    // socket/thread pool 에 반영 안 됨 — 그 경우만 재기동 필요.
    setStatus(fail === 0
      ? `✓ preset 적용 + SIGUSR1 reload 큐잉 (${ok}/${memberDepsForPkg.length} 멤버). 부트스트랩 필드는 우측 하단 "재기동" 필요.`
      : `⚠ ${ok}/${memberDepsForPkg.length} 성공 — ${errors.slice(0, 2).join('; ')}`)
    if (onApplied) await onApplied()
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
      ? `✓ 양쪽 재기동 큐잉 완료 (${ok}/${memberDepsForPkg.length})`
      : `⚠ 재기동 ${ok}/${memberDepsForPkg.length} 큐잉`)
    if (onApplied) await onApplied()
  }

  return (
    <div style={{
      position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.4)', zIndex: 1000,
      display: 'flex', alignItems: 'center', justifyContent: 'center',
    }}>
      <div style={{ background: 'white', padding: 0, borderRadius: 6,
                    width: '92vw', maxWidth: 1100, height: '88vh', display: 'flex', flexDirection: 'column' }}>
        {/* 헤더 */}
        <div style={{ padding: '16px 24px', borderBottom: '1px solid #e0e6ed' }}>
          <h3 style={{ margin: 0, fontSize: 18 }}>그룹 서비스 설정 — {groupName}</h3>
          <p style={{ color: '#666', fontSize: 12, margin: '6px 0 0' }}>
            그룹 멤버 ({members.length}개) 공통 설정. 한 번 편집 시 양쪽 멤버에 자동 sync.
            멤버 specific 값 (LocalIp 등) 은 각 서버의 시스템 설정에서.
          </p>
        </div>

        {/* 본문 — 좌측 탭 + 우측 패널 */}
        <div style={{ flex: 1, display: 'flex', minHeight: 0 }}>
          {/* 좌측 탭 사이드바 */}
          <div style={{ width: 240, borderRight: '1px solid #e0e6ed', padding: 12,
                        overflowY: 'auto', background: '#fafbfc' }}>
            <div style={{ marginBottom: 12 }}>
              <label style={{ fontSize: 11, color: '#888', display: 'block', marginBottom: 4 }}>패키지</label>
              <select value={effectivePkgId}
                      onChange={e => { setSelectedPkg(Number(e.target.value)); setSelectedPreset(''); setTab({ kind: 'preset' }) }}
                      disabled={working}
                      style={{ width: '100%', padding: '4px 8px', fontSize: 13 }}>
                {groupPackages.length === 0 && <option value={0}>(그룹에 패키지 없음)</option>}
                {groupPackages.map(p => (
                  <option key={p.id} value={p.id}>{p.name} {p.version}</option>
                ))}
              </select>
            </div>
            <TabButton active={tab.kind === 'preset'} onClick={() => setTab({ kind: 'preset' })}>
              ✨ Preset 일괄 적용
            </TabButton>
            <div style={{ fontSize: 11, color: '#888', margin: '12px 0 4px' }}>
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
              <div style={{ fontSize: 11, color: '#aaa', padding: 8 }}>
                (scope=service collection 없음)
              </div>
            )}
          </div>

          {/* 우측 패널 */}
          <div style={{ flex: 1, padding: 16, overflow: 'auto' }}>
            {!pkg && (
              <div style={{ padding: 32, color: '#999', textAlign: 'center' }}>
                그룹에 패키지 미배포 — `/deploy/services` 에서 패키지 추가 후 진입.
              </div>
            )}
            {pkg && tab.kind === 'preset' && (
              <div>
                <h4 style={{ marginTop: 0 }}>Preset 일괄 적용 <span style={{ fontSize: 11, color: '#27ae60', fontWeight: 'normal' }}>· ⚡ SIGUSR1 reload</span></h4>
                <p style={{ color: '#666', fontSize: 12 }}>
                  preset 의 키-값을 양쪽 멤버 config.json 에 merge → PUT → agent 가 SIGUSR1
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
                  {working ? '적용 중…' : '양쪽 멤버에 preset 적용'}
                </button>
              </div>
            )}
            {pkg && tab.kind === 'collection' && (() => {
              const c = serviceCollections.find(x => x.key === tab.key)
              if (!c) return <div>collection not found</div>
              if (memberDeploymentIds.length === 0) {
                return <div style={{ color: '#999' }}>그룹에 배포된 멤버 없음</div>
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
                  <p style={{ fontSize: 11, color: '#888' }}>
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
                  title="section/preset 변경 후에만 필요. collection 저장은 SIGUSR1 reload — 재기동 불필요."
                  style={{ padding: '6px 14px', fontSize: 12, cursor: working ? 'wait' : 'pointer',
                           color: '#666', background: '#f5f5f5', border: '1px solid #ddd', borderRadius: 3 }}>
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
