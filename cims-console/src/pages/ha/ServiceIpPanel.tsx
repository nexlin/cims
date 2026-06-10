import { useState } from 'react'
import type { AgentRoute } from '../../api/deployment'
import type { NetIface, ServiceIpRow, IpSlot } from './types'
import { ImeSafeInput } from './ImeSafeInput'
import { btnSmall, btnDanger } from './styles'

// ServiceIpPanel — 인터페이스별 cims-managed IP 추가/삭제 + specific route 관리.
// 모델: 각 IP 가 row (iface, ip 단위). agent 가 보고한 interfaces.managed=true 인 IP 만
// [삭제] 허용 (외부 IP 는 readonly). [+IP 추가] / [+라우팅 추가] 로 명시적 op 발사.
// route 는 kernel 외 default GW + specific subnet 모두 변경 가능.
export function ServiceIpPanel({ title, interfaces, storedRows, storedRoutes, slots, applying,
                                 onApply, onUpdateSlot, vipIps }: {
  title: string
  interfaces: NetIface[]
  storedRows: ServiceIpRow[]                                                    // slot 라벨 매칭용 (iface, ip) keyed
  storedRoutes: AgentRoute[]
  slots: IpSlot[]
  applying?: boolean
  onApply: (
    ops: {
      service_ip_rows?: Array<{ op: 'add'|'del'; iface: string; ip: string; mask: number; slot?: string }>
      routes?:          Array<{ op: 'add'|'del'; dst: string; via: string; dev: string }>
    },
    label: string,
  ) => void
  // 외부/cims-managed 모든 IP 의 slot 편집 — file_store service_ip_rows 에 (iface, ip, slot) 저장.
  // VIP / module config 매핑 용. ip addr 변경은 안 함.
  onUpdateSlot: (iface: string, ip: string, mask: number, slot: string) => void
  // HA group vip_bindings 의 VIP IP 집합 — keepalived 가 관리하는 부동 IP.
  // 이 IP 는 서버 고정 IP 가 아니라 VIP 표시(MASTER 보유)일 뿐 → 망/용도 편집·삭제 불가.
  vipIps?: Set<string>
}) {
  const mgmtIfaces = new Set(interfaces.filter(x => x.mgmt).map(x => x.name))
  // iface 그룹 — 출현 순서대로. 빈 NIC 도 1 row.
  const ifaceOrder: string[] = []
  const ipsByIface = new Map<string, NetIface[]>()
  for (const i of interfaces) {
    if (!ifaceOrder.includes(i.name)) {
      ifaceOrder.push(i.name)
      ipsByIface.set(i.name, [])
    }
    if (i.ip) ipsByIface.get(i.name)!.push(i)
  }
  const slotByKey = (iface: string, ip: string): string => {
    const m = storedRows.find(r => r.iface === iface && r.ip === ip)
    return m?.slot || ''
  }
  const slotHints = slots.map(s => s.name).join(' / ')

  const addableIfaces = ifaceOrder.filter(n => !mgmtIfaces.has(n))               // mgmt 는 추가 불가 (자기 단절 방지)
  const [addOpen, setAddOpen] = useState(false)
  const [addIface, setAddIface] = useState('')
  const [addIp, setAddIp] = useState('')
  const [addMask, setAddMask] = useState(24)
  const [addSlot, setAddSlot] = useState('')

  const beginAdd = () => {
    setAddOpen(true)
    setAddIface(addableIfaces[0] || '')
    setAddIp(''); setAddMask(24); setAddSlot('')
  }
  const cancelAdd = () => setAddOpen(false)
  const commitAdd = () => {
    if (!addIface || !addIp || !addMask) return
    onApply(
      { service_ip_rows: [{ op: 'add', iface: addIface, ip: addIp, mask: addMask, slot: addSlot }] },
      `${addIface} += ${addIp}/${addMask}`,
    )
    setAddOpen(false)
  }
  const deleteIp = (iface: string, ip: string, mask: number) => {
    if (!confirm(`${iface} 에서 ${ip}/${mask} 를 제거할까요?\n(agent 가 ip addr del 호출)`)) return
    onApply(
      { service_ip_rows: [{ op: 'del', iface, ip, mask }] },
      `${iface} -= ${ip}/${mask}`,
    )
  }

  // ── Routes section ──
  const [routeAddOpen, setRouteAddOpen] = useState(false)
  const [routeDst, setRouteDst] = useState('')
  const [routeVia, setRouteVia] = useState('')
  const [routeDev, setRouteDev] = useState('')
  const routableIfaces = ifaceOrder.filter(n => !mgmtIfaces.has(n))
  const beginAddRoute = () => {
    setRouteAddOpen(true); setRouteDst(''); setRouteVia('')
    setRouteDev(routableIfaces[0] || '')
  }
  const cancelAddRoute = () => setRouteAddOpen(false)
  const commitAddRoute = () => {
    if (!routeDst || !routeVia || !routeDev) return
    onApply(
      { routes: [{ op: 'add', dst: routeDst, via: routeVia, dev: routeDev }] },
      `route += ${routeDst} via ${routeVia} dev ${routeDev}`,
    )
    setRouteAddOpen(false)
  }
  const deleteRoute = (r: AgentRoute) => {
    if (!confirm(`route ${r.dst} via ${r.via} dev ${r.dev} 를 제거할까요?`)) return
    onApply(
      { routes: [{ op: 'del', dst: r.dst, via: r.via, dev: r.dev }] },
      `route -= ${r.dst} via ${r.via} dev ${r.dev}`,
    )
  }
  const [routeEditKey, setRouteEditKey] = useState<string | null>(null)
  const [routeEditVia, setRouteEditVia] = useState('')
  const [routeEditDev, setRouteEditDev] = useState('')
  const beginEditRoute = (r: AgentRoute) => {
    setRouteEditKey(r.dst); setRouteEditVia(r.via); setRouteEditDev(r.dev)
  }
  const cancelEditRoute = () => setRouteEditKey(null)
  const commitEditRoute = (orig: AgentRoute) => {
    if (!routeEditVia || !routeEditDev) return
    const ops: Array<{ op: 'add'|'del'; dst: string; via: string; dev: string }> = []
    if (orig.dev !== routeEditDev) {
      ops.push({ op: 'del', dst: orig.dst, via: orig.via, dev: orig.dev })
    }
    ops.push({ op: 'add', dst: orig.dst, via: routeEditVia, dev: routeEditDev })
    onApply(
      { routes: ops },
      `route ✎ ${orig.dst} → via ${routeEditVia} dev ${routeEditDev}`,
    )
    setRouteEditKey(null)
  }

  return (
    <div style={{
      borderLeft: '3px solid var(--border)', borderRadius: 4, padding: '10px 12px',
      background: 'var(--bg-soft)',
    }}>
      <div style={{ fontSize: 12, fontWeight: 'bold', color: 'var(--text-muted)', marginBottom: 8 }}>
        {title}
        <span style={{ marginLeft: 8, fontSize: 11, color: 'var(--text-muted)', fontWeight: 'normal' }}>
          (cims-managed 만 변경 가능 — 외부 IP / mgmt NIC 은 보호)
        </span>
      </div>

      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ background: 'var(--surface-2)', color: 'var(--text-muted)' }}>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 90 }}>인터페이스</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 170 }}>IP / mask</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 150 }}>용도(slot)</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 90 }}>소유</th>
            <th style={{ padding: '4px 8px', textAlign: 'left' }}>액션</th>
          </tr>
        </thead>
        <tbody>
          {ifaceOrder.length === 0 && (
            <tr><td colSpan={5} style={{ padding: '8px', color: 'var(--text-muted)' }}>(인터페이스 없음 — agent 보고 대기)</td></tr>
          )}
          {ifaceOrder.flatMap((iface) => {
            const isMgmt = mgmtIfaces.has(iface)
            const ips = ipsByIface.get(iface) || []
            const ifaceRows = ips.length > 0
              ? ips.map((ni, ipIdx) => {
                  const managed = !!ni.managed
                  const slot = slotByKey(iface, ni.ip)
                  const isMgmtIp = isMgmt && ni.mgmt
                  const isVip = !!vipIps?.has(ni.ip)
                  return (
                    <tr key={`${iface}-${ni.ip}-${ipIdx}`}
                        style={isMgmtIp ? { background: 'var(--surface-2)' } : undefined}>
                      <td style={{ padding: '4px 8px', fontFamily: 'monospace' }}>
                        {ipIdx === 0 && <b>{iface}</b>}
                        {ipIdx === 0 && isMgmt && (
                          <span title="agent ↔ CSC 통신 NIC — 변경 시 단절 위험으로 잠금"
                                style={{ marginLeft: 6, fontSize: 10, color: 'var(--text-muted)' }}>🔒 mgmt</span>
                        )}
                      </td>
                      <td style={{ padding: '4px 8px', fontFamily: 'monospace' }}>
                        {ni.ip}/{ni.mask}
                      </td>
                      <td style={{ padding: '4px 8px' }}>
                        {/* 용도(slot) — NIC 의 단일 분류 키. VIP→NIC 매핑도 이 값으로 결정.
                            mgmt 는 자동 도출(소유 컬럼 배지), VIP 는 HA 그룹 바인딩에서 결정 → 읽기전용. */}
                        {isVip ? (
                          <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>{slot || '—'}</span>
                        ) : isMgmtIp ? (
                          <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>{slot || '—'}</span>
                        ) : (
                          <ImeSafeInput value={slot}
                                        onCommit={(v) => {
                                          if (v !== slot) onUpdateSlot(iface, ni.ip, ni.mask, v)
                                        }}
                                        placeholder="(용도)"
                                        style={{ width: '95%', padding: '2px 6px', fontSize: 11,
                                                 border: '1px solid var(--border)', borderRadius: 3 }} />
                        )}
                      </td>
                      <td style={{ padding: '4px 8px', fontSize: 11 }}>
                        {isMgmtIp ? <span style={{ color: 'var(--text-muted)' }}>mgmt</span>
                          : isVip ? <span style={{ color: '#8e44ad', fontWeight: 'bold' }}>🔗 VIP</span>
                          : managed ? <span style={{ color: '#27ae60' }}>● cims</span>
                          : <span style={{ color: 'var(--text-muted)' }}>○ 외부</span>}
                      </td>
                      <td style={{ padding: '4px 8px' }}>
                        {managed && !isMgmtIp && !isVip && (
                          <button onClick={() => deleteIp(iface, ni.ip, ni.mask)}
                                  style={btnDanger()} disabled={applying}>
                            삭제
                          </button>
                        )}
                      </td>
                    </tr>
                  )
                })
              : [(
                  <tr key={`${iface}-empty`}>
                    <td style={{ padding: '4px 8px', fontFamily: 'monospace' }}><b>{iface}</b></td>
                    <td colSpan={3} style={{ padding: '4px 8px', color: 'var(--text-muted)', fontSize: 11 }}>
                      (IP 미할당)
                    </td>
                    <td style={{ padding: '4px 8px' }}></td>
                  </tr>
                )]
            return ifaceRows
          })}
          {addOpen ? (
            <tr style={{ background: 'var(--warn-soft)' }}>
              <td style={{ padding: '4px 8px' }}>
                <select value={addIface} onChange={e => setAddIface(e.target.value)}
                        style={{ width: '95%', padding: '2px 4px', fontSize: 12,
                                 border: '1px solid #e67e22', borderRadius: 3 }}>
                  {addableIfaces.length === 0 && <option value="">(없음)</option>}
                  {addableIfaces.map(name => <option key={name} value={name}>{name}</option>)}
                </select>
              </td>
              <td style={{ padding: '4px 8px' }}>
                <input value={addIp}
                       placeholder="10.0.3.45"
                       onChange={e => setAddIp(e.target.value)}
                       style={{ width: 110, padding: '2px 6px', fontSize: 12,
                                border: '1px solid #e67e22', borderRadius: 3 }} />
                <span> / </span>
                <input type="number" value={addMask}
                       onChange={e => setAddMask(parseInt(e.target.value) || 24)}
                       style={{ width: 40, padding: '2px 6px', fontSize: 12,
                                border: '1px solid #e67e22', borderRadius: 3 }} />
              </td>
              <td style={{ padding: '4px 8px' }}>
                <ImeSafeInput value={addSlot}
                              onCommit={setAddSlot}
                              placeholder="(용도)"
                              style={{ width: '95%', padding: '2px 6px', fontSize: 12,
                                       border: '1px solid var(--border)', borderRadius: 3 }} />
              </td>
              <td colSpan={2} style={{ padding: '4px 8px' }}>
                <button onClick={commitAdd} style={btnSmall()}
                        disabled={!addIface || !addIp || !addMask || applying}>
                  추가
                </button>
                <button onClick={cancelAdd} style={btnSmall()}>취소</button>
              </td>
            </tr>
          ) : (
            addableIfaces.length > 0 && (
              <tr>
                <td colSpan={5} style={{ padding: '4px 8px' }}>
                  <button onClick={beginAdd} style={btnSmall()} disabled={applying}>
                    + IP 추가
                  </button>
                </td>
              </tr>
            )
          )}
        </tbody>
      </table>

      <div style={{ marginTop: 16, fontSize: 12, fontWeight: 'bold', color: 'var(--text-muted)' }}>
        라우팅 (subnet 자동(🔒 kernel) 외 모두 변경 가능 — default gateway 포함)
      </div>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12, marginTop: 4 }}>
        <thead>
          <tr style={{ background: 'var(--surface-2)', color: 'var(--text-muted)' }}>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 200 }}>dest CIDR</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 140 }}>gateway</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 90 }}>dev</th>
            <th style={{ padding: '4px 8px', textAlign: 'left', width: 100 }}>소유</th>
            <th style={{ padding: '4px 8px', textAlign: 'left' }}>액션</th>
          </tr>
        </thead>
        <tbody>
          {storedRoutes.length === 0 && !routeAddOpen && (
            <tr><td colSpan={5} style={{ padding: '8px', color: 'var(--text-muted)' }}>(라우팅 없음)</td></tr>
          )}
          {[...storedRoutes].sort((a, b) => {
            const ga = a.is_default ? 0 : a.kernel_auto ? 1 : 2
            const gb = b.is_default ? 0 : b.kernel_auto ? 1 : 2
            if (ga !== gb) return ga - gb
            return (a.dst || '').localeCompare(b.dst || '')
          }).map((r) => {
            const managed = !!r.managed
            const isDefault = !!r.is_default
            const kernelAuto = !!r.kernel_auto
            const rowKey = `route-${r.dst}-${r.via}-${r.dev}`
            const ownerChip = kernelAuto  ? <span style={{ color: 'var(--text-muted)' }}>🔒 kernel</span>
                            : isDefault   ? <span style={{ color: '#3498db' }}>★ default</span>
                            : managed     ? <span style={{ color: '#27ae60' }}>● cims</span>
                            :               <span style={{ color: 'var(--text-muted)' }}>○ 외부</span>
            const canEdit = !kernelAuto
            const isEditing = routeEditKey === r.dst
            if (isEditing) {
              return (
                <tr key={rowKey} style={{ background: 'var(--warn-soft)' }}>
                  <td style={{ padding: '4px 8px', fontFamily: 'monospace' }}>{r.dst}</td>
                  <td style={{ padding: '4px 8px' }}>
                    <input value={routeEditVia}
                           onChange={e => setRouteEditVia(e.target.value)}
                           style={{ width: 120, padding: '2px 6px', fontSize: 12,
                                    border: '1px solid #e67e22', borderRadius: 3 }} />
                  </td>
                  <td style={{ padding: '4px 8px' }}>
                    <select value={routeEditDev} onChange={e => setRouteEditDev(e.target.value)}
                            style={{ width: 80, padding: '2px 6px', fontSize: 12,
                                     border: '1px solid #e67e22', borderRadius: 3 }}>
                      {routableIfaces.map(d => <option key={d} value={d}>{d}</option>)}
                    </select>
                  </td>
                  <td style={{ padding: '4px 8px', fontSize: 11 }}>{ownerChip}</td>
                  <td style={{ padding: '4px 8px' }}>
                    <button onClick={() => commitEditRoute(r)} style={btnSmall()}
                            disabled={!routeEditVia || !routeEditDev || applying ||
                                      (routeEditVia === r.via && routeEditDev === r.dev)}>
                      저장
                    </button>
                    <button onClick={cancelEditRoute} style={btnSmall()}>취소</button>
                  </td>
                </tr>
              )
            }
            const bg = kernelAuto ? 'var(--surface-2)' : undefined
            return (
              <tr key={rowKey} style={bg ? { background: bg } : undefined}>
                <td style={{ padding: '4px 8px', fontFamily: 'monospace' }}>{r.dst}</td>
                <td style={{ padding: '4px 8px', fontFamily: 'monospace' }}>{r.via || '—'}</td>
                <td style={{ padding: '4px 8px', fontFamily: 'monospace' }}>{r.dev || '—'}</td>
                <td style={{ padding: '4px 8px', fontSize: 11 }}>{ownerChip}</td>
                <td style={{ padding: '4px 8px' }}>
                  {canEdit && (
                    <>
                      <button onClick={() => beginEditRoute(r)} style={btnSmall()} disabled={applying}>
                        수정
                      </button>
                      <button onClick={() => deleteRoute(r)} style={btnDanger()} disabled={applying}>
                        삭제
                      </button>
                    </>
                  )}
                </td>
              </tr>
            )
          })}
          {routeAddOpen && (
            <tr style={{ background: 'var(--warn-soft)' }}>
              <td style={{ padding: '4px 8px' }}>
                <input value={routeDst}
                       placeholder="192.168.100.0/24"
                       onChange={e => setRouteDst(e.target.value)}
                       style={{ width: 180, padding: '2px 6px', fontSize: 12,
                                border: '1px solid #e67e22', borderRadius: 3 }} />
              </td>
              <td style={{ padding: '4px 8px' }}>
                <input value={routeVia}
                       placeholder="10.0.3.1"
                       onChange={e => setRouteVia(e.target.value)}
                       style={{ width: 120, padding: '2px 6px', fontSize: 12,
                                border: '1px solid #e67e22', borderRadius: 3 }} />
              </td>
              <td style={{ padding: '4px 8px' }}>
                <select value={routeDev} onChange={e => setRouteDev(e.target.value)}
                        style={{ width: 80, padding: '2px 6px', fontSize: 12,
                                 border: '1px solid #e67e22', borderRadius: 3 }}>
                  {routableIfaces.map(d => <option key={d} value={d}>{d}</option>)}
                  {routableIfaces.length === 0 && <option value="">(없음)</option>}
                </select>
              </td>
              <td colSpan={2} style={{ padding: '4px 8px' }}>
                <button onClick={commitAddRoute} style={btnSmall()}
                        disabled={!routeDst || !routeVia || !routeDev || applying}>
                  추가
                </button>
                <button onClick={cancelAddRoute} style={btnSmall()}>취소</button>
              </td>
            </tr>
          )}
          {!routeAddOpen && (
            <tr>
              <td colSpan={5} style={{ padding: '4px 8px' }}>
                <button onClick={beginAddRoute} style={btnSmall()}
                        disabled={applying || routableIfaces.length === 0}
                        title={routableIfaces.length === 0 ? 'mgmt 외 NIC 없음' : 'route 추가 (default GW 포함)'}>
                  + 라우팅 추가
                </button>
              </td>
            </tr>
          )}
        </tbody>
      </table>

      <div style={{ marginTop: 8, fontSize: 11, color: 'var(--text-muted)' }}>
        {slots.length > 0
          ? <>ℹ 참고 — 설치된 패키지의 권장 용도: <code>{slotHints}</code> (자유 입력 가능)</>
          : <>ℹ 인프라 단계 — NIC 이름이 곧 용도 라벨로 사용됩니다.</>}
      </div>
    </div>
  )
}
