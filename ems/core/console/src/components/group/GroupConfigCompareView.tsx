//  GroupConfigCompareView — 그룹 선택 + [패키지 설정] 탭의 읽기 전용 비교 뷰 (R2).
//
//  멤버별 deployment config 를 나란히 비교 — 편집은 하지 않는다 (편집 = 각 서버의
//  패키지 설정 탭, HA 정합은 필드별 🔗 동기화가 담당). 필드 상태 3종:
//    · 🔗 동기화 + 값 동일  → 정상 (녹색)
//    · 🔗 동기화 + 값 상이  → 드리프트 경고 (주황) — 해당 서버에서 재저장으로 해소
//    · 비동기화             → 중립 ("개별" — 서버별 고유값)
//  동기화 키 = ha_group.config_sync[pkg] (영속값), 없으면 scope=service 섹션 기본.
import { useCallback, useEffect, useMemo, useState } from 'react'
import { useToast } from '../Toast'
import {
  deploymentApi,
  type Deployment, type SipPackage, type ConfigTemplateField,
} from '../../api/deployment'
import type { HaGroup } from '../../api/ha_groups'
import { defaultValue, serviceScopeKeys, type FieldValue } from '../module/ModuleConfigModal'

interface Props {
  group: HaGroup
  members: Array<{ id: number; name: string }>
  deployments: Deployment[]
  packages: SipPackage[]
  // 셀/헤더 클릭 → 해당 서버의 패키지 설정 화면으로 점프
  onSelectMember: (agentId: number, packageName?: string) => void
}

type CellState = 'ok' | 'drift' | 'individual'

export function GroupConfigCompareView({ group, members: liveMembers,
    deployments: liveDeployments, packages: livePackages, onSelectMember }: Props) {
  const { show } = useToast()
  // 부모 폴링의 prop identity churn 차단 — 열린 시점 스냅샷 (새로고침 버튼으로 갱신)
  const [frozen] = useState(() => ({
    members: liveMembers, deployments: liveDeployments, packages: livePackages,
  }))
  const { members, deployments, packages } = frozen
  const [selectedPkg, setSelectedPkg] = useState<number>(0)
  const [loading, setLoading] = useState(false)
  // agent_id → config overlay (멤버별 GET /deployments/{id}/config 병렬 합성)
  const [configs, setConfigs] = useState<Map<number, Record<string, unknown>> | null>(null)

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

  const memberDepsForPkg = useMemo(
    () => deployments.filter(d => memberIds.has(d.agent_id) && d.package_id === effectivePkgId),
    [deployments, memberIds, effectivePkgId]
  )
  const depByAgent = useMemo(() => {
    const m = new Map<number, Deployment>()
    for (const d of memberDepsForPkg) m.set(d.agent_id, d)
    return m
  }, [memberDepsForPkg])

  // 동기화 키 — 영속값(config_sync[pkg]) 우선, 없으면 scope=service 기본 (모달과 동일 규칙)
  const syncKeys = useMemo(() => new Set(
    (pkg && group.config_sync?.[pkg.name]) ?? serviceScopeKeys(template ?? null)
  ), [group.config_sync, pkg, template])

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

  // 멤버별 실효값 — overlay 값 없으면 template default (fromDefault 표시용)
  function effective(agentId: number, f: ConfigTemplateField): { v: FieldValue; fromDefault: boolean } {
    const c = configs?.get(agentId)
    const v = c?.[f.key]
    if (v === undefined) return { v: defaultValue(f), fromDefault: true }
    return { v: v as FieldValue, fromDefault: false }
  }

  function cellState(f: ConfigTemplateField, deployedIds: number[]): CellState {
    if (!syncKeys.has(f.key)) return 'individual'
    if (deployedIds.length < 2) return 'ok'
    const first = JSON.stringify(effective(deployedIds[0], f).v)
    return deployedIds.every(aid => JSON.stringify(effective(aid, f).v) === first) ? 'ok' : 'drift'
  }

  function display(f: ConfigTemplateField, v: FieldValue): string {
    if (f.type === 'password') return v === null || v === undefined || v === '' ? '(빈 값)' : '●●●'
    if (v === null || v === undefined || v === '') return '(빈 값)'
    if (typeof v === 'boolean') return v ? 'true' : 'false'
    if (Array.isArray(v)) return v.join(', ')
    return String(v)
  }

  // 배포된 멤버(열 순서 = 그룹 멤버 순서) / 미배포 멤버 분리
  const deployedMembers = members.filter(m => depByAgent.has(m.id))
  const undeployedMembers = members.filter(m => !depByAgent.has(m.id))
  const deployedIds = deployedMembers.map(m => m.id)

  // 상단 요약 — 동기화/드리프트/개별 카운트
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

  if (groupPackages.length === 0) {
    return <div className="empty" style={{ padding: 40 }}>
      그룹 멤버에 배포된 모듈 없음 — [패키지 설치] 탭에서 모듈을 먼저 배포하세요
    </div>
  }

  return (
    <div style={{ height: '100%', display: 'flex', flexDirection: 'column' }}>
      {/* 모듈 탭 */}
      <div style={{ flex: '0 0 auto', display: 'flex', gap: 2, padding: '10px 16px 0',
                    borderBottom: '1px solid var(--border)', background: 'var(--bg-soft)' }}>
        {groupPackages.map(p => {
          const active = p.id === effectivePkgId
          return (
            <button key={p.id} onClick={() => setSelectedPkg(p.id)}
                    style={{
                      padding: '8px 18px', fontSize: 13, fontWeight: active ? 700 : 400,
                      background: active ? 'var(--surface)' : 'transparent',
                      color: active ? '#1976d2' : 'var(--text-muted)',
                      border: '1px solid var(--border)', borderBottom: 'none',
                      borderRadius: '6px 6px 0 0', cursor: 'pointer',
                    }}>
              {p.name} <span style={{ fontSize: 10 }}>v{p.version}</span>
            </button>
          )
        })}
        <button className="btn btn--sm" style={{ marginLeft: 'auto', marginBottom: 6 }}
                onClick={() => void load()} disabled={loading}>
          {loading ? '로딩...' : '↻ 새로고침'}
        </button>
      </div>

      <div style={{ flex: 1, overflow: 'auto', padding: 20 }}>
        {/* 요약 + 안내 */}
        <div style={{ fontSize: 12, marginBottom: 12, display: 'flex', gap: 12,
                      alignItems: 'center', flexWrap: 'wrap' }}>
          <span style={{ color: '#1e7d34' }}>🔗 동기화 {summary.ok}</span>
          <span style={{ color: summary.drift ? '#e67e22' : 'var(--text-muted)',
                         fontWeight: summary.drift ? 700 : 400 }}>
            ⚠ 드리프트 {summary.drift}
          </span>
          <span style={{ color: 'var(--text-muted)' }}>개별 {summary.individual}</span>
          <span style={{ color: 'var(--text-muted)' }}>
            · 읽기 전용 — 편집은 각 서버(멤버) 선택 후 [패키지 설정] 탭에서
          </span>
        </div>
        {summary.drift > 0 && (
          <div style={{ padding: 10, background: '#fff3e0', border: '1px solid #f39c12',
                        borderRadius: 4, fontSize: 12, marginBottom: 12 }}>
            ⚠ 동기화 대상인데 멤버 간 값이 다른 필드: <b>{summary.driftFields.join(', ')}</b>
            &nbsp;— 기준이 될 서버에서 해당 필드를 🔗 체크 상태로 재저장하면 정렬됩니다.
          </div>
        )}
        {undeployedMembers.length > 0 && (
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 12 }}>
            미배포 멤버: {undeployedMembers.map(m => m.name).join(', ')}
          </div>
        )}

        {!template ? (
          <div className="empty" style={{ padding: 20 }}>
            이 패키지에는 config_template 이 없습니다 — 비교할 설정 항목 없음
          </div>
        ) : !configs ? (
          <div className="empty" style={{ padding: 20 }}>로딩 중...</div>
        ) : (
          template.sections.map(sec => (
            <div key={sec.key} style={{ border: '1px solid #e5e5e5', borderRadius: 6,
                                        marginBottom: 12, background: '#fff', overflow: 'hidden' }}>
              <div style={{ padding: '10px 14px', background: '#fafafa',
                            borderBottom: '1px solid #eee',
                            display: 'flex', alignItems: 'baseline', gap: 8 }}>
                <b>{sec.title}</b>
                {sec.scope === 'service' && (
                  <span style={{ fontSize: 10, padding: '1px 6px', borderRadius: 3,
                                 background: '#e8f0fe', color: '#1a73e8' }}>그룹 공통 권장</span>
                )}
                {sec.description && (
                  <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>— {sec.description}</span>
                )}
              </div>
              <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
                <thead>
                  <tr style={{ color: 'var(--text-muted)' }}>
                    <th style={{ textAlign: 'left', padding: '6px 14px', width: 240 }}>필드</th>
                    <th style={{ width: 70, textAlign: 'center' }}>동기화</th>
                    {deployedMembers.map(m => (
                      <th key={m.id} style={{ textAlign: 'left', padding: '6px 10px',
                                              cursor: 'pointer', color: '#1976d2' }}
                          title={`${m.name} 의 설정 편집으로 이동`}
                          onClick={() => onSelectMember(m.id, pkg?.name)}>
                        {m.name} ↗
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
                                ? <span title="동기화 대상인데 멤버 간 값 상이" style={{ color: '#e67e22' }}>⚠</span>
                                : <span title="동기화 — 멤버 간 값 동일" style={{ color: '#1e7d34' }}>🔗</span>)
                            : <span title="서버별 고유값" style={{ fontSize: 10, color: 'var(--text-muted)' }}>개별</span>}
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
                                onClick={() => onSelectMember(m.id, pkg?.name)}>
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
          ))
        )}

        <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 4 }}>
          컬렉션(리스너/트렁크 등)은 scope 기반 자동 동기화 — 드리프트는 각 서버 설정의
          컬렉션 탭 peers 표시에서 확인합니다.
        </div>
      </div>
    </div>
  )
}

export default GroupConfigCompareView
